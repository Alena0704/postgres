#include "postgres.h"

#include "catalog/catalog.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/pgstat_kind.h"
#include "utils/pgstat_internal.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/* Two kinds: relations (tables/indexes) and database aggregates */
#define PGSTAT_KIND_EXTVAC_RELATION	24
#define PGSTAT_KIND_EXTVAC_DB		25

#define SJ_NODENAME		"vacuum_statistics"
#define EVS_TRACK_FILENAME	"pg_stat/ext_vacuum_statistics_track.oid"

/* Collect mask bits */
#define EVS_COLLECT_BUFFERS	0x1	/* blks_*, blk_*_time */
#define EVS_COLLECT_WAL		0x2	/* wal_records, wal_fpi, wal_bytes */
#define EVS_COLLECT_TUPLES	0x4	/* tuples_deleted, pages_*, vm_*, etc. */
#define EVS_COLLECT_TIMING	0x8	/* delay_time, total_time */
#define EVS_COLLECT_ALL		(EVS_COLLECT_BUFFERS | EVS_COLLECT_WAL | \
							 EVS_COLLECT_TUPLES | EVS_COLLECT_TIMING)

/* --- GUCs --- */
static bool evs_enabled = true;
static char *evs_track = "all";		/* 'all', 'databases', 'relations' */
static char *evs_track_relations = "all";	/* 'all', 'system', 'user' */
static char *evs_track_databases = "";	/* comma-separated OIDs, empty = all */
static char *evs_track_relations_list = "";	/* comma-separated OIDs, empty = all */
static char *evs_collect = "all";	/* space-separated: buffers, wal, tuples, timing, or all */
static int	evs_collect_mask = EVS_COLLECT_ALL;

/* --- Hook chaining --- */
static set_report_vacuum_hook_type prev_report_vacuum_hook = NULL;

/* --- Forward declarations --- */
static void pgstat_report_vacuum_extstats(Oid tableoid, bool shared,
										 PgStat_VacuumRelationCounts *params);
static bool evs_oid_in_list(HTAB *hash, Oid oid);
static bool evs_should_track_relation(Oid dboid, Oid relid);
static void evs_assign_collect(const char *newval, void *extra);
static void evs_assign_track_databases(const char *newval, void *extra);
static void evs_assign_track_relations_list(const char *newval, void *extra);
static void evs_track_hash_ensure_init(void);
static void evs_track_parse_guc_to_hash(HTAB *hash, const char *list);
static void evs_track_save_file(void);
static void evs_track_load_file(void);

/* objid encoding for relations: (relid << 2) | (type & 3) */
#define EXTVAC_OBJID(relid, type) (((uint64) (relid)) << 2 | ((type) & 3))

/* Hash tables for track_databases and track_relations_list (OID sets) */
static HTAB *evs_track_databases_hash = NULL;
static HTAB *evs_track_relations_hash = NULL;
static bool evs_track_hash_initialized = false;

static void evs_track_load_file(void);

static void
evs_track_hash_ensure_init(void)
{
	HASHCTL		ctl;

	if (evs_track_hash_initialized)
		return;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(Oid);
	ctl.hcxt = TopMemoryContext;

	evs_track_databases_hash = hash_create("ext_vacuum_statistics track databases",
										  64, &ctl, HASH_ELEM | HASH_BLOBS);
	evs_track_relations_hash = hash_create("ext_vacuum_statistics track relations",
										  64, &ctl, HASH_ELEM | HASH_BLOBS);

	evs_track_load_file();
	/* GUC values override file: non-empty = parse GUC; empty = clear (track all) */
	if (evs_track_databases && evs_track_databases[0] != '\0')
		evs_track_parse_guc_to_hash(evs_track_databases_hash, evs_track_databases);
	else
		evs_track_parse_guc_to_hash(evs_track_databases_hash, "");
	if (evs_track_relations_list && evs_track_relations_list[0] != '\0')
		evs_track_parse_guc_to_hash(evs_track_relations_hash, evs_track_relations_list);
	else
		evs_track_parse_guc_to_hash(evs_track_relations_hash, "");
	evs_track_hash_initialized = true;
}

static void
evs_track_parse_guc_to_hash(HTAB *hash, const char *list)
{
	char	   *copy;
	char	   *p;
	char	   *tok;
	Oid			oid;
	bool		found;

	if (!hash)
		return;

	/* Clear hash */
	{
		HASH_SEQ_STATUS status;
		Oid		   *entry;

		hash_seq_init(&status, hash);
		while ((entry = (Oid *) hash_seq_search(&status)) != NULL)
			hash_search(hash, entry, HASH_REMOVE, &found);
	}

	if (!list || list[0] == '\0')
		return;

	copy = pstrdup(list);
	for (p = copy; (tok = strtok(p, " ,\t")); p = NULL)
	{
		char	   *end;
		unsigned long v;

		v = strtoul(tok, &end, 10);
		if (tok == end)
			continue;
		oid = (Oid) v;
		hash_search(hash, &oid, HASH_ENTER, &found);
	}
	pfree(copy);
}

static void
evs_track_load_file(void)
{
	char		path[MAXPGPATH];
	FILE	   *fp;
	char		buf[256];
	HTAB	   *curhash = NULL;
	Oid			oid;
	bool		found;

	if (!DataDir || DataDir[0] == '\0' || !evs_track_databases_hash || !evs_track_relations_hash)
		return;

	snprintf(path, sizeof(path), "%s/%s", DataDir, EVS_TRACK_FILENAME);
	fp = AllocateFile(path, "r");
	if (!fp)
		return;

	while (fgets(buf, sizeof(buf), fp))
	{
		if (strncmp(buf, "[databases]", 11) == 0)
		{
			curhash = evs_track_databases_hash;
			continue;
		}
		if (strncmp(buf, "[relations]", 11) == 0)
		{
			curhash = evs_track_relations_hash;
			continue;
		}
		if (curhash && sscanf(buf, "%u", &oid) == 1)
			hash_search(curhash, &oid, HASH_ENTER, &found);
	}
	FreeFile(fp);
}

static void
evs_track_save_file(void)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	FILE	   *fp;
	HASH_SEQ_STATUS status;
	Oid		   *entry;

	if (!DataDir || DataDir[0] == '\0' || !evs_track_databases_hash || !evs_track_relations_hash)
		return;

	snprintf(path, sizeof(path), "%s/%s", DataDir, EVS_TRACK_FILENAME);
	snprintf(tmppath, sizeof(tmppath), "%s/%s.tmp", DataDir, EVS_TRACK_FILENAME);
	fp = AllocateFile(tmppath, "w");
	if (!fp)
		return;

	fprintf(fp, "[databases]\n");
	hash_seq_init(&status, evs_track_databases_hash);
	while ((entry = (Oid *) hash_seq_search(&status)) != NULL)
		fprintf(fp, "%u\n", *entry);

	fprintf(fp, "[relations]\n");
	hash_seq_init(&status, evs_track_relations_hash);
	while ((entry = (Oid *) hash_seq_search(&status)) != NULL)
		fprintf(fp, "%u\n", *entry);

	if (FreeFile(fp) != 0 || rename(tmppath, path) != 0)
		unlink(tmppath);
}

static bool
evs_oid_in_list(HTAB *hash, Oid oid)
{
	if (!hash)
		return true;
	if (hash_get_num_entries(hash) == 0)
		return true;
	return hash_search(hash, &oid, HASH_FIND, NULL) != NULL;
}

static void
evs_assign_track_databases(const char *newval, void *extra)
{
	evs_track_hash_ensure_init();
	evs_track_parse_guc_to_hash(evs_track_databases_hash, newval);
	evs_track_save_file();
}

static void
evs_assign_track_relations_list(const char *newval, void *extra)
{
	evs_track_hash_ensure_init();
	evs_track_parse_guc_to_hash(evs_track_relations_hash, newval);
	evs_track_save_file();
}

static bool
evs_should_track_relation(Oid dboid, Oid relid)
{
	evs_track_hash_ensure_init();

	if (!evs_oid_in_list(evs_track_databases_hash, dboid))
		return false;
	if (strcmp(evs_track, "databases") == 0)
		return true;			/* will only accumulate to db */
	if (!evs_oid_in_list(evs_track_relations_hash, relid))
		return false;
	if (strcmp(evs_track_relations, "system") == 0)
		return IsCatalogRelationOid(relid);
	if (strcmp(evs_track_relations, "user") == 0)
		return !IsCatalogRelationOid(relid);
	return true;
}

static void
evs_assign_collect(const char *newval, void *extra)
{
	int			mask = 0;
	char	   *copy;
	char	   *p;
	char	   *tok;

	if (!newval || newval[0] == '\0')
	{
		evs_collect_mask = EVS_COLLECT_ALL;
		return;
	}

	copy = pstrdup(newval);
	for (p = copy; (tok = strtok(p, " \t")); p = NULL)
	{
		if (pg_strcasecmp(tok, "all") == 0)
		{
			mask = EVS_COLLECT_ALL;
			break;
		}
		if (pg_strcasecmp(tok, "buffers") == 0)
			mask |= EVS_COLLECT_BUFFERS;
		else if (pg_strcasecmp(tok, "wal") == 0)
			mask |= EVS_COLLECT_WAL;
		else if (pg_strcasecmp(tok, "tuples") == 0)
			mask |= EVS_COLLECT_TUPLES;
		else if (pg_strcasecmp(tok, "timing") == 0)
			mask |= EVS_COLLECT_TIMING;
		/* ignore unknown tokens */
	}
	pfree(copy);

	evs_collect_mask = (mask != 0) ? mask : EVS_COLLECT_ALL;
}

#define ACCUM_IF(flag, field) \
	do { if ((evs_collect_mask & (flag)) != 0) dst->field += src->field; } while (0)

static inline void
pgstat_accumulate_common(PgStat_CommonCounts *dst, const PgStat_CommonCounts *src)
{
	ACCUM_IF(EVS_COLLECT_BUFFERS, total_blks_read);
	ACCUM_IF(EVS_COLLECT_BUFFERS, total_blks_hit);
	ACCUM_IF(EVS_COLLECT_BUFFERS, total_blks_dirtied);
	ACCUM_IF(EVS_COLLECT_BUFFERS, total_blks_written);
	ACCUM_IF(EVS_COLLECT_BUFFERS, blks_fetched);
	ACCUM_IF(EVS_COLLECT_BUFFERS, blks_hit);
	ACCUM_IF(EVS_COLLECT_BUFFERS, blk_read_time);
	ACCUM_IF(EVS_COLLECT_BUFFERS, blk_write_time);
	ACCUM_IF(EVS_COLLECT_TIMING, delay_time);
	ACCUM_IF(EVS_COLLECT_TIMING, total_time);
	ACCUM_IF(EVS_COLLECT_WAL, wal_records);
	ACCUM_IF(EVS_COLLECT_WAL, wal_fpi);
	ACCUM_IF(EVS_COLLECT_WAL, wal_bytes);
	dst->wraparound_failsafe_count += src->wraparound_failsafe_count;
	dst->interrupts_count += src->interrupts_count;
	ACCUM_IF(EVS_COLLECT_TUPLES, tuples_deleted);
}

static inline void
pgstat_accumulate_extvac_stats(PgStat_VacuumRelationCounts *dst,
							   const PgStat_VacuumRelationCounts *src)
{
	if (dst->type == PGSTAT_EXTVAC_INVALID)
		dst->type = src->type;

	Assert(src->type != PGSTAT_EXTVAC_INVALID && src->type != PGSTAT_EXTVAC_DB);
	Assert(src->type == dst->type);

	pgstat_accumulate_common(&dst->common, &src->common);

	if (dst->type == PGSTAT_EXTVAC_TABLE && (evs_collect_mask & EVS_COLLECT_TUPLES) != 0)
	{
		dst->table.pages_scanned += src->table.pages_scanned;
		dst->table.pages_removed += src->table.pages_removed;
		dst->table.tuples_frozen += src->table.tuples_frozen;
		dst->table.recently_dead_tuples += src->table.recently_dead_tuples;
		dst->table.vm_new_frozen_pages += src->table.vm_new_frozen_pages;
		dst->table.vm_new_visible_pages += src->table.vm_new_visible_pages;
		dst->table.vm_new_visible_frozen_pages += src->table.vm_new_visible_frozen_pages;
		dst->table.missed_dead_pages += src->table.missed_dead_pages;
		dst->table.missed_dead_tuples += src->table.missed_dead_tuples;
		dst->table.index_vacuum_count += src->table.index_vacuum_count;
	}
	else if (dst->type == PGSTAT_EXTVAC_INDEX && (evs_collect_mask & EVS_COLLECT_TUPLES) != 0)
	{
		dst->index.pages_deleted += src->index.pages_deleted;
	}
}

static inline void
pgstat_accumulate_common_for_db(PgStat_CommonCounts *dst,
								const PgStat_CommonCounts *src)
{
	pgstat_accumulate_common(dst, src);
}

/* Shared memory entry for vacuum stats */
typedef struct PgStatShared_ExtVacEntry
{
	PgStatShared_Common header;
	PgStat_VacuumRelationCounts stats;
} PgStatShared_ExtVacEntry;

static const PgStat_KindInfo extvac_relation_kind_info = {
	.name = "ext_vacuum_statistics_relation",
	.fixed_amount = false,
	.accessed_across_databases = true,
	.write_to_file = true,
	.track_entry_count = true,
	.shared_size = sizeof(PgStatShared_ExtVacEntry),
	.shared_data_off = offsetof(PgStatShared_ExtVacEntry, stats),
	.shared_data_len = sizeof(PgStat_VacuumRelationCounts),
	.pending_size = 0,
	.flush_pending_cb = NULL,
};

static const PgStat_KindInfo extvac_db_kind_info = {
	.name = "ext_vacuum_statistics_db",
	.fixed_amount = false,
	.accessed_across_databases = true,
	.write_to_file = true,
	.track_entry_count = true,
	.shared_size = sizeof(PgStatShared_ExtVacEntry),
	.shared_data_off = offsetof(PgStatShared_ExtVacEntry, stats),
	.shared_data_len = sizeof(PgStat_VacuumRelationCounts),
	.pending_size = 0,
	.flush_pending_cb = NULL,
};

/*
 * Store incoming vacuum stats into pgstat custom statistics.
 * store_relation: create/update per-relation entry
 * store_db: accumulate into database-level entry (dboid, objid=0)
 */
static void
extvac_store(Oid dboid, Oid relid, int type,
			 PgStat_VacuumRelationCounts *params,
			 bool store_relation, bool store_db)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_ExtVacEntry *shared;
	uint64		objid;

	if (!evs_enabled)
		return;

	if (store_relation)
	{
		objid = EXTVAC_OBJID(relid, type);
		entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_EXTVAC_RELATION, dboid, objid, false);
		if (entry_ref)
		{
			shared = (PgStatShared_ExtVacEntry *) entry_ref->shared_stats;
			if (shared->stats.type == PGSTAT_EXTVAC_INVALID)
			{
				memset(&shared->stats, 0, sizeof(shared->stats));
				shared->stats.type = params->type;
			}
			pgstat_accumulate_extvac_stats(&shared->stats, params);
			pgstat_unlock_entry(entry_ref);
		}
	}

	if (store_db)
	{
		entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_EXTVAC_DB, dboid, InvalidOid, false);
		if (entry_ref)
		{
			shared = (PgStatShared_ExtVacEntry *) entry_ref->shared_stats;
			if (shared->stats.type == PGSTAT_EXTVAC_INVALID)
			{
				memset(&shared->stats, 0, sizeof(shared->stats));
				shared->stats.type = PGSTAT_EXTVAC_DB;
			}
			pgstat_accumulate_common_for_db(&shared->stats.common, &params->common);
			pgstat_unlock_entry(entry_ref);
		}
	}
}

static void
pgstat_report_vacuum_extstats(Oid tableoid, bool shared,
							  PgStat_VacuumRelationCounts *params)
{
	Oid			dboid = shared ? InvalidOid : MyDatabaseId;
	bool		store_relation;
	bool		store_db;

	if (!evs_enabled)
		goto chain;

	if (!evs_should_track_relation(dboid, tableoid))
		goto chain;

	store_relation = (strcmp(evs_track, "databases") != 0);
	store_db = (strcmp(evs_track, "relations") != 0);

	extvac_store(dboid, tableoid, params->type, params, store_relation, store_db);

chain:
	if (prev_report_vacuum_hook)
		prev_report_vacuum_hook(tableoid, shared, params);
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ext_vacuum_statistics module could be loaded only on startup."),
				 errdetail("Add 'ext_vacuum_statistics' into the shared_preload_libraries list.")));

	DefineCustomBoolVariable("vacuum_statistics.enabled",
							 "Enable extended vacuum statistics collection.",
							 NULL, &evs_enabled, true,
							 PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomStringVariable("vacuum_statistics.track",
							   "What to track: 'all', 'databases', 'relations'.",
							   NULL, &evs_track, "all",
							   PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomStringVariable("vacuum_statistics.track_relations",
							   "When tracking relations: 'all', 'system', 'user'.",
							   NULL, &evs_track_relations, "all",
							   PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomStringVariable("vacuum_statistics.track_databases",
							   "Comma-separated database OIDs to track; empty = all.",
							   NULL, &evs_track_databases, "",
							   PGC_SUSET, 0, NULL, evs_assign_track_databases, NULL);

	DefineCustomStringVariable("vacuum_statistics.track_relations_list",
							   "Comma-separated relation OIDs to track; empty = all.",
							   NULL, &evs_track_relations_list, "",
							   PGC_SUSET, 0, NULL, evs_assign_track_relations_list, NULL);

	DefineCustomStringVariable("vacuum_statistics.collect",
							   "Space-separated list of stats to collect: buffers, wal, tuples, timing, or all.",
							   NULL, &evs_collect, "all",
							   PGC_SUSET, 0, NULL, evs_assign_collect, NULL);

	MarkGUCPrefixReserved(SJ_NODENAME);

	pgstat_register_kind(PGSTAT_KIND_EXTVAC_RELATION, &extvac_relation_kind_info);
	pgstat_register_kind(PGSTAT_KIND_EXTVAC_DB, &extvac_db_kind_info);

	prev_report_vacuum_hook = set_report_vacuum_hook;
	set_report_vacuum_hook = pgstat_report_vacuum_extstats;
}

static bool
extvac_reset_by_relid(Oid dboid, Oid relid, int type)
{
	uint64		objid = EXTVAC_OBJID(relid, type);

	pgstat_reset_entry(PGSTAT_KIND_EXTVAC_RELATION, dboid, objid, 0);
	return true;
}

static int64
extvac_database_reset(Oid dboid)
{
	pgstat_reset_entry(PGSTAT_KIND_EXTVAC_DB, dboid, 0, 0);
	return 1;					/* one database entry reset */
}

static int64
extvac_stat_reset(void)
{
	pgstat_reset_of_kind(PGSTAT_KIND_EXTVAC_RELATION);
	pgstat_reset_of_kind(PGSTAT_KIND_EXTVAC_DB);
	return 0;					/* count not available */
}

PG_FUNCTION_INFO_V1(vacuum_statistics_reset);
PG_FUNCTION_INFO_V1(extvac_reset_entry);
PG_FUNCTION_INFO_V1(extvac_reset_db_entry);

Datum
vacuum_statistics_reset(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(extvac_stat_reset());
}

Datum
extvac_reset_entry(PG_FUNCTION_ARGS)
{
	Oid			dboid = PG_GETARG_OID(0);
	Oid			relid = PG_GETARG_OID(1);
	int			type = PG_GETARG_INT32(2);

	PG_RETURN_BOOL(extvac_reset_by_relid(dboid, relid, type));
}

Datum
extvac_reset_db_entry(PG_FUNCTION_ARGS)
{
	Oid			dboid = PG_GETARG_OID(0);

	PG_RETURN_INT64(extvac_database_reset(dboid));
}

/* --- Track OID functions (add/remove, persisted to file) --- */

PG_FUNCTION_INFO_V1(evs_add_track_database);
PG_FUNCTION_INFO_V1(evs_remove_track_database);
PG_FUNCTION_INFO_V1(evs_add_track_relation);
PG_FUNCTION_INFO_V1(evs_remove_track_relation);

Datum
evs_add_track_database(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		found;

	evs_track_hash_ensure_init();
	hash_search(evs_track_databases_hash, &oid, HASH_ENTER, &found);
	evs_track_save_file();
	PG_RETURN_BOOL(!found);		/* true if newly added */
}

Datum
evs_remove_track_database(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		found;

	evs_track_hash_ensure_init();
	hash_search(evs_track_databases_hash, &oid, HASH_REMOVE, &found);
	evs_track_save_file();
	PG_RETURN_BOOL(found);
}

Datum
evs_add_track_relation(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		found;

	evs_track_hash_ensure_init();
	hash_search(evs_track_relations_hash, &oid, HASH_ENTER, &found);
	evs_track_save_file();
	PG_RETURN_BOOL(!found);		/* true if newly added */
}

Datum
evs_remove_track_relation(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		found;

	evs_track_hash_ensure_init();
	hash_search(evs_track_relations_hash, &oid, HASH_REMOVE, &found);
	evs_track_save_file();
	PG_RETURN_BOOL(found);
}

/* --- Output helpers --- */
#define EXTVAC_COMMON_STAT_COLS 12

static void
tuplestore_put_common(PgStat_CommonCounts *vacuum_ext,
					  Datum *values, bool *nulls, int *i)
{
	char		buf[256];
	const int	base = *i;

	values[(*i)++] = Int64GetDatum(vacuum_ext->total_blks_read);
	values[(*i)++] = Int64GetDatum(vacuum_ext->total_blks_hit);
	values[(*i)++] = Int64GetDatum(vacuum_ext->total_blks_dirtied);
	values[(*i)++] = Int64GetDatum(vacuum_ext->total_blks_written);
	values[(*i)++] = Int64GetDatum(vacuum_ext->wal_records);
	values[(*i)++] = Int64GetDatum(vacuum_ext->wal_fpi);
	snprintf(buf, sizeof buf, UINT64_FORMAT, vacuum_ext->wal_bytes);
	values[(*i)++] = DirectFunctionCall3(numeric_in,
										 CStringGetDatum(buf),
										 ObjectIdGetDatum(0),
										 Int32GetDatum(-1));
	values[(*i)++] = Float8GetDatum(vacuum_ext->blk_read_time);
	values[(*i)++] = Float8GetDatum(vacuum_ext->blk_write_time);
	values[(*i)++] = Float8GetDatum(vacuum_ext->delay_time);
	values[(*i)++] = Float8GetDatum(vacuum_ext->total_time);
	values[(*i)++] = Int32GetDatum(vacuum_ext->wraparound_failsafe_count);
	Assert((*i - base) == EXTVAC_COMMON_STAT_COLS);
}

#define EXTVAC_HEAP_STAT_COLS	26
#define EXTVAC_IDX_STAT_COLS	17
#define EXTVAC_MAX_STAT_COLS	Max(EXTVAC_HEAP_STAT_COLS, EXTVAC_IDX_STAT_COLS)

static void
tuplestore_put_for_relation(Oid relid, Tuplestorestate *tupstore,
							TupleDesc tupdesc, PgStat_VacuumRelationCounts *vacuum_ext)
{
	Datum		values[EXTVAC_MAX_STAT_COLS];
	bool		nulls[EXTVAC_MAX_STAT_COLS];
	int			i = 0;

	memset(nulls, 0, sizeof(nulls));
	values[i++] = ObjectIdGetDatum(relid);

	tuplestore_put_common(&vacuum_ext->common, values, nulls, &i);
	values[i++] = Int64GetDatum(vacuum_ext->common.blks_fetched - vacuum_ext->common.blks_hit);
	values[i++] = Int64GetDatum(vacuum_ext->common.blks_hit);

	if (vacuum_ext->type == PGSTAT_EXTVAC_TABLE)
	{
		values[i++] = Int64GetDatum(vacuum_ext->common.tuples_deleted);
		values[i++] = Int64GetDatum(vacuum_ext->table.pages_scanned);
		values[i++] = Int64GetDatum(vacuum_ext->table.pages_removed);
		values[i++] = Int64GetDatum(vacuum_ext->table.vm_new_frozen_pages);
		values[i++] = Int64GetDatum(vacuum_ext->table.vm_new_visible_pages);
		values[i++] = Int64GetDatum(vacuum_ext->table.vm_new_visible_frozen_pages);
		values[i++] = Int64GetDatum(vacuum_ext->table.tuples_frozen);
		values[i++] = Int64GetDatum(vacuum_ext->table.recently_dead_tuples);
		values[i++] = Int64GetDatum(vacuum_ext->table.index_vacuum_count);
		values[i++] = Int64GetDatum(vacuum_ext->table.missed_dead_pages);
		values[i++] = Int64GetDatum(vacuum_ext->table.missed_dead_tuples);
	}
	else if (vacuum_ext->type == PGSTAT_EXTVAC_INDEX)
	{
		values[i++] = Int64GetDatum(vacuum_ext->common.tuples_deleted);
		values[i++] = Int64GetDatum(vacuum_ext->index.pages_deleted);
	}

	Assert(i == ((vacuum_ext->type == PGSTAT_EXTVAC_TABLE) ? EXTVAC_HEAP_STAT_COLS : EXTVAC_IDX_STAT_COLS));
	tuplestore_putvalues(tupstore, tupdesc, values, nulls);
}

static Datum
pg_stats_vacuum(FunctionCallInfo fcinfo, int type)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	Tuplestorestate *tupstore;
	TupleDesc tupdesc;
	Oid			dbid = PG_GETARG_OID(0);

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ext_vacuum_statistics: set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ext_vacuum_statistics: materialize mode required")));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "ext_vacuum_statistics: return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	if (type == PGSTAT_EXTVAC_INDEX || type == PGSTAT_EXTVAC_TABLE)
	{
		Oid			relid = PG_GETARG_OID(1);
		PgStat_VacuumRelationCounts *stats;

		if (!OidIsValid(relid))
			return (Datum) 0;

		stats = (PgStat_VacuumRelationCounts *)
			pgstat_fetch_entry(PGSTAT_KIND_EXTVAC_RELATION, dbid, EXTVAC_OBJID(relid, type));

		if (!stats)
			stats = (PgStat_VacuumRelationCounts *)
				pgstat_fetch_entry(PGSTAT_KIND_EXTVAC_RELATION, InvalidOid, EXTVAC_OBJID(relid, type));

		if (stats && stats->type == type)
			tuplestore_put_for_relation(relid, tupstore, tupdesc, stats);
	}
	else if (type == PGSTAT_EXTVAC_DB)
	{
		if (OidIsValid(dbid))
		{
#define EXTVAC_DB_STAT_COLS 14
			Datum		values[EXTVAC_DB_STAT_COLS];
			bool		nulls[EXTVAC_DB_STAT_COLS];
			int			i = 0;
			PgStat_VacuumRelationCounts *stats;

			stats = (PgStat_VacuumRelationCounts *)
				pgstat_fetch_entry(PGSTAT_KIND_EXTVAC_DB, dbid, InvalidOid);
			if (stats && stats->type == PGSTAT_EXTVAC_DB)
			{
				memset(nulls, 0, sizeof(nulls));
				values[i++] = ObjectIdGetDatum(dbid);
				tuplestore_put_common(&stats->common, values, nulls, &i);
				values[i++] = Int32GetDatum(stats->common.interrupts_count);
				Assert(i == EXTVAC_DB_STAT_COLS);
				tuplestore_putvalues(tupstore, tupdesc, values, nulls);
			}
		}
		/* invalid dbid: return empty set */
	}
	else
		elog(PANIC, "ext_vacuum_statistics: invalid type %d", type);

	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(pg_stats_get_vacuum_tables);
PG_FUNCTION_INFO_V1(pg_stats_get_vacuum_indexes);
PG_FUNCTION_INFO_V1(pg_stats_get_vacuum_database);

Datum
pg_stats_get_vacuum_tables(PG_FUNCTION_ARGS)
{
	return pg_stats_vacuum(fcinfo, PGSTAT_EXTVAC_TABLE);
}

Datum
pg_stats_get_vacuum_indexes(PG_FUNCTION_ARGS)
{
	return pg_stats_vacuum(fcinfo, PGSTAT_EXTVAC_INDEX);
}

Datum
pg_stats_get_vacuum_database(PG_FUNCTION_ARGS)
{
	return pg_stats_vacuum(fcinfo, PGSTAT_EXTVAC_DB);
}

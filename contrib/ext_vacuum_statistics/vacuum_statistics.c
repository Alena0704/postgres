/*
 * ext_vacuum_statistics - Extended vacuum statistics for PostgreSQL
 *
 * This module collects detailed vacuum statistics (I/O, WAL, timing, etc.)
 * at relation and database level by hooking into the vacuum reporting path.
 * Statistics are stored via pgstat custom statistics. Management of statistics
 * storage and output functions are implemented in this module.
 */
#include "postgres.h"

#include "access/transam.h"
#include "catalog/catalog.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/pgstat_kind.h"
#include "utils/pgstat_internal.h"
#include "utils/tuplestore.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/* Two kinds: relations (tables/indexes) and database aggregates */
#define PGSTAT_KIND_EXTVAC_RELATION	24
#define PGSTAT_KIND_EXTVAC_DB		25

#define SJ_NODENAME		"vacuum_statistics"
#define EVS_TRACK_FILENAME	"pg_stat/ext_vacuum_statistics_track.oid"

/* Bit flags for evs_track (object_types): 'all', 'databases', 'relations' */
#define EVS_TRACK_RELATIONS		0x01
#define EVS_TRACK_DATABASES		0x02

/* Bit flags for evs_track_relations: 'all', 'system', 'user' */
#define EVS_FILTER_SYSTEM		0x01
#define EVS_FILTER_USER			0x02

/*  GUCs  */
static bool evs_enabled = true;
static char *evs_track = "all"; /* 'all', 'databases', 'relations' */
static char *evs_track_relations = "all";	/* 'all', 'system', 'user' */
static int	evs_track_bits = EVS_TRACK_RELATIONS | EVS_TRACK_DATABASES;
static int	evs_track_relations_bits = EVS_FILTER_SYSTEM | EVS_FILTER_USER;
static bool evs_track_databases_from_list = false;	/* if true, track only
													 * databases in list */
static bool evs_track_relations_from_list = false;	/* if true, track only
													 * relations in list */

/*  Hook  */
static set_report_vacuum_hook_type prev_report_vacuum_hook = NULL;
static object_access_hook_type prev_object_access_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;

/*  Forward declarations  */
static void pgstat_report_vacuum_extstats(Oid tableoid, bool shared,
										  PgStat_VacuumRelationCounts * params);
static bool evs_oid_in_list(HTAB *hash, Oid oid);
static void evs_track_hash_ensure_init(void);
static void evs_track_save_file(void);
static void evs_track_load_file(void);
static void evs_drop_access_hook(ObjectAccessType access, Oid classId,
								 Oid objectId, int subId, void *arg);
static void evs_shmem_request(void);

/* Hash tables for track_databases and track_relations_list (backend-local) */
static HTAB *evs_track_databases_hash = NULL;
static HTAB *evs_track_relations_hash = NULL;
static bool evs_track_hash_initialized = false;

/*
 * Named LWLock tranche protecting the on-disk track file and serializing
 * backend-local reloads/saves across concurrent backends.
 */
#define EVS_TRACK_TRANCHE_NAME "ext_vacuum_statistics_track"
static LWLock *evs_track_lock = NULL;

static inline LWLock *
evs_get_track_lock(void)
{
	if (evs_track_lock == NULL)
		evs_track_lock = &GetNamedLWLockTranche(EVS_TRACK_TRANCHE_NAME)->lock;
	return evs_track_lock;
}

/*
 * objid encoding for relations: (relid << 2) | (type & 3)
 */
#define EXTVAC_OBJID(relid, type) (((uint64) (relid)) << 2 | ((type) & 3))

/* Key for relation tracking: (dboid, reloid).
 * InvalidOid for dboid means it is a cluster object.
 */
typedef struct
{
	Oid			dboid;
	Oid			reloid;
}			EvsTrackRelKey;

/* Shared memory entry for vacuum stats; one per relation or database. */
typedef struct PgStatShared_ExtVacEntry
{
	PgStatShared_Common header;
	PgStat_VacuumRelationCounts stats;
}			PgStatShared_ExtVacEntry;

/* PgStat kind for per-relation vacuum statistics (tables/indexes) */
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

/* PgStat kind for per-database aggregated vacuum statistics */
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

#define ACCUM_IF(dst, src, field) \
	do { ((dst))->field += ((src))->field; } while (0)

static inline void
pgstat_accumulate_common(PgStat_CommonCounts * dst, const PgStat_CommonCounts * src)
{
	ACCUM_IF(dst, src, total_blks_read);
	ACCUM_IF(dst, src, total_blks_hit);
	ACCUM_IF(dst, src, total_blks_dirtied);
	ACCUM_IF(dst, src, total_blks_written);
	ACCUM_IF(dst, src, blks_fetched);
	ACCUM_IF(dst, src, blks_hit);
	ACCUM_IF(dst, src, blk_read_time);
	ACCUM_IF(dst, src, blk_write_time);
	ACCUM_IF(dst, src, delay_time);
	ACCUM_IF(dst, src, total_time);
	ACCUM_IF(dst, src, wal_records);
	ACCUM_IF(dst, src, wal_fpi);
	ACCUM_IF(dst, src, wal_bytes);
	ACCUM_IF(dst, src, wraparound_failsafe_count);
	ACCUM_IF(dst, src, interrupts_count);
	ACCUM_IF(dst, src, tuples_deleted);
}

static inline void
pgstat_accumulate_extvac_stats(PgStat_VacuumRelationCounts * dst,
							   const PgStat_VacuumRelationCounts * src)
{
	if (dst->type == PGSTAT_EXTVAC_INVALID)
		dst->type = src->type;

	Assert(src->type != PGSTAT_EXTVAC_INVALID && src->type != PGSTAT_EXTVAC_DB);
	Assert(src->type == dst->type);

	pgstat_accumulate_common(&dst->common, &src->common);

	if (dst->type == PGSTAT_EXTVAC_TABLE)
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
	else if (dst->type == PGSTAT_EXTVAC_INDEX)
	{
		dst->index.pages_deleted += src->index.pages_deleted;
	}
}

/*
 * GUC check hooks: validate the string and compute the bitmask into *extra.
 * Rejecting unknown values here prevents silent fall-through to "all".
 */
static bool
evs_track_check_hook(char **newval, void **extra, GucSource source)
{
	int		   *bits;

	if (*newval == NULL)
		return false;

	bits = (int *) guc_malloc(LOG, sizeof(int));
	if (!bits)
		return false;

	if (strcmp(*newval, "all") == 0)
		*bits = EVS_TRACK_RELATIONS | EVS_TRACK_DATABASES;
	else if (strcmp(*newval, "databases") == 0)
		*bits = EVS_TRACK_DATABASES;
	else if (strcmp(*newval, "relations") == 0)
		*bits = EVS_TRACK_RELATIONS;
	else
	{
		guc_free(bits);
		GUC_check_errdetail("Allowed values are \"all\", \"databases\", \"relations\".");
		return false;
	}
	*extra = bits;
	return true;
}

static void
evs_track_assign_hook(const char *newval, void *extra)
{
	evs_track_bits = *((int *) extra);
}

static bool
evs_track_relations_check_hook(char **newval, void **extra, GucSource source)
{
	int		   *bits;

	if (*newval == NULL)
		return false;

	bits = (int *) guc_malloc(LOG, sizeof(int));
	if (!bits)
		return false;

	if (strcmp(*newval, "all") == 0)
		*bits = EVS_FILTER_SYSTEM | EVS_FILTER_USER;
	else if (strcmp(*newval, "system") == 0)
		*bits = EVS_FILTER_SYSTEM;
	else if (strcmp(*newval, "user") == 0)
		*bits = EVS_FILTER_USER;
	else
	{
		guc_free(bits);
		GUC_check_errdetail("Allowed values are \"all\", \"system\", \"user\".");
		return false;
	}
	*extra = bits;
	return true;
}

static void
evs_track_relations_assign_hook(const char *newval, void *extra)
{
	evs_track_relations_bits = *((int *) extra);
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

	DefineCustomStringVariable("vacuum_statistics.object_types",
							   "Object types for statistics: 'all', 'databases', 'relations'.",
							   NULL, &evs_track, "all",
							   PGC_SUSET, 0,
							   evs_track_check_hook,
							   evs_track_assign_hook, NULL);

	DefineCustomStringVariable("vacuum_statistics.track_relations",
							   "When tracking relations: 'all', 'system', 'user'.",
							   NULL, &evs_track_relations, "all",
							   PGC_SUSET, 0,
							   evs_track_relations_check_hook,
							   evs_track_relations_assign_hook, NULL);

	DefineCustomBoolVariable("vacuum_statistics.track_databases_from_list",
							 "If true, track only databases added via add_track_database.",
							 NULL, &evs_track_databases_from_list, false,
							 PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable("vacuum_statistics.track_relations_from_list",
							 "If true, track only relations added via add_track_relation.",
							 NULL, &evs_track_relations_from_list, false,
							 PGC_SUSET, 0, NULL, NULL, NULL);

	MarkGUCPrefixReserved(SJ_NODENAME);

	pgstat_register_kind(PGSTAT_KIND_EXTVAC_RELATION, &extvac_relation_kind_info);
	pgstat_register_kind(PGSTAT_KIND_EXTVAC_DB, &extvac_db_kind_info);

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = evs_shmem_request;

	prev_report_vacuum_hook = set_report_vacuum_hook;
	set_report_vacuum_hook = pgstat_report_vacuum_extstats;

	prev_object_access_hook = object_access_hook;
	object_access_hook = evs_drop_access_hook;
}

static void
evs_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestNamedLWLockTranche(EVS_TRACK_TRANCHE_NAME, 1);
}

/*
 * Object access hook: remove dropped objects from track lists.
 */
static void
evs_drop_access_hook(ObjectAccessType access, Oid classId,
					 Oid objectId, int subId, void *arg)
{
	if (prev_object_access_hook)
		(*prev_object_access_hook) (access, classId, objectId, subId, arg);

	if (access == OAT_DROP)
	{
		if (classId == RelationRelationId && subId == 0)
		{
			char		relkind = get_rel_relkind(objectId);
			EvsTrackRelKey key;
			bool		found;

			if (relkind == RELKIND_RELATION || relkind == RELKIND_INDEX)
			{
				LWLock	   *lock = evs_get_track_lock();

				LWLockAcquire(lock, LW_EXCLUSIVE);
				evs_track_hash_ensure_init();
				key.dboid = MyDatabaseId;
				key.reloid = objectId;
				hash_search(evs_track_relations_hash, &key, HASH_REMOVE, &found);
				key.dboid = InvalidOid;
				hash_search(evs_track_relations_hash, &key, HASH_REMOVE, &found);
				evs_track_save_file();
				LWLockRelease(lock);
			}
		}

		if (classId == DatabaseRelationId && objectId != InvalidOid)
		{
			LWLock	   *lock = evs_get_track_lock();
			bool		found;

			LWLockAcquire(lock, LW_EXCLUSIVE);
			evs_track_hash_ensure_init();
			hash_search(evs_track_databases_hash, &objectId, HASH_REMOVE, &found);
			evs_track_save_file();
			LWLockRelease(lock);
		}
	}
}

/*
 * Storage of track lists in a separate file.
 *
 * Stores the lists of database OIDs and (dboid, reloid) pairs used for
 * selective tracking when track_databases_from_list or track_relations_from_list
 * is enabled.
 * Data stores in pg_stat/ext_vacuum_statistics_track.oid
 */
/*
 * Initialize the backend-local tracking hashes and load their contents
 * from the on-disk file.
 *
 * The hashes are per-backend, so no lock is needed to protect them from
 * other processes; however, another backend may be concurrently rewriting
 * the track file, so we take a shared lock for the file read.
 */
static void
evs_track_hash_ensure_init(void)
{
	HASHCTL		ctl;
	LWLock	   *lock;
	bool		need_load;

	if (evs_track_hash_initialized)
		return;

	lock = evs_get_track_lock();

	if (evs_track_databases_hash == NULL)
	{
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(Oid);
		ctl.hcxt = TopMemoryContext;
		evs_track_databases_hash =
			hash_create("ext_vacuum_statistics track databases",
						64, &ctl, HASH_ELEM | HASH_BLOBS);
	}

	if (evs_track_relations_hash == NULL)
	{
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(EvsTrackRelKey);
		ctl.entrysize = sizeof(EvsTrackRelKey);
		ctl.hcxt = TopMemoryContext;
		evs_track_relations_hash =
			hash_create("ext_vacuum_statistics track relations",
						64, &ctl, HASH_ELEM | HASH_BLOBS);
	}

	need_load = !LWLockHeldByMe(lock);
	if (need_load)
		LWLockAcquire(lock, LW_SHARED);
	PG_TRY();
	{
		evs_track_load_file();
		evs_track_hash_initialized = true;
	}
	PG_FINALLY();
	{
		if (need_load)
			LWLockRelease(lock);
	}
	PG_END_TRY();
}

/*
 * Load track lists from disk into the backend-local hashes.
 *
 * Caller must hold evs_track_lock at least in shared mode, since the file
 * may be concurrently rewritten by another backend.
 */
static void
evs_track_load_file(void)
{
	char		path[MAXPGPATH];
	FILE	   *fp;
	char		buf[MAXPGPATH];
	bool		in_relations = false;
	Oid			oid;
	EvsTrackRelKey key;
	bool		found;

	if (!DataDir || DataDir[0] == '\0' ||
		!evs_track_databases_hash || !evs_track_relations_hash)
		return;

	snprintf(path, sizeof(path), "%s/%s", DataDir, EVS_TRACK_FILENAME);
	fp = AllocateFile(path, "r");
	if (!fp)
	{
		if (errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not open track file \"%s\": %m", path)));
		return;
	}

	PG_TRY();
	{
		while (fgets(buf, sizeof(buf), fp))
		{
			size_t		len = strlen(buf);

			/* Reject unterminated lines (longer than buffer) as corruption. */
			if (len > 0 && buf[len - 1] != '\n' && !feof(fp))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("line too long in track file \"%s\"", path)));

			if (strncmp(buf, "[databases]", 11) == 0)
			{
				in_relations = false;
				continue;
			}
			if (strncmp(buf, "[relations]", 11) == 0)
			{
				in_relations = true;
				continue;
			}
			if (in_relations)
			{
				if (sscanf(buf, "%u %u", &key.dboid, &key.reloid) == 2)
					hash_search(evs_track_relations_hash, &key, HASH_ENTER, &found);
				else if (sscanf(buf, "%u", &oid) == 1)
				{
					key.dboid = InvalidOid;
					key.reloid = oid;
					hash_search(evs_track_relations_hash, &key, HASH_ENTER, &found);
				}
			}
			else if (sscanf(buf, "%u", &oid) == 1)
				hash_search(evs_track_databases_hash, &oid, HASH_ENTER, &found);
		}

		if (ferror(fp))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read track file \"%s\": %m", path)));
	}
	PG_FINALLY();
	{
		FreeFile(fp);
	}
	PG_END_TRY();
}

/*
 * Atomically rewrite the track file. Caller must hold evs_track_lock
 * in exclusive mode.
 */
static void
evs_track_save_file(void)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	FILE	   *fp;
	HASH_SEQ_STATUS status;
	Oid		   *entry;
	EvsTrackRelKey *rel_entry;
	bool		failed = false;

	if (!DataDir || DataDir[0] == '\0' ||
		!evs_track_databases_hash || !evs_track_relations_hash)
		return;

	snprintf(path, sizeof(path), "%s/%s", DataDir, EVS_TRACK_FILENAME);
	snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);

	fp = AllocateFile(tmppath, PG_BINARY_W);
	if (!fp)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not create track file \"%s\": %m", tmppath)));
		return;
	}

	PG_TRY();
	{
		if (fputs("[databases]\n", fp) == EOF)
			failed = true;

		if (!failed)
		{
			hash_seq_init(&status, evs_track_databases_hash);
			while ((entry = (Oid *) hash_seq_search(&status)) != NULL)
			{
				if (fprintf(fp, "%u\n", *entry) < 0)
				{
					hash_seq_term(&status);
					failed = true;
					break;
				}
			}
		}

		if (!failed && fputs("[relations]\n", fp) == EOF)
			failed = true;

		if (!failed)
		{
			hash_seq_init(&status, evs_track_relations_hash);
			while ((rel_entry = (EvsTrackRelKey *) hash_seq_search(&status)) != NULL)
			{
				int			rc;

				if (OidIsValid(rel_entry->dboid))
					rc = fprintf(fp, "%u %u\n", rel_entry->dboid, rel_entry->reloid);
				else
					rc = fprintf(fp, "0 %u\n", rel_entry->reloid);
				if (rc < 0)
				{
					hash_seq_term(&status);
					failed = true;
					break;
				}
			}
		}

		if (!failed && fflush(fp) != 0)
			failed = true;

		if (!failed)
		{
			int			fd = fileno(fp);

			if (fd >= 0 && pg_fsync(fd) != 0)
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not fsync track file \"%s\": %m",
								tmppath)));
		}
	}
	PG_CATCH();
	{
		FreeFile(fp);
		(void) unlink(tmppath);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (FreeFile(fp) != 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not close track file \"%s\": %m", tmppath)));
		failed = true;
	}

	if (failed)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write track file \"%s\": %m", tmppath)));
		if (unlink(tmppath) != 0 && errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not unlink \"%s\": %m", tmppath)));
		return;
	}

	if (durable_rename(tmppath, path, LOG) != 0)
	{
		if (unlink(tmppath) != 0 && errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not unlink \"%s\": %m", tmppath)));
	}
}

/*
 * Check if OID is in the given hash
 */
static bool
evs_oid_in_list(HTAB *hash, Oid oid)
{
	if (!hash)
		return false;
	if (hash_get_num_entries(hash) == 0)
		return false;
	return hash_search(hash, &oid, HASH_FIND, NULL) != NULL;
}

/*
 * Check if (dboid, relid) is in track_relations list.
 */
static bool
evs_rel_in_list(Oid dboid, Oid relid)
{
	EvsTrackRelKey key;

	if (!evs_track_relations_hash)
		return false;
	if (hash_get_num_entries(evs_track_relations_hash) == 0)
		return false;
	key.dboid = dboid;
	key.reloid = relid;
	if (hash_search(evs_track_relations_hash, &key, HASH_FIND, NULL) != NULL)
		return true;
	key.dboid = InvalidOid;
	return hash_search(evs_track_relations_hash, &key, HASH_FIND, NULL) != NULL;
}

/*
 * Decide whether to track statistics for relations.
 * Relation is tracked if it is in the track list or a special filter is enabled.
 */
static bool
evs_should_track_relation_statistics(Oid dboid, Oid relid)
{
	evs_track_hash_ensure_init();

	if (evs_track_databases_from_list &&
		!evs_oid_in_list(evs_track_databases_hash, dboid))
		return false;
	if (evs_track_relations_from_list &&
		!(evs_rel_in_list(dboid, relid) || evs_rel_in_list(InvalidOid, relid)))
		return false;

	if ((evs_track_bits & EVS_TRACK_RELATIONS) == 0)
		return false;			/* database-only mode */
	if (evs_track_relations_bits == EVS_FILTER_SYSTEM)
		return IsCatalogRelationOid(relid);
	if (evs_track_relations_bits == EVS_FILTER_USER)
		return !IsCatalogRelationOid(relid);
	return true;
}

/*
 * Decide whether to track statistics for databases.
 * Database statistics is tracked if it is in the track list or a special filter is enabled.
 */
static bool
evs_should_track_database_statistics(Oid dboid)
{
	evs_track_hash_ensure_init();

	if (evs_track_databases_from_list &&
		!evs_oid_in_list(evs_track_databases_hash, dboid))
		return false;
	if ((evs_track_bits & EVS_TRACK_DATABASES) == 0)
		return false;			/* relations-only mode */
	if (evs_track_bits == EVS_TRACK_DATABASES)
		return true;			/* databases-only, accumulate to db */
	return true;
}


/* Accumulate common counts for database-level stats. */
static inline void
pgstat_accumulate_common_for_db(PgStat_CommonCounts * dst,
								const PgStat_CommonCounts * src)
{
	pgstat_accumulate_common(dst, src);
}

/*
 * Store incoming vacuum stats into pgstat custom statistics.
 * store_relation: create/update per-relation entry
 * store_db: accumulate into database-level entry (dboid, objid=0).
 * Uses pgstat_get_entry_ref_locked and pgstat_accumulate_* for atomic updates.
 */
static void
extvac_store(Oid dboid, Oid relid, int type,
			 PgStat_VacuumRelationCounts * params,
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

/*
 * Vacuum report hook: called when vacuum finishes. Filters by track settings,
 * stores stats per-relation and/or per-database, then chains to previous hook.
 */
static void
pgstat_report_vacuum_extstats(Oid tableoid, bool shared,
							  PgStat_VacuumRelationCounts * params)
{
	Oid			dboid = shared ? InvalidOid : MyDatabaseId;
	bool		store_relation;
	bool		store_db;

	if (evs_enabled)
	{
		store_relation = evs_should_track_relation_statistics(dboid, tableoid);
		store_db = evs_should_track_database_statistics(dboid);

		if (store_relation || store_db)
			extvac_store(dboid, tableoid, params->type, params, store_relation, store_db);
	}
	if (prev_report_vacuum_hook)
		prev_report_vacuum_hook(tableoid, shared, params);
}

/* Reset statistics for a single relation entry. */
static bool
extvac_reset_by_relid(Oid dboid, Oid relid, int type)
{
	uint64		objid = EXTVAC_OBJID(relid, type);

	pgstat_reset_entry(PGSTAT_KIND_EXTVAC_RELATION, dboid, objid, 0);
	return true;
}

/* Callback for pgstat_reset_matching_entries: match relation entries for given db */
static bool
match_extvac_relations_for_db(PgStatShared_HashEntry *entry, Datum match_data)
{
	return entry->key.kind == PGSTAT_KIND_EXTVAC_RELATION &&
		entry->key.dboid == DatumGetObjectId(match_data);
}

/*
 * Reset statistics for a database (aggregate entry) and all its relations.
 */
static int64
extvac_database_reset(Oid dboid)
{
	pgstat_reset_matching_entries(match_extvac_relations_for_db,
								  ObjectIdGetDatum(dboid), 0);
	pgstat_reset_entry(PGSTAT_KIND_EXTVAC_DB, dboid, 0, 0);
	return 1;
}

/* Reset all vacuum statistics (both relation and database entries). */
static int64
extvac_stat_reset(void)
{
	pgstat_reset_of_kind(PGSTAT_KIND_EXTVAC_RELATION);
	pgstat_reset_of_kind(PGSTAT_KIND_EXTVAC_DB);
	return 0;					/* count not available */
}

PG_FUNCTION_INFO_V1(vacuum_statistics_reset);
PG_FUNCTION_INFO_V1(extvac_shared_memory_size);
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

/*
 * Return total shared memory in bytes used by the extension for vacuum stats.
 * Used for monitoring and capacity planning: memory grows with the number of
 * tracked relations and databases.
 */
Datum
extvac_shared_memory_size(PG_FUNCTION_ARGS)
{
	uint64		rel_count;
	uint64		db_count;
	uint64		total;
	size_t		entry_size = sizeof(PgStatShared_ExtVacEntry);

	rel_count = pgstat_get_entry_count(PGSTAT_KIND_EXTVAC_RELATION);
	db_count = pgstat_get_entry_count(PGSTAT_KIND_EXTVAC_DB);
	total = rel_count + db_count;

	PG_RETURN_INT64((int64) (total * entry_size));
}

/*
 * Track list management: add/remove database or relation OIDs.
 * Changes are persisted to pg_stat/ext_vacuum_statistics_track.oid.
 */

PG_FUNCTION_INFO_V1(evs_add_track_database);
PG_FUNCTION_INFO_V1(evs_remove_track_database);
PG_FUNCTION_INFO_V1(evs_add_track_relation);
PG_FUNCTION_INFO_V1(evs_remove_track_relation);

/*
 * Mutating track-list entry points: require server-wide privilege, since
 * the underlying lists steer tracking for every backend.
 */
static void
evs_require_track_privilege(const char *funcname)
{
	if (!superuser() && !has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for function %s", funcname),
				 errhint("Only superusers and members of pg_read_all_stats "
						 "may change the vacuum statistics track list.")));
}

Datum
evs_add_track_database(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		found;
	LWLock	   *lock;

	evs_require_track_privilege("add_track_database");
	lock = evs_get_track_lock();
	LWLockAcquire(lock, LW_EXCLUSIVE);
	evs_track_hash_ensure_init();
	hash_search(evs_track_databases_hash, &oid, HASH_ENTER, &found);
	evs_track_save_file();
	LWLockRelease(lock);
	PG_RETURN_BOOL(!found);		/* true if newly added */
}

Datum
evs_remove_track_database(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		found;
	LWLock	   *lock;

	evs_require_track_privilege("remove_track_database");
	lock = evs_get_track_lock();
	LWLockAcquire(lock, LW_EXCLUSIVE);
	evs_track_hash_ensure_init();
	hash_search(evs_track_databases_hash, &oid, HASH_REMOVE, &found);
	evs_track_save_file();
	LWLockRelease(lock);
	PG_RETURN_BOOL(found);
}

Datum
evs_add_track_relation(PG_FUNCTION_ARGS)
{
	EvsTrackRelKey key;
	bool		found;
	LWLock	   *lock;

	evs_require_track_privilege("add_track_relation");
	key.dboid = PG_GETARG_OID(0);
	key.reloid = PG_GETARG_OID(1);
	lock = evs_get_track_lock();
	LWLockAcquire(lock, LW_EXCLUSIVE);
	evs_track_hash_ensure_init();
	hash_search(evs_track_relations_hash, &key, HASH_ENTER, &found);
	evs_track_save_file();
	LWLockRelease(lock);
	PG_RETURN_BOOL(!found);		/* true if newly added */
}

Datum
evs_remove_track_relation(PG_FUNCTION_ARGS)
{
	EvsTrackRelKey key;
	bool		found;
	LWLock	   *lock;

	evs_require_track_privilege("remove_track_relation");
	key.dboid = PG_GETARG_OID(0);
	key.reloid = PG_GETARG_OID(1);
	lock = evs_get_track_lock();
	LWLockAcquire(lock, LW_EXCLUSIVE);
	evs_track_hash_ensure_init();
	hash_search(evs_track_relations_hash, &key, HASH_REMOVE, &found);
	evs_track_save_file();
	LWLockRelease(lock);
	PG_RETURN_BOOL(found);
}

/*
 * Returns the list of database and relation OIDs for which statistics
 * are collected.
 */
PG_FUNCTION_INFO_V1(evs_track_list);

Datum
evs_track_list(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	HASH_SEQ_STATUS status;
	Oid		   *entry;
	EvsTrackRelKey *rel_entry;

	if (!rsinfo || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ext_vacuum_statistics: set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ext_vacuum_statistics: materialize mode required")));

	evs_track_hash_ensure_init();

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "ext_vacuum_statistics: return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	/* Databases */
	if (hash_get_num_entries(evs_track_databases_hash) == 0)
	{
		values[0] = CStringGetTextDatum("database");
		nulls[1] = true;
		nulls[2] = true;
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		nulls[1] = false;
		nulls[2] = false;
	}
	else
	{
		hash_seq_init(&status, evs_track_databases_hash);
		while ((entry = (Oid *) hash_seq_search(&status)) != NULL)
		{
			values[0] = CStringGetTextDatum("database");
			values[1] = ObjectIdGetDatum(*entry);
			nulls[2] = true;
			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
			nulls[2] = false;
		}
	}

	/* Relations */
	if (hash_get_num_entries(evs_track_relations_hash) == 0)
	{
		values[0] = CStringGetTextDatum("relation");
		nulls[1] = true;
		nulls[2] = true;
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		nulls[1] = false;
		nulls[2] = false;
	}
	else
	{
		hash_seq_init(&status, evs_track_relations_hash);
		while ((rel_entry = (EvsTrackRelKey *) hash_seq_search(&status)) != NULL)
		{
			values[0] = CStringGetTextDatum("relation");
			values[1] = ObjectIdGetDatum(rel_entry->dboid);
			values[2] = ObjectIdGetDatum(rel_entry->reloid);
			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		}
	}

	MemoryContextSwitchTo(oldcontext);

	return (Datum) 0;
}

/*
 * Output vacuum statistics (tables, indexes, or per-database aggregates).
 */
#define EXTVAC_COMMON_STAT_COLS 12

static void
tuplestore_put_common(PgStat_CommonCounts * vacuum_ext,
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
							TupleDesc tupdesc, PgStat_VacuumRelationCounts * vacuum_ext)
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
	TupleDesc	tupdesc;
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
			pgstat_fetch_entry(PGSTAT_KIND_EXTVAC_RELATION, dbid,
							   EXTVAC_OBJID(relid, type), NULL);

		if (!stats)
			stats = (PgStat_VacuumRelationCounts *)
				pgstat_fetch_entry(PGSTAT_KIND_EXTVAC_RELATION, InvalidOid,
								   EXTVAC_OBJID(relid, type), NULL);

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
				pgstat_fetch_entry(PGSTAT_KIND_EXTVAC_DB, dbid,
								   InvalidOid, NULL);
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

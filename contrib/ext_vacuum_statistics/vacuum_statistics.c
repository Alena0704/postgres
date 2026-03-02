#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
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

/* --- GUCs --- */
static bool evs_enabled = true;

/* --- Hook chaining --- */
static set_report_vacuum_hook_type prev_report_vacuum_hook = NULL;

/* --- Forward declarations --- */
static void pgstat_report_vacuum_extstats(Oid tableoid, bool shared,
										 PgStat_VacuumRelationCounts *params);

/* objid encoding for relations: (relid << 2) | (type & 3) */
#define EXTVAC_OBJID(relid, type) (((uint64) (relid)) << 2 | ((type) & 3))

/* Database aggregate: one entry per db, objid = 0 */
#define EXTVAC_DB_OBJID	0

#define ACCUMULATE_FIELD(field) dst->field += src->field
#define ACCUMULATE_SUBFIELD(substruct, field) (dst->substruct.field += src->substruct.field)

static inline void
pgstat_accumulate_common(PgStat_CommonCounts *dst, const PgStat_CommonCounts *src)
{
	ACCUMULATE_FIELD(total_blks_read);
	ACCUMULATE_FIELD(total_blks_hit);
	ACCUMULATE_FIELD(total_blks_dirtied);
	ACCUMULATE_FIELD(total_blks_written);
	ACCUMULATE_FIELD(blks_fetched);
	ACCUMULATE_FIELD(blks_hit);
	ACCUMULATE_FIELD(blk_read_time);
	ACCUMULATE_FIELD(blk_write_time);
	ACCUMULATE_FIELD(delay_time);
	ACCUMULATE_FIELD(total_time);
	ACCUMULATE_FIELD(wal_records);
	ACCUMULATE_FIELD(wal_fpi);
	ACCUMULATE_FIELD(wal_bytes);
	ACCUMULATE_FIELD(wraparound_failsafe_count);
	ACCUMULATE_FIELD(interrupts_count);
	ACCUMULATE_FIELD(tuples_deleted);
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

	if (dst->type == PGSTAT_EXTVAC_TABLE)
	{
		ACCUMULATE_SUBFIELD(table, pages_scanned);
		ACCUMULATE_SUBFIELD(table, pages_removed);
		ACCUMULATE_SUBFIELD(table, tuples_frozen);
		ACCUMULATE_SUBFIELD(table, recently_dead_tuples);
		ACCUMULATE_SUBFIELD(table, vm_new_frozen_pages);
		ACCUMULATE_SUBFIELD(table, vm_new_visible_pages);
		ACCUMULATE_SUBFIELD(table, vm_new_visible_frozen_pages);
		ACCUMULATE_SUBFIELD(table, missed_dead_pages);
		ACCUMULATE_SUBFIELD(table, missed_dead_tuples);
		ACCUMULATE_SUBFIELD(table, index_vacuum_count);
	}
	else if (dst->type == PGSTAT_EXTVAC_INDEX)
	{
		ACCUMULATE_SUBFIELD(index, pages_deleted);
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
 * Also accumulate into database-level entry (dboid, objid=0).
 */
static void
extvac_store(Oid dboid, Oid relid, int type,
			 PgStat_VacuumRelationCounts *params)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_ExtVacEntry *shared;
	uint64		objid;

	if (!evs_enabled)
		return;

	/* Relation entry: (kind, dboid, (relid<<2)|type) */
	objid = EXTVAC_OBJID(relid, type);
	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_EXTVAC_RELATION, dboid, objid, false);
	if (entry_ref)
	{
		shared = (PgStatShared_ExtVacEntry *) entry_ref->shared_stats;
		if (shared->stats.type == PGSTAT_EXTVAC_INVALID)
		{
			memcpy(&shared->stats, params, sizeof(shared->stats));
		}
		else
		{
			pgstat_accumulate_extvac_stats(&shared->stats, params);
		}
		pgstat_unlock_entry(entry_ref);
	}

	/* Database aggregate: (kind, dboid, objid=0) */
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

static void
pgstat_report_vacuum_extstats(Oid tableoid, bool shared,
							  PgStat_VacuumRelationCounts *params)
{
	Oid			dboid = shared ? InvalidOid : MyDatabaseId;

	extvac_store(dboid, tableoid, params->type, params);

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

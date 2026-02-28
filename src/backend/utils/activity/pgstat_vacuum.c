#include "postgres.h"

#include "pgstat.h"
#include "utils/pgstat_internal.h"
#include "utils/memutils.h"

/* ----------
 * GUC parameters
 * ----------
 */
bool		pgstat_track_vacuum_statistics_for_relations = false;

#define ACCUMULATE_FIELD(field) dst->field += src->field;

#define ACCUMULATE_SUBFIELD(substruct, field) \
    (dst->substruct.field += src->substruct.field)

static void
pgstat_accumulate_common(PgStat_CommonCounts * dst, const PgStat_CommonCounts * src)
{
	ACCUMULATE_FIELD(total_blks_read);
	ACCUMULATE_FIELD(total_blks_hit);
	ACCUMULATE_FIELD(total_blks_dirtied);
	ACCUMULATE_FIELD(total_blks_written);

	ACCUMULATE_FIELD(blks_fetched);
	ACCUMULATE_FIELD(blks_hit);

	ACCUMULATE_FIELD(wal_records);
	ACCUMULATE_FIELD(wal_fpi);
	ACCUMULATE_FIELD(wal_bytes);

	ACCUMULATE_FIELD(blk_read_time);
	ACCUMULATE_FIELD(blk_write_time);
	ACCUMULATE_FIELD(delay_time);
	ACCUMULATE_FIELD(total_time);

	ACCUMULATE_FIELD(tuples_deleted);
	ACCUMULATE_FIELD(wraparound_failsafe_count);
}

static void
pgstat_accumulate_extvac_stats_relations(PgStat_VacuumRelationCounts * dst, PgStat_VacuumRelationCounts * src)
{
	if (!pgstat_track_vacuum_statistics)
		return;

	if (dst->type == PGSTAT_EXTVAC_INVALID)
		dst->type = src->type;

	Assert(src->type != PGSTAT_EXTVAC_INVALID && src->type != PGSTAT_EXTVAC_DB && src->type == dst->type);

	pgstat_accumulate_common(&dst->common, &src->common);

	ACCUMULATE_SUBFIELD(common, blks_fetched);
	ACCUMULATE_SUBFIELD(common, blks_hit);

	if (dst->type == PGSTAT_EXTVAC_TABLE)
	{
		ACCUMULATE_SUBFIELD(common, tuples_deleted);
		ACCUMULATE_SUBFIELD(table, pages_scanned);
		ACCUMULATE_SUBFIELD(table, pages_removed);
		ACCUMULATE_SUBFIELD(table, vm_new_frozen_pages);
		ACCUMULATE_SUBFIELD(table, vm_new_visible_pages);
		ACCUMULATE_SUBFIELD(table, vm_new_visible_frozen_pages);
		ACCUMULATE_SUBFIELD(table, tuples_frozen);
		ACCUMULATE_SUBFIELD(table, recently_dead_tuples);
		ACCUMULATE_SUBFIELD(table, index_vacuum_count);
		ACCUMULATE_SUBFIELD(table, missed_dead_pages);
		ACCUMULATE_SUBFIELD(table, missed_dead_tuples);
	}
	else if (dst->type == PGSTAT_EXTVAC_INDEX)
	{
		ACCUMULATE_SUBFIELD(common, tuples_deleted);
		ACCUMULATE_SUBFIELD(index, pages_deleted);
	}
}

static void
pgstat_accumulate_extvac_stats_db(PgStat_VacuumDBCounts * dst, PgStat_VacuumDBCounts * src)
{
	if (!pgstat_track_vacuum_statistics)
		return;

	pgstat_accumulate_common(&dst->common, &src->common);
}

/*
 * Report that the table was just vacuumed and flush statistics.
 */
void
pgstat_report_vacuum_extstats(Oid tableoid, bool shared,
							  PgStat_VacuumRelationCounts * params)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_VacuumRelation *shtabentry;
	PgStatShared_VacuumDB *shdbentry;
	Oid			dboid = (shared ? InvalidOid : MyDatabaseId);

	if (!pgstat_track_vacuum_statistics)
		return;

	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_VACUUM_RELATION,
											dboid, tableoid, false);
	shtabentry = (PgStatShared_VacuumRelation *) entry_ref->shared_stats;
	pgstat_accumulate_extvac_stats_relations(&shtabentry->stats, params);

	pgstat_unlock_entry(entry_ref);

	if (!shared)
		entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_VACUUM_DB,
												dboid, InvalidOid, false);
	else
		entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_VACUUM_DB,
												MyDatabaseId, InvalidOid, false);

	shdbentry = (PgStatShared_VacuumDB *) entry_ref->shared_stats;

	pgstat_accumulate_common(&shdbentry->stats.common, &params->common);

	pgstat_unlock_entry(entry_ref);
}

/*
 * Flush out pending stats for the entry
 *
 * If nowait is true, this function returns false if lock could not
 * immediately acquired, otherwise true is returned.
 */
bool
pgstat_vacuum_relation_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStatShared_VacuumRelation *shtabstats;
	PgStat_RelationVacuumPending *pendingent;	/* table entry of shared stats */

	pendingent = (PgStat_RelationVacuumPending *) entry_ref->pending;
	shtabstats = (PgStatShared_VacuumRelation *) entry_ref->shared_stats;

	/*
	 * Ignore entries that didn't accumulate any actual counts.
	 */
	if (pg_memory_is_all_zeros(&pendingent,
							   sizeof(struct PgStat_RelationVacuumPending)))
		return true;

	if (!pgstat_lock_entry(entry_ref, nowait))
	{
		return false;
	}

	pgstat_accumulate_extvac_stats_relations(&(shtabstats->stats), &(pendingent->counts));

	pgstat_unlock_entry(entry_ref);

	return true;
}

/*
 * Support function for the SQL-callable pgstat* functions. Returns
 * the vacuum collected statistics for one relation or NULL.
 */
PgStat_VacuumRelationCounts *
pgstat_fetch_stat_vacuum_tabentry(Oid relid, Oid dbid)
{
	return (PgStat_VacuumRelationCounts *)
		pgstat_fetch_entry(PGSTAT_KIND_VACUUM_RELATION, dbid, relid);
}

PgStat_VacuumDBCounts *
pgstat_fetch_stat_vacuum_dbentry(Oid dbid)
{
	return (PgStat_VacuumDBCounts *)
		pgstat_fetch_entry(PGSTAT_KIND_VACUUM_DB, dbid, InvalidOid);
}

bool
pgstat_vacuum_db_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStatShared_VacuumDB *sharedent;
	PgStat_VacuumDBCounts *pendingent;

	pendingent = (PgStat_VacuumDBCounts *) entry_ref->pending;
	sharedent = (PgStatShared_VacuumDB *) entry_ref->shared_stats;

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

	/* The entry was successfully flushed, add the same to database stats */
	pgstat_accumulate_extvac_stats_db(&(sharedent->stats), pendingent);

	pgstat_unlock_entry(entry_ref);

	return true;
}

/*
 * Find or create a local PgStat_VacuumDBCounts entry for dboid.
 */
PgStat_VacuumDBCounts *
pgstat_prep_vacuum_database_pending(Oid dboid)
{
	PgStat_EntryRef *entry_ref;

	/*
	 * This should not report stats on database objects before having
	 * connected to a database.
	 */
	Assert(!OidIsValid(dboid) || OidIsValid(MyDatabaseId));

	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_VACUUM_DB, dboid, InvalidOid,
										  NULL);

	if (entry_ref == NULL)
		return NULL;

	return entry_ref->pending;
}

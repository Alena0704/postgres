/*-------------------------------------------------------------------------
 *
 * ext_vacuum_statistics--1.0.sql
 *    Extended vacuum statistics via hook and custom storage
 *
 * This extension collects extended vacuum statistics via set_report_vacuum_hook
 * and stores them in shared memory.
 *
 *-------------------------------------------------------------------------
 */

\echo Use "CREATE EXTENSION ext_vacuum_statistics" to load this file. \quit

CREATE SCHEMA IF NOT EXISTS ext_vacuum_statistics;

COMMENT ON SCHEMA ext_vacuum_statistics IS
  'Extended vacuum statistics (heap, index, database)';

-- Reset functions
CREATE OR REPLACE FUNCTION ext_vacuum_statistics.extvac_reset_entry(
    dboid oid,
    relid oid,
    type int4
)
RETURNS boolean
AS 'MODULE_PATHNAME', 'extvac_reset_entry'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION ext_vacuum_statistics.extvac_reset_db_entry(dboid oid)
RETURNS bigint
AS 'MODULE_PATHNAME', 'extvac_reset_db_entry'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION ext_vacuum_statistics.vacuum_statistics_reset()
RETURNS bigint
AS 'MODULE_PATHNAME', 'vacuum_statistics_reset'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION ext_vacuum_statistics.shared_memory_size()
RETURNS bigint
AS 'MODULE_PATHNAME', 'extvac_shared_memory_size'
LANGUAGE C STRICT PARALLEL SAFE;

COMMENT ON FUNCTION ext_vacuum_statistics.shared_memory_size() IS
  'Total shared memory in bytes used by the extension for vacuum statistics.';

-- Add/remove OIDs for tracking
CREATE OR REPLACE FUNCTION ext_vacuum_statistics.add_track_database(dboid oid)
RETURNS boolean
AS 'MODULE_PATHNAME', 'evs_add_track_database'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION ext_vacuum_statistics.remove_track_database(dboid oid)
RETURNS boolean
AS 'MODULE_PATHNAME', 'evs_remove_track_database'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION ext_vacuum_statistics.add_track_relation(dboid oid, reloid oid)
RETURNS boolean
AS 'MODULE_PATHNAME', 'evs_add_track_relation'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION ext_vacuum_statistics.remove_track_relation(dboid oid, reloid oid)
RETURNS boolean
AS 'MODULE_PATHNAME', 'evs_remove_track_relation'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION ext_vacuum_statistics.track_list()
RETURNS TABLE(track_kind text, dboid oid, reloid oid)
AS 'MODULE_PATHNAME', 'evs_track_list'
LANGUAGE C STRICT;

COMMENT ON FUNCTION ext_vacuum_statistics.track_list() IS
  'List of database and relation OIDs for which vacuum statistics are collected.';

-- Track-list mutation requires superuser or pg_read_all_stats; hide the
-- functions from PUBLIC so the error is also produced for ordinary users
-- before the C-level privilege check runs.
REVOKE ALL ON FUNCTION ext_vacuum_statistics.add_track_database(oid) FROM PUBLIC;
REVOKE ALL ON FUNCTION ext_vacuum_statistics.remove_track_database(oid) FROM PUBLIC;
REVOKE ALL ON FUNCTION ext_vacuum_statistics.add_track_relation(oid, oid) FROM PUBLIC;
REVOKE ALL ON FUNCTION ext_vacuum_statistics.remove_track_relation(oid, oid) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION ext_vacuum_statistics.add_track_database(oid) TO pg_read_all_stats;
GRANT EXECUTE ON FUNCTION ext_vacuum_statistics.remove_track_database(oid) TO pg_read_all_stats;
GRANT EXECUTE ON FUNCTION ext_vacuum_statistics.add_track_relation(oid, oid) TO pg_read_all_stats;
GRANT EXECUTE ON FUNCTION ext_vacuum_statistics.remove_track_relation(oid, oid) TO pg_read_all_stats;

-- Internal C function to fetch table vacuum stats
CREATE OR REPLACE FUNCTION ext_vacuum_statistics.pg_stats_get_vacuum_tables(
    IN  dboid oid,
    IN  reloid oid,
    OUT relid oid,
    OUT total_blks_read bigint,
    OUT total_blks_hit bigint,
    OUT total_blks_dirtied bigint,
    OUT total_blks_written bigint,
    OUT wal_records bigint,
    OUT wal_fpi bigint,
    OUT wal_bytes numeric,
    OUT blk_read_time double precision,
    OUT blk_write_time double precision,
    OUT delay_time double precision,
    OUT total_time double precision,
    OUT wraparound_failsafe_count integer,
    OUT rel_blks_read bigint,
    OUT rel_blks_hit bigint,
    OUT tuples_deleted bigint,
    OUT pages_scanned bigint,
    OUT pages_removed bigint,
    OUT vm_new_frozen_pages bigint,
    OUT vm_new_visible_pages bigint,
    OUT vm_new_visible_frozen_pages bigint,
    OUT tuples_frozen bigint,
    OUT recently_dead_tuples bigint,
    OUT index_vacuum_count bigint,
    OUT missed_dead_pages bigint,
    OUT missed_dead_tuples bigint
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stats_get_vacuum_tables'
LANGUAGE C STRICT STABLE;

-- Internal C function to fetch index vacuum stats
CREATE OR REPLACE FUNCTION ext_vacuum_statistics.pg_stats_get_vacuum_indexes(
    IN  dboid oid,
    IN  reloid oid,
    OUT relid oid,
    OUT total_blks_read bigint,
    OUT total_blks_hit bigint,
    OUT total_blks_dirtied bigint,
    OUT total_blks_written bigint,
    OUT wal_records bigint,
    OUT wal_fpi bigint,
    OUT wal_bytes numeric,
    OUT blk_read_time double precision,
    OUT blk_write_time double precision,
    OUT delay_time double precision,
    OUT total_time double precision,
    OUT wraparound_failsafe_count integer,
    OUT rel_blks_read bigint,
    OUT rel_blks_hit bigint,
    OUT tuples_deleted bigint,
    OUT pages_deleted bigint
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stats_get_vacuum_indexes'
LANGUAGE C STRICT STABLE;

-- Internal C function to fetch database vacuum stats
CREATE OR REPLACE FUNCTION ext_vacuum_statistics.pg_stats_get_vacuum_database(
    IN  dboid oid,
    OUT dbid oid,
    OUT total_blks_read bigint,
    OUT total_blks_hit bigint,
    OUT total_blks_dirtied bigint,
    OUT total_blks_written bigint,
    OUT wal_records bigint,
    OUT wal_fpi bigint,
    OUT wal_bytes numeric,
    OUT blk_read_time double precision,
    OUT blk_write_time double precision,
    OUT delay_time double precision,
    OUT total_time double precision,
    OUT wraparound_failsafe_count integer,
    OUT interrupts_count integer
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stats_get_vacuum_database'
LANGUAGE C STRICT STABLE;

-- View: vacuum statistics per table (heap)
CREATE VIEW ext_vacuum_statistics.pg_stats_vacuum_tables AS
SELECT
  rel.oid AS relid,
  ns.nspname AS schema,
  rel.relname AS relname,
  db.datname AS dbname,
  stats.total_blks_read,
  stats.total_blks_hit,
  stats.total_blks_dirtied,
  stats.total_blks_written,
  stats.wal_records,
  stats.wal_fpi,
  stats.wal_bytes,
  stats.blk_read_time,
  stats.blk_write_time,
  stats.delay_time,
  stats.total_time,
  stats.wraparound_failsafe_count,
  stats.rel_blks_read,
  stats.rel_blks_hit,
  stats.tuples_deleted,
  stats.pages_scanned,
  stats.pages_removed,
  stats.vm_new_frozen_pages,
  stats.vm_new_visible_pages,
  stats.vm_new_visible_frozen_pages,
  stats.tuples_frozen,
  stats.recently_dead_tuples,
  stats.index_vacuum_count,
  stats.missed_dead_pages,
  stats.missed_dead_tuples
FROM pg_database db,
     pg_class rel,
     pg_namespace ns,
     LATERAL ext_vacuum_statistics.pg_stats_get_vacuum_tables(db.oid, rel.oid) stats
WHERE db.datname = current_database()
  AND rel.relkind = 'r'
  AND rel.relnamespace = ns.oid
  AND rel.oid = stats.relid;

COMMENT ON VIEW ext_vacuum_statistics.pg_stats_vacuum_tables IS
  'Extended vacuum statistics per table (heap)';

-- View: vacuum statistics per index
CREATE VIEW ext_vacuum_statistics.pg_stats_vacuum_indexes AS
SELECT
  rel.oid AS indexrelid,
  ns.nspname AS schema,
  rel.relname AS indexrelname,
  db.datname AS dbname,
  stats.total_blks_read,
  stats.total_blks_hit,
  stats.total_blks_dirtied,
  stats.total_blks_written,
  stats.wal_records,
  stats.wal_fpi,
  stats.wal_bytes,
  stats.blk_read_time,
  stats.blk_write_time,
  stats.delay_time,
  stats.total_time,
  stats.wraparound_failsafe_count,
  stats.rel_blks_read,
  stats.rel_blks_hit,
  stats.tuples_deleted,
  stats.pages_deleted
FROM pg_database db,
     pg_class rel,
     pg_namespace ns,
     LATERAL ext_vacuum_statistics.pg_stats_get_vacuum_indexes(db.oid, rel.oid) stats
WHERE db.datname = current_database()
  AND rel.relkind = 'i'
  AND rel.relnamespace = ns.oid
  AND rel.oid = stats.relid;

COMMENT ON VIEW ext_vacuum_statistics.pg_stats_vacuum_indexes IS
  'Extended vacuum statistics per index';

-- View: vacuum statistics per database (aggregate)
CREATE VIEW ext_vacuum_statistics.pg_stats_vacuum_database AS
SELECT
  db.oid AS dboid,
  db.datname AS dbname,
  stats.total_blks_read AS db_blks_read,
  stats.total_blks_hit AS db_blks_hit,
  stats.total_blks_dirtied AS db_blks_dirtied,
  stats.total_blks_written AS db_blks_written,
  stats.wal_records AS db_wal_records,
  stats.wal_fpi AS db_wal_fpi,
  stats.wal_bytes AS db_wal_bytes,
  stats.blk_read_time AS db_blk_read_time,
  stats.blk_write_time AS db_blk_write_time,
  stats.delay_time AS db_delay_time,
  stats.total_time AS db_total_time,
  stats.wraparound_failsafe_count AS db_wraparound_failsafe_count,
  stats.interrupts_count
FROM pg_database db
LEFT JOIN LATERAL ext_vacuum_statistics.pg_stats_get_vacuum_database(db.oid) stats ON db.oid = stats.dbid;

COMMENT ON VIEW ext_vacuum_statistics.pg_stats_vacuum_database IS
  'Extended vacuum statistics per database (aggregate)';

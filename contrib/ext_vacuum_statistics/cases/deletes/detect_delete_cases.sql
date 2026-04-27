-- detect_delete_cases.sql
--
-- Three additional vacuum-misconfig detectors that build on top of
-- evs_window (created by detect_vacuum_misconfig.sql) and add an
-- index-side window that joins sample_stat_vacuum_indexes -> tables_list
-- so each window carries both heap and index deltas for the same
-- (table, sample-pair).
--
-- Cases:
--   D — freeze postponed       (heap-only signal: tuples_frozen ≈ 0
--                               while live work is happening)
--   E — index_cleanup off      (heap deletes a lot, indexes don't)
--   F — wraparound failsafe    (wraparound_failsafe_count > 0 OR
--                               huge tuples_frozen burst combined with
--                               idx cleanup skipped)
--
-- Re-run this file after detect_vacuum_misconfig.sql.
--
-- Output tables:
--   evs_index_window           — per (table, idx, sample-pair) deltas
--   evs_window_with_indexes    — heap window enriched with summed index
--                                 deltas across all indexes of that table
--   evs_case4_freeze_lag       — case D
--   evs_case5_index_bloat      — case E
--   evs_case6_wraparound       — case F

\set ON_ERROR_STOP on
SET search_path = public, pg_catalog;

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_class WHERE relname = 'evs_window') THEN
        RAISE EXCEPTION 'evs_window is missing; run detect_vacuum_misconfig.sql first';
    END IF;
END
$$;

-- ----------------------------------------------------------------------
-- Per-(table, idx, sample-pair) index window.
-- ----------------------------------------------------------------------
DROP TABLE IF EXISTS evs_index_window;
CREATE UNLOGGED TABLE evs_index_window AS
SELECT  i2.server_id,
        i2.datid,
        il.relid                                       AS relid,
        il.indexrelid                                  AS indexrelid,
        il.schemaname,
        tl.relname                                     AS relname,
        il.indexrelname,
        i1.sample_id                                   AS s_from,
        i2.sample_id                                   AS s_to,
        s1.sample_time                                 AS t_from,
        s2.sample_time                                 AS t_to,
        EXTRACT(epoch FROM s2.sample_time - s1.sample_time) AS dt_sec,
        i2.total_blks_read    - i1.total_blks_read     AS d_blks_read,
        i2.total_blks_hit     - i1.total_blks_hit      AS d_blks_hit,
        i2.total_blks_dirtied - i1.total_blks_dirtied  AS d_blks_dirty,
        i2.total_blks_written - i1.total_blks_written  AS d_blks_written,
        i2.wal_records        - i1.wal_records         AS d_wal_records,
        i2.wal_fpi            - i1.wal_fpi             AS d_wal_fpi,
        i2.wal_bytes          - i1.wal_bytes           AS d_wal_bytes,
        i2.total_time         - i1.total_time          AS d_total,
        i2.delay_time         - i1.delay_time          AS d_delay,
        i2.tuples_deleted     - i1.tuples_deleted      AS d_tuples_deleted,
        i2.pages_deleted      - i1.pages_deleted       AS d_pages_deleted,
        i2.wraparound_failsafe_count
            - i1.wraparound_failsafe_count             AS d_failsafe
FROM    sample_stat_vacuum_indexes i1
JOIN    sample_stat_vacuum_indexes i2
          ON  i2.server_id  = i1.server_id
         AND  i2.datid      = i1.datid
         AND  i2.indexrelid = i1.indexrelid
         AND  i2.sample_id  = i1.sample_id + 1
JOIN    samples s1 ON s1.server_id = i1.server_id AND s1.sample_id = i1.sample_id
JOIN    samples s2 ON s2.server_id = i2.server_id AND s2.sample_id = i2.sample_id
JOIN    indexes_list il
          ON  il.server_id  = i2.server_id
         AND  il.datid      = i2.datid
         AND  il.indexrelid = i2.indexrelid
JOIN    tables_list tl
          ON  tl.server_id = il.server_id
         AND  tl.datid     = il.datid
         AND  tl.relid     = il.relid;

CREATE INDEX ix_evs_index_window_rel
    ON evs_index_window (relname, t_to);

-- ----------------------------------------------------------------------
-- Heap window joined with summed index activity (across all indexes of
-- the same parent table) for the same sample pair.
-- ----------------------------------------------------------------------
DROP TABLE IF EXISTS evs_window_with_indexes;
CREATE UNLOGGED TABLE evs_window_with_indexes AS
WITH idx_agg AS (
    SELECT  server_id, datid, relid, s_from, s_to,
            sum(d_tuples_deleted) AS d_idx_tuples_deleted,
            sum(d_pages_deleted)  AS d_idx_pages_deleted,
            sum(d_blks_dirty)     AS d_idx_blks_dirty,
            sum(d_blks_read)      AS d_idx_blks_read,
            sum(d_blks_hit)       AS d_idx_blks_hit,
            sum(d_wal_records)    AS d_idx_wal_records,
            sum(d_wal_bytes)      AS d_idx_wal_bytes,
            sum(d_total)          AS d_idx_total_time,
            sum(d_failsafe)       AS d_idx_failsafe,
            count(*)              AS n_indexes
    FROM   evs_index_window
    GROUP  BY server_id, datid, relid, s_from, s_to
)
SELECT  w.*,
        COALESCE(ia.d_idx_tuples_deleted, 0)  AS d_idx_tuples_deleted,
        COALESCE(ia.d_idx_pages_deleted,  0)  AS d_idx_pages_deleted,
        COALESCE(ia.d_idx_blks_dirty,     0)  AS d_idx_blks_dirty,
        COALESCE(ia.d_idx_blks_read,      0)  AS d_idx_blks_read,
        COALESCE(ia.d_idx_blks_hit,       0)  AS d_idx_blks_hit,
        COALESCE(ia.d_idx_wal_records,    0)  AS d_idx_wal_records,
        COALESCE(ia.d_idx_wal_bytes,      0)  AS d_idx_wal_bytes,
        COALESCE(ia.d_idx_total_time,     0)  AS d_idx_total_time,
        COALESCE(ia.d_idx_failsafe,       0)  AS d_idx_failsafe,
        COALESCE(ia.n_indexes,            0)  AS n_indexes
FROM    evs_window w
LEFT JOIN idx_agg ia
       ON  ia.server_id = w.server_id
      AND  ia.datid     = w.datid
      AND  ia.relid     = w.relid
      AND  ia.s_from    = w.s_from
      AND  ia.s_to      = w.s_to;

CREATE INDEX ix_evs_window_with_indexes_rel
    ON evs_window_with_indexes (relname, t_to);

-- ----------------------------------------------------------------------
-- Case D: freeze postponed
--
-- Symptom: heap is being actively reclaimed (pages_scanned and either
-- tuples_deleted or pages_removed > 0), but the freeze counters stay
-- pinned at zero.  After the per-table freeze GUCs are RESET, the
-- next vacuum produces a one-shot burst — visible as a single window
-- where vm_new_frozen_pages and tuples_frozen jump together.
-- ----------------------------------------------------------------------
DROP TABLE IF EXISTS evs_case4_freeze_lag;
CREATE UNLOGGED TABLE evs_case4_freeze_lag AS
SELECT  t_from, t_to, schemaname, relname,
        d_pages_scanned, d_tuples_deleted, d_pages_removed,
        d_tuples_frozen, d_vm_frozen, d_vm_visible_frozen,
        d_wal_bytes
FROM    evs_window
WHERE   d_pages_scanned > 1000
  AND   d_tuples_deleted > 5000
  AND   d_tuples_frozen = 0
  AND   d_vm_frozen     = 0
ORDER BY t_to, relname;

-- ----------------------------------------------------------------------
-- Case E: vacuum_index_cleanup = off (per-table) → vacuum scans heap
-- pages but performs no index passes, and as a side effect cannot do
-- the second heap pass that reclaims LP_DEAD line pointers (that pass
-- requires the index entries to be removed first).  Fingerprint:
--   * d_pages_scanned > 5000 (vacuum is doing real heap work)
--   * d_idx_passes = 0 AND d_idx_tuples_deleted = 0 (no index work)
--   * d_tuples_deleted = 0 (heap reclaim blocked because indexes weren't cleaned)
--   * not failsafe (case F catches that path separately)
--
-- KNOWN LIMITATION: this fingerprint is also produced by the natural
-- "index scan bypassed" optimisation when frequent autovacuum on a
-- moving workload sees < 2% of heap pages with dead items per cycle.
-- The healthy FIXED runs in this test suite still trigger this case
-- because autovacuum_naptime=5s with autovacuum_vacuum_threshold=5000
-- never lets dead items accumulate above the bypass threshold.
-- Distinguishing the two needs either a sustained-pattern signal across
-- many windows, or a dedicated counter from the extension that records
-- "skipped indexes because vacuum_index_cleanup=off".
-- ----------------------------------------------------------------------
DROP TABLE IF EXISTS evs_case5_index_bloat;
CREATE UNLOGGED TABLE evs_case5_index_bloat AS
SELECT  t_from, t_to, schemaname, relname,
        n_indexes,
        d_pages_scanned,
        d_tuples_deleted        AS d_table_tuples_deleted,
        d_idx_passes,
        d_idx_tuples_deleted,
        d_idx_pages_deleted,
        d_idx_blks_dirty,
        d_idx_wal_bytes,
        d_failsafe
FROM    evs_window_with_indexes
WHERE   d_pages_scanned > 5000
  AND   n_indexes > 0
  AND   d_idx_passes = 0
  AND   d_idx_tuples_deleted = 0
  AND   d_tuples_deleted = 0
  AND   d_failsafe = 0           -- failsafe also skips idx; case F catches that
ORDER BY t_to, relname;

-- ----------------------------------------------------------------------
-- Case F: wraparound failsafe — fires when the per-table failsafe
-- counter advances.  The behavioural fallback ("freeze-burst-no-idx")
-- has been retired because the explicit settle VACUUM (FREEZE) at the
-- start of every phase can produce a freeze burst with no index work
-- on healthy tables, generating false positives.
-- ----------------------------------------------------------------------
DROP TABLE IF EXISTS evs_case6_wraparound;
CREATE UNLOGGED TABLE evs_case6_wraparound AS
SELECT  t_from, t_to, schemaname, relname,
        d_failsafe,
        d_tuples_frozen,
        d_vm_frozen,
        d_idx_tuples_deleted,
        d_idx_pages_deleted,
        d_idx_blks_dirty,
        'failsafe-counter' AS signal
FROM    evs_window_with_indexes
WHERE   d_failsafe > 0
ORDER BY t_to, relname;

-- ----------------------------------------------------------------------
-- Database-level wraparound: ext_vacuum_statistics also exposes
-- db_wraparound_failsafe_count and interrupts_count via pg_stats_vacuum_database.
-- This is the catch-all — even system catalogs that aren't in tables_list
-- contribute, and the failsafe counter is incremented per database.
-- ----------------------------------------------------------------------
DROP TABLE IF EXISTS evs_case6_wraparound_db;
CREATE UNLOGGED TABLE evs_case6_wraparound_db AS
WITH db_pairs AS (
    SELECT  d2.server_id, d2.datid,
            d1.sample_id AS s_from, d2.sample_id AS s_to,
            s1.sample_time AS t_from, s2.sample_time AS t_to,
            d2.db_wraparound_failsafe_count - d1.db_wraparound_failsafe_count
                                                       AS d_db_failsafe,
            d2.interrupts_count - d1.interrupts_count  AS d_interrupts,
            d2.db_wal_bytes     - d1.db_wal_bytes      AS d_db_wal_bytes,
            d2.db_total_time    - d1.db_total_time     AS d_db_total_time
    FROM    sample_stat_vacuum_database d1
    JOIN    sample_stat_vacuum_database d2
              ON  d2.server_id = d1.server_id
             AND  d2.datid     = d1.datid
             AND  d2.sample_id = d1.sample_id + 1
    JOIN    samples s1 ON s1.server_id = d1.server_id AND s1.sample_id = d1.sample_id
    JOIN    samples s2 ON s2.server_id = d2.server_id AND s2.sample_id = d2.sample_id
)
SELECT  p.t_from, p.t_to, sd.datname,
        p.d_db_failsafe,
        p.d_interrupts,
        p.d_db_wal_bytes,
        p.d_db_total_time
FROM    db_pairs p
LEFT JOIN sample_stat_database sd
       ON  sd.server_id = p.server_id
      AND  sd.sample_id = p.s_to
      AND  sd.datid     = p.datid
WHERE   p.d_db_failsafe > 0
   OR   p.d_interrupts  > 0
ORDER BY p.t_to, sd.datname;

-- Convenience output for interactive runs.
SELECT 'case4_freeze_lag'      AS case, count(*) FROM evs_case4_freeze_lag
UNION ALL
SELECT 'case5_index_bloat',         count(*) FROM evs_case5_index_bloat
UNION ALL
SELECT 'case6_wraparound',          count(*) FROM evs_case6_wraparound
UNION ALL
SELECT 'case6_wraparound_db',       count(*) FROM evs_case6_wraparound_db;

-- detect_vacuum_misconfig.sql
--
-- Detection queries built strictly on the per-table counters that
-- ext_vacuum_statistics publishes (and that pg_profile snapshots in
-- sample_stat_vacuum_tables).  Nothing here joins sample_stat_tables —
-- everything we use comes from the extension.
--
-- Counters from ext_vacuum_statistics that we leverage:
--   buffers   — total_blks_read / total_blks_hit / total_blks_dirtied / total_blks_written
--   WAL       — wal_records / wal_fpi / wal_bytes
--   changes   — tuples_deleted / pages_scanned / pages_removed / recently_dead_tuples
--   freezing  — tuples_frozen / vm_new_frozen_pages / vm_new_visible_pages /
--               vm_new_visible_frozen_pages
--   timings   — total_time / delay_time / blk_read_time / blk_write_time
--   retries   — index_vacuum_count / wraparound_failsafe_count /
--               missed_dead_pages / missed_dead_tuples
--
-- Output tables:
--   evs_window               — full per-(server, table, sample_pair) deltas
--   evs_workload_profile     — long-form per-relation workload metrics
--   evs_case1_throttled      — vacuum sleeping > half its wall-clock
--   evs_case2_mwm_small      — index passes inflated → maintenance_work_mem too small
--   evs_case3_vac_thrash     — vacuum scans far more pages than it cleans
--                              (autovacuum_vacuum_scale_factor / threshold too low)

\set ON_ERROR_STOP on
SET search_path = public, pg_catalog;

DROP TABLE IF EXISTS evs_window;
CREATE UNLOGGED TABLE evs_window AS
WITH pairs AS (
    SELECT  v2.server_id,
            v2.datid,
            v2.relid,
            v1.sample_id          AS s_from,
            v2.sample_id          AS s_to,
            s1.sample_time        AS t_from,
            s2.sample_time        AS t_to,
            EXTRACT(epoch FROM s2.sample_time - s1.sample_time)
                                  AS dt_sec,
            tl.relname,
            tl.schemaname,
            -- ── timings (ms) ────────────────────────────────────────
            v2.total_time          - v1.total_time          AS d_total,
            v2.delay_time          - v1.delay_time          AS d_delay,
            v2.blk_read_time       - v1.blk_read_time       AS d_blk_read_t,
            v2.blk_write_time      - v1.blk_write_time      AS d_blk_write_t,
            -- ── buffers (blocks) ────────────────────────────────────
            v2.total_blks_read     - v1.total_blks_read     AS d_blks_read,
            v2.total_blks_hit      - v1.total_blks_hit      AS d_blks_hit,
            v2.total_blks_dirtied  - v1.total_blks_dirtied  AS d_blks_dirty,
            v2.total_blks_written  - v1.total_blks_written  AS d_blks_written,
            -- ── WAL (records / FPI / bytes) ─────────────────────────
            v2.wal_records         - v1.wal_records         AS d_wal_records,
            v2.wal_fpi             - v1.wal_fpi             AS d_wal_fpi,
            v2.wal_bytes           - v1.wal_bytes           AS d_wal_bytes,
            -- ── changes & cleanup ──────────────────────────────────
            v2.tuples_deleted      - v1.tuples_deleted      AS d_tuples_deleted,
            v2.pages_scanned       - v1.pages_scanned       AS d_pages_scanned,
            v2.pages_removed       - v1.pages_removed       AS d_pages_removed,
            v2.recently_dead_tuples - v1.recently_dead_tuples
                                                            AS d_recent_dead,
            -- ── freezing & VM transitions ──────────────────────────
            v2.tuples_frozen       - v1.tuples_frozen       AS d_tuples_frozen,
            v2.vm_new_visible_pages - v1.vm_new_visible_pages
                                                            AS d_vm_visible,
            v2.vm_new_frozen_pages - v1.vm_new_frozen_pages AS d_vm_frozen,
            v2.vm_new_visible_frozen_pages
                - v1.vm_new_visible_frozen_pages            AS d_vm_visible_frozen,
            -- ── retries / interrupts ───────────────────────────────
            v2.index_vacuum_count  - v1.index_vacuum_count  AS d_idx_passes,
            v2.wraparound_failsafe_count
                - v1.wraparound_failsafe_count              AS d_failsafe,
            v2.missed_dead_pages   - v1.missed_dead_pages   AS d_missed_pages,
            v2.missed_dead_tuples  - v1.missed_dead_tuples  AS d_missed_tuples
    FROM   sample_stat_vacuum_tables v1
    JOIN   sample_stat_vacuum_tables v2
              ON  v2.server_id = v1.server_id
             AND  v2.datid     = v1.datid
             AND  v2.relid     = v1.relid
             AND  v2.sample_id = v1.sample_id + 1
    JOIN   samples s1 ON s1.server_id = v1.server_id AND s1.sample_id = v1.sample_id
    JOIN   samples s2 ON s2.server_id = v2.server_id AND s2.sample_id = v2.sample_id
    JOIN   tables_list tl
              ON  tl.server_id = v2.server_id
             AND  tl.datid     = v2.datid
             AND  tl.relid     = v2.relid
)
SELECT * FROM pairs;

CREATE INDEX ix_evs_window_relname ON evs_window (relname, t_to);

-- ----------------------------------------------------------------------
-- Workload-profile in a long, easy-to-pivot form.  This is the answer
-- to "describe how the table is being changed".
--   * buffers — read / hit / dirtied / written / read_time / write_time
--   * wal     — records / fpi / bytes
--   * changes — tuples_deleted / pages_scanned / pages_removed /
--               recently_dead
--   * vm      — vm_new_visible_pages / vm_new_frozen_pages /
--               vm_new_visible_frozen_pages / tuples_frozen
-- ----------------------------------------------------------------------
DROP TABLE IF EXISTS evs_workload_profile;
CREATE UNLOGGED TABLE evs_workload_profile AS
SELECT  schemaname, relname, t_from, t_to, dt_sec,
        -- buffers per second
        d_blks_read     / NULLIF(dt_sec, 0)   AS blks_read_per_sec,
        d_blks_hit      / NULLIF(dt_sec, 0)   AS blks_hit_per_sec,
        d_blks_dirty    / NULLIF(dt_sec, 0)   AS blks_dirty_per_sec,
        d_blks_written  / NULLIF(dt_sec, 0)   AS blks_written_per_sec,
        -- WAL per second
        d_wal_records   / NULLIF(dt_sec, 0)   AS wal_records_per_sec,
        d_wal_fpi       / NULLIF(dt_sec, 0)   AS wal_fpi_per_sec,
        d_wal_bytes     / NULLIF(dt_sec, 0)   AS wal_bytes_per_sec,
        -- changes & dead tuples
        d_tuples_deleted   / NULLIF(dt_sec, 0)  AS tuples_deleted_per_sec,
        d_pages_scanned    / NULLIF(dt_sec, 0)  AS pages_scanned_per_sec,
        d_pages_removed    / NULLIF(dt_sec, 0)  AS pages_removed_per_sec,
        d_recent_dead      / NULLIF(dt_sec, 0)  AS recent_dead_per_sec,
        -- freezing / VM transitions
        d_tuples_frozen    / NULLIF(dt_sec, 0)  AS tuples_frozen_per_sec,
        d_vm_visible       / NULLIF(dt_sec, 0)  AS vm_visible_per_sec,
        d_vm_frozen        / NULLIF(dt_sec, 0)  AS vm_frozen_per_sec,
        d_vm_visible_frozen / NULLIF(dt_sec, 0) AS vm_visible_frozen_per_sec,
        -- "implied unfreeze" load: pages dirtied that were not refrozen
        -- in the same window.  Positive number means vacuum lost more
        -- frozen-pages worth of work than it produced.
        GREATEST(d_blks_dirty - d_vm_frozen, 0)::numeric / NULLIF(dt_sec, 0)
                                              AS unfrozen_pressure_per_sec
FROM    evs_window
ORDER BY t_to, relname;

CREATE INDEX ix_evs_workload_profile_rel
    ON evs_workload_profile (relname, t_to);

-- ----------------------------------------------------------------------
-- Case 1: vacuum is throttled by cost-delay.
-- All inputs come from ext_vacuum_statistics.delay_time / .total_time.
-- We require a meaningful amount of vacuum activity in the window
-- (d_total > 1 second of millis) so a single sleep at the tail does
-- not get flagged.
-- ----------------------------------------------------------------------
DROP TABLE IF EXISTS evs_case1_throttled;
CREATE UNLOGGED TABLE evs_case1_throttled AS
SELECT  t_from, t_to, schemaname, relname,
        d_total, d_delay,
        ROUND( (d_delay / NULLIF(d_total, 0))::numeric, 3) AS delay_share,
        d_tuples_deleted, d_pages_scanned, d_blks_dirty,
        d_wal_bytes
FROM    evs_window
WHERE   d_total > 1000             -- 1+ second of vacuum work in the window
  AND   d_delay / NULLIF(d_total, 0) > 0.5
ORDER BY t_to, relname;

-- ----------------------------------------------------------------------
-- Case 2: maintenance_work_mem too small.
-- index_vacuum_count climbs much faster than expected: with a healthy
-- m_w_m every vacuum scans the indexes 1× and removes thousands of TIDs
-- per pass.  When TID storage overflows, the same vacuum has to make
-- many passes, each cleaning ~few-thousand TIDs, so:
--     pages_scanned / index_vacuum_count   becomes very small AND
--     tuples_deleted / index_vacuum_count  is bounded by m_w_m / TID-size
-- Threshold "<500 pages scanned per index pass" combined with
-- "index_vacuum_count grew by > 5 in this window" reliably flags the
-- broken configuration on small pgbench tables; raise both thresholds
-- for production-sized relations.
-- ----------------------------------------------------------------------
DROP TABLE IF EXISTS evs_case2_mwm_small;
CREATE UNLOGGED TABLE evs_case2_mwm_small AS
SELECT  t_from, t_to, schemaname, relname,
        d_idx_passes,
        d_pages_scanned,
        d_tuples_deleted,
        ROUND((d_pages_scanned::numeric  / NULLIF(d_idx_passes, 0)), 1)
                                        AS pages_per_pass,
        ROUND((d_tuples_deleted::numeric / NULLIF(d_idx_passes, 0)), 0)
                                        AS tuples_per_pass,
        d_wal_records, d_wal_bytes
FROM    evs_window
WHERE   d_idx_passes > 5
  AND   d_pages_scanned::numeric / d_idx_passes < 500
ORDER BY t_to, relname;

-- ----------------------------------------------------------------------
-- Case 3: vacuum thrashing — autovacuum_vacuum_scale_factor / threshold
-- so low that vacuum keeps re-scanning the whole table for tiny dead-tuple
-- harvests.  Pure ext_vacuum_statistics signal:
--      pages_scanned > 1000  AND  pages_scanned / tuples_deleted > 5
-- (vacuum reads >5 pages for every tuple it cleans).  Distinct from
-- case 1 because here delay_share is low — vacuum is just busy producing
-- I/O for nothing.
-- ----------------------------------------------------------------------
DROP TABLE IF EXISTS evs_case3_vac_thrash;
CREATE UNLOGGED TABLE evs_case3_vac_thrash AS
SELECT  t_from, t_to, schemaname, relname,
        d_pages_scanned, d_tuples_deleted, d_pages_removed,
        d_blks_read, d_blks_dirty, d_wal_bytes,
        ROUND((d_pages_scanned::numeric /
               GREATEST(d_tuples_deleted, 1))::numeric, 2)
                                AS pages_per_dead_tuple,
        ROUND((d_blks_read::numeric  /
               GREATEST(d_tuples_deleted, 1))::numeric, 2)
                                AS blks_read_per_dead_tuple,
        d_failsafe
FROM    evs_window
WHERE   d_pages_scanned > 1000
  AND   d_pages_scanned::numeric / GREATEST(d_tuples_deleted, 1) > 5
ORDER BY t_to, relname;

-- Convenience output for interactive runs.
SELECT 'case1_throttled' AS case, count(*) FROM evs_case1_throttled
UNION ALL
SELECT 'case2_mwm_small',  count(*) FROM evs_case2_mwm_small
UNION ALL
SELECT 'case3_vac_thrash', count(*) FROM evs_case3_vac_thrash;

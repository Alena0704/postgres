-- aggregate_patterns.sql
--
-- Tag every (sample_id_from -> sample_id_to) window in evs_window
-- with the update pattern that was active during that window.  The
-- driver script (run_update_patterns.sh) populates evs_pattern_phases
-- with one row per pattern execution.
--
-- Output:
--   evs_window_labeled    — every window joined with its pattern label
--                           (NULL for windows outside any phase)
--   evs_pattern_summary   — per-pattern aggregates of every counter
--                           that ext_vacuum_statistics exposes per table.
--
-- All numbers come from sample_stat_vacuum_tables (i.e. ext_vacuum_statistics
-- via pg_profile snapshots).  No join with sample_stat_tables.

\set ON_ERROR_STOP on
\i :detect_path

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_class WHERE relname = 'evs_pattern_phases') THEN
        RAISE EXCEPTION 'evs_pattern_phases is missing; '
                        'did run_update_patterns.sh write any rows?';
    END IF;
END
$$;

DROP TABLE IF EXISTS evs_window_labeled;
CREATE UNLOGGED TABLE evs_window_labeled AS
SELECT  w.*,
        p.pattern AS pattern
FROM    evs_window w
LEFT JOIN evs_pattern_phases p
       ON  w.t_to    >  p.started_at
       AND w.t_from  <  p.ended_at;

CREATE INDEX ix_evs_window_labeled_pattern
   ON evs_window_labeled (pattern, t_to);

DROP TABLE IF EXISTS evs_pattern_summary;
CREATE UNLOGGED TABLE evs_pattern_summary AS
SELECT  pattern, relname,
        count(*)                        AS windows,
        sum(dt_sec)                     AS phase_seconds,
        -- timings
        sum(d_total)                    AS total_time_ms,
        sum(d_delay)                    AS delay_time_ms,
        -- buffers
        sum(d_blks_read)                AS blks_read,
        sum(d_blks_hit)                 AS blks_hit,
        sum(d_blks_dirty)               AS blks_dirty,
        sum(d_blks_written)             AS blks_written,
        -- wal
        sum(d_wal_records)              AS wal_records,
        sum(d_wal_fpi)                  AS wal_fpi,
        sum(d_wal_bytes)                AS wal_bytes,
        -- changes
        sum(d_tuples_deleted)           AS tuples_deleted,
        sum(d_pages_scanned)            AS pages_scanned,
        sum(d_pages_removed)            AS pages_removed,
        sum(d_recent_dead)              AS recent_dead,
        -- freezing & VM
        sum(d_tuples_frozen)            AS tuples_frozen,
        sum(d_vm_visible)               AS vm_visible,
        sum(d_vm_frozen)                AS vm_frozen,
        sum(d_vm_visible_frozen)        AS vm_visible_frozen,
        -- retries / interrupts
        sum(d_idx_passes)               AS index_passes,
        sum(d_failsafe)                 AS failsafe,
        sum(d_missed_pages)             AS missed_pages,
        sum(d_missed_tuples)            AS missed_tuples
FROM    evs_window_labeled
WHERE   pattern IS NOT NULL
GROUP BY pattern, relname
ORDER BY pattern, relname;

SELECT 'phases'   AS what, count(*) FROM evs_pattern_phases
UNION ALL
SELECT 'windows', count(*) FROM evs_window_labeled
                            WHERE pattern IS NOT NULL
UNION ALL
SELECT 'summary', count(*) FROM evs_pattern_summary;

-- pattern: middle (hotspot)
-- All updates fall into a narrow ~10k-row window in the middle of
-- pgbench_accounts.  Dead tuples concentrate on a handful of heap
-- pages → vacuum scans a small range, rel_blks_dirtied stays low,
-- but the same pages get re-dirtied many times (HOT pruning matters).
\set max     100000 * :scale
\set centre  :max / 2
\set lo      :centre - 5000
\set hi      :centre + 5000
\set aid     random(:lo, :hi)
UPDATE pgbench_accounts SET abalance = abalance + 1 WHERE aid = :aid;

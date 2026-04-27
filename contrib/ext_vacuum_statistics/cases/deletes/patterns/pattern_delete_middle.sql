-- pattern: middle (hotspot) DELETE
--
-- Every transaction deletes a row from a narrow ~10k window around the
-- middle of pgbench_accounts.  The same heap pages get re-targeted over
-- and over: dead tuples concentrate, but `pages_removed` stays tiny
-- because a single page rarely empties.  An INSERT at the tail keeps
-- the table a constant size, so the bloat lands on the index instead.
--
-- What we expect in ext_vacuum_statistics:
--   pages_scanned LOW           — narrow vacuum range
--   tuples_deleted HIGH
--   pages_removed ≈ 0           — pages stay partly populated
--   index tuples_deleted HIGH   — many index entries cleared
--   index pages_deleted LOW     — leaf pages don't empty out
--   recently_dead_tuples grows  — under MVCC short snapshots
\set max     100000 * :scale
\set centre  :max / 2
\set lo      :centre - 5000
\set hi      :centre + 5000
\set aid     random(:lo, :hi)
DELETE FROM pgbench_accounts WHERE aid = :aid;
INSERT INTO pgbench_accounts (aid, bid, abalance, filler)
     VALUES (nextval('evs_delete_seq'), 1, 0, '');

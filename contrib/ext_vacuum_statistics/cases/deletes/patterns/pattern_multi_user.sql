-- pattern_multi_user.sql — multi-table workload for the vacuum-misconfig
-- investigation deck.
--
-- The standard pgbench TPC-B script UPDATEs pgbench_branches (50 rows
-- with scale=50), and 16 clients beat that hot row to ~3 TPS on a debug
-- build of postgres.  This pattern keeps the multi-table feel — random
-- UPDATEs on pgbench_accounts (5M rows, almost no collision), occasional
-- UPDATEs on pgbench_tellers (500 rows, low collision), and INSERTs into
-- pgbench_history — but drops the pgbench_branches UPDATE entirely.
--
-- Result: each client makes thousands of transactions per phase, so:
--   * autovacuum on pgbench_accounts has real dead-tuple backlog
--   * pgbench_history grows steadily (insert-only → no dead tuples but
--     age-driven freezing matters)
--   * pgbench_tellers gets a moderate update rate
--
-- Used by multi_table_sim.sh.

\set aid    random(1, 100000 * :scale)
\set tid    random(1, 10 * :scale)
\set delta  random(-5000, 5000)

BEGIN;
UPDATE pgbench_accounts
   SET abalance = abalance + :delta
 WHERE aid = :aid;
UPDATE pgbench_tellers
   SET tbalance = tbalance + :delta
 WHERE tid = :tid;
INSERT INTO pgbench_history (tid, bid, aid, delta, mtime)
     VALUES (:tid, 1, :aid, :delta, CURRENT_TIMESTAMP);
END;

-- pattern: random
-- Baseline. UPDATE one random row anywhere in pgbench_accounts.
-- Dead tuples are spread uniformly across the heap, vacuum has to
-- scan most of the relation to clean up.
\set max  100000 * :scale
\set aid  random(1, :max)
UPDATE pgbench_accounts SET abalance = abalance + 1 WHERE aid = :aid;

-- pattern: page boundaries
-- pgbench_accounts row is ~100 bytes -> ~80 tuples per 8 KiB page.
-- Stepping aid by 80 hits roughly one row per page, so each update
-- creates a dead tuple on a different heap page.  Result: vacuum has
-- to dirty many pages but each page sheds only a single tuple.
-- vm_new_visible_pages should jump because most pages get
-- all-visible after cleanup.
\set max_chunks 1250 * :scale
\set chunk random(0, :max_chunks - 1)
\set aid (:chunk * 80) + 1
UPDATE pgbench_accounts SET abalance = abalance + 1 WHERE aid = :aid;

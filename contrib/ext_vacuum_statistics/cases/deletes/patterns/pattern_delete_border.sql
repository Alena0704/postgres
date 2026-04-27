-- pattern: border DELETE
--
-- Each transaction deletes one row from the bottom 5% or top 5% of
-- the pgbench_accounts aid range, then inserts a new row at the
-- monotonically growing tail (evs_delete_seq).  The combined stream
-- mimics rotational data: old rows die at the edges, new rows arrive
-- at the high end of the heap and B-tree.
--
-- What we expect to see in ext_vacuum_statistics:
--   pages_removed > 0      — entire heap pages on the left edge empty out
--                             and become candidates for relation truncate
--   tuples_deleted high
--   index pages_deleted > 0 — leaf pages on the left edge of the btree
--                             empty out and are recyclable
--   wal_fpi spikes on first dirtied page after each checkpoint
\set max         100000 * :scale
\set border_pct  5
\set width       :max * :border_pct / 100
\set side        random(0, 1)
\set base        random(1, :width)
\set aid         :base + :side * (:max - :width)
DELETE FROM pgbench_accounts WHERE aid = :aid;
INSERT INTO pgbench_accounts (aid, bid, abalance, filler)
     VALUES (nextval('evs_delete_seq'), 1, 0, '');

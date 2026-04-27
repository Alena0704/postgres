-- pattern: sparse / "дырявчатый" DELETE
--
-- pgbench_accounts row ≈ 100 bytes → ~80 live tuples per 8 KiB heap
-- page.  Stepping aid by 80 hits roughly one tuple per heap page, which
-- is the worst-case dead-tuple distribution for VACUUM:
--   * almost every page in the relation has to be visited
--   * almost no page actually empties (one dead tuple is not enough)
--   * the visibility map flips back to "not all-visible" all over the
--     table — recovery later reads them again as not-frozen
--
-- For indexes the cost is also at its peak: each leaf page holds
-- entries from many heap pages; deleting one tuple per heap page
-- touches almost every leaf.
--
-- What we expect in ext_vacuum_statistics:
--   pages_scanned ≫ pages_removed
--   vm_new_visible_pages high               — most pages turn all-visible
--                                             again only after vacuum
--   wal_fpi very high                       — first touch after checkpoint
--                                             on many pages
--   index total_blks_dirtied high
--   index wal_bytes high
--   tuples_frozen elevated on the second pass through the table
\set max         100000 * :scale
\set max_chunks  :max / 80
\set chunk       random(0, :max_chunks - 1)
\set aid         (:chunk * 80) + 1
DELETE FROM pgbench_accounts WHERE aid = :aid;
INSERT INTO pgbench_accounts (aid, bid, abalance, filler)
     VALUES (nextval('evs_delete_seq'), 1, 0, '');

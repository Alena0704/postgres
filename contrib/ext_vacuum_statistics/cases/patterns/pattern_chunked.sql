-- pattern: chunked / rolling sweep
-- Each transaction picks the next chunk_id from a server-side
-- sequence (CYCLE) and rewrites a contiguous 100-row block.
-- The driver pre-creates `evs_chunk_seq` sized to floor(max_aid/100).
-- Effect: dirty pages move across the heap as a wave, so successive
-- vacuums have very different pages_scanned ranges — perfect for
-- watching vm_new_frozen_pages climb monotonically.
WITH c AS (SELECT nextval('evs_chunk_seq') AS cid)
UPDATE pgbench_accounts SET abalance = abalance + 1
  FROM c
 WHERE aid BETWEEN ((c.cid - 1) * 100 + 1) AND (c.cid * 100);

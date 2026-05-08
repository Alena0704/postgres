--
-- APPEND OR PATH (enable_mdam)
--
-- Test Append/MergeAppend index scan paths for OR conditions.
-- The optimizer builds Append(IndexScan, IndexScan) or
-- MergeAppend(IndexScan, IndexScan) as an alternative to BitmapOr.
--

CREATE TABLE append_or_tab (
    a int,
    b int,
    c text
);

CREATE INDEX append_or_tab_a_idx ON append_or_tab (a);
CREATE INDEX append_or_tab_b_idx ON append_or_tab (b);
CREATE INDEX append_or_tab_ab_idx ON append_or_tab (a, b);

INSERT INTO append_or_tab
SELECT i % 1000, i % 500, 'row_' || i
FROM generate_series(1, 50000) AS i;

ANALYZE append_or_tab;

SET enable_bitmapscan = off;

--
-- Basic OR with range conditions (not collapsible to ScalarArrayOp)
-- Should use Append(IndexScan, IndexScan)
--
EXPLAIN (COSTS OFF)
SELECT * FROM append_or_tab WHERE a < 3 OR (a > 100 AND a < 105);

-- Verify correctness
SELECT count(*) FROM append_or_tab WHERE a < 3 OR (a > 100 AND a < 105);

--
-- OR + ORDER BY => MergeAppend or sorted Append
--
EXPLAIN (COSTS OFF)
SELECT * FROM append_or_tab WHERE a < 3 OR (a > 100 AND a < 105) ORDER BY a;

-- Verify ordering
SELECT a FROM append_or_tab WHERE a < 3 OR (a > 100 AND a < 105) ORDER BY a LIMIT 10;

--
-- Backward scan: ORDER BY DESC
--
EXPLAIN (COSTS OFF)
SELECT * FROM append_or_tab WHERE a < 3 OR (a > 100 AND a < 105) ORDER BY a DESC;

SELECT a FROM append_or_tab WHERE a < 3 OR (a > 100 AND a < 105) ORDER BY a DESC LIMIT 10;

--
-- Three-arm OR with ranges
--
EXPLAIN (COSTS OFF)
SELECT * FROM append_or_tab WHERE a < 3 OR (a > 100 AND a < 103) OR a > 997;

EXPLAIN (COSTS OFF)
SELECT * FROM append_or_tab WHERE a < 3 OR (a > 100 AND a < 103) OR a > 997 ORDER BY a;

--
-- OR + LIMIT benefits from sorted path (avoids scanning all arms)
--
EXPLAIN (COSTS OFF)
SELECT * FROM append_or_tab WHERE a < 5 OR a > 995 ORDER BY a LIMIT 10;

SELECT a FROM append_or_tab WHERE a < 5 OR a > 995 ORDER BY a LIMIT 10;

--
-- OR combined with AND filter on composite index
--
EXPLAIN (COSTS OFF)
SELECT * FROM append_or_tab WHERE (a < 5 OR a > 995) AND b < 10;

EXPLAIN (COSTS OFF)
SELECT * FROM append_or_tab WHERE (a < 5 OR a > 995) AND b < 10 ORDER BY a;

SELECT a, b FROM append_or_tab WHERE (a < 5 OR a > 995) AND b < 10 ORDER BY a, b LIMIT 10;

--
-- GUC test: enable_mdam = off disables Append-OR paths
--
SET enable_mdam = off;
EXPLAIN (COSTS OFF)
SELECT * FROM append_or_tab WHERE a < 3 OR (a > 100 AND a < 105);
SET enable_mdam = on;

--
-- OR on different indexed columns - NOT suitable for Append-OR
-- (each arm uses a different index)
--
EXPLAIN (COSTS OFF)
SELECT * FROM append_or_tab WHERE a < 3 OR b > 497;

RESET enable_bitmapscan;

-- Clean up
DROP TABLE append_or_tab;

--
-- MDAM (Multi-Dimensional Access Method) OR-clause optimization tests
--
-- Tests the transformation of complex OR predicates on multi-column B-tree
-- indexes into non-overlapping index scan retrievals combined via Append.
--

-- Setup: create test table with multi-column index
DROP TABLE IF EXISTS mdam_test_tbl;
CREATE TABLE mdam_test_tbl (
    a int,
    b int,
    c int,
    d int
);

-- Insert enough rows for the optimizer to prefer index scans
INSERT INTO mdam_test_tbl
SELECT i / 1000, i % 100, i % 50, i % 25
FROM generate_series(1, 100000) i;

CREATE INDEX mdam_test_tbl_idx ON mdam_test_tbl (a, b, c, d);
ANALYZE mdam_test_tbl;

-- Disable seqscan and parallelism so we can see index plan choices clearly
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET max_parallel_workers_per_gather = 0;

--
-- Basic: OR on leading column with different EQ values (non-overlapping)
-- Each arm constrains different columns of the same index.
-- MDAM should produce Append of IndexOnlyScans.
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE (a = 5 AND c = 10) OR (a = 10 AND b = 20);

-- Verify correctness: compare MDAM result with seqscan
SELECT a, b, c, d FROM mdam_test_tbl
WHERE (a = 5 AND c = 10) OR (a = 10 AND b = 20)
ORDER BY a, b, c, d;

--
-- Overlapping ranges on same column should merge into single tight scan.
-- BETWEEN 4 AND 7 OR BETWEEN 5 AND 11 => single scan with >= 4 AND <= 11
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE a BETWEEN 4 AND 7 OR a BETWEEN 5 AND 11;

-- Verify correctness
SET enable_mdam = off;
SET enable_bitmapscan = on;
SELECT count(*) AS expect FROM mdam_test_tbl
WHERE a BETWEEN 4 AND 7 OR a BETWEEN 5 AND 11;
SET enable_mdam = on;
SET enable_bitmapscan = off;
SELECT count(*) AS actual FROM mdam_test_tbl
WHERE a BETWEEN 4 AND 7 OR a BETWEEN 5 AND 11;

--
-- Non-overlapping ranges: should produce Append of two scans
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE a BETWEEN 4 AND 7 OR a BETWEEN 9 AND 11;

--
-- Mixed depth: one arm has 1 column, other has 2 columns.
-- Tests that expansion correctly preserves all column constraints.
-- The second arm must keep BOTH a>=9 AND a<=11 (not just a>=9).
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE (a BETWEEN 4 AND 7) OR (a BETWEEN 9 AND 11 AND b = 1);

-- Verify correctness
SET enable_mdam = off;
SET enable_bitmapscan = on;
SELECT count(*) AS expect FROM mdam_test_tbl
WHERE (a BETWEEN 4 AND 7) OR (a BETWEEN 9 AND 11 AND b = 1);
SET enable_mdam = on;
SET enable_bitmapscan = off;
SELECT count(*) AS actual FROM mdam_test_tbl
WHERE (a BETWEEN 4 AND 7) OR (a BETWEEN 9 AND 11 AND b = 1);

--
-- Three-arm OR: all on same index, non-overlapping leading column
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE (a = 5 AND b = 10 AND c = 3)
   OR (a = 5 AND b = 20 AND c = 7)
   OR (a = 10 AND c = 5);

-- Verify correctness
SET enable_mdam = off;
SET enable_bitmapscan = on;
SELECT count(*) AS expect FROM mdam_test_tbl
WHERE (a = 5 AND b = 10 AND c = 3)
   OR (a = 5 AND b = 20 AND c = 7)
   OR (a = 10 AND c = 5);
SET enable_mdam = on;
SET enable_bitmapscan = off;
SELECT count(*) AS actual FROM mdam_test_tbl
WHERE (a = 5 AND b = 10 AND c = 3)
   OR (a = 5 AND b = 20 AND c = 7)
   OR (a = 10 AND c = 5);

--
-- Contradiction detection: OR arm that simplifies to empty
-- a IN (10,20) AND a > 25 => contradiction, so only one arm survives
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE (a IN (10, 20) AND a > 25) OR (a = 5 AND b = 3);

--
-- GUC test: enable_mdam = off should fall back to BitmapOr
--
SET enable_mdam = off;
SET enable_bitmapscan = on;
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE (a = 5 AND c = 10) OR (a = 10 AND b = 20);
SET enable_mdam = on;
SET enable_bitmapscan = off;

--
-- Redundant OR: one arm is subset of the other.
-- a BETWEEN 3 AND 10 fully contains a = 5, so should merge to single scan.
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE a BETWEEN 3 AND 10 OR a = 5;

--
-- IN list in OR arm: tests SAOP handling during DNF extraction
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE (a = 5 AND b IN (1, 2, 3)) OR (a = 10 AND b = 50);

-- Verify correctness
SET enable_mdam = off;
SET enable_bitmapscan = on;
SELECT count(*) AS expect FROM mdam_test_tbl
WHERE (a = 5 AND b IN (1, 2, 3)) OR (a = 10 AND b = 50);
SET enable_mdam = on;
SET enable_bitmapscan = off;
SELECT count(*) AS actual FROM mdam_test_tbl
WHERE (a = 5 AND b IN (1, 2, 3)) OR (a = 10 AND b = 50);

--
-- Multiple ANDed ORs on separate columns should collapse to one scan with IN lists.
-- (a=1 OR a=2) AND (b=10 OR b=20) AND (c=3 OR c=4) => single scan
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE (a = 1 OR a = 2) AND (b = 10 OR b = 20) AND (c = 3 OR c = 4);

-- Verify correctness
SET enable_mdam = off;
SET enable_bitmapscan = on;
SELECT count(*) AS expect FROM mdam_test_tbl
WHERE (a = 1 OR a = 2) AND (b = 10 OR b = 20) AND (c = 3 OR c = 4);
SET enable_mdam = on;
SET enable_bitmapscan = off;
SELECT count(*) AS actual FROM mdam_test_tbl
WHERE (a = 1 OR a = 2) AND (b = 10 OR b = 20) AND (c = 3 OR c = 4);

--
-- Complementary ranges should merge to unconstrained column (IS_ANYTHING).
-- (a=10 AND c>5) OR (a=10 AND c<=5) => just a=10
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE (a = 10 AND c > 5) OR (a = 10 AND c <= 5);

-- Verify correctness
SET enable_mdam = off;
SET enable_bitmapscan = on;
SELECT count(*) AS expect FROM mdam_test_tbl
WHERE (a = 10 AND c > 5) OR (a = 10 AND c <= 5);
SET enable_mdam = on;
SET enable_bitmapscan = off;
SELECT count(*) AS actual FROM mdam_test_tbl
WHERE (a = 10 AND c > 5) OR (a = 10 AND c <= 5);

--
-- EQ-to-IN folding: multiple arms sharing same prefix, differing on trailing col.
-- Should fold the trailing EQ values into an IN list.
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE (a = 5 AND b = 10 AND c = 1 AND d = 0)
   OR (a = 5 AND b = 10 AND c = 1 AND d = 5)
   OR (a = 5 AND b = 10 AND c = 2 AND d = 0)
   OR (a = 5 AND b = 10 AND c = 2 AND d = 5)
   OR (a = 10 AND b = 20 AND c = 3 AND d = 0);

--
-- IN expansion then coalesce roundtrip: leading IN expands then merges back.
-- a IN (1,2) AND b IN (10,20) AND c=3 => should stay as 1 retrieval
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE a IN (1, 2) AND b IN (10, 20) AND c = 3 AND d > 10;

-- Verify correctness
SET enable_mdam = off;
SET enable_bitmapscan = on;
SELECT count(*) AS expect FROM mdam_test_tbl
WHERE a IN (1, 2) AND b IN (10, 20) AND c = 3 AND d > 10;
SET enable_mdam = on;
SET enable_bitmapscan = off;
SELECT count(*) AS actual FROM mdam_test_tbl
WHERE a IN (1, 2) AND b IN (10, 20) AND c = 3 AND d > 10;

--
-- Ordering conflict: non-point constraint on leading col + different later cols.
-- (a > 10 AND b = 5) OR (a > 10 AND c = 3) -- MDAM should reject (fall back).
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE (a > 10 AND b = 5) OR (a > 10 AND c = 3);

--
-- No ordering conflict: same structure but leading col is point constraint.
-- (a = 10 AND b = 5) OR (a = 10 AND c = 3) -- MDAM can handle this.
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE (a = 10 AND b = 5) OR (a = 10 AND c = 3);

-- Verify correctness
SET enable_mdam = off;
SET enable_bitmapscan = on;
SELECT count(*) AS expect FROM mdam_test_tbl
WHERE (a = 10 AND b = 5) OR (a = 10 AND c = 3);
SET enable_mdam = on;
SET enable_bitmapscan = off;
SELECT count(*) AS actual FROM mdam_test_tbl
WHERE (a = 10 AND b = 5) OR (a = 10 AND c = 3);

--
-- Middle column gap: arm skips column b entirely.
-- Should still produce Append after expanding the unconstrained b.
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE (a = 10 AND c = 5)
   OR (a = 10 AND b = 20 AND c = 7)
   OR (a = 20 AND c = 1);

-- Verify correctness
SET enable_mdam = off;
SET enable_bitmapscan = on;
SELECT count(*) AS expect FROM mdam_test_tbl
WHERE (a = 10 AND c = 5)
   OR (a = 10 AND b = 20 AND c = 7)
   OR (a = 20 AND c = 1);
SET enable_mdam = on;
SET enable_bitmapscan = off;
SELECT count(*) AS actual FROM mdam_test_tbl
WHERE (a = 10 AND c = 5)
   OR (a = 10 AND b = 20 AND c = 7)
   OR (a = 20 AND c = 1);

--
-- MDAM paper example (simplified to 4-column int index).
-- Deeply nested AND/OR with IN lists and ranges -- the flagship test.
--
-- Original predicate:
--   ((c = 10 AND b BETWEEN 4 AND 25) OR a IN (2, 4, 5))
--   AND
--   ((a = 4 AND c = 5) OR (c IN (5, 10) AND (b = 4 OR a = 2)))
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE ((c = 10 AND b >= 4 AND b <= 25) OR a IN (2, 4, 5))
  AND ((a = 4 AND c = 5) OR (c IN (5, 10) AND (b = 4 OR a = 2)));

-- Verify correctness
SET enable_mdam = off;
SET enable_bitmapscan = on;
SELECT count(*) AS expect FROM mdam_test_tbl
WHERE ((c = 10 AND b >= 4 AND b <= 25) OR a IN (2, 4, 5))
  AND ((a = 4 AND c = 5) OR (c IN (5, 10) AND (b = 4 OR a = 2)));
SET enable_mdam = on;
SET enable_bitmapscan = off;
SELECT count(*) AS actual FROM mdam_test_tbl
WHERE ((c = 10 AND b >= 4 AND b <= 25) OR a IN (2, 4, 5))
  AND ((a = 4 AND c = 5) OR (c IN (5, 10) AND (b = 4 OR a = 2)));

--
-- Date column type: MDAM must handle non-integer types correctly.
--
DROP TABLE IF EXISTS mdam_date_tbl;
CREATE TABLE mdam_date_tbl (dept int, sdate date, item_class int, store int);
INSERT INTO mdam_date_tbl
  SELECT i/100, '1995-01-01'::date + (i % 400), i % 75, i % 300
  FROM generate_series(1, 100000) i;
CREATE INDEX mdam_date_tbl_idx ON mdam_date_tbl(dept, sdate, item_class, store);
ANALYZE mdam_date_tbl;

-- MDAM paper flagship query shape (adapted for date column).
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_date_tbl
WHERE ((item_class = 10 AND sdate >= '1995-06-04' AND sdate <= '1995-06-25')
       OR dept IN (2, 4, 5))
  AND ((dept = 4 AND item_class = 5)
       OR (item_class IN (5, 10) AND (sdate = '1995-06-04' OR dept = 2)));

-- Verify correctness
SET enable_mdam = off;
SET enable_bitmapscan = on;
SELECT count(*) AS expect FROM mdam_date_tbl
WHERE ((item_class = 10 AND sdate >= '1995-06-04' AND sdate <= '1995-06-25')
       OR dept IN (2, 4, 5))
  AND ((dept = 4 AND item_class = 5)
       OR (item_class IN (5, 10) AND (sdate = '1995-06-04' OR dept = 2)));
SET enable_mdam = on;
SET enable_bitmapscan = off;
SELECT count(*) AS actual FROM mdam_date_tbl
WHERE ((item_class = 10 AND sdate >= '1995-06-04' AND sdate <= '1995-06-25')
       OR dept IN (2, 4, 5))
  AND ((dept = 4 AND item_class = 5)
       OR (item_class IN (5, 10) AND (sdate = '1995-06-04' OR dept = 2)));

DROP TABLE mdam_date_tbl;

--
-- Only 2+ key column indexes qualify: single-column index should not trigger MDAM
--
DROP TABLE IF EXISTS mdam_single_col_tbl;
CREATE TABLE mdam_single_col_tbl (x int);
CREATE INDEX ON mdam_single_col_tbl (x);
INSERT INTO mdam_single_col_tbl SELECT i FROM generate_series(1, 1000) i;
ANALYZE mdam_single_col_tbl;

EXPLAIN (COSTS OFF)
SELECT * FROM mdam_single_col_tbl WHERE x = 1 OR x = 5;

DROP TABLE mdam_single_col_tbl;

--
-- Prepared statements: MDAM requires constant operands for its rewrite, so
-- generic plans (which use Param nodes instead of Const) must not produce
-- MDAM paths.  Force plan_cache_mode = force_generic_plan so we reliably
-- get the generic plan rather than a custom plan that still has constants.
--
SET plan_cache_mode = force_generic_plan;

PREPARE mdam_prep(int, int, int, int) AS
SELECT * FROM mdam_test_tbl
WHERE (a = $1 AND c = $2) OR (a = $3 AND b = $4);

EXPLAIN (COSTS OFF) EXECUTE mdam_prep(5, 10, 10, 20);

DEALLOCATE mdam_prep;
RESET plan_cache_mode;

--
-- Backward scan: ORDER BY ... DESC should use backward index scan
-- without an explicit Sort node, by reversing disjunct order and
-- scanning each sub-path in BackwardScanDirection.
--
EXPLAIN (COSTS OFF)
SELECT * FROM mdam_test_tbl
WHERE (a = 5 AND c = 10) OR (a = 10 AND b = 20)
ORDER BY a DESC, b DESC, c DESC, d DESC;

SELECT a, b, c, d FROM mdam_test_tbl
WHERE (a = 5 AND c = 10) OR (a = 10 AND b = 20)
ORDER BY a DESC, b DESC, c DESC, d DESC;

--
-- Fuzz-derived regression tests
--
-- The following tests are condensed reproducers for bugs found by
-- mdam_fuzz_test.py.  Each compares MDAM output against a seqscan
-- oracle; if they disagree, the MDAM output is wrong.
--
-- These tests document known MDAM bugs.  When the bugs are fixed,
-- the MDAM and oracle row counts will match.
--
DROP TABLE IF EXISTS mdam_fuzz_tbl;
CREATE TABLE mdam_fuzz_tbl (a int, b int, c int, d int);
INSERT INTO mdam_fuzz_tbl
SELECT NULLIF((i*7+13) % 100, 0), NULLIF((i*11+3) % 200, 0),
       NULLIF((i*5+17) % 50, 0),  NULLIF((i*3+7) % 25, 0)
FROM generate_series(1, 200000) i;
CREATE INDEX mdam_fuzz_tbl_abcd ON mdam_fuzz_tbl (a, b, c, d);
ANALYZE mdam_fuzz_tbl;

SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET max_parallel_workers_per_gather = 0;

--
-- Bug 1: multi-arm OR where one arm has leading-column IN + range on
-- middle column, the other has IN + EQ on middle+trailing.  MDAM drops
-- arms covering the upper IN values.
--
SET enable_mdam = on;
SELECT count(*) AS mdam_count FROM mdam_fuzz_tbl
WHERE (b IN (1, 19, 32, 39, 113, 138, 155, 183) AND c >= 33 AND d >= 13)
   OR (a IN (5, 8, 28, 55, 56, 67, 93) AND b > 188 AND c = 42);

SET enable_mdam = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SELECT count(*) AS oracle_count FROM mdam_fuzz_tbl
WHERE (b IN (1, 19, 32, 39, 113, 138, 155, 183) AND c >= 33 AND d >= 13)
   OR (a IN (5, 8, 28, 55, 56, 67, 93) AND b > 188 AND c = 42);

RESET enable_indexscan;
RESET enable_indexonlyscan;

--
-- Bug 2: arm with only trailing-column constraint (d = 1, no a/b/c).
-- MDAM limits retrievals to a few leading-column values and loses the
-- majority of matching rows.
--
SET enable_mdam = on;
SELECT count(*) AS mdam_count FROM mdam_fuzz_tbl
WHERE (a = 94 AND b IN (21, 51, 65, 175, 198)
       AND c IN (2, 15, 26, 37, 38, 46) AND d <= 10)
   OR (a IN (8, 12, 74, 93) AND a > 30 AND b = 16 AND d = 18)
   OR d = 1
   OR (a IN (7, 24, 35, 50, 55, 61, 84) AND b >= 106
       AND c = 10 AND d < 24);

SET enable_mdam = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SELECT count(*) AS oracle_count FROM mdam_fuzz_tbl
WHERE (a = 94 AND b IN (21, 51, 65, 175, 198)
       AND c IN (2, 15, 26, 37, 38, 46) AND d <= 10)
   OR (a IN (8, 12, 74, 93) AND a > 30 AND b = 16 AND d = 18)
   OR d = 1
   OR (a IN (7, 24, 35, 50, 55, 61, 84) AND b >= 106
       AND c = 10 AND d < 24);

RESET enable_indexscan;
RESET enable_indexonlyscan;

--
-- Bug 3: text + int mixed, IN-list on leading int col not fully
-- covered by generated retrievals.
--
DROP TABLE IF EXISTS mdam_fuzz_mixed;
CREATE TABLE mdam_fuzz_mixed (x int, y text, z int, w int);
INSERT INTO mdam_fuzz_mixed
SELECT NULLIF((i*3+5) % 50, 0),
       'val_' || lpad(((i*7+11) % 100)::text, 3, '0'),
       NULLIF((i*11+3) % 40, 0),
       NULLIF((i*5+1) % 20, 0)
FROM generate_series(1, 50000) i;
CREATE INDEX mdam_fuzz_mixed_xyzw ON mdam_fuzz_mixed (x, y, z, w);
ANALYZE mdam_fuzz_mixed;

SET enable_mdam = on;
SELECT count(*) AS mdam_count FROM mdam_fuzz_mixed
WHERE (x IN (7, 20, 41) AND y > 'val_034' AND z >= 19)
   OR (x IN (3, 10, 36, 39, 42) AND y < 'val_084'
       AND z IN (4, 8, 11, 13, 17, 21, 39) AND w IN (2, 5, 11));

SET enable_mdam = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SELECT count(*) AS oracle_count FROM mdam_fuzz_mixed
WHERE (x IN (7, 20, 41) AND y > 'val_034' AND z >= 19)
   OR (x IN (3, 10, 36, 39, 42) AND y < 'val_084'
       AND z IN (4, 8, 11, 13, 17, 21, 39) AND w IN (2, 5, 11));

RESET enable_indexscan;
RESET enable_indexonlyscan;

DROP TABLE mdam_fuzz_mixed;
DROP TABLE mdam_fuzz_tbl;

-- Cleanup
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET max_parallel_workers_per_gather;
RESET enable_mdam;

DROP TABLE mdam_test_tbl;

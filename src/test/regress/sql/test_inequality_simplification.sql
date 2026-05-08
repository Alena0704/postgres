--
-- INEQUALITY SIMPLIFICATION
--
-- Test canonicalize_qual() / find_duplicate_ors() range simplifier:
-- AND => intersection of intervals, OR => union (single interval only).
-- Operators: <, <=, >, >=, = on one Var, immutable, btree opfamily.
--

CREATE TABLE inequality_simplification_tab (x int, y int);
INSERT INTO inequality_simplification_tab (x, y)
VALUES (0,10), (1,9), (2,8), (3,7), (4,6), (5,5), (6,4), (7,3), (8,2), (9,1), (10,0), (15,-5);
ANALYZE inequality_simplification_tab;
--
-- AND: intersection
--

-- (x < 1 AND x < 2) => x < 1
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x < 1 AND x < 2);

-- (x <= 1 AND x <= 2) => x <= 1
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x <= 1 AND x <= 2);

-- (x > 5 AND x > 10) => x > 10
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x > 5 AND x > 10);

-- (x >= 5 AND x >= 10) => x >= 10
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x >= 5 AND x >= 10);

-- (x > 5 AND x >= 5) => x > 5  (stricter wins at same bound)
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x > 5 AND x >= 5);

-- (x >= 5 AND x > 5) => x > 5  (order shouldn't matter)
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x >= 5 AND x > 5);

-- (x > 1 AND x < 6) => unchanged (both bounds kept)
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x > 1 AND x < 6);

-- (x > 5 AND x = 6) => x = 6  (equality absorbs lower bound)
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x > 5 AND x = 6);

-- (x = 6 AND x < 10) => x = 6  (equality absorbs upper bound)
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x = 6 AND x < 10);

-- AND contradiction: x > 5 AND x < 4 => false
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x > 5 AND x < 4);

-- AND contradiction: x = 1 AND x = 2 => false
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x = 1 AND x = 2);

--
-- AND: equality + lower bound (regression test for apply_lower bug)
--

-- x = 5 AND x > 0 => x = 5  (equality above lower bound is valid)
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x = 5 AND x > 0);

-- x = 5 AND x >= 5 => x = 5  (equality on inclusive bound is valid)
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x = 5 AND x >= 5);

-- x = 5 AND x > 5 => false  (equality on exclusive bound is contradiction)
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x = 5 AND x > 5);

-- x = 0 AND x > 0 => false  (equality below exclusive lower bound)
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x = 0 AND x > 0);

-- x = 15 AND x < 10 => false  (equality above upper bound)
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x = 15 AND x < 10);

--
-- AND: Const op Var (reversed operand order)
--

-- 0 < x AND 10 > x => x > 0 AND x < 10
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (0 < x AND 10 > x);

-- 5 = x AND 0 < x => x = 5
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (5 = x AND 0 < x);

--
-- AND: multiple Vars (each Var simplified independently)
--

-- x > 1 AND x < 10 AND y > 3 AND y < 8 => both ranges kept
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x > 1 AND x < 10 AND y > 3 AND y < 8);

-- x > 5 AND x > 10 AND y < 3 AND y < 8 => x > 10 AND y < 3
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x > 5 AND x > 10 AND y < 3 AND y < 8);

--
-- OR: absorption (single-sided same Var)
--

-- x < 1 OR x < 10 => x < 10
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x < 1 OR x < 10);

-- x <= 1 OR x <= 10 => x <= 10
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x <= 1 OR x <= 10);

-- x > 1 OR x > 10 => x > 1
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x > 1 OR x > 10);

-- x >= 1 OR x >= 10 => x >= 1
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x >= 1 OR x >= 10);

--
-- OR: boolean absorption  A OR (A AND B) => A
--

-- (x > 1) OR (x > 1 AND x < 5) => x > 1
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE ((x > 1) OR (x > 1 AND x < 5));

-- (x > 1 AND y < 5) OR (x > 1 AND y < 5 AND y > 0) => x > 1 AND y < 5
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab
WHERE ((x > 1 AND y < 5) OR (x > 1 AND y < 5 AND y > 0));

--
-- OR: union of intervals (overlapping => single interval)
--

-- (x > 1 AND x < 5) OR (x > 2 AND x < 8) => (x > 1 AND x < 8)
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab
WHERE ((x > 1 AND x < 5) OR (x > 2 AND x < 8));

-- (x > 1 AND x < 4) OR (x < 2 AND x < 8) => x < 4
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab
WHERE ((x > 1 AND x < 4) OR (x < 2 AND x < 8));

-- (x >= 1 AND x <= 5) OR (x >= 2 AND x <= 8) => (x >= 1 AND x <= 8)
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab
WHERE ((x >= 1 AND x <= 5) OR (x >= 2 AND x <= 8));

-- touching ranges: (x <= 5) OR (x >= 5) => always true? No, union = single interval x <= 5 OR x >= 5
-- These touch at 5. Union should be (-inf, +inf) = no filter needed.
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab
WHERE (x <= 5 OR x >= 5);

-- equality as OR arm: (x = 3) OR (x > 5 AND x < 8) => should merge if treated as point range
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab
WHERE (x = 3 OR (x > 5 AND x < 8));

--
-- OR: non-overlapping => do not merge (keep OR)
--

-- (x > 1 AND x < 6) OR (x > 8) => must stay OR
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab
WHERE ((x > 1 AND x < 6) OR (x > 2 AND x > 8));

-- (x < 2) OR (x > 5) => must stay OR (gap)
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab WHERE (x < 2 OR x > 5);

--
-- Bail-out: non-scalar types should not be simplified
--

CREATE TABLE inequality_simplification_geo (p point);
INSERT INTO inequality_simplification_geo VALUES ('(0,0)'), ('(1,1)'), ('(2,2)');
ANALYZE inequality_simplification_geo;

-- point operators are not btree: must keep original expression
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_geo WHERE (p <@ box '(0,0),(1,1)');

DROP TABLE inequality_simplification_geo;

--
-- Different data types
--

CREATE TABLE inequality_simplification_types (
    d date,
    n numeric,
    f float8
);
INSERT INTO inequality_simplification_types VALUES
    ('2024-01-01', 1.5, 1.0),
    ('2024-06-15', 5.5, 5.0),
    ('2024-12-31', 10.5, 10.0);
ANALYZE inequality_simplification_types;

-- date: (d > '2024-01-01' AND d > '2024-06-01') => d > '2024-06-01'
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_types
WHERE (d > '2024-01-01' AND d > '2024-06-01');

-- numeric: (n < 10 AND n < 5) => n < 5
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_types
WHERE (n < 10 AND n < 5);

-- float8: (f > 1.0 AND f > 5.0) => f > 5.0
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_types
WHERE (f > 1.0 AND f > 5.0);

DROP TABLE inequality_simplification_types;

--
-- IN-list (SAOP) integration with range union
--
-- IN-list values fully inside a range -> range alone
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab
WHERE x IN (5, 6, 7) OR (x >= 1 AND x <= 10);

-- IN-list completely outside range -> stays as OR
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab
WHERE x IN (1, 2, 3) OR (x >= 10 AND x <= 20);

--
-- Not-equal (<>) simplification
--
-- x <> V OR x = V => TRUE  (full universe except NULL)
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab
WHERE x <> 5 OR x = 5;

-- order swapped
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab
WHERE x = 5 OR x <> 5;

-- x < V OR x > V => x <> V
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab
WHERE x < 5 OR x > 5;

-- different constants: should NOT simplify
EXPLAIN (COSTS OFF) SELECT * FROM inequality_simplification_tab
WHERE x <> 5 OR x = 7;

--
-- Result checks (deterministic row sets)
--
-- x = 6 only
SELECT x FROM inequality_simplification_tab WHERE (x > 5 AND x = 6) ORDER BY x;

-- x = 5 (equality + lower bound, regression for apply_lower bug)
SELECT x FROM inequality_simplification_tab WHERE (x = 5 AND x > 0) ORDER BY x;

-- empty (equality on exclusive lower bound)
SELECT x FROM inequality_simplification_tab WHERE (x = 5 AND x > 5) ORDER BY x;

-- empty
SELECT x FROM inequality_simplification_tab WHERE (x > 5 AND x < 4) ORDER BY x;

-- empty
SELECT x FROM inequality_simplification_tab WHERE (x = 1 AND x = 2) ORDER BY x;

-- x < 4: 0,1,2,3
SELECT x FROM inequality_simplification_tab WHERE ((x > 1 AND x < 4) OR (x < 2 AND x < 8)) ORDER BY x;

-- (1,6) union (8,inf): 2,3,4,5,9,10,15
SELECT x FROM inequality_simplification_tab WHERE ((x > 1 AND x < 6) OR (x > 8)) ORDER BY x;

DROP TABLE inequality_simplification_tab;

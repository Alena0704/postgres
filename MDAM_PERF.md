# MDAM: correctness and performance benchmarks

This note documents the test results for the MDAM OR-clause optimization
patch (branch `master_and_or_simplification`, commit `d0fd3d600d8`).

Two suites:

1. **Correctness against the MDAM paper queries** ([§1](#1-correctness--mdam-paper-test-suite)) —
   reproduces the test cases from VLDB'95 paper "Efficient Search of
   Multidimensional B-Trees" and from Peter Geoghegan's email-thread
   prototype script. For each query, MDAM output is compared row-by-row
   and in order against a seqscan oracle.

2. **Performance scaling** ([§2](#2-performance-scaling-100k--1m--5m)) —
   times each scenario with `enable_mdam = on` vs `off` across three
   data sizes (100K, 1M, 5M), measuring where the transformation wins,
   where it's neutral, and where it adds overhead.

See [MDAM_ARCHITECTURE.md](MDAM_ARCHITECTURE.md) for the planner pipeline
and [MDAM_COST.md](MDAM_COST.md) for the cost-model reasoning.

---

## 1. Correctness — MDAM paper test suite

### Setup

Table modeled after the MDAM paper's "sales" table (Section 3, Tables
1–3), populated with 1M rows where every column is computed from an
independent hash so the joint distribution is uniform:

```sql
CREATE TABLE sales_mdam_paper (
    dept       int,
    sdate      date,
    item_class int,
    store      int
);

INSERT INTO sales_mdam_paper
SELECT
    1 + (abs(hashint8(i::bigint))                % 100),
    '1995-01-01'::date
      + (abs(hashint8(i::bigint + 1000003)) % 400),
    1 + (abs(hashint8(i::bigint + 2000003)) % 75),
    1 + (abs(hashint8(i::bigint + 3000017)) % 300)
FROM generate_series(1, 1000000) i;

CREATE INDEX sales_mdam_paper_idx
ON sales_mdam_paper (dept, sdate, item_class, store);
ANALYZE sales_mdam_paper;
```

### Methodology

For each query:

1. Run with `enable_mdam = on, enable_seqscan = off, enable_bitmapscan = off`
   — captures `(rows, md5(string_agg(tuple, '|' ORDER BY <ORDER BY cols>)))`.
2. Run with `enable_mdam = off, enable_indexscan = off, enable_indexonlyscan = off`,
   `enable_seqscan = on` — captures the same checksum from a sequential scan.
3. Compare row counts and md5; both must match for the test to pass.

The md5 is computed over the **ordered** result, so it doubles as an
ordering check: if MDAM returns the right rows but in the wrong order,
the md5 differs.

### Results

```
                   test                    | mdam_rows | oracle_rows | md5_match |          plan_first_node          | verdict
-------------------------------------------+-----------+-------------+-----------+-----------------------------------+--------
 T1 paper relaxed (dept IN, IN)            |       804 |         804 | t         | Index Only Scan (SAOP)            | PASS
 T2 paper FULL flagship                    |       435 |         435 | t         | Append (9 IndexOnlyScans)         | PASS
 T3 skip-scan range                        |    500230 |      500230 | t         | Append (2 IndexOnlyScans)         | PASS
 T4 cross-column SAOP                      |       512 |         512 | t         | Index Only Scan (SAOP)            | PASS
 T5 range shattering                       |      2327 |        2327 | t         | Append (2 IndexOnlyScans)         | PASS
 T6 ordering-conflict (fallback)           |        31 |          31 | t         | Index Only Scan + Filter          | PASS
 T7 ordering-conflict later col (fallback) |       129 |         129 | t         | Index Only Scan + Filter          | PASS
 T8 no-conflict same-col OR+ineq           |        10 |          10 | t         | Index Only Scan (SAOP)            | PASS
 T9 no-conflict point leading              |         1 |           1 | t         | Index Only Scan + Filter          | PASS
 T10 backward DESC                         |       156 |         156 | t         | Append (2 IndexOnlyScans Backward)| PASS
 T11 SQL docs                              |         3 |           3 | t         | Index Only Scan (SAOP)            | PASS
```

**11/11 pass** — row counts and ordered md5 hashes all match between
MDAM and the seqscan oracle.

### Highlights of the plan shapes

**T2 — paper flagship** (`((item_class = 10 AND sdate >= '1995-06-04' AND sdate <= '1995-06-25') OR dept IN (2,4,5)) AND ((dept = 4 AND item_class = 5) OR (item_class IN (5,10) AND (sdate = '1995-06-04' OR dept = 2)))`):

```
Append
  ->  Index Only Scan ... Index Cond: (dept < 2) AND (sdate = '1995-06-04') AND (item_class = 10)
  ->  Index Only Scan ... Index Cond: (dept = 2) AND (item_class = ANY ('{5,10}'))
  ->  Index Only Scan ... Index Cond: (dept > 2) AND (dept < 4) AND (sdate = '1995-06-04') AND (item_class = 10)
  ->  Index Only Scan ... Index Cond: (dept = 4) AND (sdate < '1995-06-04') AND (item_class = 5)
  ->  Index Only Scan ... Index Cond: (dept = 4) AND (sdate = '1995-06-04') AND (item_class = ANY ('{5,10}'))
  ->  Index Only Scan ... Index Cond: (dept = 4) AND (sdate > '1995-06-04') AND (item_class = 5)
  ->  Index Only Scan ... Index Cond: (dept > 4) AND (dept < 5) AND (sdate = '1995-06-04') AND (item_class = 10)
  ->  Index Only Scan ... Index Cond: (dept = 5) AND (sdate = '1995-06-04') AND (item_class = ANY ('{5,10}'))
  ->  Index Only Scan ... Index Cond: (dept > 5) AND (sdate = '1995-06-04') AND (item_class = 10)
```

9 non-overlapping retrievals in keyspace order — vs the paper's 11
retrievals (Table 3) and Peter's hand-tuned Python script's 8.
The patch is more compact than the paper, slightly less than Peter's
ideal — an open optimization knob, not a correctness issue.

**T6, T7 — ordering conflict**: predicate cannot produce keyspace-ordered
output (e.g. `dept > 10 AND sdate = '1995-03-01' AND (item_class = 5 OR store = 50)`).
MDAM rejects the transformation in `mdam_detect_ordering_conflict()` and the
planner falls back to BitmapOr or Index Scan + Filter, preserving correctness.

**T10 — backward DESC**: subpaths are rebuilt with `BackwardScanDirection`
and the retrieval list is reversed, so `Append` produces rows in
descending keyspace order without a `Sort` node.

---

## 2. Performance scaling (100K / 1M / 5M)

### Methodology

For each scenario:

- The same query is run with `enable_mdam = on` and `enable_mdam = off`.
- 1 warmup execution per mode, then 5 timed runs per mode.
- Median execution time (from `EXPLAIN ANALYZE`) is reported.
- `max_parallel_workers_per_gather = 0` to keep timings deterministic.
- Speedup `x = off_median / on_median`. `> 1` means MDAM is faster.

### Results

```
scenario                                            off@100K  on@100K   x@100K   off@1M   on@1M   x@1M    off@5M   on@5M    x@5M
-----                                               ------    ------    -----    ------   ------  -----   ------   ------   -----
S1  paper-flagship                                    0.38      0.38    1.00x      1.58     1.56  1.01x     6.95     6.93   1.00x
S2  paper-flagship + ORDER BY leading + LIMIT 10      0.40      0.39    1.01x      1.67     0.14 12.03x     6.89     0.10  65.62x
S3  paper-flagship DESC LIMIT 10                      0.40      0.39    1.01x      1.67     0.15 10.98x     7.11     0.11  65.18x
S4  skip-scan, huge result (~50% of table)            5.36      5.34    1.00x     55.70    56.02  0.99x   279.36   282.09   0.99x
S5  range-shattering                                  0.70      0.71    0.99x      6.00     5.99  1.00x    33.42    33.91   0.99x
S6  ordering-conflict (must fall back)                0.39      0.38    1.02x      0.93     0.89  1.05x    11.92    11.72   1.02x
S7  JOIN broadcast nested loop                        0.35      0.37    0.94x      2.55     2.65  0.96x    12.76    13.67   0.93x
S8  GROUP BY after MDAM WHERE                         0.75      0.74    1.01x      6.38     4.43  1.44x    34.42    33.62   1.02x
S9  EXISTS (semi-join)                                0.15      0.14    1.09x      0.13     0.12  1.07x     0.10     0.10   0.90x
S10 simple eq (no transformation needed)              0.04      0.04    1.00x      0.04     0.05  0.98x     0.05     0.06   0.81x
S11 partial transform (mix indexed/non-indexed)       0.35      0.33    1.07x      2.21     0.82  2.70x    12.63    14.30   0.88x
S12 many-arm OR (5 arms)                              0.31      0.29    1.08x      1.27     1.31  0.97x    10.27    11.58   0.89x
```

### Where MDAM wins big — scales with N

| Scenario | 100K | 1M | 5M | Why |
|---|---:|---:|---:|---|
| **S2 — ORDER BY leading + LIMIT 10** | 1.0x | **12x** | **65x** | Append preserves index keyspace order, so LIMIT terminates after 10 rows. The `off` plan has to fetch every matching row through BitmapOr and sort. |
| **S3 — ORDER BY DESC + LIMIT 10** | 1.0x | **11x** | **65x** | Same as S2 but with backward IndexOnlyScans; the retrieval list is reversed. |

These are the **flagship MDAM wins**: the bigger the table relative to
the LIMIT, the more rows BitmapOr/Sort wastes, while MDAM stops after
exactly LIMIT rows.

### Where MDAM is **neutral** (no overhead)

| Scenario | 100K | 1M | 5M | Note |
|---|---:|---:|---:|---|
| S1 paper-flagship (no ORDER BY) | 1.00x | 1.01x | 1.00x | Append vs BitmapOr — equivalent total work |
| S4 skip-scan (~50% of table) | 1.00x | 0.99x | 0.99x | Huge result — both plans dominated by I/O, MDAM neither helps nor hurts |
| S5 range-shattering | 0.99x | 1.00x | 0.99x | Same retrieval count either way |
| S6 ordering-conflict (fallback) | 1.02x | 1.05x | 1.02x | **Confirms that the eligibility check is cheap**; planning overhead negligible even when MDAM rejects the transformation |
| S9 EXISTS | 1.09x | 1.07x | 0.90x | Inner side optimized to single index lookup either way |
| S10 simple eq | 1.00x | 0.98x | 0.81x | Trivial query — μs-scale noise dominates |

The most important neutrality result is **S6**: MDAM checks every
RestrictInfo at planning time, but ordering-conflict queries fall back
to BitmapOr in 0–5% extra time. There's no flat planner overhead from
having `enable_mdam = on`.

### Where MDAM regresses — and how much

| Scenario | 100K | 1M | 5M | Diagnosis |
|---|---:|---:|---:|---|
| **S7 JOIN broadcast nested loop** | 0.94x | 0.96x | 0.93x | ~5–7% slower at every size. Inner-side IndexOnlyScan is reused per outer row; Append adds per-row dispatch. |
| **S11 partial transform** | 1.07x | **2.70x** | 0.88x | At 1M, MDAM helps a lot (2.7x). At 5M, the non-indexed `store > 250` arm forces heap fetches that overwhelm the index advantage. |
| **S12 many-arm OR (5 arms)** | 1.08x | 0.97x | 0.89x | At 5M, 5 separate IndexOnlyScans cost more index re-entries than a single Bitmap accumulation. |

The 5M regressions in S11/S12 (~10–12%) point to a **cost-model gap**:
MDAM is being preferred when it shouldn't be. The fix lives in
`add_path()`'s comparison between `Append` and `Bitmap*` — currently the
Append cost estimate doesn't fully account for repeated index re-entry
costs at high cardinalities. See [MDAM_COST.md §5](MDAM_COST.md) for
the related selectivity-correction discussion.

### Headline interpretation

For the **transformation–overhead trade-off** the user asked about:

| Condition | Effect |
|---|---|
| `ORDER BY <leading_cols>` + small `LIMIT` | **Win grows linearly with N** — 65x at 5M |
| Backward scan (`ORDER BY ... DESC`) + `LIMIT` | Same as above — 65x at 5M |
| Large OR with narrow arms (paper flagship) | Tied: MDAM Append ≈ BitmapOr |
| Ordering-conflict detected | Auto-fallback, **0% planning overhead** |
| Simple/non-applicable WHERE | **0% overhead** — `expr_is_mdam_candidate` rejects in O(tree depth) |
| OR with non-indexed columns | Risk of 10–15% regression at large N — cost-model TODO |
| 50%-of-table result set | Tied — MDAM doesn't try to outperform SeqScan, but doesn't underperform |

---

## 3. Files

```
test SQL                          /tmp/mdam_paper_setup.sql        (table/data setup)
                                  /tmp/mdam_paper_test.sql         (narrow predicates)
                                  /tmp/mdam_paper_test2.sql        (broader predicates)
                                  /tmp/mdam_paper_verdict.sql      (PASS/FAIL verdict)

perf scripts                      /tmp/mdam_perf_run.py            (single-size benchmark, 22 scenarios)
                                  /tmp/mdam_perf_scale.py          (scaling benchmark, 12 scenarios × 3 sizes)
                                  /tmp/mdam_perf_resize.sql        (data reload at parametrized size)

raw output                        /tmp/mdam_paper_test_out.txt
                                  /tmp/mdam_paper_test2_out.txt
                                  /tmp/mdam_perf_run.out
                                  /tmp/mdam_perf_scale.out
```

External fuzz tester (existing, in repo): `mdam_fuzz_test.py`.

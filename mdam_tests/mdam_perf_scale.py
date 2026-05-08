#!/usr/bin/env python3
"""
MDAM perf benchmark across data sizes.

Reloads sales_mdam_paper at each size, then times a representative
subset of queries with enable_mdam = on/off.
"""

import os
import re
import statistics
import subprocess
import sys
import time

PSQL = os.environ.get("PSQL", "psql")
CONN = [
    "-h", os.environ.get("PGHOST", "/tmp"),
    "-p", os.environ.get("PGPORT", "5432"),
    "-U", os.environ.get("PGUSER", "postgres"),
    "-d", os.environ.get("PGDATABASE", "postgres"),
]

WARMUP = 1
N_RUNS = 5

# Representative subset: covers all distinct behaviors
SCENARIOS = [
    ("S1 paper-flagship",
     """SELECT dept, sdate, item_class, store
        FROM sales_mdam_paper
        WHERE ((item_class = 10 AND sdate >= '1995-06-04' AND sdate <= '1995-06-25')
               OR dept IN (2, 4, 5))
          AND ((dept = 4 AND item_class = 5)
               OR (item_class IN (5, 10) AND (sdate = '1995-06-04' OR dept = 2)))"""),
    ("S2 paper-flagship + ORDER BY leading + LIMIT 10",
     """SELECT dept, sdate, item_class, store
        FROM sales_mdam_paper
        WHERE ((item_class = 10 AND sdate >= '1995-06-04' AND sdate <= '1995-06-25')
               OR dept IN (2, 4, 5))
          AND ((dept = 4 AND item_class = 5)
               OR (item_class IN (5, 10) AND (sdate = '1995-06-04' OR dept = 2)))
        ORDER BY dept, sdate, item_class, store
        LIMIT 10"""),
    ("S3 paper-flagship DESC LIMIT 10",
     """SELECT dept, sdate, item_class, store
        FROM sales_mdam_paper
        WHERE ((item_class = 10 AND sdate >= '1995-06-04' AND sdate <= '1995-06-25')
               OR dept IN (2, 4, 5))
          AND ((dept = 4 AND item_class = 5)
               OR (item_class IN (5, 10) AND (sdate = '1995-06-04' OR dept = 2)))
        ORDER BY dept DESC, sdate DESC, item_class DESC, store DESC
        LIMIT 10"""),
    ("S4 skip-scan (huge result)",
     """SELECT dept, sdate, item_class, store FROM sales_mdam_paper
        WHERE (dept = 1 AND sdate < '1995-02-01') OR (dept > 50)"""),
    ("S5 range-shattering",
     """SELECT dept, sdate, item_class, store FROM sales_mdam_paper
        WHERE (dept BETWEEN 4 AND 7 AND item_class IN (5, 10, 15))
           OR (dept BETWEEN 9 AND 11 AND item_class IN (7, 12))"""),
    ("S6 ordering-conflict (must fall back)",
     """SELECT dept, sdate, item_class, store FROM sales_mdam_paper
        WHERE dept > 10 AND sdate = '1995-03-01'
          AND (item_class = 5 OR store = 50)"""),
    ("S7 JOIN broadcast nested loop",
     """SELECT s.dept, s.sdate, s.item_class
        FROM sales_mdam_paper s
        JOIN (VALUES (2),(4),(5)) v(d) ON s.dept = v.d
        WHERE s.item_class IN (5, 10)"""),
    ("S8 GROUP BY after MDAM WHERE",
     """SELECT dept, count(*) FROM sales_mdam_paper
        WHERE (dept BETWEEN 4 AND 7 AND item_class IN (5, 10, 15))
           OR (dept BETWEEN 9 AND 11 AND item_class IN (7, 12))
        GROUP BY dept ORDER BY dept"""),
    ("S9 EXISTS (semi-join)",
     """SELECT count(*) FROM (VALUES (2),(4),(5),(10)) v(d)
        WHERE EXISTS (
            SELECT 1 FROM sales_mdam_paper s
            WHERE s.dept = v.d AND s.item_class IN (5, 10)
        )"""),
    ("S10 simple eq (no transformation)",
     """SELECT count(*) FROM sales_mdam_paper
        WHERE dept = 5 AND sdate = '1995-06-04' AND item_class = 10"""),
    ("S11 partial transform (mix indexed/non-indexed)",
     """SELECT count(*) FROM sales_mdam_paper
        WHERE (dept = 5 AND item_class = 10)
           OR (dept = 10 AND store > 250)"""),
    ("S12 many-arm OR (5 arms)",
     """SELECT count(*) FROM sales_mdam_paper
        WHERE (dept = 1 AND item_class = 10)
           OR (dept = 2 AND item_class = 10)
           OR (dept = 3 AND item_class = 10)
           OR (dept = 4 AND item_class = 10)
           OR (dept = 5 AND item_class = 10)"""),
]

SIZES = [100_000, 1_000_000, 5_000_000]


def run_psql(sql: str, timeout: int = 600) -> str:
    p = subprocess.run([PSQL, *CONN, "-AtX", "-c", sql],
                       capture_output=True, text=True, timeout=timeout)
    return p.stdout + p.stderr


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def reload_size(rows: int):
    sys.stderr.write(f"[reload] sales_mdam_paper -> {rows} rows ...\n")
    sys.stderr.flush()
    p = subprocess.run([PSQL, *CONN, "-X", "-v", f"rows={rows}",
                        "-f", os.path.join(SCRIPT_DIR, "mdam_perf_resize.sql")],
                       capture_output=True, text=True, timeout=900)
    if "ERROR" in p.stdout + p.stderr:
        sys.stderr.write(p.stdout + p.stderr)
        sys.exit(1)


def time_query(sql: str, mdam: bool) -> float:
    full = f"""
BEGIN;
SET LOCAL enable_mdam = {'on' if mdam else 'off'};
SET LOCAL max_parallel_workers_per_gather = 0;
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF) {sql};
COMMIT;
"""
    out = run_psql(full)
    m = re.search(r"Execution Time:\s+([\d.]+)\s+ms", out)
    return float(m.group(1)) if m else -1.0


def bench_one(sql: str) -> tuple[float, float]:
    """Return (off_median_ms, on_median_ms)."""
    for _ in range(WARMUP):
        time_query(sql, False)
        time_query(sql, True)
    off_runs = [time_query(sql, False) for _ in range(N_RUNS)]
    on_runs  = [time_query(sql, True)  for _ in range(N_RUNS)]
    off_runs = [x for x in off_runs if x >= 0]
    on_runs  = [x for x in on_runs  if x >= 0]
    om = statistics.median(off_runs) if off_runs else -1
    nm = statistics.median(on_runs)  if on_runs  else -1
    return om, nm


def main():
    results: dict[tuple[str, int], tuple[float, float]] = {}
    for size in SIZES:
        reload_size(size)
        for label, sql in SCENARIOS:
            sys.stderr.write(f"[bench n={size}] {label} ...\n")
            sys.stderr.flush()
            t0 = time.monotonic()
            om, nm = bench_one(sql)
            sys.stderr.write(f"           off={om:.2f}ms  on={nm:.2f}ms  "
                             f"({time.monotonic()-t0:.1f}s)\n")
            results[(label, size)] = (om, nm)

    # Print combined table
    print()
    print(f"# MDAM scaling perf  (medians over {N_RUNS} runs after {WARMUP} warmup)")
    print()
    header = ["scenario"]
    for s in SIZES:
        header += [f"off@{s//1000}K_ms", f"on@{s//1000}K_ms", f"x@{s//1000}K"]
    print("  ".join(f"{h:>16}" if i > 0 else f"{h:<46}" for i, h in enumerate(header)))
    print("  ".join(["-" * 46] + ["-" * 16] * (len(header) - 1)))
    for label, _ in SCENARIOS:
        cells = [f"{label[:46]:<46}"]
        for s in SIZES:
            om, nm = results[(label, s)]
            sp = (om / nm) if nm > 0 else float("nan")
            cells += [f"{om:>16.2f}", f"{nm:>16.2f}",
                      f"{sp:>15.2f}x"]
        print("  ".join(cells))


if __name__ == "__main__":
    main()

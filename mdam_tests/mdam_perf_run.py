#!/usr/bin/env python3
"""
MDAM perf benchmark.

For each test case, run the same query with enable_mdam = on and = off,
N times each (after a warmup), report mean + stddev + speedup, and the
plan node used in each mode.

We use EXPLAIN (ANALYZE, BUFFERS) and parse the "Execution Time" line.
"""

import os
import re
import statistics
import subprocess
import sys

PSQL = os.environ.get("PSQL", "psql")
CONN = [
    "-h", os.environ.get("PGHOST", "/tmp"),
    "-p", os.environ.get("PGPORT", "5432"),
    "-U", os.environ.get("PGUSER", "postgres"),
    "-d", os.environ.get("PGDATABASE", "postgres"),
]

WARMUP = 1
N_RUNS = 5


def run_psql(sql: str) -> str:
    p = subprocess.run([PSQL, *CONN, "-AtX", "-c", sql],
                       capture_output=True, text=True, timeout=120)
    return p.stdout + p.stderr


def time_query(sql: str, mdam: bool, mode_setup: str = "") -> tuple[float, str]:
    """Run query once with EXPLAIN ANALYZE and return (ms, top_plan_node).

    mode_setup lets the caller add extra SET LOCAL lines (e.g. force seqscan).
    """
    full = f"""
BEGIN;
SET LOCAL enable_mdam = {'on' if mdam else 'off'};
SET LOCAL max_parallel_workers_per_gather = 0;
{mode_setup}
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF) {sql};
COMMIT;
"""
    out = run_psql(full)
    # Find "Execution Time: X ms"
    m = re.search(r"Execution Time:\s+([\d.]+)\s+ms", out)
    if not m:
        return -1.0, "<error>"
    exec_ms = float(m.group(1))

    # Top plan node = first non-empty line of EXPLAIN
    plan_node = "?"
    for line in out.splitlines():
        if line.startswith("Planning") or line.startswith("Execution"):
            continue
        s = line.strip()
        if s and not s.startswith("BEGIN") and not s.startswith("SET") \
                and not s.startswith("COMMIT") and not s.startswith("->") \
                and "rows=" not in s.split("(")[0]:
            plan_node = s.split("(")[0].strip()
            break
        if s.startswith("->"):
            plan_node = s.lstrip("->").split("(")[0].strip()
            break
    return exec_ms, plan_node


def bench(label: str, sql: str, mode_setup_off: str = "",
          mode_setup_on: str = "") -> dict:
    """Bench a single SQL with mdam=off then on. Return summary dict."""
    # Warmup
    for _ in range(WARMUP):
        time_query(sql, mdam=False, mode_setup=mode_setup_off)
        time_query(sql, mdam=True,  mode_setup=mode_setup_on)

    off_runs = []
    on_runs = []
    plan_off = plan_on = "?"
    for _ in range(N_RUNS):
        ms, plan_off = time_query(sql, mdam=False, mode_setup=mode_setup_off)
        if ms >= 0:
            off_runs.append(ms)
        ms, plan_on = time_query(sql, mdam=True, mode_setup=mode_setup_on)
        if ms >= 0:
            on_runs.append(ms)

    def stats(xs):
        if not xs:
            return (-1, -1, -1)
        return (statistics.median(xs), min(xs),
                statistics.pstdev(xs) if len(xs) > 1 else 0)

    off_med, off_min, off_sd = stats(off_runs)
    on_med,  on_min,  on_sd  = stats(on_runs)
    speedup = off_med / on_med if on_med > 0 else float("nan")
    return {
        "label": label,
        "off_med": off_med, "off_min": off_min, "off_sd": off_sd,
        "on_med":  on_med,  "on_min":  on_min,  "on_sd":  on_sd,
        "speedup": speedup,
        "plan_off": plan_off, "plan_on": plan_on,
    }


# ---------------------------------------------------------------------------
# Scenario catalog
# ---------------------------------------------------------------------------

#   - mode_off forces a non-MDAM plan (so we benchmark "the alternative
#     without MDAM"); we let the planner pick freely (typically BitmapOr)
#   - mode_on enables all standard plan choices; MDAM Append should win
#     where applicable

SCENARIOS = [
    # ---- 1. Pure WHERE OR, no ordering ----
    dict(
        label="1. Paper flagship, no ORDER BY",
        sql="""SELECT dept, sdate, item_class, store
               FROM sales_mdam_paper
               WHERE ((item_class = 10 AND sdate >= '1995-06-04' AND sdate <= '1995-06-25')
                      OR dept IN (2, 4, 5))
                 AND ((dept = 4 AND item_class = 5)
                      OR (item_class IN (5, 10) AND (sdate = '1995-06-04' OR dept = 2)))""",
    ),
    dict(
        label="2. Paper flagship + ORDER BY leading cols",
        sql="""SELECT dept, sdate, item_class, store
               FROM sales_mdam_paper
               WHERE ((item_class = 10 AND sdate >= '1995-06-04' AND sdate <= '1995-06-25')
                      OR dept IN (2, 4, 5))
                 AND ((dept = 4 AND item_class = 5)
                      OR (item_class IN (5, 10) AND (sdate = '1995-06-04' OR dept = 2)))
               ORDER BY dept, sdate, item_class, store""",
    ),
    dict(
        label="3. Paper flagship + ORDER BY non-leading col",
        sql="""SELECT dept, sdate, item_class, store
               FROM sales_mdam_paper
               WHERE ((item_class = 10 AND sdate >= '1995-06-04' AND sdate <= '1995-06-25')
                      OR dept IN (2, 4, 5))
                 AND ((dept = 4 AND item_class = 5)
                      OR (item_class IN (5, 10) AND (sdate = '1995-06-04' OR dept = 2)))
               ORDER BY store""",
    ),
    dict(
        label="4. Paper flagship + ORDER BY leading + LIMIT 10",
        sql="""SELECT dept, sdate, item_class, store
               FROM sales_mdam_paper
               WHERE ((item_class = 10 AND sdate >= '1995-06-04' AND sdate <= '1995-06-25')
                      OR dept IN (2, 4, 5))
                 AND ((dept = 4 AND item_class = 5)
                      OR (item_class IN (5, 10) AND (sdate = '1995-06-04' OR dept = 2)))
               ORDER BY dept, sdate, item_class, store
               LIMIT 10""",
    ),
    dict(
        label="5. Paper flagship + DESC LIMIT 10 (backward Append)",
        sql="""SELECT dept, sdate, item_class, store
               FROM sales_mdam_paper
               WHERE ((item_class = 10 AND sdate >= '1995-06-04' AND sdate <= '1995-06-25')
                      OR dept IN (2, 4, 5))
                 AND ((dept = 4 AND item_class = 5)
                      OR (item_class IN (5, 10) AND (sdate = '1995-06-04' OR dept = 2)))
               ORDER BY dept DESC, sdate DESC, item_class DESC, store DESC
               LIMIT 10""",
    ),
    # ---- 6-8. Skip-scan style ----
    dict(
        label="6. Skip-scan (dept=1,sdate<X) OR (dept>50)",
        sql="""SELECT dept, sdate, item_class, store FROM sales_mdam_paper
               WHERE (dept = 1 AND sdate < '1995-02-01') OR (dept > 50)""",
    ),
    dict(
        label="7. Skip-scan + ORDER BY leading + LIMIT 10",
        sql="""SELECT dept, sdate, item_class, store FROM sales_mdam_paper
               WHERE (dept = 1 AND sdate < '1995-02-01') OR (dept > 50)
               ORDER BY dept, sdate, item_class, store LIMIT 10""",
    ),
    # ---- 9. Range shattering ----
    dict(
        label="8. Range shattering across dept",
        sql="""SELECT dept, sdate, item_class, store FROM sales_mdam_paper
               WHERE (dept BETWEEN 4 AND 7 AND item_class IN (5, 10, 15))
                  OR (dept BETWEEN 9 AND 11 AND item_class IN (7, 12))""",
    ),
    # ---- 10-11. Ordering-conflict cases (must fall back) ----
    dict(
        label="9. Ordering-conflict (MDAM falls back)",
        sql="""SELECT dept, sdate, item_class, store FROM sales_mdam_paper
               WHERE dept > 10 AND sdate = '1995-03-01'
                 AND (item_class = 5 OR store = 50)""",
    ),
    dict(
        label="10. Ordering-conflict + ORDER BY",
        sql="""SELECT dept, sdate, item_class, store FROM sales_mdam_paper
               WHERE dept > 10 AND sdate = '1995-03-01'
                 AND (item_class = 5 OR store = 50)
               ORDER BY dept, sdate, item_class, store""",
    ),
    # ---- 12. JOIN ----
    dict(
        label="11. JOIN small lookup (broadcast nested loop)",
        sql="""SELECT s.dept, s.sdate, l.dept_name, s.item_class
               FROM sales_mdam_paper s
               JOIN dept_lookup l USING (dept)
               WHERE ((s.item_class = 10 AND s.sdate = '1995-06-04')
                      OR s.dept IN (2, 4, 5) AND s.item_class IN (5, 10))""",
    ),
    dict(
        label="12. JOIN small lookup + ORDER BY + LIMIT",
        sql="""SELECT s.dept, s.sdate, l.dept_name, s.item_class
               FROM sales_mdam_paper s
               JOIN dept_lookup l USING (dept)
               WHERE ((s.item_class = 10 AND s.sdate = '1995-06-04')
                      OR s.dept IN (2, 4, 5) AND s.item_class IN (5, 10))
               ORDER BY s.dept, s.sdate, s.item_class
               LIMIT 50""",
    ),
    dict(
        label="13. JOIN large + WHERE OR on outer side",
        sql="""SELECT s.dept, s.item_class, sum(o.rev) AS rev
               FROM sales_mdam_paper s
               JOIN sales_other o USING (dept, item_class)
               WHERE (s.dept = 5 AND s.item_class = 10)
                  OR (s.dept = 10 AND s.sdate = '1995-06-04')
               GROUP BY s.dept, s.item_class""",
    ),
    # ---- 14-15. GROUP BY / aggregation ----
    dict(
        label="14. GROUP BY dept after MDAM-eligible WHERE",
        sql="""SELECT dept, count(*) FROM sales_mdam_paper
               WHERE (dept BETWEEN 4 AND 7 AND item_class IN (5, 10, 15))
                  OR (dept BETWEEN 9 AND 11 AND item_class IN (7, 12))
               GROUP BY dept ORDER BY dept""",
    ),
    dict(
        label="15. count(*) over MDAM WHERE",
        sql="""SELECT count(*) FROM sales_mdam_paper
               WHERE ((item_class = 10 AND sdate = '1995-06-04')
                      OR dept IN (2, 4, 5) AND item_class IN (5, 10))""",
    ),
    # ---- 16. Subquery / EXISTS ----
    dict(
        label="16. EXISTS (semi-join) MDAM in inner side",
        sql="""SELECT l.dept_name, l.dept_cat
               FROM dept_lookup l
               WHERE EXISTS (
                   SELECT 1 FROM sales_mdam_paper s
                   WHERE s.dept = l.dept
                     AND ((s.item_class = 10 AND s.sdate = '1995-06-04')
                          OR s.dept IN (2, 4, 5) AND s.item_class IN (5, 10))
               )""",
    ),
    dict(
        label="17. IN (subquery) MDAM in inner side",
        sql="""SELECT count(*) FROM sales_mdam_paper s
               WHERE s.dept IN (
                   SELECT d FROM (VALUES (2), (4), (5), (10)) v(d)
               )
                 AND s.item_class IN (5, 10)""",
    ),
    # ---- 18. No transformation needed (simple eq) ----
    dict(
        label="18. Simple eq (no transformation needed)",
        sql="""SELECT count(*) FROM sales_mdam_paper
               WHERE dept = 5 AND sdate = '1995-06-04' AND item_class = 10""",
    ),
    dict(
        label="19. Simple eq with no OR (no transformation)",
        sql="""SELECT count(*) FROM sales_mdam_paper
               WHERE dept BETWEEN 5 AND 10 AND item_class = 5""",
    ),
    # ---- 20. Partial transformation: one OR is index-eligible, another not ----
    dict(
        label="20. Partial transform: OR mixes indexed + non-indexed col",
        sql="""SELECT count(*) FROM sales_mdam_paper
               WHERE (dept = 5 AND item_class = 10)
                  OR (dept = 10 AND store > 250)""",
    ),
    dict(
        label="21. WHERE has both eligible OR and non-OR clauses",
        sql="""SELECT count(*) FROM sales_mdam_paper
               WHERE store > 290
                 AND ((dept = 5 AND item_class = 10)
                      OR (dept = 10 AND sdate = '1995-06-04'))""",
    ),
    # ---- 22. Bigger blowup test ----
    dict(
        label="22. Many-arm OR (5 arms, all MDAM-eligible)",
        sql="""SELECT count(*) FROM sales_mdam_paper
               WHERE (dept = 1 AND item_class = 10)
                  OR (dept = 2 AND item_class = 10)
                  OR (dept = 3 AND item_class = 10)
                  OR (dept = 4 AND item_class = 10)
                  OR (dept = 5 AND item_class = 10)""",
    ),
]


def fmt(x):
    if x < 0:
        return "    -"
    return f"{x:7.2f}"


def main():
    print(f"# MDAM perf benchmark — {N_RUNS} runs per (mode, query) after {WARMUP} warmup")
    print()
    rows = []
    for sc in SCENARIOS:
        sys.stderr.write(f"[bench] {sc['label']} ...\n")
        sys.stderr.flush()
        rows.append(bench(sc["label"], sc["sql"]))

    # Print table
    print(f"{'#':>2}  {'label':<55}  {'off ms':>9}  {'on ms':>9}  "
          f"{'speedup':>7}  plan_off → plan_on")
    print("-" * 160)
    for i, r in enumerate(rows, 1):
        po = r["plan_off"][:30]
        pn = r["plan_on"][:30]
        sp = (f"{r['speedup']:6.2f}x" if r["speedup"] > 0
              and r["speedup"] != float("inf") else "    -  ")
        print(f"{i:>2}  {r['label'][:55]:<55}  "
              f"{fmt(r['off_med'])}  {fmt(r['on_med'])}  "
              f"{sp}  {po:<30} → {pn}")

    print()
    print("Notes:")
    print("  off ms / on ms : median Execution Time over runs")
    print("  speedup        : off_med / on_med   (>1 means MDAM faster)")
    print("  plan_off / plan_on : top plan node (first non-trivial line of EXPLAIN)")


if __name__ == "__main__":
    main()

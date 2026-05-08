# MDAM tests — manual correctness and perf scripts

Manual / out-of-tree test harness for the MDAM OR-clause optimization
on branch `master_and_or_simplification`.

Results: see [../MDAM_PERF.md](../MDAM_PERF.md).

In-tree regression test:
[`src/test/regress/sql/mdam.sql`](../src/test/regress/sql/mdam.sql).

## Files

| File | Purpose |
|---|---|
| `mdam_paper_setup.sql` | Build `sales_mdam_paper` (1M rows, 4-col btree index) — based on the MDAM paper's sales table |
| `mdam_paper_verdict.sql` | Run all paper / Geoghegan-thread test queries, compare MDAM vs seqscan oracle (rows + ordered md5), print PASS/FAIL |
| `mdam_perf_resize.sql` | Reload `sales_mdam_paper` and `sales_other` at a parametrized size (`-v rows=N`) |
| `mdam_perf_run.py` | Single-size benchmark — 22 scenarios, `enable_mdam` on vs off, median over 5 runs |
| `mdam_perf_scale.py` | Scaling benchmark — 12 scenarios at 100K / 1M / 5M rows |

## Running

Set the libpq env vars (or `PSQL` to point at a non-default binary), then:

```sh
# Configure your build
export PSQL=/path/to/postgres/bin/psql
export PGHOST=/tmp
export PGPORT=5432
export PGUSER=postgres
export PGDATABASE=postgres

cd mdam_tests/

# 1. Correctness suite
$PSQL -f mdam_paper_setup.sql
$PSQL -f mdam_paper_verdict.sql

# 2. Single-size perf (1M rows — must run setup first)
python3 mdam_perf_run.py

# 3. Scaling perf (reloads at each size, ~10 minutes total)
python3 mdam_perf_scale.py
```

Both Python scripts default to 1 warmup + 5 timed runs per `(query, mode)`
pair; tweak `WARMUP` and `N_RUNS` at the top of each script.

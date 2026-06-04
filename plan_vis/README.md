# plan_vis — standard PostgreSQL plan-formation visualizer (DP + GEQO)

Standalone tooling to visualize how the **standard** PostgreSQL planner forms a
plan — both DP (`standard_join_search`) and GEQO — and to compare their sampled
subplans. Independent of the MCTS project: nothing here imports or needs
`practical_mcts_qo`.

## Pieces

- `plan_trace/` — a small extension that records, for the last planned query,
  every joinrel the standard join search formed (`plan_trace_joins()`) and the
  GEQO genetic search generation by generation (`plan_trace_geqo()`).
  It installs two core hooks (`join_rel_trace_hook`, `geqo_gen_trace_hook`) that
  must exist in the server — they are part of this fork's core.
- `plan_tree.py` — runs a query through DP and/or GEQO and draws the plan: DP
  candidate lattice (bushy highlighted), GEQO winning tree + fitness + edge
  inheritance + crossover. `--slices` emits the full analytics suite.
- `plan_slices.py` — the slice analytics (GEQO subplan table/scatter/survival,
  DP frontier/rank/counts/bushy/tree, DP↔GEQO overlap). Imported by
  `plan_tree.py --slices`.

## Build the extension

```bash
PG_CONFIG=/Users/alena/my_postgres11/my/inst/bin/pg_config
make -C plan_trace PG_CONFIG="$PG_CONFIG" install
# in the target database, once:
psql ... -c "CREATE EXTENSION plan_trace;"
```

The core hooks `join_rel_trace_hook` / `geqo_gen_trace_hook` (and the
`geqo_eval_seq` counter used for the good/bad-tour slices) must be present in the
server build. They live in core: `src/backend/optimizer/path/allpaths.c`,
`src/backend/optimizer/geqo/{geqo_main,geqo_eval}.c` and the matching headers.
See **[CORE_CHANGES.md](CORE_CHANGES.md)** for the exact additions (two hooks, a
flag and a counter — 84 lines across 5 files, inert when unset).

## Run

```bash
BIN=/Users/alena/my_postgres11/my/inst/bin

# one picture for whatever the planner actually used (DP < 12 rels, GEQO >= 12):
python3 plan_tree.py --pgbin "$BIN" --db imdb --port 5499 --user alena \
  --sql /path/to/query.sql --out /tmp/q

# force a mode, or both:
python3 plan_tree.py ... --only dp     # or geqo / both / natural (default)

# full slice analytics (forces one DP + one GEQO run, ~16 figures):
python3 plan_tree.py ... --slices --out /tmp/q
```

`--set k=v` passes through GUCs, e.g. `--set geqo_threshold=2` to force GEQO on a
small query, or `--set geqo_threshold=20` to keep DP on a large one.

## Outputs (with `--slices`)

DP: `_dp.png` (candidate lattice), `_dp_frontier.png`, `_dp_rank.png`,
`_dp_counts.png`, `_dp_bushy.png`, `_dp_tree.png`.
GEQO: `_geqo.png`, `_geqo_lattice.png`, `_geqo_crossover.png`,
`_geqo_slices.{csv,png}`, `_geqo_slice_scatter.png`, `_geqo_survival.png`,
`_geqo_winner_prov.png`.
Comparison: `_cmp_overlap.png`, `_cmp_subplans.png`.

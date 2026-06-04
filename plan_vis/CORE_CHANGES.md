# Core changes required by plan_vis

The `plan_trace` extension is a pure observer — it only reads planner state — but
core has no hook at the points it needs to observe. So plan_vis requires a small
set of additions to this fork's core: **two hooks, one flag, one counter**
(84 insertions across 5 files, no behavior change when the hooks are unset).
Rebuild & install core (`make -C src/backend install`, `make -C src/include
install`) before building the extension.

## 1. `join_rel_trace_hook` — observe every joinrel both planners form

Fires once per joinrel right after `set_cheapest()`, for **both** DP and GEQO,
so the extension can record the candidate lattice / winning tour. The 4th arg
`source` is 0 for DP, 1 for GEQO.

- **`src/include/optimizer/paths.h`** — typedef + global decl:
  ```c
  typedef void (*join_rel_trace_hook_type) (PlannerInfo *root,
                                            RelOptInfo *joinrel,
                                            int level, int source);
  extern PGDLLIMPORT join_rel_trace_hook_type join_rel_trace_hook;
  ```
- **`src/backend/optimizer/path/allpaths.c`** — definition + call in
  `standard_join_search()` (DP), after `set_cheapest(rel)`:
  ```c
  join_rel_trace_hook_type join_rel_trace_hook = NULL;
  ...
      if (join_rel_trace_hook)
          join_rel_trace_hook(root, rel, lev, 0);   /* source 0 = DP */
  ```
- **`src/backend/optimizer/geqo/geqo_eval.c`** — call in `merge_clump()`, after
  `set_cheapest(joinrel)`:
  ```c
      if (join_rel_trace_hook)
          join_rel_trace_hook(root, joinrel,
                              bms_num_members(joinrel->relids), 1);  /* 1 = GEQO */
  ```

## 2. `geqo_gen_trace_hook` + `geqo_tracing_final` — observe the GA generation by generation

The hook reports, per generation, the two parents, the recombined child and the
fitnesses — so the visualizer can show inherited traits. `geqo_tracing_final`
gates `join_rel_trace_hook` to record the joinrels of the **winning tour only**,
not of every tour GEQO evaluates.

- **`src/include/optimizer/geqo.h`** — typedef + decls:
  ```c
  typedef void (*geqo_gen_trace_hook_type) (PlannerInfo *root, int generation,
                                            const Gene *momma, const Gene *daddy,
                                            const Gene *kid, int num_gene,
                                            double kid_worth, double best_worth);
  extern PGDLLIMPORT geqo_gen_trace_hook_type geqo_gen_trace_hook;
  extern PGDLLIMPORT bool geqo_tracing_final;
  ```
- **`src/backend/optimizer/geqo/geqo_main.c`** — globals, the per-generation call,
  and the winning-tour bracket around `gimme_tree()`:
  ```c
  geqo_gen_trace_hook_type geqo_gen_trace_hook = NULL;
  bool        geqo_tracing_final = false;
  ...
      geqo_tracing_final = true;
      best_rel = gimme_tree(root, best_tour, pool->string_length);
      geqo_tracing_final = false;
  ```
  **Subtlety — `momma_snap`:** with ERX (the default crossover) the code does
  `kid = momma` and rewrites `momma->string` in place, so by the time the hook
  runs `momma` already equals the kid. A snapshot of momma is taken *before*
  crossover and passed to the hook, so it can attribute each child edge to the
  real parent:
  ```c
      Gene *momma_snap = palloc(pool->string_length * sizeof(Gene));
      ...
      if (geqo_gen_trace_hook)              /* before recombination */
          memcpy(momma_snap, momma->string, pool->string_length * sizeof(Gene));
      ...
      if (geqo_gen_trace_hook)              /* after evaluation */
          geqo_gen_trace_hook(root, generation, momma_snap, daddy->string,
                              kid->string, pool->string_length, kid->worth,
                              pool->data[0].worth);
  ```

## 3. `geqo_eval_seq` — group a tour's joinrels by the tour that built them

A monotonic counter bumped once per `geqo_eval()` (i.e. once per evaluated tour),
exposed so the extension can stamp each candidate joinrel with the tour it came
from. That is what powers the "frequent in good tours vs bad tours",
`best_cost_seen`, `first/last_gen` slices (SRF column `eval_seq`).

- **`src/include/optimizer/geqo.h`** — `extern PGDLLIMPORT int geqo_eval_seq;`
- **`src/backend/optimizer/geqo/geqo_eval.c`** — `int geqo_eval_seq = 0;` and
  `geqo_eval_seq++;` at the top of `geqo_eval()`.

## Files touched

| file | what |
|------|------|
| `src/include/optimizer/paths.h` | `join_rel_trace_hook` typedef + decl |
| `src/backend/optimizer/path/allpaths.c` | hook global + DP call (source 0) |
| `src/include/optimizer/geqo.h` | `geqo_gen_trace_hook`, `geqo_tracing_final`, `geqo_eval_seq` decls |
| `src/backend/optimizer/geqo/geqo_main.c` | hook/flag globals, momma snapshot, per-gen call, winning-tour bracket |
| `src/backend/optimizer/geqo/geqo_eval.c` | GEQO joinrel call (source 1), `geqo_eval_seq` counter |

All additions are inert when the hooks are `NULL` / tracing is off, so a stock
build is unaffected; only loading `plan_trace` and `SET plan_trace.enabled = on`
arms them.

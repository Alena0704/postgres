# MDAM: cost model for Append vs Sort

This note explains the cost reasoning behind MDAM's path-shape
choices and why the implementation only emits a plain `Append`.

See [MDAM_ARCHITECTURE.md](MDAM_ARCHITECTURE.md) for the broader
pipeline.

---

## 1. The candidate shapes

Given an MDAM-produced list of N non-overlapping retrievals (each
already an `IndexScan` / `IndexOnlyScan` on the same multi-column
btree), the planner can in principle wrap them in:

| Shape | What it does | Output ordering |
|---|---|---|
| `Append` | Concatenates subpaths in list order | Whatever the subpaths produce, concatenated |
| `MergeAppend` | N-way heap merge of pre-sorted subpaths | Globally sorted by the supplied `pathkeys` |
| `Sort` over `Append` | Append everything, then full sort | Sorted by the supplied sort key |

**MDAM only emits `Append`.**  The reason is that retrievals are
delivered in non-overlapping key-space order by step 4 of the
pipeline (`mdam_expand_sort_coalesce`), so a plain Append already
carries the right pathkeys for free — no per-tuple heap-merge work
is needed.  When the query orders by a non-leading column, the
planner adds a `Sort` on top of Append; in that case the
hypothetical MergeAppend's heap overhead would be wasted because
the subpaths don't claim the requested suffix as their pathkeys
(see §4).

---

## 2. Heap-merge overhead — what `MergeAppend` actually costs

`MergeAppend` is an *N-way merge*: it takes N already-sorted streams
and produces one sorted stream.

### The mechanism

A binary heap of size N is maintained in memory, holding the current
front tuple from each input.  Each output tuple costs:

1. Extract minimum from heap   →   `O(log₂ N)` comparisons
2. Pull next tuple from that input
3. Insert it into the heap     →   `O(log₂ N)` comparisons

So per output tuple: `2 × log₂(N) × comparison_cost`.

### The formula in `cost_merge_append` (`costsize.c`)

```c
N    = n_streams;                                  /* number of inputs    */
logN = log2(N);
comparison_cost = 2 * cpu_operator_cost;

/* Initial heap build */
startup_cost += comparison_cost * N * logN;

/* Per-tuple maintenance */
run_cost     += tuples * comparison_cost * logN;

/* Small per-tuple plumbing overhead */
run_cost     += cpu_tuple_cost * APPEND_CPU_COST_MULTIPLIER * tuples;
```

Notice: `logN` is `log₂ of the number of streams`, **not** of the
number of tuples.  That's the key reason MergeAppend can be much
cheaper than Sort.

### Comparison of the three shapes

For `tuples` output rows from N retrievals:

| Shape | Sort/merge cost | Notes |
|---|---|---|
| `Append` (no ordering) | `0` | only sums subpath costs |
| `MergeAppend` | `tuples × log₂(N) × c` | + small heap-build startup |
| `Sort + Append` | `tuples × log₂(tuples) × c` | full quicksort |

Where `c = 2 × cpu_operator_cost`.

### Numerical example

100 000-row table, MDAM produces 5 retrievals selecting 200 rows total
(`tuples = 200`, `N = 5`):

| Shape | Cost factor | Roughly |
|---|---|---|
| `Append` | `0` | only sum of subpath costs |
| `MergeAppend` | `200 × log₂(5) ≈ 200 × 2.32 ≈ 464 c` | 464 comparisons |
| `Sort + Append` | `200 × log₂(200) ≈ 200 × 7.64 ≈ 1528 c` | 1528 comparisons |

Here `MergeAppend` is ~3.3× cheaper than `Sort` *if both can satisfy
the ordering*.  But — as section 4 explains — it isn't always able to.

---

## 3. What MDAM emits

`mdam_build_append_path()` adds at most two paths per index:

```
   ├── Append, pathkeys = forward index pathkeys
   └── Append, pathkeys = backward index pathkeys (DESC scan, useful)
```

Upper-rel processing either uses one directly (if its pathkeys satisfy
the query's `ORDER BY`) or wraps it in `Sort`.

### Observed outcomes (from `mdam` regression tests)

| Query shape | Winning plan | Why |
|---|---|---|
| `WHERE ... OR ...` (no ORDER BY) | `Append` | retrievals already non-overlapping |
| `ORDER BY a, b, c, d` (leading) | `Append` | retrievals already in `(a, b, c, d)` keyspace order — free ordering |
| `ORDER BY a DESC` | `Append` over `Index Scan Backward` | backward subpaths reversed so list is in descending keyspace order |
| `ORDER BY b` (non-leading) | `Sort + Append` | see §4 — would need stripped pathkeys to do better |

---

## 4. When MergeAppend *should* win but currently doesn't

### Matthias's scenario

```sql
-- Index: (a, b, c)
-- Query:
SELECT * FROM t WHERE a IN (1, 2, 3) ORDER BY b, c;
```

MDAM produces three retrievals, one per `a`-value:

```
retrieval 1: (a = 1)   →  internally sorted by (b, c) because 'a' is fixed
retrieval 2: (a = 2)   →  internally sorted by (b, c)
retrieval 3: (a = 3)   →  internally sorted by (b, c)
```

Each retrieval, *taken in isolation*, is sorted by `(b, c)` — the
leading column `a` is a constant, so the index's `(a, b, c)` ordering
collapses to `(b, c)` for that scan.

A 3-way `MergeAppend` of these three sorted streams gives a globally
sorted `(b, c)` output without any `Sort` node — at cost
`tuples × log₂(3) × c`.  A `Sort + Append` would cost
`tuples × log₂(tuples) × c`.

### Why our code doesn't take advantage of it yet

When `mdam_build_append_path()` calls `create_merge_append_path_ext()`,
it passes the **index's** pathkeys:

```c
pathkeys = build_index_pathkeys(root, index, ForwardScanDirection);
   /* = (a, b, c) for index (a, b, c) */
```

These pathkeys are **wrong** for representing what the merge actually
provides.  The merge's true output ordering is `(b, c)` (the `a`
position is collapsed to a constant), not `(a, b, c)`.

The planner sees `MergeAppend` claiming `(a, b, c)` pathkeys.  When the
query asks for `ORDER BY b, c`:

* `(a, b, c)` doesn't satisfy `(b, c)` (it's not a prefix-match in
  either direction)
* So the planner can't use this MergeAppend to avoid the Sort
* It builds `Sort + Append` instead, which is more expensive but
  correct

### Why we don't emit MergeAppend at all

We considered emitting `MergeAppend` alongside `Append` and letting
`add_path()` pick the cheaper one.  We decided against it:

1. **For leading-order queries** Append already provides the requested
   ordering for free — MergeAppend would only add per-tuple heap
   overhead that loses the comparison.
2. **For unordered queries** the heap overhead is pure waste.
3. **For trailing-order queries** (Matthias's scenario above)
   MergeAppend *could* help, but only if its supplied pathkeys are
   the *stripped* suffix `(b, c)`.  Just calling
   `create_merge_append_path_ext()` with the index's full pathkeys
   `(a, b, c)` produces a node that misrepresents its output ordering
   and never gets used by upper-rel matching anyway.

The fix for case 3 (a TODO) would be:

```
for each retrieval:
   determine the leading prefix that has equality constraints
   take the smallest such prefix across all retrievals — call it K
   merge_pathkeys = index_pathkeys[K..]    /* drop first K positions */
build MergeAppend with pathkeys = merge_pathkeys
```

This is the standard equivalence-class trick the planner already uses
elsewhere (`pathkeys_useful_for_ordering`).  Until that's
implemented, `mdam_build_append_path()` just doesn't bother.

---

## 5. Selectivity correction in subpaths

Independent of the merge/append/sort choice, MDAM also affects how
subpath rows are estimated.

In a normal Append the planner sums `subpath->rows`.  But MDAM
retrievals are **disjoint by construction**, so the union of their
selectivities equals the OR-clause's overall selectivity — not the
sum-of-each-arm selectivity.

`create_append_path_ext()` accepts an explicit `Selectivity`
parameter that overrides each subpath's `rows`:

```c
if (selectivity >= 0.0)
    subpath->rows = clamp_row_est(selectivity * rel->tuples);
```

The same applies to `create_merge_append_path_ext()`.  Without this,
`Sort`'s cost estimate (which depends on `tuples`) would inflate, and
join cardinality estimates downstream would be skewed.

---

## 6. Why we don't need an explicit "Append vs MergeAppend" rule

The original concern from the design discussion was:

> *"approaching the task of correctly calculating the cost or
> introducing the rule what operator we should choose between Append,
> Merge Append, Bitmap"*

Both options produce a correct plan; the question is which is faster.
Once both costs are computed correctly, `add_path()`'s standard
keep-cheapest-per-pathkeys logic does the right thing automatically.
A separate rule would only re-implement what cost comparison already
expresses, and would have to be re-tuned every time the costing
constants change.

The two places where we *do* need explicit logic:

1. **Don't propose `MergeAppend` when its pathkeys are wrong** — see
   §4.  This is a correctness issue, not a cost issue.

2. **Don't propose paths at all if `enable_mdam` is `off`** — handled
   by the GUC check at the top of `generate_mdam_or_paths()`.

---

## 7. Summary of the current state

| Aspect | Status |
|---|---|
| `cost_append` correctly handles ordered append (sum-of-startup-costs) | ✓ in tree |
| `cost_merge_append` includes heap-merge overhead `O(tuples × log₂ N)` | ✓ in tree |
| `create_append_path_ext` selectivity-corrects subpath rows | ✓ in our changes |
| `create_merge_append_path_ext` selectivity-corrects subpath rows | ✓ in our changes |
| MDAM builds both Append and MergeAppend for forward+backward | ✓ in our changes |
| `add_path()` picks the cheapest variant for each pathkeys set | ✓ — built-in |
| MergeAppend uses *reduced* pathkeys when leading cols are equality | ✗ TODO (§4) |
| Bitmap-vs-MDAM decision | left to `add_path()`, no special rule |

Regression tests in `mdam.sql` lock in the expected plan shapes for
the leading / backward / non-leading scenarios.

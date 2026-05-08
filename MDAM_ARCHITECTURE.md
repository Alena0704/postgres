# MDAM OR-Clause Optimization — Architecture

This document describes how MDAM (Multi-Dimensional Access Method)
OR-clause optimization is split across the planner.

## Goal

Transform complex OR predicates over a single multi-column B-tree
index into an `Append`/`MergeAppend` of disjoint `IndexScan`/
`IndexOnlyScan` sub-paths.  Preserve key-space ordering without
inserting a `Sort` node.

The pipeline is split into three layers:

| Layer | File | Responsibility |
|---|---|---|
| 1. Predicate simplification | `prepqual.c` | Generic AND/OR simplification, MDAM-shape detection, marking `RestrictInfo->mdam_candidate` |
| 2. Index matching + DNF/shatter/merge | `indxpath.c` + `mdampath.c` | For each index, transform OR-arms into elementary intervals, generate non-overlapping retrieval atom-lists |
| 3. Path building | `mdampath.c` | Convert each retrieval into an `IndexClause`, build `IndexPath`s, wrap in `Append`/`MergeAppend` |

---

## Layer 1 — `prepqual.c`

Runs during `canonicalize_qual()`, **before** index information is
available.

### Transformations

- **Range intersection** in AND clauses
  (`x > 0 AND x = 5` → `x = 5`)
- **Boolean absorption** in OR clauses
  (`A OR (A AND B)` → `A`)
- **Single-bound OR absorption**
  (`x < 1 OR x < 10` → `x < 10`)
- **Range union**
  (`(x > 1 AND x < 5) OR (x > 3 AND x < 8)` → `x > 1 AND x < 8`)
- **Contradiction detection**
  (`x = 1 AND x = 2` → `false`)

### MDAM candidate marking

After `canonicalize_qual()` produces simplified clauses, each
`RestrictInfo` carries a flag:

```c
/* in pathnodes.h */
typedef struct RestrictInfo {
    ...
    bool mdam_candidate;   /* set in make_plain_restrictinfo */
};
```

The flag is set by `expr_is_mdam_candidate(Expr *)`, which performs a
**cheap structural check**:

- top-level node is OR (or AND of ORs);
- every leaf is `Var op Const`, `Const op Var`, `Var = ANY(Const array)`,
  or `Var IS [NOT] NULL`;
- no `NOT`, no volatile expressions, no row-types.

The flag is a **fast filter** for layer 2 — it does *not* guarantee
MDAM applicability (final viability is decided per-index in
layer 2).

---

## Layer 2 — `indxpath.c` + `mdampath.c`

Runs during `create_index_paths()`, with full index info.

### Entry point

```
create_index_paths()
    for each index:
        match_clauses_to_index()      // standard matching (may collapse OR→SAOP)
    generate_mdam_or_paths()          // MDAM entry — looks at mdam_candidate
```

`generate_mdam_or_paths()` walks `rel->baserestrictinfo`, collecting
RestrictInfos with `mdam_candidate = true`.  For each btree index
on the relation it tries the four-step MDAM transformation.

### The four-step pipeline

#### Step 1: DNF extraction (`mdam_extract_dnf`)

Convert the simplified OR-of-ANDs tree into a flat list of
**atom-lists**.  An atom is a single column predicate:

```c
typedef struct MdamAtom {
    int        colno;          /* index column number */
    MdamOpType op;             /* EQ, LT, LE, GT, GE, SAOP, RANGE_EXCL, IS_ANYTHING */
    Datum      value;
    Datum     *in_values; int n_in_values;
    Datum      range_lo, range_hi;
} MdamAtom;
```

Per-arm simplification (`mdam_simplify_conjunct`) intersects ranges,
folds `EQ`/`IN`, and detects contradictions.

#### Step 2: Shattering (`mdam_generate_retrievals`)

For each index column, collect **critical points** (boundary values
from the DNF) and partition the value space into elementary intervals:

```
< pt1, = pt1, (pt1, pt2), = pt2, ..., > ptN
```

Recursively enumerate all column-interval combinations.  Filter via
`mdam_retrieval_satisfies_dnf()` — keep only combinations satisfying
at least one DNF arm.

A hard cap (`MDAM_MAX_RETRIEVALS_HARD = 16 × MDAM_MAX_RETRIEVALS`)
prevents exponential blow-up.  If the cap is hit, the entire MDAM
attempt is rejected so the planner falls back to bitmap/seq paths.

#### Step 3: Merge (`mdam_merge_retrievals`)

For each column in reverse index order:

- Coalesce overlapping/adjacent intervals
- Fold `col=v1, col=v2` paths sharing a base into `col IN (v1, v2)`

#### Step 4: Expand / sort / coalesce (`mdam_expand_sort_coalesce`)

- **Expand** leading IN/range constraints into elementary intervals so
  every retrieval has point constraints on leading columns
- **Sort** retrievals by index key-space order
- **Coalesce** adjacent retrievals that became redundant

After step 4 the retrievals are guaranteed:
- non-overlapping (no duplicate rows in the Append output)
- sorted in key-space order (Append preserves index ordering)

### Ordering conflict check

Before building paths, `mdam_detect_ordering_conflict()` verifies that
each retrieval can be served by an index scan whose output sequence
matches the others.  A conflict (e.g. one retrieval has
`b < 5 AND c = 3`, another has `b > 5 AND c = 3` but the order
between rows is uncertain) aborts MDAM for that query.

---

## Layer 3 — `mdampath.c` (path building)

### `mdam_build_index_path`

For each retrieval atom-list:

- Convert each atom to an `Expr` via `mdam_atom_to_expr()`
- Wrap in an `IndexClause`
- Reuse `create_index_path()` to build a single `IndexPath` (forward
  or backward scan)

### `mdam_build_append_path`

- Wrap the retrievals' IndexPaths in an `Append` (preserves index
  pathkeys because retrievals are in key-space order)
- Optionally also build a `MergeAppend` alternative
- Mark with `is_mdam = true` on `AppendPath` and `Append` plan node

For `ORDER BY ... DESC` queries the function rebuilds each IndexPath
with `BackwardScanDirection` and reverses the retrieval list.

---

## Why this split?

1. **`prepqual.c`** runs once per query, on the raw expression tree.
   It does the cheap, index-independent simplification that benefits
   *every* path type (seq, bitmap, index, MDAM).  Marking
   `mdam_candidate` here means layer 2 doesn't re-walk the tree.

2. **`mdampath.c` step 1–2** needs the index column mapping
   (`match_index_to_operand`, opfamily, opcollation), so it must run
   in layer 2.  But the *generic* parts (DNF distribution,
   contradiction detection, interval merging) are pure data-structure
   manipulation that could in principle live elsewhere.

3. **`mdampath.c` step 3** needs `IndexOptInfo` (for `create_index_path`,
   `build_index_pathkeys`, `check_index_only`) and is naturally
   path-builder code.

The current implementation keeps steps 1–3 in `mdampath.c` for
cohesion and to track Peter Geoghegan's upstream prototype closely.
The `mdam_candidate` flag is the single point of integration with
`prepqual.c`.

---

## Configuration

| GUC | Default | Effect |
|---|---|---|
| `enable_mdam` | `on` | Enables/disables `generate_mdam_or_paths()` |

| Hard limit | Value | Purpose |
|---|---|---|
| `MDAM_MAX_RETRIEVALS` | 256 | Soft cap; results discarded if exceeded |
| `MDAM_MAX_RETRIEVALS_HARD` | 4096 (16×) | Recursion cutoff to prevent blow-up |
| `MDAM_MAX_CRITICAL_POINTS` | 64 | Per-column critical-point cap |
| `MDAM_MAX_DNF_CONJUNCTS` | 64 | DNF size cap |

---

## Open issues

- **Cost model**: the current `IndexPath`/`AppendPath` cost calculation
  doesn't account for the fact that MDAM retrievals are disjoint —
  selectivity should not be naively summed.  See `create_append_path_ext`
  which accepts an explicit `Selectivity` parameter for row correction
  in subpaths, similar to bitmap-and-node costing
  (`cost_bitmap_and_node` in `costsize.c`).  Tuning this so MDAM is
  preferred when it should be, and not when it shouldn't, is the next
  major item.

- **prepqual ↔ mdampath dedup**: there is some overlap between
  `prepqual.c`'s interval intersection (`apply_lower`/`apply_upper`)
  and `mdampath.c`'s `mdam_extract_interval`.  Both could share a
  common interval primitive in `rangeutils` (an attempt was made and
  reverted; revisit when the algorithm stabilizes upstream).

/*-------------------------------------------------------------------------
 *
 * mdampath.c
 *	  MDAM (Multi-Dimensional Access Method) OR-clause optimization for
 *	  multi-column B-tree indexes.
 *
 * Transforms complex OR predicates involving multiple columns of a single
 * B-tree index into non-overlapping index scan retrievals combined via
 * Append, preserving index key space ordering.
 *
 * Algorithm (four steps):
 *
 *   1. DNF conversion + simplification: Convert the predicate tree into
 *      OR-of-ANDs form, simplifying each conjunction (detect contradictions,
 *      intersect overlapping ranges, merge EQ/IN).
 *
 *   2. Generate initial retrievals (shattering): For each index column,
 *      collect boundary values ("critical points") from the DNF, partition
 *      each column's space into elementary intervals, enumerate all
 *      combinations, and filter to keep only paths satisfying at least
 *      one DNF clause.  Guarantees non-overlapping paths.
 *
 *   3. Merge retrievals: For each column in reverse index order, coalesce
 *      overlapping/adjacent intervals and fold col=v1, col=v2 into
 *      col IN (v1,v2).
 *
 *   4. Expand leading constraints, sort, and coalesce: Expand leading IN
 *      and range constraints into elementary intervals so each path has
 *      point constraints on leading columns (enabling key space ordering).
 *      Sort by index key space, then coalesce adjacent paths that were
 *      needlessly shattered.
 *
 * Based on the "general OR optimization" from "Efficient Search of
 * Multidimensional B-Trees".
 *
 * Future extensions (intentionally out of scope for the initial design):
 *
 *   - Mixed access methods per retrieval.  Each non-overlapping retrieval
 *     is an independent sub-problem; once disjointness is proven, any
 *     access method (btree, GIN, GiST, bitmap) can satisfy a retrieval
 *     without affecting correctness.  A branch with a text-search
 *     predicate might be better served by GIN+Sort while sibling
 *     branches use plain btree scans.  Requires decoupling "retrieval"
 *     (set of RestrictInfos + interval bounds) from IndexPath, so that
 *     per-branch path generation can reuse standard add_path machinery.
 *     Caveat: branches that cannot pipeline (e.g. Sort over Bitmap Heap)
 *     break the Append+LIMIT early-termination guarantee if they end up
 *     first in sort order, so costing must account for this.
 *
 *   - Cross-index decomposition.  Today the outer loop picks one index
 *     and decomposes against it; in principle different OR branches
 *     could be decomposed against different indexes when predicates are
 *     structurally heterogeneous.  This is a generalization of the
 *     mixed-access-methods case and has the same costing concerns.
 *
 *   - Order-losing, dedup-free decomposition.  When the caller does not
 *     require sorted output, a looser disjoint decomposition with fewer
 *     child scans may be possible while still avoiding the need for
 *     duplicate elimination.  Strictly more speculative than the
 *     ordered case; the moment dedup is required, BitmapOr is already
 *     the better tool by construction.
 *
 *   - Costing harmonization with MergeAppend/Sort work in flight.
 *     MergeAppend's per-tuple comparison cost is under-modeled today
 *     (see the thread on CAPpHfdvgAjOpLmZntLUSR_8GD=S0nCkFxC6UxQrJVOzLc7yi_Q
 *     about raising the multiplier from 2.0 to 4.0 * cpu_operator_cost
 *     and the interaction with the 1% fuzzy factor on startup_cost).
 *     Mis-costing currently biases the planner toward MergeAppend on
 *     large inputs; for MDAM specifically, the practical consequence is
 *     that the plain-Append form should be preferred whenever its
 *     disjoint-range ordering matches the required pathkeys, and the
 *     MergeAppend form reserved for the prefix-equality/suffix-ordering
 *     case where Append cannot produce the required order at all.
 *
 *   - Honest early-termination costing for ORDER BY ... LIMIT k.
 *     The main advantage of Append over BitmapOr is that later child
 *     scans stay "never executed" once k tuples have been produced.
 *     The cost model must reflect this, otherwise the planner will
 *     systematically prefer BitmapOr in cases where MDAM wins by
 *     orders of magnitude.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/optimizer/path/mdampath.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "access/stratnum.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/mdampath.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"

/* GUC variable — defined in costsize.c */

/* Collation match check -- same as in indxpath.c */
#define IndexCollMatchesExprColl(idxcollation, exprcollation) \
	((idxcollation) == InvalidOid || (idxcollation) == (exprcollation))

/* Maximum number of retrievals we'll generate before giving up */
#define MDAM_MAX_RETRIEVALS		256

/*
 * Hard limit for recursion — prevents exponential blowup but allows
 * going over MDAM_MAX_RETRIEVALS so that the top-level check can
 * detect truncation and bail out with correct results (rather than
 * silently returning a truncated set that misses some OR arms).
 */
#define MDAM_MAX_RETRIEVALS_HARD	(MDAM_MAX_RETRIEVALS * 16)

/* Maximum number of critical points per column */
#define MDAM_MAX_CRITICAL_POINTS 64

/* Maximum number of DNF conjuncts after conversion */
#define MDAM_MAX_DNF_CONJUNCTS	64

static MdamContext *mdam_init_context(PlannerInfo *root, RelOptInfo *rel,
									 IndexOptInfo *index);
static int	mdam_compare(MdamContext *ctx, int colno, Datum v1, Datum v2);
static bool mdam_datum_eq(MdamContext *ctx, int colno, Datum v1, Datum v2);
static Datum mdam_copy_datum(MdamContext *ctx, int colno, Datum val);
static MdamAtom *mdam_make_atom(MdamContext *ctx, int colno, MdamOpType op,
								Datum value);
static MdamAtom *mdam_make_atom_saop(MdamContext *ctx, int colno,
									 Datum *values, int nvalues);
static MdamAtom *mdam_make_atom_range(MdamContext *ctx, int colno,
									  Datum lo, Datum hi);
static MdamAtom *mdam_copy_atom(MdamContext *ctx, MdamAtom *src);
static List *mdam_extract_dnf(MdamContext *ctx, List *clauses);
static List *mdam_expr_to_dnf(MdamContext *ctx, Expr *expr);
static List *mdam_conjunct_from_opexpr(MdamContext *ctx, OpExpr *opexpr);
static List *mdam_conjunct_from_saop(MdamContext *ctx,
									 ScalarArrayOpExpr *saop);
static List *mdam_simplify_conjunct(MdamContext *ctx, List *atoms);
static MdamInterval *mdam_extract_interval(MdamContext *ctx, int colno,
										   List *col_atoms);
static bool mdam_point_in_interval(MdamContext *ctx, int colno,
								   Datum point, MdamInterval *iv);
static List *mdam_interval_to_atoms(MdamContext *ctx, int colno,
									MdamInterval *iv);
static List *mdam_merge_interval_list(MdamContext *ctx, int colno,
									  List *intervals);
static List *mdam_get_critical_points(MdamContext *ctx, int colno, List *dnf);
static List *mdam_generate_elementary_intervals(MdamContext *ctx, int colno,
												List *critical_points);
static List *mdam_generate_retrievals(MdamContext *ctx, List *dnf);
static void mdam_generate_recursive(MdamContext *ctx, List *orig_dnf,
									int col_idx, List *current_path,
									List **result);
static bool mdam_retrieval_satisfies_dnf(MdamContext *ctx, List *orig_dnf,
										 List *path);
static bool mdam_atoms_compatible(MdamContext *ctx, MdamAtom *dnf_atom,
								  MdamAtom *path_atom);
static List *mdam_atoms_for_col(List *path, int colno);
static List *mdam_atoms_except_col(List *path, int colno);
static List *mdam_sort_path_atoms(List *path);
static bool mdam_base_atoms_equal(MdamContext *ctx, List *a, List *b);
static List *mdam_merge_retrievals(MdamContext *ctx, List *paths);
static List *mdam_merge_eq_to_in(MdamContext *ctx, int colno, List *paths,
								 bool adjacent_only);
static int mdam_last_constrained_col(List *path);
static MdamInterval *mdam_get_path_sort_key(MdamContext *ctx, List *path);
static int	mdam_path_sort_cmp(const void *a, const void *b, void *arg);
static List *mdam_expand_sort_coalesce(MdamContext *ctx, List *dnf,
									   List *paths);
static bool mdam_detect_ordering_conflict(MdamContext *ctx, List *paths);
static Expr *mdam_atom_to_expr(MdamContext *ctx, MdamAtom *atom);
static IndexPath *mdam_build_index_path(MdamContext *ctx,
										List *retrieval_atoms,
										List *or_rinfos,
										ScanDirection scandir);
static void mdam_build_append_path(MdamContext *ctx, List *retrievals,
								   List *or_rinfos, ScanDirection scandir);

/*
 * Sort key for path ordering.  Precomputed before sort to avoid palloc inside
 * qsort comparator.
 */
typedef struct MdamSortEntry
{
	List	   *path;
	MdamInterval *key;			/* array[nkeycolumns] */
} MdamSortEntry;


/*
 * generate_mdam_or_paths
 *		Main entry point: called from create_index_paths() to generate
 *		MDAM-style Append-of-IndexScans paths for OR predicates.
 *
 * For each B-tree index on the relation, we check if the restriction
 * clauses contain OR predicates that reference multiple columns of the
 * index.  If so, we run the MDAM transformation pipeline and generate
 * an Append path with non-overlapping IndexScan sub-paths.
 */
/*
 * mdam_transform_predicates
 *		Pipeline steps 1-3: turn rel->baserestrictinfo into a list of
 *		non-overlapping retrieval atom-lists for the given index.
 *
 * This is the "predicate-only" half of MDAM — it does DNF extraction,
 * simplification, shattering, merging and expand/sort/coalesce.  No
 * Path nodes are produced; the result is just a list of atom-lists.
 *
 * Returns NIL if the predicates can't be expressed as MDAM retrievals
 * for this index (cross-type, contradictory, too many retrievals, etc).
 *
 * The caller must supply a memory context (ctx->mdam_mcxt); allocations
 * for the returned list and atoms live there.
 */
static List *
mdam_transform_predicates(MdamContext *ctx)
{
	List	   *dnf;
	List	   *initial_retrievals;
	List	   *merged;
	List	   *final_paths;

	/* Step 1: DNF + simplify */
	dnf = mdam_extract_dnf(ctx, ctx->rel->baserestrictinfo);
	if (dnf == NIL || list_length(dnf) < 2)
		return NIL;

	/* Step 2: shatter into elementary intervals */
	initial_retrievals = mdam_generate_retrievals(ctx, dnf);
	if (initial_retrievals == NIL ||
		list_length(initial_retrievals) > MDAM_MAX_RETRIEVALS)
		return NIL;

	/* Step 3: merge overlapping retrievals, fold EQ→IN */
	merged = mdam_merge_retrievals(ctx, initial_retrievals);

	/* Step 4: expand leading constraints, sort by key space, coalesce */
	final_paths = mdam_expand_sort_coalesce(ctx, dnf, merged);

	return final_paths;
}

/*
 * try_mdam_for_index
 *		Process one index: run the predicate transformation pipeline,
 *		then build IndexPath/Append/MergeAppend paths.  Adds discovered
 *		paths to rel->pathlist via add_path().
 *
 * 'or_rinfos' is the list of MDAM-candidate OR RestrictInfos collected
 * by the caller (used for IndexClause attribution).
 */
static void
try_mdam_for_index(PlannerInfo *root, RelOptInfo *rel,
				   IndexOptInfo *index, List *or_rinfos)
{
	MdamContext *ctx;
	MemoryContext old_mcxt;
	List	   *retrievals;
	List	   *bwd_pathkeys;

	check_stack_depth();

	/* Only B-tree indexes with multiple key columns */
	if (index->relam != BTREE_AM_OID)
		return;
	if (index->nkeycolumns < 2)
		return;
	/* Skip partial indexes that don't match */
	if (index->indpred != NIL && !index->predOK)
		return;

	ctx = mdam_init_context(root, rel, index);
	if (ctx == NULL)
		return;

	old_mcxt = MemoryContextSwitchTo(ctx->mdam_mcxt);

	/* Predicate-only pipeline (steps 1-4) */
	retrievals = mdam_transform_predicates(ctx);

	if (retrievals == NIL)
	{
		MemoryContextSwitchTo(old_mcxt);
		MemoryContextDelete(ctx->mdam_mcxt);
		return;
	}

	/*
	 * Path-building stage.  All Path nodes must be allocated in the outer
	 * (planner) memory context, not in ctx->mdam_mcxt which is short-lived.
	 */
	if (list_length(retrievals) == 1)
	{
		/*
		 * Single retrieval — pipeline collapsed all OR arms into one
		 * tighter range.  Emit as a plain IndexPath; no Append needed.
		 */
		IndexPath  *ipath;

		MemoryContextSwitchTo(old_mcxt);
		ipath = mdam_build_index_path(ctx,
									  (List *) linitial(retrievals),
									  or_rinfos,
									  ForwardScanDirection);
		if (ipath != NULL)
			add_path(rel, (Path *) ipath);
		return;
	}

	/* Multi-retrieval: check ordering, build Append/MergeAppend */
	if (mdam_detect_ordering_conflict(ctx, retrievals))
	{
		MemoryContextSwitchTo(old_mcxt);
		MemoryContextDelete(ctx->mdam_mcxt);
		return;
	}

	MemoryContextSwitchTo(old_mcxt);

	/* Forward Append/MergeAppend */
	mdam_build_append_path(ctx, retrievals, or_rinfos, ForwardScanDirection);

	/* Backward Append/MergeAppend, if useful */
	bwd_pathkeys = build_index_pathkeys(root, index, BackwardScanDirection);
	if (bwd_pathkeys != NIL)
		mdam_build_append_path(ctx, retrievals, or_rinfos,
							   BackwardScanDirection);

	/* Don't delete ctx->mdam_mcxt — built paths reference its contents */
}

void
generate_mdam_or_paths(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *lc;
	List	   *or_rinfos = NIL;

	if (!enable_mdam)
		return;

	/*
	 * Collect OR clauses from the restriction list.  The mdam_candidate
	 * flag is set by prepqual.c (via make_plain_restrictinfo) and acts as
	 * a fast filter — RestrictInfos that aren't structurally shaped like
	 * MDAM-eligible OR predicates are skipped without re-walking trees.
	 */
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (!rinfo->mdam_candidate)
			continue;
		if (restriction_is_or_clause(rinfo))
			or_rinfos = lappend(or_rinfos, rinfo);
	}

	if (or_rinfos == NIL)
		return;

	/* Try each index in turn */
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);

		try_mdam_for_index(root, rel, index, or_rinfos);
	}
}

/* ---------------------------------------------------
 * Context initialization and comparison utilities
 * ---------------------------------------------------
 */

/*
 * mdam_init_context
 *		Set up comparison functions and type info for each index column.
 */
static MdamContext *
mdam_init_context(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *index)
{
	MdamContext *ctx;
	MemoryContext mdam_mcxt;
	MemoryContext old_mcxt;

	mdam_mcxt = AllocSetContextCreate(CurrentMemoryContext,
									  "MDAM working context",
									  ALLOCSET_DEFAULT_SIZES);
	old_mcxt = MemoryContextSwitchTo(mdam_mcxt);

	ctx = palloc0(sizeof(MdamContext));
	ctx->root = root;
	ctx->rel = rel;
	ctx->index = index;
	ctx->nkeycolumns = index->nkeycolumns;
	ctx->mdam_mcxt = mdam_mcxt;
	ctx->col_ctx = palloc0(sizeof(MdamColContext) * index->nkeycolumns);

	for (int i = 0; i < index->nkeycolumns; i++)
	{
		MdamColContext *cc = &ctx->col_ctx[i];
		Oid			opfamily = index->opfamily[i];
		Oid			opcintype = index->opcintype[i];
		Oid			cmp_proc;

		cc->colno = i;
		cc->typid = opcintype;
		cc->collid = index->indexcollations[i];
		cc->opfamily = opfamily;

		get_typlenbyval(opcintype, &cc->typlen, &cc->typbyval);

		/* Get equality and less-than operators from the opfamily */
		cc->eq_opr = get_opfamily_member(opfamily, opcintype, opcintype,
										 BTEqualStrategyNumber);
		cc->lt_opr = get_opfamily_member(opfamily, opcintype, opcintype,
										 BTLessStrategyNumber);

		/* Get btree comparison support function */
		cmp_proc = get_opfamily_proc(opfamily, opcintype, opcintype,
									 BTORDER_PROC);
		if (!OidIsValid(cmp_proc))
		{
			/* Can't do MDAM without comparison function */
			MemoryContextSwitchTo(old_mcxt);
			MemoryContextDelete(mdam_mcxt);
			return NULL;
		}
		fmgr_info(cmp_proc, &cc->cmp_finfo);
	}

	MemoryContextSwitchTo(old_mcxt);
	return ctx;
}

/*
 * Compare two Datum values for a column. Returns <0, 0, >0.
 */
static int
mdam_compare(MdamContext *ctx, int colno, Datum v1, Datum v2)
{
	MdamColContext *cc = &ctx->col_ctx[colno];

	return DatumGetInt32(FunctionCall2Coll(&cc->cmp_finfo, cc->collid,
										   v1, v2));
}

static bool
mdam_datum_eq(MdamContext *ctx, int colno, Datum v1, Datum v2)
{
	return mdam_compare(ctx, colno, v1, v2) == 0;
}

/*
 * Copy a Datum value, handling pass-by-reference types.
 */
static Datum
mdam_copy_datum(MdamContext *ctx, int colno, Datum val)
{
	MdamColContext *cc = &ctx->col_ctx[colno];

	if (cc->typbyval)
		return val;
	return datumCopy(val, cc->typbyval, cc->typlen);
}


/* ---------------------------------------------------
 * Atom constructors
 * ---------------------------------------------------
 */
static MdamAtom *
mdam_make_atom(MdamContext *ctx, int colno, MdamOpType op, Datum value)
{
	MdamAtom   *atom = palloc0(sizeof(MdamAtom));

	atom->colno = colno;
	atom->op = op;
	if (op != MDAM_OP_IS_ANYTHING)
		atom->value = mdam_copy_datum(ctx, colno, value);
	return atom;
}

static MdamAtom *
mdam_make_atom_saop(MdamContext *ctx, int colno, Datum *values, int nvalues)
{
	MdamAtom   *atom = palloc0(sizeof(MdamAtom));

	atom->colno = colno;
	atom->op = MDAM_OP_SAOP;
	atom->in_values = palloc(sizeof(Datum) * nvalues);
	atom->n_in_values = nvalues;
	for (int i = 0; i < nvalues; i++)
		atom->in_values[i] = mdam_copy_datum(ctx, colno, values[i]);
	return atom;
}

static MdamAtom *
mdam_make_atom_range(MdamContext *ctx, int colno, Datum lo, Datum hi)
{
	MdamAtom   *atom = palloc0(sizeof(MdamAtom));

	atom->colno = colno;
	atom->op = MDAM_OP_RANGE_EXCL;
	atom->range_lo = mdam_copy_datum(ctx, colno, lo);
	atom->range_hi = mdam_copy_datum(ctx, colno, hi);
	return atom;
}

static MdamAtom *
mdam_copy_atom(MdamContext *ctx, MdamAtom *src)
{
	MdamAtom   *dst = palloc0(sizeof(MdamAtom));

	dst->colno = src->colno;
	dst->op = src->op;

	switch (src->op)
	{
		case MDAM_OP_EQ:
		case MDAM_OP_LT:
		case MDAM_OP_LE:
		case MDAM_OP_GT:
		case MDAM_OP_GE:
			dst->value = mdam_copy_datum(ctx, src->colno, src->value);
			break;
		case MDAM_OP_SAOP:
			{
				dst->in_values = palloc(sizeof(Datum) * src->n_in_values);
				dst->n_in_values = src->n_in_values;
				for (int i = 0; i < src->n_in_values; i++)
					dst->in_values[i] = mdam_copy_datum(ctx, src->colno,
														src->in_values[i]);
			}
			break;
		case MDAM_OP_RANGE_EXCL:
			dst->range_lo = mdam_copy_datum(ctx, src->colno, src->range_lo);
			dst->range_hi = mdam_copy_datum(ctx, src->colno, src->range_hi);
			break;
		case MDAM_OP_IS_ANYTHING:
			break;
	}
	return dst;
}


/* ---------------------------------------------------
 * DNF extraction from expression trees
 * ---------------------------------------------------
 */

/*
 * mdam_extract_dnf
 *		Convert restriction clauses to internal DNF representation.
 *
 * Input: list of RestrictInfo nodes from rel->baserestrictinfo.
 * Output: list of retrieval paths (each path is a List of MdamAtom*).
 * Returns NIL if the clauses can't be represented in MDAM form.
 *
 * Multiple RestrictInfo nodes are implicitly ANDed together.
 */
static List *
mdam_extract_dnf(MdamContext *ctx, List *clauses)
{
	List	   *result = NIL;
	ListCell   *lc;
	bool		found_or = false;

	/* Start with a single empty conjunct */
	result = list_make1(NIL);

	foreach(lc, clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		List	   *clause_dnf;
		List	   *new_result = NIL;
		ListCell   *lc2,
				   *lc3;

		clause_dnf = mdam_expr_to_dnf(ctx, rinfo->clause);
		if (clause_dnf == NIL)
			return NIL;			/* clause not representable */

		if (restriction_is_or_clause(rinfo))
			found_or = true;

		/*
		 * Cross-product: for each existing conjunct and each new DNF arm,
		 * merge their atoms.  This distributes AND over OR.
		 */
		foreach(lc2, result)
		{
			List	   *existing = (List *) lfirst(lc2);

			foreach(lc3, clause_dnf)
			{
				List	   *new_arm = (List *) lfirst(lc3);
				List	   *merged = list_concat_copy(existing, new_arm);
				List	   *simplified;

				/* Simplify immediately to prune contradictions early */
				simplified = mdam_simplify_conjunct(ctx, merged);
				if (simplified != NIL)
				{
					new_result = lappend(new_result, simplified);

					/* Safety check: bail if DNF blows up */
					if (list_length(new_result) > MDAM_MAX_DNF_CONJUNCTS)
						return NIL;
				}
			}
		}

		result = new_result;
		if (result == NIL)
			return NIL;			/* all conjuncts contradictory */
	}

	/* Only worth doing MDAM if there's at least one OR clause */
	if (!found_or)
		return NIL;

	return result;
}

/*
 * mdam_expr_to_dnf
 *		Convert a single expression to DNF form (list of lists of MdamAtom).
 *		Returns NIL if the expression can't be handled.
 */
static List *
mdam_expr_to_dnf(MdamContext *ctx, Expr *expr)
{
	check_stack_depth();

	if (IsA(expr, BoolExpr))
	{
		BoolExpr   *boolexpr = (BoolExpr *) expr;

		if (boolexpr->boolop == OR_EXPR)
		{
			/* OR: collect all arms */
			List	   *result = NIL;
			ListCell   *lc;

			foreach(lc, boolexpr->args)
			{
				Expr	   *arg = (Expr *) lfirst(lc);
				List	   *arm_dnf;

				/*
				 * OR args in the optimizer are wrapped in RestrictInfo.
				 */
				if (IsA(arg, RestrictInfo))
					arg = ((RestrictInfo *) arg)->clause;

				arm_dnf = mdam_expr_to_dnf(ctx, arg);
				if (arm_dnf == NIL)
					return NIL;
				result = list_concat(result, arm_dnf);

				if (list_length(result) > MDAM_MAX_DNF_CONJUNCTS)
					return NIL;
			}
			return result;
		}
		else if (boolexpr->boolop == AND_EXPR)
		{
			/* AND: cross-product the DNF of each operand */
			List	   *result = list_make1(NIL);
			ListCell   *lc;

			foreach(lc, boolexpr->args)
			{
				Expr	   *arg = (Expr *) lfirst(lc);
				List	   *arm_dnf;
				List	   *new_result = NIL;
				ListCell   *lc2,
						   *lc3;

				if (IsA(arg, RestrictInfo))
					arg = ((RestrictInfo *) arg)->clause;

				arm_dnf = mdam_expr_to_dnf(ctx, arg);
				if (arm_dnf == NIL)
					return NIL;

				foreach(lc2, result)
				{
					List	   *existing = (List *) lfirst(lc2);

					foreach(lc3, arm_dnf)
					{
						List	   *new_arm = (List *) lfirst(lc3);
						List	   *merged = list_concat_copy(existing, new_arm);
						List	   *simplified;

						simplified = mdam_simplify_conjunct(ctx, merged);
						if (simplified != NIL)
						{
							new_result = lappend(new_result, simplified);
							if (list_length(new_result) > MDAM_MAX_DNF_CONJUNCTS)
								return NIL;
						}
					}
				}

				result = new_result;
				if (result == NIL)
					return NIL;
			}
			return result;
		}
		else
		{
			/* NOT -- can't handle */
			return NIL;
		}
	}
	else if (IsA(expr, OpExpr))
	{
		List	   *atoms = mdam_conjunct_from_opexpr(ctx, (OpExpr *) expr);

		if (atoms == NIL)
			return NIL;
		return list_make1(atoms);
	}
	else if (IsA(expr, ScalarArrayOpExpr))
	{
		List	   *atoms = mdam_conjunct_from_saop(ctx,
													(ScalarArrayOpExpr *) expr);

		if (atoms == NIL)
			return NIL;
		return list_make1(atoms);
	}
	else if (IsA(expr, RestrictInfo))
	{
		return mdam_expr_to_dnf(ctx, ((RestrictInfo *) expr)->clause);
	}

	/* Can't handle this expression type */
	return NIL;
}

/*
 * mdam_conjunct_from_opexpr
 *		Convert an OpExpr (e.g. dept = 5, sdate > '1995-01-01') to a
 *		single-element list containing one MdamAtom.
 *		Returns NIL if the expression doesn't match any index column.
 */
static List *
mdam_conjunct_from_opexpr(MdamContext *ctx, OpExpr *opexpr)
{
	IndexOptInfo *index = ctx->index;
	Node	   *leftop,
			   *rightop;
	int			colno;
	Oid			opno;
	int			strategy;
	Datum		constval;
	MdamAtom   *atom;

	if (list_length(opexpr->args) != 2)
		return NIL;

	leftop = (Node *) linitial(opexpr->args);
	rightop = (Node *) lsecond(opexpr->args);
	opno = opexpr->opno;

	/* Try to match the operator to an index column */
	for (colno = 0; colno < ctx->nkeycolumns; colno++)
	{
		Oid			opfamily = index->opfamily[colno];
		bool		is_left;

		if (match_index_to_operand(leftop, colno, index) &&
			!contain_volatile_functions(rightop))
		{
			is_left = true;
		}
		else if (match_index_to_operand(rightop, colno, index) &&
				 !contain_volatile_functions(leftop))
		{
			is_left = false;
			opno = get_commutator(opno);
			if (!OidIsValid(opno))
				continue;
		}
		else
			continue;

		/* Check collation compatibility */
		if (!IndexCollMatchesExprColl(index->indexcollations[colno],
									  opexpr->inputcollid))
			continue;

		/* Check the operator is in the opfamily and get its strategy */
		strategy = get_op_opfamily_strategy(opno, opfamily);
		if (strategy == 0)
			continue;

		/* Extract constant value */
		{
			Node	   *constNode = is_left ? rightop : leftop;

			/* Strip RelabelType */
			if (IsA(constNode, RelabelType))
				constNode = (Node *) ((RelabelType *) constNode)->arg;

			if (!IsA(constNode, Const))
				return NIL;		/* non-constant, can't use for MDAM */

			if (((Const *) constNode)->constisnull)
				return NIL;		/* NULL constant, skip */

			/*
			 * Reject cross-type comparisons.  The constant type must match
			 * the index column's opcintype, otherwise we'd need to cast
			 * values when building synthesized quals, and we'd risk
			 * misinterpreting Datum representations (e.g. int8 Datum stuffed
			 * into an int4 array).
			 */
			if (exprType(constNode) != ctx->col_ctx[colno].typid)
				return NIL;

			constval = ((Const *) constNode)->constvalue;
		}

		/* Map btree strategy to MdamOpType */
		switch (strategy)
		{
			case BTEqualStrategyNumber:
				atom = mdam_make_atom(ctx, colno, MDAM_OP_EQ, constval);
				break;
			case BTLessStrategyNumber:
				atom = mdam_make_atom(ctx, colno, MDAM_OP_LT, constval);
				break;
			case BTLessEqualStrategyNumber:
				atom = mdam_make_atom(ctx, colno, MDAM_OP_LE, constval);
				break;
			case BTGreaterStrategyNumber:
				atom = mdam_make_atom(ctx, colno, MDAM_OP_GT, constval);
				break;
			case BTGreaterEqualStrategyNumber:
				atom = mdam_make_atom(ctx, colno, MDAM_OP_GE, constval);
				break;
			default:
				return NIL;		/* unknown strategy */
		}

		return list_make1(atom);
	}

	return NIL;					/* no matching index column */
}

/*
 * mdam_conjunct_from_saop
 *		Convert a ScalarArrayOpExpr (e.g. dept = ANY(ARRAY[2,4,5])) to an
 *		MdamAtom with MDAM_OP_SAOP.
 */
static List *
mdam_conjunct_from_saop(MdamContext *ctx, ScalarArrayOpExpr *saop)
{
	IndexOptInfo *index = ctx->index;
	Node	   *leftop;
	Node	   *rightop;
	int			colno;
	int			strategy;
	MdamAtom   *atom;

	if (!saop->useOr)
		return NIL;				/* ALL, not ANY */

	if (list_length(saop->args) != 2)
		return NIL;

	leftop = (Node *) linitial(saop->args);
	rightop = (Node *) lsecond(saop->args);

	for (colno = 0; colno < ctx->nkeycolumns; colno++)
	{
		Oid			opfamily = index->opfamily[colno];

		if (!match_index_to_operand(leftop, colno, index))
			continue;
		if (!IndexCollMatchesExprColl(index->indexcollations[colno],
									  saop->inputcollid))
			continue;
		strategy = get_op_opfamily_strategy(saop->opno, opfamily);
		if (strategy == 0)
			continue;

		/* Only equality SAOPs can be converted to IN */
		if (strategy != BTEqualStrategyNumber)
			return NIL;

		/* Extract the array of constants */
		if (IsA(rightop, Const))
		{
			Const	   *arrayConst = (Const *) rightop;
			ArrayType  *arrayVal;
			Datum	   *elems;
			bool	   *nulls;
			int			nelems;
			Oid			elemtype;
			int16		elemlen;
			bool		elembyval;
			char		elemalign;
			int			nvalid;
			Datum	   *valid_elems;

			if (arrayConst->constisnull)
				return NIL;

			arrayVal = DatumGetArrayTypeP(arrayConst->constvalue);
			elemtype = ARR_ELEMTYPE(arrayVal);

			get_typlenbyvalalign(elemtype, &elemlen, &elembyval, &elemalign);
			deconstruct_array(arrayVal, elemtype, elemlen, elembyval,
							  elemalign, &elems, &nulls, &nelems);

			/* Filter out NULLs */
			valid_elems = palloc(sizeof(Datum) * nelems);
			nvalid = 0;
			for (int i = 0; i < nelems; i++)
			{
				if (!nulls[i])
					valid_elems[nvalid++] = elems[i];
			}

			if (nvalid == 0)
				return NIL;

			if (nvalid == 1)
				atom = mdam_make_atom(ctx, colno, MDAM_OP_EQ, valid_elems[0]);
			else
				atom = mdam_make_atom_saop(ctx, colno, valid_elems, nvalid);

			pfree(valid_elems);
			return list_make1(atom);
		}

		/* Non-Const array (ArrayExpr with Params etc.) -- can't handle */
		return NIL;
	}

	return NIL;
}


/* ---------------------------------------------------
 * DNF simplification (Step 1 of the algorithm)
 * ---------------------------------------------------
 */

/*
 * mdam_simplify_conjunct
 *		Simplify a conjunction (list of atoms) by intersecting constraints
 *		per column, detecting contradictions, and producing a canonical form.
 *
 * Returns NIL if the conjunction is contradictory (empty result set).
 */
static List *
mdam_simplify_conjunct(MdamContext *ctx, List *atoms)
{
	List	   *result = NIL;
	List	  **col_atoms;
	ListCell   *lc;

	col_atoms = palloc0(sizeof(List *) * ctx->nkeycolumns);

	/* Group atoms by column */
	foreach(lc, atoms)
	{
		MdamAtom   *atom = (MdamAtom *) lfirst(lc);

		if (atom->op == MDAM_OP_IS_ANYTHING)
			continue;
		col_atoms[atom->colno] = lappend(col_atoms[atom->colno], atom);
	}

	/* Process each column */
	for (int i = 0; i < ctx->nkeycolumns; i++)
	{
		List	   *atoms_for_col = col_atoms[i];
		Datum	   *eq_values = NULL;
		int			n_eq = 0;
		Datum	   *in_intersection = NULL;
		int			n_in_intersection = -1; /* -1 = not yet set */
		List	   *range_atoms = NIL;

		if (atoms_for_col == NIL)
			continue;

		/* Categorize atoms into EQ, IN, and range types */
		eq_values = palloc(sizeof(Datum) * list_length(atoms_for_col));

		foreach(lc, atoms_for_col)
		{
			MdamAtom   *atom = (MdamAtom *) lfirst(lc);

			switch (atom->op)
			{
				case MDAM_OP_EQ:
					eq_values[n_eq++] = atom->value;
					break;
				case MDAM_OP_SAOP:
					if (n_in_intersection < 0)
					{
						/* First IN: take all values */
						in_intersection = palloc(sizeof(Datum) * atom->n_in_values);
						memcpy(in_intersection, atom->in_values,
							   sizeof(Datum) * atom->n_in_values);
						n_in_intersection = atom->n_in_values;
					}
					else
					{
						/* Intersect with existing IN values */
						int			k,
									new_n = 0;

						for (int j = 0; j < n_in_intersection; j++)
						{
							bool		found = false;

							for (k = 0; k < atom->n_in_values; k++)
							{
								if (mdam_datum_eq(ctx, i, in_intersection[j],
												  atom->in_values[k]))
								{
									found = true;
									break;
								}
							}
							if (found)
								in_intersection[new_n++] = in_intersection[j];
						}
						n_in_intersection = new_n;
						if (n_in_intersection == 0)
							return NIL; /* contradiction */
					}
					break;
				default:
					range_atoms = lappend(range_atoms, atom);
					break;
			}
		}

		/* Check for multiple distinct EQ values = contradiction */
		if (n_eq > 1)
		{
			for (int j = 1; j < n_eq; j++)
			{
				if (!mdam_datum_eq(ctx, i, eq_values[0], eq_values[j]))
					return NIL; /* contradiction: e.g. dept=5 AND dept=10 */
			}
		}

		/*
		 * Determine candidate point values from EQ + IN intersection.
		 */
		if (n_eq > 0 && n_in_intersection >= 0)
		{
			/* Both EQ and IN present: EQ value must be in IN set */
			bool		found = false;
			int			j;

			for (j = 0; j < n_in_intersection; j++)
			{
				if (mdam_datum_eq(ctx, i, eq_values[0], in_intersection[j]))
				{
					found = true;
					break;
				}
			}
			if (!found)
				return NIL;		/* contradiction */

			/* Result is just the single EQ value */
			n_in_intersection = -1; /* handled via EQ below */
		}

		if (n_eq > 0)
		{
			/* We have a point constraint. Check it against range atoms. */
			if (range_atoms != NIL)
			{
				MdamInterval *range_iv = mdam_extract_interval(ctx, i,
															   range_atoms);

				if (range_iv == NULL)
					return NIL; /* contradictory ranges */
				if (!mdam_point_in_interval(ctx, i, eq_values[0], range_iv))
					return NIL; /* EQ value outside range */
			}
			result = lappend(result, mdam_make_atom(ctx, i, MDAM_OP_EQ,
													eq_values[0]));
		}
		else if (n_in_intersection >= 0)
		{
			/* Filter IN values by range if present */
			if (range_atoms != NIL)
			{
				MdamInterval *range_iv = mdam_extract_interval(ctx, i,
															   range_atoms);
				int			j,
							kept = 0;

				if (range_iv == NULL)
					return NIL;

				for (j = 0; j < n_in_intersection; j++)
				{
					if (mdam_point_in_interval(ctx, i, in_intersection[j],
											   range_iv))
						in_intersection[kept++] = in_intersection[j];
				}
				n_in_intersection = kept;
			}

			if (n_in_intersection == 0)
				return NIL;		/* all IN values filtered out */

			if (n_in_intersection == 1)
				result = lappend(result,
								 mdam_make_atom(ctx, i, MDAM_OP_EQ,
												in_intersection[0]));
			else
			{
				/* Sort IN values for canonical form */
				int			j,
							k;
				Datum		tmp;

				for (j = 0; j < n_in_intersection - 1; j++)
					for (k = j + 1; k < n_in_intersection; k++)
						if (mdam_compare(ctx, i, in_intersection[j],
										 in_intersection[k]) > 0)
						{
							tmp = in_intersection[j];
							in_intersection[j] = in_intersection[k];
							in_intersection[k] = tmp;
						}

				result = lappend(result,
								 mdam_make_atom_saop(ctx, i, in_intersection,
													 n_in_intersection));
			}
		}
		else if (range_atoms != NIL)
		{
			MdamInterval *iv = mdam_extract_interval(ctx, i, range_atoms);
			List	   *iv_atoms;

			if (iv == NULL)
				return NIL;

			iv_atoms = mdam_interval_to_atoms(ctx, i, iv);
			result = list_concat(result, iv_atoms);
		}
	}

	pfree(col_atoms);
	return result;
}


/* ================================================================
 * Section 5: Interval arithmetic
 * ================================================================
 */

/*
 * mdam_extract_interval
 *		Compute the effective interval from a list of ANDed atoms on one column.
 *		Returns NULL for contradictory constraints.
 */
static MdamInterval *
mdam_extract_interval(MdamContext *ctx, int colno, List *col_atoms)
{
	MdamInterval *iv = palloc0(sizeof(MdamInterval));
	ListCell   *lc;

	iv->lo_infinite = true;
	iv->hi_infinite = true;
	iv->lo_inclusive = true;
	iv->hi_inclusive = true;

	foreach(lc, col_atoms)
	{
		MdamAtom   *atom = (MdamAtom *) lfirst(lc);
		Datum		val;
		bool		lo_inf,
					hi_inf,
					lo_incl,
					hi_incl;

		switch (atom->op)
		{
			case MDAM_OP_EQ:
				val = atom->value;
				lo_inf = hi_inf = false;
				lo_incl = hi_incl = true;
				break;
			case MDAM_OP_LT:
				val = atom->value;
				lo_inf = true;
				hi_inf = false;
				lo_incl = true;
				hi_incl = false;
				goto update_hi;
			case MDAM_OP_LE:
				val = atom->value;
				lo_inf = true;
				hi_inf = false;
				lo_incl = true;
				hi_incl = true;
				goto update_hi;
			case MDAM_OP_GT:
				val = atom->value;
				lo_inf = false;
				hi_inf = true;
				lo_incl = false;
				hi_incl = true;
				goto update_lo;
			case MDAM_OP_GE:
				val = atom->value;
				lo_inf = false;
				hi_inf = true;
				lo_incl = true;
				hi_incl = true;
				goto update_lo;
			case MDAM_OP_SAOP:
				{
					/* Treat IN as closed range [min, max] */
					Datum		min_v = atom->in_values[0];
					Datum		max_v = atom->in_values[0];
					int			j;

					for (j = 1; j < atom->n_in_values; j++)
					{
						if (mdam_compare(ctx, colno, atom->in_values[j], min_v) < 0)
							min_v = atom->in_values[j];
						if (mdam_compare(ctx, colno, atom->in_values[j], max_v) > 0)
							max_v = atom->in_values[j];
					}
					/* Update lo bound */
					if (iv->lo_infinite ||
						mdam_compare(ctx, colno, min_v, iv->lo) > 0)
					{
						iv->lo = min_v;
						iv->lo_inclusive = true;
						iv->lo_infinite = false;
					}
					else if (!iv->lo_infinite &&
							 mdam_compare(ctx, colno, min_v, iv->lo) == 0)
					{
						iv->lo_inclusive = iv->lo_inclusive && true;
					}
					/* Update hi bound */
					if (iv->hi_infinite ||
						mdam_compare(ctx, colno, max_v, iv->hi) < 0)
					{
						iv->hi = max_v;
						iv->hi_inclusive = true;
						iv->hi_infinite = false;
					}
					else if (!iv->hi_infinite &&
							 mdam_compare(ctx, colno, max_v, iv->hi) == 0)
					{
						iv->hi_inclusive = iv->hi_inclusive && true;
					}
					continue;
				}
			case MDAM_OP_RANGE_EXCL:
				/* Update lo */
				if (iv->lo_infinite ||
					mdam_compare(ctx, colno, atom->range_lo, iv->lo) > 0)
				{
					iv->lo = atom->range_lo;
					iv->lo_inclusive = false;
					iv->lo_infinite = false;
				}
				else if (!iv->lo_infinite &&
						 mdam_compare(ctx, colno, atom->range_lo, iv->lo) == 0)
				{
					iv->lo_inclusive = false;
				}
				/* Update hi */
				if (iv->hi_infinite ||
					mdam_compare(ctx, colno, atom->range_hi, iv->hi) < 0)
				{
					iv->hi = atom->range_hi;
					iv->hi_inclusive = false;
					iv->hi_infinite = false;
				}
				else if (!iv->hi_infinite &&
						 mdam_compare(ctx, colno, atom->range_hi, iv->hi) == 0)
				{
					iv->hi_inclusive = false;
				}
				goto check_empty;
			case MDAM_OP_IS_ANYTHING:
				continue;
			default:
				pfree(iv);
				return NULL;
		}

		/* EQ case: both bounds set to val */
		if (!lo_inf && !hi_inf)
		{
			/* Update both bounds for EQ */
			if (iv->lo_infinite || mdam_compare(ctx, colno, val, iv->lo) > 0)
			{
				iv->lo = val;
				iv->lo_inclusive = lo_incl;
				iv->lo_infinite = false;
			}
			else if (!iv->lo_infinite &&
					 mdam_compare(ctx, colno, val, iv->lo) == 0)
			{
				iv->lo_inclusive = iv->lo_inclusive && lo_incl;
			}

			if (iv->hi_infinite || mdam_compare(ctx, colno, val, iv->hi) < 0)
			{
				iv->hi = val;
				iv->hi_inclusive = hi_incl;
				iv->hi_infinite = false;
			}
			else if (!iv->hi_infinite &&
					 mdam_compare(ctx, colno, val, iv->hi) == 0)
			{
				iv->hi_inclusive = iv->hi_inclusive && hi_incl;
			}
			goto check_empty;
		}
		continue;

update_lo:
		if (iv->lo_infinite || mdam_compare(ctx, colno, val, iv->lo) > 0)
		{
			iv->lo = val;
			iv->lo_inclusive = lo_incl;
			iv->lo_infinite = false;
		}
		else if (!iv->lo_infinite &&
				 mdam_compare(ctx, colno, val, iv->lo) == 0)
		{
			iv->lo_inclusive = iv->lo_inclusive && lo_incl;
		}
		goto check_empty;

update_hi:
		if (iv->hi_infinite || mdam_compare(ctx, colno, val, iv->hi) < 0)
		{
			iv->hi = val;
			iv->hi_inclusive = hi_incl;
			iv->hi_infinite = false;
		}
		else if (!iv->hi_infinite &&
				 mdam_compare(ctx, colno, val, iv->hi) == 0)
		{
			iv->hi_inclusive = iv->hi_inclusive && hi_incl;
		}
		/* fall through to check_empty */

check_empty:
		/* Check for contradictions */
		if (!iv->lo_infinite && !iv->hi_infinite)
		{
			int			cmp = mdam_compare(ctx, colno, iv->lo, iv->hi);

			if (cmp > 0)
			{
				pfree(iv);
				return NULL;	/* lo > hi: empty */
			}
			if (cmp == 0 && (!iv->lo_inclusive || !iv->hi_inclusive))
			{
				pfree(iv);
				return NULL;	/* lo == hi but not both inclusive: empty */
			}
		}
	}

	return iv;
}

/*
 * Check if a point value lies within an interval.
 */
static bool
mdam_point_in_interval(MdamContext *ctx, int colno, Datum point,
					   MdamInterval *iv)
{
	if (!iv->lo_infinite)
	{
		int			cmp = mdam_compare(ctx, colno, point, iv->lo);

		if (cmp < 0 || (cmp == 0 && !iv->lo_inclusive))
			return false;
	}
	if (!iv->hi_infinite)
	{
		int			cmp = mdam_compare(ctx, colno, point, iv->hi);

		if (cmp > 0 || (cmp == 0 && !iv->hi_inclusive))
			return false;
	}
	return true;
}

/*
 * Convert an interval back to a list of MdamAtom constraints.
 */
static List *
mdam_interval_to_atoms(MdamContext *ctx, int colno, MdamInterval *iv)
{
	List	   *result = NIL;

	if (iv->lo_infinite && iv->hi_infinite)
		return NIL;				/* unconstrained */

	if (!iv->lo_infinite && !iv->hi_infinite &&
		mdam_compare(ctx, colno, iv->lo, iv->hi) == 0)
	{
		/* Point interval: EQ */
		return list_make1(mdam_make_atom(ctx, colno, MDAM_OP_EQ, iv->lo));
	}

	if (!iv->lo_infinite)
		result = lappend(result,
						 mdam_make_atom(ctx, colno,
										iv->lo_inclusive ? MDAM_OP_GE : MDAM_OP_GT,
										iv->lo));

	if (!iv->hi_infinite)
		result = lappend(result,
						 mdam_make_atom(ctx, colno,
										iv->hi_inclusive ? MDAM_OP_LE : MDAM_OP_LT,
										iv->hi));

	return result;
}

/*
 * Merge a list of intervals into a minimal sorted list of non-overlapping
 * intervals.  Input intervals should already be sorted by lower bound.
 */
static List *
mdam_merge_interval_list(MdamContext *ctx, int colno, List *intervals)
{
	List	   *merged = NIL;
	ListCell   *lc;

	foreach(lc, intervals)
	{
		MdamInterval *cur = (MdamInterval *) lfirst(lc);

		if (merged == NIL)
		{
			merged = list_make1(cur);
			continue;
		}

		{
			MdamInterval *last = (MdamInterval *) llast(merged);
			bool		can_merge;

			/*
			 * Check if current interval overlaps or is adjacent to last.
			 * "adjacent" means the end of last touches the start of current.
			 */
			if (last->hi_infinite)
				can_merge = true;
			else if (cur->lo_infinite)
				can_merge = true;
			else
			{
				int			cmp = mdam_compare(ctx, colno, cur->lo, last->hi);

				can_merge = (cmp < 0 ||
							 (cmp == 0 && (last->hi_inclusive || cur->lo_inclusive)));
			}

			if (!can_merge)
			{
				merged = lappend(merged, cur);
				continue;
			}

			/* Merge: extend last's upper bound */
			if (cur->hi_infinite)
				last->hi_infinite = true;
			else if (!last->hi_infinite)
			{
				int			cmp = mdam_compare(ctx, colno, cur->hi, last->hi);

				if (cmp > 0)
				{
					last->hi = cur->hi;
					last->hi_inclusive = cur->hi_inclusive;
				}
				else if (cmp == 0)
					last->hi_inclusive = last->hi_inclusive || cur->hi_inclusive;
			}
		}
	}

	return merged;
}


/* ---------------------------------------------------
 * Generate initial retrievals (shattering) -- Step 2
 * ---------------------------------------------------
 */

/*
 * Collect all critical (boundary) values for a column from the DNF.
 * Returns a sorted, deduplicated list of Datum values (as DatumCopy'd).
 */
static List *
mdam_get_critical_points(MdamContext *ctx, int colno, List *dnf)
{
	List	   *points = NIL;
	ListCell   *lc,
			   *lc2;
	int			ip,
				j,
				n,
				out;
	Datum	   *arr;

	/* Collect all values */
	foreach(lc, dnf)
	{
		List	   *conjunct = (List *) lfirst(lc);

		foreach(lc2, conjunct)
		{
			MdamAtom   *atom = (MdamAtom *) lfirst(lc2);

			if (atom->colno != colno)
				continue;

			switch (atom->op)
			{
				case MDAM_OP_EQ:
				case MDAM_OP_LT:
				case MDAM_OP_LE:
				case MDAM_OP_GT:
				case MDAM_OP_GE:
					points = lappend(points, (void *) (uintptr_t) atom->value);
					break;
				case MDAM_OP_SAOP:
					for (int i = 0; i < atom->n_in_values; i++)
						points = lappend(points,
										 (void *) (uintptr_t) atom->in_values[i]);
					break;
				case MDAM_OP_RANGE_EXCL:
					points = lappend(points,
									 (void *) (uintptr_t) atom->range_lo);
					points = lappend(points,
									 (void *) (uintptr_t) atom->range_hi);
					break;
				default:
					break;
			}
		}
	}

	if (points == NIL)
		return NIL;

	/* Convert to array, sort, deduplicate */
	n = list_length(points);
	arr = palloc(sizeof(Datum) * n);
	ip = 0;
	foreach(lc, points)
		arr[ip++] = (Datum) (uintptr_t) lfirst(lc);

	/* Simple insertion sort (n is small due to MDAM_MAX_CRITICAL_POINTS) */
	for (int i = 1; i < n; i++)
	{
		Datum		key = arr[i];

		j = i - 1;
		while (j >= 0 && mdam_compare(ctx, colno, arr[j], key) > 0)
		{
			arr[j + 1] = arr[j];
			j--;
		}
		arr[j + 1] = key;
	}

	/* Deduplicate */
	out = 0;
	for (int i = 0; i < n; i++)
	{
		if (out == 0 || mdam_compare(ctx, colno, arr[i], arr[out - 1]) != 0)
			arr[out++] = arr[i];
	}
	n = out;

	/* Safety limit */
	if (n > MDAM_MAX_CRITICAL_POINTS)
	{
		pfree(arr);
		return NIL;
	}

	/* Convert back to list */
	points = NIL;
	for (int i = 0; i < n; i++)
		points = lappend(points, (void *) (uintptr_t) arr[i]);

	pfree(arr);
	return points;
}

/*
 * Generate elementary intervals for a column from its critical points.
 * For critical points [a, b]: <a, =a, (a,b), =b, >b
 */
static List *
mdam_generate_elementary_intervals(MdamContext *ctx, int colno,
								   List *critical_points)
{
	List	   *intervals = NIL;
	int			npts;
	Datum	   *pts;
	ListCell   *lc;
	int			ip;

	if (critical_points == NIL)
	{
		/* No critical points: single IS_ANYTHING interval */
		MdamAtom   *atom = palloc0(sizeof(MdamAtom));

		atom->colno = colno;
		atom->op = MDAM_OP_IS_ANYTHING;
		return list_make1(atom);
	}

	npts = list_length(critical_points);
	pts = palloc(sizeof(Datum) * npts);
	ip = 0;
	foreach(lc, critical_points)
		pts[ip++] = (Datum) (uintptr_t) lfirst(lc);

	/* < first point */
	intervals = lappend(intervals,
						mdam_make_atom(ctx, colno, MDAM_OP_LT, pts[0]));

	for (int i = 0; i < npts; i++)
	{
		/* = this point */
		intervals = lappend(intervals,
							mdam_make_atom(ctx, colno, MDAM_OP_EQ, pts[i]));

		/* (this, next) exclusive range */
		if (i + 1 < npts && mdam_compare(ctx, colno, pts[i], pts[i + 1]) < 0)
		{
			intervals = lappend(intervals,
								mdam_make_atom_range(ctx, colno,
													 pts[i], pts[i + 1]));
		}
	}

	/* > last point */
	intervals = lappend(intervals,
						mdam_make_atom(ctx, colno, MDAM_OP_GT,
									   pts[npts - 1]));

	pfree(pts);
	return intervals;
}

/*
 * mdam_generate_retrievals
 *		Step 2: Generate initial non-overlapping retrieval paths by shattering
 *		the value space into elementary intervals and filtering.
 */
static List *
mdam_generate_retrievals(MdamContext *ctx, List *dnf)
{
	List	   *result = NIL;

#ifdef USE_ASSERT_CHECKING
	{
		int			col;

		for (col = 0; col < ctx->nkeycolumns; col++)
		{
			List	   *cpts = mdam_get_critical_points(ctx, col, dnf);
			List	   *eivs = mdam_generate_elementary_intervals(ctx, col, cpts);

			elog(DEBUG1, "MDAM: col %d: %d critical points, %d elementary intervals",
				 col, list_length(cpts), list_length(eivs));
		}
	}
#endif

	mdam_generate_recursive(ctx, dnf, 0, NIL, &result);
	return result;
}

/*
 * Recursive helper: enumerate combinations of elementary intervals across
 * all index columns, keeping only paths that satisfy at least one DNF clause.
 */
static void
mdam_generate_recursive(MdamContext *ctx, List *orig_dnf,
						int col_idx, List *current_path, List **result)
{
	List	   *critical_points;
	List	   *elem_intervals;
	ListCell   *lc;

	check_stack_depth();

	/* Base case: all columns processed */
	if (col_idx >= ctx->nkeycolumns)
	{
		if (mdam_retrieval_satisfies_dnf(ctx, orig_dnf, current_path))
		{
			/* Deep copy the path atoms */
			List	   *copy = NIL;
			ListCell   *lc2;

			foreach(lc2, current_path)
			{
				MdamAtom   *atom = (MdamAtom *) lfirst(lc2);

				copy = lappend(copy, mdam_copy_atom(ctx, atom));
			}
			*result = lappend(*result, copy);
		}
		return;
	}

	/*
	 * Hard limit to prevent exponential blowup.  We allow going over
	 * MDAM_MAX_RETRIEVALS so that the top-level check in
	 * generate_mdam_or_paths() can detect truncation and reject the
	 * whole set (rather than silently returning a truncated result
	 * that may miss some OR arms).
	 */
	if (list_length(*result) > MDAM_MAX_RETRIEVALS_HARD)
		return;

	critical_points = mdam_get_critical_points(ctx, col_idx, orig_dnf);
	elem_intervals = mdam_generate_elementary_intervals(ctx, col_idx,
														critical_points);

	foreach(lc, elem_intervals)
	{
		MdamAtom   *interval_atom = (MdamAtom *) lfirst(lc);
		List	   *next_path;

		if (interval_atom->op == MDAM_OP_IS_ANYTHING)
			next_path = list_copy(current_path);
		else
			next_path = lappend(list_copy(current_path), interval_atom);

		mdam_generate_recursive(ctx, orig_dnf, col_idx + 1,
								next_path, result);

		if (list_length(*result) > MDAM_MAX_RETRIEVALS_HARD)
			return;
	}
}

/*
 * Check if a retrieval path satisfies at least one DNF conjunct.
 */
static bool
mdam_retrieval_satisfies_dnf(MdamContext *ctx, List *orig_dnf, List *path)
{
	ListCell   *lc;

	foreach(lc, orig_dnf)
	{
		List	   *conjunct = (List *) lfirst(lc);
		bool		all_satisfied = true;
		ListCell   *lc2;

		foreach(lc2, conjunct)
		{
			MdamAtom   *dnf_atom = (MdamAtom *) lfirst(lc2);
			MdamAtom   *path_atom = NULL;
			ListCell   *lc3;

			/* Find the path atom for this column */
			foreach(lc3, path)
			{
				MdamAtom   *pa = (MdamAtom *) lfirst(lc3);

				if (pa->colno == dnf_atom->colno)
				{
					path_atom = pa;
					break;
				}
			}

			/* If no path atom, column is unconstrained (IS_ANYTHING) */
			if (path_atom == NULL)
			{
				/*
				 * An unconstrained column in the path is compatible with any
				 * DNF atom -- the path includes all values for this column.
				 */
				continue;
			}

			if (!mdam_atoms_compatible(ctx, dnf_atom, path_atom))
			{
				all_satisfied = false;
				break;
			}
		}

		if (all_satisfied)
			return true;
	}

	return false;
}

/*
 * Check if a path atom is compatible with a DNF atom (i.e., the path atom's
 * value space is a subset of or overlaps with the DNF atom's).
 */
static bool
mdam_atoms_compatible(MdamContext *ctx, MdamAtom *dnf_atom,
					  MdamAtom *path_atom)
{
	int			colno = dnf_atom->colno;
	List	   *both;
	MdamInterval *iv;

	Assert(colno == path_atom->colno);

	if (path_atom->op == MDAM_OP_IS_ANYTHING)
		return true;

	/* If path is EQ, check if the point satisfies the DNF atom */
	if (path_atom->op == MDAM_OP_EQ)
	{
		Datum		point = path_atom->value;

		switch (dnf_atom->op)
		{
			case MDAM_OP_EQ:
				return mdam_datum_eq(ctx, colno, point, dnf_atom->value);
			case MDAM_OP_LT:
				return mdam_compare(ctx, colno, point, dnf_atom->value) < 0;
			case MDAM_OP_LE:
				return mdam_compare(ctx, colno, point, dnf_atom->value) <= 0;
			case MDAM_OP_GT:
				return mdam_compare(ctx, colno, point, dnf_atom->value) > 0;
			case MDAM_OP_GE:
				return mdam_compare(ctx, colno, point, dnf_atom->value) >= 0;
			case MDAM_OP_SAOP:
				{
					for (int i = 0; i < dnf_atom->n_in_values; i++)
						if (mdam_datum_eq(ctx, colno, point,
										  dnf_atom->in_values[i]))
							return true;
					return false;
				}
			case MDAM_OP_RANGE_EXCL:
				return (mdam_compare(ctx, colno, point, dnf_atom->range_lo) > 0 &&
						mdam_compare(ctx, colno, point, dnf_atom->range_hi) < 0);
			case MDAM_OP_IS_ANYTHING:
				return true;
		}
		return false;
	}

	/* If DNF atom is EQ, check if the point is in the path's range */
	if (dnf_atom->op == MDAM_OP_EQ)
	{
		Datum		point = dnf_atom->value;

		switch (path_atom->op)
		{
			case MDAM_OP_EQ:
				return mdam_datum_eq(ctx, colno, point, path_atom->value);
			case MDAM_OP_LT:
				return mdam_compare(ctx, colno, point, path_atom->value) < 0;
			case MDAM_OP_LE:
				return mdam_compare(ctx, colno, point, path_atom->value) <= 0;
			case MDAM_OP_GT:
				return mdam_compare(ctx, colno, point, path_atom->value) > 0;
			case MDAM_OP_GE:
				return mdam_compare(ctx, colno, point, path_atom->value) >= 0;
			case MDAM_OP_SAOP:
				{
					for (int i = 0; i < path_atom->n_in_values; i++)
						if (mdam_datum_eq(ctx, colno, point,
										  path_atom->in_values[i]))
							return true;
					return false;
				}
			case MDAM_OP_RANGE_EXCL:
				return (mdam_compare(ctx, colno, point, path_atom->range_lo) > 0 &&
						mdam_compare(ctx, colno, point, path_atom->range_hi) < 0);
			case MDAM_OP_IS_ANYTHING:
				return true;
		}
		return false;
	}

	/* IN vs non-EQ, non-IN: check if any IN value satisfies the path atom */
	if (dnf_atom->op == MDAM_OP_SAOP && path_atom->op != MDAM_OP_SAOP)
	{
		for (int i = 0; i < dnf_atom->n_in_values; i++)
		{
			MdamAtom	tmp;

			tmp.colno = colno;
			tmp.op = MDAM_OP_EQ;
			tmp.value = dnf_atom->in_values[i];
			if (mdam_atoms_compatible(ctx, &tmp, path_atom))
				return true;
		}
		return false;
	}
	if (path_atom->op == MDAM_OP_SAOP && dnf_atom->op != MDAM_OP_SAOP)
	{
		for (int i = 0; i < path_atom->n_in_values; i++)
		{
			MdamAtom	tmp;

			tmp.colno = colno;
			tmp.op = MDAM_OP_EQ;
			tmp.value = path_atom->in_values[i];
			if (mdam_atoms_compatible(ctx, dnf_atom, &tmp))
				return true;
		}
		return false;
	}

	/* General case: use interval intersection */
	both = list_make2(dnf_atom, path_atom);
	iv = mdam_extract_interval(ctx, colno, both);

	list_free(both);
	return (iv != NULL);
}


/* ---------------------------------------------------
 * Merge retrievals -- Step 3
 * ---------------------------------------------------
 */

/*
 * Helper: get atoms for a specific column from a path.
 */
static List *
mdam_atoms_for_col(List *path, int colno)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, path)
	{
		MdamAtom   *atom = (MdamAtom *) lfirst(lc);

		if (atom->colno == colno)
			result = lappend(result, atom);
	}
	return result;
}

/*
 * Helper: get atoms for all columns EXCEPT the specified one.
 */
static List *
mdam_atoms_except_col(List *path, int colno)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, path)
	{
		MdamAtom   *atom = (MdamAtom *) lfirst(lc);

		if (atom->colno != colno)
			result = lappend(result, atom);
	}
	return result;
}

/*
 * Canonical sort key for a path: sort atoms by (colno, op, value).
 */
static List *
mdam_sort_path_atoms(List *path)
{
	/* Simple: just sort by colno since we have at most one atom per col */
	ListCell   *lc;
	int			n = list_length(path);
	MdamAtom  **arr;
	List	   *result = NIL;
	int			i;

	if (n <= 1)
		return path;

	arr = palloc(sizeof(MdamAtom *) * n);
	i = 0;
	foreach(lc, path)
		arr[i++] = (MdamAtom *) lfirst(lc);

	/* Insertion sort by colno */
	for (i = 1; i < n; i++)
	{
		MdamAtom   *key = arr[i];
		int			j = i - 1;

		while (j >= 0 && arr[j]->colno > key->colno)
		{
			arr[j + 1] = arr[j];
			j--;
		}
		arr[j + 1] = key;
	}

	for (i = 0; i < n; i++)
		result = lappend(result, arr[i]);

	pfree(arr);
	return result;
}

/*
 * Compute a "base key" for a path, excluding a specific column.
 * Two paths with the same base key can have their target column merged.
 *
 * We produce a canonical string representation for hashing/comparison.
 * (This is a simplification; a proper implementation would use a more
 * efficient key representation.)
 */
static bool
mdam_base_atoms_equal(MdamContext *ctx, List *a, List *b)
{
	ListCell   *lca,
			   *lcb;

	if (list_length(a) != list_length(b))
		return false;

	forboth(lca, a, lcb, b)
	{
		MdamAtom   *aa = (MdamAtom *) lfirst(lca);
		MdamAtom   *ab = (MdamAtom *) lfirst(lcb);

		if (aa->colno != ab->colno || aa->op != ab->op)
			return false;

		switch (aa->op)
		{
			case MDAM_OP_EQ:
			case MDAM_OP_LT:
			case MDAM_OP_LE:
			case MDAM_OP_GT:
			case MDAM_OP_GE:
				if (!mdam_datum_eq(ctx, aa->colno, aa->value, ab->value))
					return false;
				break;
			case MDAM_OP_SAOP:
				{
					if (aa->n_in_values != ab->n_in_values)
						return false;
					for (int i = 0; i < aa->n_in_values; i++)
						if (!mdam_datum_eq(ctx, aa->colno, aa->in_values[i],
										   ab->in_values[i]))
							return false;
				}
				break;
			case MDAM_OP_RANGE_EXCL:
				if (!mdam_datum_eq(ctx, aa->colno, aa->range_lo, ab->range_lo) ||
					!mdam_datum_eq(ctx, aa->colno, aa->range_hi, ab->range_hi))
					return false;
				break;
			default:
				break;
		}
	}
	return true;
}

/*
 * mdam_merge_retrievals
 *		Step 3: Merge retrieval paths by coalescing intervals and folding
 *		EQ values into IN lists.  Process columns in reverse index order.
 */
static List *
mdam_merge_retrievals(MdamContext *ctx, List *paths)
{
	for (int col = ctx->nkeycolumns - 1; col >= 0; col--)
	{
		/*
		 * Stage 1: Interval coalescing.  Group paths by their "base" (atoms
		 * on all columns except the current one), then merge intervals on the
		 * current column.
		 */
		List	   *coalesced = NIL;
		ListCell   *lc;

		/*
		 * Simple O(n^2) grouping.  For small number of paths this is fine.
		 */
		bool	   *used = palloc0(sizeof(bool) * list_length(paths));
		int			i = 0;

		foreach(lc, paths)
		{
			List	   *path = (List *) lfirst(lc);
			List	   *base;
			List	   *intervals = NIL;
			List	   *col_atoms_list;
			MdamInterval *iv;
			ListCell   *lc2;
			int			j;

			if (used[i])
			{
				i++;
				continue;
			}
			used[i] = true;

			base = mdam_sort_path_atoms(mdam_atoms_except_col(path, col));

			/* Extract interval for this path's target column */
			col_atoms_list = mdam_atoms_for_col(path, col);

			iv = mdam_extract_interval(ctx, col, col_atoms_list);
			if (iv)
				intervals = lappend(intervals, iv);

			/* Find other paths with same base */
			j = i + 1;
			for_each_from(lc2, paths, i + 1)
			{
				List	   *other = (List *) lfirst(lc2);
				List	   *other_base;

				if (used[j])
				{
					j++;
					continue;
				}

				other_base = mdam_sort_path_atoms(
												  mdam_atoms_except_col(other, col));

				if (mdam_base_atoms_equal(ctx, base, other_base))
				{
					List	   *other_col_atoms = mdam_atoms_for_col(other, col);

					iv = mdam_extract_interval(ctx, col, other_col_atoms);
					if (iv)
						intervals = lappend(intervals, iv);

					used[j] = true;
				}
				j++;
			}

			/* Merge the intervals */
			if (intervals != NIL)
			{
				List	   *merged_ivs = mdam_merge_interval_list(ctx, col,
																  intervals);
				ListCell   *lc3;

				foreach(lc3, merged_ivs)
				{
					MdamInterval *miv = (MdamInterval *) lfirst(lc3);
					List	   *iv_atoms = mdam_interval_to_atoms(ctx, col, miv);
					List	   *new_path = list_concat_copy(base, iv_atoms);

					coalesced = lappend(coalesced,
										mdam_sort_path_atoms(new_path));
				}
			}
			else
			{
				/* No intervals on this column: keep the base as-is */
				coalesced = lappend(coalesced, base);
			}

			i++;
		}

		pfree(used);
		paths = coalesced;

		/* Stage 2: EQ-to-IN merging */
		paths = mdam_merge_eq_to_in(ctx, col, paths, false);
	}

	return paths;
}

/*
 * mdam_merge_eq_to_in
 *		Merge paths that differ only in an EQ value on col into IN lists.
 */
static List *
mdam_merge_eq_to_in(MdamContext *ctx, int colno, List *paths,
					bool adjacent_only)
{
	List	   *result = NIL;
	bool	   *used;
	int			npaths = list_length(paths);
	ListCell   *lc;
	int			ip;

	used = palloc0(sizeof(bool) * npaths);

	ip = 0;
	foreach(lc, paths)
	{
		List	   *path = (List *) lfirst(lc);
		MdamAtom   *eq_atom = NULL;
		List	   *base;
		Datum	   *values;
		int			nvalues;
		ListCell   *lc2;
		int			j;
		ListCell   *alc;

		if (used[ip])
		{
			ip++;
			continue;
		}
		used[ip] = true;

		/* Find EQ atom on target column */
		foreach(alc, path)
		{
			MdamAtom   *a = (MdamAtom *) lfirst(alc);

			if (a->colno == colno && a->op == MDAM_OP_EQ)
			{
				eq_atom = a;
				break;
			}
		}

		if (eq_atom == NULL)
		{
			result = lappend(result, path);
			ip++;
			continue;
		}

		base = mdam_sort_path_atoms(mdam_atoms_except_col(path, colno));
		values = palloc(sizeof(Datum) * npaths);
		values[0] = eq_atom->value;
		nvalues = 1;

		/* Find other paths with same base and EQ on this column */
		j = ip + 1;
		for_each_from(lc2, paths, ip + 1)
		{
			List	   *other = (List *) lfirst(lc2);
			MdamAtom   *other_eq = NULL;
			List	   *other_base;

			if (used[j])
			{
				if (adjacent_only)
					break;
				j++;
				continue;
			}

			foreach(alc, other)
			{
				MdamAtom   *a = (MdamAtom *) lfirst(alc);

				if (a->colno == colno && a->op == MDAM_OP_EQ)
				{
					other_eq = a;
					break;
				}
			}

			if (other_eq == NULL)
			{
				if (adjacent_only)
					break;
				j++;
				continue;
			}

			other_base = mdam_sort_path_atoms(mdam_atoms_except_col(other, colno));

			if (mdam_base_atoms_equal(ctx, base, other_base))
			{
				values[nvalues++] = other_eq->value;
				used[j] = true;
			}
			else if (adjacent_only)
				break;

			j++;
		}

		/* Build merged atom */
		if (nvalues == 1)
		{
			result = lappend(result, path);
		}
		else
		{
			/* Sort values */
			int			k,
						l;
			Datum		tmp;
			MdamAtom   *merged_atom;
			List	   *new_path;

			for (k = 0; k < nvalues - 1; k++)
				for (l = k + 1; l < nvalues; l++)
					if (mdam_compare(ctx, colno, values[k], values[l]) > 0)
					{
						tmp = values[k];
						values[k] = values[l];
						values[l] = tmp;
					}

			merged_atom = mdam_make_atom_saop(ctx, colno, values, nvalues);
			new_path = lappend(list_copy(base), merged_atom);
			result = lappend(result, mdam_sort_path_atoms(new_path));
		}

		pfree(values);
		ip++;
	}

	pfree(used);
	return result;
}


/* ---------------------------------------------------
 * Expand, sort, coalesce -- Step 4
 * ---------------------------------------------------
 */

/*
 * Get the index of the last constrained column in a path.
 */
static int
mdam_last_constrained_col(List *path)
{
	int			last = -1;
	ListCell   *lc;

	foreach(lc, path)
	{
		MdamAtom   *atom = (MdamAtom *) lfirst(lc);

		if (atom->op != MDAM_OP_IS_ANYTHING && atom->colno > last)
			last = atom->colno;
	}
	return last;
}

/*
 * Compute a sort key for a path based on effective intervals per column.
 * Returns a palloc'd array of MdamInterval structures (one per column).
 */
static MdamInterval *
mdam_get_path_sort_key(MdamContext *ctx, List *path)
{
	MdamInterval *key = palloc0(sizeof(MdamInterval) * ctx->nkeycolumns);

	for (int i = 0; i < ctx->nkeycolumns; i++)
	{
		List	   *col_atoms = mdam_atoms_for_col(path, i);
		MdamInterval *iv = mdam_extract_interval(ctx, i, col_atoms);

		if (iv)
			key[i] = *iv;
		else
		{
			/* Default: full range */
			key[i].lo_infinite = true;
			key[i].hi_infinite = true;
		}
	}

	return key;
}

/*
 * Compare two MdamSortEntry's by precomputed sort keys
 */
static int
mdam_path_sort_cmp(const void *a, const void *b, void *arg)
{
	const		MdamSortEntry *ea = (const MdamSortEntry *) a;
	const		MdamSortEntry *eb = (const MdamSortEntry *) b;
	MdamContext *ctx = (MdamContext *) arg;

	for (int col = 0; col < ctx->nkeycolumns; col++)
	{
		const		MdamInterval *ia = &ea->key[col];
		const		MdamInterval *ib = &eb->key[col];
		int			cmp;

		/* Compare lower bounds */
		if (ia->lo_infinite && !ib->lo_infinite)
			return -1;
		if (!ia->lo_infinite && ib->lo_infinite)
			return 1;
		if (!ia->lo_infinite && !ib->lo_infinite)
		{
			cmp = mdam_compare(ctx, col, ia->lo, ib->lo);
			if (cmp != 0)
				return cmp;
			if (ia->lo_inclusive != ib->lo_inclusive)
				return ia->lo_inclusive ? -1 : 1;
		}

		/* Compare upper bounds */
		if (ia->hi_infinite && !ib->hi_infinite)
			return 1;
		if (!ia->hi_infinite && ib->hi_infinite)
			return -1;
		if (!ia->hi_infinite && !ib->hi_infinite)
		{
			cmp = mdam_compare(ctx, col, ia->hi, ib->hi);
			if (cmp != 0)
				return cmp;
			if (ia->hi_inclusive != ib->hi_inclusive)
				return ia->hi_inclusive ? 1 : -1;
		}
	}

	return 0;
}

/*
 * mdam_expand_sort_coalesce
 *		Step 4: Expand leading IN/range constraints into elementary intervals,
 *		sort by index key space, and coalesce adjacent paths.
 */
static List *
mdam_expand_sort_coalesce(MdamContext *ctx, List *dnf, List *paths)
{
	List	   *all_expanded = NIL;
	List	   *unique = NIL;
	int			np;
	MdamSortEntry *entries;
	ListCell   *lc;
	ListCell   *lc2;
	ListCell   *slc;
	int			si;
	bool		changed;

	elog(DEBUG1, "MDAM step 4a: expanding %d paths", list_length(paths));

	/*
	 * Step 4a: Expand leading constraints
	 */
	foreach(lc, paths)
	{
		List	   *path = (List *) lfirst(lc);
		List	   *current_paths = list_make1(path);
		int			col_idx;

		for (col_idx = 0; col_idx < ctx->nkeycolumns; col_idx++)
		{
			List	   *new_paths = NIL;
			ListCell   *plc;

			foreach(plc, current_paths)
			{
				List	   *p = (List *) lfirst(plc);
				MdamAtom   *atom = NULL;
				ListCell   *alc;
				int			p_last;
				List	   *col_atom_list;
				List	   *crit_pts;
				List	   *elem_ivs;
				List	   *base;

				p_last = mdam_last_constrained_col(p);
				if (col_idx >= p_last)
				{
					new_paths = lappend(new_paths, p);
					continue;
				}

				/*
				 * Collect ALL atoms for this column (there may be multiple,
				 * e.g. GE + LE forming a range).
				 */
				col_atom_list = mdam_atoms_for_col(p, col_idx);

				atom = NULL;
				if (col_atom_list != NIL)
					atom = (MdamAtom *) linitial(col_atom_list);

				base = mdam_atoms_except_col(p, col_idx);

				if (atom == NULL || atom->op == MDAM_OP_IS_ANYTHING)
				{
					/* Shatter unconstrained column */
					crit_pts = mdam_get_critical_points(ctx, col_idx, dnf);
					elem_ivs = mdam_generate_elementary_intervals(ctx, col_idx,
																  crit_pts);

					foreach(alc, elem_ivs)
					{
						MdamAtom   *ei = (MdamAtom *) lfirst(alc);
						List *atoms;

						if (ei->op == MDAM_OP_IS_ANYTHING)
						{
							if (list_length(elem_ivs) == 1)
							{
								atoms = mdam_sort_path_atoms(list_copy(base));
								new_paths = lappend(new_paths, atoms);
							}
						}
						else
						{
							atoms = mdam_sort_path_atoms(lappend(list_copy(base),
																 mdam_copy_atom(ctx, ei)));
							new_paths = lappend(new_paths, atoms);
						}
					}
				}
				else if (list_length(col_atom_list) == 1 &&
						 atom->op == MDAM_OP_SAOP)
				{
					/* Expand IN into individual EQ paths */
					int			vi;

					for (vi = 0; vi < atom->n_in_values; vi++)
					{
						MdamAtom   *eq_atom = mdam_make_atom(ctx, col_idx,
															 MDAM_OP_EQ,
															 atom->in_values[vi]);

						new_paths = lappend(new_paths,
											mdam_sort_path_atoms(
																 lappend(list_copy(base),
																		 eq_atom)));
					}
				}
				else if (list_length(col_atom_list) == 1 &&
						 atom->op == MDAM_OP_EQ)
				{
					/* Single EQ: keep as-is */
					new_paths = lappend(new_paths, p);
				}
				else
				{
					/*
					 * Range (possibly multi-atom like GE+LE). Shatter
					 * into elementary intervals, intersecting ALL atoms
					 * on this column with each interval.
					 */
					bool		shattered = false;

					crit_pts = mdam_get_critical_points(ctx, col_idx, dnf);
					elem_ivs = mdam_generate_elementary_intervals(ctx, col_idx,
																  crit_pts);

					foreach(alc, elem_ivs)
					{
						MdamAtom   *ei = (MdamAtom *) lfirst(alc);
						List	   *all_atoms;
						MdamInterval *iv;

						if (ei->op == MDAM_OP_IS_ANYTHING)
							continue;

						/* Intersect ALL column atoms with this interval */
						all_atoms = lappend(list_copy(col_atom_list), ei);
						iv = mdam_extract_interval(ctx, col_idx, all_atoms);
						list_free(all_atoms);

						if (iv != NULL &&
							!(iv->lo_infinite && iv->hi_infinite))
						{
							List	   *iv_atoms = mdam_interval_to_atoms(ctx,
																		  col_idx,
																		  iv);

							if (iv_atoms != NIL)
							{
								new_paths = lappend(new_paths,
													mdam_sort_path_atoms(
																		 list_concat(list_copy(base),
																					 iv_atoms)));
								shattered = true;
							}
						}
					}

					if (!shattered)
						new_paths = lappend(new_paths, p);
				}
			}

			current_paths = new_paths;
		}

		all_expanded = list_concat(all_expanded, current_paths);
	}

	/*
	 * Deduplicate
	 *
	 * XXX O(n^2)
	 */
	foreach(lc2, all_expanded)
	{
		List	   *p = (List *) lfirst(lc2);
		bool		dup = false;
		ListCell   *lc3;

		foreach(lc3, unique)
		{
			List	   *q = (List *) lfirst(lc3);

			if (mdam_base_atoms_equal(ctx, p, q))
			{
				dup = true;
				break;
			}
		}
		if (!dup)
			unique = lappend(unique, p);
	}
	all_expanded = unique;

	elog(DEBUG1, "MDAM step 4a done: %d expanded paths", list_length(all_expanded));

	/*
	 * Step 4b: Sort by index key space order.
	 *
	 * Precompute sort keys into a separate array, then sort with qsort_arg
	 */
	np = list_length(all_expanded);
	entries = palloc(sizeof(MdamSortEntry) * np);

	si = 0;
	foreach(slc, all_expanded)
	{
		entries[si].path = (List *) lfirst(slc);
		entries[si].key = mdam_get_path_sort_key(ctx, entries[si].path);
		si++;
	}

	qsort_arg(entries, np, sizeof(MdamSortEntry),
			  mdam_path_sort_cmp, ctx);

	all_expanded = NIL;
	for (si = 0; si < np; si++)
	{
		all_expanded = lappend(all_expanded, entries[si].path);
		pfree(entries[si].key);
	}
	pfree(entries);
	elog(DEBUG1, "MDAM step 4b done: sorted");

	/*
	 * Step 4c: Coalesce adjacent paths
	 */
	changed = true;
	while (changed)
	{
		List	   *merged_paths = NIL;
		int			npaths = list_length(all_expanded);
		bool	   *skip;

		changed = false;
		skip = palloc0(sizeof(bool) * npaths);

		for (int i = 0; i < npaths; i++)
		{
			List	   *cur_path;
			bool		did_merge = false;

			if (skip[i])
				continue;

			cur_path = (List *) list_nth(all_expanded, i);

			if (i + 1 < npaths && !skip[i + 1])
			{
				int			merge_col;

				/* Try each column as merge dimension */
				for (merge_col = 0; merge_col < ctx->nkeycolumns; merge_col++)
				{
					List	   *cur_base = mdam_sort_path_atoms(mdam_atoms_except_col(cur_path,
																					  merge_col));
					List	   *next_path = (List *) list_nth(all_expanded, i + 1);
					List	   *next_base = mdam_sort_path_atoms(mdam_atoms_except_col(next_path,
																					   merge_col));

					if (!mdam_base_atoms_equal(ctx, cur_base, next_base))
						continue;

					{
						List	   *cur_col_atoms = mdam_atoms_for_col(cur_path, merge_col);
						List	   *next_col_atoms = mdam_atoms_for_col(next_path, merge_col);
						MdamInterval *iv1 = mdam_extract_interval(ctx, merge_col, cur_col_atoms);
						MdamInterval *iv2 = mdam_extract_interval(ctx, merge_col, next_col_atoms);

						if (iv1 && iv2)
						{
							List	   *both = list_make2(iv1, iv2);
							List	   *merged_ivs = mdam_merge_interval_list(ctx, merge_col, both);

							if (list_length(merged_ivs) == 1)
							{
								MdamInterval *miv = (MdamInterval *) linitial(merged_ivs);
								List	   *new_atoms = mdam_interval_to_atoms(ctx, merge_col, miv);
								List	   *new_path = list_concat_copy(cur_base, new_atoms);

								merged_paths = lappend(merged_paths, mdam_sort_path_atoms(new_path));
								skip[i + 1] = true; /* consumed */
								did_merge = true;
								changed = true;
								break;
							}
						}
					}
				}
			}

			if (!did_merge)
				merged_paths = lappend(merged_paths, cur_path);
		}

		pfree(skip);
		all_expanded = merged_paths;
	}

	/* Final EQ-to-IN pass (adjacent only) */
	changed = true;
	while (changed)
	{
		List	   *prev = all_expanded;
		int			c;

		for (c = 0; c < ctx->nkeycolumns; c++)
			all_expanded = mdam_merge_eq_to_in(ctx, c, all_expanded,
											   true);

		changed = (list_length(all_expanded) != list_length(prev));
	}

	return all_expanded;
}


/* ---------------------------------------------------
 * Ordering conflict detection
 * ---------------------------------------------------
 */

/*
 * mdam_detect_ordering_conflict
 *		Check if the sorted retrieval paths would produce rows out of
 *		index key space order when executed as UNION ALL (Append).
 *
 * Returns true if there's an ordering conflict (paths cannot be used).
 */
static bool
mdam_detect_ordering_conflict(MdamContext *ctx, List *paths)
{
	int			npaths = list_length(paths);
	MdamInterval **sort_keys;
	ListCell   *lc;
	int			ip;

	if (npaths <= 1)
		return false;

	sort_keys = palloc(sizeof(MdamInterval *) * npaths);
	ip = 0;
	foreach(lc, paths)
	{
		sort_keys[ip++] = mdam_get_path_sort_key(ctx, (List *) lfirst(lc));
	}

	for (int i = 0; i < npaths - 1; i++)
	{
		MdamInterval *ka = sort_keys[i];
		MdamInterval *kb = sort_keys[i + 1];
		int			first_diff = -1;

		/* Find first column where the sort keys differ */
		for (int col = 0; col < ctx->nkeycolumns; col++)
		{
			bool		same = true;

			if (ka[col].lo_infinite != kb[col].lo_infinite)
				same = false;
			else if (!ka[col].lo_infinite &&
					 mdam_compare(ctx, col, ka[col].lo, kb[col].lo) != 0)
				same = false;
			else if (ka[col].lo_inclusive != kb[col].lo_inclusive)
				same = false;
			else if (ka[col].hi_infinite != kb[col].hi_infinite)
				same = false;
			else if (!ka[col].hi_infinite &&
					 mdam_compare(ctx, col, ka[col].hi, kb[col].hi) != 0)
				same = false;
			else if (ka[col].hi_inclusive != kb[col].hi_inclusive)
				same = false;

			if (!same)
			{
				first_diff = col;
				break;
			}
		}

		if (first_diff <= 0)
			continue;

		/*
		 * Check all leading columns before first_diff for non-point
		 * constraints
		 */
		for (int col = 0; col < first_diff; col++)
		{
			bool		is_point;

			is_point = (!ka[col].lo_infinite && !ka[col].hi_infinite &&
						ka[col].lo_inclusive && ka[col].hi_inclusive &&
						mdam_compare(ctx, col, ka[col].lo, ka[col].hi) == 0);

			if (!is_point)
			{
				/* Ordering conflict found */
				for (int j = 0; j < npaths; j++)
					pfree(sort_keys[j]);
				pfree(sort_keys);
				return true;
			}
		}
	}

	for (ip = 0; ip < npaths; ip++)
		pfree(sort_keys[ip]);
	pfree(sort_keys);
	return false;
}


/* ---------------------------------------------------
 * Build paths from MDAM retrievals
 * ---------------------------------------------------
 */

/*
 * Convert an MdamAtom to an expression suitable for use as an index qual.
 * Returns an OpExpr or ScalarArrayOpExpr.
 */
static Expr *
mdam_atom_to_expr(MdamContext *ctx, MdamAtom *atom)
{
	IndexOptInfo *index = ctx->index;
	MdamColContext *cc = &ctx->col_ctx[atom->colno];
	int			varattno = index->indexkeys[atom->colno];
	Oid			rel_oid = index->rel->relid;
	Var		   *indexvar;
	Const	   *constval;
	Oid			opno;

	/*
	 * Build the index key Var.  We use the index relation's attribute.
	 */
	indexvar = makeVar(rel_oid,
					   varattno,
					   cc->typid,
					   -1,		/* typmod */
					   cc->collid,
					   0);

	switch (atom->op)
	{
		case MDAM_OP_EQ:
			constval = makeConst(cc->typid, -1, cc->collid, cc->typlen,
								 atom->value, false, cc->typbyval);
			opno = cc->eq_opr;
			return make_opclause(opno, BOOLOID, false,
								 (Expr *) indexvar, (Expr *) constval,
								 InvalidOid, cc->collid);

		case MDAM_OP_LT:
			constval = makeConst(cc->typid, -1, cc->collid, cc->typlen,
								 atom->value, false, cc->typbyval);
			opno = get_opfamily_member(cc->opfamily, cc->typid, cc->typid,
									   BTLessStrategyNumber);
			return make_opclause(opno, BOOLOID, false,
								 (Expr *) indexvar, (Expr *) constval,
								 InvalidOid, cc->collid);

		case MDAM_OP_LE:
			constval = makeConst(cc->typid, -1, cc->collid, cc->typlen,
								 atom->value, false, cc->typbyval);
			opno = get_opfamily_member(cc->opfamily, cc->typid, cc->typid,
									   BTLessEqualStrategyNumber);
			return make_opclause(opno, BOOLOID, false,
								 (Expr *) indexvar, (Expr *) constval,
								 InvalidOid, cc->collid);

		case MDAM_OP_GT:
			constval = makeConst(cc->typid, -1, cc->collid, cc->typlen,
								 atom->value, false, cc->typbyval);
			opno = get_opfamily_member(cc->opfamily, cc->typid, cc->typid,
									   BTGreaterStrategyNumber);
			return make_opclause(opno, BOOLOID, false,
								 (Expr *) indexvar, (Expr *) constval,
								 InvalidOid, cc->collid);

		case MDAM_OP_GE:
			constval = makeConst(cc->typid, -1, cc->collid, cc->typlen,
								 atom->value, false, cc->typbyval);
			opno = get_opfamily_member(cc->opfamily, cc->typid, cc->typid,
									   BTGreaterEqualStrategyNumber);
			return make_opclause(opno, BOOLOID, false,
								 (Expr *) indexvar, (Expr *) constval,
								 InvalidOid, cc->collid);

		case MDAM_OP_SAOP:
			{
				/*
				 * Build a ScalarArrayOpExpr: indexvar = ANY(ARRAY[v1,v2,...])
				 */
				Oid			arraytype;
				int16		elemlen;
				bool		elembyval;
				char		elemalign;
				Datum	   *elems;
				ArrayType  *arrayVal;
				Const	   *arrayConst;
				ScalarArrayOpExpr *saop;

				arraytype = get_array_type(cc->typid);
				if (!OidIsValid(arraytype))
					return NULL;

				get_typlenbyvalalign(cc->typid, &elemlen, &elembyval,
									 &elemalign);

				elems = palloc(sizeof(Datum) * atom->n_in_values);
				for (int i = 0; i < atom->n_in_values; i++)
					elems[i] = atom->in_values[i];

				arrayVal = construct_array(elems, atom->n_in_values,
										   cc->typid, elemlen, elembyval,
										   elemalign);
				arrayConst = makeConst(arraytype, -1, cc->collid, -1,
									   PointerGetDatum(arrayVal),
									   false, false);

				saop = makeNode(ScalarArrayOpExpr);
				saop->opno = cc->eq_opr;
				saop->opfuncid = get_opcode(cc->eq_opr);
				saop->hashfuncid = InvalidOid;
				saop->negfuncid = InvalidOid;
				saop->useOr = true;
				saop->inputcollid = cc->collid;
				saop->args = list_make2(indexvar, arrayConst);
				saop->location = -1;

				pfree(elems);
				return (Expr *) saop;
			}

		case MDAM_OP_RANGE_EXCL:
			{
				/*
				 * Generate: indexvar > lo AND indexvar < hi We return just
				 * the GT part; caller handles both. Actually, we need to
				 * return both as a BoolExpr AND.
				 */
				Const	   *lo_const,
						   *hi_const;
				Expr	   *gt_expr,
						   *lt_expr;
				Oid			gt_op,
							lt_op;

				lo_const = makeConst(cc->typid, -1, cc->collid, cc->typlen,
									 atom->range_lo, false, cc->typbyval);
				hi_const = makeConst(cc->typid, -1, cc->collid, cc->typlen,
									 atom->range_hi, false, cc->typbyval);

				gt_op = get_opfamily_member(cc->opfamily, cc->typid, cc->typid,
											BTGreaterStrategyNumber);
				lt_op = get_opfamily_member(cc->opfamily, cc->typid, cc->typid,
											BTLessStrategyNumber);

				gt_expr = make_opclause(gt_op, BOOLOID, false,
										(Expr *) indexvar, (Expr *) lo_const,
										InvalidOid, cc->collid);
				lt_expr = make_opclause(lt_op, BOOLOID, false,
										(Expr *) indexvar, (Expr *) hi_const,
										InvalidOid, cc->collid);

				return makeBoolExpr(AND_EXPR, list_make2(gt_expr, lt_expr), -1);
			}

		case MDAM_OP_IS_ANYTHING:
			return NULL;		/* no qual needed */
	}

	return NULL;
}

/*
 * Build IndexClause nodes from a retrieval path's atoms and create an
 * IndexPath for the given index.
 *
 * or_rinfos: list of original OR RestrictInfo nodes that this retrieval
 * covers.  The first IndexClause's rinfo is set to the first OR
 * RestrictInfo so that is_redundant_with_indexclauses() recognizes the
 * original OR clause as handled, preventing it from appearing as a
 * redundant Filter qual.
 */
static IndexPath *
mdam_build_index_path(MdamContext *ctx, List *retrieval_atoms,
					  List *or_rinfos, ScanDirection scandir)
{
	IndexOptInfo *index = ctx->index;
	PlannerInfo *root = ctx->root;
	RelOptInfo *rel = ctx->rel;
	List	   *index_clauses = NIL;
	List	   *pathkeys;
	bool		index_only_scan;
	bool		first_clause = true;

	for (int colno = 0; colno < ctx->nkeycolumns; colno++)
	{
		ListCell   *lc;

		foreach(lc, retrieval_atoms)
		{
			MdamAtom   *atom = (MdamAtom *) lfirst(lc);
			Expr	   *expr;
			IndexClause *iclause;

			if (atom->colno != colno)
				continue;
			if (atom->op == MDAM_OP_IS_ANYTHING)
				continue;

			expr = mdam_atom_to_expr(ctx, atom);
			if (expr == NULL)
				continue;

			if (atom->op == MDAM_OP_RANGE_EXCL)
			{
				/*
				 * RANGE_EXCL generates an AND of GT and LT.  We need to
				 * create two IndexClauses, one for each bound.
				 */
				BoolExpr   *boolexpr = (BoolExpr *) expr;
				ListCell   *elc;

				foreach(elc, boolexpr->args)
				{
					Expr	   *sub = (Expr *) lfirst(elc);
					RestrictInfo *rinfo = make_simple_restrictinfo(root, sub);

					iclause = makeNode(IndexClause);
					if (first_clause && or_rinfos != NIL)
					{
						iclause->rinfo = linitial_node(RestrictInfo, or_rinfos);
						first_clause = false;
					}
					else
						iclause->rinfo = rinfo;
					iclause->indexquals = list_make1(rinfo);
					iclause->lossy = false;
					iclause->indexcol = colno;
					iclause->indexcols = NIL;
					index_clauses = lappend(index_clauses, iclause);
				}
			}
			else
			{
				RestrictInfo *rinfo = make_simple_restrictinfo(root, expr);

				iclause = makeNode(IndexClause);
				if (first_clause && or_rinfos != NIL)
				{
					iclause->rinfo = linitial_node(RestrictInfo, or_rinfos);
					first_clause = false;
				}
				else
					iclause->rinfo = rinfo;
				iclause->indexquals = list_make1(rinfo);
				iclause->lossy = false;
				iclause->indexcol = colno;
				iclause->indexcols = NIL;
				index_clauses = lappend(index_clauses, iclause);
			}
		}
	}

	if (index_clauses == NIL)
		return NULL;

	/* Compute pathkeys for the index scan */
	pathkeys = build_index_pathkeys(root, index, scandir);

	/* Check if index-only scan is possible */
	index_only_scan = check_index_only(rel, index);

	return create_index_path(root, index,
							 index_clauses,
							 NIL,	/* no ORDER BY */
							 NIL,
							 pathkeys,
							 scandir,
							 index_only_scan,
							 NULL,	/* no outer relids */
							 1.0,	/* loop_count */
							 false);	/* not partial */
}

/*
 * mdam_build_append_path
 *		Build an Append path from MDAM retrievals and add it to the relation.
 */
static void
mdam_build_append_path(MdamContext *ctx, List *retrievals, List *or_rinfos,
					   ScanDirection scandir)
{
	PlannerInfo *root = ctx->root;
	RelOptInfo *rel = ctx->rel;
	List	   *subpaths = NIL;
	ListCell   *lc;
	AppendPath *appendpath;
	List	   *pathkeys;
	AppendPathInput input;

	/*
	 * For backward scans, iterate retrievals in reverse order so that the
	 * Append produces tuples in descending index key space order.
	 */
	if (ScanDirectionIsBackward(scandir))
	{
		for (int i = list_length(retrievals) - 1; i >= 0; i--)
		{
			List	   *retrieval = (List *) list_nth(retrievals, i);
			IndexPath  *ipath = mdam_build_index_path(ctx, retrieval,
													  or_rinfos, scandir);

			if (ipath == NULL)
				return;

			subpaths = lappend(subpaths, ipath);
		}
	}
	else
	{
		foreach(lc, retrievals)
		{
			List	   *retrieval = (List *) lfirst(lc);
			IndexPath  *ipath = mdam_build_index_path(ctx, retrieval,
													  or_rinfos, scandir);

			if (ipath == NULL)
				return;

			subpaths = lappend(subpaths, ipath);
		}
	}

	if (subpaths == NIL)
		return;

	/*
	 * Each IndexPath preserves the same pathkeys (since they all use the same
	 * index in the same direction), and the retrievals are sorted in key
	 * space order.  Therefore the Append preserves the index ordering.
	 */
	pathkeys = build_index_pathkeys(root, ctx->index, scandir);

	memset(&input, 0, sizeof(input));
	input.subpaths = subpaths;
	input.partial_subpaths = NIL;
	input.child_append_relid_sets = NIL;

	appendpath = create_append_path(root, rel, input,
									pathkeys,
									NULL,	/* required_outer */
									0,	/* parallel_workers */
									false,	/* parallel_aware */
									-1);	/* rows: let it be computed */
	appendpath->is_mdam = true;

	add_path(rel, (Path *) appendpath);

	/*
	 * Also build a MergeAppend path.  While the plain Append above already
	 * preserves ordering (because MDAM guarantees key-space order), the
	 * MergeAppend gives the optimizer an alternative with different cost
	 * characteristics — particularly useful with LIMIT.
	 */
	if (pathkeys != NIL && list_length(subpaths) > 1)
	{
		MergeAppendPath *mpath;

		mpath = create_merge_append_path_ext(root, rel,
											 subpaths,
											 NIL,
											 pathkeys,
											 NULL,
											 -1);
		add_path(rel, (Path *) mpath);
	}
}

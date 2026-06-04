/*-------------------------------------------------------------------------
 *
 * plan_trace.c
 *	  Trace how the standard PostgreSQL planner forms a join plan.
 *
 *	  A standalone extension (unrelated to mcts_extreme).  Using the core
 *	  hooks join_rel_trace_hook / geqo_gen_trace_hook it records, per query:
 *
 *	    * every joinrel the join search forms -- relids, DP level (or GEQO
 *	      joinrel size), source (dp / geqo), estimated rows, cheapest cost,
 *	      the join shape (left/right sides) and whether it is bushy;
 *	    * for GEQO, the genetic search generation by generation -- the two
 *	      parents (momma, daddy), the recombined child, the child's fitness
 *	      and the pool's best fitness -- so a viewer can show what the GA does
 *	      each phase and which traits the children inherit.
 *
 *	  DP naturally explores bushy plans (a join of two multi-relation
 *	  sub-joins); GEQO can produce bushy shapes too (merge_clump joins
 *	  multi-relation clumps).  The 'bushy' flag marks those.
 *
 *	  Exposed via the SRFs plan_trace_joins() and plan_trace_geqo().
 *	  Gated by plan_trace.enabled; reset per top-level query by planner_hook.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>

#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "nodes/pathnodes.h"
#include "optimizer/geqo.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC;

void		_PG_init(void);
void		_PG_fini(void);

/* ----------
 *  State
 * ----------
 */
static bool plan_trace_enabled = false;

static join_rel_trace_hook_type prev_join_rel_trace_hook = NULL;
static geqo_gen_trace_hook_type prev_geqo_gen_trace_hook = NULL;

/*
 * Buffers live in a dedicated long-lived context so the SRFs can read the most
 * recent query's trace from a later query.  We must NOT reset per-planning
 * (planning the SRF's own SELECT would wipe the data first); instead the SRFs
 * mark the buffer "consumed", and the first hook call of the next traced query
 * resets it.  A plain "SELECT * FROM plan_trace_joins()" is a function scan
 * with no join search, so it never fires the hooks and never resets.
 */
static MemoryContext plan_trace_cxt = NULL;
static List *join_rows = NIL;
static List *geqo_rows = NIL;
static int	join_next_id = 0;
static bool plan_trace_consumed = true;

static void
plan_trace_maybe_reset(void)
{
	if (!plan_trace_consumed)
		return;
	if (plan_trace_cxt == NULL)
		plan_trace_cxt = AllocSetContextCreate(TopMemoryContext, "plan_trace",
											   ALLOCSET_SMALL_SIZES);
	else
		MemoryContextReset(plan_trace_cxt);
	join_rows = NIL;
	geqo_rows = NIL;
	join_next_id = 0;
	plan_trace_consumed = false;
}

#define PLAN_TRACE_MAX 200000		/* runaway guard (GEQO candidates are many) */

typedef struct JoinRow
{
	int			id;
	char	   *relids;
	int			level;
	int			source;			/* 0 = dp, 1 = geqo */
	double		est_rows;
	double		cost;			/* cheapest_total_path cost, -1 if none */
	char	   *left;			/* outer side of the cheapest top join */
	char	   *right;			/* inner side */
	bool		bushy;
	int			eval_seq;		/* GEQO tour eval id (groups a tour's joinrels) */
} JoinRow;

typedef struct GeqoRow
{
	int			generation;
	char	   *momma;
	char	   *daddy;
	char	   *kid;
	double		kid_worth;
	double		best_worth;
} GeqoRow;

/* ----------
 *  Helpers
 * ----------
 */

/* Render a Relids set as space-separated relation aliases. */
static char *
relids_text(PlannerInfo *root, Relids relids)
{
	StringInfoData buf;
	int			x = -1;
	bool		first = true;

	if (relids == NULL)
		return pstrdup("");
	initStringInfo(&buf);
	while ((x = bms_next_member(relids, x)) >= 0)
	{
		const char *name = NULL;

		if (root != NULL && x > 0 && x < root->simple_rel_array_size &&
			root->simple_rte_array[x] != NULL &&
			root->simple_rte_array[x]->eref != NULL)
			name = root->simple_rte_array[x]->eref->aliasname;
		if (!first)
			appendStringInfoChar(&buf, ' ');
		first = false;
		if (name != NULL)
			appendStringInfoString(&buf, name);
		else
			appendStringInfo(&buf, "rel%d", x);
	}
	return buf.data;
}

/* Render a gene tour (RT indices) as space-separated aliases. */
static char *
tour_text(PlannerInfo *root, const Gene *tour, int n)
{
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);
	for (i = 0; i < n; i++)
	{
		int			x = tour[i];
		const char *name = NULL;

		if (root != NULL && x > 0 && x < root->simple_rel_array_size &&
			root->simple_rte_array[x] != NULL &&
			root->simple_rte_array[x]->eref != NULL)
			name = root->simple_rte_array[x]->eref->aliasname;
		if (i > 0)
			appendStringInfoChar(&buf, ' ');
		if (name != NULL)
			appendStringInfoString(&buf, name);
		else
			appendStringInfo(&buf, "rel%d", x);
	}
	return buf.data;
}

/*
 * Unwrap common single-child wrappers (Gather, projection, sort, material,
 * memoize, ...) to find the underlying join path, and return its two input
 * relids.  Returns true and sets l, r and bushy when a join is found.
 */
static bool
join_sides(Path *path, Relids *l, Relids *r, bool *bushy)
{
	int			guard = 0;

	while (path != NULL && guard++ < 16)
	{
		switch (nodeTag(path))
		{
			case T_NestPath:
			case T_MergePath:
			case T_HashPath:
				{
					JoinPath   *jp = (JoinPath *) path;
					Relids		lo = jp->outerjoinpath->parent->relids;
					Relids		ri = jp->innerjoinpath->parent->relids;

					*l = lo;
					*r = ri;
					*bushy = (bms_membership(lo) == BMS_MULTIPLE &&
							  bms_membership(ri) == BMS_MULTIPLE);
					return true;
				}
			case T_GatherPath:
				path = ((GatherPath *) path)->subpath;
				break;
			case T_GatherMergePath:
				path = ((GatherMergePath *) path)->subpath;
				break;
			case T_ProjectionPath:
				path = ((ProjectionPath *) path)->subpath;
				break;
			case T_SortPath:
				path = ((SortPath *) path)->subpath;
				break;
			case T_IncrementalSortPath:
				path = ((IncrementalSortPath *) path)->spath.subpath;
				break;
			case T_MaterialPath:
				path = ((MaterialPath *) path)->subpath;
				break;
			case T_MemoizePath:
				path = ((MemoizePath *) path)->subpath;
				break;
			default:
				return false;
		}
	}
	return false;
}

/* ----------
 *  Recording hooks
 * ----------
 */
static void
plan_trace_join_hook(PlannerInfo *root, RelOptInfo *joinrel, int level,
					 int source)
{
	MemoryContext old;
	JoinRow    *jr;
	Relids		l = NULL;
	Relids		r = NULL;
	bool		bushy = false;
	Path	   *cp;

	if (prev_join_rel_trace_hook)
		prev_join_rel_trace_hook(root, joinrel, level, source);

	if (!plan_trace_enabled)
		return;

	/*
	 * GEQO evaluates many tours.  The winning tour (geqo_tracing_final) is
	 * recorded as source 1.  Every other evaluated tour's joinrels are
	 * recorded as candidates (source 2), so the visualizer can show the whole
	 * explored search space as a candidate lattice -- like the MCTS search
	 * view -- with the winning tour highlighted on top.
	 */
	if (source == 1 && !geqo_tracing_final)
		source = 2;

	plan_trace_maybe_reset();
	if (list_length(join_rows) >= PLAN_TRACE_MAX)
		return;

	cp = joinrel->cheapest_total_path;
	if (cp != NULL)
		(void) join_sides(cp, &l, &r, &bushy);

	old = MemoryContextSwitchTo(plan_trace_cxt);
	jr = (JoinRow *) palloc0(sizeof(JoinRow));
	jr->id = join_next_id++;
	jr->relids = relids_text(root, joinrel->relids);
	jr->level = level;
	jr->source = source;
	jr->est_rows = joinrel->rows;
	jr->cost = cp ? (double) cp->total_cost : -1.0;
	jr->left = l ? relids_text(root, l) : NULL;
	jr->right = r ? relids_text(root, r) : NULL;
	jr->bushy = bushy;
	jr->eval_seq = geqo_eval_seq;	/* meaningful for GEQO candidates */
	join_rows = lappend(join_rows, jr);
	MemoryContextSwitchTo(old);
}

static void
plan_trace_geqo_hook(PlannerInfo *root, int generation,
					 const Gene *momma, const Gene *daddy, const Gene *kid,
					 int num_gene, double kid_worth, double best_worth)
{
	MemoryContext old;
	GeqoRow    *gr;

	if (prev_geqo_gen_trace_hook)
		prev_geqo_gen_trace_hook(root, generation, momma, daddy, kid,
								 num_gene, kid_worth, best_worth);

	if (!plan_trace_enabled)
		return;
	plan_trace_maybe_reset();
	if (list_length(geqo_rows) >= PLAN_TRACE_MAX)
		return;

	old = MemoryContextSwitchTo(plan_trace_cxt);
	gr = (GeqoRow *) palloc0(sizeof(GeqoRow));
	gr->generation = generation;
	gr->momma = tour_text(root, momma, num_gene);
	gr->daddy = tour_text(root, daddy, num_gene);
	gr->kid = tour_text(root, kid, num_gene);
	gr->kid_worth = kid_worth;
	gr->best_worth = best_worth;
	geqo_rows = lappend(geqo_rows, gr);
	MemoryContextSwitchTo(old);
}

/* ----------
 *  SRFs
 * ----------
 */
PG_FUNCTION_INFO_V1(plan_trace_joins);

Datum
plan_trace_joins(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ListCell   *lc;

	InitMaterializedSRF(fcinfo, 0);

	foreach(lc, join_rows)
	{
		JoinRow    *n = (JoinRow *) lfirst(lc);
		Datum		v[10];
		bool		nulls[10];

		memset(nulls, false, sizeof(nulls));
		v[0] = Int32GetDatum(n->id);
		v[1] = n->relids ? CStringGetTextDatum(n->relids) : (nulls[1] = true, (Datum) 0);
		v[2] = Int32GetDatum(n->level);
		v[3] = CStringGetTextDatum(n->source == 0 ? "dp" :
								   n->source == 1 ? "geqo" : "geqo_cand");
		v[4] = Float8GetDatum(n->est_rows);
		if (n->cost < 0)
			nulls[5] = true;
		else
			v[5] = Float8GetDatum(n->cost);
		if (n->left)
			v[6] = CStringGetTextDatum(n->left);
		else
			nulls[6] = true;
		if (n->right)
			v[7] = CStringGetTextDatum(n->right);
		else
			nulls[7] = true;
		v[8] = BoolGetDatum(n->bushy);
		v[9] = Int32GetDatum(n->eval_seq);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, nulls);
	}
	plan_trace_consumed = true;		/* next traced query starts fresh */
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(plan_trace_geqo);

Datum
plan_trace_geqo(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ListCell   *lc;

	InitMaterializedSRF(fcinfo, 0);

	foreach(lc, geqo_rows)
	{
		GeqoRow    *n = (GeqoRow *) lfirst(lc);
		Datum		v[6];
		bool		nulls[6];

		memset(nulls, false, sizeof(nulls));
		v[0] = Int32GetDatum(n->generation);
		v[1] = CStringGetTextDatum(n->momma);
		v[2] = CStringGetTextDatum(n->daddy);
		v[3] = CStringGetTextDatum(n->kid);
		v[4] = Float8GetDatum(n->kid_worth);
		v[5] = Float8GetDatum(n->best_worth);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, nulls);
	}
	plan_trace_consumed = true;
	return (Datum) 0;
}

/* ----------
 *  Init
 * ----------
 */
void
_PG_init(void)
{
	DefineCustomBoolVariable("plan_trace.enabled",
							 "Record how the standard planner (DP/GEQO) forms the plan",
							 "When on, plan_trace_joins() and plan_trace_geqo() "
							 "return the join-formation and GEQO-generation trace "
							 "of the most recently planned query.",
							 &plan_trace_enabled,
							 false,
							 PGC_USERSET,
							 0, NULL, NULL, NULL);
	MarkGUCPrefixReserved("plan_trace");

	prev_join_rel_trace_hook = join_rel_trace_hook;
	join_rel_trace_hook = plan_trace_join_hook;
	prev_geqo_gen_trace_hook = geqo_gen_trace_hook;
	geqo_gen_trace_hook = plan_trace_geqo_hook;
}

void
_PG_fini(void)
{
	join_rel_trace_hook = prev_join_rel_trace_hook;
	geqo_gen_trace_hook = prev_geqo_gen_trace_hook;
}

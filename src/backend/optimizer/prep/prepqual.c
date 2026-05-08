/*-------------------------------------------------------------------------
 *
 * prepqual.c
 *	  Routines for preprocessing qualification expressions
 *
 *
 * While the parser will produce flattened (N-argument) AND/OR trees from
 * simple sequences of AND'ed or OR'ed clauses, there might be an AND clause
 * directly underneath another AND, or OR underneath OR, if the input was
 * oddly parenthesized.  Also, rule expansion and subquery flattening could
 * produce such parsetrees.  The planner wants to flatten all such cases
 * to ensure consistent optimization behavior.
 *
 * Formerly, this module was responsible for doing the initial flattening,
 * but now we leave it to eval_const_expressions to do that since it has to
 * make a complete pass over the expression tree anyway.  Instead, we just
 * have to ensure that our manipulations preserve AND/OR flatness.
 * pull_ands() and pull_ors() are used to maintain flatness of the AND/OR
 * tree after local transformations that might introduce nested AND/ORs.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepqual.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/cmptype.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/stratnum.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "utils/array.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "utils/catcache.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static List *pull_ands(List *andlist);
static List *pull_ors(List *orlist);
static Expr *find_duplicate_ors(Expr *qual, bool is_check);
static Expr *process_duplicate_ors(List *orlist);
static Oid	get_btree_opfamily(Oid opno);

/*
 * Types we allow for range simplification (btree, same-type comparison safe).
 * Geometry (point, box, polygon, etc.) and other non-scalar types are excluded
 * because they lack a total ordering suitable for range intersection/union.
 */
static const Oid range_simplification_safe_types[] = {
	BOOLOID, INT8OID, INT2OID, INT4OID, OIDOID,
	FLOAT4OID, FLOAT8OID, MONEYOID,
	DATEOID, TIMEOID, TIMESTAMPOID, TIMESTAMPTZOID,
	INTERVALOID, TIMETZOID, NUMERICOID,
	InvalidOid					/* sentinel */
};

static bool
is_safe_type_for_range_simplification(Oid typid)
{
	const Oid   *p = range_simplification_safe_types;

	for (; *p != InvalidOid; p++)
		if (*p == typid)
			return true;
	return false;
}

/*
 * If the qual contains any operator that is not in a btree opfamily, or any
 * Var/Const of a type we don't allow (e.g. geometry), we skip range
 * simplification to avoid coredumps.
 */
static bool
qual_contains_non_btree_operator_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;
		Oid			opfamily = get_btree_opfamily(op->opno);

		if (!OidIsValid(opfamily))
		{
			/*
			 * Allow "<>" — it's not in any btree opfamily but its negator
			 * (=) is, and try_or_neq_simplification() can fold patterns
			 * like (x <> V OR x = V) => TRUE.
			 */
			Oid			negator = get_negator(op->opno);

			if (OidIsValid(negator) &&
				OidIsValid(get_btree_opfamily(negator)))
			{
				/* OK, it's <> */
			}
			else
				return true;	/* not btree: skip simplification */
		}
	}
	if (IsA(node, Var))
	{
		if (!is_safe_type_for_range_simplification(((Var *) node)->vartype))
			return true;
	}
	if (IsA(node, Const))
	{
		Oid			ctype = ((Const *) node)->consttype;
		Oid			elemtype;

		if (is_safe_type_for_range_simplification(ctype))
			/* OK */ ;
		else if (OidIsValid(elemtype = get_element_type(ctype)) &&
				 is_safe_type_for_range_simplification(elemtype))
			/* Safe array (used in IN-lists) — OK */ ;
		else
			return true;
	}
	return expression_tree_walker(node, qual_contains_non_btree_operator_walker, context);
}

static bool
qual_contains_unsupported_op_or_type(Expr *qual)
{
	return qual && qual_contains_non_btree_operator_walker((Node *) qual, NULL);
}

static bool
qual_contains_not_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, BoolExpr) && ((BoolExpr *) node)->boolop == NOT_EXPR)
		return true;
	return expression_tree_walker(node, qual_contains_not_walker, context);
}

static bool
qual_contains_not(Expr *qual)
{
	return qual && qual_contains_not_walker((Node *) qual, NULL);
}

/* Max AND/OR nesting depth we allow; avoids stack overflow and edge-case crashes */
#define MAX_QUAL_SIMPLIFY_DEPTH  100

static int
qual_and_or_depth_walker(Node *node, int depth)
{
	if (node == NULL)
		return depth;
	if (is_andclause((Expr *) node) || is_orclause((Expr *) node))
	{
		ListCell   *lc;
		int			maxchild = depth + 1;

		foreach(lc, ((BoolExpr *) node)->args)
		{
			int			d = qual_and_or_depth_walker((Node *) lfirst(lc), depth + 1);

			if (d > maxchild)
				maxchild = d;
		}
		return maxchild;
	}
	return depth;
}

static bool
qual_too_deep_to_simplify(Expr *qual)
{
	return qual_and_or_depth_walker((Node *) qual, 0) > MAX_QUAL_SIMPLIFY_DEPTH;
}

/* Avoid huge AND/OR lists that can cause long loops or excessive memory use */
#define MAX_QUAL_SIMPLIFY_TOPLEVEL_ARGS  500

static bool
qual_too_large_to_simplify(Expr *qual)
{
	if (qual != NULL && IsA(qual, BoolExpr))
		return list_length(((BoolExpr *) qual)->args) > MAX_QUAL_SIMPLIFY_TOPLEVEL_ARGS;
	return false;
}

/*
 * Range simplifier: intersect AND'ed inequalities on the same Var
 * (immutable, btree, Var OP Const only).  Key per Var: (var identity, opfamily, collation).
 */
typedef struct VarRangeKey
{
	int			varno;
	AttrNumber	varattno;
	Oid			vartype;
	int32		vartypmod;
	Oid			varcollid;
	Index		varlevelsup;
	Oid			opfamily;
	Oid			collation;
} VarRangeKey;

typedef struct VarRange
{
	VarRangeKey	key;			/* copy of key for this range */

	Const	   *lower;
	bool		lower_inc;

	Const	   *upper;
	bool		upper_inc;

	Const	   *equal;			/* equality shortcut */

	bool		empty;

	Var		   *var;			/* representative Var for building clauses */
} VarRange;

/* Unified boolean simplifier (recursive AND/OR + range intersection/union) */
static Expr *simplify_boolean_tree(Expr *expr, bool is_check);
static Expr *simplify_and_clause(BoolExpr *andexpr, bool is_check);
static Expr *simplify_or_clause(BoolExpr *orexpr, bool is_check);
static List *try_range_intersection(List *andlist);
static Expr *try_or_absorption_boolean(List *orlist);
static Expr *try_or_absorption(List *orlist);
static Expr *try_range_union(List *orlist);

/* OR interval union: extract range per arm, merge overlapping, single interval only */
static bool extract_range_from_expr(Expr *expr, VarRange *out);
static List *extract_ranges_from_expr(Expr *expr);
static Expr *try_or_neq_simplification(List *orlist);
static bool build_range_from_and(BoolExpr *andexpr, VarRange *out);
static bool build_range_from_op(Expr *opexpr, VarRange *out);
static VarRange *copy_var_range(VarRange *r);
static bool same_var_and_family(VarRange *r1, VarRange *r2);
static int	range_lower_cmp(const ListCell *a, const ListCell *b);
static bool ranges_overlap_or_touch(VarRange *a, VarRange *b);
static void union_two_ranges(VarRange *a, VarRange *b, VarRange *out);
static void merge_ranges(List *ranges, VarRange *out, bool *single_interval);
static Expr *build_and_from_range(VarRange *r);
/* Defensive: merged must be a valid superset of the union of ranges. */
static bool range_union_sanity_check(List *ranges, VarRange *merged);

static bool is_const_true(Expr *expr, bool is_check);
static bool is_const_false(Expr *expr, bool is_check);


/*
 * negate_clause
 *	  Negate a Boolean expression.
 *
 * Input is a clause to be negated (e.g., the argument of a NOT clause).
 * Returns a new clause equivalent to the negation of the given clause.
 *
 * Although this can be invoked on its own, it's mainly intended as a helper
 * for eval_const_expressions(), and that context drives several design
 * decisions.  In particular, if the input is already AND/OR flat, we must
 * preserve that property.  We also don't bother to recurse in situations
 * where we can assume that lower-level executions of eval_const_expressions
 * would already have simplified sub-clauses of the input.
 *
 * The difference between this and a simple make_notclause() is that this
 * tries to get rid of the NOT node by logical simplification.  It's clearly
 * always a win if the NOT node can be eliminated altogether.  However, our
 * use of DeMorgan's laws could result in having more NOT nodes rather than
 * fewer.  We do that unconditionally anyway, because in WHERE clauses it's
 * important to expose as much top-level AND/OR structure as possible.
 * Also, eliminating an intermediate NOT may allow us to flatten two levels
 * of AND or OR together that we couldn't have otherwise.  Finally, one of
 * the motivations for doing this is to ensure that logically equivalent
 * expressions will be seen as physically equal(), so we should always apply
 * the same transformations.
 */
Node *
negate_clause(Node *node)
{
	if (node == NULL)			/* should not happen */
		elog(ERROR, "can't negate an empty subexpression");
	switch (nodeTag(node))
	{
		case T_Const:
			{
				Const	   *c = (Const *) node;

				/* NOT NULL is still NULL */
				if (c->constisnull)
					return makeBoolConst(false, true);
				/* otherwise pretty easy */
				return makeBoolConst(!DatumGetBool(c->constvalue), false);
			}
			break;
		case T_OpExpr:
			{
				/*
				 * Negate operator if possible: (NOT (< A B)) => (>= A B)
				 */
				OpExpr	   *opexpr = (OpExpr *) node;
				Oid			negator = get_negator(opexpr->opno);

				if (negator)
				{
					OpExpr	   *newopexpr = makeNode(OpExpr);

					newopexpr->opno = negator;
					newopexpr->opfuncid = InvalidOid;
					newopexpr->opresulttype = opexpr->opresulttype;
					newopexpr->opretset = opexpr->opretset;
					newopexpr->opcollid = opexpr->opcollid;
					newopexpr->inputcollid = opexpr->inputcollid;
					newopexpr->args = opexpr->args;
					newopexpr->location = opexpr->location;
					return (Node *) newopexpr;
				}
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				/*
				 * Negate a ScalarArrayOpExpr if its operator has a negator;
				 * for example x = ANY (list) becomes x <> ALL (list)
				 */
				ScalarArrayOpExpr *saopexpr = (ScalarArrayOpExpr *) node;
				Oid			negator = get_negator(saopexpr->opno);

				if (negator)
				{
					ScalarArrayOpExpr *newopexpr = makeNode(ScalarArrayOpExpr);

					newopexpr->opno = negator;
					newopexpr->opfuncid = InvalidOid;
					newopexpr->hashfuncid = InvalidOid;
					newopexpr->negfuncid = InvalidOid;
					newopexpr->useOr = !saopexpr->useOr;
					newopexpr->inputcollid = saopexpr->inputcollid;
					newopexpr->args = saopexpr->args;
					newopexpr->location = saopexpr->location;
					return (Node *) newopexpr;
				}
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				switch (expr->boolop)
				{
						/*--------------------
						 * Apply DeMorgan's Laws:
						 *		(NOT (AND A B)) => (OR (NOT A) (NOT B))
						 *		(NOT (OR A B))	=> (AND (NOT A) (NOT B))
						 * i.e., swap AND for OR and negate each subclause.
						 *
						 * If the input is already AND/OR flat and has no NOT
						 * directly above AND or OR, this transformation preserves
						 * those properties.  For example, if no direct child of
						 * the given AND clause is an AND or a NOT-above-OR, then
						 * the recursive calls of negate_clause() can't return any
						 * OR clauses.  So we needn't call pull_ors() before
						 * building a new OR clause.  Similarly for the OR case.
						 *--------------------
						 */
					case AND_EXPR:
						{
							List	   *nargs = NIL;
							ListCell   *lc;

							foreach(lc, expr->args)
							{
								nargs = lappend(nargs,
												negate_clause(lfirst(lc)));
							}
							return (Node *) make_orclause(nargs);
						}
						break;
					case OR_EXPR:
						{
							List	   *nargs = NIL;
							ListCell   *lc;

							foreach(lc, expr->args)
							{
								nargs = lappend(nargs,
												negate_clause(lfirst(lc)));
							}
							return (Node *) make_andclause(nargs);
						}
						break;
					case NOT_EXPR:

						/*
						 * NOT underneath NOT: they cancel.  We assume the
						 * input is already simplified, so no need to recurse.
						 */
						return (Node *) linitial(expr->args);
					default:
						elog(ERROR, "unrecognized boolop: %d",
							 (int) expr->boolop);
						break;
				}
			}
			break;
		case T_NullTest:
			{
				NullTest   *expr = (NullTest *) node;

				/*
				 * In the rowtype case, the two flavors of NullTest are *not*
				 * logical inverses, so we can't simplify.  But it does work
				 * for scalar datatypes.
				 */
				if (!expr->argisrow)
				{
					NullTest   *newexpr = makeNode(NullTest);

					newexpr->arg = expr->arg;
					newexpr->nulltesttype = (expr->nulltesttype == IS_NULL ?
											 IS_NOT_NULL : IS_NULL);
					newexpr->argisrow = expr->argisrow;
					newexpr->location = expr->location;
					return (Node *) newexpr;
				}
			}
			break;
		case T_BooleanTest:
			{
				BooleanTest *expr = (BooleanTest *) node;
				BooleanTest *newexpr = makeNode(BooleanTest);

				newexpr->arg = expr->arg;
				switch (expr->booltesttype)
				{
					case IS_TRUE:
						newexpr->booltesttype = IS_NOT_TRUE;
						break;
					case IS_NOT_TRUE:
						newexpr->booltesttype = IS_TRUE;
						break;
					case IS_FALSE:
						newexpr->booltesttype = IS_NOT_FALSE;
						break;
					case IS_NOT_FALSE:
						newexpr->booltesttype = IS_FALSE;
						break;
					case IS_UNKNOWN:
						newexpr->booltesttype = IS_NOT_UNKNOWN;
						break;
					case IS_NOT_UNKNOWN:
						newexpr->booltesttype = IS_UNKNOWN;
						break;
					default:
						elog(ERROR, "unrecognized booltesttype: %d",
							 (int) expr->booltesttype);
						break;
				}
				newexpr->location = expr->location;
				return (Node *) newexpr;
			}
			break;
		default:
			/* else fall through */
			break;
	}

	/*
	 * Otherwise we don't know how to simplify this, so just tack on an
	 * explicit NOT node.
	 */
	return (Node *) make_notclause((Expr *) node);
}


/*
 * canonicalize_qual
 *	  Convert a qualification expression to the most useful form.
 *
 * This is primarily intended to be used on top-level WHERE (or JOIN/ON)
 * clauses.  It can also be used on top-level CHECK constraints, for which
 * pass is_check = true.  DO NOT call it on any expression that is not known
 * to be one or the other, as it might apply inappropriate simplifications.
 *
 * The name of this routine is a holdover from a time when it would try to
 * force the expression into canonical AND-of-ORs or OR-of-ANDs form.
 * Eventually, we recognized that that had more theoretical purity than
 * actual usefulness, and so now the transformation doesn't involve any
 * notion of reaching a canonical form.
 *
 * NOTE: we assume the input has already been through eval_const_expressions
 * and therefore possesses AND/OR flatness.  Formerly this function included
 * its own flattening logic, but that requires a useless extra pass over the
 * tree.
 *
 * Returns the modified qualification.
 */
Expr *
canonicalize_qual(Expr *qual, bool is_check)
{
	Expr	   *newqual;

	/* Quick exit for empty qual */
	if (qual == NULL)
		return NULL;

	/* This should not be invoked on quals in implicit-AND format */
	Assert(!IsA(qual, List));

	/*
	 * Pull up redundant subclauses in OR-of-AND trees.  We do this only
	 * within the top-level AND/OR structure; there's no point in looking
	 * deeper.  Also remove any NULL constants in the top-level structure.
	 */
	newqual = find_duplicate_ors(qual, is_check);

	return newqual;
}


/*
 * is_simple_index_qual
 *		Recognize predicates that could match an index column directly:
 *		Var op Const, Var IS NULL/NOT NULL, Var = ANY(Const array).
 *		Used by expr_is_mdam_candidate() as the leaf-level test.
 */
static bool
is_simple_index_qual(Expr *expr)
{
	if (expr == NULL)
		return false;

	if (IsA(expr, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) expr;
		Node	   *l,
				   *r;

		if (list_length(op->args) != 2)
			return false;
		l = (Node *) linitial(op->args);
		r = (Node *) lsecond(op->args);
		/* strip RelabelType */
		if (IsA(l, RelabelType))
			l = (Node *) ((RelabelType *) l)->arg;
		if (IsA(r, RelabelType))
			r = (Node *) ((RelabelType *) r)->arg;
		return ((IsA(l, Var) && IsA(r, Const)) ||
				(IsA(l, Const) && IsA(r, Var)));
	}

	if (IsA(expr, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) expr;
		Node	   *l,
				   *r;

		if (!saop->useOr)
			return false;
		if (list_length(saop->args) != 2)
			return false;
		l = (Node *) linitial(saop->args);
		r = (Node *) lsecond(saop->args);
		return (IsA(l, Var) && IsA(r, Const));
	}

	if (IsA(expr, NullTest))
	{
		NullTest   *nt = (NullTest *) expr;

		return IsA(nt->arg, Var) && !nt->argisrow;
	}

	return false;
}

/*
 * mdam_candidate_walker
 *		Recursive helper for expr_is_mdam_candidate.
 */
static bool
mdam_candidate_walker(Expr *expr)
{
	if (expr == NULL)
		return false;

	if (IsA(expr, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) expr;
		ListCell   *lc;

		if (b->boolop == NOT_EXPR)
			return false;
		foreach(lc, b->args)
		{
			Expr	   *arg = (Expr *) lfirst(lc);

			if (IsA(arg, RestrictInfo))
				arg = ((RestrictInfo *) arg)->clause;
			if (!mdam_candidate_walker(arg))
				return false;
		}
		return true;
	}

	return is_simple_index_qual(expr);
}

/*
 * expr_is_mdam_candidate
 *		Decide whether an expression is a potential MDAM candidate.
 *
 * Returns true when the top-level node is an OR clause (or AND of ORs)
 * whose every leaf is a simple Var-op-Const / SAOP / IS [NOT] NULL
 * predicate.  This is a *cheap structural check* — final viability
 * (matching to an actual index, type compatibility, etc.) is decided
 * later in mdampath.c.
 *
 * Used to pre-mark RestrictInfo->mdam_candidate so that mdampath.c can
 * quickly skip RestrictInfos that have no chance of being processed.
 */
bool
expr_is_mdam_candidate(Expr *expr)
{
	if (expr == NULL)
		return false;
	/* Strip top-level RestrictInfo if any */
	if (IsA(expr, RestrictInfo))
		expr = ((RestrictInfo *) expr)->clause;
	/* Must be (or contain) an OR clause to be useful for MDAM */
	if (!is_orclause(expr))
		return false;
	return mdam_candidate_walker(expr);
}


/*
 * pull_ands
 *	  Recursively flatten nested AND clauses into a single and-clause list.
 *
 * Input is the arglist of an AND clause.
 * Returns the rebuilt arglist (note original list structure is not touched).
 */
static List *
pull_ands(List *andlist)
{
	List	   *out_list = NIL;
	ListCell   *arg;

	foreach(arg, andlist)
	{
		Node	   *subexpr = (Node *) lfirst(arg);

		if (is_andclause(subexpr))
			out_list = list_concat(out_list,
								   pull_ands(((BoolExpr *) subexpr)->args));
		else
			out_list = lappend(out_list, subexpr);
	}
	return out_list;
}

/*
 * pull_ors
 *	  Recursively flatten nested OR clauses into a single or-clause list.
 *
 * Input is the arglist of an OR clause.
 * Returns the rebuilt arglist (note original list structure is not touched).
 */
static List *
pull_ors(List *orlist)
{
	List	   *out_list = NIL;
	ListCell   *arg;

	foreach(arg, orlist)
	{
		Node	   *subexpr = (Node *) lfirst(arg);

		if (is_orclause(subexpr))
			out_list = list_concat(out_list,
								   pull_ors(((BoolExpr *) subexpr)->args));
		else
			out_list = lappend(out_list, subexpr);
	}
	return out_list;
}


/* Unified boolean simplifier: recursive AND (intersection) / OR (absorption + union).
 * rollback on complex cases.
 */

static bool
is_const_true(Expr *expr, bool is_check)
{
	Const	   *c;

	if (expr == NULL || !IsA(expr, Const))
		return false;

	c = (Const *) expr;
	if (c->constisnull)
		return is_check;		/* NULL = TRUE in CHECK */
	return DatumGetBool(c->constvalue);
}

static bool
is_const_false(Expr *expr, bool is_check)
{
	Const	   *c;

	if (expr == NULL || !IsA(expr, Const))
		return false;

	c = (Const *) expr;
	if (c->constisnull)
		return !is_check;		/* NULL = FALSE in WHERE */
	return !DatumGetBool(c->constvalue);
}

static Expr *
simplify_boolean_tree(Expr *expr, bool is_check)
{
	if (expr == NULL)
		return NULL;
	if (is_andclause(expr))
		return simplify_and_clause((BoolExpr *) expr, is_check);
	if (is_orclause(expr))
		return simplify_or_clause((BoolExpr *) expr, is_check);
	return expr;
}

/* AND: recurse, drop TRUE, short-circuit FALSE, then try range intersection */
static Expr *
simplify_and_clause(BoolExpr *andexpr, bool is_check)
{
	List	   *newargs = NIL;
	ListCell   *lc;

	foreach(lc, andexpr->args)
	{
		Expr	   *arg = simplify_boolean_tree((Expr *) lfirst(lc), is_check);

		if (is_const_true(arg, is_check))
			continue;
		if (is_const_false(arg, is_check))
			return (Expr *) makeBoolConst(false, false);

		newargs = lappend(newargs, arg);
	}

	newargs = pull_ands(newargs);

	if (newargs == NIL)
		return (Expr *) makeBoolConst(true, false);

	newargs = try_range_intersection(newargs);

	/* Check if range intersection found a contradiction */
	if (list_length(newargs) == 1 && IsA(linitial(newargs), Const))
	{
		Const	   *k = (Const *) linitial(newargs);

		if (!k->constisnull && !DatumGetBool(k->constvalue))
			return (Expr *) makeBoolConst(false, false);
	}
	if (list_length(newargs) == 1)
		return (Expr *) linitial(newargs);
	return make_andclause(newargs);
}

/* OR: recurse, drop FALSE, short-circuit TRUE, then absorption / union / distributive */
static Expr *
simplify_or_clause(BoolExpr *orexpr, bool is_check)
{
	List	   *newargs = NIL;
	ListCell   *lc;
	Expr	   *result;

	foreach(lc, orexpr->args)
	{
		Expr	   *arg = simplify_boolean_tree((Expr *) lfirst(lc), is_check);

		if (is_const_false(arg, is_check))
			continue;
		if (is_const_true(arg, is_check))
			return (Expr *) makeBoolConst(true, false);

		newargs = lappend(newargs, arg);
	}

	newargs = pull_ors(newargs);

	if (newargs == NIL)
		return (Expr *) makeBoolConst(false, false);

	if (list_length(newargs) == 1)
		return (Expr *) linitial(newargs);

	/* Try boolean absorption: A OR (A AND B) => A */
	result = try_or_absorption_boolean(newargs);
	if (result != NULL)
		return result;

	/* Try single-bound absorption: x < 1 OR x < 10 => x < 10 */
	result = try_or_absorption(newargs);
	if (result != NULL)
		return result;

	/* Try range union: (x > 1 AND x < 5) OR (x > 3 AND x < 8) => x > 1 AND x < 8 */
	result = try_range_union(newargs);
	if (result != NULL)
		return result;

	/* Try not-equal simplification: x <> V OR x = V => true,
	 *                                x < V OR x > V => x <> V */
	result = try_or_neq_simplification(newargs);
	if (result != NULL)
		return result;

	/* Fall back to distributive law */
	return process_duplicate_ors(newargs);
}


/*
 * Range simplifier: intersect AND'ed inequalities (Var OP Const) per Var.
 * Immutable, btree opfamily, one Var per group, only < <= > >= =.
 */

static Oid
get_btree_opfamily(Oid opno)
{
	CatCList   *catlist;
	int			i;
	Oid			result = InvalidOid;

	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));
	if (!catlist)
		return InvalidOid;
	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tup = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tup);

		if (aform->amopmethod == BTREE_AM_OID)
		{
			result = aform->amopfamily;
			break;
		}
	}
	ReleaseSysCacheList(catlist);
	return result;
}

static void
make_var_range_key(Var *var, Oid opfamily, Oid collation, VarRangeKey *key)
{
	key->varno = var->varno;
	key->varattno = var->varattno;
	key->vartype = var->vartype;
	key->vartypmod = var->vartypmod;
	key->varcollid = var->varcollid;
	key->varlevelsup = var->varlevelsup;
	key->opfamily = opfamily;
	key->collation = collation;
}

static HTAB *
create_range_hashtable(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(VarRangeKey);
	ctl.entrysize = sizeof(VarRange);
	ctl.hcxt = CurrentMemoryContext;
	return hash_create("Var range table", 16, &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static VarRange *
lookup_or_create_range(HTAB *ht, VarRangeKey *key, Var *var)
{
	VarRange   *r;
	bool		found;

	r = (VarRange *) hash_search(ht, key, HASH_ENTER, &found);
	if (!found)
	{
		r->key = *key;
		r->lower = NULL;
		r->lower_inc = false;
		r->upper = NULL;
		r->upper_inc = false;
		r->equal = NULL;
		r->empty = false;
		r->var = var;
	}
	return r;
}

/*
 * Extract Var and Const from OpExpr.
 * Return true if exactly one side is Var and the other is Const.
 */
static bool
extract_var_const(OpExpr *op, Var **var, Const **constval, bool *var_on_left)
{
	Node	   *left = get_leftop((Expr *) op);
	Node	   *right = get_rightop((Expr *) op);

	if (left == NULL || right == NULL)
		return false;
	if (IsA(left, Var) && IsA(right, Const))
	{
		*var = (Var *) left;
		*constval = (Const *) right;
		*var_on_left = true;
		return true;
	}
	if (IsA(left, Const) && IsA(right, Var))
	{
		*var = (Var *) right;
		*constval = (Const *) left;
		*var_on_left = false;
		return true;
	}
	return false;
}

static bool
is_supported_operator(OpExpr *op)
{
	List	   *infos = get_op_index_interpretation(op->opno);
	ListCell   *lc;
	bool		ok = false;

	if (infos == NIL)
		return false;
	foreach(lc, infos)
	{
		OpIndexInterpretation *info = (OpIndexInterpretation *) lfirst(lc);
		CompareType cmptype = info->cmptype;

		if (cmptype == COMPARE_LT || cmptype == COMPARE_LE ||
			cmptype == COMPARE_EQ || cmptype == COMPARE_GE ||
			cmptype == COMPARE_GT)
		{
			ok = true;
			break;
		}
	}
	list_free(infos);
	return ok;
}

static int
compare_consts(Const *a, Const *b, Oid opfamily, Oid collation)
{
	Oid			typid = a->consttype;
	Oid			cmpfn;
	Datum		da,
				db;
	Datum		result;

	if (a->constisnull || b->constisnull)
		return 0;
	if (!OidIsValid(opfamily))
		return 0;
	/* BTORDER_PROC is for same-type comparison; avoid calling with mixed types. */
	if (a->consttype != b->consttype)
		return 0;
	cmpfn = get_opfamily_proc(opfamily, typid, typid, BTORDER_PROC);
	if (!OidIsValid(cmpfn))
		return 0;
	da = a->constvalue;
	db = b->constvalue;
	if (!OidIsValid(collation))
		collation = a->constcollid;
	result = OidFunctionCall2Coll(cmpfn, collation, da, db);
	return DatumGetInt32(result);
}

static void apply_equality(VarRange *r, Const *c, Oid opfamily, Oid collation);

/*
 * Apply a lower bound (x > c or x >= c) to the range.
 * If an equality is already set, check that it satisfies this bound:
 * empty when equal < lower, or equal == lower with exclusive bound.
 */
static void
apply_lower(VarRange *r, Const *c, bool inclusive, Oid opfamily, Oid collation)
{
	if (r->empty)
		return;
	if (r->equal != NULL)
	{
		int			cmp = compare_consts(r->equal, c, opfamily, collation);

		/* Empty if equality value is below lower bound, or on exclusive bound */
		if (cmp < 0 || (cmp == 0 && !inclusive))
			r->empty = true;
		return;
	}
	if (r->lower == NULL)
	{
		r->lower = c;
		r->lower_inc = inclusive;
	}
	else
	{
		int			cmp = compare_consts(c, r->lower, opfamily, collation);

		if (cmp > 0)
		{
			r->lower = c;
			r->lower_inc = inclusive;
		}
		else if (cmp == 0)
			/* Take the stricter bound: intersection of > and >= is > */
			r->lower_inc &= inclusive;
	}
}

/*
 * Apply an upper bound (x < c or x <= c) to the range.
 * If an equality is already set, check that it satisfies this bound:
 * empty when equal > upper, or equal == upper with exclusive bound.
 */
static void
apply_upper(VarRange *r, Const *c, bool inclusive, Oid opfamily, Oid collation)
{
	if (r->empty)
		return;
	if (r->equal != NULL)
	{
		int			cmp = compare_consts(r->equal, c, opfamily, collation);

		/* Empty if equality value is above upper bound, or on exclusive bound */
		if (cmp > 0 || (cmp == 0 && !inclusive))
			r->empty = true;
		return;
	}
	if (r->upper == NULL)
	{
		r->upper = c;
		r->upper_inc = inclusive;
	}
	else
	{
		int			cmp = compare_consts(c, r->upper, opfamily, collation);

		if (cmp < 0)
		{
			r->upper = c;
			r->upper_inc = inclusive;
		}
		else if (cmp == 0)
			r->upper_inc &= inclusive;
	}
}

static void
apply_equality(VarRange *r, Const *c, Oid opfamily, Oid collation)
{
	if (r->empty)
		return;
	if (r->equal != NULL)
	{
		if (compare_consts(r->equal, c, opfamily, collation) != 0)
			r->empty = true;
		return;
	}
	r->equal = c;
	if (r->lower != NULL || r->upper != NULL)
	{
		int			cmp_l = (r->lower != NULL) ?
						compare_consts(c, r->lower, opfamily, collation) : 0;
		int			cmp_u = (r->upper != NULL) ?
						compare_consts(c, r->upper, opfamily, collation) : 0;

		if (r->lower != NULL && (cmp_l < 0 || (cmp_l == 0 && !r->lower_inc)))
			r->empty = true;
		else if (r->upper != NULL && (cmp_u > 0 || (cmp_u == 0 && !r->upper_inc)))
			r->empty = true;
	}
}

/*
 * Flip a btree strategy to the commutator's perspective.
 * E.g. "5 < x" from the operator's view is "x > 5" from the Var's view.
 */
static int
flip_btree_strategy(int strat)
{
	switch (strat)
	{
		case BTLessStrategyNumber:
			return BTGreaterStrategyNumber;
		case BTLessEqualStrategyNumber:
			return BTGreaterEqualStrategyNumber;
		case BTGreaterStrategyNumber:
			return BTLessStrategyNumber;
		case BTGreaterEqualStrategyNumber:
			return BTLessEqualStrategyNumber;
		default:
			return strat;
	}
}

/*
 * Map strategy to range update.  When var_on_left is false, the clause is
 * (Const op Var), so e.g. "5 < x" means x > 5 → apply_lower.
 */
static void
update_range_with_clause(VarRange *r, OpExpr *op, Const *c, bool var_on_left)
{
	int			strat;
	Oid			opfamily = r->key.opfamily;
	Oid			collation = r->key.collation;

	if (r->empty)
		return;
	strat = get_op_opfamily_strategy(op->opno, opfamily);
	if (strat == 0)
	{
		r->empty = true;
		return;
	}
	if (!var_on_left)
		strat = flip_btree_strategy(strat);
	switch (strat)
	{
		case BTEqualStrategyNumber:
			apply_equality(r, c, opfamily, collation);
			break;
		case BTLessStrategyNumber:
			apply_upper(r, c, false, opfamily, collation);
			break;
		case BTLessEqualStrategyNumber:
			apply_upper(r, c, true, opfamily, collation);
			break;
		case BTGreaterStrategyNumber:
			apply_lower(r, c, false, opfamily, collation);
			break;
		case BTGreaterEqualStrategyNumber:
			apply_lower(r, c, true, opfamily, collation);
			break;
		default:
			r->empty = true;
			break;
	}
	if (!r->empty && r->lower != NULL && r->upper != NULL && r->equal == NULL)
	{
		int			cmp = compare_consts(r->lower, r->upper, opfamily, collation);

		if (cmp > 0 || (cmp == 0 && (!r->lower_inc || !r->upper_inc)))
			r->empty = true;
	}
}

static bool
try_add_clause_to_range(HTAB *ht, Expr *clause)
{
	OpExpr	   *op;
	Var		   *var;
	Const	   *c;
	bool		var_on_left;
	Oid			opfamily;
	VarRangeKey key;
	VarRange   *r;

	if (!IsA(clause, OpExpr))
		return false;
	op = (OpExpr *) clause;
	if (!is_supported_operator(op))
		return false;
	if (!extract_var_const(op, &var, &c, &var_on_left))
		return false;
	if (c->constisnull)
		return false;
	/* Only same-type comparisons (e.g. avoid Oid = int4) to prevent coredumps when building expr. */
	if (var->vartype != c->consttype)
		return false;
	if (op_volatile(op->opno) != PROVOLATILE_IMMUTABLE)
		return false;
	opfamily = get_btree_opfamily(op->opno);
	if (!OidIsValid(opfamily))
		return false;
	make_var_range_key(var, opfamily, op->inputcollid, &key);
	r = lookup_or_create_range(ht, &key, var);
	update_range_with_clause(r, op, c, var_on_left);
	return true;
}

static Expr *
make_eq_expr(VarRange *r)
{
	Oid			eqop = get_opfamily_member(r->key.opfamily,
											r->var->vartype,
											r->equal->consttype,
											BTEqualStrategyNumber);

	if (!OidIsValid(eqop))
		return NULL;
	return (Expr *) make_opclause(eqop, BOOLOID, false,
								  (Expr *) copyObject(r->var),
								  (Expr *) copyObject(r->equal),
								  InvalidOid, r->key.collation);
}

static Expr *
make_lower_expr(VarRange *r)
{
	StrategyNumber strat = r->lower_inc ? BTGreaterEqualStrategyNumber : BTGreaterStrategyNumber;
	Oid			opno = get_opfamily_member(r->key.opfamily,
											r->var->vartype,
											r->lower->consttype,
											strat);

	if (!OidIsValid(opno))
		return NULL;
	return (Expr *) make_opclause(opno, BOOLOID, false,
								  (Expr *) copyObject(r->var),
								  (Expr *) copyObject(r->lower),
								  InvalidOid, r->key.collation);
}

static Expr *
make_upper_expr(VarRange *r)
{
	StrategyNumber strat = r->upper_inc ? BTLessEqualStrategyNumber : BTLessStrategyNumber;
	Oid			opno = get_opfamily_member(r->key.opfamily,
											r->var->vartype,
											r->upper->consttype,
											strat);

	if (!OidIsValid(opno))
		return NULL;
	return (Expr *) make_opclause(opno, BOOLOID, false,
								  (Expr *) copyObject(r->var),
								  (Expr *) copyObject(r->upper),
								  InvalidOid, r->key.collation);
}

static List *
build_range_clauses(HTAB *ht)
{
	List	   *result = NIL;
	HASH_SEQ_STATUS status;
	VarRange   *r;

	hash_seq_init(&status, ht);
	while ((r = (VarRange *) hash_seq_search(&status)) != NULL)
	{
		if (r->empty)
		{
			hash_seq_term(&status);
			return list_make1(makeBoolConst(false, false));
		}

		if (r->equal != NULL)
		{
			Expr	   *eq = make_eq_expr(r);

			if (eq != NULL)
				result = lappend(result, eq);
			continue;
		}

		if (r->lower != NULL)
		{
			Expr	   *lo = make_lower_expr(r);

			if (lo != NULL)
				result = lappend(result, lo);
		}
		if (r->upper != NULL)
		{
			Expr	   *hi = make_upper_expr(r);

			if (hi != NULL)
				result = lappend(result, hi);
		}
	}
	/* Normal exit: hash_seq_search returned NULL, scan already cleaned up */
	return result;
}

static List *
try_range_intersection(List *andlist)
{
	HTAB	   *range_table;
	List	   *others = NIL;
	ListCell   *lc;

	range_table = create_range_hashtable();

	foreach(lc, andlist)
	{
		Expr	   *clause = (Expr *) lfirst(lc);

		if (!try_add_clause_to_range(range_table, clause))
			others = lappend(others, clause);
	}

	return list_concat(build_range_clauses(range_table), others);
}

/*
 * Helper: is every element of sublist contained in superlist (by equal())?
 */
static bool
list_is_subset(List *sublist, List *superlist)
{
	ListCell   *lc;

	foreach(lc, sublist)
	{
		if (!list_member(superlist, lfirst(lc)))
			return false;
	}
	return true;
}

/*
 * Boolean absorption: A OR (A AND B) => A.
 * Find an OR arm whose set of subclauses is contained in every other arm.
 */
static Expr *
try_or_absorption_boolean(List *orlist)
{
	ListCell   *lc;
	ListCell   *lc2;

	foreach(lc, orlist)
	{
		Expr	   *candidate = (Expr *) lfirst(lc);
		List	   *cand_list;
		bool		contained_in_all = true;

		if (is_andclause(candidate))
			cand_list = list_copy(((BoolExpr *) candidate)->args);
		else
			cand_list = list_make1(candidate);

		foreach(lc2, orlist)
		{
			Expr	   *other = (Expr *) lfirst(lc2);

			if (other == candidate)
				continue;
			if (is_andclause(other))
			{
				List	   *other_list = ((BoolExpr *) other)->args;

				if (!list_is_subset(cand_list, other_list))
				{
					contained_in_all = false;
					break;
				}
			}
			else
			{
				if (list_length(cand_list) != 1 || !equal(linitial(cand_list), other))
				{
					contained_in_all = false;
					break;
				}
			}
		}

		if (contained_in_all)
		{
			Expr	   *result = (list_length(cand_list) == 1)
				? (Expr *) copyObject(linitial(cand_list))
				: (Expr *) copyObject(candidate);

			list_free(cand_list);
			return result;
		}
		list_free(cand_list);
	}
	return NULL;
}

/*
 * OR absorption for single-sided bounds on same Var: x < 1 OR x < 10 => x < 10.
 * All args must be OpExpr Var OP Const, same Var/opfamily, all upper or all lower.
 */
static Expr *
try_or_absorption(List *orlist)
{
	VarRangeKey refkey;
	bool		refkey_set = false;
	bool		upper_only = false;	/* true = only LT/LE, false = only GT/GE */
	Const	   *best_upper = NULL;
	bool		best_upper_inc = false;
	Const	   *best_lower = NULL;
	bool		best_lower_inc = false;
	Var		   *refvar = NULL;
	Oid			opfamily = InvalidOid;
	Oid			collation = InvalidOid;
	ListCell   *lc;

	foreach(lc, orlist)
	{
		OpExpr	   *op;
		Var		   *var;
		Const	   *c;
		bool		var_on_left;
		int			strat;
		VarRangeKey key;

		if (!IsA(lfirst(lc), OpExpr))
			return NULL;
		op = (OpExpr *) lfirst(lc);
		if (!is_supported_operator(op) || !extract_var_const(op, &var, &c, &var_on_left))
			return NULL;
		if (c->constisnull || op_volatile(op->opno) != PROVOLATILE_IMMUTABLE)
			return NULL;
		opfamily = get_btree_opfamily(op->opno);
		if (!OidIsValid(opfamily))
			return NULL;
		make_var_range_key(var, opfamily, op->inputcollid, &key);
		strat = get_op_opfamily_strategy(op->opno, opfamily);
		if (strat == 0)
			return NULL;
		if (!var_on_left)
			strat = flip_btree_strategy(strat);
		if (strat == BTEqualStrategyNumber)
			return NULL;			/* equality not absorbed as single bound */

		if (!refkey_set)
		{
			refkey = key;
			refkey_set = true;
			refvar = var;
			collation = op->inputcollid;
			upper_only = (strat == BTLessStrategyNumber || strat == BTLessEqualStrategyNumber);
		}
		else if (refkey.varno != key.varno || refkey.varattno != key.varattno ||
				 refkey.opfamily != key.opfamily || refkey.collation != key.collation)
			return NULL;

		if (upper_only)
		{
			if (strat == BTLessStrategyNumber || strat == BTLessEqualStrategyNumber)
			{
				if (best_upper == NULL)
				{
					best_upper = c;
					best_upper_inc = (strat == BTLessEqualStrategyNumber);
				}
				else
				{
					int			cmp = compare_consts(c, best_upper, opfamily, collation);

					if (cmp > 0 || (cmp == 0 && strat == BTLessEqualStrategyNumber && !best_upper_inc))
					{
						best_upper = c;
						best_upper_inc = (strat == BTLessEqualStrategyNumber);
					}
				}
			}
			else
				return NULL;
		}
		else
		{
			if (strat == BTGreaterStrategyNumber || strat == BTGreaterEqualStrategyNumber)
			{
				if (best_lower == NULL)
				{
					best_lower = c;
					best_lower_inc = (strat == BTGreaterEqualStrategyNumber);
				}
				else
				{
					int			cmp = compare_consts(c, best_lower, opfamily, collation);

					if (cmp < 0 || (cmp == 0 && strat == BTGreaterEqualStrategyNumber && !best_lower_inc))
					{
						best_lower = c;
						best_lower_inc = (strat == BTGreaterEqualStrategyNumber);
					}
				}
			}
			else
				return NULL;
		}
	}

	if (!refkey_set)
		return NULL;

	/* Build single bound expr */
	if (upper_only && best_upper != NULL)
	{
		VarRange	r;

		r.key = refkey;
		r.var = refvar;
		r.lower = NULL;
		r.upper = best_upper;
		r.upper_inc = best_upper_inc;
		r.equal = NULL;
		r.empty = false;
		return make_upper_expr(&r);
	}
	if (!upper_only && best_lower != NULL)
	{
		VarRange	r;

		r.key = refkey;
		r.var = refvar;
		r.lower = best_lower;
		r.lower_inc = best_lower_inc;
		r.upper = NULL;
		r.equal = NULL;
		r.empty = false;
		return make_lower_expr(&r);
	}
	return NULL;
}

/*
 * Get the single VarRange from a hashtable (exactly one entry, not empty).
 */
static bool
get_single_range_from_ht(HTAB *ht, VarRange *out)
{
	HASH_SEQ_STATUS status;
	VarRange   *entry;
	int			count = 0;

	hash_seq_init(&status, ht);
	while ((entry = (VarRange *) hash_seq_search(&status)) != NULL)
	{
		if (count++ > 0 || entry->empty)
		{
			hash_seq_term(&status);
			return false;
		}
		out->key = entry->key;
		out->lower = entry->lower ? (Const *) copyObject(entry->lower) : NULL;
		out->lower_inc = entry->lower_inc;
		out->upper = entry->upper ? (Const *) copyObject(entry->upper) : NULL;
		out->upper_inc = entry->upper_inc;
		out->equal = entry->equal ? (Const *) copyObject(entry->equal) : NULL;
		out->empty = false;
		out->var = (Var *) copyObject(entry->var);
	}
	return (count == 1);
}

static bool
build_range_from_and(BoolExpr *andexpr, VarRange *out)
{
	HTAB	   *ht;
	ListCell   *lc;
	bool		ok = false;

	ht = create_range_hashtable();
	foreach(lc, andexpr->args)
	{
		if (!try_add_clause_to_range(ht, (Expr *) lfirst(lc)))
		{
			hash_destroy(ht);
			return false;
		}
	}
	if (hash_get_num_entries(ht) == 1)
		ok = get_single_range_from_ht(ht, out);
	hash_destroy(ht);
	return ok;
}

static bool
build_range_from_op(Expr *opexpr, VarRange *out)
{
	HTAB	   *ht;
	bool		ok = false;

	if (!IsA(opexpr, OpExpr))
		return false;
	ht = create_range_hashtable();
	if (try_add_clause_to_range(ht, opexpr) && hash_get_num_entries(ht) == 1)
		ok = get_single_range_from_ht(ht, out);
	hash_destroy(ht);
	return ok;
}

static bool
extract_range_from_expr(Expr *expr, VarRange *out)
{
	if (is_andclause(expr))
		return build_range_from_and((BoolExpr *) expr, out);
	if (IsA(expr, OpExpr))
		return build_range_from_op(expr, out);
	return false;
}

/*
 * extract_ranges_from_saop
 *		Convert a ScalarArrayOpExpr "Var = ANY (Const array)" into a list of
 *		single-equality VarRanges (one per array element).  Returns NIL if
 *		the SAOP isn't suitable (non-Const array, non-equality op, NULL,
 *		volatile, cross-type, etc).  Used by try_range_union() so that
 *		IN-lists participate in OR-range merging just like equality clauses.
 */
static List *
extract_ranges_from_saop(ScalarArrayOpExpr *saop)
{
	Var		   *var;
	Const	   *arrayConst;
	Node	   *leftop,
			   *rightop;
	Oid			opfamily;
	int			strat;
	ArrayType  *arr;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	Oid			elemtype;
	int16		elemlen;
	bool		elembyval;
	char		elemalign;
	List	   *result = NIL;
	VarRangeKey key;
	int			i;

	if (!saop->useOr)
		return NIL;
	if (list_length(saop->args) != 2)
		return NIL;
	leftop = (Node *) linitial(saop->args);
	rightop = (Node *) lsecond(saop->args);

	/* Var on left, Const array on right */
	if (IsA(leftop, RelabelType))
		leftop = (Node *) ((RelabelType *) leftop)->arg;
	if (!IsA(leftop, Var) || !IsA(rightop, Const))
		return NIL;
	var = (Var *) leftop;
	arrayConst = (Const *) rightop;
	if (arrayConst->constisnull)
		return NIL;

	/* Operator must be a btree equality */
	opfamily = get_btree_opfamily(saop->opno);
	if (!OidIsValid(opfamily))
		return NIL;
	strat = get_op_opfamily_strategy(saop->opno, opfamily);
	if (strat != BTEqualStrategyNumber)
		return NIL;
	if (op_volatile(saop->opno) != PROVOLATILE_IMMUTABLE)
		return NIL;

	/* Element type must match the Var */
	arr = DatumGetArrayTypeP(arrayConst->constvalue);
	elemtype = ARR_ELEMTYPE(arr);
	if (elemtype != var->vartype)
		return NIL;

	get_typlenbyvalalign(elemtype, &elemlen, &elembyval, &elemalign);
	deconstruct_array(arr, elemtype, elemlen, elembyval, elemalign,
					  &elems, &nulls, &nelems);
	if (nelems == 0)
		return NIL;

	make_var_range_key(var, opfamily, saop->inputcollid, &key);

	/* Build one VarRange per non-null array element */
	for (i = 0; i < nelems; i++)
	{
		VarRange   *r;
		Const	   *c;

		if (nulls[i])
			continue;
		c = makeConst(elemtype, -1, var->varcollid, elemlen,
					  elems[i], false, elembyval);
		r = palloc0(sizeof(VarRange));
		r->key = key;
		r->equal = c;
		r->lower = NULL;
		r->upper = NULL;
		r->empty = false;
		r->var = var;
		result = lappend(result, r);
	}

	return result;
}

/*
 * extract_ranges_from_expr
 *		Like extract_range_from_expr() but also handles ScalarArrayOpExpr
 *		(IN-lists), returning multiple VarRanges instead of one.  Used by
 *		try_range_union() to fold IN-lists into the merge.
 *
 * Returns NIL if the expression can't be turned into ranges.  The
 * returned list is owned by the caller (palloc'd ranges).
 */
static List *
extract_ranges_from_expr(Expr *expr)
{
	VarRange	r;

	if (IsA(expr, ScalarArrayOpExpr))
		return extract_ranges_from_saop((ScalarArrayOpExpr *) expr);

	if (extract_range_from_expr(expr, &r))
		return list_make1(copy_var_range(&r));

	return NIL;
}

static VarRange *
copy_var_range(VarRange *r)
{
	VarRange   *out = palloc(sizeof(VarRange));

	out->key = r->key;
	out->lower = r->lower ? (Const *) copyObject(r->lower) : NULL;
	out->lower_inc = r->lower_inc;
	out->upper = r->upper ? (Const *) copyObject(r->upper) : NULL;
	out->upper_inc = r->upper_inc;
	out->equal = r->equal ? (Const *) copyObject(r->equal) : NULL;
	out->empty = r->empty;
	out->var = (Var *) copyObject(r->var);
	return out;
}

static bool
same_var_and_family(VarRange *r1, VarRange *r2)
{
	return r1->key.varno == r2->key.varno &&
		r1->key.varattno == r2->key.varattno &&
		r1->key.vartype == r2->key.vartype &&
		r1->key.opfamily == r2->key.opfamily &&
		r1->key.collation == r2->key.collation;
}

/*
 * Sort comparator: by lower bound for merge.  NULL lower = -inf, must sort first
 * so that unbounded-below ranges are merged before bounded ones.
 */
static int
range_lower_cmp(const ListCell *a, const ListCell *b)
{
	VarRange   *ra = (VarRange *) a->ptr_value;
	VarRange   *rb = (VarRange *) b->ptr_value;
	Const	   *la = ra->equal ? ra->equal : ra->lower;
	Const	   *lb = rb->equal ? rb->equal : rb->lower;

	/* NULL lower = -inf, comes first */
	if (la == NULL && lb == NULL)
		return 0;
	if (la == NULL)
		return -1;				/* ra first */
	if (lb == NULL)
		return 1;				/* rb first */
	return compare_consts(la, lb, ra->key.opfamily, ra->key.collation);
}

/*
 * Do ranges a and b overlap or touch?  Compare a's upper with b's lower.
 * Treat equality (point) as both lower and upper.
 */
static bool
ranges_overlap_or_touch(VarRange *a, VarRange *b)
{
	Const	   *a_upper = a->equal ? a->equal : a->upper;
	Const	   *b_lower = b->equal ? b->equal : b->lower;
	bool		a_upper_inc = a->equal ? true : a->upper_inc;
	bool		b_lower_inc = b->equal ? true : b->lower_inc;
	Oid			opfamily = a->key.opfamily;
	Oid			collation = a->key.collation;
	int			cmp;

	if (a_upper == NULL || b_lower == NULL)
		return true;			/* unbounded */

	cmp = compare_consts(a_upper, b_lower, opfamily, collation);
	if (cmp > 0)
		return true;
	if (cmp == 0)
		return (a_upper_inc || b_lower_inc);
	return false;
}

/*
 * Union of two ranges: min of lowers, max of uppers, with inclusivity.
 * NULL lower = -inf, NULL upper = +inf.
 */
static void
union_two_ranges(VarRange *a, VarRange *b, VarRange *out)
{
	Const	   *a_lower = a->equal ? a->equal : a->lower;
	Const	   *a_upper = a->equal ? a->equal : a->upper;
	Const	   *b_lower = b->equal ? b->equal : b->lower;
	Const	   *b_upper = b->equal ? b->equal : b->upper;
	bool		a_lower_inc = a->equal ? true : a->lower_inc;
	bool		a_upper_inc = a->equal ? true : a->upper_inc;
	bool		b_lower_inc = b->equal ? true : b->lower_inc;
	bool		b_upper_inc = b->equal ? true : b->upper_inc;
	Oid			opfamily = a->key.opfamily;
	Oid			collation = a->key.collation;

	out->key = a->key;
	out->var = a->var;
	out->equal = NULL;			/* union is never a point unless both are same point */

	/* Lower: if either is unbounded below (-inf), union is unbounded below */
	if (a_lower == NULL || b_lower == NULL)
	{
		out->lower = NULL;
		out->lower_inc = false;
	}
	else
	{
		int			cmp = compare_consts(a_lower, b_lower, opfamily, collation);

		if (cmp < 0)
		{
			out->lower = a_lower;
			out->lower_inc = a_lower_inc;
		}
		else if (cmp > 0)
		{
			out->lower = b_lower;
			out->lower_inc = b_lower_inc;
		}
		else
		{
			out->lower = a_lower;
			out->lower_inc = a_lower_inc || b_lower_inc;
		}
	}

	/* Upper: if either is unbounded above (+inf), union is unbounded above */
	if (a_upper == NULL || b_upper == NULL)
	{
		out->upper = NULL;
		out->upper_inc = false;
	}
	else
	{
		int			cmp = compare_consts(a_upper, b_upper, opfamily, collation);

		if (cmp > 0)
		{
			out->upper = a_upper;
			out->upper_inc = a_upper_inc;
		}
		else if (cmp < 0)
		{
			out->upper = b_upper;
			out->upper_inc = b_upper_inc;
		}
		else
		{
			out->upper = a_upper;
			out->upper_inc = a_upper_inc || b_upper_inc;
		}
	}
	out->empty = false;
}

static void
merge_ranges(List *ranges, VarRange *out, bool *single_interval)
{
	ListCell   *lc;
	VarRange   *current;
	VarRange   *next_merged;
	bool		current_is_alloced = false;

	if (ranges == NIL)
	{
		*single_interval = false;
		return;
	}
	current = (VarRange *) linitial(ranges);
	next_merged = palloc(sizeof(VarRange));
	for_each_from(lc, ranges, 1)
	{
		VarRange   *next = (VarRange *) lfirst(lc);

		if (!ranges_overlap_or_touch(current, next))
		{
			pfree(next_merged);
			if (current_is_alloced)
				pfree(current);
			*single_interval = false;
			return;
		}
		union_two_ranges(current, next, next_merged);
		if (current_is_alloced)
			pfree(current);
		current = next_merged;
		current_is_alloced = true;
		next_merged = palloc(sizeof(VarRange));
	}
	pfree(next_merged);
	*out = *current;
	if (current_is_alloced)
		pfree(current);
	*single_interval = true;
}

/*
 * Defensive check: merged must be a valid superset of the union of ranges.
 * If any original is unbounded below (lower NULL), merged must be unbounded below.
 * If any original is unbounded above (upper NULL), merged must be unbounded above.
 * Otherwise merged.lower <= min(original lowers) and merged.upper >= max(original uppers).
 */
static bool
range_union_sanity_check(List *ranges, VarRange *merged)
{
	ListCell   *lc;
	bool		any_unbounded_below = false;
	bool		any_unbounded_above = false;
	Const	   *min_lower = NULL;
	Const	   *max_upper = NULL;
	Oid			opfamily = merged->key.opfamily;
	Oid			collation = merged->key.collation;

	foreach(lc, ranges)
	{
		VarRange   *r = (VarRange *) lfirst(lc);
		Const	   *eff_lower = r->equal ? r->equal : r->lower;
		Const	   *eff_upper = r->equal ? r->equal : r->upper;

		if (eff_lower == NULL)
			any_unbounded_below = true;
		else if (min_lower == NULL || compare_consts(eff_lower, min_lower, opfamily, collation) < 0)
			min_lower = eff_lower;

		if (eff_upper == NULL)
			any_unbounded_above = true;
		else if (max_upper == NULL || compare_consts(eff_upper, max_upper, opfamily, collation) > 0)
			max_upper = eff_upper;
	}

	if (any_unbounded_below && merged->lower != NULL)
		return false;
	if (any_unbounded_above && merged->upper != NULL)
		return false;
	if (min_lower != NULL && merged->lower != NULL &&
		compare_consts(merged->lower, min_lower, opfamily, collation) > 0)
		return false;
	if (max_upper != NULL && merged->upper != NULL &&
		compare_consts(merged->upper, max_upper, opfamily, collation) < 0)
		return false;
	return true;
}

static Expr *
build_and_from_range(VarRange *r)
{
	List	   *clauses = NIL;
	Expr	   *eq;

	if (r->equal != NULL)
	{
		eq = make_eq_expr(r);
		if (eq == NULL)
			return NULL;		/* cross-type or missing op, caller must not use */
		return eq;
	}
	if (r->lower != NULL)
	{
		Expr	   *lo = make_lower_expr(r);

		if (lo != NULL)
			clauses = lappend(clauses, lo);
	}
	if (r->upper != NULL)
	{
		Expr	   *hi = make_upper_expr(r);

		if (hi != NULL)
			clauses = lappend(clauses, hi);
	}
	if (clauses == NIL)
		return (Expr *) makeBoolConst(true, false);
	if (list_length(clauses) == 1)
		return (Expr *) linitial(clauses);
	return make_andclause(clauses);
}

/*
 * OR interval union: extract one range per arm (AND or single Op), same Var+opfamily,
 * sort by lower, merge overlapping/adjacent; if single interval, return AND expr.
 */
static Expr *
try_range_union(List *orlist)
{
	List	   *ranges = NIL;
	VarRange   *common = NULL;
	ListCell   *lc;
	VarRange	merged;
	bool		single_interval = false;

	foreach(lc, orlist)
	{
		Expr	   *arg = (Expr *) lfirst(lc);
		List	   *arm_ranges;
		ListCell   *lc2;

		/*
		 * Extract one or more VarRanges per arm.  IN-lists give one
		 * range per array element; everything else gives a single
		 * range or fails.
		 */
		arm_ranges = extract_ranges_from_expr(arg);
		if (arm_ranges == NIL)
			goto fail;

		foreach(lc2, arm_ranges)
		{
			VarRange   *r = (VarRange *) lfirst(lc2);

			if (common == NULL)
				common = copy_var_range(r);
			else if (!same_var_and_family(r, common))
			{
				pfree(common);
				common = NULL;
				list_free(arm_ranges);
				goto fail;
			}
			ranges = lappend(ranges, r);
		}
		list_free(arm_ranges);	/* list cells only; ranges now belong to us */
	}
	if (ranges == NIL)
		goto fail;

	list_sort(ranges, range_lower_cmp);
	merge_ranges(ranges, &merged, &single_interval);

	if (!single_interval)
		goto fail;

	/* merged must be a valid superset of the union of inputs */
	if (!range_union_sanity_check(ranges, &merged))
		goto fail;

	/* Free list of VarRange* (each was palloc'd by copy_var_range) */
	foreach(lc, ranges)
		pfree(lfirst(lc));
	list_free(ranges);
	if (common != NULL)
		pfree(common);

	return build_and_from_range(&merged);

fail:
	foreach(lc, ranges)
		pfree(lfirst(lc));
	if (ranges != NIL)
		list_free(ranges);
	if (common != NULL)
		pfree(common);
	return NULL;
}


/*
 * Helper: classify an OpExpr on (Var, Const) into one of the kinds we care
 * about for not-equal simplification.  Returns the strategy plus the Var,
 * Const, and whether it's a "<>" operator.  Returns false on no match.
 *
 *   Strategy values:
 *     BTLessStrategyNumber       — Var <  Const   (or Const >  Var)
 *     BTLessEqualStrategyNumber  — Var <= Const   (or Const >= Var)
 *     BTGreaterStrategyNumber    — Var >  Const   (or Const <  Var)
 *     BTGreaterEqualStrategyNumber — Var >= Const (or Const <= Var)
 *     BTEqualStrategyNumber      — Var =  Const
 *     0 (with *is_neq=true)      — Var <> Const   (no btree strategy)
 */
static bool
classify_var_const_op(Expr *expr, Var **var, Const **c, int *strat,
					  bool *is_neq)
{
	OpExpr	   *op;
	Node	   *l,
			   *r;
	Oid			opfamily;
	Oid			opno;
	bool		var_on_left;

	if (!IsA(expr, OpExpr))
		return false;
	op = (OpExpr *) expr;
	if (list_length(op->args) != 2)
		return false;
	l = (Node *) linitial(op->args);
	r = (Node *) lsecond(op->args);
	if (IsA(l, RelabelType))
		l = (Node *) ((RelabelType *) l)->arg;
	if (IsA(r, RelabelType))
		r = (Node *) ((RelabelType *) r)->arg;

	if (IsA(l, Var) && IsA(r, Const))
	{
		*var = (Var *) l;
		*c = (Const *) r;
		var_on_left = true;
	}
	else if (IsA(l, Const) && IsA(r, Var))
	{
		*var = (Var *) r;
		*c = (Const *) l;
		var_on_left = false;
	}
	else
		return false;

	if ((*c)->constisnull)
		return false;
	if ((*var)->vartype != (*c)->consttype)
		return false;
	if (op_volatile(op->opno) != PROVOLATILE_IMMUTABLE)
		return false;

	*is_neq = false;

	/* Try btree opfamily classification */
	opfamily = get_btree_opfamily(op->opno);
	if (OidIsValid(opfamily))
	{
		opno = op->opno;
		if (!var_on_left)
			opno = get_commutator(opno);
		if (!OidIsValid(opno))
			return false;
		*strat = get_op_opfamily_strategy(opno, opfamily);
		return *strat != 0;
	}

	/*
	 * Not in any btree opfamily — could be "<>".  Detect by checking if
	 * the operator is the negator of an equality op.
	 */
	{
		Oid			negator = get_negator(op->opno);
		Oid			negfamily;

		if (!OidIsValid(negator))
			return false;
		negfamily = get_btree_opfamily(negator);
		if (!OidIsValid(negfamily))
			return false;
		if (get_op_opfamily_strategy(negator, negfamily) ==
			BTEqualStrategyNumber)
		{
			*is_neq = true;
			*strat = 0;
			return true;
		}
	}

	return false;
}

/*
 * try_or_neq_simplification
 *		Recognize OR-of-two patterns involving "<>" or paired strict
 *		inequalities and fold them.
 *
 *		x <> V OR x = V    =>  TRUE          (full universe except NULL)
 *		x = V OR x <> V    =>  TRUE          (same, order swapped)
 *		x < V OR x > V     =>  x <> V        (when V is the same value)
 *
 * Only fires for two-arm ORs.  Returns NULL if no pattern matches.
 */
static Expr *
try_or_neq_simplification(List *orlist)
{
	Expr	   *a,
			   *b;
	Var		   *va,
			   *vb;
	Const	   *ca,
			   *cb;
	int			sa,
				sb;
	bool		neq_a,
				neq_b;
	OpExpr	   *opa,
			   *opb;

	if (list_length(orlist) != 2)
		return NULL;

	a = (Expr *) linitial(orlist);
	b = (Expr *) lsecond(orlist);
	if (!classify_var_const_op(a, &va, &ca, &sa, &neq_a))
		return NULL;
	if (!classify_var_const_op(b, &vb, &cb, &sb, &neq_b))
		return NULL;

	/* Same Var */
	if (va->varno != vb->varno || va->varattno != vb->varattno)
		return NULL;
	if (va->vartype != vb->vartype)
		return NULL;

	/*
	 * Same constant value.  Get opfamily from whichever arm is a real btree
	 * operator (the other might be "<>" which isn't in any btree opfamily).
	 */
	{
		Oid			opfamily = InvalidOid;

		if (!neq_a)
			opfamily = get_btree_opfamily(((OpExpr *) a)->opno);
		if (!OidIsValid(opfamily) && !neq_b)
			opfamily = get_btree_opfamily(((OpExpr *) b)->opno);
		if (!OidIsValid(opfamily))
			return NULL;
		if (compare_consts(ca, cb, opfamily, ((OpExpr *) a)->inputcollid) != 0)
			return NULL;
	}

	opa = (OpExpr *) a;
	opb = (OpExpr *) b;

	/* Pattern 1: x <> V OR x = V  (in either order)  =>  TRUE */
	if ((neq_a && sb == BTEqualStrategyNumber) ||
		(neq_b && sa == BTEqualStrategyNumber))
		return (Expr *) makeBoolConst(true, false);

	/* Pattern 2: x < V OR x > V  =>  x <> V */
	if ((sa == BTLessStrategyNumber && sb == BTGreaterStrategyNumber) ||
		(sa == BTGreaterStrategyNumber && sb == BTLessStrategyNumber))
	{
		Oid			eqop;
		Oid			neqop;

		/* Find equality operator and its negator (<>) */
		{
			Oid			opfamily = get_btree_opfamily(opa->opno);

			eqop = get_opfamily_member(opfamily, va->vartype, va->vartype,
									   BTEqualStrategyNumber);
		}
		if (!OidIsValid(eqop))
			return NULL;
		neqop = get_negator(eqop);
		if (!OidIsValid(neqop))
			return NULL;

		return (Expr *) make_opclause(neqop, BOOLOID, false,
									  (Expr *) copyObject(va),
									  (Expr *) copyObject(ca),
									  InvalidOid, opa->inputcollid);
	}

	/* Pattern 3: x <= V OR x >= V  =>  TRUE  (already handled by range union) */

	(void) opb;					/* silence unused warning when no pattern */
	return NULL;
}


/*--------------------
 * The following code attempts to apply the inverse OR distributive law:
 *		((A AND B) OR (A AND C))  =>  (A AND (B OR C))
 * That is, locate OR clauses in which every subclause contains an
 * identical term, and pull out the duplicated terms.
 *
 * This may seem like a fairly useless activity, but it turns out to be
 * applicable to many machine-generated queries, and there are also queries
 * in some of the TPC benchmarks that need it.  This was in fact almost the
 * sole useful side-effect of the old prepqual code that tried to force
 * the query into canonical AND-of-ORs form: the canonical equivalent of
 *		((A AND B) OR (A AND C))
 * is
 *		((A OR A) AND (A OR C) AND (B OR A) AND (B OR C))
 * which the code was able to simplify to
 *		(A AND (A OR C) AND (B OR A) AND (B OR C))
 * thus successfully extracting the common condition A --- but at the cost
 * of cluttering the qual with many redundant clauses.
 *--------------------
 */

/*
 * find_duplicate_ors
 *	  Unified recursive boolean simplifier: AND (interval intersection),
 *	  OR (absorption + single-bound union + distributive law).
 *
 * Context is_check: WHERE (NULL = FALSE) vs CHECK (NULL = TRUE).
 * No DNF, no distributivity expansion; rollback on complex cases.
 * Returns the modified qualification.  AND/OR flatness is preserved.
 */
static Expr *
find_duplicate_ors(Expr *qual, bool is_check)
{
	/* Do not simplify if any operator is user-defined */
	if (qual_contains_unsupported_op_or_type(qual))
		return qual;
	/* Do not transform when NOT is present (for now). */
	if (qual_contains_not(qual))
		return qual;
	/* Avoid deep recursion and edge-case crashes. */
	if (qual_too_deep_to_simplify(qual))
		return qual;
	/* Avoid huge top-level AND/OR lists (for now?). */
	if (qual_too_large_to_simplify(qual))
		return qual;
	return simplify_boolean_tree(qual, is_check);
}

/*
 * process_duplicate_ors
 *	  Given a list of exprs which are ORed together, try to apply
 *	  the inverse OR distributive law.
 *
 * Returns the resulting expression (could be an AND clause, an OR
 * clause, or maybe even a single subexpression).
 */
static Expr *
process_duplicate_ors(List *orlist)
{
	List	   *reference = NIL;
	int			num_subclauses = 0;
	List	   *winners;
	List	   *neworlist;
	ListCell   *temp;

	/* OR of no inputs reduces to FALSE */
	if (orlist == NIL)
		return (Expr *) makeBoolConst(false, false);

	/* Single-expression OR just reduces to that expression */
	if (list_length(orlist) == 1)
		return (Expr *) linitial(orlist);

	/*
	 * Choose the shortest AND clause as the reference list --- obviously, any
	 * subclause not in this clause isn't in all the clauses. If we find a
	 * clause that's not an AND, we can treat it as a one-element AND clause,
	 * which necessarily wins as shortest.
	 */
	foreach(temp, orlist)
	{
		Expr	   *clause = (Expr *) lfirst(temp);

		if (is_andclause(clause))
		{
			List	   *subclauses = ((BoolExpr *) clause)->args;
			int			nclauses = list_length(subclauses);

			if (reference == NIL || nclauses < num_subclauses)
			{
				reference = subclauses;
				num_subclauses = nclauses;
			}
		}
		else
		{
			reference = list_make1(clause);
			break;
		}
	}

	/*
	 * Just in case, eliminate any duplicates in the reference list.
	 */
	reference = list_union(NIL, reference);

	/*
	 * Check each element of the reference list to see if it's in all the OR
	 * clauses.  Build a new list of winning clauses.
	 */
	winners = NIL;
	foreach(temp, reference)
	{
		Expr	   *refclause = (Expr *) lfirst(temp);
		bool		win = true;
		ListCell   *temp2;

		foreach(temp2, orlist)
		{
			Expr	   *clause = (Expr *) lfirst(temp2);

			if (is_andclause(clause))
			{
				if (!list_member(((BoolExpr *) clause)->args, refclause))
				{
					win = false;
					break;
				}
			}
			else
			{
				if (!equal(refclause, clause))
				{
					win = false;
					break;
				}
			}
		}

		if (win)
			winners = lappend(winners, refclause);
	}

	/*
	 * If no winners, we can't transform the OR
	 */
	if (winners == NIL)
		return make_orclause(orlist);

	/*
	 * Generate new OR list consisting of the remaining sub-clauses.
	 *
	 * If any clause degenerates to empty, then we have a situation like (A
	 * AND B) OR (A), which can be reduced to just A --- that is, the
	 * additional conditions in other arms of the OR are irrelevant.
	 *
	 * Note that because we use list_difference, any multiple occurrences of a
	 * winning clause in an AND sub-clause will be removed automatically.
	 */
	neworlist = NIL;
	foreach(temp, orlist)
	{
		Expr	   *clause = (Expr *) lfirst(temp);

		if (is_andclause(clause))
		{
			List	   *subclauses = ((BoolExpr *) clause)->args;

			subclauses = list_difference(subclauses, winners);
			if (subclauses != NIL)
			{
				if (list_length(subclauses) == 1)
					neworlist = lappend(neworlist, linitial(subclauses));
				else
					neworlist = lappend(neworlist, make_andclause(subclauses));
			}
			else
			{
				neworlist = NIL;	/* degenerate case, see above */
				break;
			}
		}
		else
		{
			if (!list_member(winners, clause))
				neworlist = lappend(neworlist, clause);
			else
			{
				neworlist = NIL;	/* degenerate case, see above */
				break;
			}
		}
	}

	/*
	 * Append reduced OR to the winners list, if it's not degenerate, handling
	 * the special case of one element correctly (can that really happen?).
	 * Also be careful to maintain AND/OR flatness in case we pulled up a
	 * sub-sub-OR-clause.
	 */
	if (neworlist != NIL)
	{
		if (list_length(neworlist) == 1)
			winners = lappend(winners, linitial(neworlist));
		else
			winners = lappend(winners, make_orclause(pull_ors(neworlist)));
	}

	/*
	 * And return the constructed AND clause, again being wary of a single
	 * element and AND/OR flatness.
	 */
	if (list_length(winners) == 1)
		return (Expr *) linitial(winners);
	else
		return make_andclause(pull_ands(winners));
}

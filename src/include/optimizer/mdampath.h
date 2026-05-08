/*-------------------------------------------------------------------------
 *
 * mdampath.h
 *	  prototypes for mdampath.c -- MDAM (Multi-Dimensional Access Method)
 *	  OR-clause optimization for multi-column B-tree indexes.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/mdampath.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MDAMPATH_H
#define MDAMPATH_H

#include "nodes/pathnodes.h"
#include "utils/fmgrprotos.h"

/*
 * Operator types for MDAM atoms.
 */
typedef enum MdamOpType
{
	MDAM_OP_EQ,					/* column = value */
	MDAM_OP_LT,					/* column < value */
	MDAM_OP_LE,					/* column <= value */
	MDAM_OP_GT,					/* column > value */
	MDAM_OP_GE,					/* column >= value */
	MDAM_OP_SAOP,				/* column IN (v1, v2, ...) */
	MDAM_OP_RANGE_EXCL,			/* low < column < high (exclusive both ends) */
	MDAM_OP_IS_ANYTHING			/* column is unconstrained */
} MdamOpType;

/*
 * MdamAtom -- a single predicate on one index column.
 */
typedef struct MdamAtom
{
	int			colno;			/* index column number (0-based) */
	MdamOpType	op;
	Datum		value;			/* for EQ, LT, LE, GT, GE */
	Datum	   *in_values;		/* for SAOP: palloc'd sorted array */
	int			n_in_values;	/* number of SAOP array values */
	Datum		range_lo;		/* for RANGE_EXCL: exclusive lower bound */
	Datum		range_hi;		/* for RANGE_EXCL: exclusive upper bound */
} MdamAtom;

/*
 * Single-column interval [lo, hi] representation
 */
typedef struct MdamInterval
{
	Datum		lo;
	Datum		hi;
	bool		lo_inclusive;
	bool		hi_inclusive;
	bool		lo_infinite;	/* no lower bound (-inf bound)? */
	bool		hi_infinite;	/* no upper bound (+inf bound)? */
} MdamInterval;

/*
 * Per-column comparison context (cached once during initialization)
 */
typedef struct MdamColContext
{
	int			colno;			/* 0-based index column number */
	Oid			typid;			/* column data type */
	int16		typlen;			/* type length */
	bool		typbyval;		/* type pass-by-value? */
	Oid			collid;			/* collation OID */
	Oid			opfamily;		/* btree operator family */
	Oid			eq_opr;			/* equality operator OID */
	Oid			lt_opr;			/* less-than operator OID */
	FmgrInfo	cmp_finfo;		/* btree ORDER proc */
} MdamColContext;

/*
 * Top-level context for one MDAM transformation
 */
typedef struct MdamContext
{
	PlannerInfo *root;
	RelOptInfo *rel;
	IndexOptInfo *index;
	int			nkeycolumns;
	MdamColContext *col_ctx;	/* array[nkeycolumns] */
	MemoryContext mdam_mcxt;	/* scratch memory context */
} MdamContext;

extern void generate_mdam_or_paths(PlannerInfo *root, RelOptInfo *rel);


#endif							/* MDAMPATH_H */

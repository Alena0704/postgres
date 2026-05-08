/*-------------------------------------------------------------------------
 *
 * appendorpath.c
 *	  Build Append/MergeAppend index scan paths from BitmapOrPath results.
 *
 * When generate_bitmap_or_paths() is called with append_or_path=true,
 * it produces BitmapOrPaths whose children are IndexPaths (instead of
 * BitmapIndexPaths).  This module takes those results and builds
 * Append (unsorted) and MergeAppend (sorted, forward and backward)
 * paths as alternatives to BitmapOr scans.
 *
 * Controlled by the enable_mdam GUC.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/appendorpath.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"

static void build_append_or_paths(PlannerInfo *root, RelOptInfo *rel,
								  List *index_subpaths, Selectivity or_selec);

/*
 * try_generate_append_or_path
 *
 * Called from generate_bitmap_or_paths() when append_or_path is true.
 * 'orpaths' is the list of BitmapOrPaths produced by the caller, where
 * each BitmapOrPath's bitmapquals are actually IndexPaths (built with
 * ST_INDEXSCAN instead of ST_BITMAPSCAN).
 *
 * For each BitmapOrPath, extract the IndexPath children and build:
 *   - An unsorted Append path (is_mdam = true)
 *   - A forward MergeAppend (IndexScan per arm)
 *   - A backward MergeAppend (IndexScan Backward per arm)
 *   - If only one subpath survives, emit it as a plain IndexPath
 */
void
try_generate_append_or_path(PlannerInfo *root, RelOptInfo *rel,
							List *orpaths)
{
	ListCell   *lc;

	/* GUC check */
	if (!enable_mdam)
		return;

	/*
	 * Only apply to plain base relations, not to partitioned tables or
	 * other complex cases where Append children belong to different rels.
	 */
	if (rel->reloptkind != RELOPT_BASEREL || rel->part_scheme != NULL)
		return;

	foreach(lc, orpaths)
	{
		Path	   *orpath = (Path *) lfirst(lc);
		BitmapOrPath *bmorpath;
		List	   *index_subpaths = NIL;
		ListCell   *j;
		bool		all_index = true;
		Selectivity or_selec;

		if (!IsA(orpath, BitmapOrPath))
			continue;

		bmorpath = (BitmapOrPath *) orpath;

		/*
		 * Extract IndexPath children.  Skip if any subpath is not an
		 * IndexPath or belongs to a different rel.
		 */
		foreach(j, bmorpath->bitmapquals)
		{
			Path	   *subpath = (Path *) lfirst(j);

			if (IsA(subpath, IndexPath) &&
				bms_equal(subpath->parent->relids, rel->relids))
			{
				index_subpaths = lappend(index_subpaths, subpath);
			}
			else
			{
				all_index = false;
				break;
			}
		}

		if (!all_index || index_subpaths == NIL)
			continue;

		/*
		 * Single subpath: emit as plain IndexPath, no Append needed.
		 */
		if (list_length(index_subpaths) == 1)
		{
			add_path(rel, (Path *) linitial(index_subpaths));
			continue;
		}

		or_selec = bmorpath->bitmapselectivity;

		build_append_or_paths(root, rel, index_subpaths, or_selec);
	}
}

/*
 * Rebuild a list of IndexPaths with the given ScanDirection and pathkeys.
 * Returns a new list of freshly-created IndexPaths.
 */
static List *
rebuild_index_subpaths(PlannerInfo *root, List *orig_subpaths,
					   List *pathkeys, ScanDirection scandir)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, orig_subpaths)
	{
		IndexPath  *orig = (IndexPath *) lfirst(lc);
		bool		indexonly = (orig->path.pathtype == T_IndexOnlyScan);
		IndexPath  *newpath;

		newpath = create_index_path(root,
									orig->indexinfo,
									orig->indexclauses,
									orig->indexorderbys,
									orig->indexorderbycols,
									pathkeys,
									scandir,
									indexonly,
									NULL, 1.0, false);
		result = lappend(result, newpath);
	}

	return result;
}

/*
 * build_append_or_paths
 *		Build unsorted Append, forward MergeAppend, and backward MergeAppend
 *		paths from a list of IndexPath subpaths (all on the same index).
 */
static void
build_append_or_paths(PlannerInfo *root, RelOptInfo *rel,
					  List *index_subpaths, Selectivity or_selec)
{
	AppendPath *appendpath;
	AppendPathInput input = {0};
	IndexPath  *first_ipath;
	IndexOptInfo *index;
	List	   *fwd_pathkeys;
	List	   *bwd_pathkeys;

	input.subpaths = index_subpaths;
	input.partial_subpaths = NIL;
	input.child_append_relid_sets = NIL;

	/*
	 * 1. Unsorted Append path.
	 */
	appendpath = create_append_path_ext(root, rel,
										input,
										NIL, NULL,
										0, false,
										-1, true,
										or_selec);
	appendpath->is_mdam = true;
	add_path(rel, (Path *) appendpath);

	/*
	 * Compute pathkeys from the index (not from subpaths, which may have
	 * empty pathkeys if they came from the bitmap infrastructure).
	 */
	first_ipath = (IndexPath *) linitial(index_subpaths);
	index = first_ipath->indexinfo;

	fwd_pathkeys = build_index_pathkeys(root, index, ForwardScanDirection);
	if (fwd_pathkeys == NIL)
		return;

	/*
	 * 2. Forward MergeAppend: rebuild subpaths with ForwardScanDirection
	 * so each has proper pathkeys.
	 */
	{
		List	   *fwd_subpaths;
		MergeAppendPath *mpath;

		fwd_subpaths = rebuild_index_subpaths(root, index_subpaths,
											  fwd_pathkeys,
											  ForwardScanDirection);
		mpath = create_merge_append_path_ext(root, rel,
											 fwd_subpaths,
											 NIL,
											 fwd_pathkeys,
											 NULL,
											 or_selec);
		add_path(rel, (Path *) mpath);
	}

	/*
	 * 3. Backward MergeAppend: rebuild each IndexPath with
	 * BackwardScanDirection so each produces rows in descending order,
	 * then MergeAppend merges them — no separate Sort needed.
	 */
	bwd_pathkeys = build_index_pathkeys(root, index, BackwardScanDirection);
	if (bwd_pathkeys == NIL)
		return;

	{
		List	   *bwd_subpaths;
		MergeAppendPath *mpath;

		bwd_subpaths = rebuild_index_subpaths(root, index_subpaths,
											  bwd_pathkeys,
											  BackwardScanDirection);
		mpath = create_merge_append_path_ext(root, rel,
											 bwd_subpaths,
											 NIL,
											 bwd_pathkeys,
											 NULL,
											 or_selec);
		add_path(rel, (Path *) mpath);
	}
}

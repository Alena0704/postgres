#!/usr/bin/env python3
"""
plan_tree.py -- visualise how the *standard* PostgreSQL planner forms a join
plan, for both DP (standard_join_search) and GEQO.

Standalone (unrelated to mcts_extreme / sweep.py).  It drives a live cluster
with the plan_trace extension (which installs the core join_rel_trace_hook /
geqo_gen_trace_hook) and renders, side by side:

  DP  (enable_geqo=off):
     * the final plan's join tree -- bushy joins (both inputs are joins) are
       highlighted, since DP freely builds bushy plans;
     * a per-level summary: how many joinrels the dynamic program built at each
       level (its exploration breadth) and the cheapest cost reached.

  GEQO (enable_geqo=on, low geqo_threshold):
     * the winning tour's join tree (GEQO can be bushy too -- merge_clump joins
       multi-relation clumps);
     * the genetic search by generation: best/child fitness curve, and which
       traits (genes) each child inherited from momma vs daddy.

With --analyze it also matches the executed plan's actual row counts.

Examples
--------
    python3 plan_tree.py --pgbin ~/pg/bin --db imdb --port 5499 --user alena \
        --sql 31c.sql --out /tmp/job31c_std
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import os
import shutil
import subprocess
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

JOIN_MARK = "@@@JOINS@@@"
GEQO_MARK = "@@@GEQO@@@"
PLAN_MARK = "@@@PLAN@@@"

GREEN = "#2ecc71"
GREEN_D = "#1e8449"
BUSHY_FC = "#fdf0d5"
BUSHY_EC = "#b9770e"
JOIN_FC = "#dfeffc"
JOIN_EC = "#2980b9"
BASE_FC = "#eafaf1"
BASE_EC = "#27ae60"


# ----------------------------------------------------------------------------
# psql plumbing
# ----------------------------------------------------------------------------
def build_script(query, geqo, geqo_threshold, analyze, extra_sets):
    q = query.strip().rstrip(";")
    lines = [
        "CREATE EXTENSION IF NOT EXISTS plan_trace;",
        "LOAD 'plan_trace';",
        "SET plan_trace.enabled = on;",
    ]
    if geqo is not None:                     # None = leave the planner's default
        lines.append(f"SET geqo = {'on' if geqo else 'off'};")
        lines.append(f"SET geqo_threshold = {geqo_threshold};")
    lines += [f"SET {s};" for s in extra_sets]
    lines += [
        r"\o /dev/null",
        f"EXPLAIN (COSTS on) {q};",          # plan only -> fills plan_trace
        r"\o",
        "COPY (SELECT id,relids,level,source,est_rows,cost,\"left\",\"right\","
        "bushy,eval_seq FROM plan_trace_joins() ORDER BY id) TO STDOUT WITH "
        "(FORMAT csv, HEADER true);",
        rf"\echo {GEQO_MARK}",
        "COPY (SELECT generation,momma,daddy,kid,kid_worth,best_worth FROM "
        "plan_trace_geqo() ORDER BY generation) TO STDOUT WITH "
        "(FORMAT csv, HEADER true);",
    ]
    if analyze:
        lines += [
            rf"\echo {PLAN_MARK}",
            r"\pset format unaligned",
            r"\pset tuples_only on",
            f"EXPLAIN (ANALYZE, FORMAT JSON, TIMING off, SUMMARY off) {q};",
            r"\pset tuples_only off",
            r"\pset format aligned",
        ]
    return "\n".join(lines) + "\n"


def run_psql(psql, db, user, host, port, script):
    proc = subprocess.run(
        [psql, "-h", host, "-p", str(port), "-d", db, "-U", user, "-X", "-q",
         "-v", "ON_ERROR_STOP=1", "-f", "-"],
        input=script, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"psql failed (exit {proc.returncode})")
    if proc.stderr.strip():
        sys.stderr.write(proc.stderr)
    return proc.stdout


def _f(s):
    s = (s or "").strip()
    try:
        return float(s) if s else None
    except ValueError:
        return None


def split_output(out):
    geqo_csv = plan_json = ""
    if PLAN_MARK in out:
        out, plan_json = out.split(PLAN_MARK, 1)
    if GEQO_MARK in out:
        out, geqo_csv = out.split(GEQO_MARK, 1)
    return out, geqo_csv, plan_json


# ----------------------------------------------------------------------------
# Parse the join-formation trace and rebuild the final plan tree
# ----------------------------------------------------------------------------
def parse_joins(csv_text):
    rows = []
    for r in csv.DictReader(io.StringIO(csv_text.strip())):
        rows.append({
            "relids": r["relids"] or "",
            "level": int(r["level"]),
            "source": r["source"],
            "est_rows": _f(r["est_rows"]),
            "cost": _f(r["cost"]),
            "left": r["left"] or "",
            "right": r["right"] or "",
            "bushy": (r.get("bushy") in ("t", "true", "True")),
            "eval_seq": int(r["eval_seq"]) if r.get("eval_seq") else 0,
        })
    return rows


def parse_plan_actual(plan_text):
    plan_text = plan_text.strip()
    if not plan_text:
        return {}
    try:
        data = json.loads(plan_text)
    except Exception:
        return {}
    if isinstance(data, list):
        data = data[0]
    root = data.get("Plan")
    if not root:
        return {}
    out = {}

    def walk(node):
        al = set()
        if node.get("Alias"):
            al.add(node["Alias"])
        elif node.get("Relation Name"):
            al.add(node["Relation Name"])
        for ch in node.get("Plans", []):
            al |= walk(ch)
        ar = node.get("Actual Rows")
        loops = node.get("Actual Loops", 1) or 1
        out[frozenset(al)] = ar * loops if ar is not None else None
        return al

    walk(root)
    return out


def final_tree(rows):
    """From all formed joinrels, keep the last (cheapest) record per relid-set,
    find the top (all rels), and walk down via left/right to the final plan
    tree.  Returns (top_key, nodes) where nodes[key] = row dict."""
    by_relset = {}
    for r in rows:
        by_relset[frozenset(r["relids"].split())] = r   # last wins
    if not by_relset:
        return None, {}
    top = max(by_relset, key=len)
    return top, by_relset


# ----------------------------------------------------------------------------
# Drawing: the final join tree (shared by DP and GEQO)
# ----------------------------------------------------------------------------
def draw_join_tree(ax, top, nodes, actual, title):
    # tidy layout over the final tree only
    pos, xleaf = {}, [0.0]

    def walk(key, depth):
        n = nodes.get(key)
        kids = []
        if n and (n["left"] or n["right"]):
            for side in (n["left"], n["right"]):
                k = frozenset(side.split())
                if k:
                    kids.append(k)
        if not kids:
            x = xleaf[0]
            xleaf[0] += 1.0
        else:
            x = sum(walk(k, depth + 1) for k in kids) / len(kids)
        pos[key] = (x, depth)
        return x

    walk(top, 0)
    maxd = max(d for _, d in pos.values())

    def y(d):
        return maxd - d

    for key, (x, d) in pos.items():
        n = nodes.get(key)
        if n and (n["left"] or n["right"]):
            for side in (n["left"], n["right"]):
                k = frozenset(side.split())
                if k in pos:
                    cx, cd = pos[k]
                    ax.plot([x, cx], [y(d), y(cd)], color="#999", lw=1.0, zorder=1)

    for key, (x, d) in pos.items():
        n = nodes.get(key)
        is_join = bool(n and (n["left"] or n["right"]))
        bushy = bool(n and n["bushy"])
        if not is_join:
            fc, ec = BASE_FC, BASE_EC
        elif bushy:
            fc, ec = BUSHY_FC, BUSHY_EC
        else:
            fc, ec = JOIN_FC, JOIN_EC
        label = "{" + " ".join(sorted(key)) + "}"
        if n:
            est = f"{n['est_rows']:.0f}" if n["est_rows"] is not None else "?"
            act = actual.get(key)
            card = f"est={est}" + (f" act={act:.0f}" if act is not None else "")
            label += f"\n{card}"
            if bushy:
                label += "\nBUSHY"
        ax.text(x, y(d), label, ha="center", va="center", fontsize=7.5,
                zorder=3, fontweight="bold" if bushy else "normal",
                bbox=dict(boxstyle="round,pad=0.35", fc=fc, ec=ec, lw=1.4))
    ax.set_title(title, fontsize=10)
    ax.axis("off")
    ax.margins(0.1)


# ----------------------------------------------------------------------------
# DP figure: final tree + per-level breadth
# ----------------------------------------------------------------------------
def _spine_keys(top, nodes):
    """Relid-sets on the final plan path (the chosen splits)."""
    spine, edges = set(), set()

    def walk(key):
        spine.add(key)
        n = nodes.get(key)
        if n and (n["left"] or n["right"]):
            for side in (n["left"], n["right"]):
                k = frozenset(side.split())
                if k:
                    edges.add((key, k))
                    walk(k)
    walk(top)
    return spine, edges


def draw_dp(rows, actual, out_png):
    """The DP lattice as a candidate view (like the MCTS search tree): every
    joinrel DP built, by level; the final plan is the green spine; bushy
    joinrels are amber."""
    top, nodes = final_tree(rows)
    if top is None:
        return
    spine, spine_edges = _spine_keys(top, nodes)
    bushy = {frozenset(r["relids"].split()) for r in rows if r["bushy"]}

    # positions: x = level (joinrel size), y = index within the level
    by_level = {}
    for k in nodes:
        by_level.setdefault(len(k), []).append(k)
    pos = {}
    for lev, ks in by_level.items():
        ks.sort(key=lambda k: (k not in spine, " ".join(sorted(k))))
        for i, k in enumerate(ks):
            pos[k] = (lev, i - (len(ks) - 1) / 2.0)

    maxn = max(len(v) for v in by_level.values())
    fig, ax = plt.subplots(figsize=(max(9, len(by_level) * 1.5),
                                    max(6, maxn * 0.16)))

    # the cheapest-split edges of the whole lattice (light), spine bold green
    for k, n in nodes.items():
        if not (n and (n["left"] or n["right"])):
            continue
        for side in (n["left"], n["right"]):
            ck = frozenset(side.split())
            if ck in pos and k in pos:
                on = (k, ck) in spine_edges
                x0, y0 = pos[k]
                x1, y1 = pos[ck]
                ax.plot([x0, x1], [y0, y1],
                        color=(GREEN_D if on else "#e3e3e3"),
                        lw=2.2 if on else 0.5, zorder=2 if on else 1)

    # MCTS-search styling: green = the chosen plan (Selection path), red = the
    # candidates DP enumerated but that did not make the final plan (eliminated).
    PRUNE_C = "#e74c3c"
    n_pruned = 0
    for k, (x, y) in pos.items():
        on_spine = k in spine
        is_bushy = k in bushy
        if on_spine:
            c, al, sz = GREEN_D, 1.0, 70
        elif is_bushy:
            c, al, sz = BUSHY_EC, 1.0, 45      # bushy: keep amber so it stands out
            n_pruned += 1
        else:
            c, al, sz = PRUNE_C, 0.45, 13      # eliminated candidate
            n_pruned += 1
        ax.scatter([x], [y], s=sz, c=c, alpha=al, zorder=3,
                   edgecolors="#333" if (on_spine or is_bushy) else "none",
                   linewidths=0.5)
        if on_spine or is_bushy:
            ax.annotate("{" + " ".join(sorted(k)) + "}", (x, y),
                        fontsize=6.5, ha="left", va="center",
                        xytext=(4, 0), textcoords="offset points",
                        color=GREEN_D if on_spine else BUSHY_EC)

    ax.set_xlabel("DP level (joinrel size)  →  full plan")
    ax.set_yticks([])
    ax.set_xticks(sorted(by_level))
    n_bushy = len(bushy)
    ax.set_title("How DP forms the plan -- candidate search (like the MCTS view)\n"
                 f"{len(nodes)} joinrels enumerated, {len(spine)} on the final "
                 f"plan, {n_pruned} eliminated, {n_bushy} bushy\n"
                 "green = chosen plan   red = eliminated candidate (didn't make "
                 "the plan)   amber = BUSHY (both inputs are joins)",
                 fontsize=9)
    from matplotlib.lines import Line2D
    ax.legend(handles=[
        Line2D([], [], marker="o", ls="", mfc=GREEN_D, mec="#333",
               label="chosen plan"),
        Line2D([], [], marker="o", ls="", mfc=PRUNE_C, mec="none", alpha=0.6,
               label="eliminated candidate"),
        Line2D([], [], marker="o", ls="", mfc=BUSHY_EC, mec="#333",
               label="bushy joinrel")],
        fontsize=7, loc="upper left", framealpha=0.9)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)


# ----------------------------------------------------------------------------
# GEQO figure: final tree + fitness curve + inheritance
# ----------------------------------------------------------------------------
def parse_geqo(csv_text):
    return list(csv.DictReader(io.StringIO(csv_text.strip()))) if csv_text.strip() else []


CROSS_C = "#5dade2"    # light blue: parent material actually used in crossover
NOUSE_C = "#ecf0f1"    # pale: new edge (not inherited -- mutation/forced)
BOTH_C = "#7f8c8d"     # join-edge present in both parents
MOM_C = "#8e44ad"      # edge inherited from momma only
DAD_C = "#16a085"      # edge inherited from daddy only
NEW_C = "#e67e22"      # new edge (in neither parent -- recombination/mutation)


def _edge_provenance(momma, daddy, kid):
    """Per child join-edge (adjacent pair in the tour): which parent it came
    from.  0 both, 1 momma-only, 2 daddy-only, 3 new.  This is the meaningful
    'inherited trait' for edge-recombination crossover (the default ERX)."""
    me = {frozenset((momma[i], momma[i + 1])) for i in range(len(momma) - 1)}
    de = {frozenset((daddy[i], daddy[i + 1])) for i in range(len(daddy) - 1)}
    out = []
    for i in range(len(kid) - 1):
        e = frozenset((kid[i], kid[i + 1]))
        inm, ind = e in me, e in de
        out.append(0 if (inm and ind) else 1 if inm else 2 if ind else 3)
    return out


def draw_geqo(rows, geqo_gens, actual, out_png):
    winner = [r for r in rows if r["source"] != "geqo_cand"]
    top, nodes = final_tree(winner)
    have_tree = top is not None

    fig = plt.figure(figsize=(15, 7.5))
    ax_tree = fig.add_axes([0.02, 0.08, 0.42, 0.84])
    ax_fit = fig.add_axes([0.52, 0.60, 0.45, 0.30])
    ax_inh = fig.add_axes([0.52, 0.09, 0.45, 0.38])

    if have_tree:
        draw_join_tree(ax_tree, top, nodes, actual,
                       "GEQO -- winning tour's join tree\n"
                       "amber = BUSHY (merge_clump joins multi-rel clumps)")
    else:
        ax_tree.axis("off")
        ax_tree.text(0.5, 0.5, "no GEQO tree captured", ha="center")

    if geqo_gens:
        import numpy as np
        g = [int(r["generation"]) for r in geqo_gens]
        best = [float(r["best_worth"]) for r in geqo_gens]
        kid = [float(r["kid_worth"]) for r in geqo_gens]

        # GEQO marks invalid tours with DBL_MAX (~1.8e308): plot the real
        # candidates on a readable scale and flag the invalid ones separately.
        INVALID = 1e300
        valid = [(gi, k) for gi, k in zip(g, kid) if k < INVALID]
        bad_g = [gi for gi, k in zip(g, kid) if k >= INVALID]
        if valid:
            vg, vk = zip(*valid)
            ax_fit.plot(vg, vk, ".", color="#95a5a6", ms=3.5,
                        label=f"candidate (child), n={len(valid)}")
        ax_fit.plot(g, best, "-", color="#c0392b", lw=1.8, label="pool best")

        # Winners: each point where the pool best improves = a new champion that
        # entered the pool; the global minimum = the tour GEQO finally returns.
        champs = [i for i in range(len(best))
                  if i == 0 or best[i] < best[i - 1] - 1e-6]
        if champs:
            ax_fit.plot([g[i] for i in champs], [best[i] for i in champs],
                        "D", color="#c0392b", ms=5, mec="white", mew=0.6,
                        label="new champion (best improved)")
        wbest = min(best)
        wgen = g[best.index(wbest)]
        ax_fit.axhline(wbest, ls="--", lw=0.9, color="#d4ac0d", zorder=0)
        ax_fit.plot([wgen], [wbest], "*", color="#d4ac0d", ms=18,
                    mec="#7d6608", mew=0.8, zorder=5,
                    label=f"WINNER = {wbest:,.0f} (gen {wgen})")

        # Park invalid tours at the top edge so they are visible but off the
        # fitness scale (they would otherwise blow up the log axis).
        if valid and bad_g:
            ytop = max(vk)
            ax_fit.plot(bad_g, [ytop] * len(bad_g), "x", color="#e74c3c",
                        ms=4, label=f"invalid tour (n={len(bad_g)})")

        ax_fit.set_xlabel("generation (phase)")
        ax_fit.set_ylabel("cost (total_cost)")
        ax_fit.set_yscale("log")
        ax_fit.set_title("GEQO fitness by generation "
                         "(candidates + champions + winner)", fontsize=10)
        ax_fit.legend(fontsize=6.5, loc="upper right", framealpha=0.9, ncol=2)

        # Inheritance heatmap: rows = generations, columns = child join-edges,
        # colour = which parent that join-adjacency came from (edge crossover).
        ncol = max(len(r["kid"].split()) - 1 for r in geqo_gens)
        prov = np.full((len(geqo_gens), max(ncol, 1)), np.nan)
        for gi, r in enumerate(geqo_gens):
            for pi, p in enumerate(_edge_provenance(r["momma"].split(),
                                                    r["daddy"].split(),
                                                    r["kid"].split())):
                prov[gi, pi] = p
        from matplotlib.colors import ListedColormap
        cmap = ListedColormap([BOTH_C, MOM_C, DAD_C, NEW_C])
        ax_inh.imshow(prov, aspect="auto", cmap=cmap, vmin=0, vmax=3,
                      interpolation="nearest", origin="lower")
        ax_inh.set_xlabel("join-edge slot = adjacent pair in the child's tour")
        ax_inh.set_ylabel("generation (bottom = early)")
        ax_inh.set_title("Inherited traits: where each child join-edge came from",
                         fontsize=10)
        from matplotlib.patches import Patch
        ax_inh.legend(handles=[Patch(fc=BOTH_C, label="both parents (kept)"),
                               Patch(fc=MOM_C, label="momma only"),
                               Patch(fc=DAD_C, label="daddy only"),
                               Patch(fc=NEW_C, label="new (recomb./mut.)")],
                      fontsize=6.5, loc="upper right", framealpha=0.9, ncol=2)
        # How to read it.
        frac_new = float(np.mean(prov == 3)) if np.isfinite(prov).any() else 0.0
        frac_both = float(np.mean(prov == 0)) if np.isfinite(prov).any() else 0.0
        ax_inh.text(
            0.0, -0.30,
            "Read per ROW = one generation's child; each CELL = one join-edge it "
            "carries.\nGrey = edge both parents already had (conserved); "
            "purple/teal = taken from one parent;\norange = brand-new edge from "
            "recombination/mutation.  More grey upward = population converging "
            f"(here {frac_both:.0%} kept, {frac_new:.0%} new).",
            transform=ax_inh.transAxes, fontsize=6.6, va="top", color="#333333")
    else:
        for ax in (ax_fit, ax_inh):
            ax.axis("off")
        ax_fit.text(0.5, 0.5, "no GEQO generations (below geqo_threshold?)",
                    ha="center", fontsize=9)

    fig.suptitle("How GEQO forms the plan", fontsize=12, y=0.99)
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)


# ----------------------------------------------------------------------------
def draw_geqo_lattice(rows, actual, out_png):
    """GEQO candidate lattice (the MCTS-search-style view): every joinrel GEQO
    evaluated across ALL tours, placed by size; a candidate's opacity/size grows
    with how often it was explored; the winning tour is the green spine; bushy
    joinrels are amber."""
    winner = [r for r in rows if r["source"] == "geqo"]
    cand = [r for r in rows if r["source"] in ("geqo", "geqo_cand")]
    if not cand:
        return None
    top, wnodes = final_tree(winner)
    spine, spine_edges = (_spine_keys(top, wnodes) if top else (set(), set()))

    from collections import Counter
    freq = Counter(frozenset(r["relids"].split()) for r in cand)
    bushy = {frozenset(r["relids"].split()) for r in cand if r["bushy"]}
    split = {}
    for r in cand:
        k = frozenset(r["relids"].split())
        if k not in split and r["left"] and r["right"]:
            split[k] = (frozenset(r["left"].split()),
                        frozenset(r["right"].split()))

    by_level = {}
    for k in freq:
        by_level.setdefault(len(k), []).append(k)
    pos = {}
    for lev, ks in by_level.items():
        ks.sort(key=lambda k: (k not in spine, -freq[k], " ".join(sorted(k))))
        for i, k in enumerate(ks):
            pos[k] = (lev, i - (len(ks) - 1) / 2.0)

    maxn = max(len(v) for v in by_level.values())
    fmax = max(freq.values())
    fig, ax = plt.subplots(figsize=(max(9, len(by_level) * 1.5),
                                    min(16, max(6, maxn * 0.16))))

    drawn = set()
    for k, (a, b) in split.items():
        if k not in pos:
            continue
        for ck in (a, b):
            if ck in pos and (k, ck) not in drawn:
                drawn.add((k, ck))
                on = (k, ck) in spine_edges
                x0, y0 = pos[k]
                x1, y1 = pos[ck]
                ax.plot([x0, x1], [y0, y1],
                        color=(GREEN_D if on else "#ececec"),
                        lw=2.2 if on else 0.4, zorder=3 if on else 1)

    for k, (x, y) in pos.items():
        on_spine = k in spine
        is_bushy = k in bushy
        if on_spine:
            c, al, sz = GREEN_D, 1.0, 70
        elif is_bushy:
            c, al, sz = BUSHY_EC, 1.0, 46
        else:
            c = "#9fb6c4"
            al = 0.25 + 0.65 * (freq[k] / fmax)   # darker = explored more often
            sz = 10 + 34 * (freq[k] / fmax)
        ax.scatter([x], [y], s=sz, c=c, alpha=al, zorder=4,
                   edgecolors="#333" if (on_spine or is_bushy) else "none",
                   linewidths=0.5)
        if on_spine:        # label only the winning tour; bushy stay amber dots
            ax.annotate("{" + " ".join(sorted(k)) + "}", (x, y),
                        fontsize=6.5, ha="left", va="center",
                        xytext=(4, 0), textcoords="offset points",
                        color=GREEN_D)

    ax.set_xlabel("joinrel size (number of base rels)  →  full plan")
    ax.set_yticks([])
    ax.set_xticks(sorted(by_level))
    nuniq, ntot = len(freq), sum(freq.values())
    ax.set_title("How GEQO forms the plan -- candidate lattice "
                 f"({nuniq} distinct joinrels explored over {ntot} tour-builds, "
                 f"{len(bushy)} bushy)\n"
                 "green spine = winning tour   amber = BUSHY (GEQO can build bushy "
                 "via clumping)   grey = explored candidate (darker = more often)",
                 fontsize=9)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return out_png


def draw_geqo_crossover(geqo_gens, out_png):
    """Separate graph: per GA step (generation), highlight in light blue which
    join-edges of the child were taken from a parent -- i.e. the material that
    was actually used in the crossover -- vs new edges (mutation/forced)."""
    if not geqo_gens:
        return None
    import numpy as np
    # ERX preserves parental edges, so "from a parent vs new" is ~100% trivially.
    # The informative crossover signal is what the child pulled in from the
    # *second* parent (daddy-only edges): that is the recombination's actual
    # contribution at each step.  Highlight those in light blue.
    ncol = max(len(r["kid"].split()) - 1 for r in geqo_gens)
    used = np.full((len(geqo_gens), max(ncol, 1)), np.nan)
    for gi, r in enumerate(geqo_gens):
        prov = _edge_provenance(r["momma"].split(), r["daddy"].split(),
                                r["kid"].split())
        for pi, p in enumerate(prov):
            used[gi, pi] = 1 if p == 2 else 0   # 1 = from daddy (crossover-in)

    fig, ax = plt.subplots(figsize=(max(7, ncol * 0.55),
                                    max(4, len(geqo_gens) * 0.035)))
    from matplotlib.colors import ListedColormap
    ax.imshow(used, aspect="auto", cmap=ListedColormap([NOUSE_C, CROSS_C]),
              vmin=0, vmax=1, interpolation="nearest", origin="lower")
    ax.set_xlabel("child join-edge slot (adjacent pair in the tour)")
    ax.set_ylabel("GA step / generation (bottom = early)")
    frac = float(np.nanmean(used)) if np.isfinite(used).any() else 0.0
    ax.set_title("GEQO -- what the crossover pulled in at each step\n"
                 "light blue = join-edge brought in from the 2nd parent (daddy) by "
                 "the crossover;\npale = kept from momma / shared.  "
                 f"{frac:.0%} of child edges came from the crossover partner",
                 fontsize=10)
    from matplotlib.patches import Patch
    ax.legend(handles=[Patch(fc=CROSS_C, label="brought in by crossover (from daddy)"),
                       Patch(fc=NOUSE_C, label="kept from momma / shared")],
              fontsize=7, loc="upper right", framealpha=0.9)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return out_png


def emit_geqo(args, joins, gens, actual):
    """Write the GEQO figures and return the paths actually produced:
    the GA panel, the candidate lattice (MCTS-search style), and the
    separate crossover graph."""
    out = []
    p = f"{args.out}_geqo.png"
    draw_geqo(joins, gens, actual, p)
    out.append(p)
    for fn, name in ((draw_geqo_lattice(joins, actual,
                                        f"{args.out}_geqo_lattice.png"),
                      "lattice"),
                     (draw_geqo_crossover(gens,
                                          f"{args.out}_geqo_crossover.png"),
                      "crossover")):
        if fn:
            out.append(fn)
    return out


def one_run(args, psql, geqo):
    # geqo: True forces GEQO (low threshold), False forces DP, None leaves the
    # planner's own decision (driven by the geqo / geqo_threshold GUCs, which the
    # user can still bend via --set, e.g. --set geqo_threshold=2).
    threshold = 2 if geqo else 1000000
    out = run_psql(psql, args.db, args.user, args.host, args.port,
                   build_script(args.query_sql, geqo, threshold,
                                args.analyze, args.set))
    join_csv, geqo_csv, plan_json = split_output(out)
    return (parse_joins(join_csv), parse_geqo(geqo_csv),
            parse_plan_actual(plan_json))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--sql")
    src.add_argument("--query")
    ap.add_argument("--db", default=os.environ.get("DB", "postgres"))
    ap.add_argument("--user", default=os.environ.get("PGUSER", "postgres"))
    ap.add_argument("--host", default=os.environ.get("PGHOST", "/tmp"))
    ap.add_argument("--port", default=os.environ.get("PGPORT", "5432"))
    ap.add_argument("--pgbin", default=os.environ.get("PGBIN", ""))
    ap.add_argument("--set", action="append", default=[], metavar="k=v")
    ap.add_argument("--analyze", action="store_true",
                    help="also run EXPLAIN ANALYZE for actual cardinalities")
    ap.add_argument("--only", choices=["natural", "dp", "geqo", "both"],
                    default="natural",
                    help="natural (default): one picture for whatever the "
                         "planner actually used (DP for few rels, GEQO for "
                         ">= geqo_threshold; honours --set geqo/geqo_threshold). "
                         "dp/geqo: force that mode. both: force both.")
    ap.add_argument("--out", default="plan")
    ap.add_argument("--slices", action="store_true",
                    help="emit the full GEQO/DP slice analytics (separate "
                         "figures); forces one DP and one GEQO run")
    args = ap.parse_args()

    args.query_sql = open(args.sql).read() if args.sql else args.query
    if not args.query_sql or not args.query_sql.strip():
        ap.error("empty query")
    psql = (os.path.join(args.pgbin, "psql") if args.pgbin
            else shutil.which("psql"))
    if not psql or not os.path.exists(psql):
        raise SystemExit("psql not found; pass --pgbin or set PGBIN")

    written = []
    if args.slices:
        import plan_slices
        dpj, _, actual = one_run(args, psql, geqo=False)
        gj, gens, _ = one_run(args, psql, geqo=True)
        if dpj:
            draw_dp(dpj, actual, f"{args.out}_dp.png")
            written.append(f"{args.out}_dp.png")
        if gj or gens:
            written += emit_geqo(args, gj, gens, actual)
        written += plan_slices.emit_all(args, dpj, gj, gens, actual, args.out)
        print(f"slices: DP {len(dpj)} joinrels, GEQO {len(gens)} generations / "
              f"{sum(1 for r in gj if r['source'] == 'geqo_cand')} candidates")
        if written:
            print(f"\n[wrote {len(written)} files: {', '.join(written)}]")
        return

    if args.only == "natural":
        # No force: let the planner pick by geqo / geqo_threshold (default 12).
        joins, gens, actual = one_run(args, psql, geqo=None)
        is_geqo = bool(gens) or any(r["source"] == "geqo" for r in joins)
        if not joins and not gens:
            print("natural: no formation trace "
                  "(is plan_trace installed / enabled?)")
        elif is_geqo:
            for p in emit_geqo(args, joins, gens, actual):
                written.append(p)
            win = [r for r in joins if r["source"] == "geqo"]
            print(f"natural -> GEQO (>= geqo_threshold): {len(gens)} "
                  f"generations, {len(win)} joinrels in the winning tour "
                  f"({sum(1 for r in win if r['bushy'])} bushy), "
                  f"{sum(1 for r in joins if r['source'] == 'geqo_cand')} "
                  f"candidate joinrels explored")
        else:
            p = f"{args.out}_dp.png"
            draw_dp(joins, actual, p)
            written.append(p)
            print(f"natural -> DP (< geqo_threshold, or GEQO fell back to DP): "
                  f"{len(joins)} joinrels formed, "
                  f"{sum(1 for r in joins if r['bushy'])} bushy")

    if args.only in ("dp", "both"):
        joins, _, actual = one_run(args, psql, geqo=False)
        if joins:
            p = f"{args.out}_dp.png"
            draw_dp(joins, actual, p)
            written.append(p)
            print(f"DP: {len(joins)} joinrels formed, "
                  f"{sum(1 for r in joins if r['bushy'])} bushy")
        else:
            print("DP: no formation trace (is plan_trace installed / enabled?)")
    if args.only in ("geqo", "both"):
        joins, gens, actual = one_run(args, psql, geqo=True)
        if joins or gens:
            for p in emit_geqo(args, joins, gens, actual):
                written.append(p)
            win = [r for r in joins if r["source"] == "geqo"]
            print(f"GEQO: {len(gens)} generations, {len(win)} joinrels in the "
                  f"winning tour ({sum(1 for r in win if r['bushy'])} bushy), "
                  f"{sum(1 for r in joins if r['source'] == 'geqo_cand')} "
                  f"candidate joinrels explored")
        else:
            print("GEQO: no trace (too few rels for geqo_threshold, or fell to DP)")

    if written:
        print(f"\n[wrote {', '.join(written)}]")


if __name__ == "__main__":
    main()

"""Plan-formation slice analytics: GEQO *sampled* subplans vs DP *optimal*
subplans.  Every analysis is written to its OWN png (and the slice table to
CSV), driven by the trace from the plan_trace extension (see plan_tree.py).

The joinrel rows carry eval_seq (one tick per GEQO tour evaluation), which lets
us attribute every candidate subplan to the fitness of the tour it came from --
so we can split "frequent in good tours" vs "frequent in bad tours", track a
slice across generations, etc.
"""
import csv
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

from plan_tree import (final_tree, _spine_keys, _edge_provenance,
                       draw_join_tree, GREEN_D, BUSHY_EC,
                       BOTH_C, MOM_C, DAD_C, NEW_C)

WIN_C = "#d4ac0d"     # in the GEQO winning tour
DP_C = "#2e86c1"      # in the DP optimal plan
NOISE_C = "#95a5a6"   # sampled, in neither


def _key(relids):
    return frozenset(relids.split())


# ----------------------------------------------------------------------------
# Analysis
# ----------------------------------------------------------------------------
def analyze_geqo(geqo_joins, gens):
    """Per-subplan slice stats from GEQO candidate + winner joinrels."""
    winner = [r for r in geqo_joins if r["source"] == "geqo"]
    allrows = [r for r in geqo_joins if r["source"] in ("geqo", "geqo_cand")]

    by_eval = defaultdict(list)
    for r in allrows:
        by_eval[r["eval_seq"]].append(r)
    maxlevel = max((r["level"] for r in allrows), default=0)

    # tour fitness = cost of the tour's full-rel joinrel (the completed plan)
    tour_fit = {}
    for ev, rs in by_eval.items():
        top = max(rs, key=lambda r: r["level"])
        if top["level"] == maxlevel and top["cost"] is not None and top["cost"] >= 0:
            tour_fit[ev] = top["cost"]

    # eval_seq -> generation (the first pool_size evals are the random init pool)
    G = len(gens)
    total = max((r["eval_seq"] for r in allrows), default=0)
    pool_size = max(total - G, 0)

    def gen_of(ev):
        return ev - pool_size - 1 if ev > pool_size else None

    win_spine = set()
    wtop, wnodes = final_tree(winner)
    if wtop:
        win_spine, _ = _spine_keys(wtop, wnodes)

    # good vs bad tours: split completed tours at the median fitness
    fits_sorted = sorted(tour_fit.values())
    med = fits_sorted[len(fits_sorted) // 2] if fits_sorted else None
    good_evals = {e for e, c in tour_fit.items() if med is not None and c <= med}
    bad_evals = {e for e, c in tour_fit.items() if med is not None and c > med}

    seen = defaultdict(set)
    for r in allrows:
        seen[_key(r["relids"])].add(r["eval_seq"])

    stats = {}
    for k, evs in seen.items():
        fits = [tour_fit[e] for e in evs if e in tour_fit]
        gseen = [gen_of(e) for e in evs if gen_of(e) is not None]
        stats[k] = {
            "size": len(k),
            "frequency": len(evs),
            "freq_good": len(evs & good_evals),
            "freq_bad": len(evs & bad_evals),
            "best_cost_seen": min(fits) if fits else None,
            "avg_cost_when_seen": float(np.mean(fits)) if fits else None,
            "in_winner": k in win_spine,
            "first_gen": min(gseen) if gseen else None,
            "last_gen": max(gseen) if gseen else None,
            "evals": evs,
        }
    return {"stats": stats, "tour_fit": tour_fit, "maxlevel": maxlevel,
            "pool_size": pool_size, "G": G, "gen_of": gen_of,
            "win_spine": win_spine, "good": good_evals, "bad": bad_evals}


def analyze_dp(dp_joins):
    top, nodes = final_tree(dp_joins)
    spine = set()
    if top:
        spine, _ = _spine_keys(top, nodes)
    by_level = defaultdict(list)
    for r in dp_joins:
        if r["cost"] is not None and r["cost"] >= 0:
            by_level[r["level"]].append((_key(r["relids"]), r["cost"], r["bushy"]))
    return {"spine": spine, "nodes": nodes, "top": top, "by_level": by_level}


# ----------------------------------------------------------------------------
# GEQO slice table (CSV + rendered top-N png)
# ----------------------------------------------------------------------------
SLICE_COLS = ["joinrel", "size", "frequency", "freq_good", "freq_bad",
              "best_cost_seen", "avg_cost_when_seen", "in_winner", "in_DP",
              "first_gen", "last_gen"]


def _row_for(k, s):
    return [
        "{" + " ".join(sorted(k)) + "}", s["size"], s["frequency"],
        s["freq_good"], s["freq_bad"],
        f"{s['best_cost_seen']:.0f}" if s["best_cost_seen"] is not None else "",
        f"{s['avg_cost_when_seen']:.0f}" if s["avg_cost_when_seen"] is not None else "",
        "Y" if s["in_winner"] else "", "Y" if s["in_DP"] else "",
        s["first_gen"] if s["first_gen"] is not None else "",
        s["last_gen"] if s["last_gen"] is not None else "",
    ]


def slice_table(stats, out_csv, out_png, top_n=30):
    rows = sorted(stats.items(), key=lambda kv: -kv[1]["frequency"])
    with open(out_csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(SLICE_COLS)
        for k, s in rows:
            w.writerow(_row_for(k, s))

    show = rows[:top_n]
    fig, ax = plt.subplots(figsize=(13, 0.32 * len(show) + 1.2))
    ax.axis("off")
    cells = [_row_for(k, s) for k, s in show]
    tbl = ax.table(cellText=cells, colLabels=SLICE_COLS, loc="center",
                   cellLoc="center")
    tbl.auto_set_font_size(False)
    tbl.set_fontsize(7)
    tbl.scale(1, 1.2)
    for j, k in enumerate(SLICE_COLS):           # header style
        tbl[0, j].set_facecolor("#34495e")
        tbl[0, j].get_text().set_color("white")
    for i, (k, s) in enumerate(show, start=1):   # tint winner / DP rows
        if s["in_winner"]:
            for j in range(len(SLICE_COLS)):
                tbl[i, j].set_facecolor("#fcf3cf")
        elif s["in_DP"]:
            for j in range(len(SLICE_COLS)):
                tbl[i, j].set_facecolor("#eaf2f8")
    ax.set_title(f"GEQO subplan slices -- top {len(show)} by frequency "
                 "(yellow = in winner, blue = in DP optimal)", fontsize=11)
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)


# ----------------------------------------------------------------------------
# GEQO slice scatter: frequency vs best tour cost; colour = winner/DP; size=rels
# ----------------------------------------------------------------------------
def slice_scatter(stats, out_png):
    pts = [(s["frequency"], s["best_cost_seen"], s["size"], s["in_winner"],
            s["in_DP"]) for s in stats.values() if s["best_cost_seen"] is not None]
    if not pts:
        return None
    fig, ax = plt.subplots(figsize=(9, 6.5))
    for freq, cost, size, win, dp in pts:
        if win:
            c, z, ec = WIN_C, 5, "#7d6608"
        elif dp:
            c, z, ec = DP_C, 4, "#1a5276"
        else:
            c, z, ec = NOISE_C, 2, "none"
        ax.scatter([freq], [cost], s=12 + size * 9, c=c, alpha=0.75, zorder=z,
                   edgecolors=ec, linewidths=0.5)
    ax.set_xlabel("frequency (number of tours the subplan appeared in)")
    ax.set_ylabel("best tour cost where it appeared")
    ax.set_yscale("log")
    from matplotlib.lines import Line2D
    ax.legend(handles=[
        Line2D([], [], marker="o", ls="", mfc=WIN_C, mec="#7d6608",
               label="in GEQO winner"),
        Line2D([], [], marker="o", ls="", mfc=DP_C, mec="#1a5276",
               label="in DP optimal"),
        Line2D([], [], marker="o", ls="", mfc=NOISE_C, mec="none",
               label="sampled only (noise)")],
        fontsize=8, loc="upper right")
    ax.set_title("GEQO subplan slices: building blocks vs noise\n"
                 "x = how often sampled, y = best tour it helped build, "
                 "size = joinrel size", fontsize=10)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return out_png


# ----------------------------------------------------------------------------
# GEQO survival: popular slices across generations
# ----------------------------------------------------------------------------
def slice_survival(A, out_png, top_n=30):
    stats, gen_of, G = A["stats"], A["gen_of"], A["G"]
    if G == 0:
        return None
    top = sorted(stats.items(), key=lambda kv: -kv[1]["frequency"])[:top_n]
    M = np.zeros((len(top), G))
    for i, (k, s) in enumerate(top):
        for e in s["evals"]:
            g = gen_of(e)
            if g is not None and 0 <= g < G:
                M[i, g] += 1
    fig, ax = plt.subplots(figsize=(12, 0.3 * len(top) + 1.5))
    ax.imshow(M, aspect="auto", cmap="viridis", interpolation="nearest")
    ax.set_yticks(range(len(top)))
    ax.set_yticklabels(["{" + " ".join(sorted(k)) + "}" for k, _ in top],
                       fontsize=6)
    ax.set_xlabel("generation")
    ax.set_title("Do good building blocks stick? -- popular GEQO slices over "
                 "generations\n(bright = the slice appears in that generation's "
                 "tour; persistent rows = blocks that locked in)", fontsize=10)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return out_png


# ----------------------------------------------------------------------------
# GEQO winner-edge provenance (a single row: where each winner edge came from)
# ----------------------------------------------------------------------------
def winner_provenance(gens, out_png):
    if not gens:
        return None
    # the generation that produced the best child is our proxy for the winner
    best = min(gens, key=lambda r: float(r["kid_worth"]))
    prov = _edge_provenance(best["momma"].split(), best["daddy"].split(),
                            best["kid"].split())
    from matplotlib.colors import ListedColormap
    from matplotlib.patches import Patch
    fig, ax = plt.subplots(figsize=(max(7, len(prov) * 0.5), 1.8))
    ax.imshow(np.array(prov).reshape(1, -1), aspect="auto",
              cmap=ListedColormap([BOTH_C, MOM_C, DAD_C, NEW_C]),
              vmin=0, vmax=3, interpolation="nearest")
    ax.set_yticks([])
    ax.set_xlabel("winning tour join-edge (adjacent pair)")
    ax.legend(handles=[Patch(fc=BOTH_C, label="both parents"),
                       Patch(fc=MOM_C, label="momma only"),
                       Patch(fc=DAD_C, label="daddy only"),
                       Patch(fc=NEW_C, label="new (recomb./mut.)")],
              fontsize=7, loc="upper center", bbox_to_anchor=(0.5, -0.6), ncol=4)
    ax.set_title(f"Winner-edge provenance (gen {best['generation']}, "
                 f"cost {float(best['kid_worth']):.0f}): where each edge of the "
                 "winning tour came from", fontsize=9)
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return out_png


# ----------------------------------------------------------------------------
# DP frontier: cost distribution of candidates per level, chosen highlighted
# ----------------------------------------------------------------------------
def dp_frontier(D, out_png):
    levels = sorted(D["by_level"])
    if not levels:
        return None
    data = [[c for _, c, _ in D["by_level"][lv]] for lv in levels]
    fig, ax = plt.subplots(figsize=(max(8, len(levels) * 1.1), 6))
    parts = ax.violinplot(data, positions=levels, widths=0.8,
                          showextrema=False)
    for b in parts["bodies"]:
        b.set_facecolor("#d6dbdf")
        b.set_alpha(0.7)
    for lv in levels:
        chosen = [c for k, c, _ in D["by_level"][lv] if k in D["spine"]]
        if chosen:
            ax.scatter([lv], [min(chosen)], c=GREEN_D, s=55, zorder=5,
                       edgecolors="#145a32", label="chosen" if lv == levels[0] else "")
    ax.set_yscale("log")
    ax.set_xlabel("joinrel size (DP level)")
    ax.set_ylabel("cheapest_total_path cost")
    ax.set_xticks(levels)
    ax.legend(fontsize=8)
    ax.set_title("DP frontier by level: cost of all candidates vs the chosen "
                 "joinrel\n(narrow violin + low green dot = chosen was clearly "
                 "cheapest; green inside a fat violin = it barely won)",
                 fontsize=10)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return out_png


# ----------------------------------------------------------------------------
# DP rank of the chosen subplan among same-size candidates
# ----------------------------------------------------------------------------
def dp_rank(D, out_png):
    levels = sorted(D["by_level"])
    xs, ys, ns = [], [], []
    for lv in levels:
        cand = sorted(D["by_level"][lv], key=lambda t: t[1])
        chosen_idx = [i for i, (k, _, _) in enumerate(cand) if k in D["spine"]]
        if chosen_idx:
            xs.append(lv)
            ys.append(chosen_idx[0] + 1)        # rank 1 = cheapest
            ns.append(len(cand))
    if not xs:
        return None
    fig, ax = plt.subplots(figsize=(max(8, len(xs) * 1.1), 5))
    ax.bar(xs, ys, color="#5499c7", zorder=3)
    for x, y, n in zip(xs, ys, ns):
        ax.text(x, y, f"{y}/{n}", ha="center", va="bottom", fontsize=7)
    ax.axhline(1, ls="--", color=GREEN_D, lw=1)
    ax.set_xlabel("joinrel size (DP level)")
    ax.set_ylabel("rank of chosen subplan (1 = cheapest)")
    ax.set_xticks(xs)
    ax.set_title("Was the DP choice obvious? Rank of the chosen joinrel among "
                 "same-size candidates\n(rank 1 everywhere = stable DP path; a "
                 "high bar = the winning subplan was not the locally cheapest)",
                 fontsize=10)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return out_png


# ----------------------------------------------------------------------------
# DP candidate counts per level (created / on final path)
# ----------------------------------------------------------------------------
def dp_counts(D, out_png):
    levels = sorted(D["by_level"])
    if not levels:
        return None
    created = [len(D["by_level"][lv]) for lv in levels]
    final = [sum(1 for k, _, _ in D["by_level"][lv] if k in D["spine"])
             for lv in levels]
    elim = [c - f for c, f in zip(created, final)]
    fig, ax = plt.subplots(figsize=(max(8, len(levels) * 1.1), 5))
    ax.bar(levels, elim, color="#e74c3c", label="eliminated (not in final plan)",
           zorder=3)
    ax.bar(levels, final, bottom=elim, color=GREEN_D, label="on final plan",
           zorder=3)
    for lv, c in zip(levels, created):
        ax.text(lv, c, str(c), ha="center", va="bottom", fontsize=7)
    ax.set_xlabel("joinrel size (DP level)")
    ax.set_ylabel("joinrels enumerated")
    ax.set_xticks(levels)
    ax.legend(fontsize=8)
    ax.set_title("DP enumeration per level: how many joinrels built, how many "
                 "survive into the final plan", fontsize=10)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return out_png


# ----------------------------------------------------------------------------
# DP bushy vs linear per level (stacked)
# ----------------------------------------------------------------------------
def dp_bushy(D, out_png):
    levels = sorted(D["by_level"])
    if not levels:
        return None
    bushy = [sum(1 for _, _, b in D["by_level"][lv] if b) for lv in levels]
    linear = [len(D["by_level"][lv]) - b for lv, b in zip(levels, bushy)]
    fig, ax = plt.subplots(figsize=(max(8, len(levels) * 1.1), 5))
    ax.bar(levels, linear, color="#aeb6bf", label="linear (≥1 input is a base rel)",
           zorder=3)
    ax.bar(levels, bushy, bottom=linear, color=BUSHY_EC,
           label="bushy (both inputs are joins)", zorder=3)
    for lv, b in zip(levels, bushy):
        if b:
            ax.text(lv, linear[levels.index(lv)] + b, str(b), ha="center",
                    va="bottom", fontsize=7, color=BUSHY_EC)
    ax.set_xlabel("joinrel size (DP level)")
    ax.set_ylabel("joinrels")
    ax.set_xticks(levels)
    ax.legend(fontsize=8)
    ax.set_title("DP bushy vs linear joinrels per level "
                 "(at which sizes does DP build bushy, and how many?)",
                 fontsize=10)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return out_png


# ----------------------------------------------------------------------------
# DP chosen plan tree with costs / cardinality
# ----------------------------------------------------------------------------
def dp_tree(dp_joins, actual, out_png):
    top, nodes = final_tree(dp_joins)
    if top is None:
        return None
    fig, ax = plt.subplots(figsize=(11, 7))
    draw_join_tree(ax, top, nodes, actual,
                   "DP final plan tree (cost / est rows per joinrel)")
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return out_png


# ----------------------------------------------------------------------------
# DP vs GEQO overlap (which subsets live in DP-final / GEQO-winner / sampled)
# ----------------------------------------------------------------------------
def compare_overlap(A, D, out_png, top_n=40):
    stats = A["stats"]
    sampled = {k for k in stats if len(k) >= 2}
    winner = {k for k in A["win_spine"] if len(k) >= 2}
    dp_final = {k for k in D["spine"] if len(k) >= 2}
    freq = set(k for k, _ in sorted(stats.items(),
                                    key=lambda kv: -kv[1]["frequency"])[:top_n])
    groups = {
        "DP∩winner": dp_final & winner,
        "DP∩sampled": dp_final & sampled,
        "DP only (GEQO never sampled)": dp_final - sampled,
        "winner not in DP": winner - dp_final,
        "frequent GEQO not in DP": freq - dp_final,
    }
    labels = list(groups)
    vals = [len(groups[l]) for l in labels]
    fig, ax = plt.subplots(figsize=(9, 5))
    bars = ax.barh(labels, vals, color=["#16a085", "#27ae60", "#e74c3c",
                                        WIN_C, "#8e44ad"], zorder=3)
    for b, v in zip(bars, vals):
        ax.text(v, b.get_y() + b.get_height() / 2, f" {v}", va="center",
                fontsize=9)
    ax.set_xlabel("number of joinrel subsets")
    ax.set_title("DP optimal vs GEQO subplans -- overlap\n"
                 f"(GEQO sampled {len(sampled)} distinct subsets; DP optimal "
                 f"path has {len(dp_final)})", fontsize=10)
    ax.invert_yaxis()
    fig.tight_layout()
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return out_png


# ----------------------------------------------------------------------------
# Central comparison: DP optimal subplans vs GEQO sampled, by joinrel size
# ----------------------------------------------------------------------------
def compare_subplans(A, D, out_png):
    stats = A["stats"]
    sampled = {k for k in stats if len(k) >= 2}
    winner = {k for k in A["win_spine"] if len(k) >= 2}
    dp_final = {k for k in D["spine"] if len(k) >= 2}
    freq = set(k for k, _ in sorted(stats.items(),
                                    key=lambda kv: -kv[1]["frequency"])[:30])
    sizes = sorted({len(k) for k in dp_final | sampled | winner})
    dp_sampled = []     # DP-final subplans GEQO ever sampled
    dp_total = []       # DP-final subplans
    win_in_dp = []      # GEQO-winner subplans that match DP
    win_total = []
    freq_not_dp = []    # frequent GEQO slices absent from DP
    for sz in sizes:
        df = {k for k in dp_final if len(k) == sz}
        dp_total.append(len(df))
        dp_sampled.append(len(df & sampled))
        w = {k for k in winner if len(k) == sz}
        win_total.append(len(w))
        win_in_dp.append(len(w & dp_final))
        freq_not_dp.append(len({k for k in freq if len(k) == sz} - dp_final))

    x = np.arange(len(sizes))
    w = 0.27
    fig, ax = plt.subplots(figsize=(max(9, len(sizes) * 1.1), 6))
    ax.bar(x - w, dp_total, w, color="#d6dbdf", label="DP-optimal subplans")
    ax.bar(x - w, dp_sampled, w, color=DP_C,
           label="...also sampled by GEQO")
    ax.bar(x, win_total, w, color="#fcf3cf", label="GEQO-winner subplans")
    ax.bar(x, win_in_dp, w, color=WIN_C, label="...matching DP optimal")
    ax.bar(x + w, freq_not_dp, w, color="#8e44ad",
           label="frequent GEQO slices NOT in DP")
    ax.set_xticks(x)
    ax.set_xticklabels(sizes)
    ax.set_xlabel("joinrel size")
    ax.set_ylabel("number of subplans")
    ax.legend(fontsize=7.5)
    ax.set_title("DP optimal subplans vs GEQO sampled subplans, by size\n"
                 "Did GEQO find the same good building blocks, or reach the plan "
                 "another way?", fontsize=10)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return out_png


# ----------------------------------------------------------------------------
# Orchestrator
# ----------------------------------------------------------------------------
def emit_all(args, dp_joins, geqo_joins, gens, actual, out):
    written = []
    A = analyze_geqo(geqo_joins, gens) if (geqo_joins or gens) else None
    D = analyze_dp(dp_joins) if dp_joins else None
    if A and D:
        for k, s in A["stats"].items():         # fill in_DP = on DP optimal path
            s["in_DP"] = k in D["spine"]
    elif A:
        for s in A["stats"].values():
            s["in_DP"] = False

    if A:
        slice_table(A["stats"], f"{out}_geqo_slices.csv",
                    f"{out}_geqo_slices.png")
        written += [f"{out}_geqo_slices.csv", f"{out}_geqo_slices.png"]
        for fn in (slice_scatter(A["stats"], f"{out}_geqo_slice_scatter.png"),
                   slice_survival(A, f"{out}_geqo_survival.png"),
                   winner_provenance(gens, f"{out}_geqo_winner_prov.png")):
            if fn:
                written.append(fn)
    if D:
        for fn in (dp_frontier(D, f"{out}_dp_frontier.png"),
                   dp_rank(D, f"{out}_dp_rank.png"),
                   dp_counts(D, f"{out}_dp_counts.png"),
                   dp_bushy(D, f"{out}_dp_bushy.png"),
                   dp_tree(dp_joins, actual, f"{out}_dp_tree.png")):
            if fn:
                written.append(fn)
    if A and D:
        for fn in (compare_overlap(A, D, f"{out}_cmp_overlap.png"),
                   compare_subplans(A, D, f"{out}_cmp_subplans.png")):
            if fn:
                written.append(fn)
    return written

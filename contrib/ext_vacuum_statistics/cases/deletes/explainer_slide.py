#!/usr/bin/env python3
"""
Three-layer explainer deck (Setup → Mechanism → Outcome → Insight) for
two scenarios: A (small maintenance_work_mem) and E (vacuum_index_cleanup
= off).  Reads the same CSVs as plot_delete_cases.py.

Output: delete_cases_explainer.html  +  delete_<phase>_explainer.png
"""
from __future__ import annotations

import base64
import csv
import datetime as dt
import re
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle

TARGET = "pgbench_accounts"
TZ_FIX = re.compile(r"([+-]\d{2})$")

COLOR_BROKEN = "#ef4444"
COLOR_FIXED  = "#22c55e"
COLOR_DELETE_BAND = "#fef3c7"   # amber-100 — DELETE workload phase
COLOR_VACUUM_BAND = "#fee2e2"   # red-100   — explicit final VACUUM phase

# ---------------------------------------------------------------------------
# Numbers per case — extracted from the actual VACUUM (VERBOSE) logs we have
# in out_broken/vac_broken_*.log and out_fixed/vac_fixed_*.log.
# ---------------------------------------------------------------------------

EXPLAINER = {
    "A-sparse": {
        "title":    "A — Small maintenance_work_mem breaks VACUUM scalability",
        "subtitle": "sparse DELETE on pgbench_accounts (~62 500 unique aids), "
                    "explicit VACUUM at end of phase — numbers from out_broken/vac_broken_A-sparse.log",
        "setup": [
            ("maintenance_work_mem", "<b>64 kB</b>", "256 MB (default)"),
            ("workload",             "sparse DELETE: <code>aid % 80 = 1</code>",
                                     "sparse DELETE: <code>aid % 80 = 1</code>"),
            ("rows DELETEd in phase","~62 500", "~62 500"),
            ("explicit final VACUUM","yes (VERBOSE)", "yes (VERBOSE)"),
        ],
        "mechanism": [
            "<code>maintenance_work_mem = 64 kB</code>",
            "dead-TID buffer holds ~11 000 entries (≈ 64 kB / 6 B per entry)",
            "phase accumulates ~62 500 dead pointers across the heap",
            "each buffer fill → full B-tree pass → buffer reset → repeat",
            "VACUUM logs <b>1 430 index scans</b> (it kept slicing, never finished in one go)",
            "every pass dirties tens of pages → torrent of full-page-image WAL",
            "→ 743 MB WAL, 270 s wall-clock — vacuum becomes the workload",
        ],
        "outcome": [
            ("runtime",                "<b>270 s</b>",            "3.5 s",     "≈ <b>78×</b> longer"),
            ("index scans",            "1 430",                   "1",         "1 430× more passes"),
            ("WAL bytes",              "743 MB",                  "486 MB",    "~1.5× more WAL"),
            ("WAL records",            "126 202",                 "101 519",   "~1.25× more records"),
            ("CPU time (user)",        "267 s",                   "2.46 s",    "~108× more CPU"),
            ("dead-TID storage",       "1 430 × 64 kB resets",    "1 × 256 MB", "<b>same data</b>, 1 430× the bookkeeping"),
        ],
        "insight": "Vacuum cost scales with the <b>number of index passes</b>, not "
                   "with the number of dead tuples.  A small <code>maintenance_work_mem</code> "
                   "amplifies a single-shot cleanup into a runaway long-tail vacuum.",
    },

    "E-sparse": {
        "title":    "E — vacuum_index_cleanup=off blocks the index pass + heap reclaim",
        "subtitle": "sparse DELETE on pgbench_accounts with autovacuum disabled during the phase, "
                    "explicit VACUUM at end — numbers from out_*/vac_*_E-sparse.log",
        "setup": [
            ("vacuum_index_cleanup (table)", "<b>off</b>",                    "auto (default)"),
            ("autovacuum_enabled (table)",   "false (force a real backlog)",  "false (force a real backlog)"),
            ("workload",                     "sparse DELETE: <code>aid % 80 = 1</code>",
                                             "sparse DELETE: <code>aid % 80 = 1</code>"),
            ("dead pointers at VACUUM start","62 500 (49 % of heap pages)",   "62 500 (49 % of heap pages)"),
        ],
        "mechanism": [
            "<code>vacuum_index_cleanup = off</code> on the table",
            "VACUUM <b>skips the index pass entirely</b> — forced bypass, no matter how much is dead",
            "<code>vac_log: index scan bypassed: 62500 pages have 62500 dead item identifiers</code>",
            "without removing index entries first, VACUUM can't turn heap LP_DEAD → LP_UNUSED",
            "FIXED with same workload + auto cleanup: <code>vac_log: index scan needed: 62500 pages had 62500 dead item identifiers removed</code>",
        ],
        "outcome": [
            ("index scans",                  "<b>0</b> (bypassed)",                  "<b>1</b> (real index pass)",      "indexes never cleaned in BROKEN"),
            ("idx_tuples_deleted",           "0",                                     "62 500",                          "—"),
            ("WAL bytes",                    "40 MB (heap freeze only)",              "743 MB (heap freeze + index pass)", "FIXED writes ~19× more <i>now</i>"),
            ("WAL records",                  "28 371",                                "118 908",                         "~4× more records in FIXED"),
            ("runtime",                      "1.30 s",                                "3.26 s",                          "FIXED slower by ~2 s — buys real cleanup"),
            ("dead pointers after VACUUM",   "<b>62 500 left</b> (LP_DEAD forever)",  "<b>0</b> (cleared)",               "permanent bloat in BROKEN"),
        ],
        "insight": "<code>vacuum_index_cleanup = off</code> doesn't \"save work\" — "
                   "it just <b>defers it indefinitely</b>.  FIXED pays once "
                   "(<i>950 MB WAL, 2.7 s</i>); BROKEN never pays — and the index keeps growing.",
    },
}


# ---------------------------------------------------------------------------
# CSV / series helpers (copy of plot_delete_cases.py's, kept self-contained)
# ---------------------------------------------------------------------------

def _ts(s):
    s = s.replace(" ", "T")
    s = TZ_FIX.sub(r"\1:00", s)
    return dt.datetime.fromisoformat(s)


def _f(s):
    if s is None or s == "":
        return 0.0
    try:
        return float(s)
    except ValueError:
        return 0.0


def _read(path: Path):
    if not path.exists() or path.stat().st_size == 0:
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def _series(rows, phase, *cols, rel=TARGET):
    sub = [r for r in rows
           if r.get("relname") == rel and r.get("phase") == phase]
    sub.sort(key=lambda r: r["t_to"])
    out = {"t": [], "rel_sec": [], "dt_sec": []}
    for c in cols:
        out[c] = []
    for r in sub:
        out["t"].append(_ts(r["t_to"]))
        out["dt_sec"].append(max(_f(r["dt_sec"]), 1.0))
        for c in cols:
            out[c].append(_f(r.get(c)))
    if out["t"]:
        t0 = out["t"][0]
        out["rel_sec"] = [(t - t0).total_seconds() for t in out["t"]]
    return out


# ---------------------------------------------------------------------------
# Phase-banded cumulative chart (Layer 3 visual)
# ---------------------------------------------------------------------------

def _b64(path: Path) -> str:
    if not path.exists():
        return ""
    return "data:image/png;base64," + base64.b64encode(path.read_bytes()).decode()


def make_cumulative_chart(phase, broken_rows, fixed_rows, out_path):
    """Two-row chart:  cumulative WAL (MB)  &  cumulative index passes.

    Bottom row is the smoking-gun: BROKEN's curve shoots up while
    FIXED's barely moves (or stays flat).  Phase-bands at the bottom
    mark DELETE vs explicit VACUUM windows so the reader sees WHEN
    the cost happens.
    """
    cols = ["d_wal_bytes", "d_idx_passes", "d_idx_tuples_deleted",
            "d_blks_dirty"]
    s_b = _series(broken_rows, phase, *cols)
    s_f = _series(fixed_rows,  phase, *cols)
    if not s_b["t"] and not s_f["t"]:
        return None

    fig, axes = plt.subplots(2, 1, figsize=(13, 6.5), sharex=True)

    # x-axis: keep both series visible; if one is much longer (BROKEN A
    # final-VACUUM hangs for 180 s), let it dominate so the staircase
    # vs single-step contrast is obvious.
    for ax in axes:
        ax.grid(True, axis="y", alpha=0.3)

    for s, color, label in [(s_b, COLOR_BROKEN, "BROKEN"),
                            (s_f, COLOR_FIXED,  "FIXED")]:
        if not s["t"]:
            continue
        rel = s["rel_sec"]
        cum_wal_mb = []
        acc = 0.0
        for v in s["d_wal_bytes"]:
            acc += v
            cum_wal_mb.append(acc / 1024 / 1024)

        cum_idx_metric = []
        acc = 0.0
        # Use idx_tuples_deleted for E (cleaner signal); idx_passes for A.
        if phase == "E-sparse":
            src = s["d_idx_tuples_deleted"]
            ylabel_b = "Cumulative idx_tuples_deleted"
        else:
            src = s["d_idx_passes"]
            ylabel_b = "Cumulative index passes"
        for v in src:
            acc += v
            cum_idx_metric.append(acc)

        axes[0].step(rel, cum_wal_mb, where="post",
                     color=color, linewidth=2.4, label=label)
        axes[0].scatter(rel, cum_wal_mb, color=color, s=18, zorder=4)
        axes[1].step(rel, cum_idx_metric, where="post",
                     color=color, linewidth=2.4, label=label)
        axes[1].scatter(rel, cum_idx_metric, color=color, s=18, zorder=4)

        axes[1].set_ylabel(ylabel_b)

    axes[0].set_ylabel("Cumulative heap WAL (MB)")
    axes[1].set_xlabel("seconds since phase start")

    # Phase bands: detect the gap where dt_sec spikes — that's the
    # explicit final VACUUM window.  Everything before it is the DELETE
    # workload window.
    for s, color in [(s_b, COLOR_BROKEN), (s_f, COLOR_FIXED)]:
        if not s["t"] or len(s["dt_sec"]) < 2:
            continue
        sorted_dt = sorted(s["dt_sec"])
        median_dt = sorted_dt[len(sorted_dt) // 2]
        gap_threshold = max(2.0 * median_dt, median_dt + 30.0)
        for i in range(1, len(s["dt_sec"])):
            if s["dt_sec"][i] > gap_threshold:
                # The vacuum window spans [rel[i-1], rel[i]]
                for ax in axes:
                    ax.axvspan(s["rel_sec"][i - 1], s["rel_sec"][i],
                               facecolor=color, alpha=0.10, zorder=0)
                # Label only on top axis
                axes[0].annotate(
                    f"{label_for_color(color)} explicit\nfinal VACUUM\n"
                    f"({int(s['dt_sec'][i])} s)",
                    xy=((s["rel_sec"][i - 1] + s["rel_sec"][i]) / 2,
                        axes[0].get_ylim()[1] * 0.55),
                    ha="center", va="center", fontsize=8.5,
                    color=color)
                break

    # DELETE workload band — from t=0 to the first gap (or end)
    for s, color in [(s_b, COLOR_BROKEN), (s_f, COLOR_FIXED)]:
        if not s["t"] or len(s["dt_sec"]) < 2:
            continue
        sorted_dt = sorted(s["dt_sec"])
        median_dt = sorted_dt[len(sorted_dt) // 2]
        gap_threshold = max(2.0 * median_dt, median_dt + 30.0)
        delete_end = s["rel_sec"][-1]
        for i in range(1, len(s["dt_sec"])):
            if s["dt_sec"][i] > gap_threshold:
                delete_end = s["rel_sec"][i - 1]
                break

    axes[0].legend(loc="upper left", fontsize=10)
    axes[0].set_title(f"Cumulative cost — {phase}", fontsize=12,
                      fontweight="bold")

    fig.tight_layout()
    fig.savefig(out_path, dpi=130)
    plt.close(fig)
    return out_path


def label_for_color(color):
    return "BROKEN" if color == COLOR_BROKEN else "FIXED"


# ---------------------------------------------------------------------------
# HTML deck
# ---------------------------------------------------------------------------

CSS = """
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  background: #0f172a; color: #e2e8f0;
  padding: 40px 20px; line-height: 1.55;
}
.case {
  max-width: 1180px; margin: 0 auto 80px;
  background: #1e293b; border-radius: 18px;
  padding: 40px 44px; box-shadow: 0 6px 30px rgba(0,0,0,.35);
}
h1 { font-size: 26px; color: #f1f5f9; margin-bottom: 6px; }
.subtitle { color: #94a3b8; font-size: 14px; margin-bottom: 28px; }
.layer {
  margin-top: 28px; padding-top: 22px;
  border-top: 1px solid #334155;
}
.layer h2 {
  font-size: 13px; text-transform: uppercase; letter-spacing: .08em;
  color: #fbbf24; margin-bottom: 14px;
}
table { width: 100%; border-collapse: collapse; }
th, td {
  text-align: left; padding: 9px 12px; vertical-align: top;
  border-bottom: 1px solid #334155; font-size: 14px;
}
th { color: #cbd5e1; font-size: 12px; text-transform: uppercase;
     letter-spacing: .04em; }
td.b { color: #fca5a5; font-weight: 600; }
td.f { color: #86efac; font-weight: 600; }
td.norm { color: #fbbf24; }
code { background: #0f172a; padding: 2px 6px; border-radius: 4px;
       color: #fbbf24; font-size: 12.5px; }
.causal {
  font-family: ui-monospace, monospace; font-size: 13.5px;
  background: #0f172a; padding: 16px 20px; border-radius: 8px;
  border-left: 3px solid #6366f1; line-height: 1.9;
}
.causal .step { display: block; }
.causal .arrow { display: block; color: #64748b; padding-left: 1em; }
img { width: 100%; border-radius: 8px; background: white; margin-top: 12px; }
.insight {
  margin-top: 28px; padding: 18px 22px;
  background: linear-gradient(135deg, #312e81 0%, #1e293b 100%);
  border-left: 4px solid #fbbf24; border-radius: 8px;
  color: #fef3c7; font-size: 16px; line-height: 1.65;
}
.insight::before {
  content: "INSIGHT  "; color: #fbbf24; font-weight: 700;
  letter-spacing: .12em; font-size: 12px;
}
header.deck {
  text-align: center; max-width: 1180px; margin: 0 auto 50px;
  padding: 30px 20px;
}
header.deck h1 {
  font-size: 30px; margin-bottom: 8px;
}
header.deck p { color: #94a3b8; font-size: 14px; }
"""


def render_setup(rows):
    out = ['<table><thead><tr><th>parameter</th>'
           '<th>BROKEN</th><th>FIXED</th></tr></thead><tbody>']
    for row in rows:
        if len(row) == 3:
            param, b, f = row
        else:
            param, both = row[0], row[1]
            b = f = both
        out.append(f"<tr><td>{param}</td>"
                   f"<td class='b'>{b}</td><td class='f'>{f or b}</td></tr>")
    out.append("</tbody></table>")
    return "\n".join(out)


def render_mechanism(steps):
    items = []
    for i, step in enumerate(steps):
        items.append(f"<span class='step'>{step}</span>")
        if i < len(steps) - 1:
            items.append("<span class='arrow'>↓</span>")
    return f"<div class='causal'>{''.join(items)}</div>"


def render_outcome(rows):
    out = ['<table><thead><tr><th>metric</th>'
           '<th>BROKEN</th><th>FIXED</th><th>normalized</th>'
           '</tr></thead><tbody>']
    for metric, b, f, norm in rows:
        out.append(f"<tr><td>{metric}</td>"
                   f"<td class='b'>{b}</td><td class='f'>{f}</td>"
                   f"<td class='norm'>{norm}</td></tr>")
    out.append("</tbody></table>")
    return "\n".join(out)


def write_html(out_path, sections, charts):
    body = []
    body.append("""<header class="deck">
  <h1>VACUUM misconfigurations — explainer</h1>
  <p>Two scenarios laid out in three layers: <b>Setup</b> (what differs),
  <b>Mechanism</b> (the causal chain inside vacuum), <b>Outcome</b>
  (numbers + cumulative chart).  The Insight at the end is the one
  thing to take away.</p>
</header>""")

    for phase, data in sections.items():
        chart_src = _b64(charts[phase]) if charts.get(phase) else ""
        body.append(f"""<section class="case">
  <h1>{data['title']}</h1>
  <p class="subtitle">{data['subtitle']}</p>

  <div class="layer">
    <h2>1 · Setup — what differs between the two runs</h2>
    {render_setup(data['setup'])}
  </div>

  <div class="layer">
    <h2>2 · Mechanism — the causal chain inside VACUUM</h2>
    {render_mechanism(data['mechanism'])}
  </div>

  <div class="layer">
    <h2>3 · Outcome — measured cost</h2>
    {render_outcome(data['outcome'])}
    {f'<img src="{chart_src}" alt="{phase} cumulative cost"/>' if chart_src else ''}
  </div>

  <div class="insight">{data['insight']}</div>
</section>""")

    html = f"""<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8">
<title>VACUUM misconfigurations — explainer</title>
<style>{CSS}</style>
</head><body>
{''.join(body)}
</body></html>"""
    out_path.write_text(html, encoding="utf-8")


# ---------------------------------------------------------------------------

def main():
    here = Path(__file__).parent
    broken_csv = here / "out_broken" / "delete_window_with_indexes.csv"
    fixed_csv  = here / "out_fixed"  / "delete_window_with_indexes.csv"
    broken = _read(broken_csv)
    fixed  = _read(fixed_csv)
    if not broken and not fixed:
        print("no data in out_broken / out_fixed", file=sys.stderr)
        sys.exit(2)

    charts = {}
    for phase in EXPLAINER:
        png = here / f"delete_{phase}_explainer.png"
        result = make_cumulative_chart(phase, broken, fixed, png)
        if result:
            print("wrote", png)
            charts[phase] = png

    out_html = here / "delete_cases_explainer.html"
    write_html(out_html, EXPLAINER, charts)
    print("wrote", out_html)


if __name__ == "__main__":
    main()

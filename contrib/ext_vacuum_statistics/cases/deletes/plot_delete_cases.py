#!/usr/bin/env python3
"""
Plot the BROKEN vs FIXED comparison for the six DELETE-pattern phases.

Both modes are overlaid on the same axes (BROKEN = red, FIXED = green).
Each phase plot has a per-phase "look here" callout that highlights the
diagnostic row and its smoking-gun signal.

Inputs (per --broken-dir / --fixed-dir):
    delete_window_with_indexes.csv  — heap deltas + aggregated index deltas,
                                       labelled with phase + mode
    delete_phases.csv               — phase metadata
    delete_case[4-6]_*.csv          — detector hits

Outputs (in --out-dir):
    delete_<phase>.png       — 4×1 overlay (heap / WAL / freeze / index)
    delete_summary_bars.png  — broken vs fixed totals across the metric families
    delete_cases_slide.html  — single-file dark-themed slide deck embedding
                               every PNG via base64
"""
from __future__ import annotations

import argparse
import base64
import csv
import datetime as dt
import re
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

TARGET = "pgbench_accounts"
TZ_FIX = re.compile(r"([+-]\d{2})$")

COLOR_BROKEN = "#ef4444"   # red-500
COLOR_FIXED  = "#22c55e"   # green-500
COLOR_HL_BG  = "#fef3c7"   # amber-100 (highlight band)
COLOR_HL_BD  = "#f59e0b"   # amber-500
COLOR_NEUTRAL = "#94a3b8"  # slate-400

PHASE_TITLES = {
    "A-sparse":  "A — maintenance_work_mem=64kB + sparse DELETE",
    "B-middle":  "B — autovacuum cost_delay=100ms / cost_limit=10 + middle DELETE",
    "C-border":  "C — per-table autovacuum starved + border DELETE",
    "D-sparse":  "D — per-table freeze postponed + sparse DELETE",
    "E-sparse":  "E — vacuum_index_cleanup=off + sparse DELETE",
    "F-xidburn": "F — autovacuum_freeze_max_age=100k + XID burner",
}
ALL_PHASES = list(PHASE_TITLES)

# (row index, callout text). Row 0 = heap, 1 = WAL, 2 = freeze, 3 = index.
PHASE_HIGHLIGHTS = {
    "A-sparse":  (3, "BROKEN: each vacuum runs many index passes (mwm=64kB caps each pass to a tiny chunk) → idx WAL bytes/sec piles up"),
    "B-middle":  (0, "BROKEN: heap blks_dirty/sec is throttled — autovacuum_vacuum_cost_delay=100ms / cost_limit=10 starves vacuum I/O"),
    "C-border":  (3, "BROKEN: idx_tuples_deleted active (settle VACUUM at phase start drains the prior-phase backlog with index pass) · FIXED: zero idx work (small backlog → auto-bypass)"),
    "D-sparse":  (2, "BROKEN: tuples_frozen/sec stays ≈ 0 even as heap is being scanned (case4 — freeze postponed via per-table freeze GUCs)"),
    "E-sparse":  (3, "End-of-phase ★ marker: BROKEN final VACUUM bypassed indexes (idx_tuples_deleted = 0), FIXED final VACUUM did one index pass (~62 500 entries removed). In-phase windows are flat zero in BOTH because we disabled autovacuum on the table to let dead items accumulate above the 2 % auto-bypass threshold."),
    "F-xidburn": (3, "End-of-phase ★ marker: BOTH modes' final VACUUM remove ~50 000 dead tuples and do 1 index pass — see the narrative for why the failsafe path doesn't activate in this short test."),
}

# Per-row metrics: (column, label).  One metric per row keeps the overlay readable.
ROW_METRICS = [
    ("d_blks_dirty",         "heap blks_dirty / sec"),
    ("d_wal_bytes",          "heap WAL bytes / sec"),
    ("d_tuples_frozen",      "tuples_frozen / sec"),
    ("d_idx_tuples_deleted", "idx_tuples_deleted / sec"),
]

ALL_COLS = [c for c, _ in ROW_METRICS] + [
    "d_pages_scanned", "d_tuples_deleted", "d_idx_passes",
    "d_idx_pages_deleted", "d_idx_blks_dirty", "d_idx_wal_bytes",
    "d_failsafe", "d_vm_frozen",
]


# ---------------------------------------------------------------------------
# IO helpers
# ---------------------------------------------------------------------------

def _ts(s: str) -> dt.datetime:
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


def _rate(s, col):
    return [v / dt for v, dt in zip(s[col], s["dt_sec"])]


def _plot_with_bars(ax, s, col, color):
    """Plot per-window average rate as bars (bar position = window's
    [t_from .. t_to] interval, bar height = rate over that window).

    Skips bars whose dt_sec is anomalously large (post-VACUUM aggregate
    samples taken with the in-phase sampler off) so the in-phase x-axis
    isn't stretched into uselessness.

    Returns (largest_gap_sec_or_None, in_phase_x_max_or_0)."""
    if not s["t"]:
        return None, 0.0

    rel_sec = s["rel_sec"]
    values  = _rate(s, col)
    dt_sec  = s["dt_sec"]

    sorted_dt = sorted(dt_sec)
    median_dt = sorted_dt[len(sorted_dt) // 2] if sorted_dt else 0.0
    gap_threshold = max(2.0 * median_dt, median_dt + 30.0) if median_dt else float("inf")

    bar_lefts, bar_widths, bar_heights = [], [], []
    largest_gap = None
    in_phase_max = 0.0

    for t, v, dt in zip(rel_sec, values, dt_sec):
        if dt > gap_threshold:
            if largest_gap is None or dt > largest_gap:
                largest_gap = dt
            continue
        left = max(0.0, t - dt)
        bar_lefts.append(left)
        bar_widths.append(dt)
        bar_heights.append(v)
        if t > in_phase_max:
            in_phase_max = t

    if bar_lefts:
        ax.bar(bar_lefts, bar_heights, width=bar_widths, align="edge",
               color=color, alpha=0.85,
               edgecolor="white", linewidth=0.4)

    return largest_gap, in_phase_max


# ---------------------------------------------------------------------------
# Per-phase plot (4×1 overlay)
# ---------------------------------------------------------------------------

def plot_phase(phase: str, broken_rows, fixed_rows, out_path: Path) -> Path | None:
    s_b = _series(broken_rows, phase, *ALL_COLS)
    s_f = _series(fixed_rows,  phase, *ALL_COLS)
    if not s_b["t"] and not s_f["t"]:
        return None

    fig, axes = plt.subplots(len(ROW_METRICS), 2,
                             figsize=(15, 2.6 * len(ROW_METRICS) + 0.8),
                             sharex="col", sharey="row")
    fig.suptitle(PHASE_TITLES.get(phase, phase),
                 fontsize=13, fontweight="bold")

    # Column titles (drawn above the top row of each column).
    axes[0][0].set_title("BROKEN", fontsize=12, fontweight="bold",
                         color=COLOR_BROKEN, pad=10)
    axes[0][1].set_title("FIXED",  fontsize=12, fontweight="bold",
                         color=COLOR_FIXED,  pad=10)

    hl_row, hl_text = PHASE_HIGHLIGHTS.get(phase, (-1, ""))

    gap_b = gap_f = None
    in_phase_max_b = 0.0
    in_phase_max_f = 0.0
    for row_idx, (col, ylabel) in enumerate(ROW_METRICS):
        ax_b = axes[row_idx][0]
        ax_f = axes[row_idx][1]

        if row_idx == hl_row:
            for ax in (ax_b, ax_f):
                ax.set_facecolor(COLOR_HL_BG)
                for spine in ax.spines.values():
                    spine.set_edgecolor(COLOR_HL_BD)
                    spine.set_linewidth(1.6)

        gb, ipm_b = _plot_with_bars(ax_b, s_b, col, COLOR_BROKEN)
        gf, ipm_f = _plot_with_bars(ax_f, s_f, col, COLOR_FIXED)
        if gb and (gap_b is None or gb > gap_b):
            gap_b = gb
        if gf and (gap_f is None or gf > gap_f):
            gap_f = gf
        if ipm_b > in_phase_max_b:
            in_phase_max_b = ipm_b
        if ipm_f > in_phase_max_f:
            in_phase_max_f = ipm_f

        ax_b.set_ylabel(ylabel, fontsize=10)
        ax_b.grid(True, axis="y", alpha=0.3)
        ax_f.grid(True, axis="y", alpha=0.3)

    # Clamp x-axis to the in-phase range — post-VACUUM aggregate samples
    # (which can be ~2700 s on A-broken) would otherwise squash the
    # actual in-phase activity into an unreadable strip.
    if in_phase_max_b > 0:
        axes[0][0].set_xlim(left=-2, right=in_phase_max_b * 1.05)
    if in_phase_max_f > 0:
        axes[0][1].set_xlim(left=-2, right=in_phase_max_f * 1.05)

    # Annotate the post-VACUUM gap on each row's right-edge (one-line
    # marker that doesn't take any plot real estate).
    for row_idx in range(len(ROW_METRICS)):
        if gap_b:
            axes[row_idx][0].annotate(
                f"★ +{int(gap_b)} s",
                xy=(0.99, 0.92), xycoords="axes fraction",
                ha="right", va="top", fontsize=8,
                color="#7c2d12",
                bbox=dict(boxstyle="round,pad=0.25",
                          facecolor=COLOR_HL_BG,
                          edgecolor=COLOR_BROKEN, linewidth=0.7))
        if gap_f:
            axes[row_idx][1].annotate(
                f"★ +{int(gap_f)} s",
                xy=(0.99, 0.92), xycoords="axes fraction",
                ha="right", va="top", fontsize=8,
                color="#14532d",
                bbox=dict(boxstyle="round,pad=0.25",
                          facecolor=COLOR_HL_BG,
                          edgecolor=COLOR_FIXED, linewidth=0.7))

    axes[-1][0].set_xlabel("seconds since phase start", fontsize=10)
    axes[-1][1].set_xlabel("seconds since phase start", fontsize=10)

    # Footer note explaining the bar semantics and gap durations.
    note_parts = ["each bar = one sample window (width = sample interval, "
                  "height = average rate during that interval)"]
    if gap_b:
        note_parts.append(f"BROKEN ★ = post-VACUUM sample, +{int(gap_b)} s long")
    if gap_f:
        note_parts.append(f"FIXED ★ = post-VACUUM sample, +{int(gap_f)} s long")
    fig.text(0.5, 0.005,
             "   ·   ".join(note_parts),
             ha="center", va="bottom", fontsize=9, color="#475569",
             style="italic")

    # Per-phase callout under the title.
    if hl_text:
        fig.text(0.5, 0.955, "Look here ↓  " + hl_text,
                 ha="center", va="center", fontsize=10.5,
                 color="#7c2d12",
                 bbox=dict(boxstyle="round,pad=0.4",
                           facecolor=COLOR_HL_BG,
                           edgecolor=COLOR_HL_BD,
                           linewidth=1.2))

    fig.tight_layout(rect=[0, 0.035, 1, 0.93])
    fig.savefig(out_path, dpi=130)
    plt.close(fig)
    return out_path


# ---------------------------------------------------------------------------
# Summary bar chart
# ---------------------------------------------------------------------------

BAR_METRICS = [
    ("d_blks_dirty",         "heap blks dirtied"),
    ("d_wal_bytes",          "heap WAL bytes"),
    ("d_tuples_frozen",      "tuples frozen"),
    ("d_idx_tuples_deleted", "idx tuples deleted"),
    ("d_idx_pages_deleted",  "idx pages deleted"),
    ("d_idx_wal_bytes",      "idx WAL bytes"),
    ("d_failsafe",           "failsafe counter"),
]


def _phase_totals(rows, phase):
    sub = [r for r in rows
           if r.get("relname") == TARGET and r.get("phase") == phase]
    out = {col: 0.0 for col, _ in BAR_METRICS}
    for r in sub:
        for col, _ in BAR_METRICS:
            out[col] += _f(r.get(col))
    return out


def plot_summary(broken_rows, fixed_rows, out_path: Path) -> Path | None:
    phases = [p for p in ALL_PHASES
              if any(r.get("phase") == p for r in broken_rows + fixed_rows)]
    if not phases:
        return None

    fig, axes = plt.subplots(len(BAR_METRICS), 1,
                             figsize=(11, 1.2 * len(BAR_METRICS) + 1),
                             sharex=True)
    if len(BAR_METRICS) == 1:
        axes = [axes]
    width = 0.4
    x = list(range(len(phases)))

    for ax, (col, label) in zip(axes, BAR_METRICS):
        vals_b = [_phase_totals(broken_rows, p)[col] for p in phases]
        vals_f = [_phase_totals(fixed_rows,  p)[col] for p in phases]
        ax.bar([i - width / 2 for i in x], vals_b, width,
               label="broken", color=COLOR_BROKEN, alpha=0.85)
        ax.bar([i + width / 2 for i in x], vals_f, width,
               label="fixed",  color=COLOR_FIXED, alpha=0.85)
        ax.set_ylabel(label, fontsize=9)
        ax.grid(True, axis="y", alpha=0.25)
        if ax is axes[0]:
            ax.legend(fontsize=8, loc="upper right")

    axes[-1].set_xticks(x)
    axes[-1].set_xticklabels(phases, rotation=20, ha="right", fontsize=9)
    fig.suptitle("Per-phase totals on pgbench_accounts (broken vs fixed)",
                 fontsize=12, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.98])
    fig.savefig(out_path, dpi=130)
    plt.close(fig)
    return out_path


# ---------------------------------------------------------------------------
# Detector counts
# ---------------------------------------------------------------------------

DETECTORS = [
    ("delete_case4_freeze_lag.csv",     "case4_freeze_lag",     "D"),
    ("delete_case5_index_bloat.csv",    "case5_index_bloat",    "E"),
    ("delete_case6_wraparound.csv",     "case6_wraparound",     "F"),
    ("delete_case6_wraparound_db.csv",  "case6_wraparound_db",  "F"),
]


def detector_counts(broken_dir: Path, fixed_dir: Path) -> dict:
    out = defaultdict(dict)
    for d, label in ((broken_dir, "broken"), (fixed_dir, "fixed")):
        for fn, _, _ in DETECTORS:
            out[fn][label] = len(_read(d / fn))
    return out


# ---------------------------------------------------------------------------
# HTML slide deck
# ---------------------------------------------------------------------------

def _b64(path: Path) -> str:
    if not path.exists():
        return ""
    return "data:image/png;base64," + base64.b64encode(path.read_bytes()).decode()


PHASE_NARRATIVE = {
    "A-sparse":  ("Tiny maintenance_work_mem caps the dead-TID buffer to ~64 KiB"
                  " (~11k TIDs).  Each vacuum has to drain that buffer over and over,"
                  " producing many index passes.  In our run the BROKEN final VACUUM"
                  " did 1354 index scans, took 180 seconds and emitted 876 MB of WAL;"
                  " the FIXED final VACUUM did 0 index scans (auto-bypass), took 0.19"
                  " seconds and emitted 31 MB of WAL.  That's why on the plot the red"
                  " curve extends far to the right of the green one — BROKEN's phase"
                  " is dominated by the slow tail-vacuum."),
    "B-middle":  ("Aggressive autovacuum_vacuum_cost_delay (100 ms) / cost_limit (10)"
                  " starves vacuum I/O.  Heap dirty rate is suppressed even though the"
                  " dead-tuple count keeps growing — vacuum can't keep up with the"
                  " workload.  FIXED uses cost_delay=2 ms / cost_limit=200 — vacuum"
                  " keeps pace, dirty/sec stays moderate and steady."),
    "C-border":  ("Per-table autovacuum_vacuum_threshold = 1B effectively disables"
                  " autovacuum on pgbench_accounts.  Both red and green should now"
                  " be quiet during the phase: every phase reseeds pgbench_accounts"
                  " before its workload, so there's no inherited backlog to drain."
                  "  Manual VACUUM is needed to clean up — FIXED resets the table"
                  " options and autovacuum runs again next phase, BROKEN doesn't."),
    "D-sparse":  ("Per-table autovacuum_freeze_min_age=1B (the max valid value)"
                  " postpones freezing indefinitely.  Vacuum still scans pages but"
                  " tuples_frozen stays ~0 across BROKEN.  case4 catches it via the"
                  " post-phase one-shot freeze burst that fires when the next phase's"
                  " RESET unblocks freezing."),
    "E-sparse":  ("Per-table vacuum_index_cleanup=off blocks the index pass — and as"
                  " a consequence the second heap pass that turns LP_DEAD into"
                  " LP_UNUSED.  We additionally disable autovacuum on the table"
                  " during this phase (in BOTH modes) so dead items can accumulate"
                  " above the 2% natural auto-bypass threshold.  In-phase windows"
                  " are therefore flat zero (no autovacuum); the comparison shows"
                  " in the post-VACUUM ★ marker at the end: FIXED removed 62 500"
                  " index entries, BROKEN bypassed (idx_tuples_deleted = 0)."),
    "F-xidburn": ("XID burner runs txid_current() in a tight loop, advancing the"
                  " cluster XID by ~16 M during 90 s.  The pattern doesn't touch"
                  " pgbench_accounts — in-phase windows are zero by design."
                  "  An explicit DELETE + VACUUM at end produces the only visible"
                  " activity.  KNOWN LIMITATION: the failsafe path "
                  " (vacuum_xid_failsafe_check, src/backend/commands/vacuum.c) needs"
                  " relfrozenxid to precede ReadNextTransactionId() − max(vacuum_failsafe_age,"
                  " autovacuum_freeze_max_age × 1.05).  With the minimum allowed"
                  " autovacuum_freeze_max_age = 100 000 and only 16 M XIDs burned,"
                  " the math should fire — but in practice the manual VACUUM in"
                  " our setup completes a normal index pass instead.  Both modes"
                  " show identical numbers (~50 000 idx_tuples_deleted) because the"
                  " contrast in this phase is XID-pressure based and 90 s isn't"
                  " enough to demo it locally.  case6_wraparound = 0 / 0 reflects"
                  " that honestly."),
}

# Per-row reading guide shown beside the chart.
ROW_GUIDE = [
    ("heap blks_dirty / sec",
     "Heap pages vacuum had to write per second.  High value means vacuum is "
     "actually doing reclaim work; suppressed value means vacuum is throttled or "
     "starved (B-middle, C-border) or has nothing to do."),
    ("heap WAL bytes / sec",
     "WAL emitted by heap vacuum work per second.  Spikes correspond to large "
     "VACUUM (FREEZE) bursts (lots of FPI) or to many index passes (BROKEN A — "
     "1354 passes, 876 MB WAL)."),
    ("tuples_frozen / sec",
     "Heap tuples frozen per second.  Stays ~0 when freeze GUCs are postponed "
     "(BROKEN D); spikes once the GUCs reset and the next vacuum catches up."),
    ("idx_tuples_deleted / sec",
     "Index entries removed per second.  Zero in BROKEN E (vacuum_index_cleanup="
     "off) and also zero whenever vacuum naturally bypasses index cleanup (dead "
     "items < 2 % of pages).  Non-zero only when there's a real backlog AND "
     "indexes are allowed to run."),
]


def write_html_slide(out_path: Path, *,
                     phase_pngs: dict[str, Path],
                     summary_png: Path | None,
                     detector: dict,
                     broken_present: bool,
                     fixed_present: bool) -> Path:
    sections = []

    # Title slide
    sections.append(f"""
    <section class="slide title">
      <h1>DELETE patterns × VACUUM misconfigurations</h1>
      <p class="subtitle">
        Six phases on pgbench_accounts, broken vs fixed.
        BROKEN samples drawn in <span class="b">red</span>,
        FIXED in <span class="f">green</span>.
        The amber band marks the diagnostic row for each phase.
      </p>
      <p class="status">
        broken data: <strong>{'present' if broken_present else 'MISSING'}</strong> &nbsp;·&nbsp;
        fixed data: <strong>{'present' if fixed_present else 'MISSING'}</strong>
      </p>
    </section>
    """)

    # Phase slides
    for phase in ALL_PHASES:
        png_path = phase_pngs.get(phase)
        if png_path is None or not png_path.exists():
            continue

        title  = PHASE_TITLES[phase]
        narr   = PHASE_NARRATIVE.get(phase, "")
        _, hl  = PHASE_HIGHLIGHTS.get(phase, (-1, ""))
        det_html = ""
        for fn, name, letter in DETECTORS:
            if letter != phase[0]:
                continue
            b = detector.get(fn, {}).get("broken", 0)
            f = detector.get(fn, {}).get("fixed",  0)
            det_html += (f'<li><code>{name}</code>: '
                         f'broken=<span class="b">{b}</span>, '
                         f'fixed=<span class="f">{f}</span></li>')

        guide_items = "".join(
            f'<div class="guide-item"><h4>{rname}</h4><p>{rdesc}</p></div>'
            for rname, rdesc in ROW_GUIDE
        )
        sections.append(f"""
    <section class="slide">
      <h2>{title}</h2>
      <p class="narrative">{narr}</p>
      <div class="callout">Look at: {hl}</div>
      <img src="{_b64(png_path)}" alt="{phase}"/>
      <h3 class="guide-title">How to read this chart (top → bottom)</h3>
      <div class="guide">{guide_items}</div>
      {('<h3 class="det-title">Detector hits during this phase</h3>'
        '<ul class="detectors">' + det_html + '</ul>') if det_html else ''}
    </section>
    """)

    # Summary slide
    if summary_png and summary_png.exists():
        rows = ""
        for fn, name, _ in DETECTORS:
            b = detector.get(fn, {}).get("broken", 0)
            f = detector.get(fn, {}).get("fixed",  0)
            rows += (f'<tr><td><code>{name}</code></td>'
                     f'<td class="b">{b}</td><td class="f">{f}</td></tr>')
        sections.append(f"""
    <section class="slide">
      <h2>Summary — per-phase totals & detector hits</h2>
      <img src="{_b64(summary_png)}" alt="summary"/>
      <table class="detsum">
        <thead><tr><th>detector</th><th>broken</th><th>fixed</th></tr></thead>
        <tbody>{rows}</tbody>
      </table>
    </section>
    """)

    body = "\n".join(sections)
    html = f"""<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8">
<title>DELETE × VACUUM misconfig — slide deck</title>
<style>
  * {{ box-sizing: border-box; margin: 0; padding: 0; }}
  body {{
    font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
    background: #0f172a; color: #e2e8f0; padding: 40px 20px;
  }}
  .slide {{
    max-width: 1200px; margin: 0 auto 60px; padding: 32px;
    background: #1e293b; border-radius: 16px;
    box-shadow: 0 6px 30px rgba(0,0,0,0.35);
  }}
  .slide.title {{
    text-align: center; padding: 60px 32px;
    background: linear-gradient(135deg, #1e293b 0%, #312e81 100%);
  }}
  h1 {{ font-size: 32px; margin-bottom: 18px; color: #f1f5f9; }}
  h2 {{ font-size: 22px; margin-bottom: 12px; color: #f1f5f9;
        border-bottom: 2px solid #334155; padding-bottom: 8px; }}
  .subtitle {{ font-size: 16px; color: #cbd5e1; margin: 16px auto;
              max-width: 760px; line-height: 1.55; }}
  .status {{ font-size: 13px; color: #94a3b8; margin-top: 24px; }}
  .narrative {{ font-size: 14px; line-height: 1.6; color: #cbd5e1;
               margin: 8px 0 16px; }}
  .callout {{
    background: #fef3c7; color: #7c2d12;
    border-left: 4px solid #f59e0b; border-radius: 6px;
    padding: 10px 14px; margin: 14px 0 18px;
    font-size: 13.5px; line-height: 1.5;
  }}
  img {{ width: 100%; border-radius: 8px; background: white; }}
  ul.detectors {{ padding-left: 22px; font-size: 14px;
                 color: #cbd5e1; line-height: 1.6; }}
  ul.detectors code {{ background: #0f172a; padding: 2px 6px;
                      border-radius: 4px; color: #fbbf24; }}
  h3.guide-title, h3.det-title {{
    margin-top: 22px; margin-bottom: 12px;
    font-size: 14px; color: #cbd5e1; text-transform: uppercase;
    letter-spacing: 0.06em;
  }}
  .guide {{
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 12px;
    margin-bottom: 12px;
  }}
  .guide-item {{
    background: #0f172a; border-radius: 8px; padding: 12px 14px;
    border-left: 3px solid #6366f1;
  }}
  .guide-item h4 {{ font-size: 13px; color: #fbbf24; margin-bottom: 6px;
                   font-family: ui-monospace, monospace; font-weight: 600; }}
  .guide-item p {{ font-size: 12.5px; color: #cbd5e1; line-height: 1.55; }}
  span.b {{ color: #fca5a5; font-weight: 700; }}
  span.f {{ color: #86efac; font-weight: 700; }}
  table.detsum {{ width: 60%; margin-top: 22px; border-collapse: collapse; }}
  table.detsum th, table.detsum td {{
    text-align: left; padding: 8px 12px;
    border-bottom: 1px solid #334155;
  }}
  table.detsum th {{ color: #f1f5f9; font-size: 13px; }}
  table.detsum td.b {{ color: #fca5a5; font-weight: 700; }}
  table.detsum td.f {{ color: #86efac; font-weight: 700; }}
  table.detsum code {{ color: #fbbf24; }}
</style>
</head><body>
{body}
</body></html>
"""
    out_path.write_text(html, encoding="utf-8")
    return out_path


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--broken-dir", default="out_broken", type=Path)
    ap.add_argument("--fixed-dir",  default="out_fixed",  type=Path)
    ap.add_argument("--out-dir",    default=".",          type=Path)
    ap.add_argument("--no-html",    action="store_true",
                    help="skip generating the HTML slide deck")
    args = ap.parse_args()

    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)

    broken_csv = args.broken_dir.resolve() / "delete_window_with_indexes.csv"
    fixed_csv  = args.fixed_dir.resolve()  / "delete_window_with_indexes.csv"
    broken = _read(broken_csv)
    fixed  = _read(fixed_csv)

    if not broken and not fixed:
        print("no data in either directory", file=sys.stderr)
        sys.exit(2)

    phase_pngs = {}
    for phase in ALL_PHASES:
        path = plot_phase(phase, broken, fixed,
                          out / f"delete_{phase}.png")
        phase_pngs[phase] = path
        print("wrote" if path else "skipped", path or f"delete_{phase}.png")

    bars = plot_summary(broken, fixed, out / "delete_summary_bars.png")
    print("wrote" if bars else "skipped",
          bars or "delete_summary_bars.png")

    counts = detector_counts(args.broken_dir.resolve(),
                             args.fixed_dir.resolve())
    print("\nDetector hit counts (rows in CSV):")
    print(f"  {'file':40s}  broken  fixed")
    for fn, side in counts.items():
        print(f"  {fn:40s}  {side.get('broken', 0):>6d}"
              f"  {side.get('fixed', 0):>5d}")

    if not args.no_html:
        slide = out / "delete_cases_slide.html"
        write_html_slide(slide,
                         phase_pngs={p: phase_pngs[p] for p in ALL_PHASES
                                     if phase_pngs.get(p) is not None},
                         summary_png=bars,
                         detector=counts,
                         broken_present=bool(broken),
                         fixed_present=bool(fixed))
        print(f"\nwrote {slide}")


if __name__ == "__main__":
    main()

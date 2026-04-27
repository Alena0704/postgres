#!/usr/bin/env python3
"""
Render the per-pattern workload profile from ext_vacuum_statistics
counters (no pg_stat_all_tables, no external joins).

Inputs (under --in-dir):
    pattern_phases.csv          — phase boundaries
    pattern_window_labeled.csv  — full evs_window rows + 'pattern' column
    pattern_summary.csv         — per-pattern totals

Outputs (under --out-dir):
    workload_<pattern>.png      — 4-panel per-pattern timeline:
        1) buffers   (read / hit / dirty / written) per second
        2) WAL       (records / FPI / bytes) per second
        3) changes   (tuples_deleted, pages_scanned, pages_removed,
                      recently_dead) per second
        4) freezing  (tuples_frozen, vm_new_frozen, vm_new_visible,
                      vm_new_visible_frozen, implied unfrozen pressure)
                      per second
    workload_overlay.png        — 4-panel overlay with one line per pattern
    pattern_comparison.png      — 8-panel bar chart, all dimensions
                                  normalised to /sec by phase length
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import re
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.dates as mdates  # noqa: E402
import matplotlib.pyplot as plt    # noqa: E402

TARGET_REL = "pgbench_accounts"
TZ_FIX = re.compile(r"([+-]\d{2})$")


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


def _series_for_pattern(rows, pattern, rel=TARGET_REL):
    cols = ["d_blks_read", "d_blks_hit", "d_blks_dirty", "d_blks_written",
            "d_wal_records", "d_wal_fpi", "d_wal_bytes",
            "d_tuples_deleted", "d_pages_scanned", "d_pages_removed",
            "d_recent_dead",
            "d_tuples_frozen", "d_vm_visible", "d_vm_frozen",
            "d_vm_visible_frozen"]
    out = {"t": [], "rel_sec": [], "dt_sec": [], **{c: [] for c in cols}}
    sub = [r for r in rows
           if r.get("pattern") == pattern and r.get("relname") == rel]
    sub.sort(key=lambda r: r["t_to"])
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


def _patterns(rows):
    seen = []
    for r in rows:
        p = r.get("pattern")
        if p and p not in seen:
            seen.append(p)
    return seen


def _format_x_seconds(ax):
    ax.set_xlabel("seconds since phase start")


def _draw_buffers(ax, x, s):
    ax.plot(x, _rate(s, "d_blks_read"),    label="read",    linewidth=1.5)
    ax.plot(x, _rate(s, "d_blks_hit"),     label="hit",     linewidth=1.2,
            alpha=0.7)
    ax.plot(x, _rate(s, "d_blks_dirty"),   label="dirtied", linewidth=1.6,
            color="tab:red")
    ax.plot(x, _rate(s, "d_blks_written"), label="written", linewidth=1.2,
            linestyle="--")
    ax.set_ylabel("buffers / sec")
    ax.legend(loc="upper right", fontsize=7, ncol=2)


def _draw_wal(ax, x, s):
    ax.plot(x, _rate(s, "d_wal_records"), label="records", linewidth=1.6)
    ax.plot(x, _rate(s, "d_wal_fpi"),     label="FPI",     linewidth=1.2,
            linestyle="--")
    ax_r = ax.twinx()
    ax_r.plot(x, _rate(s, "d_wal_bytes"), label="bytes",
              color="tab:red", linewidth=1.2, alpha=0.8)
    ax.set_ylabel("WAL records & FPI / sec")
    ax_r.set_ylabel("WAL bytes / sec")
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax_r.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, loc="upper right", fontsize=7)


def _draw_changes(ax, x, s):
    ax.plot(x, _rate(s, "d_tuples_deleted"), label="tuples_deleted",
            linewidth=1.6)
    ax.plot(x, _rate(s, "d_pages_scanned"), label="pages_scanned",
            linewidth=1.4, alpha=0.8)
    ax.plot(x, _rate(s, "d_pages_removed"), label="pages_removed",
            linewidth=1.2, linestyle="--")
    ax.plot(x, _rate(s, "d_recent_dead"), label="recently_dead",
            linewidth=1.2, linestyle=":")
    ax.set_ylabel("tuples / pages / sec")
    ax.legend(loc="upper right", fontsize=7, ncol=2)


def _draw_freeze(ax, x, s):
    ax.plot(x, _rate(s, "d_tuples_frozen"), label="tuples_frozen",
            linewidth=1.6, color="tab:orange")
    ax.plot(x, _rate(s, "d_vm_frozen"), label="vm_new_frozen",
            linewidth=1.6, color="tab:purple")
    ax.plot(x, _rate(s, "d_vm_visible"), label="vm_new_visible",
            linewidth=1.2, alpha=0.8)
    ax.plot(x, _rate(s, "d_vm_visible_frozen"),
            label="vm_new_visible_frozen", linewidth=1.0, linestyle=":")
    pressure = [max(d - f, 0)
                for d, f in zip(_rate(s, "d_blks_dirty"),
                                _rate(s, "d_vm_frozen"))]
    ax.plot(x, pressure, label="implied unfrozen pressure",
            linewidth=1.4, color="tab:red", linestyle="-.")
    ax.set_ylabel("tuples / pages / sec")
    ax.legend(loc="upper right", fontsize=7, ncol=2)


def plot_pattern_workload(rows, pattern, out_path):
    s = _series_for_pattern(rows, pattern)
    if not s["t"]:
        return None

    fig, axes = plt.subplots(4, 1, figsize=(11, 11))
    fig.suptitle(f"Pattern '{pattern}' on {TARGET_REL} — vacuum-side workload",
                 fontsize=12)
    _draw_buffers(axes[0], s["t"], s)
    _draw_wal    (axes[1], s["t"], s)
    _draw_changes(axes[2], s["t"], s)
    _draw_freeze (axes[3], s["t"], s)
    for ax in axes:
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))
    for label in axes[3].get_xticklabels():
        label.set_rotation(30); label.set_ha("right")
    axes[3].set_xlabel("sample_time")
    fig.tight_layout(rect=[0, 0, 1, 0.97])
    fig.savefig(out_path, dpi=130)
    plt.close(fig)
    return out_path


def plot_overlay(rows, patterns, out_path):
    if not patterns:
        return None

    fig, axes = plt.subplots(4, 1, figsize=(11, 11))
    fig.suptitle("All update patterns on pgbench_accounts (timelines aligned)",
                 fontsize=12)
    cmap = plt.get_cmap("tab10")
    metrics = [
        ("d_blks_dirty",     "blks_dirtied / sec"),
        ("d_wal_bytes",      "WAL bytes / sec"),
        ("d_tuples_deleted", "tuples_deleted / sec"),
        ("d_vm_frozen",      "vm_new_frozen / sec"),
    ]
    for ax, (col, lbl) in zip(axes, metrics):
        for idx, p in enumerate(patterns):
            s = _series_for_pattern(rows, p)
            if not s["t"]:
                continue
            ax.plot(s["rel_sec"], _rate(s, col),
                    color=cmap(idx % 10), label=p, linewidth=1.6)
        ax.set_ylabel(lbl)
        ax.legend(loc="upper right", fontsize=8)
    _format_x_seconds(axes[3])
    fig.tight_layout(rect=[0, 0, 1, 0.97])
    fig.savefig(out_path, dpi=130)
    plt.close(fig)
    return out_path


def plot_pattern_comparison(rows, summary, phases, out_path):
    sub = [r for r in summary if r.get("relname") == TARGET_REL]
    if not sub:
        return None

    durations = defaultdict(float)
    for p in phases:
        try:
            durations[p["pattern"]] += (
                _ts(p["ended_at"]) - _ts(p["started_at"])).total_seconds()
        except Exception:
            pass

    if not durations:
        # Fallback: use unique-window dt_sec
        seen = set()
        for r in rows:
            p = r.get("pattern")
            if not p:
                continue
            key = (p, r.get("s_from"), r.get("s_to"))
            if key in seen:
                continue
            seen.add(key)
            durations[p] += _f(r.get("dt_sec"))

    patterns = [r["pattern"] for r in sub]
    metrics = [
        ("blks_read",         "blks_read / sec"),
        ("blks_dirty",        "blks_dirtied / sec"),
        ("wal_records",       "wal_records / sec"),
        ("wal_bytes",         "wal_bytes / sec"),
        ("tuples_deleted",    "tuples_deleted / sec"),
        ("pages_scanned",     "pages_scanned / sec"),
        ("tuples_frozen",     "tuples_frozen / sec"),
        ("vm_frozen",         "vm_new_frozen / sec"),
    ]

    cmap = plt.get_cmap("tab10")
    bar_colors = [cmap(i % 10) for i, _ in enumerate(patterns)]

    fig, axes = plt.subplots(2, 4, figsize=(16, 8))
    for ax, (col, title) in zip(axes.flat, metrics):
        values = []
        for r in sub:
            d = durations.get(r["pattern"]) or 1.0
            values.append(_f(r[col]) / d)
        ax.bar(patterns, values, color=bar_colors)
        ax.set_title(title)
        ax.tick_params(axis="x", rotation=15)
    fig.suptitle(f"ext_vacuum_statistics rates on {TARGET_REL} per pattern",
                 fontsize=13)
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(out_path, dpi=130)
    plt.close(fig)
    return out_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in-dir",  default="out_patterns", type=Path)
    ap.add_argument("--out-dir", default=None, type=Path)
    args = ap.parse_args()

    in_dir  = args.in_dir.resolve()
    out_dir = (args.out_dir or in_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    rows    = _read(in_dir / "pattern_window_labeled.csv")
    summary = _read(in_dir / "pattern_summary.csv")
    phases  = _read(in_dir / "pattern_phases.csv")

    if not rows:
        print(f"empty input in {in_dir}", file=sys.stderr)
        sys.exit(2)

    patterns = _patterns(rows)

    for p in patterns:
        path = plot_pattern_workload(rows, p, out_dir / f"workload_{p}.png")
        print("wrote" if path else "skipped", path or f"workload_{p}")

    overlay = plot_overlay(rows, patterns,
                           out_dir / "workload_overlay.png")
    print("wrote" if overlay else "skipped",
          overlay or "workload_overlay.png")

    comp = plot_pattern_comparison(rows, summary, phases,
                                   out_dir / "pattern_comparison.png")
    print("wrote" if comp else "skipped",
          comp or "pattern_comparison.png")


if __name__ == "__main__":
    main()

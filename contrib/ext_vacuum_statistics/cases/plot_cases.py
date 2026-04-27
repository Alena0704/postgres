#!/usr/bin/env python3
"""
Plot the three misconfig cases using only ext_vacuum_statistics counters.

For each case we render a 4-panel figure:
   1.  the case-specific signal (delay_share, pages-per-pass, pages-per-tuple)
   2.  buffers (read / hit / dirtied / written) per second
   3.  WAL (records / FPI / bytes) per second
   4.  freezing & VM transitions (tuples_frozen, vm_new_frozen,
       vm_new_visible, vm_new_visible_frozen) plus an "unfrozen
       pressure" line: blks_dirty - vm_new_frozen.

Inputs (under --in-dir):
    all_window.csv          — full evs_window dump
    workload_profile.csv    — long-form per-relation per-second metrics
    case1_throttled.csv
    case2_mwm_small.csv
    case3_vac_thrash.csv

Outputs (under --out-dir):
    case1_throttled.png
    case2_mwm_small.png
    case3_vac_thrash.png
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
import matplotlib.dates as mdates
import matplotlib.pyplot as plt

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


def _series(rows, *cols, rel=TARGET_REL):
    sub = [r for r in rows if r.get("relname") == rel]
    sub.sort(key=lambda r: r["t_to"])
    out = {"t": [], "dt_sec": []}
    for c in cols:
        out[c] = []
    for r in sub:
        out["t"].append(_ts(r["t_to"]))
        out["dt_sec"].append(max(_f(r["dt_sec"]), 1.0))
        for c in cols:
            out[c].append(_f(r.get(c)))
    return out


def _rate(s, col):
    return [v / dt for v, dt in zip(s[col], s["dt_sec"])]


def _ratio(num, den):
    return [(n / d) if d else 0 for n, d in zip(num, den)]


def _format_x(ax):
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))
    for label in ax.get_xticklabels():
        label.set_rotation(30)
        label.set_ha("right")


def _plot_buffers(ax, s):
    ax.plot(s["t"], _rate(s, "d_blks_read"),    label="read",    linewidth=1.7)
    ax.plot(s["t"], _rate(s, "d_blks_hit"),     label="hit",     linewidth=1.4,
            alpha=0.7)
    ax.plot(s["t"], _rate(s, "d_blks_dirty"),   label="dirtied", linewidth=1.7,
            color="tab:red")
    ax.plot(s["t"], _rate(s, "d_blks_written"), label="written", linewidth=1.4,
            linestyle="--")
    ax.set_ylabel("buffers / sec")
    ax.legend(loc="upper left", fontsize=8, ncol=4)


def _plot_wal(ax, s):
    ax.plot(s["t"], _rate(s, "d_wal_records"), label="records", linewidth=1.7)
    ax.plot(s["t"], _rate(s, "d_wal_fpi"),     label="FPI",     linewidth=1.4,
            linestyle="--")
    ax_r = ax.twinx()
    ax_r.plot(s["t"], _rate(s, "d_wal_bytes"), label="bytes",
              color="tab:red", linewidth=1.4, alpha=0.8)
    ax.set_ylabel("WAL records & FPI / sec")
    ax_r.set_ylabel("WAL bytes / sec")
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax_r.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, loc="upper left", fontsize=8, ncol=3)


def _plot_freeze(ax, s):
    ax.plot(s["t"], _rate(s, "d_tuples_frozen"),
            label="tuples_frozen", linewidth=1.7, color="tab:orange")
    ax.plot(s["t"], _rate(s, "d_vm_visible"),
            label="vm_new_visible", linewidth=1.4, alpha=0.8)
    ax.plot(s["t"], _rate(s, "d_vm_frozen"),
            label="vm_new_frozen", linewidth=1.7, color="tab:purple")
    ax.plot(s["t"], _rate(s, "d_vm_visible_frozen"),
            label="vm_new_visible_frozen",
            linewidth=1.2, linestyle=":")
    # Implied "unfreeze pressure" — pages dirtied that vacuum did not
    # turn back into frozen pages within the same window.
    pressure = [max(d - f, 0)
                for d, f in zip(_rate(s, "d_blks_dirty"),
                                _rate(s, "d_vm_frozen"))]
    ax.plot(s["t"], pressure, label="implied unfrozen pressure",
            linewidth=1.4, color="tab:red", linestyle="-.")
    ax.set_ylabel("tuples / pages / sec")
    ax.legend(loc="upper left", fontsize=8, ncol=3)


def _common_signal_panels(fig, gs, s, signal_label, signal_values,
                          threshold=None, threshold_label=None):
    ax1 = fig.add_subplot(gs[0])
    ax1.plot(s["t"], signal_values, marker="o", linewidth=1.7,
             color="tab:blue")
    if threshold is not None:
        ax1.axhline(threshold, color="red", linestyle="--",
                    linewidth=1.0, label=threshold_label)
        ax1.legend(loc="upper right", fontsize=8)
    ax1.set_ylabel(signal_label)

    ax2 = fig.add_subplot(gs[1], sharex=ax1)
    _plot_buffers(ax2, s)

    ax3 = fig.add_subplot(gs[2], sharex=ax1)
    _plot_wal(ax3, s)

    ax4 = fig.add_subplot(gs[3], sharex=ax1)
    _plot_freeze(ax4, s)

    _format_x(ax4)
    ax4.set_xlabel("sample_time")
    return ax1, ax2, ax3, ax4


def _figure(title):
    fig = plt.figure(figsize=(11, 11))
    gs = fig.add_gridspec(4, 1, hspace=0.45)
    fig.suptitle(title, fontsize=12)
    return fig, gs


def plot_case1(rows, hits, out_path):
    s = _series(rows,
                "d_total", "d_delay",
                "d_blks_read", "d_blks_hit", "d_blks_dirty", "d_blks_written",
                "d_wal_records", "d_wal_fpi", "d_wal_bytes",
                "d_tuples_frozen", "d_vm_visible", "d_vm_frozen",
                "d_vm_visible_frozen", "d_tuples_deleted", "d_pages_scanned")
    if not s["t"]:
        return None
    delay_share = _ratio(s["d_delay"], s["d_total"])
    fig, gs = _figure(
        "Case 1 — vacuum throttled by autovacuum_vacuum_cost_delay\n"
        f"signal: delay_time / total_time on {TARGET_REL}")
    _common_signal_panels(fig, gs, s,
                          "delay_time / total_time", delay_share,
                          threshold=0.5,
                          threshold_label="threshold 0.5")
    fig.savefig(out_path, dpi=130, bbox_inches="tight")
    plt.close(fig)
    return out_path


def plot_case2(rows, hits, out_path):
    s = _series(rows,
                "d_idx_passes", "d_pages_scanned", "d_tuples_deleted",
                "d_blks_read", "d_blks_hit", "d_blks_dirty", "d_blks_written",
                "d_wal_records", "d_wal_fpi", "d_wal_bytes",
                "d_tuples_frozen", "d_vm_visible", "d_vm_frozen",
                "d_vm_visible_frozen")
    if not s["t"]:
        return None
    pages_per_pass = _ratio(s["d_pages_scanned"], s["d_idx_passes"])
    fig, gs = _figure(
        "Case 2 — maintenance_work_mem too small\n"
        f"signal: pages_scanned / index_vacuum_count on {TARGET_REL} "
        "(low → many index passes per vacuum)")
    ax1, *_ = _common_signal_panels(fig, gs, s,
        "pages_scanned / index_vacuum_count", pages_per_pass,
        threshold=500, threshold_label="threshold 500")
    fig.savefig(out_path, dpi=130, bbox_inches="tight")
    plt.close(fig)
    return out_path


def plot_case3(rows, hits, out_path):
    s = _series(rows,
                "d_pages_scanned", "d_tuples_deleted", "d_pages_removed",
                "d_blks_read", "d_blks_hit", "d_blks_dirty", "d_blks_written",
                "d_wal_records", "d_wal_fpi", "d_wal_bytes",
                "d_tuples_frozen", "d_vm_visible", "d_vm_frozen",
                "d_vm_visible_frozen", "d_total", "d_delay")
    if not s["t"]:
        return None
    pages_per_dead = [(p / max(t, 1))
                      for p, t in zip(s["d_pages_scanned"], s["d_tuples_deleted"])]
    fig, gs = _figure(
        "Case 3 — vacuum thrashing (autovacuum_vacuum_scale_factor too low)\n"
        f"signal: pages_scanned / tuples_deleted on {TARGET_REL} "
        "(high → vacuum reads pages it can't reclaim)")
    _common_signal_panels(fig, gs, s,
        "pages_scanned / tuples_deleted", pages_per_dead,
        threshold=5, threshold_label="threshold 5")
    fig.savefig(out_path, dpi=130, bbox_inches="tight")
    plt.close(fig)
    return out_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in-dir",  default="out", type=Path)
    ap.add_argument("--out-dir", default=None, type=Path)
    args = ap.parse_args()

    in_dir  = args.in_dir.resolve()
    out_dir = (args.out_dir or in_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = _read(in_dir / "all_window.csv")
    if not rows:
        print(f"empty all_window.csv in {in_dir}", file=sys.stderr)
        sys.exit(2)

    for fn, name in (
        (plot_case1, "case1_throttled.png"),
        (plot_case2, "case2_mwm_small.png"),
        (plot_case3, "case3_vac_thrash.png"),
    ):
        path = fn(rows, [], out_dir / name)
        print("wrote" if path else "skipped", path or name)


if __name__ == "__main__":
    main()

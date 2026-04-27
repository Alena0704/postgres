#!/usr/bin/env python3
"""
BROKEN vs FIXED side-by-side for the three misconfig cases.
Uses only ext_vacuum_statistics counters from each side's all_window.csv.

For each case the figure has 2 columns (BROKEN / FIXED) and 4 rows:
   row 1 — case-specific signal
   row 2 — buffers (read / hit / dirty / written) per second
   row 3 — WAL (records / FPI / bytes) per second
   row 4 — freezing & VM transitions + implied unfrozen pressure

Inputs:
    --broken-dir / all_window.csv
    --fixed-dir  / all_window.csv

Outputs (under --out-dir):
    compare_case1_throttled.png
    compare_case2_mwm_small.png
    compare_case3_vac_thrash.png
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import re
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

TARGET = "pgbench_accounts"
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


def _series(rows, *cols, rel=TARGET):
    sub = [r for r in rows if r.get("relname") == rel]
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


def _ratio(num, den):
    return [(n / d) if d else 0 for n, d in zip(num, den)]


def _draw_buffers(ax, s):
    ax.plot(s["rel_sec"], _rate(s, "d_blks_read"),    label="read",
            linewidth=1.5)
    ax.plot(s["rel_sec"], _rate(s, "d_blks_hit"),     label="hit",
            linewidth=1.2, alpha=0.7)
    ax.plot(s["rel_sec"], _rate(s, "d_blks_dirty"),   label="dirtied",
            linewidth=1.6, color="tab:red")
    ax.plot(s["rel_sec"], _rate(s, "d_blks_written"), label="written",
            linewidth=1.2, linestyle="--")
    ax.set_ylabel("buffers / sec")
    ax.legend(loc="upper right", fontsize=7, ncol=2)


def _draw_wal(ax, s):
    ax.plot(s["rel_sec"], _rate(s, "d_wal_records"), label="records",
            linewidth=1.6)
    ax.plot(s["rel_sec"], _rate(s, "d_wal_fpi"),     label="FPI",
            linewidth=1.2, linestyle="--")
    ax_r = ax.twinx()
    ax_r.plot(s["rel_sec"], _rate(s, "d_wal_bytes"), label="bytes",
              color="tab:red", linewidth=1.2, alpha=0.8)
    ax.set_ylabel("WAL records & FPI / sec")
    ax_r.set_ylabel("WAL bytes / sec")
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax_r.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, loc="upper right", fontsize=7)


def _draw_freeze(ax, s):
    ax.plot(s["rel_sec"], _rate(s, "d_tuples_frozen"),
            label="tuples_frozen", linewidth=1.6, color="tab:orange")
    ax.plot(s["rel_sec"], _rate(s, "d_vm_frozen"),
            label="vm_new_frozen", linewidth=1.6, color="tab:purple")
    ax.plot(s["rel_sec"], _rate(s, "d_vm_visible"),
            label="vm_new_visible", linewidth=1.2, alpha=0.8)
    ax.plot(s["rel_sec"], _rate(s, "d_vm_visible_frozen"),
            label="vm_new_visible_frozen", linewidth=1.0, linestyle=":")
    pressure = [max(d - f, 0)
                for d, f in zip(_rate(s, "d_blks_dirty"),
                                _rate(s, "d_vm_frozen"))]
    ax.plot(s["rel_sec"], pressure,
            label="implied unfrozen pressure",
            linewidth=1.4, color="tab:red", linestyle="-.")
    ax.set_ylabel("tuples / pages / sec")
    ax.legend(loc="upper right", fontsize=7, ncol=2)


def _signal_for_case(case, s):
    if case == "case1":
        return _ratio(s["d_delay"], s["d_total"]), \
               "delay_time / total_time", 0.5
    if case == "case2":
        return _ratio(s["d_pages_scanned"], s["d_idx_passes"]), \
               "pages_scanned / index_vacuum_count", 500
    if case == "case3":
        ratio = [(p / max(t, 1))
                 for p, t in zip(s["d_pages_scanned"], s["d_tuples_deleted"])]
        return ratio, "pages_scanned / tuples_deleted", 5
    raise ValueError(case)


def _draw_signal(ax, sig_values, sig_label, threshold, s, color):
    ax.plot(s["rel_sec"], sig_values, marker="o", linewidth=1.7, color=color)
    ax.axhline(threshold, color="grey", linestyle="--",
               label=f"threshold {threshold}")
    ax.set_ylabel(sig_label)
    ax.legend(loc="upper right", fontsize=7)


def _columns_for_case(case):
    base = ["d_blks_read", "d_blks_hit", "d_blks_dirty", "d_blks_written",
            "d_wal_records", "d_wal_fpi", "d_wal_bytes",
            "d_tuples_frozen", "d_vm_visible", "d_vm_frozen",
            "d_vm_visible_frozen", "d_pages_scanned", "d_tuples_deleted"]
    if case == "case1":
        return base + ["d_total", "d_delay"]
    if case == "case2":
        return base + ["d_idx_passes"]
    if case == "case3":
        return base + ["d_total", "d_delay"]
    return base


def plot_compare(case_id: str, broken_rows, fixed_rows,
                 broken_label: str, fixed_label: str,
                 title: str, out_path: Path) -> Path | None:
    cols = _columns_for_case(case_id)
    s_b = _series(broken_rows, *cols)
    s_f = _series(fixed_rows,  *cols)
    if not s_b["t"] and not s_f["t"]:
        return None

    fig, axes = plt.subplots(4, 2, figsize=(14, 12), sharex="col")
    fig.suptitle(title, fontsize=13)

    for col_idx, (s, label, color) in enumerate((
        (s_b, broken_label, "tab:red"),
        (s_f, fixed_label,  "tab:green"),
    )):
        sig_values, sig_label, threshold = _signal_for_case(case_id, s)
        ax_top = axes[0][col_idx]
        ax_top.set_title(label)
        _draw_signal(ax_top, sig_values, sig_label, threshold, s, color)

        _draw_buffers(axes[1][col_idx], s)
        _draw_wal    (axes[2][col_idx], s)
        _draw_freeze (axes[3][col_idx], s)
        axes[3][col_idx].set_xlabel("seconds since phase start")

    fig.tight_layout(rect=[0, 0, 1, 0.97])
    fig.savefig(out_path, dpi=130)
    plt.close(fig)
    return out_path


CASES = [
    ("case1", "compare_case1_throttled.png",
     "BROKEN: cost_delay=100ms, cost_limit=10",
     "FIXED: cost_delay=2ms, cost_limit=200",
     "Case 1 — vacuum throttling on pgbench_accounts"),
    ("case2", "compare_case2_mwm_small.png",
     "BROKEN: maintenance_work_mem=64kB",
     "FIXED: maintenance_work_mem=256MB",
     "Case 2 — index passes per vacuum on pgbench_accounts"),
    ("case3", "compare_case3_vac_thrash.png",
     "BROKEN: scale_factor=0.0, threshold=10, naptime=1s",
     "FIXED: scale_factor=0.05, threshold=50, naptime=1min",
     "Case 3 — vacuum thrashing on pgbench_accounts"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--broken-dir", default="out", type=Path)
    ap.add_argument("--fixed-dir",  default="out_fix", type=Path)
    ap.add_argument("--out-dir",    default=".", type=Path)
    args = ap.parse_args()

    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)

    broken = _read(args.broken_dir.resolve() / "all_window.csv")
    fixed  = _read(args.fixed_dir.resolve()  / "all_window.csv")

    if not broken and not fixed:
        print("no data in either directory", file=sys.stderr)
        sys.exit(2)

    for case_id, fname, b_label, f_label, title in CASES:
        path = plot_compare(case_id, broken, fixed,
                            b_label, f_label, title,
                            out / fname)
        print("wrote" if path else "skipped", path or fname)


if __name__ == "__main__":
    main()

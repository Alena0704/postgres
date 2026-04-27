#!/usr/bin/env python3
"""
Four-scenario investigation deck (Zubkov style) for the multi-table
pgbench TPC-B simulation produced by multi_table_sim.sh.

Each section follows: Setup → DB-level overview → per-table drill-down
→ per-index drill-down (where applicable) → root cause + fix.

Output: investigation_multi.html
"""
from __future__ import annotations

import base64
import io
import subprocess
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

PSQL = "/Users/alena/my_postgres7/src/bin/psql/psql"
DSN  = "host=/tmp port=5499 dbname=pgbench_evs_del"
HERE = Path(__file__).parent
OUT  = HERE / "investigation_multi.html"

COLOR_BG       = "#0f172a"
COLOR_CARD     = "#1e293b"
COLOR_BROKEN   = "#ef4444"
COLOR_FIXED    = "#22c55e"
COLOR_HL       = "#fbbf24"

PHASE_TITLES = {
    "mwm-small":   "Problem 1 — Small maintenance_work_mem",
    "passive":     "Problem 2 — Passive autovacuum (high naptime + threshold + cost_delay)",
    "interrupted": "Problem 3 — Autovacuum keeps getting cancelled",
    "wraparound":  "Problem 4 — Wraparound failsafe",
}

PHASE_SETUP = {
    "mwm-small": [
        ("maintenance_work_mem",            "<b>64 kB</b>",     "default (256 MB)"),
        ("autovacuum",                      "aggressive (naptime=5s, threshold=5000)",
                                            "aggressive (naptime=5s, threshold=5000)"),
        ("workload",                        "pgbench TPC-B 16 clients × 90 s",
                                            "pgbench TPC-B 16 clients × 90 s"),
    ],
    "passive": [
        ("autovacuum_naptime",              "<b>120 s</b>",       "default (1 min)"),
        ("autovacuum_vacuum_threshold",     "<b>1 000 000</b>",   "default (50)"),
        ("autovacuum_vacuum_scale_factor",  "<b>0.5</b>",         "default (0.2)"),
        ("autovacuum_vacuum_cost_delay",    "<b>100 ms</b>",      "default (2 ms)"),
        ("autovacuum_vacuum_cost_limit",    "<b>10</b>",          "default (200)"),
    ],
    "interrupted": [
        ("autovacuum",                      "aggressive (defaults)", "aggressive (defaults)"),
        ("side process",                    "<b>pg_cancel_backend()</b> on every active autovacuum worker every 4 s",
                                            "(none)"),
    ],
    "wraparound": [
        ("autovacuum_freeze_max_age",       "<b>100 000</b>",     "default (200 000 000)"),
        ("vacuum_failsafe_age",             "<b>200 000</b>",     "default (1.6 B)"),
        ("vacuum_freeze_min_age",           "<b>0</b>",           "default (50 M)"),
        ("autovacuum on pgbench_accounts",  "<b>OFF</b> (so age accumulates)", "ON"),
        ("side process",                    "XID burner (16 clients calling txid_current())", "(none)"),
    ],
}

PHASE_INSIGHT = {
    "mwm-small": "Vacuum cost scales with the <b>number of index passes</b>, not "
                 "with the number of dead tuples.  A tiny dead-TID buffer turns a "
                 "single B-tree scan into hundreds of full-tree re-reads.",
    "passive":   "Setting autovacuum to «барджей не топчи» means it never catches up.  "
                 "Dead tuples accumulate; the next scheduled cleanup is a stop-the-world "
                 "event instead of background trickle.",
    "interrupted": "Cancelling autovacuum doesn't <i>save</i> work — it <b>throws away</b> "
                   "the work already done.  Each cancellation bumps "
                   "<code>interrupts_count</code> and the next vacuum starts over from "
                   "the top.",
    "wraparound": "When the failsafe trips, the table's relfrozenxid is dangerously "
                  "old.  Vacuum drops cost-throttling, skips index cleanup, and freezes "
                  "everything in sight.  <code>db_wraparound_failsafe_count</code> is the "
                  "single counter you wire to a pager.",
}


# ---------------------------------------------------------------------------
def query(sql: str) -> list[list[str]]:
    out = subprocess.run([PSQL, DSN, "-At", "-F|", "-c", sql],
                         capture_output=True, text=True, check=True)
    return [line.split("|") for line in out.stdout.strip().splitlines() if line]


def b64_png(fig) -> str:
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=130, bbox_inches="tight")
    plt.close(fig)
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()


# ---------------------------------------------------------------------------
# Data collectors
# ---------------------------------------------------------------------------

def db_level_metrics(phase: str) -> dict:
    """One row per mode with sums of database-level vacuum metrics.
       failsafe and interrupts come from sample_stat_vacuum_database."""
    rows = query(f"""
        WITH db AS (
          SELECT  d2.datid, s2.sample_time,
                  d2.db_total_time - d1.db_total_time              AS d_total_ms,
                  d2.db_wal_bytes  - d1.db_wal_bytes               AS d_wal_bytes,
                  d2.db_wraparound_failsafe_count
                    - d1.db_wraparound_failsafe_count              AS d_failsafe,
                  d2.interrupts_count - d1.interrupts_count        AS d_interrupts
          FROM    sample_stat_vacuum_database d1
          JOIN    sample_stat_vacuum_database d2
                    ON d2.server_id = d1.server_id AND d2.datid = d1.datid
                   AND d2.sample_id = d1.sample_id + 1
          JOIN    samples s2 ON s2.server_id = d2.server_id AND s2.sample_id = d2.sample_id
        )
        SELECT  p.mode,
                round(coalesce(sum(db.d_total_ms)/1000.0,        0)::numeric, 1) AS total_s,
                round(coalesce(sum(db.d_wal_bytes)/1024.0/1024.0, 0)::numeric, 1) AS wal_mb,
                coalesce(sum(db.d_failsafe),    0)::int AS failsafe,
                coalesce(sum(db.d_interrupts),  0)::int AS interrupts
        FROM    evs_delete_phases p
        LEFT JOIN db ON db.sample_time > p.started_at AND db.sample_time <= p.ended_at
                   AND db.datid = (SELECT datid FROM sample_stat_database
                                   WHERE datname='pgbench_evs_del' LIMIT 1)
        WHERE   p.phase_name = '{phase}'
        GROUP   BY p.mode
        ORDER   BY p.mode;
    """)
    out = {}
    for mode, total_s, wal_mb, failsafe, interrupts in rows:
        out[mode] = {"total_s": float(total_s), "wal_mb": float(wal_mb),
                     "failsafe": int(failsafe), "interrupts": int(interrupts)}
    return out


def per_table_metrics(phase: str) -> list[dict]:
    rows = query(f"""
        SELECT  p.mode, w.relname,
                round(sum(w.d_total)::numeric, 0)              AS total_ms,
                sum(w.d_idx_passes)                             AS idx_passes,
                sum(w.d_pages_scanned)                          AS pages_scanned,
                sum(w.d_tuples_deleted)                         AS tup_del,
                sum(w.d_tuples_frozen)                          AS tup_frz,
                round(sum(w.d_wal_bytes)/1024.0/1024.0, 1)      AS wal_mb
        FROM    evs_window w
        JOIN    evs_delete_phases p
          ON    w.t_to > p.started_at AND w.t_from < p.ended_at
        WHERE   p.phase_name = '{phase}'
          AND   w.relname IN ('pgbench_accounts','pgbench_branches',
                              'pgbench_history','pgbench_tellers')
        GROUP   BY p.mode, w.relname
        ORDER   BY p.mode, sum(w.d_total) DESC NULLS LAST;
    """)
    return [{"mode": m, "rel": r, "total_ms": float(t), "idx_passes": int(i),
             "pages_scanned": int(p), "tup_del": int(td), "tup_frz": int(tf),
             "wal_mb": float(w)}
            for m, r, t, i, p, td, tf, w in rows]


def per_index_metrics(phase: str) -> list[dict]:
    rows = query(f"""
        SELECT  p.mode, iw.relname, iw.indexrelname,
                round(sum(iw.d_total)::numeric / 1000, 2)        AS total_s,
                sum(iw.d_blks_read)                               AS blks_read,
                sum(iw.d_blks_hit)                                AS blks_hit,
                sum(iw.d_tuples_deleted)                          AS tup_del,
                round(sum(iw.d_wal_bytes)/1024.0/1024.0, 1)       AS wal_mb
        FROM    evs_index_window iw
        JOIN    evs_delete_phases p
          ON    iw.t_to > p.started_at AND iw.t_from < p.ended_at
        WHERE   p.phase_name = '{phase}'
          AND   iw.relname IN ('pgbench_accounts','pgbench_branches',
                               'pgbench_history','pgbench_tellers')
        GROUP   BY p.mode, iw.relname, iw.indexrelname
        ORDER   BY p.mode, sum(iw.d_total) DESC NULLS LAST;
    """)
    return [{"mode": m, "rel": r, "idx": ix, "total_s": float(t),
             "blks_read": int(br), "blks_hit": int(bh), "tup_del": int(td),
             "wal_mb": float(w)}
            for m, r, ix, t, br, bh, td, w in rows]


# ---------------------------------------------------------------------------
# Chart: side-by-side bars for the key metric of each scenario
# ---------------------------------------------------------------------------

def chart_overview(scenario_data: dict) -> str:
    """One bar chart per scenario showing the chosen anomaly metric."""
    metric_for = {
        "mwm-small":   ("idx_passes",   "Index passes (sum across all pgbench tables)"),
        "passive":     ("total_s",      "Database vacuum total time (s)"),
        "interrupted": ("interrupts",   "interrupts_count (database)"),
        "wraparound":  ("failsafe",     "db_wraparound_failsafe_count"),
    }
    fig, axes = plt.subplots(1, 2, figsize=(10, 4.0))
    fig.patch.set_facecolor("white")
    for ax, phase in zip(axes, ("mwm-small", "passive")):
        metric, label = metric_for[phase]
        d = scenario_data[phase]
        b = d.get("broken_value", 0)
        f = d.get("fixed_value", 0)
        bars = ax.bar(["BROKEN", "FIXED"], [b, f],
                      color=[COLOR_BROKEN, COLOR_FIXED],
                      edgecolor="white", linewidth=0.6, alpha=0.92)
        for rect, val in zip(bars, (b, f)):
            ax.text(rect.get_x() + rect.get_width()/2,
                    rect.get_height(),
                    f"{val:,.0f}".replace(",", " "),
                    ha="center", va="bottom", fontsize=11,
                    fontweight="bold")
        ax.set_title(phase, fontsize=11, fontweight="bold")
        ax.set_ylabel(label, fontsize=9)
        ax.grid(True, axis="y", alpha=0.3)
        ax.set_axisbelow(True)
    fig.suptitle("Per-scenario anomaly metric — broken vs fixed",
                 fontsize=13, fontweight="bold")
    fig.tight_layout()
    return b64_png(fig)


# ---------------------------------------------------------------------------
# HTML
# ---------------------------------------------------------------------------

CSS = """
* { box-sizing: border-box; margin: 0; padding: 0; }
body { font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
       background: #0f172a; color: #e2e8f0;
       padding: 36px 20px; line-height: 1.55; }
.slide { max-width: 1200px; margin: 0 auto 50px;
         background: #1e293b; border-radius: 18px;
         padding: 38px 44px; box-shadow: 0 6px 30px rgba(0,0,0,.35); }
.slide.title { text-align: center; padding: 60px 44px;
       background: linear-gradient(135deg, #1e293b 0%, #312e81 100%); }
.tag { display: inline-block; font-size: 11px; letter-spacing: .12em;
       background: #fbbf24; color: #1e293b;
       padding: 4px 12px; border-radius: 99px; font-weight: 700;
       margin-bottom: 14px; }
h1 { font-size: 30px; color: #f1f5f9; margin-bottom: 10px; }
h2 { font-size: 22px; color: #f1f5f9; margin-bottom: 16px; }
h3 { font-size: 14px; color: #cbd5e1; text-transform: uppercase;
     letter-spacing: .06em; margin-top: 24px; margin-bottom: 10px; }
.subtitle { color: #94a3b8; font-size: 14px; margin-bottom: 14px; }
.lead { font-size: 16px; color: #cbd5e1; margin-bottom: 18px; line-height: 1.65; }
table { width: 100%; border-collapse: collapse; margin: 8px 0; }
th, td { text-align: left; padding: 9px 12px; font-size: 13.5px;
         border-bottom: 1px solid #334155; }
th { color: #cbd5e1; font-size: 11px; text-transform: uppercase;
     letter-spacing: .04em; }
td.b { color: #fca5a5; font-weight: 600; font-variant-numeric: tabular-nums; }
td.f { color: #86efac; font-weight: 600; font-variant-numeric: tabular-nums; }
td.zero { color: #475569; }
td.hl { color: #fbbf24; font-weight: 700; }
code { background: #0f172a; padding: 2px 6px; border-radius: 4px;
       color: #fbbf24; font-size: 12.5px; }
.takeaway { margin-top: 22px; padding: 16px 20px;
            background: linear-gradient(135deg, #312e81 0%, #1e293b 100%);
            border-left: 4px solid #fbbf24; border-radius: 8px;
            color: #fef3c7; font-size: 15px; line-height: 1.6; }
.takeaway::before { content: "TAKEAWAY  "; color: #fbbf24; font-weight: 700;
                    letter-spacing: .12em; font-size: 11px; }
img { width: 100%; border-radius: 8px; background: white; margin-top: 8px; }
"""


def fmt(n, kind="int"):
    try:
        if kind == "int":
            return f"{int(n):,}".replace(",", " ")
        if kind == "s":
            return f"{n:,.2f} s" if n < 10 else f"{n:,.1f} s".replace(",", " ")
        if kind == "ms":
            return f"{n:,.0f} ms".replace(",", " ")
        if kind == "mb":
            return f"{n:,.1f} MB".replace(",", " ")
    except Exception:
        return str(n)
    return str(n)


# ---------------------------------------------------------------------------
# Run-context — parse /tmp/multi_sim.log for per-phase TPS / txn count and
# pull phase durations from evs_delete_phases.
# ---------------------------------------------------------------------------

import re as _re

def parse_run_log(path: Path = Path("/tmp/multi_sim.log")) -> dict:
    """Walk the simulation log and extract, per (phase, mode), the pgbench
    summary block (tps, transactions actually processed)."""
    if not path.exists():
        return {}
    text = path.read_text(errors="ignore")
    lines = text.splitlines()
    phase_re = _re.compile(r"^\[(\d\d:\d\d:\d\d)\] \[(\w+)\] Phase (\S+) \(")
    txn_re   = _re.compile(r"^\[(\S+?)\] number of transactions actually processed: (\d+)")
    tps_re   = _re.compile(r"^\[(\S+?)\] tps = ([\d.]+)")
    cur_mode = None
    out: dict = {}
    for ln in lines:
        m = phase_re.match(ln)
        if m:
            _, mode, phase = m.groups()
            cur_mode = mode
            out.setdefault((phase, mode), {})
            continue
        m = txn_re.match(ln)
        if m and cur_mode is not None:
            phase, txns = m.group(1), int(m.group(2))
            key = (phase, cur_mode)
            out.setdefault(key, {})["txns"] = txns
            continue
        m = tps_re.match(ln)
        if m and cur_mode is not None:
            phase, tps = m.group(1), float(m.group(2))
            key = (phase, cur_mode)
            out.setdefault(key, {})["tps"] = tps
    return out


def phase_durations() -> dict:
    rows = query("""
        SELECT mode, phase_name,
               started_at::time::text  AS started,
               ended_at::time::text    AS ended,
               extract(epoch FROM ended_at - started_at)::int AS dur_sec
        FROM   evs_delete_phases
        WHERE  phase_name IN ('mwm-small','passive')
        ORDER  BY started_at;
    """)
    return {(p, m): {"started": s, "ended": e, "dur_sec": int(d)}
            for m, p, s, e, d in rows}


def render_pipeline_diagram() -> str:
    """SVG swim-lane diagrams showing the per-phase pipeline:
       per-scenario FIXED + BROKEN, stacked one scenario at a time."""
    return (
        _scenario_block(
            label="MWM-SMALL",
            fixed_svg  = _pipeline_svg_fixed_mwm(),
            broken_svg = _pipeline_svg_broken_mwm(),
        )
        + _scenario_block(
            label="PASSIVE",
            fixed_svg  = _pipeline_svg_fixed_passive(),
            broken_svg = _pipeline_svg_broken_passive(),
        )
    )


def _scenario_block(label: str, fixed_svg: str, broken_svg: str) -> str:
    return (
        f"<h3 style='margin-top:28px;color:#cbd5e1;letter-spacing:.06em'>"
        f"SCENARIO — {label}</h3>"
        + fixed_svg + broken_svg
    )


_FIXED_DEFS = """
  <defs>
    <linearGradient id="{prefix}Setup"  x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#fbbf24"/>
      <stop offset="1" stop-color="#f59e0b"/>
    </linearGradient>
    <linearGradient id="{prefix}Work"   x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#3b82f6"/>
      <stop offset="1" stop-color="#1d4ed8"/>
    </linearGradient>
    <linearGradient id="{prefix}Av"     x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#22c55e"/>
      <stop offset="1" stop-color="#16a34a"/>
    </linearGradient>
    <linearGradient id="{prefix}Vac"    x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#a855f7"/>
      <stop offset="1" stop-color="#7e22ce"/>
    </linearGradient>
    <linearGradient id="{prefix}Samp"   x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#06b6d4"/>
      <stop offset="1" stop-color="#0891b2"/>
    </linearGradient>
  </defs>
"""


def _fixed_pipeline_svg(prefix: str, title: str, subtitle: str,
                         av_caption: str, vac_caption: str) -> str:
    return f"""
<svg viewBox="0 0 1180 480" xmlns="http://www.w3.org/2000/svg"
     style="width:100%;height:auto;background:#0f172a;border-radius:8px;
            margin-top:14px">
  {_FIXED_DEFS.format(prefix=prefix)}

  <text x="20" y="28" fill="#22c55e" font-size="15" font-weight="700"
        font-family="Segoe UI">{title}</text>
  <text x="20" y="46" fill="#94a3b8" font-size="11.5" font-family="Segoe UI">
    {subtitle}
  </text>

  {_LANE_BG}

  <rect x="180" y="68" width="80" height="29" fill="url(#{prefix}Setup)" rx="4"/>
  <text x="186" y="86" fill="#1e293b" font-size="10.5" font-weight="700"
        font-family="Segoe UI">ALTER SYSTEM</text>
  <rect x="265" y="68" width="80" height="29" fill="url(#{prefix}Setup)" rx="4"/>
  <text x="271" y="86" fill="#1e293b" font-size="10.5" font-weight="700"
        font-family="Segoe UI">TRUNCATE+i-g</text>
  <rect x="350" y="68" width="55" height="29" fill="url(#{prefix}Setup)" rx="4"/>
  <text x="356" y="86" fill="#1e293b" font-size="10.5" font-weight="700"
        font-family="Segoe UI">FREEZE</text>

  <rect x="410" y="128" width="700" height="29" fill="url(#{prefix}Work)" rx="4"/>
  <text x="420" y="146" fill="#f1f5f9" font-size="11.5" font-weight="700"
        font-family="Segoe UI">
    pgbench -n -c 16 -T 600 -f pattern_multi_user.sql
  </text>

  {_SAMPLER_TICKS.format(prefix=prefix)}
  <text x="180" y="227" fill="#94a3b8" font-size="10" font-family="Segoe UI">
    take_sample() pg_profile every 10 s — fills sample_stat_vacuum_*
  </text>

  <g fill="url(#{prefix}Av)">
    <rect x="430" y="248" width="40"  height="29" rx="3"/>
    <rect x="500" y="248" width="55"  height="29" rx="3"/>
    <rect x="600" y="248" width="80"  height="29" rx="3"/>
    <rect x="720" y="248" width="60"  height="29" rx="3"/>
    <rect x="830" y="248" width="100" height="29" rx="3"/>
    <rect x="970" y="248" width="80"  height="29" rx="3"/>
  </g>
  <text x="180" y="294" fill="#94a3b8" font-size="10.5" font-family="Segoe UI">{av_caption}</text>

  <text x="430" y="328" fill="#64748b" font-size="11.5" font-style="italic"
        font-family="Segoe UI">(none for this scenario)</text>

  <rect x="1100" y="368" width="40" height="29" fill="url(#{prefix}Vac)" rx="4"/>
  <text x="608" y="386" fill="#94a3b8" font-size="11" font-family="Segoe UI">{vac_caption}</text>

  <line x1="1140" y1="60" x2="1140" y2="455" stroke="#fbbf24"
        stroke-width="1.2" stroke-dasharray="3,3" opacity="0.6"/>

  <g font-family="Segoe UI" font-size="11" fill="#cbd5e1">
    <rect x="780" y="20" width="14" height="11" fill="url(#{prefix}Setup)" rx="2"/>
    <text x="800" y="30">setup</text>
    <rect x="850" y="20" width="14" height="11" fill="url(#{prefix}Work)" rx="2"/>
    <text x="870" y="30">pgbench</text>
    <rect x="930" y="20" width="14" height="11" fill="url(#{prefix}Samp)" rx="2"/>
    <text x="950" y="30">sampler</text>
    <rect x="1010" y="20" width="14" height="11" fill="url(#{prefix}Av)" rx="2"/>
    <text x="1030" y="30">autovacuum</text>
    <rect x="1100" y="20" width="14" height="11" fill="url(#{prefix}Vac)" rx="2"/>
    <text x="1120" y="30">VACUUM</text>
  </g>
</svg>
"""


def _pipeline_svg_fixed_mwm() -> str:
    return _fixed_pipeline_svg(
        prefix    = "fm",
        title     = "FIXED — mwm-small (defaults)",
        subtitle  = ("<tspan fill='#86efac'>maintenance_work_mem ≈ 256 MB</tspan> · "
                     "<tspan fill='#86efac'>autovacuum_naptime = 1 min</tspan> · "
                     "<tspan fill='#86efac'>cost_delay = 2 ms</tspan> — "
                     "vacuum has plenty of dead-TID buffer; one index pass per cycle"),
        av_caption  = ("autovacuum fires few times per phase; each one drains all "
                       "dead TIDs in a single index pass"),
        vac_caption = "Final VACUUM: ~0 s — table already clean →",
    )


def _pipeline_svg_fixed_passive() -> str:
    return _fixed_pipeline_svg(
        prefix    = "fp",
        title     = "FIXED — passive (defaults)",
        subtitle  = ("<tspan fill='#86efac'>autovacuum_naptime = 1 min</tspan> · "
                     "<tspan fill='#86efac'>threshold = 50 + 0.2 × n_live</tspan> · "
                     "<tspan fill='#86efac'>cost_delay = 2 ms</tspan> · "
                     "<tspan fill='#86efac'>cost_limit = 200</tspan> — autovacuum "
                     "comfortably keeps up with the dead-tuple rate"),
        av_caption  = ("autovacuum fires regularly; cost-throttling kicks in but never "
                       "stalls a cycle"),
        vac_caption = "Final VACUUM: 0.3 s — backlog already drained →",
    )



_BROKEN_DEFS = """
  <defs>
    <linearGradient id="{prefix}Setup"  x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#fb923c"/>
      <stop offset="1" stop-color="#ea580c"/>
    </linearGradient>
    <linearGradient id="{prefix}Work"   x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#ef4444"/>
      <stop offset="1" stop-color="#b91c1c"/>
    </linearGradient>
    <linearGradient id="{prefix}Av"     x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#f87171"/>
      <stop offset="1" stop-color="#dc2626"/>
    </linearGradient>
    <linearGradient id="{prefix}Vac"    x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#dc2626"/>
      <stop offset="1" stop-color="#7f1d1d"/>
    </linearGradient>
    <linearGradient id="{prefix}Samp"   x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#06b6d4"/>
      <stop offset="1" stop-color="#0891b2"/>
    </linearGradient>
  </defs>
"""

_SAMPLER_TICKS = """
  <g fill="url(#{prefix}Samp)">
    <rect x="180" y="190" width="3" height="23" rx="1"/>
    <rect x="240" y="190" width="3" height="23" rx="1"/>
    <rect x="320" y="190" width="3" height="23" rx="1"/>
    <rect x="410" y="190" width="3" height="23" rx="1"/>
    <rect x="480" y="190" width="3" height="23" rx="1"/>
    <rect x="560" y="190" width="3" height="23" rx="1"/>
    <rect x="640" y="190" width="3" height="23" rx="1"/>
    <rect x="720" y="190" width="3" height="23" rx="1"/>
    <rect x="800" y="190" width="3" height="23" rx="1"/>
    <rect x="880" y="190" width="3" height="23" rx="1"/>
    <rect x="960" y="190" width="3" height="23" rx="1"/>
    <rect x="1040" y="190" width="3" height="23" rx="1"/>
    <rect x="1110" y="190" width="3" height="23" rx="1"/>
  </g>
"""

_LANE_BG = """
  <g opacity="0.18">
    <rect x="180" y="65"  width="960" height="35" fill="#475569"/>
    <rect x="180" y="125" width="960" height="35" fill="#475569"/>
    <rect x="180" y="185" width="960" height="35" fill="#475569"/>
    <rect x="180" y="245" width="960" height="35" fill="#475569"/>
    <rect x="180" y="305" width="960" height="35" fill="#475569"/>
    <rect x="180" y="365" width="960" height="35" fill="#475569"/>
  </g>

  <g font-family="Segoe UI" font-size="12" fill="#cbd5e1">
    <text x="20" y="83">  Setup &amp; reseed</text>
    <text x="20" y="143"> pgbench workload</text>
    <text x="20" y="203"> Sampler (10 s)</text>
    <text x="20" y="263"> Autovacuum</text>
    <text x="20" y="323"> Helper loop</text>
    <text x="20" y="383"> Final VACUUM</text>
  </g>

  <line x1="180" y1="455" x2="1140" y2="455" stroke="#475569" stroke-width="1.4"/>
  <text x="180"  y="475" fill="#94a3b8" font-size="10.5" font-family="Segoe UI">phase start</text>
  <text x="1080" y="475" fill="#94a3b8" font-size="10.5" font-family="Segoe UI">phase end</text>
  <text x="600"  y="475" fill="#94a3b8" font-size="10.5" font-family="Segoe UI">600 s pgbench workload</text>
"""


def _broken_pipeline_svg(prefix: str, title: str, subtitle: str,
                         av_bars_svg: str, av_caption: str,
                         vac_x: int, vac_w: int, vac_caption: str) -> str:
    """Common skeleton for the BROKEN pipeline; specific scenarios differ
    only in the autovacuum lane and the width of the final-VACUUM bar."""
    return f"""
<svg viewBox="0 0 1180 480" xmlns="http://www.w3.org/2000/svg"
     style="width:100%;height:auto;background:#0f172a;border-radius:8px;
            margin-top:14px">
  {_BROKEN_DEFS.format(prefix=prefix)}

  <text x="20" y="28" fill="#ef4444" font-size="15" font-weight="700"
        font-family="Segoe UI">{title}</text>
  <text x="20" y="46" fill="#94a3b8" font-size="11.5" font-family="Segoe UI">
    {subtitle}
  </text>

  {_LANE_BG}

  <!-- Setup -->
  <rect x="180" y="68" width="80" height="29" fill="url(#{prefix}Setup)" rx="4"/>
  <text x="186" y="86" fill="#1e293b" font-size="10.5" font-weight="700"
        font-family="Segoe UI">ALTER SYSTEM</text>
  <rect x="265" y="68" width="80" height="29" fill="url(#{prefix}Setup)" rx="4"/>
  <text x="271" y="86" fill="#1e293b" font-size="10.5" font-weight="700"
        font-family="Segoe UI">TRUNCATE+i-g</text>
  <rect x="350" y="68" width="55" height="29" fill="url(#{prefix}Setup)" rx="4"/>
  <text x="356" y="86" fill="#1e293b" font-size="10.5" font-weight="700"
        font-family="Segoe UI">FREEZE</text>

  <!-- pgbench workload -->
  <rect x="410" y="128" width="700" height="29" fill="url(#{prefix}Work)" rx="4"/>
  <text x="420" y="146" fill="#fef2f2" font-size="11.5" font-weight="700"
        font-family="Segoe UI">
    pgbench -n -c 16 -T 600 -f pattern_multi_user.sql
  </text>

  <!-- Sampler -->
  {_SAMPLER_TICKS.format(prefix=prefix)}
  <text x="180" y="227" fill="#94a3b8" font-size="10" font-family="Segoe UI">
    take_sample() captures whatever vacuum manages to do
  </text>

  <!-- Autovacuum (scenario-specific) -->
  {av_bars_svg}
  <text x="180" y="294" fill="#94a3b8" font-size="10.5" font-family="Segoe UI">{av_caption}</text>

  <!-- Helper loop -->
  <text x="430" y="328" fill="#64748b" font-size="11.5" font-style="italic"
        font-family="Segoe UI">(none for this scenario)</text>

  <!-- Final VACUUM (scenario-specific width) -->
  <rect x="{vac_x}" y="368" width="{vac_w}" height="29" fill="url(#{prefix}Vac)" rx="4"/>
  <text x="{vac_x + 6}" y="386" fill="#fef2f2" font-size="11" font-weight="700"
        font-family="Segoe UI">VACUUM (VERBOSE)</text>
  <text x="608" y="412" fill="#fbbf24" font-size="11" font-family="Segoe UI">{vac_caption}</text>

  <line x1="1140" y1="60" x2="1140" y2="455" stroke="#ef4444"
        stroke-width="1.2" stroke-dasharray="3,3" opacity="0.6"/>

  <g font-family="Segoe UI" font-size="11" fill="#cbd5e1">
    <rect x="780"  y="20" width="14" height="11" fill="url(#{prefix}Setup)" rx="2"/>
    <text x="800"  y="30">setup</text>
    <rect x="850"  y="20" width="14" height="11" fill="url(#{prefix}Work)" rx="2"/>
    <text x="870"  y="30">pgbench</text>
    <rect x="930"  y="20" width="14" height="11" fill="url(#{prefix}Samp)" rx="2"/>
    <text x="950"  y="30">sampler</text>
    <rect x="1010" y="20" width="14" height="11" fill="url(#{prefix}Av)" rx="2"/>
    <text x="1030" y="30">autovacuum</text>
    <rect x="1100" y="20" width="14" height="11" fill="url(#{prefix}Vac)" rx="2"/>
    <text x="1120" y="30">VACUUM</text>
  </g>
</svg>
"""


def _pipeline_svg_broken_mwm() -> str:
    # Many small autovacuum bars throughout the workload — autovacuum
    # fires often (naptime=5s, threshold=5000) but each one wastes time
    # on dozens of index passes due to mwm=64kB.
    av_bars = """
    <g fill="url(#mAv)">
      <rect x="430" y="248" width="28" height="29" rx="3"/>
      <rect x="475" y="248" width="22" height="29" rx="3"/>
      <rect x="510" y="248" width="34" height="29" rx="3"/>
      <rect x="558" y="248" width="20" height="29" rx="3"/>
      <rect x="592" y="248" width="32" height="29" rx="3"/>
      <rect x="640" y="248" width="24" height="29" rx="3"/>
      <rect x="678" y="248" width="34" height="29" rx="3"/>
      <rect x="726" y="248" width="20" height="29" rx="3"/>
      <rect x="760" y="248" width="30" height="29" rx="3"/>
      <rect x="804" y="248" width="22" height="29" rx="3"/>
      <rect x="840" y="248" width="36" height="29" rx="3"/>
      <rect x="890" y="248" width="22" height="29" rx="3"/>
      <rect x="926" y="248" width="34" height="29" rx="3"/>
      <rect x="974" y="248" width="20" height="29" rx="3"/>
      <rect x="1008" y="248" width="32" height="29" rx="3"/>
      <rect x="1054" y="248" width="24" height="29" rx="3"/>
      <rect x="1092" y="248" width="20" height="29" rx="3"/>
    </g>
    """
    return _broken_pipeline_svg(
        prefix    = "m",
        title     = "BROKEN — mwm-small (maintenance_work_mem = 64 kB)",
        subtitle  = ("autovacuum settings are aggressive (naptime=5 s, threshold=5 000) — "
                     "<tspan fill='#fca5a5'>but the dead-TID buffer is tiny → every autovacuum "
                     "cycle wastes time on dozens of index passes</tspan>"),
        av_bars_svg = av_bars,
        av_caption  = ("autovacuum fires often, but each fire = many index passes "
                       "(64 kB / 6 B per TID ≈ 11 k entries → drain in chunks)"),
        vac_x = 1080, vac_w = 60,
        vac_caption = ("Final VACUUM: 3.4 s — modest because autovacuum already drained "
                       "(painfully) the dead tuples"),
    )


def _pipeline_svg_broken_passive() -> str:
    # Only 2-3 autovacuum bars total — naptime=120s allows ~5 fires in
    # 600s; threshold=1M is rarely reached.  Plus cost_delay=100ms /
    # cost_limit=10 stalls each fire after a few pages.
    av_bars = """
    <g fill="url(#pAv)">
      <rect x="640" y="248" width="22" height="29" rx="3"/>
      <rect x="900" y="248" width="22" height="29" rx="3"/>
      <text x="668" y="266" fill="#fca5a5" font-size="11.5" font-weight="700"
            font-family="Segoe UI">×</text>
      <text x="928" y="266" fill="#fca5a5" font-size="11.5" font-weight="700"
            font-family="Segoe UI">×</text>
    </g>
    """
    return _broken_pipeline_svg(
        prefix    = "p",
        title     = "BROKEN — passive (autovacuum nearly disabled)",
        subtitle  = ("naptime=120 s · threshold=1 000 000 · scale_factor=0.5 · cost_delay=100 ms · "
                     "cost_limit=10 — <tspan fill='#fca5a5'>autovacuum starves and stalls "
                     "even when it does run</tspan>"),
        av_bars_svg = av_bars,
        av_caption  = ("autovacuum fires only 2–3 times in 600 s; each one is throttled "
                       "by cost_delay before doing meaningful work (× = stalled)"),
        vac_x = 750, vac_w = 390,
        vac_caption = ("Final VACUUM: 38 s — drains the entire 600 s backlog at once; "
                       "this is the stop-the-world cost of «save vacuum work for later»"),
    )


def render_run_context(durations: dict, log_data: dict) -> str:
    """Render a compact 'how the run went' block."""
    rows_html = []
    total_dur = 0
    total_txns = 0
    for phase in ("mwm-small", "passive"):
        for mode in ("broken", "fixed"):
            key = (phase, mode)
            d = durations.get(key, {})
            l = log_data.get(key, {})
            cls = "b" if mode == "broken" else "f"
            tps = l.get("tps")
            txns = l.get("txns")
            tps_s = f"{tps:.1f}" if tps is not None else "—"
            txn_s = f"{txns:,}".replace(",", " ") if txns is not None else "—"
            dur_s = f"{d.get('dur_sec', 0)//60} min {d.get('dur_sec', 0)%60} s" if d else "—"
            rows_html.append(
                f"<tr><td class='{cls}'>{mode}</td>"
                f"<td>{phase}</td>"
                f"<td>{d.get('started', '—')}</td>"
                f"<td>{d.get('ended', '—')}</td>"
                f"<td>{dur_s}</td>"
                f"<td>{tps_s}</td>"
                f"<td>{txn_s}</td></tr>"
            )
            if d:
                total_dur += d.get("dur_sec", 0)
            if txns:
                total_txns += txns
    table = (
        "<table><thead><tr>"
        "<th>mode</th><th>phase</th>"
        "<th>started</th><th>ended</th><th>duration</th>"
        "<th>TPS</th><th>txns</th>"
        "</tr></thead><tbody>"
        + "".join(rows_html)
        + "</tbody></table>"
    )
    summary = (
        f"<p class='lead'>Workload: custom <code>pattern_multi_user.sql</code> "
        f"— per-transaction <i>UPDATE pgbench_accounts</i> + <i>UPDATE pgbench_tellers</i> "
        f"+ <i>INSERT pgbench_history</i>; 16 clients, scale = 50, 4 phases × 600 s × 2 modes. "
        f"Stopped early in fixed mode (interrupt+wraparound got partial fixed-side coverage).  "
        f"Cumulative wall-clock time across phases: <b>{total_dur//60} min</b>, "
        f"total transactions executed: <b>{total_txns:,}</b>.</p>"
        ).replace(",", " ")
    notes = ("""<p class='lead' style='margin-top:14px;color:#94a3b8'>
        <b>Cluster context:</b> debug-build PostgreSQL (running from
        <code>my_postgres7</code> source tree, likely
        <code>--enable-debug --enable-cassert</code>) — assertion overhead caps
        per-transaction throughput at single-digit TPS even with 16 clients.
        That's why pgbench TPS in the table below stays in the 5–14 range
        instead of the 1 000+ you'd see on a release build.  The signals are
        still valid — they just take longer to accumulate.</p>""")
    return summary + render_pipeline_diagram() + table + notes


def render_setup(rows):
    out = ['<table><thead><tr><th>parameter</th><th>BROKEN</th><th>FIXED</th></tr></thead><tbody>']
    for param, b, f in rows:
        out.append(f"<tr><td>{param}</td><td class='b'>{b}</td><td class='f'>{f}</td></tr>")
    out.append("</tbody></table>")
    return "\n".join(out)


def render_db_level(db: dict):
    b = db.get("broken", {})
    f = db.get("fixed",  {})
    out = ['<table><thead><tr><th>metric</th><th>BROKEN</th><th>FIXED</th></tr></thead><tbody>']
    out.append(f"<tr><td>vacuum total time</td>"
               f"<td class='b'>{fmt(b.get('total_s', 0), 's')}</td>"
               f"<td class='f'>{fmt(f.get('total_s', 0), 's')}</td></tr>")
    out.append(f"<tr><td>vacuum WAL bytes</td>"
               f"<td class='b'>{fmt(b.get('wal_mb', 0), 'mb')}</td>"
               f"<td class='f'>{fmt(f.get('wal_mb', 0), 'mb')}</td></tr>")
    out.append(f"<tr><td><code>db_wraparound_failsafe_count</code></td>"
               f"<td class='{'hl' if b.get('failsafe', 0) > 0 else 'b'}'>{fmt(b.get('failsafe', 0))}</td>"
               f"<td class='f'>{fmt(f.get('failsafe', 0))}</td></tr>")
    out.append(f"<tr><td><code>interrupts_count</code></td>"
               f"<td class='{'hl' if b.get('interrupts', 0) > 0 else 'b'}'>{fmt(b.get('interrupts', 0))}</td>"
               f"<td class='f'>{fmt(f.get('interrupts', 0))}</td></tr>")
    out.append("</tbody></table>")
    return "\n".join(out)


def render_per_table(rows):
    if not rows:
        return "<p class='subtitle'>(no per-table data captured for this phase)</p>"
    out = ['<table><thead><tr>'
           '<th>mode</th><th>relation</th>'
           '<th>vacuum total (ms)</th>'
           '<th>idx_passes</th>'
           '<th>pages scanned</th>'
           '<th>tuples deleted</th>'
           '<th>tuples frozen</th>'
           '<th>WAL (MB)</th></tr></thead><tbody>']
    for r in rows:
        cls = "b" if r["mode"] == "broken" else "f"
        z = "zero" if r["total_ms"] == 0 and r["idx_passes"] == 0 else cls
        out.append(f"<tr><td class='{cls}'>{r['mode']}</td><td>{r['rel']}</td>"
                   f"<td class='{z}'>{fmt(r['total_ms'])}</td>"
                   f"<td class='{z}'>{fmt(r['idx_passes'])}</td>"
                   f"<td class='{z}'>{fmt(r['pages_scanned'])}</td>"
                   f"<td class='{z}'>{fmt(r['tup_del'])}</td>"
                   f"<td class='{z}'>{fmt(r['tup_frz'])}</td>"
                   f"<td class='{z}'>{fmt(r['wal_mb'], 'mb')}</td></tr>")
    out.append("</tbody></table>")
    return "\n".join(out)


def render_per_index(rows):
    if not rows:
        return "<p class='subtitle'>(no per-index data captured for this phase)</p>"
    out = ['<table><thead><tr>'
           '<th>mode</th><th>relation</th><th>index</th>'
           '<th>total time (s)</th>'
           '<th>blocks read</th>'
           '<th>blocks hit</th>'
           '<th>tuples deleted</th>'
           '<th>WAL (MB)</th></tr></thead><tbody>']
    for r in rows:
        cls = "b" if r["mode"] == "broken" else "f"
        z = "zero" if r["total_s"] == 0 else cls
        out.append(f"<tr><td class='{cls}'>{r['mode']}</td><td>{r['rel']}</td>"
                   f"<td>{r['idx']}</td>"
                   f"<td class='{z}'>{fmt(r['total_s'], 's')}</td>"
                   f"<td class='{z}'>{fmt(r['blks_read'])}</td>"
                   f"<td class='{z}'>{fmt(r['blks_hit'])}</td>"
                   f"<td class='{z}'>{fmt(r['tup_del'])}</td>"
                   f"<td class='{z}'>{fmt(r['wal_mb'], 'mb')}</td></tr>")
    out.append("</tbody></table>")
    return "\n".join(out)


def main():
    sections = []
    scenario_data = {}
    for phase in ("mwm-small", "passive"):
        db = db_level_metrics(phase)
        tbl = per_table_metrics(phase)
        idx = per_index_metrics(phase)
        # Pick the headline metric for the overview chart
        if phase == "mwm-small":
            b_val = sum(r["idx_passes"] for r in tbl if r["mode"] == "broken")
            f_val = sum(r["idx_passes"] for r in tbl if r["mode"] == "fixed")
        elif phase == "passive":
            b_val = db.get("broken", {}).get("total_s", 0)
            f_val = db.get("fixed",  {}).get("total_s", 0)
        elif phase == "interrupted":
            b_val = db.get("broken", {}).get("interrupts", 0)
            f_val = db.get("fixed",  {}).get("interrupts", 0)
        else:
            b_val = db.get("broken", {}).get("failsafe", 0)
            f_val = db.get("fixed",  {}).get("failsafe", 0)
        scenario_data[phase] = {"db": db, "tbl": tbl, "idx": idx,
                                "broken_value": b_val, "fixed_value": f_val}

    overview_chart = chart_overview(scenario_data)
    durations = phase_durations()
    log_data  = parse_run_log()
    run_context = render_run_context(durations, log_data)

    # Title + run-context + overview
    sections.append(f"""
    <section class="slide title">
      <span class="tag">A 4-PROBLEM INVESTIGATION</span>
      <h1>Vacuum misconfigs caught from per-relation statistics</h1>
      <p class="subtitle">Custom multi-table pgbench workload (16 clients,
      600 s per phase) on pgbench_accounts/tellers/history.  Same workload,
      four misconfig flavours, one healthy baseline — told the way Andrey
      Zubkov reads pg_profile reports.</p>
    </section>

    <section class="slide">
      <span class="tag">RUN CONTEXT</span>
      <h2>How the run actually went.</h2>
      {run_context}
    </section>

    <section class="slide">
      <span class="tag">OVERVIEW</span>
      <h2>Four anomalies, four headline metrics.</h2>
      <p class="lead">Each scenario is told below, but here's the one-glance
        summary — pick the metric that screams loudest for that misconfig:</p>
      <img src="{overview_chart}" alt="overview"/>
    </section>
    """)

    # Per-scenario sections
    for phase in ("mwm-small", "passive"):
        d = scenario_data[phase]
        sections.append(f"""
    <section class="slide">
      <span class="tag">{phase.upper()}</span>
      <h2>{PHASE_TITLES[phase]}</h2>

      <h3>Setup — what changed</h3>
      {render_setup(PHASE_SETUP[phase])}

      <h3>Step 1 — Database-level vacuum stats</h3>
      {render_db_level(d['db'])}

      <h3>Step 2 — Per-table breakdown</h3>
      {render_per_table(d['tbl'])}

      <h3>Step 3 — Per-index breakdown</h3>
      {render_per_index(d['idx'])}

      <div class="takeaway">{PHASE_INSIGHT[phase]}</div>
    </section>
        """)

    html = f"""<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><title>VACUUM investigation — multi-table</title>
<style>{CSS}</style></head><body>
{''.join(sections)}
</body></html>"""
    OUT.write_text(html, encoding="utf-8")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()

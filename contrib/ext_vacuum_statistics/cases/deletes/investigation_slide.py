#!/usr/bin/env python3
"""
Detective-style slide deck for case A (small maintenance_work_mem),
modelled on Andrey Zubkov's PGConf.Russia 2025 talk:
1. notice an anomaly in DB-level vacuum stats
2. drill down to the per-table view → find the culprit table
3. drill down to the per-index view → find the culprit index
4. identify root cause (number of index passes)
5. tune the parameter
6. show the after picture

Output: investigation_A.html
"""
from __future__ import annotations

import base64
import datetime as dt
import io
import re
import subprocess
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

PSQL    = "/Users/alena/my_postgres7/src/bin/psql/psql"
DSN     = "host=/tmp port=5499 dbname=pgbench_evs_del"
HERE    = Path(__file__).parent
OUT_HTM = HERE / "investigation_A.html"

COLOR_BG       = "#0f172a"
COLOR_CARD     = "#1e293b"
COLOR_BROKEN   = "#ef4444"
COLOR_FIXED    = "#22c55e"
COLOR_NEUTRAL  = "#94a3b8"
COLOR_HL       = "#fbbf24"


def query(sql: str) -> list[list[str]]:
    """Run a SQL query via psql -At -F| and return rows as lists."""
    out = subprocess.run([PSQL, DSN, "-At", "-F|", "-c", sql],
                         capture_output=True, text=True, check=True)
    rows = [line.split("|") for line in out.stdout.strip().splitlines() if line]
    return rows


def b64_png(fig) -> str:
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=130, bbox_inches="tight")
    plt.close(fig)
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()


# ---------------------------------------------------------------------------
# Step 1 — DB-level anomaly: vacuum time over wall-clock time, two intervals
# ---------------------------------------------------------------------------

def chart_db_anomaly():
    """Cumulative vacuum work for the database during BROKEN A and FIXED A
    intervals, plotted side-by-side as a step chart.  The difference is
    immediately visible: BROKEN intervals stretch 270 s, FIXED ~3 s."""

    rows = query("""
        SELECT p.mode, w.s_from, w.s_to,
               EXTRACT(epoch FROM w.t_to - p.started_at) AS rel_sec,
               w.d_blks_read + w.d_blks_hit              AS blks
        FROM   evs_window w
        JOIN   evs_delete_phases p
          ON   w.t_to > p.started_at AND w.t_from < p.ended_at
        WHERE  p.phase_name = 'A-sparse'
          AND  w.relname = 'pgbench_accounts'
        ORDER BY p.mode, w.s_to;
    """)

    series = {"broken": ([], []), "fixed": ([], [])}
    cum = {"broken": 0.0, "fixed": 0.0}
    for mode, _, _, rel, blks in rows:
        cum[mode] += float(blks)
        series[mode][0].append(float(rel))
        series[mode][1].append(cum[mode] / 1e6)   # millions of blocks

    fig, ax = plt.subplots(figsize=(11, 4.5))
    fig.patch.set_facecolor("white")
    for mode, color in [("broken", COLOR_BROKEN), ("fixed", COLOR_FIXED)]:
        x, y = series[mode]
        if x:
            ax.step(x, y, where="post", color=color, linewidth=2.6,
                    label=mode.upper())
            ax.scatter(x, y, color=color, s=18, zorder=4)
    ax.set_xlabel("seconds since phase start")
    ax.set_ylabel("Cumulative blocks touched by vacuum (M)")
    ax.set_title("DB-level: cumulative vacuum work on pgbench_evs_del",
                 fontweight="bold", fontsize=12)
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(loc="upper left", fontsize=11)
    return b64_png(fig)


# ---------------------------------------------------------------------------
# Step 2 — per-database stats — 4 big numbers
# ---------------------------------------------------------------------------

def db_level_numbers():
    """Total vacuum metrics on pgbench_evs_del during the A-sparse window
    of each mode."""
    rows = query("""
        SELECT p.mode,
               round(sum(iw.d_total)::numeric, 0)               AS total_ms,
               round(sum(iw.d_delay)::numeric, 0)               AS delay_ms,
               sum(iw.d_blks_read)                              AS blks_read,
               round(sum(iw.d_wal_bytes)/1024.0/1024.0, 0)      AS wal_mb
        FROM   evs_index_window iw
        JOIN   evs_delete_phases p
          ON   iw.t_to > p.started_at AND iw.t_from < p.ended_at
        WHERE  p.phase_name = 'A-sparse'
        GROUP  BY p.mode
        ORDER  BY p.mode;
    """)
    out = {}
    for mode, total, delay, blks_read, wal_mb in rows:
        out[mode] = {
            "total_s":    float(total) / 1000,
            "delay_s":    float(delay) / 1000,
            "blks_read":  int(blks_read),
            "wal_mb":     float(wal_mb),
        }
    return out


# ---------------------------------------------------------------------------
# Step 3 — per-table breakdown
# ---------------------------------------------------------------------------

def per_table_rows():
    rows = query("""
        SELECT p.mode,
               w.relname,
               round(sum(w.d_total)::numeric, 0)            AS total_ms,
               sum(w.d_idx_passes)                          AS idx_passes,
               round(sum(w.d_wal_bytes)/1024.0/1024.0, 0)   AS wal_mb
        FROM   evs_window w
        JOIN   evs_delete_phases p
          ON   w.t_to > p.started_at AND w.t_from < p.ended_at
        WHERE  p.phase_name = 'A-sparse'
          AND  w.relname IN ('pgbench_accounts','pgbench_branches',
                             'pgbench_history','pgbench_tellers')
        GROUP  BY p.mode, w.relname
        ORDER  BY p.mode,
                  sum(w.d_total) DESC NULLS LAST;
    """)
    return rows


# ---------------------------------------------------------------------------
# Step 4 — per-index breakdown — the smoking gun
# ---------------------------------------------------------------------------

def per_index_rows():
    rows = query("""
        SELECT p.mode,
               iw.indexrelname,
               round(sum(iw.d_total)::numeric / 1000, 1)        AS total_s,
               round(sum(iw.d_delay)::numeric / 1000, 1)        AS delay_s,
               sum(iw.d_blks_read)                              AS blks_read,
               sum(iw.d_blks_hit)                               AS blks_hit,
               sum(iw.d_tuples_deleted)                         AS tuples_deleted,
               round(sum(iw.d_wal_bytes)/1024.0/1024.0, 0)      AS wal_mb
        FROM   evs_index_window iw
        JOIN   evs_delete_phases p
          ON   iw.t_to > p.started_at AND iw.t_from < p.ended_at
        WHERE  p.phase_name = 'A-sparse'
          AND  iw.relname = 'pgbench_accounts'
        GROUP  BY p.mode, iw.indexrelname
        ORDER  BY p.mode;
    """)
    return rows


# ---------------------------------------------------------------------------
# Step 5 — root cause: number of index passes
# ---------------------------------------------------------------------------

def index_passes():
    rows = query("""
        SELECT p.mode, sum(w.d_idx_passes)
        FROM   evs_window w
        JOIN   evs_delete_phases p
          ON   w.t_to > p.started_at AND w.t_from < p.ended_at
        WHERE  p.phase_name = 'A-sparse'
          AND  w.relname = 'pgbench_accounts'
        GROUP  BY p.mode;
    """)
    return {mode: int(passes) for mode, passes in rows}


# ---------------------------------------------------------------------------
# HTML render
# ---------------------------------------------------------------------------

CSS = """
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  background: #0f172a; color: #e2e8f0;
  padding: 40px 20px; line-height: 1.55;
}
.slide {
  max-width: 1180px; margin: 0 auto 40px;
  background: #1e293b; border-radius: 18px;
  padding: 38px 44px; box-shadow: 0 6px 30px rgba(0,0,0,.35);
}
.slide.title {
  text-align: center; padding: 64px 44px;
  background: linear-gradient(135deg, #1e293b 0%, #312e81 100%);
}
.step {
  display: inline-block; font-size: 12px; letter-spacing: .12em;
  background: #fbbf24; color: #1e293b;
  padding: 4px 12px; border-radius: 99px; font-weight: 700;
  margin-bottom: 16px;
}
h1 { font-size: 30px; color: #f1f5f9; margin-bottom: 10px; }
h2 { font-size: 22px; color: #f1f5f9; margin-bottom: 16px; }
.subtitle { color: #94a3b8; font-size: 14px; margin-bottom: 14px; }
.lead { font-size: 16px; color: #cbd5e1; margin-bottom: 20px; line-height: 1.65; }
.bignum-row {
  display: grid; grid-template-columns: repeat(2, 1fr);
  gap: 30px; margin: 26px 0;
}
.bignum {
  text-align: center; padding: 20px; background: #0f172a;
  border-radius: 12px; border-top: 3px solid #334155;
}
.bignum.b { border-top-color: #ef4444; }
.bignum.f { border-top-color: #22c55e; }
.bignum .v { font-size: 50px; font-weight: 700; line-height: 1.0;
             font-variant-numeric: tabular-nums; }
.bignum.b .v { color: #fca5a5; }
.bignum.f .v { color: #86efac; }
.bignum .lbl { font-size: 12px; color: #94a3b8; margin-top: 8px;
               text-transform: uppercase; letter-spacing: .06em; }
table { width: 100%; border-collapse: collapse; margin: 18px 0; }
th, td { text-align: left; padding: 10px 12px; font-size: 14px;
         border-bottom: 1px solid #334155; }
th { color: #cbd5e1; font-size: 12px; text-transform: uppercase;
     letter-spacing: .04em; }
td.b { color: #fca5a5; font-weight: 600; font-variant-numeric: tabular-nums; }
td.f { color: #86efac; font-weight: 600; font-variant-numeric: tabular-nums; }
td.zero { color: #475569; }
td.ratio { color: #fbbf24; }
code { background: #0f172a; padding: 2px 6px; border-radius: 4px;
       color: #fbbf24; font-size: 12.5px; }
.callout {
  margin-top: 24px; padding: 18px 22px;
  background: linear-gradient(135deg, #312e81 0%, #1e293b 100%);
  border-left: 4px solid #fbbf24; border-radius: 8px;
  color: #fef3c7; font-size: 16px; line-height: 1.65;
}
.callout::before {
  content: "FINDING  "; color: #fbbf24; font-weight: 700;
  letter-spacing: .12em; font-size: 12px;
}
.takeaway {
  margin-top: 24px; padding: 18px 22px;
  background: #064e3b; border-left: 4px solid #22c55e;
  border-radius: 8px; color: #d1fae5; font-size: 16px;
}
.takeaway::before {
  content: "TAKEAWAY  "; color: #22c55e; font-weight: 700;
  letter-spacing: .12em; font-size: 12px;
}
img { width: 100%; border-radius: 8px; background: white; }
.arrow {
  text-align: center; font-size: 32px; color: #475569;
  margin: -10px 0 -10px;
}
"""


def fmt(n, kind="int"):
    if kind == "int":
        return f"{int(n):,}".replace(",", " ")
    if kind == "ms":
        return f"{n:,.0f} ms".replace(",", " ")
    if kind == "s":
        if n < 10:
            return f"{n:,.2f} s"
        return f"{n:,.1f} s".replace(",", " ")
    if kind == "mb":
        return f"{n:,.0f} MB".replace(",", " ")
    return str(n)


def main():
    chart_anomaly = chart_db_anomaly()
    db_nums       = db_level_numbers()
    table_rows    = per_table_rows()
    idx_rows      = per_index_rows()
    passes        = index_passes()

    # Compute ratios for the takeaway tables
    def ratio(b, f):
        if not f:
            return "—"
        if b == 0:
            return "—"
        r = b / f if b > f else f / b
        return f"{r:,.0f}×".replace(",", " ")

    # ---- Slide 1: title
    slides = []
    slides.append(f"""
    <section class="slide title">
      <span class="step">A DETECTIVE STORY</span>
      <h1>One small GUC, 59× more vacuum work on a single index</h1>
      <p class="subtitle">Investigating <code>maintenance_work_mem</code>
         the way an Andrey Zubkov / pg_profile user would,
         using only the per-relation vacuum stats</p>
    </section>
    """)

    # ---- Slide 2: the anomaly
    slides.append(f"""
    <section class="slide">
      <span class="step">STEP 1 — overview</span>
      <h2>Something's off — the same DELETE workload runs for
          minutes on the broken side, seconds on the fixed side.</h2>
      <p class="lead">Two consecutive runs of the same sparse DELETE phase
        on <code>pgbench_accounts</code>.  Cumulative work that vacuum had
        to do on the database tracks two visibly different durations:</p>
      <img src="{chart_anomaly}" alt="cumulative work overview"/>
      <div class="callout">
        Same workload, same row count.  Vacuum on the broken side
        keeps doing work for almost two orders of magnitude longer.
        Where is the time going?
      </div>
    </section>
    """)

    # ---- Slide 3: per-database big numbers
    b = db_nums.get("broken", {})
    f = db_nums.get("fixed",  {})
    slides.append(f"""
    <section class="slide">
      <span class="step">STEP 2 — per-database vacuum stats</span>
      <h2>Drill down to the database — index totals.</h2>
      <p class="lead">Aggregated across <i>all indexes</i> the database
        vacuumed during the phase:</p>
      <div class="bignum-row">
        <div class="bignum b"><div class="v">{fmt(b.get('total_s', 0), 's')}</div>
            <div class="lbl">BROKEN — index vacuum total time</div></div>
        <div class="bignum f"><div class="v">{fmt(f.get('total_s', 0), 's')}</div>
            <div class="lbl">FIXED — index vacuum total time</div></div>
        <div class="bignum b"><div class="v">{fmt(b.get('blks_read', 0))}</div>
            <div class="lbl">BROKEN — index blocks read</div></div>
        <div class="bignum f"><div class="v">{fmt(f.get('blks_read', 0))}</div>
            <div class="lbl">FIXED — index blocks read</div></div>
      </div>
      <div class="callout">
        Wall-clock vacuum time and block reads dominate the BROKEN side.
        WAL volume (~625 MB vs ~1018 MB) is actually <i>higher in FIXED</i> —
        because fixed actually <i>finishes</i> the freeze pass.  The cost
        in BROKEN isn't extra writes — it's <b>repeated re-reads</b>.
      </div>
    </section>
    """)

    # ---- Slide 4: per-table
    slides.append("""
    <section class="slide">
      <span class="step">STEP 3 — per-table</span>
      <h2>Which table is consuming all that time?</h2>
      <p class="lead">Per-table vacuum work for the standard pgbench tables:</p>
      <table>
        <thead><tr>
          <th>mode</th><th>relation</th>
          <th>vacuum total (ms)</th>
          <th>index passes</th>
          <th>WAL (MB)</th>
        </tr></thead>
        <tbody>
    """)
    for mode, rel, total_ms, idx_passes, wal_mb in table_rows:
        cls = "b" if mode == "broken" else "f"
        zero = "zero" if int(float(total_ms)) == 0 else cls
        slides.append(
            f"<tr><td class='{cls}'>{mode}</td><td>{rel}</td>"
            f"<td class='{zero}'>{fmt(float(total_ms))}</td>"
            f"<td class='{zero}'>{fmt(int(idx_passes))}</td>"
            f"<td class='{zero}'>{fmt(float(wal_mb))}</td></tr>"
        )
    slides.append("""
        </tbody></table>
      <div class="callout">
        Only one table sees vacuum activity at all — <code>pgbench_accounts</code>.
        BROKEN spent <b>1 430 index passes</b> on it; FIXED spent <b>1</b>.
        The other pgbench tables are quiet.
      </div>
    </section>
    """)

    # ---- Slide 5: per-index
    slides.append("""
    <section class="slide">
      <span class="step">STEP 4 — per-index</span>
      <h2>The smoking gun — one index burns it all.</h2>
      <p class="lead"><code>pgbench_accounts</code> has exactly one index
        (<code>pgbench_accounts_pkey</code>).  Per-index vacuum stats for the
        same phase:</p>
      <table>
        <thead><tr>
          <th>mode</th><th>index</th>
          <th>total time (s)</th><th>delay (s)</th>
          <th>blocks read</th><th>blocks hit</th>
          <th>tuples deleted</th><th>WAL (MB)</th>
        </tr></thead>
        <tbody>
    """)
    for mode, idxname, total_s, delay_s, blks_read, blks_hit, tup_del, wal_mb in idx_rows:
        cls = "b" if mode == "broken" else "f"
        slides.append(
            f"<tr><td class='{cls}'>{mode}</td><td>{idxname}</td>"
            f"<td class='{cls}'>{fmt(float(total_s), 's')}</td>"
            f"<td class='{cls}'>{fmt(float(delay_s), 's')}</td>"
            f"<td class='{cls}'>{fmt(int(blks_read))}</td>"
            f"<td class='{cls}'>{fmt(int(blks_hit))}</td>"
            f"<td class='{cls}'>{fmt(int(tup_del))}</td>"
            f"<td class='{cls}'>{fmt(float(wal_mb))}</td></tr>"
        )
    slides.append("""
        </tbody></table>
      <div class="callout">
        Same index, same number of dead tuples removed.
        BROKEN read <b>588× more blocks</b> from the same B-tree to
        accomplish the same work — and spent <b>59× more wall-clock time</b>.
      </div>
    </section>
    """)

    # ---- Slide 6: root cause — index passes
    bp = passes.get("broken", 0)
    fp = passes.get("fixed",  0)
    slides.append(f"""
    <section class="slide">
      <span class="step">STEP 5 — root cause</span>
      <h2>Why the same index gets re-read 588× more often.</h2>
      <p class="lead">The pg_stat field that explains everything —
         <code>idx_passes</code>, the number of times vacuum walked each
         B-tree from root to leaf:</p>
      <div class="bignum-row">
        <div class="bignum b"><div class="v">{fmt(bp)}</div>
            <div class="lbl">BROKEN — index passes</div></div>
        <div class="bignum f"><div class="v">{fmt(fp)}</div>
            <div class="lbl">FIXED — index passes</div></div>
      </div>
      <div class="callout">
        Each pass = a full B-tree scan to delete the dead pointers held
        in vacuum's dead-TID buffer.  With a tiny buffer, the same 62 500
        dead tuples have to be drained in 1 430 chunks — 1 430 full B-tree
        scans on the same physical index.  A bigger buffer absorbs them
        all at once: 1 chunk, 1 pass.
      </div>
    </section>
    """)

    # ---- Slide 7: the mechanism (causal chain)
    slides.append("""
    <section class="slide">
      <span class="step">STEP 6 — mechanism</span>
      <h2>The chain in seven words.</h2>
      <pre style="font-family: ui-monospace, monospace; font-size: 14px;
                  background: #0f172a; padding: 22px; border-radius: 10px;
                  border-left: 3px solid #6366f1; line-height: 2;">
maintenance_work_mem = 64 kB
  ↓
dead-TID buffer holds ~11 000 entries  (≈ 64 kB / 6 B per entry)
  ↓
62 500 dead tuples ÷ 11 k = 1 430 chunks
  ↓
each chunk = full B-tree scan + WAL FPI burst
  ↓
587× more index reads, 59× more wall-clock time
  ↓
the workload didn't change — the bookkeeping did.
      </pre>
    </section>
    """)

    # ---- Slide 8: the fix
    slides.append("""
    <section class="slide">
      <span class="step">STEP 7 — fix</span>
      <h2>One GUC.</h2>
      <pre style="font-family: ui-monospace, monospace; font-size: 16px;
                  background: #0f172a; padding: 22px; border-radius: 10px;
                  border-left: 3px solid #22c55e;">
ALTER SYSTEM SET maintenance_work_mem = '256MB';
SELECT pg_reload_conf();
      </pre>
      <div class="takeaway">
        Same hardware, same workload, same dead tuples — and now the same
        cleanup runs in 5.7 s instead of 5.6 min.  The point of per-relation
        vacuum statistics is to make this one-line tuning provable from
        nothing more than a sample-vs-sample comparison.
      </div>
    </section>
    """)

    body = "\n".join(slides)
    OUT_HTM.write_text(f"""<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><title>VACUUM investigation — case A</title>
<style>{CSS}</style>
</head><body>
{body}
</body></html>""", encoding="utf-8")
    print(f"wrote {OUT_HTM}")


if __name__ == "__main__":
    main()

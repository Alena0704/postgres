#!/usr/bin/env bash
#
# Drive six DELETE-pattern + vacuum-misconfig phases on a pgbench
# database, with periodic pg_profile snapshots so the detector can flag
# each phase from the windowed extension counters.
#
# A single script handles both BROKEN and FIXED modes — pass MODE=broken
# (default) or MODE=fixed.  Two thin wrappers in this directory call it
# with the right mode and OUT_DIR.
#
# Phases (one DELETE pattern × one vacuum-GUC misconfig each):
#   A-sparse  : maintenance_work_mem=64kB        + sparse DELETE
#   B-middle  : cost_delay=100ms / cost_limit=10 + middle DELETE
#   C-border  : per-table autovacuum starved     + border DELETE
#   D-sparse  : per-table freeze postponed       + sparse DELETE
#   E-sparse  : per-table vacuum_index_cleanup=off + sparse DELETE
#   F-xidburn : global autovacuum_freeze_max_age=100000 + XID burner
#
# RUN_WRAPAROUND=1 is required to enable phase F because lowering
# autovacuum_freeze_max_age cluster-wide is disruptive on a shared
# instance.  Skipped silently otherwise.
#
# Output (under $OUT_DIR):
#   delete_phases.csv             — phase_id, mode, phase_name, started/ended, samples
#   delete_window.csv             — full evs_window enriched with phase + mode
#   delete_index_window.csv       — full evs_index_window with phase + mode
#   delete_window_with_indexes.csv
#   delete_case4_freeze_lag.csv   — detector hits per phase
#   delete_case5_index_bloat.csv
#   delete_case6_wraparound.csv
#   delete_case6_wraparound_db.csv
#
# Environment knobs (defaults match the rest of the cases/ scripts):
#   PG_BASE_CONN   keyword DSN minus dbname (default: 'host=/tmp port=5499')
#   ADMIN_DB       database for ALTER SYSTEM (default: postgres)
#   BENCH_DB       bench database (default: pgbench_evs_del)
#   PROFILE_DB     pg_profile database (default: $BENCH_DB)
#   SCALE          pgbench scale (default: 50)
#   PHASE_SECONDS  duration of each phase (default: 90)
#   SAMPLE_PERIOD  seconds between pg_profile samples (default: 15)
#   CLIENTS        pgbench -c for DELETE phases (default: 8)
#   XID_CLIENTS    pgbench -c for the XID burner (default: 32)
#   PHASES         comma-separated subset (default: all six)
#   MODE           broken | fixed (default: broken)
#   OUT_DIR        output dir (default: ./out_<MODE>)
#   RUN_WRAPAROUND set to 1 to actually run phase F

set -euo pipefail

PG_BASE_CONN=${PG_BASE_CONN:-'host=/tmp port=5499'}
ADMIN_DB=${ADMIN_DB:-postgres}
BENCH_DB=${BENCH_DB:-pgbench_evs_del}
PROFILE_DB=${PROFILE_DB:-$BENCH_DB}
SCALE=${SCALE:-50}
PHASE_SECONDS=${PHASE_SECONDS:-240}
SAMPLE_PERIOD=${SAMPLE_PERIOD:-10}
CLIENTS=${CLIENTS:-8}
XID_CLIENTS=${XID_CLIENTS:-32}
MODE=${MODE:-broken}
PHASES=${PHASES:-A-sparse,B-middle,C-border,D-sparse,E-sparse,F-xidburn}
OUT_DIR=${OUT_DIR:-$(pwd)/out_${MODE}}
RUN_WRAPAROUND=${RUN_WRAPAROUND:-0}

case "$MODE" in
    broken|fixed) ;;
    *) echo "MODE must be 'broken' or 'fixed', got: $MODE" >&2; exit 2;;
esac

HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT_DIR"

admin_conn()   { printf '%s dbname=%s' "$PG_BASE_CONN" "$ADMIN_DB"; }
bench_conn()   { printf '%s dbname=%s' "$PG_BASE_CONN" "$BENCH_DB"; }
profile_conn() { printf '%s dbname=%s' "$PG_BASE_CONN" "$PROFILE_DB"; }
psql_admin()   { psql "$(admin_conn)"   -v ON_ERROR_STOP=1 -At "$@"; }
psql_bench()   { psql "$(bench_conn)"   -v ON_ERROR_STOP=1 -At "$@"; }
psql_profile() { psql "$(profile_conn)" -v ON_ERROR_STOP=1 -At "$@"; }

for kv in $PG_BASE_CONN; do
    case "$kv" in
        host=*)     export PGHOST="${kv#host=}";;
        port=*)     export PGPORT="${kv#port=}";;
        user=*)     export PGUSER="${kv#user=}";;
        password=*) export PGPASSWORD="${kv#password=}";;
    esac
done

log() { printf '[%s] %s %s\n' "$(date +%H:%M:%S)" "[$MODE]" "$*"; }

take_sample() {
    psql_profile -c "SELECT * FROM take_sample();" >/dev/null
}

reset_global_vacuum_gucs() {
    psql_admin <<'SQL'
ALTER SYSTEM RESET autovacuum_vacuum_cost_delay;
ALTER SYSTEM RESET autovacuum_vacuum_cost_limit;
ALTER SYSTEM RESET autovacuum_naptime;
ALTER SYSTEM RESET autovacuum_vacuum_scale_factor;
ALTER SYSTEM RESET autovacuum_vacuum_threshold;
ALTER SYSTEM RESET autovacuum_vacuum_insert_scale_factor;
ALTER SYSTEM RESET autovacuum_vacuum_insert_threshold;
ALTER SYSTEM RESET autovacuum_freeze_max_age;
ALTER SYSTEM RESET vacuum_failsafe_age;
ALTER SYSTEM RESET vacuum_freeze_min_age;
ALTER SYSTEM RESET vacuum_freeze_table_age;
ALTER SYSTEM RESET maintenance_work_mem;
SELECT pg_reload_conf();
SQL
}

reset_table_vacuum_storage_params() {
    psql_bench <<'SQL'
ALTER TABLE pgbench_accounts RESET (
    autovacuum_enabled,
    autovacuum_vacuum_scale_factor,
    autovacuum_vacuum_threshold,
    autovacuum_vacuum_cost_delay,
    autovacuum_vacuum_cost_limit,
    autovacuum_freeze_min_age,
    autovacuum_freeze_max_age,
    autovacuum_freeze_table_age,
    vacuum_index_cleanup
);
SQL
}

reset_all_vacuum_state() {
    reset_global_vacuum_gucs
    reset_table_vacuum_storage_params
    # Make sure autovacuum is generally engaged so the FIXED phases
    # get some natural cleanup activity during the workload.
    psql_admin <<'SQL'
ALTER SYSTEM SET autovacuum_naptime              = '5s';
ALTER SYSTEM SET autovacuum_vacuum_scale_factor  = 0.0;
ALTER SYSTEM SET autovacuum_vacuum_threshold     = 5000;
SELECT pg_reload_conf();
SQL
}

# ---------------------------------------------------------------------------
# Setup: database, extensions, pgbench data, phase-tracking table.
# ---------------------------------------------------------------------------
log "Setting up databases & extensions"
psql_admin <<SQL
SELECT 'CREATE DATABASE $BENCH_DB'
WHERE  NOT EXISTS (SELECT 1 FROM pg_database WHERE datname = '$BENCH_DB')\gexec
SQL

psql_bench <<'SQL'
CREATE EXTENSION IF NOT EXISTS dblink;
CREATE EXTENSION IF NOT EXISTS pg_stat_statements VERSION '1.12';
CREATE EXTENSION IF NOT EXISTS ext_vacuum_statistics;
CREATE EXTENSION IF NOT EXISTS pg_profile;
ALTER SYSTEM RESET ext_vacuum_statistics.track_relations;
ALTER SYSTEM RESET ext_vacuum_statistics.track_tables;
SELECT pg_reload_conf();
SQL

if [[ "$(psql_bench -c \
        "SELECT count(*) FROM information_schema.tables \
         WHERE table_name='pgbench_accounts'")" != "1" ]]; then
    log "Initializing pgbench (scale=$SCALE)"
    pgbench -i -s "$SCALE" -q "$BENCH_DB"
fi

# Sequence used by every DELETE pattern to refill the heap as we delete,
# so the table doesn't shrink to zero across phases.  Start past max_aid.
MAX_AID=$(( SCALE * 100000 ))
psql_bench <<SQL
CREATE SEQUENCE IF NOT EXISTS evs_delete_seq START $((MAX_AID + 1));
SELECT setval('evs_delete_seq',
              GREATEST($((MAX_AID + 1))::bigint,
                       (SELECT COALESCE(max(aid), 0)::bigint
                          FROM pgbench_accounts)));
SQL

psql_profile <<'SQL'
CREATE TABLE IF NOT EXISTS evs_delete_phases (
    phase_id        serial primary key,
    mode            text       NOT NULL,
    phase_name      text       NOT NULL,
    description     text       NOT NULL,
    started_at      timestamptz NOT NULL,
    ended_at        timestamptz NOT NULL,
    sample_at_start integer,
    sample_at_end   integer
);
SQL

# ---------------------------------------------------------------------------
# Sampler in the background
# ---------------------------------------------------------------------------
SAMPLER_PID=""
start_sampler() {
    # Run forever (until stop_sampler kills us).  Earlier we capped the
    # number of iterations at PHASE_SECONDS / SAMPLE_PERIOD, but that
    # silenced the sampler during the explicit final VACUUM — and on
    # broken A-sparse that VACUUM lasts 45 minutes, leaving exactly one
    # post-VACUUM sample with everything aggregated.  Now we keep
    # sampling through pgbench AND through the final VACUUM, so the
    # cumulative chart has a real staircase instead of one big step.
    local period="$1"
    (
        while sleep "$period"; do
            psql_profile -c "SELECT * FROM take_sample();" >/dev/null || true
        done
    ) &
    SAMPLER_PID=$!
}
stop_sampler() {
    if [[ -n "$SAMPLER_PID" ]] && kill -0 "$SAMPLER_PID" 2>/dev/null; then
        kill "$SAMPLER_PID" 2>/dev/null || true
        wait "$SAMPLER_PID" 2>/dev/null || true
    fi
    SAMPLER_PID=""
}
trap 'stop_sampler' EXIT INT TERM

current_sample_id() {
    psql_profile -c \
        "SELECT max(sample_id) FROM samples \
         WHERE server_id = (SELECT server_id FROM servers WHERE server_name='local')"
}

# ---------------------------------------------------------------------------
# Phase configuration helpers
# ---------------------------------------------------------------------------
apply_phase_gucs() {
    local phase="$1"

    # Always start from a clean slate so misconfigs don't bleed into each other.
    reset_all_vacuum_state

    if [[ "$MODE" == "fixed" ]]; then
        # In fixed mode every phase uses healthy defaults — that's the
        # whole point of the comparison.  Per-phase additional tuning
        # below is what makes "fixed" different from "untouched".
        case "$phase" in
            A-sparse)
                psql_admin <<'SQL'
ALTER SYSTEM SET maintenance_work_mem = '256MB';
SELECT pg_reload_conf();
SQL
                ;;
            B-middle)
                psql_admin <<'SQL'
ALTER SYSTEM SET autovacuum_vacuum_cost_delay  = '2ms';
ALTER SYSTEM SET autovacuum_vacuum_cost_limit  = 200;
SELECT pg_reload_conf();
SQL
                ;;
            C-border)
                # RESET on table = "use cluster defaults"
                psql_bench <<'SQL'
ALTER TABLE pgbench_accounts RESET (
    autovacuum_enabled,
    autovacuum_vacuum_scale_factor,
    autovacuum_vacuum_threshold
);
SQL
                ;;
            D-sparse)
                psql_bench <<'SQL'
ALTER TABLE pgbench_accounts RESET (
    autovacuum_freeze_min_age,
    autovacuum_freeze_max_age,
    autovacuum_freeze_table_age
);
SQL
                ;;
            E-sparse)
                # Disable autovacuum on the table for the duration of the
                # phase so dead items accumulate above the natural index
                # scan-bypass threshold (~2% of pages).  Without this, the
                # final VACUUM in FIXED mode also bypasses indexes and the
                # comparison is invisible.  Index_cleanup itself is RESET.
                psql_bench <<'SQL'
ALTER TABLE pgbench_accounts SET (autovacuum_enabled = false);
ALTER TABLE pgbench_accounts RESET (vacuum_index_cleanup);
SQL
                ;;
            F-xidburn)
                # Defaults already applied by reset_all_vacuum_state.
                :
                ;;
        esac
        return
    fi

    # MODE = broken
    case "$phase" in
        A-sparse)
            psql_admin <<'SQL'
ALTER SYSTEM SET maintenance_work_mem        = '64kB';
ALTER SYSTEM SET autovacuum_naptime          = '5s';
ALTER SYSTEM SET autovacuum_vacuum_scale_factor = 0.0;
ALTER SYSTEM SET autovacuum_vacuum_threshold    = 50000;
SELECT pg_reload_conf();
SQL
            ;;
        B-middle)
            psql_admin <<'SQL'
ALTER SYSTEM SET autovacuum_vacuum_cost_delay  = '100ms';
ALTER SYSTEM SET autovacuum_vacuum_cost_limit  = 10;
ALTER SYSTEM SET autovacuum_naptime            = '5s';
ALTER SYSTEM SET autovacuum_vacuum_scale_factor = 0.0;
ALTER SYSTEM SET autovacuum_vacuum_threshold    = 1000;
SELECT pg_reload_conf();
SQL
            ;;
        C-border)
            # Per-table starvation: scale_factor and threshold so high
            # autovacuum effectively never fires until manual VACUUM.
            psql_bench <<'SQL'
ALTER TABLE pgbench_accounts SET (
    autovacuum_vacuum_scale_factor = 0.5,
    autovacuum_vacuum_threshold    = 1000000000
);
SQL
            ;;
        D-sparse)
            psql_bench <<'SQL'
ALTER TABLE pgbench_accounts SET (
    autovacuum_freeze_min_age   = 1000000000,
    autovacuum_freeze_max_age   = 2000000000,
    autovacuum_freeze_table_age = 2000000000
);
SQL
            ;;
        E-sparse)
            # See FIXED-side comment: also disable autovacuum so a real
            # backlog forms.  vacuum_index_cleanup=off then forces the
            # final explicit VACUUM to skip indexes (visible signal).
            psql_bench <<'SQL'
ALTER TABLE pgbench_accounts SET (autovacuum_enabled = false);
ALTER TABLE pgbench_accounts SET (vacuum_index_cleanup = off);
SQL
            ;;
        F-xidburn)
            psql_admin <<'SQL'
ALTER SYSTEM SET autovacuum_freeze_max_age = 100000;
ALTER SYSTEM SET vacuum_failsafe_age       = 80000;
ALTER SYSTEM SET vacuum_freeze_min_age     = 0;
ALTER SYSTEM SET autovacuum_naptime        = '5s';
SELECT pg_reload_conf();
SQL
            ;;
    esac
}

phase_description() {
    local phase="$1"
    case "$phase" in
        A-sparse)  printf 'maintenance_work_mem=64kB + sparse DELETE'    ;;
        B-middle)  printf 'cost_delay=100ms/limit=10 + middle DELETE'    ;;
        C-border)  printf 'per-table autovacuum starved + border DELETE' ;;
        D-sparse)  printf 'per-table freeze postponed + sparse DELETE'   ;;
        E-sparse)  printf 'per-table vacuum_index_cleanup=off + sparse'  ;;
        F-xidburn) printf 'autovacuum_freeze_max_age=100000 + xid burn'  ;;
    esac
}

# Some phases need a deterministic, high-effort vacuum at the end so
# the broken/fixed difference lands inside the last sample window
# (e.g. case E only changes its index counters when a real vacuum runs).
needs_final_vacuum() {
    case "$1" in
        A-sparse|D-sparse|E-sparse|F-xidburn) return 0;;
        *)                                    return 1;;
    esac
}

# Re-seed pgbench_accounts before EVERY phase so each phase starts with
# a fresh table state and isn't polluted by accumulated dead tuples or
# aid-range depletion from earlier phases.  Without this:
#   * pattern_delete_sparse hits empty aid slots after E/D drained them;
#   * the start-of-phase settle VACUUM(FREEZE) of phase N+1 catches up
#     phase N's backlog and that activity bleeds into phase N+1's first
#     sample window via pgstat asynchrony, blurring the per-phase signal.
needs_table_reseed() {
    return 0    # always reseed: gives each phase a fresh table state
                # and (for F) restores the rows our pre-VACUUM DELETE
                # needs to find in order to create dead tuples.
}

reseed_pgbench_accounts() {
    log "  reseeding pgbench_accounts (TRUNCATE + pgbench -i -I g)"
    psql_bench -c "TRUNCATE pgbench_accounts;"  >/dev/null
    pgbench -i -I g -s "$SCALE" -q "$BENCH_DB" 2>&1 | sed 's/^/    /'
    psql_bench -c "SELECT setval('evs_delete_seq', $((MAX_AID + 1)));" \
        >/dev/null
}

# ---------------------------------------------------------------------------
# Phase driver
# ---------------------------------------------------------------------------
run_phase() {
    local phase="$1"

    if [[ "$phase" == "F-xidburn" && "$RUN_WRAPAROUND" != "1" ]]; then
        log "Skipping $phase (set RUN_WRAPAROUND=1 to run)"
        return 0
    fi

    log "Phase $phase ($(phase_description "$phase"))"

    if needs_table_reseed "$phase"; then
        reseed_pgbench_accounts
    fi

    apply_phase_gucs "$phase"

    # Settle: vacuum (with sane settings) + sample so each phase starts
    # from a consistent baseline.  Skipped for F-xidburn: that phase
    # depends on the table having a substantial unfrozen age so the
    # post-burn final VACUUM can hit the failsafe path; the settle would
    # reset age to 0.
    if [[ "$phase" != "F-xidburn" ]]; then
        psql_bench -c "VACUUM (FREEZE) pgbench_accounts;" >/dev/null
    fi

    take_sample
    local s_start started ended s_end
    s_start=$(current_sample_id)
    started=$(date -u +"%Y-%m-%d %H:%M:%S+00")

    start_sampler "$SAMPLE_PERIOD"

    if [[ "$phase" == "F-xidburn" ]]; then
        pgbench -n -c "$XID_CLIENTS" -T "$PHASE_SECONDS" -P 30 \
                -f "$HERE/patterns/pattern_xid_burn.sql" \
                "$BENCH_DB" 2>&1 | sed "s/^/[$phase] /"
    else
        # All DELETE patterns share the same loader; the script differs.
        local script
        case "$phase" in
            A-sparse|D-sparse|E-sparse) script="pattern_delete_sparse.sql";;
            B-middle)                   script="pattern_delete_middle.sql";;
            C-border)                   script="pattern_delete_border.sql";;
        esac
        pgbench -n -c "$CLIENTS" -T "$PHASE_SECONDS" -P 30 \
                -f "$HERE/patterns/$script" "$BENCH_DB" \
                -D scale="$SCALE" 2>&1 | sed "s/^/[$phase] /"
    fi

    if needs_final_vacuum "$phase"; then
        # F-xidburn: create some dead tuples so the final VACUUM has work
        # to scan; otherwise the table is fully frozen + clean and the
        # failsafe path is never entered even though age >> failsafe_age.
        if [[ "$phase" == "F-xidburn" ]]; then
            log "  creating dead tuples on pgbench_accounts for F failsafe trigger"
            psql_bench -c "DELETE FROM pgbench_accounts WHERE aid % 100 = 1;" \
                >/dev/null || true
        fi
        log "  forcing explicit VACUUM pgbench_accounts (sampler keeps running)"
        psql_bench -c "VACUUM (VERBOSE) pgbench_accounts;" \
            > "$OUT_DIR/vac_${MODE}_${phase}.log" 2>&1 || true
    fi

    stop_sampler
    take_sample
    s_end=$(current_sample_id)
    ended=$(date -u +"%Y-%m-%d %H:%M:%S+00")

    psql_profile -v ON_ERROR_STOP=1 <<SQL
INSERT INTO evs_delete_phases (mode, phase_name, description,
                               started_at, ended_at,
                               sample_at_start, sample_at_end)
VALUES ('$MODE', '$phase',
        \$\$$(phase_description "$phase")\$\$,
        '$started'::timestamptz, '$ended'::timestamptz,
        $s_start, $s_end);
SQL

    log "  done (samples $s_start..$s_end)"
}

# ---------------------------------------------------------------------------
# Drive phases
# ---------------------------------------------------------------------------
IFS=',' read -r -a PHASE_LIST <<< "$PHASES"
for p in "${PHASE_LIST[@]}"; do
    run_phase "$p"
done

reset_all_vacuum_state

# ---------------------------------------------------------------------------
# Build detector outputs and dump
# ---------------------------------------------------------------------------
log "Running detectors"
psql_profile -f "$HERE/../detect_vacuum_misconfig.sql" >/dev/null
psql_profile -f "$HERE/detect_delete_cases.sql"

log "Exporting CSVs to $OUT_DIR"
psql_profile -c "\COPY (SELECT * FROM evs_delete_phases WHERE mode='$MODE' ORDER BY phase_id) \
                 TO '$OUT_DIR/delete_phases.csv' WITH CSV HEADER"

# Tag every window with its phase by interval-overlap.  We embed mode in
# the join so cross-mode runs don't mix.
psql_profile -v mode="$MODE" <<'SQL'
DROP TABLE IF EXISTS evs_delete_window_labeled;
CREATE UNLOGGED TABLE evs_delete_window_labeled AS
SELECT  w.*,
        p.phase_name AS phase,
        p.mode       AS mode
FROM    evs_window_with_indexes w
LEFT JOIN evs_delete_phases p
       ON  w.t_to   >  p.started_at
       AND w.t_from <  p.ended_at
       AND p.mode   = :'mode';

DROP TABLE IF EXISTS evs_delete_index_window_labeled;
CREATE UNLOGGED TABLE evs_delete_index_window_labeled AS
SELECT  iw.*,
        p.phase_name AS phase,
        p.mode       AS mode
FROM    evs_index_window iw
LEFT JOIN evs_delete_phases p
       ON  iw.t_to   >  p.started_at
       AND iw.t_from <  p.ended_at
       AND p.mode    = :'mode';
SQL

psql_profile -c "\COPY (SELECT * FROM evs_delete_window_labeled WHERE mode='$MODE' ORDER BY t_to, relname) \
                 TO '$OUT_DIR/delete_window_with_indexes.csv' WITH CSV HEADER"
psql_profile -c "\COPY (SELECT * FROM evs_delete_index_window_labeled WHERE mode='$MODE' ORDER BY t_to, relname, indexrelname) \
                 TO '$OUT_DIR/delete_index_window.csv' WITH CSV HEADER"
psql_profile -c "\COPY (SELECT * FROM evs_window ORDER BY t_to, relname) \
                 TO '$OUT_DIR/delete_window.csv' WITH CSV HEADER"
# Detector tables are global (built from all samples in the pg_profile DB),
# so filter rows whose [t_from, t_to] overlaps any phase from THIS mode.
# Otherwise out_fixed/ would also contain hits from the broken run and vice
# versa.
psql_profile -c "\COPY (SELECT c.* FROM evs_case4_freeze_lag c \
                 JOIN evs_delete_phases p ON c.t_to > p.started_at AND c.t_from < p.ended_at AND p.mode = '${MODE}' \
                 ORDER BY c.t_to, c.relname) \
                 TO '$OUT_DIR/delete_case4_freeze_lag.csv'    WITH CSV HEADER"
psql_profile -c "\COPY (SELECT c.* FROM evs_case5_index_bloat c \
                 JOIN evs_delete_phases p ON c.t_to > p.started_at AND c.t_from < p.ended_at AND p.mode = '${MODE}' \
                 ORDER BY c.t_to, c.relname) \
                 TO '$OUT_DIR/delete_case5_index_bloat.csv'   WITH CSV HEADER"
psql_profile -c "\COPY (SELECT c.* FROM evs_case6_wraparound c \
                 JOIN evs_delete_phases p ON c.t_to > p.started_at AND c.t_from < p.ended_at AND p.mode = '${MODE}' \
                 ORDER BY c.t_to, c.relname) \
                 TO '$OUT_DIR/delete_case6_wraparound.csv'    WITH CSV HEADER"
psql_profile -c "\COPY (SELECT c.* FROM evs_case6_wraparound_db c \
                 JOIN evs_delete_phases p ON c.t_to > p.started_at AND c.t_from < p.ended_at AND p.mode = '${MODE}' \
                 ORDER BY c.t_to, c.datname) \
                 TO '$OUT_DIR/delete_case6_wraparound_db.csv' WITH CSV HEADER"

log "Done. Outputs in $OUT_DIR"

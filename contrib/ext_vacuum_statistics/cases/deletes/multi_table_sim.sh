#!/usr/bin/env bash
#
# Active multi-table pgbench simulation that demonstrates four vacuum
# misconfig scenarios end-to-end, each with a matching healthy baseline:
#
#   1. mwm-small   — maintenance_work_mem = 64 kB
#   2. passive     — autovacuum nearly off + huge cost_delay
#   3. interrupted — autovacuum workers get cancelled mid-flight
#   4. wraparound  — low autovacuum_freeze_max_age + XID burner co-running
#
# Workload is the standard pgbench TPC-B-like script, which UPDATEs
# pgbench_accounts/branches/tellers and INSERTs into pgbench_history,
# so vacuum activity shows up across all four pgbench tables.
#
# Phases and their per-table/per-index totals are written into the
# existing pg_profile + ext_vacuum_statistics tables (evs_delete_phases,
# sample_stat_vacuum_*, evs_window, evs_index_window).  The companion
# investigation_multi.py reads them back to render a Zubkov-style deck.
#
# Run with:
#   PATH="$PWD/src/bin/psql:$PWD/src/bin/pgbench:$PATH" \
#     RUN_WRAPAROUND=1 \
#     contrib/ext_vacuum_statistics/cases/deletes/multi_table_sim.sh
#
# Then once for fixed:
#   MODE=fixed RUN_WRAPAROUND=1 ... multi_table_sim.sh

set -euo pipefail

PG_BASE_CONN=${PG_BASE_CONN:-'host=/tmp port=5499'}
ADMIN_DB=${ADMIN_DB:-postgres}
BENCH_DB=${BENCH_DB:-pgbench_evs_del}
PROFILE_DB=${PROFILE_DB:-$BENCH_DB}
SCALE=${SCALE:-50}
PHASE_SECONDS=${PHASE_SECONDS:-90}
SAMPLE_PERIOD=${SAMPLE_PERIOD:-10}
CLIENTS=${CLIENTS:-16}
XID_CLIENTS=${XID_CLIENTS:-16}
MODE=${MODE:-broken}
PHASES=${PHASES:-mwm-small,passive,interrupted,wraparound}
OUT_DIR=${OUT_DIR:-$(pwd)/out_multi_${MODE}}
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

take_sample()      { psql_profile -c "SELECT * FROM take_sample();" >/dev/null; }
current_sample_id(){ psql_profile -c "SELECT max(sample_id) FROM samples WHERE server_id=(SELECT server_id FROM servers WHERE server_name='local')"; }

# ---------------------------------------------------------------------------
# Background helpers
# ---------------------------------------------------------------------------
SAMPLER_PID=""; INTERRUPT_PID=""; BURNER_PID=""

start_sampler() {
    local period="$1"
    ( while sleep "$period"; do
          psql_profile -c "SELECT * FROM take_sample();" >/dev/null || true
      done ) &
    SAMPLER_PID=$!
}
stop_sampler() {
    if [[ -n "$SAMPLER_PID" ]] && kill -0 "$SAMPLER_PID" 2>/dev/null; then
        kill "$SAMPLER_PID" 2>/dev/null || true
        wait "$SAMPLER_PID" 2>/dev/null || true
    fi
    SAMPLER_PID=""
}

# Repeatedly cancel any in-flight autovacuum worker on $BENCH_DB.  This
# bumps ext_vacuum_statistics' interrupts_count for the database.
start_interrupt_loop() {
    ( while sleep 4; do
          psql_admin -c "SELECT pg_cancel_backend(pid) \
                         FROM pg_stat_activity \
                         WHERE backend_type='autovacuum worker' \
                           AND datname='$BENCH_DB' AND state='active'" >/dev/null || true
      done ) &
    INTERRUPT_PID=$!
}
stop_interrupt_loop() {
    if [[ -n "$INTERRUPT_PID" ]] && kill -0 "$INTERRUPT_PID" 2>/dev/null; then
        kill "$INTERRUPT_PID" 2>/dev/null || true
        wait "$INTERRUPT_PID" 2>/dev/null || true
    fi
    INTERRUPT_PID=""
}

# Burn XIDs in the background concurrent with the TPC-B workload, so
# pgbench_accounts.age climbs past vacuum_failsafe_age while the table
# also accumulates fresh dead tuples that the final VACUUM can scan.
start_xid_burner() {
    pgbench -n -c "$XID_CLIENTS" -T "$1" \
            -f "$HERE/patterns/pattern_xid_burn.sql" \
            "$BENCH_DB" >/dev/null 2>&1 &
    BURNER_PID=$!
}
stop_xid_burner() {
    if [[ -n "$BURNER_PID" ]] && kill -0 "$BURNER_PID" 2>/dev/null; then
        wait "$BURNER_PID" 2>/dev/null || true
    fi
    BURNER_PID=""
}

trap 'stop_sampler; stop_interrupt_loop; stop_xid_burner' EXIT INT TERM

# ---------------------------------------------------------------------------
# Cluster GUC reset (clean slate before each phase)
# ---------------------------------------------------------------------------
reset_all_vacuum_state() {
    psql_admin <<'SQL'
ALTER SYSTEM RESET autovacuum;
ALTER SYSTEM RESET autovacuum_naptime;
ALTER SYSTEM RESET autovacuum_vacuum_scale_factor;
ALTER SYSTEM RESET autovacuum_vacuum_threshold;
ALTER SYSTEM RESET autovacuum_vacuum_cost_delay;
ALTER SYSTEM RESET autovacuum_vacuum_cost_limit;
ALTER SYSTEM RESET autovacuum_freeze_max_age;
ALTER SYSTEM RESET vacuum_failsafe_age;
ALTER SYSTEM RESET vacuum_freeze_min_age;
ALTER SYSTEM RESET maintenance_work_mem;
SELECT pg_reload_conf();
SQL
    psql_bench <<'SQL' >/dev/null
ALTER TABLE pgbench_accounts RESET (autovacuum_enabled);
ALTER TABLE pgbench_history  RESET (autovacuum_enabled);
SQL
}

# ---------------------------------------------------------------------------
# Phase configuration
# ---------------------------------------------------------------------------
phase_description() {
    case "$1" in
        mwm-small)   printf 'maintenance_work_mem=64kB';;
        passive)     printf 'autovacuum naptime=120s + threshold=1M + cost_delay=100ms';;
        interrupted) printf 'autovacuum workers cancelled by pg_cancel_backend';;
        wraparound)  printf 'autovacuum_freeze_max_age=100k + XID burner concurrent';;
    esac
}

apply_phase_gucs() {
    local phase="$1"
    reset_all_vacuum_state

    if [[ "$MODE" == "fixed" ]]; then
        # FIXED is just defaults — no special tuning.  The contrast vs
        # BROKEN is the whole point.
        return
    fi

    case "$phase" in
        mwm-small)
            psql_admin <<'SQL'
ALTER SYSTEM SET maintenance_work_mem            = '64kB';
ALTER SYSTEM SET autovacuum_naptime              = '5s';
ALTER SYSTEM SET autovacuum_vacuum_scale_factor  = 0.0;
ALTER SYSTEM SET autovacuum_vacuum_threshold     = 5000;
SELECT pg_reload_conf();
SQL
            ;;
        passive)
            psql_admin <<'SQL'
ALTER SYSTEM SET autovacuum_naptime              = '120s';
ALTER SYSTEM SET autovacuum_vacuum_scale_factor  = 0.5;
ALTER SYSTEM SET autovacuum_vacuum_threshold     = 1000000;
ALTER SYSTEM SET autovacuum_vacuum_cost_delay    = '100ms';
ALTER SYSTEM SET autovacuum_vacuum_cost_limit    = 10;
SELECT pg_reload_conf();
SQL
            ;;
        interrupted)
            # Aggressive autovacuum so it fires often → more cancellations.
            psql_admin <<'SQL'
ALTER SYSTEM SET autovacuum_naptime              = '5s';
ALTER SYSTEM SET autovacuum_vacuum_scale_factor  = 0.0;
ALTER SYSTEM SET autovacuum_vacuum_threshold     = 5000;
SELECT pg_reload_conf();
SQL
            ;;
        wraparound)
            # Disable autovacuum on pgbench_accounts so age accumulates;
            # the final manual VACUUM will see a huge age and trip the
            # failsafe path.
            psql_admin <<'SQL'
ALTER SYSTEM SET autovacuum_freeze_max_age       = 100000;
ALTER SYSTEM SET vacuum_failsafe_age             = 200000;
ALTER SYSTEM SET vacuum_freeze_min_age           = 0;
ALTER SYSTEM SET autovacuum_naptime              = '5s';
SELECT pg_reload_conf();
SQL
            psql_bench -c "ALTER TABLE pgbench_accounts SET (autovacuum_enabled = false);" \
                >/dev/null
            ;;
    esac
}

# ---------------------------------------------------------------------------
# Pre-phase: re-init pgbench tables to a clean baseline
# ---------------------------------------------------------------------------
reseed_pgbench_tables() {
    log "  re-seeding pgbench tables (TRUNCATE + pgbench -i -I g, scale=$SCALE)"
    # Use TRUNCATE + only the data-generation step of pgbench -i.  The
    # earlier "-I dtg" form did DROP + CREATE TABLE, which assigns a
    # fresh OID to each pgbench table on every phase — sample_stat_vacuum_*
    # then tracks each generation as a separate relation, and any aggregation
    # across phases mixes counters from multiple relids, producing huge
    # negative window deltas.  TRUNCATE preserves the OID, so per-phase
    # comparisons stay consistent.
    psql_bench <<'SQL' >/dev/null
TRUNCATE pgbench_accounts, pgbench_branches, pgbench_history, pgbench_tellers;
SQL
    if ! pgbench -i -I g -s "$SCALE" -q "$BENCH_DB" 2>&1 | sed 's/^/    /'; then
        # First-time run: tables don't exist yet → fall back to full init.
        log "  (first-time run: doing full pgbench -i)"
        pgbench -i -s "$SCALE" -q "$BENCH_DB" 2>&1 | sed 's/^/    /'
    fi
    psql_bench -c "VACUUM (FREEZE, ANALYZE) pgbench_accounts, pgbench_branches, pgbench_history, pgbench_tellers;" \
        >/dev/null
}

# ---------------------------------------------------------------------------
# Phase driver
# ---------------------------------------------------------------------------
run_phase() {
    local phase="$1"

    if [[ "$phase" == "wraparound" && "$RUN_WRAPAROUND" != "1" ]]; then
        log "Skipping $phase (set RUN_WRAPAROUND=1 to run)"
        return 0
    fi

    log "Phase $phase ($(phase_description "$phase"))"
    reseed_pgbench_tables
    apply_phase_gucs "$phase"

    take_sample
    local s_start started ended s_end
    s_start=$(current_sample_id)
    started=$(date -u +"%Y-%m-%d %H:%M:%S+00")

    start_sampler "$SAMPLE_PERIOD"

    if [[ "$phase" == "interrupted" && "$MODE" == "broken" ]]; then
        start_interrupt_loop
    fi
    if [[ "$phase" == "wraparound" && "$MODE" == "broken" ]]; then
        start_xid_burner "$PHASE_SECONDS"
    fi

    # Custom multi-user pattern (UPDATE accounts + UPDATE tellers + INSERT
    # history).  Skips the pgbench_branches UPDATE that turns standard
    # TPC-B into a 50-row hot-spot benchmark on a debug build.
    pgbench -n -c "$CLIENTS" -T "$PHASE_SECONDS" -P 30 \
            -f "$HERE/patterns/pattern_multi_user.sql" \
            -D scale="$SCALE" \
            "$BENCH_DB" 2>&1 \
        | sed "s/^/[$phase] /"

    stop_interrupt_loop
    stop_xid_burner

    log "  forcing explicit VACUUM (VERBOSE) on every pgbench table"
    for tbl in pgbench_accounts pgbench_branches pgbench_history pgbench_tellers; do
        psql_bench -c "VACUUM (VERBOSE) $tbl;" \
            > "$OUT_DIR/vac_${MODE}_${phase}_${tbl}.log" 2>&1 || true
    done

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
# Setup: extensions + ensure phases table exists (reuse delete-cases schema)
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

IFS=',' read -r -a PHASE_LIST <<< "$PHASES"
for p in "${PHASE_LIST[@]}"; do
    run_phase "$p"
done

reset_all_vacuum_state

log "Running detectors and refreshing window views"
psql_profile -f "$HERE/../detect_vacuum_misconfig.sql" >/dev/null
psql_profile -f "$HERE/detect_delete_cases.sql" >/dev/null

log "Done. Phases recorded in evs_delete_phases for mode=$MODE."

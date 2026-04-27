#!/usr/bin/env bash
#
# Run pgbench under several distinct update patterns, take periodic
# pg_profile snapshots, and dump the labeled vacuum-stats windows so
# plot_update_patterns.py can chart the dynamics.
#
# The four patterns (definitions in patterns/*.sql):
#   random       — uniform UPDATEs across pgbench_accounts
#   middle       — hotspot in a 10k-row window in the middle
#   boundaries   — strided UPDATEs hitting roughly one tuple per page
#   chunked      — rolling 100-row contiguous sweep via a CYCLE seq
#
# Output (under $OUT_DIR, default ./out_patterns):
#   pattern_phases.csv
#   pattern_window_labeled.csv
#   pattern_summary.csv
#
# Environment overrides:
#   PG_BASE_CONN    keyword DSN minus dbname (default: 'host=/tmp port=5499')
#   ADMIN_DB        database for ALTER SYSTEM (default: postgres)
#   BENCH_DB        bench database (default: pgbench_evs)
#   PROFILE_DB      pg_profile database (default: $BENCH_DB)
#   SCALE           pgbench scale (default: 50)
#   PATTERN_SECONDS seconds per pattern (default: 60)
#   SAMPLE_PERIOD   seconds between pg_profile samples (default: 15)
#   CLIENTS         pgbench -c (default: 8)
#   PATTERNS        comma-separated subset to run
#                   (default: random,middle,boundaries,chunked)
#   OUT_DIR         output dir (default: ./out_patterns)

set -euo pipefail

PG_BASE_CONN=${PG_BASE_CONN:-'host=/tmp port=5499'}
ADMIN_DB=${ADMIN_DB:-postgres}
BENCH_DB=${BENCH_DB:-pgbench_evs}
PROFILE_DB=${PROFILE_DB:-$BENCH_DB}
SCALE=${SCALE:-50}
PATTERN_SECONDS=${PATTERN_SECONDS:-60}
SAMPLE_PERIOD=${SAMPLE_PERIOD:-15}
CLIENTS=${CLIENTS:-8}
PATTERNS=${PATTERNS:-random,middle,boundaries,chunked}
OUT_DIR=${OUT_DIR:-$(pwd)/out_patterns}

HERE=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$OUT_DIR"

admin_conn()   { printf '%s dbname=%s' "$PG_BASE_CONN" "$ADMIN_DB"; }
bench_conn()   { printf '%s dbname=%s' "$PG_BASE_CONN" "$BENCH_DB"; }
profile_conn() { printf '%s dbname=%s' "$PG_BASE_CONN" "$PROFILE_DB"; }
psql_admin()   { psql "$(admin_conn)"   -v ON_ERROR_STOP=1 -At "$@"; }
psql_bench()   { psql "$(bench_conn)"   -v ON_ERROR_STOP=1 -At "$@"; }
psql_profile() { psql "$(profile_conn)" -v ON_ERROR_STOP=1 -At "$@"; }

# pgbench takes a positional dbname; export host/port for it.
for kv in $PG_BASE_CONN; do
    case "$kv" in
        host=*)   export PGHOST="${kv#host=}";;
        port=*)   export PGPORT="${kv#port=}";;
        user=*)   export PGUSER="${kv#user=}";;
        password=*) export PGPASSWORD="${kv#password=}";;
    esac
done
log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

take_sample() {
    psql_profile -c "SELECT * FROM take_sample();" >/dev/null
}

# ---------------------------------------------------------------------------
# Prereqs (mirrors run_cases.sh but is independent)
# ---------------------------------------------------------------------------
log "Setting up databases & extensions"
psql_admin <<SQL
SELECT 'CREATE DATABASE $BENCH_DB'
WHERE  NOT EXISTS (SELECT 1 FROM pg_database WHERE datname = '$BENCH_DB')\gexec
SQL
psql_bench <<'SQL'
CREATE EXTENSION IF NOT EXISTS dblink;
-- pg_profile 4.11 supports pg_stat_statements up to 1.12.
CREATE EXTENSION IF NOT EXISTS pg_stat_statements VERSION '1.12';
CREATE EXTENSION IF NOT EXISTS ext_vacuum_statistics;
CREATE EXTENSION IF NOT EXISTS pg_profile;
SQL

# Ensure pgbench_accounts exists at the requested scale.  pgbench -i
# is idempotent in the sense that it drops and recreates the tables.
if [[ "$(psql_bench -c \
        "SELECT count(*) FROM information_schema.tables \
         WHERE table_name='pgbench_accounts'")" != "1" ]]; then
    log "Initializing pgbench (scale=$SCALE)"
    pgbench -i -s "$SCALE" -q "$BENCH_DB"
fi

# Phase-tracking table on the pg_profile side.  Recreated each run so
# stale phases never cross-contaminate the labeling.
psql_profile <<'SQL'
DROP TABLE IF EXISTS evs_pattern_phases;
CREATE UNLOGGED TABLE evs_pattern_phases (
    phase_id     serial primary key,
    pattern      text       NOT NULL,
    started_at   timestamptz NOT NULL,
    ended_at     timestamptz NOT NULL,
    sample_at_start integer,
    sample_at_end   integer
);
SQL

# Sequence used by the chunked pattern (CYCLE so it never runs out).
MAX_AID=$(( SCALE * 100000 ))
MAX_CHUNKS=$(( MAX_AID / 100 ))
psql_bench <<SQL
DROP SEQUENCE IF EXISTS evs_chunk_seq;
CREATE SEQUENCE evs_chunk_seq CYCLE
    START 1 MINVALUE 1 MAXVALUE $MAX_CHUNKS;
SQL

# Reset GUCs so prior demo runs don't leak in.
psql_admin <<'SQL'
ALTER SYSTEM RESET autovacuum_vacuum_cost_delay;
ALTER SYSTEM RESET autovacuum_vacuum_cost_limit;
ALTER SYSTEM RESET autovacuum_naptime;
ALTER SYSTEM RESET autovacuum_vacuum_scale_factor;
ALTER SYSTEM RESET autovacuum_vacuum_threshold;
ALTER SYSTEM RESET maintenance_work_mem;
ALTER SYSTEM RESET autovacuum;
SELECT pg_reload_conf();
SQL

# Mildly aggressive thresholds so vacuum actually fires several times
# per pattern.  Different from the broken settings in run_cases.sh.
psql_admin <<'SQL'
ALTER SYSTEM SET autovacuum_naptime           = '5s';
ALTER SYSTEM SET autovacuum_vacuum_scale_factor = 0.0;
ALTER SYSTEM SET autovacuum_vacuum_threshold    = 5000;
SELECT pg_reload_conf();
SQL

# ---------------------------------------------------------------------------
# Periodic-sampler helper (background loop, one process)
# ---------------------------------------------------------------------------
SAMPLER_PID=""

start_sampler() {
    local total="$1" period="$2"
    (
        local n=$(( total / period ))
        for ((i=0; i<n; i++)); do
            sleep "$period"
            psql_profile -c "SELECT * FROM take_sample();" >/dev/null \
                || true
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

# ---------------------------------------------------------------------------
# Run one pattern
# ---------------------------------------------------------------------------
run_pattern() {
    local name="$1"
    local script="$HERE/patterns/pattern_${name}.sql"
    if [[ ! -f "$script" ]]; then
        log "Skipping unknown pattern: $name"
        return 0
    fi

    log "Pattern '$name' starting (${PATTERN_SECONDS}s, ${CLIENTS} clients)"

    # Clean slate so vacuum stats accumulated in this phase reflect
    # *this* pattern (a final VACUUM also helps).
    psql_bench -c "VACUUM (FREEZE) pgbench_accounts;" >/dev/null
    take_sample
    local s_start
    s_start=$(psql_profile -c \
        "SELECT max(sample_id) FROM samples \
         WHERE server_id = (SELECT server_id FROM servers WHERE server_name='local')")

    local started ended
    started=$(date -u +"%Y-%m-%d %H:%M:%S+00")

    start_sampler "$PATTERN_SECONDS" "$SAMPLE_PERIOD"
    pgbench -n -c "$CLIENTS" -T "$PATTERN_SECONDS" -P 30 \
            -f "$script" "$BENCH_DB" \
            -D scale="$SCALE" 2>&1 | sed "s/^/[$name] /"
    stop_sampler

    take_sample
    local s_end
    s_end=$(psql_profile -c \
        "SELECT max(sample_id) FROM samples \
         WHERE server_id = (SELECT server_id FROM servers WHERE server_name='local')")
    ended=$(date -u +"%Y-%m-%d %H:%M:%S+00")

    psql_profile -v ON_ERROR_STOP=1 <<SQL
INSERT INTO evs_pattern_phases (pattern, started_at, ended_at,
                                sample_at_start, sample_at_end)
VALUES ('$name', '$started'::timestamptz, '$ended'::timestamptz,
        $s_start, $s_end);
SQL

    log "Pattern '$name' done (samples $s_start..$s_end)"
}

# ---------------------------------------------------------------------------
# Drive all patterns
# ---------------------------------------------------------------------------
IFS=',' read -r -a PATTERN_LIST <<< "$PATTERNS"
for p in "${PATTERN_LIST[@]}"; do
    run_pattern "$p"
done

# ---------------------------------------------------------------------------
# Aggregate + dump
# ---------------------------------------------------------------------------
log "Aggregating windows by pattern"
psql_profile -v detect_path="$HERE/detect_vacuum_misconfig.sql" \
             -f "$HERE/aggregate_patterns.sql"

log "Exporting CSVs to $OUT_DIR"
psql_profile -c "\COPY (SELECT * FROM evs_pattern_phases ORDER BY phase_id) \
                 TO '$OUT_DIR/pattern_phases.csv' WITH CSV HEADER"
psql_profile -c "\COPY (SELECT * FROM evs_window_labeled ORDER BY t_to, relname) \
                 TO '$OUT_DIR/pattern_window_labeled.csv' WITH CSV HEADER"
psql_profile -c "\COPY (SELECT * FROM evs_pattern_summary) \
                 TO '$OUT_DIR/pattern_summary.csv' WITH CSV HEADER"

log "Done. Outputs in $OUT_DIR"

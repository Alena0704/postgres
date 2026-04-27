#!/usr/bin/env bash
#
# Reproduce three vacuum misconfigurations on a pgbench database,
# then take pg_profile snapshots so detect_vacuum_misconfig.sql
# can flag them.
#
# Layout produced under $OUT_DIR (default ./out):
#   out/case1_throttled.csv      (delay-share per window)
#   out/case2_mwm_small.csv      (index passes per vacuum)
#   out/case3_lazy_av.csv        (lazy autovacuum bursts)
#   out/all_window.csv           (the full evs_window relation)
#
# Environment overrides:
#   PG_BASE_CONN   keyword-style DSN minus dbname (default: 'host=/tmp port=5499')
#   ADMIN_DB       database for ALTER SYSTEM/CREATE DATABASE (default: postgres)
#   BENCH_DB       database to run pgbench in (default: pgbench_evs)
#   PROFILE_DB     database hosting pg_profile (default: $BENCH_DB)
#   SCALE          pgbench -i scale (default: 50)
#   PHASE_SECONDS  duration of each pgbench phase (default: 90)
#   CLIENTS        pgbench -c (default: 16)
#   OUT_DIR        output directory (default: ./out)

set -euo pipefail

PG_BASE_CONN=${PG_BASE_CONN:-'host=/tmp port=5499'}
ADMIN_DB=${ADMIN_DB:-postgres}
BENCH_DB=${BENCH_DB:-pgbench_evs}
PROFILE_DB=${PROFILE_DB:-$BENCH_DB}
SCALE=${SCALE:-50}
PHASE_SECONDS=${PHASE_SECONDS:-90}
CLIENTS=${CLIENTS:-16}
OUT_DIR=${OUT_DIR:-$(pwd)/out}

mkdir -p "$OUT_DIR"

admin_conn()   { printf '%s dbname=%s' "$PG_BASE_CONN" "$ADMIN_DB"; }
bench_conn()   { printf '%s dbname=%s' "$PG_BASE_CONN" "$BENCH_DB"; }
profile_conn() { printf '%s dbname=%s' "$PG_BASE_CONN" "$PROFILE_DB"; }

# pgbench takes a positional dbname rather than a conninfo, so export
# host/port/user from PG_BASE_CONN as env vars for it.
for kv in $PG_BASE_CONN; do
    case "$kv" in
        host=*)   export PGHOST="${kv#host=}";;
        port=*)   export PGPORT="${kv#port=}";;
        user=*)   export PGUSER="${kv#user=}";;
        password=*) export PGPASSWORD="${kv#password=}";;
    esac
done

# ---------------------------------------------------------------------------
# Tiny helpers
# ---------------------------------------------------------------------------
psql_admin()   { psql "$(admin_conn)"   -v ON_ERROR_STOP=1 -At "$@"; }
psql_bench()   { psql "$(bench_conn)"   -v ON_ERROR_STOP=1 -At "$@"; }
psql_profile() { psql "$(profile_conn)" -v ON_ERROR_STOP=1 -At "$@"; }

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

take_sample() {
    psql_profile -c "SELECT * FROM take_sample();" >/dev/null
    log "  sample taken"
}

reset_gucs_to_sane_defaults() {
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
}

# ---------------------------------------------------------------------------
# 0. Prerequisites
# ---------------------------------------------------------------------------
log "Setting up databases"
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

# Make sure ext_vacuum_statistics actually tracks pgbench_accounts.
# All categories on by default; override here if a custom track-list is set.
psql_bench <<'SQL'
ALTER SYSTEM RESET ext_vacuum_statistics.track_relations;
ALTER SYSTEM RESET ext_vacuum_statistics.track_tables;
SELECT pg_reload_conf();
SQL

log "Initializing pgbench (scale=$SCALE)"
pgbench -i -s "$SCALE" -q "$BENCH_DB"

# Baseline sample
log "Baseline pg_profile sample"
take_sample

# ===========================================================================
# CASE 1 — autovacuum throttled by cost-delay
# ===========================================================================
log "Case 1: throttling autovacuum (cost_delay=100ms, cost_limit=10)"
reset_gucs_to_sane_defaults
psql_admin <<'SQL'
-- PG18 caps autovacuum_vacuum_cost_delay at 100ms; combine with a
-- crippled cost_limit so each cycle still spends most of its time
-- sleeping.
ALTER SYSTEM SET autovacuum_vacuum_cost_delay  = '100ms';   -- max
ALTER SYSTEM SET autovacuum_vacuum_cost_limit  = 10;        -- default 200
ALTER SYSTEM SET autovacuum_naptime            = '5s';
ALTER SYSTEM SET autovacuum_vacuum_scale_factor = 0.0;
ALTER SYSTEM SET autovacuum_vacuum_threshold    = 1000;     -- fire often
SELECT pg_reload_conf();
SQL

take_sample
pgbench -n -c "$CLIENTS" -T "$PHASE_SECONDS" -P 30 \
        -f <(cat <<'EOF'
\set aid random(1, 100000 * :scale)
UPDATE pgbench_accounts SET abalance = abalance + 1 WHERE aid = :aid;
EOF
) "$BENCH_DB"
take_sample

# ===========================================================================
# CASE 2 — maintenance_work_mem too small → multiple index passes
# ===========================================================================
log "Case 2: maintenance_work_mem=64kB"
reset_gucs_to_sane_defaults
psql_admin <<'SQL'
ALTER SYSTEM SET maintenance_work_mem        = '64kB';     -- minimum
ALTER SYSTEM SET autovacuum_naptime          = '5s';
ALTER SYSTEM SET autovacuum_vacuum_scale_factor = 0.0;
ALTER SYSTEM SET autovacuum_vacuum_threshold    = 50000;
SELECT pg_reload_conf();
SQL

take_sample
pgbench -n -c "$CLIENTS" -T "$PHASE_SECONDS" -P 30 \
        -f <(cat <<'EOF'
\set aid random(1, 100000 * :scale)
UPDATE pgbench_accounts SET abalance = abalance + 1 WHERE aid = :aid;
DELETE FROM pgbench_history WHERE random() < 0.001;
EOF
) "$BENCH_DB"
# Help the case fire deterministically: explicit VACUUM with the same low mwm
psql_bench -c "VACUUM (VERBOSE) pgbench_accounts;" >/dev/null
take_sample

# ===========================================================================
# CASE 3 — vacuum thrashing: autovacuum_vacuum_scale_factor / threshold so
# low that autovacuum re-scans the table for trivial dead-tuple harvests.
# Only vacuum GUCs are touched.
# ===========================================================================
log "Case 3: thrashing autovacuum (naptime=1s, scale_factor=0, threshold=10)"
reset_gucs_to_sane_defaults
psql_admin <<'SQL'
ALTER SYSTEM SET autovacuum_naptime              = '1s';
ALTER SYSTEM SET autovacuum_vacuum_scale_factor  = 0.0;
ALTER SYSTEM SET autovacuum_vacuum_threshold     = 10;
SELECT pg_reload_conf();
SQL

# Rate-limit the workload (1 client, 100 tps) so vacuum fires far more
# often than the workload actually needs.  This is what "thrashing"
# means: many vacuum cycles on a table with very little real bloat.
take_sample
pgbench -n -c 1 -R 100 -T "$PHASE_SECONDS" -P 30 \
        -f <(cat <<'EOF'
\set aid random(1, 100000 * :scale)
UPDATE pgbench_accounts SET abalance = abalance + 1 WHERE aid = :aid;
EOF
) "$BENCH_DB"
take_sample

reset_gucs_to_sane_defaults

# ===========================================================================
# Run the detector and dump CSVs
# ===========================================================================
log "Running detector"
psql_profile -f "$(dirname "$0")/detect_vacuum_misconfig.sql"

log "Exporting CSVs to $OUT_DIR"
psql_profile -c "\COPY (SELECT * FROM evs_window             ORDER BY t_to, relname) TO '$OUT_DIR/all_window.csv'         WITH CSV HEADER"
psql_profile -c "\COPY (SELECT * FROM evs_workload_profile   ORDER BY t_to, relname) TO '$OUT_DIR/workload_profile.csv'   WITH CSV HEADER"
psql_profile -c "\COPY (SELECT * FROM evs_case1_throttled    ORDER BY t_to, relname) TO '$OUT_DIR/case1_throttled.csv'    WITH CSV HEADER"
psql_profile -c "\COPY (SELECT * FROM evs_case2_mwm_small    ORDER BY t_to, relname) TO '$OUT_DIR/case2_mwm_small.csv'    WITH CSV HEADER"
psql_profile -c "\COPY (SELECT * FROM evs_case3_vac_thrash   ORDER BY t_to, relname) TO '$OUT_DIR/case3_vac_thrash.csv'   WITH CSV HEADER"

log "Done. Outputs in $OUT_DIR"

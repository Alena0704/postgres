#!/usr/bin/env bash
#
# Re-run the three pgbench phases from run_cases.sh, but with the
# *fixed* parameter values.  Outputs are written to a separate
# directory so plot_compare.py can put broken vs fixed side by side.
#
# Same env knobs as run_cases.sh.  OUT_DIR defaults to ./out_fix.

set -euo pipefail

PG_BASE_CONN=${PG_BASE_CONN:-'host=/tmp port=5499'}
ADMIN_DB=${ADMIN_DB:-postgres}
BENCH_DB=${BENCH_DB:-pgbench_evs}
PROFILE_DB=${PROFILE_DB:-$BENCH_DB}
SCALE=${SCALE:-50}
PHASE_SECONDS=${PHASE_SECONDS:-90}
CLIENTS=${CLIENTS:-16}
OUT_DIR=${OUT_DIR:-$(pwd)/out_fix}

mkdir -p "$OUT_DIR"

admin_conn()   { printf '%s dbname=%s' "$PG_BASE_CONN" "$ADMIN_DB"; }
bench_conn()   { printf '%s dbname=%s' "$PG_BASE_CONN" "$BENCH_DB"; }
profile_conn() { printf '%s dbname=%s' "$PG_BASE_CONN" "$PROFILE_DB"; }
psql_admin()   { psql "$(admin_conn)"   -v ON_ERROR_STOP=1 -At "$@"; }
psql_bench()   { psql "$(bench_conn)"   -v ON_ERROR_STOP=1 -At "$@"; }
psql_profile() { psql "$(profile_conn)" -v ON_ERROR_STOP=1 -At "$@"; }

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
    log "  sample taken"
}

reset_gucs() {
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

log "Setup"
psql_admin -c "DROP DATABASE IF EXISTS $BENCH_DB WITH (FORCE);"
psql_admin <<SQL
SELECT 'CREATE DATABASE $BENCH_DB'
WHERE  NOT EXISTS (SELECT 1 FROM pg_database WHERE datname = '$BENCH_DB')\gexec
SQL
psql_bench <<'SQL'
CREATE EXTENSION IF NOT EXISTS dblink;
CREATE EXTENSION IF NOT EXISTS pg_stat_statements VERSION '1.12';
CREATE EXTENSION IF NOT EXISTS ext_vacuum_statistics;
CREATE EXTENSION IF NOT EXISTS pg_profile;
SQL

log "Initializing pgbench (scale=$SCALE)"
pgbench -i -s "$SCALE" -q "$BENCH_DB"
take_sample

# ===========================================================================
# CASE 1 — fixed: defaults give vacuum room to actually work
# ===========================================================================
log "Case 1 fix: cost_delay=2ms, cost_limit=200 (defaults)"
reset_gucs
psql_admin <<'SQL'
ALTER SYSTEM SET autovacuum_vacuum_cost_delay  = '2ms';
ALTER SYSTEM SET autovacuum_vacuum_cost_limit  = 200;
ALTER SYSTEM SET autovacuum_naptime            = '5s';
ALTER SYSTEM SET autovacuum_vacuum_scale_factor = 0.0;
ALTER SYSTEM SET autovacuum_vacuum_threshold    = 1000;
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
# CASE 2 — fixed: maintenance_work_mem big enough for the dead-TID array
# ===========================================================================
log "Case 2 fix: maintenance_work_mem=256MB"
reset_gucs
psql_admin <<'SQL'
ALTER SYSTEM SET maintenance_work_mem        = '256MB';
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
psql_bench -c "VACUUM (VERBOSE) pgbench_accounts;" >/dev/null
take_sample

# ===========================================================================
# CASE 3 fix — scale_factor / threshold raised so vacuum only fires on a
# meaningful dead-tuple harvest.
# ===========================================================================
log "Case 3 fix: scale_factor=0.05, threshold=50, naptime=1min (defaults)"
reset_gucs
psql_admin <<'SQL'
ALTER SYSTEM SET autovacuum_naptime             = '1min';
ALTER SYSTEM SET autovacuum_vacuum_scale_factor = 0.05;
ALTER SYSTEM SET autovacuum_vacuum_threshold    = 50;
SELECT pg_reload_conf();
SQL

# Same rate-limited workload as the broken phase, so the comparison is
# fair: with sane settings vacuum should NOT fire at 100 tps for 45s.
take_sample
pgbench -n -c 1 -R 100 -T "$PHASE_SECONDS" -P 30 \
        -f <(cat <<'EOF'
\set aid random(1, 100000 * :scale)
UPDATE pgbench_accounts SET abalance = abalance + 1 WHERE aid = :aid;
EOF
) "$BENCH_DB"
take_sample

reset_gucs

log "Running detector"
psql_profile -f "$(dirname "$0")/detect_vacuum_misconfig.sql"

log "Exporting CSVs to $OUT_DIR"
psql_profile -c "\COPY (SELECT * FROM evs_window             ORDER BY t_to, relname) TO '$OUT_DIR/all_window.csv'         WITH CSV HEADER"
psql_profile -c "\COPY (SELECT * FROM evs_workload_profile   ORDER BY t_to, relname) TO '$OUT_DIR/workload_profile.csv'   WITH CSV HEADER"
psql_profile -c "\COPY (SELECT * FROM evs_case1_throttled    ORDER BY t_to, relname) TO '$OUT_DIR/case1_throttled.csv'    WITH CSV HEADER"
psql_profile -c "\COPY (SELECT * FROM evs_case2_mwm_small    ORDER BY t_to, relname) TO '$OUT_DIR/case2_mwm_small.csv'    WITH CSV HEADER"
psql_profile -c "\COPY (SELECT * FROM evs_case3_vac_thrash   ORDER BY t_to, relname) TO '$OUT_DIR/case3_vac_thrash.csv'   WITH CSV HEADER"

log "Done. Outputs in $OUT_DIR"

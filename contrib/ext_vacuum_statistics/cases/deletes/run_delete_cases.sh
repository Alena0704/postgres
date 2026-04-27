#!/usr/bin/env bash
# Run the six DELETE-pattern + vacuum-misconfig phases in BROKEN mode.
# Phase F (wraparound) is gated behind RUN_WRAPAROUND=1 because it
# lowers autovacuum_freeze_max_age cluster-wide.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
MODE=broken OUT_DIR=${OUT_DIR:-$HERE/out_broken} \
    exec "$HERE/run_delete_phases.sh" "$@"

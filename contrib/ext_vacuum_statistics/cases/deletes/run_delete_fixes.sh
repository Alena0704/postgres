#!/usr/bin/env bash
# Run the same six phases in FIXED mode so plot_delete_cases.py can put
# broken vs fixed side by side.  Same env knobs as run_delete_cases.sh;
# OUT_DIR defaults to ./out_fixed.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
MODE=fixed OUT_DIR=${OUT_DIR:-$HERE/out_fixed} \
    exec "$HERE/run_delete_phases.sh" "$@"

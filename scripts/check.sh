#!/usr/bin/env bash
# One-command gate for zrpc. Run before merging any change.
#
#   scripts/check.sh            # build + unit/integration + perf smoke + stress
#   scripts/check.sh --full     # also run the stability soak
#   scripts/check.sh --analyze  # only re-render the latest report vs baseline
#
# Exit code is non-zero if any hard gate (unit/integration) fails, any perf or
# stress scenario misses its expectation, or a baseline regression is detected.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PY="${PYTHON:-python3}"
ZBENCH="tools/perf/zbench.py"
BASELINE="${ZBENCH_BASELINE:-tools/perf/baseline.json}"
QPS_TOL="${ZBENCH_QPS_TOL:-0.25}"
P99_TOL="${ZBENCH_P99_TOL:-0.75}"
MODE="smoke"

for arg in "$@"; do
  case "$arg" in
    --full) MODE="full" ;;
    --analyze) MODE="analyze" ;;
    *) echo "unknown option: $arg"; exit 2 ;;
  esac
done

if [ "$MODE" = "analyze" ]; then
  exec "$PY" "$ZBENCH" analyze ${BASELINE:+--baseline "$BASELINE"}
fi

echo "== unit + integration =="
"$PY" "$ZBENCH" unit || exit $?

echo "== perf smoke =="
"$PY" "$ZBENCH" perf --quick --no-build --baseline "$BASELINE" \
  --qps-tol "$QPS_TOL" --p99-tol "$P99_TOL" || exit $?

echo "== stress =="
"$PY" "$ZBENCH" stress --no-build --baseline "$BASELINE" || exit $?

if [ "$MODE" = "full" ]; then
  echo "== stability soak =="
  "$PY" "$ZBENCH" soak --no-build --duration "${SOAK_SECONDS:-600}" || exit $?
fi

echo "check: PASS"

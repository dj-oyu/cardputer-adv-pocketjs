#!/usr/bin/env bash
# Builds tools/test_stress_app.c: apps/stress/stress.js against the real
# QuickJS and pocket.kasane (WSL, from the repo root).
#   bash tools/build_stress_app_test.sh && /tmp/test-stress-app
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${OUT:-/tmp/test-stress-app} TEST_SOURCE=tools/test_stress_app.c \
  bash tools/build_kasane_test.sh

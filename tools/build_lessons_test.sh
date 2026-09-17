#!/usr/bin/env bash
# Builds tools/test_lessons.c: the tutorial's chapters and the Playground's
# template against the real QuickJS and pocket.kasane (WSL, from the repo root).
#   bash tools/build_lessons_test.sh && /tmp/test-lessons
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${OUT:-/tmp/test-lessons} TEST_SOURCE=tools/test_lessons.c \
  EXTRA_SOURCES=main/ui/lessons.c bash tools/build_kasane_test.sh

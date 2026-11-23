#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

bash ./build.sh "$@"
QT_QPA_PLATFORM=offscreen python3 tests/test_ipc.py

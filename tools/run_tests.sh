#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-build}"

cmake -S . -B "${BUILD_DIR}" -DBUILD_TESTING=ON
cmake --build "${BUILD_DIR}" -j
cmake --build "${BUILD_DIR}" --target test

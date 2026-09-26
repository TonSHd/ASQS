#!/usr/bin/env bash
# ASQS full test suite: C++ unit tests + Python end-to-end integration.
set -e
cd "$(dirname "$0")/.."

echo "=== build ==="
make asqsd
make build/asqs_tests

echo "=== C++ unit tests ==="
./build/asqs_tests

echo "=== end-to-end integration (stratum client mines real shares) ==="
python3 tests/test_integration.py

echo ""
echo "ALL TESTS PASSED"

#!/bin/bash
set -e
PG_COMMIT=${PG_COMMIT:-5713b43}
PREFIX=${PREFIX:-$HOME/pginstall}
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== Clone PostgreSQL ==="
git clone --depth 100 https://github.com/postgres/postgres.git /tmp/pg_build
cd /tmp/pg_build
git checkout $PG_COMMIT

echo "=== Apply required patch ==="
git apply "$REPO_ROOT/patches/required/0001-executor-batch-api.patch"

echo "=== Configure ==="
./configure --prefix=$PREFIX --enable-debug --enable-cassert --without-icu

echo "=== Build ==="
make -j$(nproc)
make install

echo "=== Done: $($PREFIX/bin/postgres --version) ==="

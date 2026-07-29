#!/bin/bash
set -e
PREFIX=${PREFIX:-$HOME/pginstall}
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cd "$REPO_ROOT/extension/xp_batch"
make clean PG_CONFIG=$PREFIX/bin/pg_config 2>/dev/null || true
make PG_CONFIG=$PREFIX/bin/pg_config
make install PG_CONFIG=$PREFIX/bin/pg_config
echo "=== Extension installed ==="

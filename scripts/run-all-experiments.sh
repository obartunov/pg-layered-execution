#!/bin/bash
set -e
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
for exp in 01-batch-representation 02-batch-joins 03-partition-layers; do
    echo "=== Experiment: $exp ==="
    if [ -x "$REPO_ROOT/benchmarks/$exp/run.sh" ]; then
        cd "$REPO_ROOT/benchmarks/$exp" && ./run.sh
    else
        echo "  (run.sh not yet implemented)"
    fi
done

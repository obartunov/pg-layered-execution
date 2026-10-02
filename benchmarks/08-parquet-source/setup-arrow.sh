#!/bin/bash
#
# Provision the Arrow/Parquet C++ distribution that xpb_parquet links against.
#
#   benchmarks/08-parquet-source/setup-arrow.sh [PREFIX]     # default /home/claude/arrow-cpp
#
# This repository does not vendor Arrow and does not reimplement Parquet. It
# needs a real Arrow/Parquet C++ install, and on this host the only obtainable
# one is the distribution shipped inside the pyarrow wheel: full headers plus
# libarrow/libparquet. There is no libparquet-dev package available here and no
# DuckDB binary.
#
# The wheel is a transport, not a dependency: xpb_parquet links against
# ARROW_HOME, and any Arrow C++ install of a compatible version works. Nothing
# in the module is pyarrow-specific. The extracted prefix is laid out the
# conventional way so an OS package can be substituted by pointing ARROW_HOME
# at /usr.
set -euo pipefail

PREFIX="${1:-/home/claude/arrow-cpp}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "provisioning Arrow/Parquet C++ into $PREFIX"
mkdir -p "$PREFIX"

pip3 download pyarrow --no-deps -d "$WORK" >/dev/null
( cd "$WORK" && unzip -q ./*.whl )

mkdir -p "$PREFIX/include" "$PREFIX/lib"
cp -r "$WORK"/pyarrow/include/arrow   "$PREFIX/include/"
cp -r "$WORK"/pyarrow/include/parquet "$PREFIX/include/"
cp "$WORK"/pyarrow/libarrow.so.*     "$PREFIX/lib/"
cp "$WORK"/pyarrow/libparquet.so.*   "$PREFIX/lib/"

# Link names for -lparquet / -larrow.
( cd "$PREFIX/lib"
  for base in libarrow libparquet; do
      real=$(ls -1 $base.so.* | head -1)
      ln -sfn "$real" "$base.so"
  done )

echo "arrow:   $(ls "$PREFIX/lib"/libarrow.so.* | head -1)"
echo "parquet: $(ls "$PREFIX/lib"/libparquet.so.* | head -1)"
echo
echo "build the module with:"
echo "    make -C extension/xpb_parquet ARROW_HOME=$PREFIX"

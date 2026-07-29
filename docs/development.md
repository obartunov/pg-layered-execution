# Development guide

## Prerequisites

- PostgreSQL 20devel source (commit `5713b43`)
- GCC with C99 support
- bison, flex, libreadline-dev (or `--without-readline`)

## Build PostgreSQL with batch API patch

```bash
git clone https://github.com/postgres/postgres.git pg20
cd pg20
git checkout 5713b43
git apply ../patches/required/0001-executor-batch-api.patch

./configure --prefix=$PREFIX \
  --enable-debug --enable-cassert --without-icu
make -j$(nproc)
make install
```

## Build extension

```bash
cd extension/xp_batch
make PG_CONFIG=$PREFIX/bin/pg_config
make install PG_CONFIG=$PREFIX/bin/pg_config
```

## Initialize test cluster

```bash
initdb -D $PGDATA --no-locale
pg_ctl -D $PGDATA start -l pg.log
createdb testdb
```

## Load extension and run smoke test

```sql
CREATE EXTENSION xp_batch;
LOAD 'xp_batch';

-- Generate and load test data
-- See benchmarks/common/gen_data.py
```

## Optional: QueryEnvironment patch

The `0002-queryenv-enr-hook.patch` is needed only for the TR extension
experiment. It requires manual porting of `postgres.c` insertion points
for PG 20devel (see patches/experimental/).

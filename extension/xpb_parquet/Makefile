# xpb_parquet — OPTIONAL source provider module.
#
# Deliberately a separate shared module, not part of xp_batch.so:
#
#   xp_batch.so  (shared_preload_libraries, no Arrow reference anywhere)
#        ^ registry symbol, resolved at module load
#   xpb_parquet.so  (LOAD on demand)
#        -> C++ shim
#        -> libparquet / libarrow
#
# If this module is absent, or if Arrow is absent, the cluster starts normally
# and every existing XPBatch path works. That property is the reason for the
# split and is asserted by test/optional_module.sh.
#
# xpb_register_source_provider() is left UNDEFINED in this .so on purpose. It
# resolves against the already-loaded xp_batch.so, because PostgreSQL loads
# modules with RTLD_NOW | RTLD_GLOBAL (dfmgr.c). That is the supported
# mechanism; there is no dlopen here and no path to Arrow is compiled in.
MODULE_big = xpb_parquet
OBJS = src/xpb_parquet_module.o

PG_CPPFLAGS = -I../xp_batch/src

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

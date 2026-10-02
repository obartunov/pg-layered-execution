# xpb_abi_probe — TEST module. Not part of the extension.
#
# Registers deliberately malformed XpbSourceProvider descriptors so that a
# stale-descriptor ABI mismatch between xp_batch.so and a provider .so is
# something test/provider_abi.sh can observe. Nothing in the extension
# references it; build and install it only to run that test.
#
# Like xpb_parquet, it leaves xpb_register_source_provider undefined and resolves
# it against the already-loaded xp_batch.so (RTLD_NOW | RTLD_GLOBAL in dfmgr.c).
MODULE_big = xpb_abi_probe
OBJS = src/xpb_abi_probe.o

PG_CPPFLAGS = -I../xp_batch/src -Isrc

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

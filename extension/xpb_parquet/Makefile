# xpb_parquet — OPTIONAL source provider module.
#
#   xp_batch.so  (shared_preload_libraries, no Arrow reference anywhere)
#        ^ xpb_register_source_provider, resolved at module load
#   xpb_parquet.so  (LOAD on demand)
#        -> xpb_parquet_shim.cpp  (the only C++ here)
#        -> libparquet / libarrow
#
# If this module or Arrow is absent the cluster starts normally and every
# existing XPBatch path works. test/optional_module.sh asserts that by moving
# this .so aside and restarting, rather than by reasoning about linkage.
#
# xpb_register_source_provider() is left UNDEFINED in this .so on purpose: it
# resolves against the already-loaded xp_batch.so because PostgreSQL loads
# modules with RTLD_NOW | RTLD_GLOBAL (dfmgr.c). No dlopen, no compiled-in path
# to Arrow beyond the rpath below.
#
# ARROW_HOME points at the Arrow/Parquet C++ distribution. Here it is the one
# provisioned by benchmarks/08-parquet-source/setup-arrow.sh. Any Arrow C++
# install works -- point ARROW_HOME at /usr for an OS package. Nothing in this
# module is pyarrow-specific; the wheel is only how Arrow is obtained on a host
# with no libparquet-dev.
ARROW_HOME ?= /home/claude/arrow-cpp

MODULE_big = xpb_parquet
OBJS = src/xpb_parquet_module.o src/xpb_parquet_shim.o

PG_CPPFLAGS = -I../xp_batch/src -Isrc

# The shim is C++ and must not see PostgreSQL headers; the module is C and must
# not see Arrow headers. Only the shim gets the Arrow include path.
SHLIB_LINK = -L$(ARROW_HOME)/lib -lparquet -larrow -lstdc++ \
             -Wl,-rpath,$(ARROW_HOME)/lib

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# PGXS has no C++ rule. One explicit rule, with the Arrow include path applied
# here and nowhere else.
src/xpb_parquet_shim.o: src/xpb_parquet_shim.cpp src/xpb_parquet_shim.h
	g++ -std=c++20 -O2 -fPIC -fvisibility=hidden -Wall \
	    -I$(ARROW_HOME)/include -Isrc \
	    -c -o $@ $<

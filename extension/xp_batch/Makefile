MODULE_big = xp_batch
OBJS = src/xp_batch.o src/xpb_groupagg2.o src/xpb_pageagg.o \
       src/xpb_typeops.o src/xpb_qual.o src/xpb_cost.o \
       src/xpb_summary.o src/xpb_coldnative.o \
       src/xpb_columnar_experiment.o \
       src/xpb_columnar_pipeline.o \
       src/xpb_projection.o \
       src/xpb_zlfs.o \
       src/xpb_src_zlfs.o src/xpb_src_heap.o \
       src/xpb_src_pgcolumnar.o \
       src/xpb_batch_groupagg.o \
       src/xpb_batch_hashjoin.o \
       src/xpb_batch_partition.o \
       src/xpb_contract_probe.o \
       src/xpb_typed_pipeline.o \
       src/xpb_v2_report.o \
       src/xpb_source_registry.o src/xpb_builtin_providers.o src/xpb_conformance.o

EXTENSION = xp_batch
DATA = sql/xp_batch--1.0.sql

PG_CPPFLAGS = -Isrc
PG_CFLAGS = -O2 -DXPB_DENSE_PROBE

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

#include "postgres.h"
bool xpb_coldnative_enabled = false;
int  xpb_coldnative_prefetch = 0;
bool xpb_coldnative_readstream = false;
bool xpb_groupagg_enabled = false;
bool xpb_native_agg_enabled = false;
int  xpb_native_batch_cap = 4096;
bool xpb_native_bypass_qual = false;
bool xpb_native_snap_any = false;

/* Stubs for missing modules */
#include "nodes/pathnodes.h"
void xpbn_register(void) {}
void xpba_register(void) {}
void xpbs_register(void) {}
void xpbn_agg_add_path(PlannerInfo *root, RelOptInfo *input_rel,
                        RelOptInfo *output_rel, void *extra) {}
void xpba_add_path(PlannerInfo *root, RelOptInfo *input_rel,
                    RelOptInfo *output_rel, void *extra) {}
void xpbs_add_path(PlannerInfo *root, RelOptInfo *input_rel,
                    RelOptInfo *output_rel) {}

/* More stubs */
void cnative_register(void) {}
void cnative_add_path(PlannerInfo *root, RelOptInfo *input_rel,
                      RelOptInfo *output_rel) {}
void xpga_add_path(PlannerInfo *root, RelOptInfo *input_rel,
                    RelOptInfo *output_rel, void *extra) {}
void xpga_register(void) {}

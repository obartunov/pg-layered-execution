#include "postgres.h"
#include "optimizer/optimizer.h"
#include "xpb_cost.h"

double xpb_cost_scan_page_factor  = 0.95;
double xpb_cost_scan_tuple_factor = 0.85;
double xpb_cost_group_factor      = 0.80;
double xpb_cost_output_factor     = 1.00;
double xpb_cost_qual_factor       = 0.90;

XpbFloorCost
xpb_groupagg_floor_cost(double relpages, double relrows,
                         double ngroups, int npreds)
{
    XpbFloorCost fc;
    fc.c1_scan   = relpages * seq_page_cost * xpb_cost_scan_page_factor
                 + relrows  * cpu_tuple_cost * xpb_cost_scan_tuple_factor;
    fc.c2_group  = relrows  * cpu_operator_cost * xpb_cost_group_factor;
    fc.c3_output = ngroups  * cpu_tuple_cost * xpb_cost_output_factor;
    fc.c4_qual   = npreds * relrows * cpu_operator_cost * xpb_cost_qual_factor;
    fc.startup   = fc.c1_scan;
    fc.total     = fc.c1_scan + fc.c2_group + fc.c3_output + fc.c4_qual;
    return fc;
}

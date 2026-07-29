/*
 * xpb_cost.h
 *
 * Cost floor factors for xp_batch execution provider.
 */

#ifndef XPB_COST_H
#define XPB_COST_H

#include "postgres.h"

extern double xpb_cost_scan_page_factor;
extern double xpb_cost_scan_tuple_factor;
extern double xpb_cost_group_factor;
extern double xpb_cost_output_factor;
extern double xpb_cost_qual_factor;

typedef struct XpbFloorCost
{
    double startup;
    double total;
    double c1_scan;
    double c2_group;
    double c3_output;
    double c4_qual;
} XpbFloorCost;

extern XpbFloorCost xpb_groupagg_floor_cost(double relpages, double relrows,
                                             double ngroups, int npreds);

#endif  /* XPB_COST_H */

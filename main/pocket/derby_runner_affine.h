#ifndef DERBY_RUNNER_AFFINE_H
#define DERBY_RUNNER_AFFINE_H

#include "ui/kasane/ksn_proc_points_dispatch.h"
#include "ui/kasane/ksn_procedural.h"

/* Experimental screen-space runner only. Input is the existing eight float
 * draw arguments; colors are the runner's three registration arguments.
 * false leaves the frame unchanged and requires the ordinary VM fallback.
 * Coordinates are integer local grid units, NOT Q14 coordinate values.
 * Caller selects PIE only after supplying a device-calibrated threshold. */
bool derby_runner_affine(const float input[8], const float colors[3],
                         const KsnProcPointsPolicy *policy,
                         ksn_proc_frame *out, KsnProcPointsDecision *decision);

#ifdef POCKET_DERBY_RUNNER_AFFINE
typedef struct {
    uint32_t attempts,accepted,fallback,scalar_batches,pie_batches;
} DerbyRunnerAffineCounts;
/* Candidate-specific counts; independent of the frozen DERBY comparison ABI.
 * Owner task only. Reading optionally resets; no timing or hardware access. */
void pocket_proc_runner_affine_counts(DerbyRunnerAffineCounts *out,bool reset);
#endif

#endif

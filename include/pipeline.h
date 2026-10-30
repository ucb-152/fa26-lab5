#ifndef PIPELINE_H
#define PIPELINE_H

#include "edge_paths.h"

/* Edge-strength cutoffs and path-filtering/simplification parameters. */
struct PipelineOptions {
    uint16_t low_threshold;
    uint16_t high_threshold;
    double minimum_length;      /* Path length, in pixels. */
    double minimum_strength;    /* Mean original gradient magnitude along a path. */
    double tolerance;           /* Maximum simplification error, in pixels. */
};

/* Execution order and reporting names are shared by the pipeline and runner. */
enum PipelineStage {
    PIPELINE_STAGE_SETUP,
    PIPELINE_STAGE_BLUR,
    PIPELINE_STAGE_SOBEL,
    PIPELINE_STAGE_GRADIENT,
    PIPELINE_STAGE_SUPPRESSION,
    PIPELINE_STAGE_THRESHOLD,
    PIPELINE_STAGE_HYSTERESIS,
    PIPELINE_STAGE_GRAPH,
    PIPELINE_STAGE_TRACE,
    PIPELINE_STAGE_MEASURE,
    PIPELINE_STAGE_SIMPLIFY,
    PIPELINE_STAGE_CLEANUP,
    PIPELINE_STAGE_COUNT
};

extern const char *const pipeline_stage_names[PIPELINE_STAGE_COUNT];

/* Monotonic wall-clock seconds for contiguous intervals. Setup allocates scratch
 * images; cleanup releases them. Each processing stage includes its internal
 * allocations. Total spans setup through cleanup.
 * Validation, loading/saving, and releasing the final PipelineResult are excluded.
 * Published only after a successful invocation.
 */
struct PipelineTimings {
    double seconds[PIPELINE_STAGE_COUNT];
    double total;
};

/* Input contains width*height bytes. Output must initially be {0}.
 * Return nonzero on success. On any failure, release output with pipeline_result_free.
 */
int pipeline_run(const uint8_t *input, size_t width, size_t height,
            const struct PipelineOptions *options, struct PipelineResult *output,
            struct PipelineTimings *timings);

#endif

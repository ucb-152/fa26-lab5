#define _POSIX_C_SOURCE 200809L

#include "pipeline.h"
#include "canny.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* Record a monotonic boundary; execute on_error if the clock read fails.
 * stamps: struct timespec array; boundary: index; on_error: return/goto action.
 * Each boundary ends the preceding interval and starts the next one.
 */
#define RECORD_TIME(stamps, boundary, on_error) \
    do { \
        if (clock_gettime(CLOCK_MONOTONIC, &(stamps)[(boundary)]) != 0) { \
            on_error; \
        } \
    } while (0)

const char *const pipeline_stage_names[PIPELINE_STAGE_COUNT] = {
    [PIPELINE_STAGE_SETUP] = "setup",
    [PIPELINE_STAGE_BLUR] = "blur",
    [PIPELINE_STAGE_SOBEL] = "sobel",
    [PIPELINE_STAGE_GRADIENT] = "gradient",
    [PIPELINE_STAGE_SUPPRESSION] = "suppression",
    [PIPELINE_STAGE_THRESHOLD] = "threshold",
    [PIPELINE_STAGE_HYSTERESIS] = "hysteresis",
    [PIPELINE_STAGE_GRAPH] = "graph",
    [PIPELINE_STAGE_TRACE] = "trace",
    [PIPELINE_STAGE_MEASURE] = "measure",
    [PIPELINE_STAGE_SIMPLIFY] = "simplify",
    [PIPELINE_STAGE_CLEANUP] = "cleanup",
};

/* Convert a monotonic timestamp difference to seconds:
 * (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec)/1e9.
 * In: start, end (const struct timespec*), valid timestamps in temporal order.
 * Return: double elapsed seconds; no output parameters.
 */
static double elapsed(const struct timespec *start, const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

/* Execute all ten stages in order: blur -> Sobel -> magnitude/direction
 * -> suppression -> threshold -> hysteresis -> graph -> trace -> measure/filter
 * -> simplify. Canny strengths are retained for path scoring.
 *
 * In: input (const uint8_t*), width*height row-major grayscale pixels.
 * In: width, height (size_t), dimensions >= 5 with representable buffer sizes.
 * In: options (const struct PipelineOptions*), ordered uint16_t thresholds and finite,
 *     nonnegative double length/strength cutoffs and pixel tolerance; tolerance^2
 *     must also be finite.
 * In/out: output (struct PipelineResult*), initially zeroed; receives owned graph,
 *     original paths, retention flags, and simplified vertex IDs.
 * Out: timings (struct PipelineTimings*), stage and total double seconds, success only.
 * Return: int 1 on success, 0 on invalid input, allocation failure, or clock error.
 * Pointers must be non-NULL and buffers must not overlap. On failure, results may
 * be partial; always release output with pipeline_result_free. Temporary images are freed here.
 */
int pipeline_run(const uint8_t *input, size_t width, size_t height,
            const struct PipelineOptions *options, struct PipelineResult *output,
            struct PipelineTimings *timings)
{
    struct timespec stamp[PIPELINE_STAGE_COUNT + 1];
    uint16_t *magnitudes = NULL;
    uint8_t *blurred = NULL;
    int16_t *gradient_x = NULL;
    int16_t *gradient_y = NULL;
    uint8_t *direction = NULL;
    uint16_t *suppressed = NULL;
    uint8_t *state = NULL;
    uint8_t *edges = NULL;
    size_t pixel_count;
    int success = 0;

    /* Setup: validate inputs and allocate intermediate images. */
    if (input == NULL || options == NULL || output == NULL || timings == NULL ||
        width < 5 || height < 5 || width > SIZE_MAX / height ||
        options->low_threshold > options->high_threshold ||
        !isfinite(options->minimum_length) || options->minimum_length < 0.0 ||
        !isfinite(options->minimum_strength) || options->minimum_strength < 0.0 ||
        !isfinite(options->tolerance) || options->tolerance < 0.0 ||
        !isfinite(options->tolerance * options->tolerance)) {
        return 0;
    }
    pixel_count = width * height;
    if (pixel_count > SIZE_MAX / sizeof(uint16_t)) {
        return 0;
    }
    RECORD_TIME(stamp, PIPELINE_STAGE_SETUP, goto cleanup);
    magnitudes = malloc(pixel_count * sizeof(*magnitudes));
    blurred = malloc(pixel_count * sizeof(*blurred));
    gradient_x = malloc(pixel_count * sizeof(*gradient_x));
    gradient_y = malloc(pixel_count * sizeof(*gradient_y));
    direction = malloc(pixel_count * sizeof(*direction));
    suppressed = malloc(pixel_count * sizeof(*suppressed));
    state = malloc(pixel_count * sizeof(*state));
    edges = malloc(pixel_count * sizeof(*edges));
    if (magnitudes == NULL || blurred == NULL || gradient_x == NULL ||
        gradient_y == NULL || direction == NULL || suppressed == NULL ||
        state == NULL || edges == NULL) {
        goto cleanup;
    }

    /* Stage 1: Gaussian blur
     * Input: input (grayscale). Output: blurred (smoothed grayscale).
     */
    RECORD_TIME(stamp, PIPELINE_STAGE_BLUR, goto cleanup);
    canny_gaussian_blur(input, width, height, blurred);

    /* Stage 2: Sobel gradients
     * Input: blurred. Outputs: gradient_x and gradient_y (signed derivatives).
     */
    RECORD_TIME(stamp, PIPELINE_STAGE_SOBEL, goto cleanup);
    canny_sobel(blurred, width, height, gradient_x, gradient_y);

    /* Stage 3: Gradient magnitude and direction
     * Inputs: gradient_x, gradient_y.
     * Outputs: magnitudes (strength), direction (quantized orientation).
     */
    RECORD_TIME(stamp, PIPELINE_STAGE_GRADIENT, goto cleanup);
    canny_gradient_magnitude_and_direction(
        gradient_x, gradient_y, width, height, magnitudes, direction);

    /* Stage 4: Non-maximum suppression
     * Inputs: magnitudes, direction. Output: suppressed (thinned strengths).
     */
    RECORD_TIME(stamp, PIPELINE_STAGE_SUPPRESSION, goto cleanup);
    canny_non_maximum_suppression(
        magnitudes, direction, width, height, suppressed);

    /* Stage 5: Double threshold
     * Inputs: suppressed, low_threshold, high_threshold.
     * Output: state (absent, weak, or strong for each pixel).
     */
    RECORD_TIME(stamp, PIPELINE_STAGE_THRESHOLD, goto cleanup);
    canny_double_threshold(
        suppressed, width, height, options->low_threshold, options->high_threshold, state);

    /* Stage 6: Hysteresis
     * Input: state (updated in place as connected weak pixels are promoted).
     * Output: edges (binary 0/255 image).
     */
    RECORD_TIME(stamp, PIPELINE_STAGE_HYSTERESIS, goto cleanup);
    if (canny_hysteresis(state, width, height, edges) != CANNY_OK) {
        goto cleanup;
    }

    /* Stage 7: Graph construction
     * Inputs: edges, magnitudes, width, height.
     * Output: output->edge_pixels and pixel_to_edge (edge graph and lookup).
     */
    RECORD_TIME(stamp, PIPELINE_STAGE_GRAPH, goto cleanup);
    if (edge_graph_build(edges, magnitudes, width, height, output) != EDGE_PATHS_OK) {
        goto cleanup;
    }

    /* Stage 8: Path tracing
     * Input: graph in output (visited masks are updated in place).
     * Outputs: output->paths and point_ids (ordered original paths).
     */
    RECORD_TIME(stamp, PIPELINE_STAGE_TRACE, goto cleanup);
    if (edge_paths_trace(output) != EDGE_PATHS_OK) {
        goto cleanup;
    }

    /* Stage 9: Path measurement and filtering
     * Inputs: original paths, minimum_length, minimum_strength.
     * Output: per-path retained flag. Original paths remain available.
     */
    RECORD_TIME(stamp, PIPELINE_STAGE_MEASURE, goto cleanup);
    if (edge_paths_measure(output, options->minimum_length,
                              options->minimum_strength) != EDGE_PATHS_OK) {
        goto cleanup;
    }

    /* Stage 10: Path simplification
     * Inputs: retained original paths, tolerance (pixels).
     * Outputs: output->vertex_ids and per-path simplified vertex slices.
     */
    RECORD_TIME(stamp, PIPELINE_STAGE_SIMPLIFY, goto cleanup);
    if (edge_paths_simplify(output, options->tolerance) != EDGE_PATHS_OK) {
        goto cleanup;
    }

    RECORD_TIME(stamp, PIPELINE_STAGE_CLEANUP, goto cleanup);
    success = 1;

cleanup:
    free(blurred);
    free(gradient_x);
    free(gradient_y);
    free(direction);
    free(suppressed);
    free(state);
    free(edges);
    free(magnitudes);
    if (success) {
        RECORD_TIME(stamp, PIPELINE_STAGE_COUNT, return 0);
        /* Publish only complete measurements, including temporary cleanup. */
        for (size_t stage = 0; stage < PIPELINE_STAGE_COUNT; stage++) {
            timings->seconds[stage] = elapsed(&stamp[stage], &stamp[stage + 1]);
        }
        timings->total = elapsed(&stamp[0], &stamp[PIPELINE_STAGE_COUNT]);
    }
    return success;
}

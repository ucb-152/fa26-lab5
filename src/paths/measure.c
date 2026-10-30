#include "internal.h"

#include <math.h>

/* Compute original polyline length sum(||p[i]-p[i-1]||_2) and mean strength.
 * Exclude the repeated closing entry from the mean. Retain iff length >=
 * minimum_length and mean >= minimum_mean_strength.
 * 
 * In/out: result (struct PipelineResult*), valid traced paths; updates each path's
 *     retained flag.
 * In: minimum_length (double), finite nonnegative pixels; minimum_mean_strength
 *     (double), finite nonnegative gradient-strength cutoff.
 * Return: int EDGE_PATHS_OK or EDGE_PATHS_INVALID_ARGUMENT. Original points are preserved.
 */
int edge_paths_measure(struct PipelineResult *result, double minimum_length,
                          double minimum_mean_strength)
{
    if (result == NULL || !isfinite(minimum_length) || minimum_length < 0.0 ||
        !isfinite(minimum_mean_strength) || minimum_mean_strength < 0.0) {
        return EDGE_PATHS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < result->path_count; index++) {
        struct EdgePath *path = &result->paths[index];
        size_t unique_count = path->point_count;
        double length = 0.0;
        double strength_sum = 0.0;
        if (path->closed) {
            unique_count--; /* The final point repeats the start of the loop. */
        }

        /* These simple, separate passes are the baseline implementation. */
        for (size_t position = 1; position < path->point_count; position++) {
            const struct EdgePixel *previous = path_point(result, path, position - 1);
            const struct EdgePixel *current = path_point(result, path, position);
            double dx = (double)current->x - (double)previous->x;
            double dy = (double)current->y - (double)previous->y;
            length += sqrt(dx * dx + dy * dy);
        }

        for (size_t position = 0; position < unique_count; position++) {
            const struct EdgePixel *point = path_point(result, path, position);
            strength_sum += point->strength;
        }
        path->retained = length >= minimum_length &&
                         strength_sum / (double)unique_count >= minimum_mean_strength;
    }
    return EDGE_PATHS_OK;
}


#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Count the initial call as depth 1: at most 64 range calls are active. */
enum { MAX_SIMPLIFICATION_DEPTH = 64 };

/* Distance to the finite segment AB, not its infinitely extended line. The
 * projection is clamped so points beyond an endpoint use that endpoint.
 */
static double squared_segment_distance(const struct EdgePixel *point,
                                        const struct EdgePixel *a,
                                        const struct EdgePixel *b)
{
    double ab_x = (double)b->x - (double)a->x;
    double ab_y = (double)b->y - (double)a->y;
    double ap_x = (double)point->x - (double)a->x;
    double ap_y = (double)point->y - (double)a->y;
    double denominator = ab_x * ab_x + ab_y * ab_y;
    double fraction = 0.0;
    double error_x;
    double error_y;

    if (denominator != 0.0) {
        fraction = (ap_x * ab_x + ap_y * ab_y) / denominator;
        if (fraction < 0.0) {
            fraction = 0.0;
        } else if (fraction > 1.0) {
            fraction = 1.0;
        }
    }
    error_x = ap_x - fraction * ab_x;
    error_y = ap_y - fraction * ab_y;
    return error_x * error_x + error_y * error_y;
}

/* Keep endpoints, find the farthest interior point, and split only when its
 * error exceeds tolerance. Equal errors choose the earliest original point.
 */
static void simplify_range(const struct PipelineResult *result,
                           const struct EdgePath *path, size_t first, size_t last,
                           double tolerance_squared, uint8_t *selected,
                           unsigned int depth)
{
    selected[first] = 1;
    selected[last] = 1;
    if (last - first <= 1) {
        return; /* A single point or segment has no interior points to test. */
    }

    if (depth >= MAX_SIMPLIFICATION_DEPTH) {
        /* Keep this subrange unchanged: no further recursion or added error. */
        for (size_t position = first; position <= last; position++) {
            selected[position] = 1;
        }
        return;
    }

    const struct EdgePixel *a = path_point(result, path, first);
    const struct EdgePixel *b = path_point(result, path, last);
    double largest_error = -1.0;
    size_t split = first;

    for (size_t position = first + 1; position < last; position++) {
        const struct EdgePixel *point = path_point(result, path, position);
        double error = squared_segment_distance(point, a, b);
        if (error > largest_error) {
            largest_error = error;
            split = position;
        }
    }
    if (largest_error > tolerance_squared) {
        simplify_range(result, path, first, split, tolerance_squared, selected,
                       depth + 1);
        simplify_range(result, path, split, last, tolerance_squared, selected,
                       depth + 1);
    }
}

/* A closed path has coincident endpoints. Give it three deterministic anchors
 * before simplifying its arcs, so even a very large tolerance cannot collapse
 * a loop to a point or a two-vertex line.
 */
static void simplify_closed_path(const struct PipelineResult *result,
                                 const struct EdgePath *path,
                                 double tolerance_squared, uint8_t *selected)
{
    const struct EdgePixel *start = path_point(result, path, 0);
    size_t last = path->point_count - 1;
    size_t farthest = 1;
    size_t third = 1;
    double largest_distance = -1.0;
    double largest_error = -1.0;
    size_t anchor_a;
    size_t anchor_b;

    for (size_t position = 1; position < last; position++) {
        const struct EdgePixel *point = path_point(result, path, position);
        double dx = (double)point->x - (double)start->x;
        double dy = (double)point->y - (double)start->y;
        double distance = dx * dx + dy * dy;
        if (distance > largest_distance) {
            largest_distance = distance;
            farthest = position;
        }
    }
    for (size_t position = 1; position < last; position++) {
        double error;
        if (position == farthest) {
            continue;
        }
        error = squared_segment_distance(path_point(result, path, position),
                                         start, path_point(result, path, farthest));
        if (error > largest_error) {
            largest_error = error;
            third = position;
        }
    }
    anchor_a = farthest < third ? farthest : third;
    anchor_b = farthest < third ? third : farthest;
    simplify_range(result, path, 0, anchor_a, tolerance_squared, selected, 1);
    simplify_range(result, path, anchor_a, anchor_b, tolerance_squared, selected, 1);
    simplify_range(result, path, anchor_b, last, tolerance_squared, selected, 1);
}

int edge_paths_simplify(struct PipelineResult *result, double tolerance)
{
    size_t longest_path = 0;
    uint8_t *selected = NULL;
    size_t *vertices = NULL;
    double tolerance_squared;

    if (result == NULL || !isfinite(tolerance) || tolerance < 0.0) {
        return EDGE_PATHS_INVALID_ARGUMENT;
    }
    tolerance_squared = tolerance * tolerance;
    if (!isfinite(tolerance_squared)) {
        return EDGE_PATHS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < result->path_count; index++) {
        const struct EdgePath *path = &result->paths[index];
        if (path->retained && path->point_count > longest_path) {
            longest_path = path->point_count;
        }
    }
    if (result->point_count > SIZE_MAX / sizeof(size_t)) {
        return EDGE_PATHS_INVALID_ARGUMENT;
    }
    if (longest_path != 0) {
        selected = malloc(longest_path);
        vertices = malloc(result->point_count * sizeof(size_t));
        if (selected == NULL || vertices == NULL) {
            free(selected);
            free(vertices);
            return EDGE_PATHS_OUT_OF_MEMORY;
        }
    }

    free(result->vertex_ids);
    result->vertex_ids = vertices;
    result->vertex_count = 0;
    for (size_t index = 0; index < result->path_count; index++) {
        struct EdgePath *path = &result->paths[index];
        path->first_vertex = result->vertex_count;
        path->vertex_count = 0;
        if (!path->retained) {
            continue;
        }
        memset(selected, 0, path->point_count);
        if (path->closed) {
            simplify_closed_path(result, path, tolerance_squared, selected);
        } else {
            simplify_range(result, path, 0, path->point_count - 1,
                           tolerance_squared, selected, 1);
        }
        for (size_t position = 0; position < path->point_count; position++) {
            if (selected[position]) {
                result->vertex_ids[result->vertex_count] =
                    result->point_ids[path->first_point + position];
                result->vertex_count++;
                path->vertex_count++;
            }
        }
    }
    free(selected);
    return EDGE_PATHS_OK;
}

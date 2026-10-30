#include "internal.h"

#include <stdlib.h>

/* Find the lowest set bit in neighbor_mask & ~visited_mask.
 * In: edge_pixel (const struct EdgePixel*), a valid edge pixel.
 * Return: int direction 0..7, or -1 when no unused connection remains.
 * No output parameters or mutations.
 */
static int first_unused_direction(const struct EdgePixel *edge_pixel)
{
    for (int direction = 0; direction < 8; direction++) {
        unsigned int bit = 1u << direction;
        if ((edge_pixel->neighbor_mask & bit) != 0 &&
            (edge_pixel->visited_mask & bit) == 0) {
            return direction;
        }
    }
    return -1;
}

/* Consume a graph walk until an endpoint/junction or return to its start.
 * Mark each traversed connection at both ends so it is used only once.
 * In/out: result (struct PipelineResult*), valid graph with preallocated paths/point_ids
 *     capacity; appends one path and its IDs, increments counts, updates visited masks.
 * In: start_id (size_t), valid edge_pixel ID; direction (int), unused neighbor 0..7,
 *     or -1 to emit an isolated single-point path.
 * Return: void. Closed paths repeat their starting ID.
 */
static void trace_one_path(struct PipelineResult *result, size_t start_id, int direction)
{
    struct EdgePath path = {0};
    size_t current_id = start_id;
    path.first_point = result->point_count;
    result->point_ids[result->point_count++] = start_id;

    while (direction >= 0) {
        struct EdgePixel *current = &result->edge_pixels[current_id];
        size_t nx;
        size_t ny;
        size_t next_id;
        struct EdgePixel *next;
        int opposite = (direction + 4) % 8;

        /* The graph contains only in-bounds connections. */
        neighbor_position(current->x, current->y, direction,
                          result->width, result->height, &nx, &ny);
        next_id = result->pixel_to_edge[ny * result->width + nx];
        next = &result->edge_pixels[next_id];
        current->visited_mask |= (uint8_t)(1u << direction);
        next->visited_mask |= (uint8_t)(1u << opposite);
        result->point_ids[result->point_count++] = next_id;
        if (next_id == start_id) {
            path.closed = 1;
            break;
        }
        if (next->degree != 2) {
            break;
        }
        current_id = next_id;
        direction = first_unused_direction(next);
    }

    path.point_count = result->point_count - path.first_point;
    result->paths[result->path_count++] = path;
}

/* Decompose the graph into ordered paths: first walk unused connections
 * from endpoints/junctions, then remaining degree-two cycles. Isolated pixels
 * become single-point paths; junctions may appear in multiple paths.
 * 
 * In/out: result (struct PipelineResult*), graph from edge_graph_build, not yet traced.
 *     Allocates paths and point_ids, sets their counts, and updates visited masks.
 *     Each undirected connection appears once; closed paths repeat their first ID.
 * Return: int EDGE_PATHS_OK, EDGE_PATHS_INVALID_ARGUMENT, or EDGE_PATHS_OUT_OF_MEMORY.
 * Release result with pipeline_result_free even after failure.
 */
int edge_paths_trace(struct PipelineResult *result)
{
    if (result == NULL || result->pixel_to_edge == NULL ||
        result->paths != NULL || result->point_ids != NULL ||
        result->path_count != 0 || result->point_count != 0) {
        return EDGE_PATHS_INVALID_ARGUMENT;
    }

    /* Each connection contributes two degrees. A path uses at least one
     * connection and stores at most two points per connection. Isolated pixels
     * each need one extra path and one point. Allocate these upper bounds once
     * so the walk below only follows connections and edge_pixels points.
     */
    size_t degree_sum = 0;
    size_t isolated_count = 0;
    for (size_t id = 0; id < result->edge_pixel_count; id++) {
        size_t degree = result->edge_pixels[id].degree;
        if (degree > SIZE_MAX - degree_sum) {
            return EDGE_PATHS_INVALID_ARGUMENT;
        }
        degree_sum += degree;
        isolated_count += degree == 0;
    }
    if (isolated_count > SIZE_MAX - degree_sum) {
        return EDGE_PATHS_INVALID_ARGUMENT;
    }
    size_t point_capacity = degree_sum + isolated_count;
    size_t path_capacity = degree_sum / 2 + isolated_count;
    if (point_capacity > SIZE_MAX / sizeof(size_t) ||
        path_capacity > SIZE_MAX / sizeof(struct EdgePath)) {
        return EDGE_PATHS_INVALID_ARGUMENT;
    }
    if (path_capacity != 0) {
        result->point_ids = malloc(point_capacity * sizeof(size_t));
        result->paths = calloc(path_capacity, sizeof(struct EdgePath));
        if (result->point_ids == NULL || result->paths == NULL) {
            return EDGE_PATHS_OUT_OF_MEMORY;
        }
    }

    /* First extract all paths attached to endpoints or junctions. */
    for (size_t id = 0; id < result->edge_pixel_count; id++) {
        struct EdgePixel *edge_pixel = &result->edge_pixels[id];
        if (edge_pixel->degree == 2) {
            continue;
        }
        if (edge_pixel->degree == 0) {
            trace_one_path(result, id, -1);
        } else {
            int direction = first_unused_direction(edge_pixel);
            while (direction >= 0) {
                trace_one_path(result, id, direction);
                direction = first_unused_direction(edge_pixel);
            }
        }
    }

    /* Any unvisited connection now belongs to a cycle of degree-two pixels. */
    for (size_t id = 0; id < result->edge_pixel_count; id++) {
        int direction = first_unused_direction(&result->edge_pixels[id]);
        if (direction >= 0) {
            trace_one_path(result, id, direction);
        }
    }
    return EDGE_PATHS_OK;
}

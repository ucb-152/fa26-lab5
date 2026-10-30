#ifndef EDGE_PATHS_INTERNAL_H
#define EDGE_PATHS_INTERNAL_H

#include "edge_paths.h"

/* Clockwise order, starting east. Opposite directions differ by four. */
static const int direction_x[8] = {1, 1, 0, -1, -1, -1, 0, 1};
static const int direction_y[8] = {0, 1, 1, 1, 0, -1, -1, -1};

/* Translate a pixel by one of eight clockwise unit offsets (east first).
 * In: x, y, width, height (size_t), valid pixel and positive image dimensions;
 *     direction (int), index 0..7.
 * Out: neighbor_x, neighbor_y (size_t*), translated coordinates on success only.
 * Return: int 1 if the neighbor is in bounds, otherwise 0; no unsigned wraparound.
 */
static inline int neighbor_position(size_t x, size_t y, int direction,
                             size_t width, size_t height,
                             size_t *neighbor_x, size_t *neighbor_y)
{
    int dx = direction_x[direction];
    int dy = direction_y[direction];

    if ((dx < 0 && x == 0) || (dx > 0 && x == width - 1) ||
        (dy < 0 && y == 0) || (dy > 0 && y == height - 1)) {
        return 0;
    }

    *neighbor_x = x;
    *neighbor_y = y;
    if (dx < 0) {
        (*neighbor_x)--;
    } else if (dx > 0) {
        (*neighbor_x)++;
    }
    if (dy < 0) {
        (*neighbor_y)--;
    } else if (dy > 0) {
        (*neighbor_y)++;
    }
    return 1;
}

/* Resolve a path-relative position through point_ids to its edge pixel.
 * In: result (const struct PipelineResult*), valid traced graph;
 *     path (const struct EdgePath*), path within it; position (size_t), < point_count.
 * Return: const struct EdgePixel* borrowed from result->edge_pixels; no mutation.
 */
static inline const struct EdgePixel *path_point(const struct PipelineResult *result,
                                           const struct EdgePath *path,
                                           size_t position)
{
    size_t id = result->point_ids[path->first_point + position];
    return &result->edge_pixels[id];
}

#endif

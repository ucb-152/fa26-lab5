#include "internal.h"

#include <stdlib.h>

/* Assign row-major IDs to nonzero edge pixels and build a symmetric
 * neighbor graph. Connect orthogonal neighbors; connect diagonal neighbors only
 * when both intervening orthogonal pixels are absent. Degree is the mask bit count.
 * 
 * In: edges (const uint8_t*), binary/nonzero edge image; strengths
 *     (const uint16_t*), original gradient magnitude image, each width*height.
 * In: width, height (size_t), positive dimensions with representable allocations.
 * In/out: result (struct PipelineResult*), initially zeroed; receives dimensions,
 *     edge_pixels (coordinates, strength, masks, degree), and pixel_to_edge lookup
 *     (SIZE_MAX for absent pixels). Owns allocated arrays; release with pipeline_result_free.
 * Return: int EDGE_PATHS_OK, EDGE_PATHS_INVALID_ARGUMENT, or EDGE_PATHS_OUT_OF_MEMORY.
 */
int edge_graph_build(const uint8_t *edges, const uint16_t *strengths,
                        size_t width, size_t height, struct PipelineResult *result)
{
    size_t pixel_count;
    size_t edge_pixel_count = 0;
    size_t edge_pixel_id = 0;

    if (edges == NULL || strengths == NULL || result == NULL ||
        width == 0 || height == 0 || width > SIZE_MAX / height) {
        return EDGE_PATHS_INVALID_ARGUMENT;
    }
    pixel_count = width * height;
    if (pixel_count > SIZE_MAX / sizeof(size_t)) {
        return EDGE_PATHS_INVALID_ARGUMENT;
    }

    for (size_t index = 0; index < pixel_count; index++) {
        if (edges[index] != 0) {
            edge_pixel_count++;
        }
    }
    if (edge_pixel_count > SIZE_MAX / sizeof(struct EdgePixel)) {
        return EDGE_PATHS_INVALID_ARGUMENT;
    }

    result->width = width;
    result->height = height;
    result->edge_pixel_count = edge_pixel_count;
    result->pixel_to_edge = malloc(pixel_count * sizeof(size_t));
    if (edge_pixel_count != 0) {
        result->edge_pixels = calloc(edge_pixel_count, sizeof(struct EdgePixel));
    }
    if (result->pixel_to_edge == NULL ||
        (edge_pixel_count != 0 && result->edge_pixels == NULL)) {
        pipeline_result_free(result);
        return EDGE_PATHS_OUT_OF_MEMORY;
    }

    /* Assign stable IDs in row-major order before constructing connections. */
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            size_t index = y * width + x;
            result->pixel_to_edge[index] = SIZE_MAX;
            if (edges[index] != 0) {
                struct EdgePixel *edge_pixel = &result->edge_pixels[edge_pixel_id];
                edge_pixel->x = x;
                edge_pixel->y = y;
                edge_pixel->strength = strengths[index];
                result->pixel_to_edge[index] = edge_pixel_id;
                edge_pixel_id++;
            }
        }
    }

    for (edge_pixel_id = 0; edge_pixel_id < edge_pixel_count; edge_pixel_id++) {
        struct EdgePixel *edge_pixel = &result->edge_pixels[edge_pixel_id];
        for (int direction = 0; direction < 8; direction++) {
            size_t nx;
            size_t ny;
            if (!neighbor_position(edge_pixel->x, edge_pixel->y, direction,
                                   width, height, &nx, &ny)) {
                continue;
            }
            if (edges[ny * width + nx] == 0) {
                continue;
            }

            /* An orthogonal route takes precedence over a diagonal shortcut.
             * This prevents a right-angle stroke from becoming a triangle.
             * This edge-connection rule is distinct from Canny's hysteresis rule.
             */
            if (direction_x[direction] != 0 && direction_y[direction] != 0) {
                if (edges[edge_pixel->y * width + nx] != 0 ||
                    edges[ny * width + edge_pixel->x] != 0) {
                    continue;
                }
            }
            edge_pixel->neighbor_mask |= (uint8_t)(1u << direction);
            edge_pixel->degree++;
        }
    }
    return EDGE_PATHS_OK;
}


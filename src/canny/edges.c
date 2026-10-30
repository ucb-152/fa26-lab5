#include "internal.h"

#include <stdlib.h>

struct PixelCoordinate {
    size_t x;
    size_t y;
};

/* Thin strengths by comparing each interior pixel to its two neighbors
 * along the quantized gradient direction. For 0/90 degrees require m > first
 * and m >= second; for 45/135 require m > both. Set all other pixels,
 * including the one-pixel border, to zero.
 * 
 * In: magnitude (const uint16_t*), strengths; direction (const uint8_t*),
 *     orientations from canny_gradient_magnitude_and_direction.
 * In: width, height (size_t), image dimensions.
 * Out: suppressed (uint16_t*), retained strengths.
 * Return: void. Arrays contain width*height elements and must not overlap.
 */
void canny_non_maximum_suppression(const uint16_t *magnitude, const uint8_t *direction,
    size_t width, size_t height, uint16_t *suppressed)
{
    size_t x;
    size_t y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            suppressed[pixel_index(x, y, width)] = 0;
        }
    }

    for (y = 1; y + 1 < height; y++) {
        for (x = 1; x + 1 < width; x++) {
            size_t index = pixel_index(x, y, width);
            uint16_t neighbor_1;
            uint16_t neighbor_2;

            if (direction[index] == DIRECTION_0) {
                neighbor_1 = magnitude[pixel_index(x - 1, y, width)];
                neighbor_2 = magnitude[pixel_index(x + 1, y, width)];
            } else if (direction[index] == DIRECTION_45) {
                neighbor_1 = magnitude[pixel_index(x - 1, y - 1, width)];
                neighbor_2 = magnitude[pixel_index(x + 1, y + 1, width)];
            } else if (direction[index] == DIRECTION_90) {
                neighbor_1 = magnitude[pixel_index(x, y - 1, width)];
                neighbor_2 = magnitude[pixel_index(x, y + 1, width)];
            } else {
                neighbor_1 = magnitude[pixel_index(x - 1, y + 1, width)];
                neighbor_2 = magnitude[pixel_index(x + 1, y - 1, width)];
            }

            if (direction[index] == DIRECTION_0 || direction[index] == DIRECTION_90) {
                if (magnitude[index] > neighbor_1 && magnitude[index] >= neighbor_2) {
                    suppressed[index] = magnitude[index];
                }
            } else if (magnitude[index] > neighbor_1 && magnitude[index] > neighbor_2) {
                suppressed[index] = magnitude[index];
            }
        }
    }
}

/* Classify m == 0 or m < low as absent, m >= high as strong,
 * and remaining nonzero magnitudes as weak. Both thresholds are inclusive.
 * 
 * In: suppressed (const uint16_t*), thinned strengths.
 * In: width, height (size_t), image dimensions.
 * In: low_threshold, high_threshold (uint16_t), ordered low <= high.
 * Out: state (uint8_t*), EDGE_NONE/EDGE_WEAK/EDGE_STRONG labels.
 * Return: void. Arrays contain width*height elements and must not overlap.
 */
void canny_double_threshold(const uint16_t *suppressed, size_t width, size_t height,
    uint16_t low_threshold, uint16_t high_threshold, uint8_t *state)
{
    size_t x;
    size_t y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            size_t index = pixel_index(x, y, width);

            if (suppressed[index] == 0) {
                state[index] = EDGE_NONE;
            } else if (suppressed[index] >= high_threshold) {
                state[index] = EDGE_STRONG;
            } else if (suppressed[index] >= low_threshold) {
                state[index] = EDGE_WEAK;
            } else {
                state[index] = EDGE_NONE;
            }
        }
    }
}

/* Flood-fill from strong pixels over eight-neighbor weak/strong
 * connectivity. Promote reachable weak pixels and emit a binary edge image.
 * 
 * In/out: state (uint8_t*), width*height labels; reachable weak labels become strong.
 * In: width, height (size_t), positive dimensions with representable allocation sizes.
 * Out: edges (uint8_t*), width*height pixels: 255 for strong, otherwise 0.
 * Return: int CANNY_OK, CANNY_INVALID_ARGUMENT, or CANNY_OUT_OF_MEMORY.
 * Buffers must not overlap. Validation/allocation failure leaves them unchanged.
 */
int canny_hysteresis(uint8_t *state, size_t width, size_t height, uint8_t *edges)
{
    size_t pixel_count;
    struct PixelCoordinate *queue;
    size_t queue_head = 0;
    size_t queue_tail = 0;
    size_t x;
    size_t y;

    if (state == NULL || edges == NULL || width == 0 || height == 0 || width > SIZE_MAX / height) {
        return CANNY_INVALID_ARGUMENT;
    }

    pixel_count = width * height;
    if (pixel_count > SIZE_MAX / sizeof(*queue)) {
        return CANNY_INVALID_ARGUMENT;
    }

    queue = malloc(pixel_count * sizeof(*queue));
    if (queue == NULL) {
        return CANNY_OUT_OF_MEMORY;
    }

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            size_t index = pixel_index(x, y, width);
            edges[index] = 0;

            if (state[index] == EDGE_STRONG) {
                queue[queue_tail].x = x;
                queue[queue_tail].y = y;
                queue_tail++;
            }
        }
    }

    while (queue_head < queue_tail) {
        struct PixelCoordinate point = queue[queue_head];
        int offset_y;
        int offset_x;
        queue_head++;

        for (offset_y = -1; offset_y <= 1; offset_y++) {
            for (offset_x = -1; offset_x <= 1; offset_x++) {
                size_t neighbor_x;
                size_t neighbor_y;
                size_t neighbor_index;

                if (offset_x == 0 && offset_y == 0) {
                    continue;
                }

                if ((offset_x < 0 && point.x == 0) ||
                    (offset_x > 0 && point.x + 1 >= width) ||
                    (offset_y < 0 && point.y == 0) ||
                    (offset_y > 0 && point.y + 1 >= height)) {
                    continue;
                }

                neighbor_x = (size_t)((int64_t)point.x + offset_x);
                neighbor_y = (size_t)((int64_t)point.y + offset_y);
                neighbor_index = pixel_index(neighbor_x, neighbor_y, width);

                if (state[neighbor_index] == EDGE_WEAK) {
                    state[neighbor_index] = EDGE_STRONG;
                    queue[queue_tail].x = neighbor_x;
                    queue[queue_tail].y = neighbor_y;
                    queue_tail++;
                }
            }
        }
    }

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            size_t index = pixel_index(x, y, width);

            if (state[index] == EDGE_STRONG) {
                edges[index] = 255;
            }
        }
    }

    free(queue);
    return CANNY_OK;
}


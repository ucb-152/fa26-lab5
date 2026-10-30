#ifndef CANNY_INTERNAL_H
#define CANNY_INTERNAL_H

#include "canny.h"

enum {
    DIRECTION_0 = 0,
    DIRECTION_45 = 45,
    DIRECTION_90 = 90,
    DIRECTION_135 = 135
};

enum {
    EDGE_NONE = 0,
    EDGE_WEAK = 1,
    EDGE_STRONG = 2
};

/* Map a row-major pixel coordinate to its linear array offset y*width+x.
 * In: x, y, width (size_t), valid coordinates with representable index arithmetic.
 * Return: size_t array index; no output parameters or mutation.
 */
static inline size_t pixel_index(size_t x, size_t y, size_t width)
{
    return y * width + x;
}

/* Replicate a border by clamping a coordinate to [0, limit-1].
 * In: coordinate (int64_t), possibly out of bounds; limit (size_t), positive extent.
 * Return: size_t nearest in-bounds coordinate; no output parameters or mutation.
 */
static inline size_t clamp_coordinate(int64_t coordinate, size_t limit)
{
    if (coordinate < 0) {
        return 0;
    }

    if ((uint64_t)coordinate >= limit) {
        return limit - 1;
    }

    return (size_t)coordinate;
}

#endif

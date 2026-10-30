#ifndef CANNY_H
#define CANNY_H

#include <stddef.h>
#include <stdint.h>

enum {
    CANNY_OK = 0,
    CANNY_INVALID_ARGUMENT = 1,
    CANNY_OUT_OF_MEMORY = 2
};

/* Write the 5-by-5 Gaussian-blurred form of input to blurred; both arrays contain width *
 * height pixels.
 */
void canny_gaussian_blur(const uint8_t *input, size_t width, size_t height, uint8_t *blurred);

/* Write the signed Sobel gradients of blurred to the width * height gradient arrays. */
void canny_sobel(const uint8_t *blurred, size_t width, size_t height, int16_t *gradient_x,
    int16_t *gradient_y);

/* Write each gradient's Euclidean magnitude and quantized direction to width * height
 * output arrays.
 */
void canny_gradient_magnitude_and_direction(const int16_t *gradient_x,
    const int16_t *gradient_y, size_t width, size_t height, uint16_t *magnitude,
    uint8_t *direction);

/* Suppress directional non-maxima and write the retained magnitudes to suppressed. */
void canny_non_maximum_suppression(const uint16_t *magnitude, const uint8_t *direction,
    size_t width, size_t height, uint16_t *suppressed);

/* Classify suppressed magnitudes into internal absent, weak, and strong states. */
void canny_double_threshold(const uint16_t *suppressed, size_t width, size_t height,
    uint16_t low_threshold, uint16_t high_threshold, uint8_t *state);

/* Resolve weak-edge connectivity in state and write binary edge pixels to edges; return a
 * CANNY_* status code.
 */
int canny_hysteresis(uint8_t *state, size_t width, size_t height, uint8_t *edges);

#endif

#include "internal.h"

/* Compute a 5x5 Gaussian correlation with kernel g*g^T / 256,
 * where g = [1, 4, 6, 4, 1]. Replicate borders; round via (sum + 128)/256.
 *
 * In: input (const uint8_t*), row-major grayscale image.
 * In: width, height (size_t), positive image dimensions.
 * Out: blurred (uint8_t*), width*height smoothed pixels.
 * Return: void. Buffers must be valid, sized for the image, and nonoverlapping.
 */
void canny_gaussian_blur(const uint8_t *input, size_t width, size_t height, uint8_t *blurred)
{
    static const int gaussian[5][5] = {
        {1,  4,  6,  4, 1},
        {4, 16, 24, 16, 4},
        {6, 24, 36, 24, 6},
        {4, 16, 24, 16, 4},
        {1,  4,  6,  4, 1}
    };
    size_t x;
    size_t y;

    /* Visit pixels row by row; each output is independent. */
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int sum = 0;
            int kernel_y;
            int kernel_x;

            for (kernel_y = 0; kernel_y < 5; kernel_y++) {
                for (kernel_x = 0; kernel_x < 5; kernel_x++) {
                    size_t input_x = clamp_coordinate((int64_t)x + kernel_x - 2, width);
                    size_t input_y = clamp_coordinate((int64_t)y + kernel_y - 2, height);
                    sum += gaussian[kernel_y][kernel_x] * input[pixel_index(input_x, input_y, width)];
                }
            }

            blurred[pixel_index(x, y, width)] = (uint8_t)((sum + 128) / 256);
        }
    }
}

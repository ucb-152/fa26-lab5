#include "internal.h"

#include <math.h>

/* Correlate with Sobel kernels [-1,0,1; -2,0,2; -1,0,1] and its
 * transpose to estimate horizontal and vertical brightness derivatives.
 * Replicate border pixels.
 * 
 * In: blurred (const uint8_t*), row-major smoothed grayscale image.
 * In: width, height (size_t), positive image dimensions.
 * Out: gradient_x, gradient_y (int16_t*), width*height signed derivatives.
 * Return: void. All arrays must be valid, sized for the image, and nonoverlapping.
 */
void canny_sobel(const uint8_t *blurred, size_t width, size_t height, int16_t *gradient_x,
    int16_t *gradient_y)
{
    static const int sobel_x[3][3] = {
        {-1, 0, 1},
        {-2, 0, 2},
        {-1, 0, 1}
    };
    static const int sobel_y[3][3] = {
        {-1, -2, -1},
        { 0,  0,  0},
        { 1,  2,  1}
    };
    size_t x;
    size_t y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int sum_x = 0;
            int sum_y = 0;
            int kernel_y;
            int kernel_x;

            for (kernel_y = 0; kernel_y < 3; kernel_y++) {
                for (kernel_x = 0; kernel_x < 3; kernel_x++) {
                    size_t input_x = clamp_coordinate((int64_t)x + kernel_x - 1, width);
                    size_t input_y = clamp_coordinate((int64_t)y + kernel_y - 1, height);
                    uint8_t pixel = blurred[pixel_index(input_x, input_y, width)];
                    sum_x += sobel_x[kernel_y][kernel_x] * pixel;
                    sum_y += sobel_y[kernel_y][kernel_x] * pixel;
                }
            }

            gradient_x[pixel_index(x, y, width)] = (int16_t)sum_x;
            gradient_y[pixel_index(x, y, width)] = (int16_t)sum_y;
        }
    }
}

/* Compute floor(sqrt(gx*gx + gy*gy)) and quantize atan2(gy,gx)
 * modulo 180 degrees to the nearest 0, 45, 90, or 135 degree orientation.
 * Bin boundaries are 22.5, 67.5, 112.5, and 157.5 degrees.
 * 
 * In: gradient_x, gradient_y (const int16_t*), Sobel derivative images.
 * In: width, height (size_t), image dimensions.
 * Out: magnitude (uint16_t*), gradient strengths; direction (uint8_t*), degrees.
 * Return: void. Arrays contain width*height elements and must not overlap.
 * Derivative values must have magnitudes representable in uint16_t (as Sobel does).
 */
void canny_gradient_magnitude_and_direction(const int16_t *gradient_x,
    const int16_t *gradient_y, size_t width, size_t height, uint16_t *magnitude,
    uint8_t *direction)
{
    const double radians_to_degrees = 57.2957795130823208768;
    size_t x;
    size_t y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            size_t index = pixel_index(x, y, width);
            double gx = gradient_x[index];
            double gy = gradient_y[index];
            double angle = atan2(gy, gx) * radians_to_degrees;

            magnitude[index] = (uint16_t)sqrt(gx * gx + gy * gy);

            if (angle < 0.0) {
                angle += 180.0;
            }

            if (angle < 22.5 || angle >= 157.5) {
                direction[index] = DIRECTION_0;
            } else if (angle < 67.5) {
                direction[index] = DIRECTION_45;
            } else if (angle < 112.5) {
                direction[index] = DIRECTION_90;
            } else {
                direction[index] = DIRECTION_135;
            }
        }
    }
}


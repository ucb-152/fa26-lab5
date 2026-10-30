#include "pgm.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Read one whitespace-delimited PGM header or sample token while skipping comments; return
 * nonzero on success.
 */
static int read_token(FILE *file, char *token, size_t token_capacity)
{
    int character;
    size_t length = 0;

    do {
        character = fgetc(file);

        if (character == '#') {
            do {
                character = fgetc(file);
            } while (character != '\n' && character != EOF);
        }
    } while (character != EOF && isspace((unsigned char)character));

    if (character == EOF) {
        return 0;
    }

    while (character != EOF && !isspace((unsigned char)character) && character != '#') {
        if (length + 1 >= token_capacity) {
            return 0;
        }

        token[length] = (char)character;
        length++;
        character = fgetc(file);
    }

    if (character == '#') {
        do {
            character = fgetc(file);
        } while (character != '\n' && character != EOF);
    } else if (character == '\r') {
        character = fgetc(file);
        if (character != '\n' && character != EOF) {
            ungetc(character, file);
        }
    }

    token[length] = '\0';
    return length != 0;
}

/* Parse one base-10 size_t value and reject malformed or overflowing text. */
static int parse_size(const char *token, size_t *value)
{
    char *end;
    unsigned long parsed;

    /* PGM dimensions and samples are unsigned decimal tokens. */
    for (const char *cursor = token; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return 0;
        }
    }
    errno = 0;
    parsed = strtoul(token, &end, 10);
    if (errno != 0 || *token == '\0' || *end != '\0' || parsed > SIZE_MAX) {
        return 0;
    }

    *value = (size_t)parsed;
    return 1;
}

/* Load an 8-bit P2 or P5 grayscale image, scale samples to [0, 255], and transfer
 * ownership of the allocated pixel array to the caller.
 */
int pgm_read(const char *path, uint8_t **pixels, size_t *width, size_t *height)
{
    FILE *file = NULL;
    uint8_t *image = NULL;
    char token[64];
    int binary;
    size_t max_value;
    size_t pixel_count;
    size_t i;
    int result = 0;

    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }

    if (!read_token(file, token, sizeof(token))) {
        goto cleanup;
    }

    if (token[0] == 'P' && token[1] == '5' && token[2] == '\0') {
        binary = 1;
    } else if (token[0] == 'P' && token[1] == '2' && token[2] == '\0') {
        binary = 0;
    } else {
        goto cleanup;
    }

    if (!read_token(file, token, sizeof(token)) || !parse_size(token, width) ||
        !read_token(file, token, sizeof(token)) || !parse_size(token, height) ||
        !read_token(file, token, sizeof(token)) || !parse_size(token, &max_value)) {
        goto cleanup;
    }

    if (*width == 0 || *height == 0 || *width > SIZE_MAX / *height || max_value == 0 || max_value > 255) {
        goto cleanup;
    }

    pixel_count = *width * *height;
    image = malloc(pixel_count * sizeof(*image));
    if (image == NULL) {
        goto cleanup;
    }

    if (binary) {
        if (fread(image, sizeof(*image), pixel_count, file) != pixel_count) {
            goto cleanup;
        }

        if (max_value != 255) {
            for (i = 0; i < pixel_count; i++) {
                if (image[i] > max_value) {
                    goto cleanup;
                }

                image[i] = (uint8_t)((image[i] * 255) / max_value);
            }
        }
    } else {
        for (i = 0; i < pixel_count; i++) {
            size_t value;

            if (!read_token(file, token, sizeof(token)) || !parse_size(token, &value) || value > max_value) {
                goto cleanup;
            }

            image[i] = (uint8_t)((value * 255) / max_value);
        }
    }

    *pixels = image;
    image = NULL;
    result = 1;

cleanup:
    free(image);
    fclose(file);
    return result;
}

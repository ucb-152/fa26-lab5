#ifndef PGM_H
#define PGM_H

#include <stddef.h>
#include <stdint.h>

/* Load a P2 or P5 image and return an allocated 8-bit pixel array owned by the caller. */
int pgm_read(const char *path, uint8_t **pixels, size_t *width, size_t *height);

#endif

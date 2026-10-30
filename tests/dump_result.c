/* Serialize the complete pipeline result for local reference comparisons.
 * Same logical fields and encoding as the staff grader; excludes timings,
 * pointer addresses, and struct padding. This program is not benchmarked. */
#include "pipeline.h"
#include "pgm.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Stable encoding of logical values, independent of padding and pointer values. */
static int put_u64(FILE *file, uint64_t value)
{
    unsigned char bytes[8];
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (unsigned char)(value >> (8 * i));
    return fwrite(bytes, 1, 8, file) == 8;
}

static int valid_pipeline_result(const struct PipelineResult *o, size_t width, size_t height)
{
    size_t n = width * height;
    if (n > SIZE_MAX / 9 || o->width != width || o->height != height ||
        o->edge_pixel_count > n || o->path_count > 5 * n ||
        o->point_count > 9 * n || o->vertex_count > o->point_count ||
        !o->pixel_to_edge || (o->edge_pixel_count && !o->edge_pixels) ||
        (o->path_count && !o->paths) || (o->point_count && !o->point_ids) ||
        (o->vertex_count && !o->vertex_ids)) return 0;
    for (size_t i = 0; i < o->edge_pixel_count; ++i) {
        const struct EdgePixel *r = &o->edge_pixels[i];
        if (r->x >= width || r->y >= height || r->degree > 8) return 0;
    }
    for (size_t i = 0; i < n; ++i) {
        size_t id = o->pixel_to_edge[i];
        if (id != SIZE_MAX && id >= o->edge_pixel_count) return 0;
    }
    for (size_t i = 0; i < o->path_count; ++i) {
        const struct EdgePath *p = &o->paths[i];
        if (p->closed > 1 || p->retained > 1 ||
            p->first_point > o->point_count ||
            p->point_count > o->point_count - p->first_point ||
            p->first_vertex > o->vertex_count ||
            p->vertex_count > o->vertex_count - p->first_vertex) return 0;
    }
    for (size_t i = 0; i < o->point_count; ++i)
        if (o->point_ids[i] >= o->edge_pixel_count) return 0;
    for (size_t i = 0; i < o->vertex_count; ++i)
        if (o->vertex_ids[i] >= o->edge_pixel_count) return 0;
    return 1;
}

static int write_pipeline_result(FILE *file, const struct PipelineResult *o)
{
#define PUT(value) do { if (!put_u64(file, (uint64_t)(value))) return 0; } while (0)
    if (fwrite("L5OUT001", 1, 8, file) != 8) return 0;
    PUT(o->width); PUT(o->height); PUT(o->edge_pixel_count); PUT(o->path_count);
    PUT(o->point_count); PUT(o->vertex_count);
    for (size_t i = 0; i < o->edge_pixel_count; ++i) {
        const struct EdgePixel *r = &o->edge_pixels[i];
        PUT(r->x); PUT(r->y); PUT(r->strength); PUT(r->neighbor_mask);
        PUT(r->visited_mask); PUT(r->degree);
    }
    for (size_t i = 0; i < o->width * o->height; ++i)
        PUT(o->pixel_to_edge[i] == SIZE_MAX ? UINT64_MAX : o->pixel_to_edge[i]);
    for (size_t i = 0; i < o->path_count; ++i) {
        const struct EdgePath *p = &o->paths[i];
        PUT(p->first_point); PUT(p->point_count); PUT(p->closed); PUT(p->retained);
        PUT(p->first_vertex); PUT(p->vertex_count);
    }
    for (size_t i = 0; i < o->point_count; ++i) PUT(o->point_ids[i]);
    for (size_t i = 0; i < o->vertex_count; ++i) PUT(o->vertex_ids[i]);
#undef PUT
    return 1;
}

/* Pipeline parameters do not change the algorithms. */
static int parse_options(int argc, char **argv, struct PipelineOptions *options)
{
    double numbers[5];
    *options = (struct PipelineOptions){22, 45, 8.0, 0.0, 1.0};
    if (argc == 3) {
        return 1;
    }
    if (argc != 8) {
        return 0;
    }
    for (size_t index = 0; index < 5; index++) {
        const char *text = argv[index + 3];
        char *end;
        errno = 0;
        numbers[index] = strtod(text, &end);
        if (errno != 0 || end == text || *end != '\0' ||
            !isfinite(numbers[index]) || numbers[index] < 0.0) {
            return 0;
        }
    }
    if (numbers[0] > numbers[1] || numbers[1] > UINT16_MAX ||
        floor(numbers[0]) != numbers[0] || floor(numbers[1]) != numbers[1] ||
        !isfinite(numbers[4] * numbers[4])) {
        return 0;
    }
    options->low_threshold = (uint16_t)numbers[0];
    options->high_threshold = (uint16_t)numbers[1];
    options->minimum_length = numbers[2];
    options->minimum_strength = numbers[3];
    options->tolerance = numbers[4];
    return 1;
}

int main(int argc, char **argv)
{
    struct PipelineOptions options;
    if (!parse_options(argc, argv, &options)) {
        fprintf(stderr, "usage: dump_result INPUT.pgm OUTPUT [LOW HIGH MIN_LENGTH MIN_STRENGTH TOLERANCE]\n");
        return 2;
    }
    uint8_t *input = NULL;
    size_t width, height;
    if (!pgm_read(argv[1], &input, &width, &height)) {
        fprintf(stderr, "Could not read input image.\n");
        return 2;
    }
    struct PipelineResult output = {0};
    struct PipelineTimings ignored = {0};
    int status = 1;
    if (!pipeline_run(input, width, height, &options, &output, &ignored) ||
        !valid_pipeline_result(&output, width, height)) {
        fprintf(stderr, "Kernel failed or returned an invalid pipeline result.\n");
        goto cleanup;
    }
    FILE *file = fopen(argv[2], "wb");
    if (!file) {
        perror(argv[2]);
        goto cleanup;
    }
    int written = write_pipeline_result(file, &output);
    if (fclose(file) || !written) {
        fprintf(stderr, "Could not write pipeline result.\n");
        goto cleanup;
    }
    status = 0;
cleanup:
    pipeline_result_free(&output);
    free(input);
    return status;
}

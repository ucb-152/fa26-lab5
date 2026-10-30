#include "svg_output.h"

#include <stdio.h>

int pipeline_result_write_svg(const char *filename, const struct PipelineResult *result)
{
    FILE *file = fopen(filename, "w");
    int success;
    if (file == NULL) {
        return 0;
    }

    fprintf(file, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(file, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%zu\" height=\"%zu\" viewBox=\"0 0 %zu %zu\">\n",
            result->width, result->height, result->width, result->height);
    fprintf(file, "  <title>Canny vector outline</title>\n");
    fprintf(file, "  <rect width=\"100%%\" height=\"100%%\" fill=\"white\"/>\n");
    fprintf(file, "  <g fill=\"none\" stroke=\"black\" stroke-width=\"1\" stroke-linecap=\"round\" stroke-linejoin=\"round\">\n");

    for (size_t index = 0; index < result->path_count; index++) {
        const struct EdgePath *path = &result->paths[index];
        size_t output_count = path->vertex_count;
        if (!path->retained) {
            continue;
        }
        if (path->closed) {
            output_count--; /* SVG's Z instruction closes the final segment. */
        }
        if (output_count == 1) {
            size_t id = result->vertex_ids[path->first_vertex];
            const struct EdgePixel *point = &result->edge_pixels[id];
            fprintf(file, "    <circle id=\"path-%zu\" cx=\"%zu.5\" cy=\"%zu.5\" r=\"0.5\" fill=\"black\" stroke=\"none\"/>\n",
                    index, point->x, point->y);
            continue;
        }
        fprintf(file, "    <path id=\"path-%zu\" d=\"", index);
        for (size_t position = 0; position < output_count; position++) {
            size_t id = result->vertex_ids[path->first_vertex + position];
            const struct EdgePixel *point = &result->edge_pixels[id];
            char command = position == 0 ? 'M' : 'L';
            fprintf(file, "%c %zu.5 %zu.5 ", command, point->x, point->y);
        }
        if (path->closed) {
            fprintf(file, "Z");
        }
        fprintf(file, "\"/>\n");
    }
    fprintf(file, "  </g>\n</svg>\n");
    success = !ferror(file);
    if (fclose(file) != 0) {
        success = 0;
    }
    return success;
}

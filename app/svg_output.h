#ifndef SVG_OUTPUT_H
#define SVG_OUTPUT_H

#include "edge_paths.h"

/* Return nonzero on success, including a successful final close. */
int pipeline_result_write_svg(const char *filename, const struct PipelineResult *result);

#endif

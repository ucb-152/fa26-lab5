#include "edge_paths.h"

#include <stdlib.h>
#include <string.h>

/* Release all arrays owned by a result and zero the entire structure.
 * In/out: result (struct PipelineResult*), an initialized result or NULL (no operation).
 * Return: void. Stack-owned or borrowed arrays must not be passed as owned fields.
 */
void pipeline_result_free(struct PipelineResult *result)
{
    if (result == NULL) {
        return;
    }
    free(result->edge_pixels);
    free(result->pixel_to_edge);
    free(result->paths);
    free(result->point_ids);
    free(result->vertex_ids);
    memset(result, 0, sizeof(*result));
}


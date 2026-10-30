#ifndef EDGE_PATHS_H
#define EDGE_PATHS_H

#include <stddef.h>
#include <stdint.h>

/* One detected edge pixel: its position, strength, and connections to neighbors.
 * Coordinates are pixel centers; IDs are indices into PipelineResult.edge_pixels. */
struct EdgePixel {
    uint8_t neighbor_mask;       /* Connected neighbors: clockwise bits, east first. */
    size_t x;
    uint16_t strength;
    size_t y;
    uint8_t visited_mask;        /* Connections already used by path tracing. */
    uint8_t degree;              /* Number of connected neighbors. */
};

/* A path through connected edge pixels, before and after simplification.
 * first_point/point_count select a slice of PipelineResult.point_ids;
 * first_vertex/vertex_count select its simplified slice in vertex_ids. */
struct EdgePath {
    size_t first_point;
    size_t point_count;
    uint8_t closed;
    uint8_t retained;            /* Passes the length and strength filters. */
    size_t first_vertex;
    size_t vertex_count;
};

/* Complete pipeline result: detected edge pixels, their connections, and paths.
 * Owns every pointer below. Initialize to {0}, and release with pipeline_result_free.
 * Closed paths repeat their first point at the end, including after simplification.
 * Paths share junction pixels but never share a graph connection.
 */
struct PipelineResult {
    size_t width;
    size_t height;
    struct EdgePixel *edge_pixels;
    size_t edge_pixel_count;
    size_t *pixel_to_edge;      /* SIZE_MAX means that the pixel is not an edge. */
    struct EdgePath *paths;
    size_t path_count;
    size_t *point_ids;            /* Consecutive slices hold the original paths. */
    size_t point_count;
    size_t *vertex_ids;           /* Consecutive slices hold simplified paths. */
    size_t vertex_count;
};

enum {
    EDGE_PATHS_OK = 0,
    EDGE_PATHS_INVALID_ARGUMENT = 1,
    EDGE_PATHS_OUT_OF_MEMORY = 2
};

/* Build the graph from nonzero edge pixels and their original gradient strengths.
 * Diagonals connect only when both intervening orthogonal pixels are absent.
 * The destination must be an empty, zero-initialized PipelineResult.
 */
int edge_graph_build(const uint8_t *edges, const uint16_t *strengths,
                        size_t width, size_t height, struct PipelineResult *result);

/* Split the graph at endpoints and junctions, then extract remaining cycles.
 * Call once after graph construction. An isolated pixel becomes a one-point path.
 */
int edge_paths_trace(struct PipelineResult *result);

/* Measure original paths and apply inclusive length and mean-strength cutoffs.
 * A one-point path has length zero. Repeated closing points count once in means.
 */
int edge_paths_measure(struct PipelineResult *result, double minimum_length,
                          double minimum_mean_strength);

/* Douglas-Peucker simplification using recursive subdivision. Distances are
 * measured to finite segments; equal maximum errors choose the earliest point.
 * At depth 64 (initial call is depth 1), retain all remaining subrange points.
 * Closed paths use three geometric anchors and repeat the starting vertex.
 * Call after measurement; may be repeated. Original points remain available.
 */
int edge_paths_simplify(struct PipelineResult *result, double tolerance);
void pipeline_result_free(struct PipelineResult *result);

#endif

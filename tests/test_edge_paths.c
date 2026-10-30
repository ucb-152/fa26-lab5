#include "pipeline.h"
#include "edge_paths.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

/* Independent definition of a graph connection; does not use neighbor masks. */
static int connected(const uint8_t *edges, size_t width,
                      const struct EdgePixel *a, const struct EdgePixel *b)
{
    double dx = fabs((double)a->x - (double)b->x);
    double dy = fabs((double)a->y - (double)b->y);
    if (dx > 1 || dy > 1 || (dx == 0 && dy == 0)) {
        return 0;
    }
    if (dx == 1 && dy == 1 &&
        (edges[a->y * width + b->x] || edges[b->y * width + a->x])) {
        return 0;
    }
    return 1;
}

/* Prove that tracing partitions graph connections: none missing or duplicated.
 * Check isolated vertices and endpoints as well.
 */
static void check_paths(const uint8_t *edges, const uint16_t *strengths,
                         const struct PipelineResult *result)
{
    size_t count = result->edge_pixel_count;
    uint8_t *used = calloc(count * count + 1, 1);
    uint8_t *seen_vertex = calloc(count + 1, 1);
    CHECK(used != NULL && seen_vertex != NULL);

    for (size_t index = 0; index < result->path_count; index++) {
        const struct EdgePath *path = &result->paths[index];
        CHECK(path->point_count > 0);
        for (size_t position = 0; position < path->point_count; position++) {
            size_t id = result->point_ids[path->first_point + position];
            const struct EdgePixel *point;
            CHECK(id < count);
            point = &result->edge_pixels[id];
            seen_vertex[id] = 1;
            CHECK(point->strength == strengths[point->y * result->width + point->x]);
            if (position != 0) {
                size_t previous_id = result->point_ids[path->first_point + position - 1];
                const struct EdgePixel *previous = &result->edge_pixels[previous_id];
                CHECK(connected(edges, result->width, previous, point));
                CHECK(!used[previous_id * count + id]);
                used[previous_id * count + id] = 1;
                used[id * count + previous_id] = 1;
            }
            if (position > 0 && position + 1 < path->point_count) {
                CHECK(point->degree == 2);
            }
        }
        CHECK(path->retained);
        if (path->closed) {
            CHECK(path->point_count >= 4);
            CHECK(result->point_ids[path->first_point] ==
                  result->point_ids[path->first_point + path->point_count - 1]);
        } else if (path->point_count > 1) {
            CHECK(result->edge_pixels[result->point_ids[path->first_point]].degree != 2);
            CHECK(result->edge_pixels[result->point_ids[path->first_point + path->point_count - 1]].degree != 2);
        }
    }
    for (size_t first = 0; first < count; first++) {
        size_t degree = 0;
        CHECK(seen_vertex[first]);
        CHECK(result->edge_pixels[first].visited_mask == result->edge_pixels[first].neighbor_mask);
        for (size_t second = 0; second < count; second++) {
            int expected = connected(edges, result->width,
                                     &result->edge_pixels[first], &result->edge_pixels[second]);
            CHECK(used[first * count + second] == expected);
            degree += (size_t)expected;
        }
        CHECK(result->edge_pixels[first].degree == degree);
    }
    free(used);
    free(seen_vertex);
}

/* Check the actual error guarantee against the original subpath assigned to
 * each output segment. Checking only the nearest segment would be too weak.
 */
static void check_simplification(const struct PipelineResult *result, double tolerance)
{
    for (size_t index = 0; index < result->path_count; index++) {
        const struct EdgePath *path = &result->paths[index];
        size_t original_position = 0;
        if (!path->retained) {
            CHECK(path->vertex_count == 0);
            continue;
        }
        CHECK(path->vertex_count > 0 && path->vertex_count <= path->point_count);
        CHECK(result->vertex_ids[path->first_vertex] == result->point_ids[path->first_point]);
        if (path->closed) {
            CHECK(path->vertex_count >= 4);
        }
        for (size_t segment = 1; segment < path->vertex_count; segment++) {
            size_t start_id = result->vertex_ids[path->first_vertex + segment - 1];
            size_t end_id = result->vertex_ids[path->first_vertex + segment];
            const struct EdgePixel *a = &result->edge_pixels[start_id];
            const struct EdgePixel *b = &result->edge_pixels[end_id];
            size_t end_position = original_position + 1;
            double dx = (double)b->x - (double)a->x;
            double dy = (double)b->y - (double)a->y;
            double squared_length = dx * dx + dy * dy;
            while (end_position < path->point_count &&
                   result->point_ids[path->first_point + end_position] != end_id) {
                end_position++;
            }
            CHECK(end_position < path->point_count);
            for (size_t position = original_position; position <= end_position; position++) {
                size_t id = result->point_ids[path->first_point + position];
                const struct EdgePixel *point = &result->edge_pixels[id];
                double px = (double)point->x - (double)a->x;
                double py = (double)point->y - (double)a->y;
                double t = squared_length == 0 ? 0 : (px * dx + py * dy) / squared_length;
                double error;
                t = fmax(0.0, fmin(1.0, t));
                error = hypot(px - t * dx, py - t * dy);
                CHECK(error <= tolerance + 1e-9);
            }
            original_position = end_position;
        }
        CHECK(original_position == path->point_count - 1);
    }
}

static void exercise_mask(const uint8_t *edges, size_t width, size_t height)
{
    struct PipelineResult result = {0};
    uint16_t *strengths = malloc(width * height * sizeof(uint16_t));
    const double tolerances[] = {0.0, 0.5, 1.0, 2.5, 100.0};
    CHECK(strengths != NULL);
    for (size_t index = 0; index < width * height; index++) {
        strengths[index] = (uint16_t)(10 + index * 7);
    }
    CHECK(edge_graph_build(edges, strengths, width, height, &result) == EDGE_PATHS_OK);
    CHECK(edge_paths_trace(&result) == EDGE_PATHS_OK);
    CHECK(edge_paths_measure(&result, 0.0, 0.0) == EDGE_PATHS_OK);
    check_paths(edges, strengths, &result);
    for (size_t index = 0; index < sizeof(tolerances) / sizeof(tolerances[0]); index++) {
        CHECK(edge_paths_simplify(&result, tolerances[index]) == EDGE_PATHS_OK);
        check_simplification(&result, tolerances[index]);
    }
    CHECK(edge_paths_measure(&result, 1e6, 0.0) == EDGE_PATHS_OK);
    for (size_t index = 0; index < result.path_count; index++) {
        CHECK(!result.paths[index].retained);
    }
    CHECK(edge_paths_simplify(&result, 1.0) == EDGE_PATHS_OK);
    CHECK(result.vertex_count == 0);
    CHECK(edge_paths_measure(&result, 0.0, 65536.0) == EDGE_PATHS_OK);
    for (size_t index = 0; index < result.path_count; index++) {
        CHECK(!result.paths[index].retained);
    }
    CHECK(edge_paths_simplify(&result, -1.0) == EDGE_PATHS_INVALID_ARGUMENT);
    CHECK(edge_paths_simplify(&result, NAN) == EDGE_PATHS_INVALID_ARGUMENT);
    pipeline_result_free(&result);
    pipeline_result_free(&result);
    free(strengths);
}

static void test_known_shapes(void)
{
    uint8_t edges[49] = {0};
    uint16_t strengths[49];
    struct PipelineResult result = {0};
    for (size_t index = 0; index < 49; index++) {
        strengths[index] = 100;
    }
    for (size_t x = 1; x <= 5; x++) {
        edges[3 * 7 + x] = 255;
    }
    CHECK(edge_graph_build(edges, strengths, 7, 7, &result) == EDGE_PATHS_OK);
    CHECK(edge_paths_trace(&result) == EDGE_PATHS_OK);
    CHECK(result.path_count == 1);
    CHECK(edge_paths_measure(&result, 4.0, 100.0) == EDGE_PATHS_OK);
    CHECK(result.paths[0].retained); /* Inclusive thresholds. */
    CHECK(edge_paths_simplify(&result, 0.0) == EDGE_PATHS_OK);
    CHECK(result.vertex_count == 2);
    CHECK(edge_paths_measure(&result, 4.01, 100.0) == EDGE_PATHS_OK);
    CHECK(!result.paths[0].retained);
    pipeline_result_free(&result);

    /* A 2x2 square is a cycle, with its diagonal shortcuts suppressed. */
    for (size_t index = 0; index < 49; index++) {
        edges[index] = 0;
    }
    edges[0] = edges[1] = edges[7] = edges[8] = 255;
    CHECK(edge_graph_build(edges, strengths, 7, 7, &result) == EDGE_PATHS_OK);
    CHECK(edge_paths_trace(&result) == EDGE_PATHS_OK);
    CHECK(result.path_count == 1 && result.paths[0].closed);
    CHECK(result.paths[0].point_count == 5);
    CHECK(edge_paths_measure(&result, 0.0, 0.0) == EDGE_PATHS_OK);
    CHECK(edge_paths_simplify(&result, 100.0) == EDGE_PATHS_OK);
    CHECK(result.vertex_count == 4);
    CHECK(result.vertex_ids[1] == result.point_ids[1]);
    CHECK(result.vertex_ids[2] == result.point_ids[2]);
    check_simplification(&result, 100.0);
    pipeline_result_free(&result);
    exercise_mask(edges, 7, 7);

    /* T junction: three paths; no spurious diagonal connections. */
    for (size_t index = 0; index < 49; index++) {
        edges[index] = 0;
    }
    for (size_t x = 1; x <= 5; x++) {
        edges[2 * 7 + x] = 255;
    }
    for (size_t y = 3; y <= 5; y++) {
        edges[y * 7 + 3] = 255;
    }
    CHECK(edge_graph_build(edges, strengths, 7, 7, &result) == EDGE_PATHS_OK);
    CHECK(edge_paths_trace(&result) == EDGE_PATHS_OK);
    CHECK(result.path_count == 3);
    pipeline_result_free(&result);
    exercise_mask(edges, 7, 7);
}

/* Check geometric splitting, equality, and deterministic tie-breaking. */
static void test_douglas_peucker(void)
{
    struct EdgePixel edge_pixels[] = {
        {.x = 0, .y = 0}, {.x = 1, .y = 0}, {.x = 2, .y = 0},
        {.x = 2, .y = 1}, {.x = 2, .y = 2}
    };
    size_t points[] = {0, 1, 2, 3, 4};
    struct EdgePath path = {.point_count = 5, .retained = 1};
    struct PipelineResult result = {
        .edge_pixels = edge_pixels, .paths = &path, .path_count = 1,
        .point_ids = points, .point_count = 5
    };
    CHECK(edge_paths_simplify(&result, 1.0) == EDGE_PATHS_OK);
    CHECK(path.vertex_count == 3);
    CHECK(result.vertex_ids[0] == 0);
    CHECK(result.vertex_ids[1] == 2); /* Retain the actual corner. */
    CHECK(result.vertex_ids[2] == 4);
    check_simplification(&result, 1.0);

    /* Equality is accepted: a point exactly one pixel from the segment. */
    edge_pixels[1].y = 1;
    path.point_count = 3;
    CHECK(edge_paths_simplify(&result, 1.0) == EDGE_PATHS_OK);
    CHECK(path.vertex_count == 2);
    check_simplification(&result, 1.0);

    /* Two equally distant points: choose the earlier original position. */
    edge_pixels[2].y = 1;
    edge_pixels[3].x = 3;
    edge_pixels[3].y = 0;
    path.point_count = 4;
    CHECK(edge_paths_simplify(&result, 0.75) == EDGE_PATHS_OK);
    CHECK(path.vertex_count == 3);
    CHECK(result.vertex_ids[1] == 1);
    check_simplification(&result, 0.75);

    /* Collinear backtracking must use finite-segment distance. */
    edge_pixels[1].x = 2;
    edge_pixels[1].y = 0;
    edge_pixels[2].x = 1;
    edge_pixels[2].y = 0;
    path.point_count = 3;
    CHECK(edge_paths_simplify(&result, 0.0) == EDGE_PATHS_OK);
    CHECK(path.vertex_count == 3);
    check_simplification(&result, 0.0);
    free(result.vertex_ids); /* Other arrays are stack-owned test data. */
}

/* Repeated backtracking forces unbalanced splits that reach the depth limit. */
static void test_simplification_depth_limit(void)
{
    enum { POINT_COUNT = 200 };
    struct EdgePixel edge_pixels[POINT_COUNT] = {0};
    size_t points[POINT_COUNT];
    struct EdgePath path = {.point_count = POINT_COUNT, .retained = 1};
    struct PipelineResult result = {
        .edge_pixels = edge_pixels, .paths = &path, .path_count = 1,
        .point_ids = points, .point_count = POINT_COUNT
    };

    for (size_t position = 0; position < POINT_COUNT; position++) {
        edge_pixels[position].x = position % 2 == 0 ? position : 1024 - position;
        points[position] = position;
    }
    /* An uncapped simplifier would remove the two interior points in this tail.
     * The depth-limit fallback must instead keep every remaining point.
     */
    edge_pixels[197].x = 500;
    edge_pixels[198].x = 501;
    edge_pixels[199].x = 502;
    CHECK(edge_paths_simplify(&result, 0.0) == EDGE_PATHS_OK);
    CHECK(path.vertex_count == POINT_COUNT);
    for (size_t position = 0; position < POINT_COUNT; position++) {
        CHECK(result.vertex_ids[position] == position);
    }
    check_simplification(&result, 0.0);
    free(result.vertex_ids);
}

/* A large tolerance still retains the original geometric loop anchors. */
static void test_closed_anchors(void)
{
    struct EdgePixel edge_pixels[100] = {0};
    size_t points[101];
    struct EdgePath path = {.point_count = 101, .retained = 1, .closed = 1};
    struct PipelineResult result = {
        .edge_pixels = edge_pixels, .paths = &path, .path_count = 1,
        .point_ids = points, .point_count = 101
    };
    const size_t expected[] = {0, 25, 50, 0};

    for (size_t step = 0; step < 25; step++) {
        edge_pixels[step].x = step;
        edge_pixels[25 + step].x = 25;
        edge_pixels[25 + step].y = step;
        edge_pixels[50 + step].x = 25 - step;
        edge_pixels[50 + step].y = 25;
        edge_pixels[75 + step].y = 25 - step;
    }
    for (size_t position = 0; position < 100; position++) {
        points[position] = position;
    }
    points[100] = 0;
    CHECK(edge_paths_simplify(&result, 100.0) == EDGE_PATHS_OK);
    CHECK(path.vertex_count == sizeof(expected) / sizeof(expected[0]));
    for (size_t vertex = 0; vertex < path.vertex_count; vertex++) {
        CHECK(result.vertex_ids[vertex] == expected[vertex]);
    }
    check_simplification(&result, 100.0);
    CHECK(edge_paths_simplify(&result, 1e-9) == EDGE_PATHS_OK);
    CHECK(path.vertex_count == 5); /* Four corners and the repeated start. */
    check_simplification(&result, 1e-9);
    free(result.vertex_ids);
}

static void test_pipeline(void)
{
    uint8_t input[81];
    struct PipelineResult result = {0};
    struct PipelineOptions options = {22, 45, 0.0, 0.0, 1.0};
    struct PipelineTimings timings;
    for (size_t index = 0; index < 81; index++) {
        input[index] = (uint8_t)(index * 37);
    }
    CHECK(pipeline_run(input, 9, 9, &options, &result, &timings));
    CHECK(result.edge_pixel_count > 0);
    for (size_t id = 0; id < result.edge_pixel_count; id++) {
        const struct EdgePixel *edge_pixel = &result.edge_pixels[id];
        CHECK(edge_pixel->strength > 0);
    }
    check_simplification(&result, options.tolerance);
    pipeline_result_free(&result);
    CHECK(!pipeline_run(input, 4, 9, &options, &result, &timings));
    options.low_threshold = 46;
    CHECK(!pipeline_run(input, 9, 9, &options, &result, &timings));
}

int main(void)
{
    uint8_t edges[81] = {0};
    uint32_t random_state = 1;
    test_known_shapes();
    test_pipeline();
    test_douglas_peucker();
    test_closed_anchors();
    test_simplification_depth_limit();
    exercise_mask(edges, 9, 9); /* Empty image. */
    edges[0] = 255;
    exercise_mask(edges, 9, 9); /* Isolated pixel at the image border. */
    for (size_t index = 0; index < 81; index++) {
        edges[index] = 255;
    }
    exercise_mask(edges, 9, 9); /* Dense junctions. */
    exercise_mask(edges, 1, 9); /* Thin images supported by graph stages. */
    exercise_mask(edges, 9, 1);
    for (size_t trial = 0; trial < 200; trial++) {
        for (size_t index = 0; index < 81; index++) {
            random_state = random_state * 1664525u + 1013904223u;
            edges[index] = (random_state >> 24) < trial ? 255 : 0;
        }
        exercise_mask(edges, 9, 9);
    }
    puts("Edge-path tests passed: topology, measurements, simplification, and full pipeline.");
    return EXIT_SUCCESS;
}

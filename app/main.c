#include "pipeline.h"
#include "svg_output.h"
#include "pgm.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Append the one supported output suffix. */
static char *svg_filename(const char *prefix)
{
    size_t length = strlen(prefix);
    char *filename;
    if (length > SIZE_MAX - 5) {
        return NULL;
    }
    filename = malloc(length + 5);
    if (filename != NULL) {
        snprintf(filename, length + 5, "%s.svg", prefix);
    }
    return filename;
}

/* Select a common display unit for the entire timing table. */
static const char *time_unit(double seconds, double *scale)
{
    if (seconds >= 1.0) {
        *scale = 1.0;
        return "s";
    }
    if (seconds >= 1e-3) {
        *scale = 1e3;
        return "ms";
    }
    if (seconds >= 1e-6) {
        *scale = 1e6;
        return "us";
    }
    *scale = 1e9;
    return "ns";
}

static void print_benchmark(const struct PipelineTimings *samples, size_t count, double *values)
{
    double largest = 0.0;
    for (size_t i = 0; i < count; i++) {
        largest = fmax(largest, samples[i].total);
        for (size_t stage = 0; stage < PIPELINE_STAGE_COUNT; stage++) {
            largest = fmax(largest, samples[i].seconds[stage]);
        }
    }
    double scale;
    const char *unit = time_unit(largest, &scale);
    char mean_label[32], stdev_label[32], min_label[32], max_label[32];
    snprintf(mean_label, sizeof(mean_label), "%s (%s)", count == 1 ? "Time" : "Mean", unit);

    if (count == 1) {
        printf("%-15s %12s\n", "Stage", mean_label);
        for (size_t stage = 0; stage < PIPELINE_STAGE_COUNT; stage++) {
            double seconds = samples[0].seconds[stage];
            printf("%-15s %12.3f\n", pipeline_stage_names[stage], seconds * scale);
        }
        printf("%-15s %12.3f\n", "total", samples[0].total * scale);
        return;
    }

    snprintf(stdev_label, sizeof(stdev_label), "Stdev (%s)", unit);
    snprintf(min_label, sizeof(min_label), "Min (%s)", unit);
    snprintf(max_label, sizeof(max_label), "Max (%s)", unit);
    printf("%-15s %12s %12s %12s %12s\n",
           "Stage", mean_label, stdev_label, min_label, max_label);
    for (size_t stage = 0; stage <= PIPELINE_STAGE_COUNT; stage++) {
        int is_total = stage == PIPELINE_STAGE_COUNT;
        const char *name = is_total ? "total" : pipeline_stage_names[stage];
        /* Welford accumulation avoids cancellation in the variance. */
        double mean = 0.0;
        double m2 = 0.0;
        double minimum = is_total ? samples[0].total : samples[0].seconds[stage];
        double maximum = minimum;
        for (size_t i = 0; i < count; i++) {
            const struct PipelineTimings *t = &samples[i];
            values[i] = is_total ? t->total : t->seconds[stage];
            minimum = fmin(minimum, values[i]);
            maximum = fmax(maximum, values[i]);
            double delta = values[i] - mean;
            mean += delta / (double)(i + 1);
            m2 += delta * (values[i] - mean);
        }
        /* Population standard deviation across measured runs. */
        double stdev = sqrt(fmax(0.0, m2 / (double)count));
        printf("%-15s %12.3f %12.3f %12.3f %12.3f\n",
               name, mean * scale, stdev * scale, minimum * scale, maximum * scale);
    }
}

int main(int argc, char **argv)
{
    struct PipelineOptions options;
    struct PipelineResult result = {0};
    struct PipelineTimings timings = {0};
    uint8_t *input = NULL;
    char *output_path = NULL;
    size_t width;
    size_t height;
    int status = EXIT_FAILURE;
    size_t runs = 1;
    struct PipelineTimings *samples = NULL;
    double *values = NULL;

    if (argc > 1 && strcmp(argv[1], "--benchmark") == 0) {
        char *end;
        errno = 0;
        unsigned long long count = argc > 2 ? strtoull(argv[2], &end, 10) : 0;
        if (argc <= 2 || argv[2][0] < '0' || argv[2][0] > '9' ||
            errno != 0 || *end != '\0' || count == 0 ||
            count > SIZE_MAX / sizeof(*samples) || count > SIZE_MAX / sizeof(*values)) {
            fprintf(stderr, "--benchmark requires a positive run count.\n");
            return EXIT_FAILURE;
        }
        runs = (size_t)count;
        argc -= 2;
        argv += 2;
    }

    if (!parse_options(argc, argv, &options)) {
        fprintf(stderr, "usage: pgm2svg [--benchmark RUNS] INPUT.pgm OUTPUT_PREFIX [LOW HIGH MIN_LENGTH MIN_STRENGTH TOLERANCE]\n");
        fprintf(stderr, "defaults: LOW=22 HIGH=45 MIN_LENGTH=8 MIN_STRENGTH=0 TOLERANCE=1\n");
        return EXIT_FAILURE;
    }
    if (!pgm_read(argv[1], &input, &width, &height)) {
        fprintf(stderr, "Could not load input PGM image: %s\n", argv[1]);
        goto cleanup;
    }
    /* pgm_read already checks multiplication overflow. */
    if (width < 5 || height < 5) {
        fprintf(stderr, "Image must be at least 5 by 5 pixels.\n");
        goto cleanup;
    }
    output_path = svg_filename(argv[2]);
    if (output_path == NULL) {
        fprintf(stderr, "Could not allocate SVG filename.\n");
        goto cleanup;
    }
    if (strcmp(argv[1], output_path) == 0) {
        fprintf(stderr, "Choose an output prefix distinct from the input filename.\n");
        goto cleanup;
    }
    if (!pipeline_run(input, width, height, &options, &result, &timings)) {
        fprintf(stderr, "Kernel failed: invalid dimensions, allocation failure, or clock error.\n");
        goto cleanup;
    }

    /* The first invocation above is a discarded warmup. Each measured run
     * starts with fresh pipeline allocations; input stays in memory. */
    samples = malloc(runs * sizeof(*samples));
    values = malloc(runs * sizeof(*values));
    if (samples == NULL || values == NULL) {
        fprintf(stderr, "Could not allocate benchmark samples.\n");
        goto cleanup;
    }
    for (size_t i = 0; i < runs; i++) {
        pipeline_result_free(&result);
        if (!pipeline_run(input, width, height, &options, &result, &samples[i])) {
            fprintf(stderr, "Benchmark kernel failed.\n");
            goto cleanup;
        }
    }
    print_benchmark(samples, runs, values);

    if (!pipeline_result_write_svg(output_path, &result)) {
        fprintf(stderr, "Could not save SVG. The output directory must already exist.\n");
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    free(samples);
    free(values);
    pipeline_result_free(&result);
    free(input);
    free(output_path);
    return status;
}

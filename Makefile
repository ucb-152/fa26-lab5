PYTHON ?= python3
CC = gcc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -mcpu=neoverse-v1 -pthread -fopenmp
CPPFLAGS ?= -Iinclude -Iapp
LDLIBS ?= -lm
BUILD ?= bin
WORKLOAD ?= quick
PROFILE ?= 0
RUNS ?= $(if $(filter 1,$(PROFILE)),40,7)
INPUT ?=
NOTE ?=
COMPARE ?=

PIPELINE_SOURCES := $(shell find src -type f -name '*.c' | sort)
PIPELINE_OBJECTS := $(PIPELINE_SOURCES:%.c=$(BUILD)/%.o)
IO_OBJECTS := $(BUILD)/app/pgm.o $(BUILD)/app/svg_output.o

.PHONY: standalone benchmark clean
standalone: $(BUILD)/pgm2svg

$(BUILD)/pgm2svg: $(PIPELINE_OBJECTS) $(IO_OBJECTS) $(BUILD)/app/main.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_pipeline: $(PIPELINE_OBJECTS) $(BUILD)/app/pgm.o $(BUILD)/scripts/test_pipeline.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/%.o: %.c
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

benchmark: export LAB_BENCH_WORKLOAD = $(WORKLOAD)
benchmark: export LAB_BENCH_RUNS = $(RUNS)
benchmark: export LAB_BENCH_INPUT = $(INPUT)
benchmark: export LAB_BENCH_NOTE = $(NOTE)
benchmark: export LAB_BENCH_PROFILE = $(PROFILE)
benchmark: export LAB_BENCH_COMPARE = $(COMPARE)
benchmark: export LAB_BENCH_CC = $(CC)
benchmark: export LAB_BENCH_CFLAGS = $(CFLAGS)
benchmark: export LAB_BENCH_CPPFLAGS = $(CPPFLAGS)
benchmark: export LAB_BENCH_LDFLAGS = $(LDFLAGS)
benchmark: export LAB_BENCH_LDLIBS = $(LDLIBS)
benchmark:
	@$(PYTHON) scripts/benchmark.py

clean:
	rm -rf bin

-include $(PIPELINE_OBJECTS:.o=.d) $(IO_OBJECTS:.o=.d)
-include $(BUILD)/app/main.d $(BUILD)/scripts/test_pipeline.d

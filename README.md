# CS 152 Lab 5

Run `bash scripts/setup.sh` on your Ubuntu Arm instance to install the compiler,
Python with YAML support, and profiling tools.

Before editing, and again after each change:

```sh
make benchmark
```

Each invocation saves the source, builds and tests that copy, checks the complete
pipeline result against saved versions, then benchmarks the same inputs on each
version. The first successful invocation establishes the starter version. Later
runs compare with it and the previous successful version. Run once before editing.

Every attempt is kept under `experiments/001/`, `experiments/002/`, and so on,
including failed builds and correctness checks. Each directory contains `source/`,
`bin/`, `inputs/`, `build.log`, `results.txt`, `results/`, and `experiment.json`.
Copy files from a saved `source/` directory to recover an older implementation.
Experiments are ignored by Git; download any you need before losing instance access.

The default workload preset is `directed`, containing all three required inputs.
Other presets and their image definitions are in `workloads.yaml`. Optional examples:

```sh
make benchmark WORKLOAD=quick
make benchmark WORKLOAD=varied
make benchmark INPUT=examples/valve.pgm
make benchmark RUNS=21 NOTE="reuse row offsets"
make benchmark COMPARE=002,005
```

`INPUT` overrides the preset. Each executable discards a warmup and measures
`RUNS` executions (seven by default). The report gives current stage timings,
total kernel times and variation, and speedups against saved versions. All
versions are rerun on the selected inputs. Failed or interrupted attempts do not
become comparison versions. Run one benchmark at a time.

`COMPARE` adds comma-separated experiment numbers to the starter and previous
comparisons. Each requested experiment must have succeeded. Duplicate versions
are shown once, and extra versions appear as `version-002`, for example. They
are checked for correctness and rerun on the selected inputs before reporting
speedups, just like the default comparisons.

Add `PROFILE=1` to collect APerf profiles after the timing measurements:

```sh
make benchmark PROFILE=1
make benchmark PROFILE=1 WORKLOAD=texture
```

Profiling runs the current saved executable for 40 iterations per input while
APerf records for 10 seconds. It uses the same fixed count for every workload,
independently of `RUNS`. Reports and recording archives are saved under
`experiments/NNN/profiles/INPUT/`; open `report/index.html` from a downloaded
report folder. A profiler failure leaves successful correctness and timing
results available for comparisons.

Run a saved application directly from the repository root, for example:

```sh
mkdir -p output
./experiments/001/bin/pgm2svg examples/valve.pgm output/valve
```

Replace `001` with the experiment number you want to run. This uses that saved
version of the code. To build your current working source without saving an
experiment, use `make standalone` (also the default for bare `make`), then run
`./bin/pgm2svg examples/valve.pgm output/valve`.

Use `make benchmark` to run correctness checks and compare performance.
`make clean` removes the current `bin/` build and leaves experiments alone.
The source snapshot includes the lab source directories, tests, scripts,
examples, Makefile, and workload definitions; keep implementation files in `src/`.

See the course [Benchmarking guide](https://cs152.org/lab/lab5/resources/benchmarking/)
for presets, interpreting results, and using saved versions with APerf.

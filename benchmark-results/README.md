# Benchmark results

This directory contains historical benchmark, profiling, comparison and hardware-acceleration result files.
New benchmark runs write their CSV and JSON outputs here by default.

Run `make bench` or `make bench-hardware` from the project root. To override the location, set
`BENCH_OUTPUT_DIR`; for the regular benchmark, `BENCH_OUTPUT_PREFIX` can select a custom file prefix.

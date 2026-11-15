# HCS benchmark methodology and results

## Historical baseline preservation

`benchmark-results-phase3.csv` is preserved unchanged. It contains 60 rows (12 scenarios × 5 backends), one run per row. Its native rows all report `cache_hit=true` and `native_compile_ms=0`; therefore it is a warm-cache runtime baseline, not evidence of cold compilation cost. Its `feedback-heavy` checksum is zero because that old workload used `x = x AND input` with initial `x=0`, which cannot leave zero.

The older phase2/phase3 CSV files remain historical artifacts. Do not compare their rates directly to phase4 without accounting for circuit-builder changes, iteration counts, warmup state, and sample methodology.

## Phase 4 benchmark design

The harness now provides:

- 20 measured repetitions by default for interpreted and warm-cache modes.
- 3 cold native compilation repetitions by default, because compiling every circuit 20 times is disproportionately expensive; use `--cold-runs` to change this.
- 4 warmup iterations on a separate simulator instance.
- Adaptive iteration calibration to target a minimum sample duration (`--min-sample-ms`, default 20 ms). The selected iteration count is common to every backend for that scenario.
- Median, min, max, mean, sample standard deviation, P95, and median-derived throughput.
- Raw per-sample CSV, summary CSV, and JSON.
- End-of-sample checksum over every public signal; mismatched backends abort the run.
- Separate cold compilation and validated warm-cache modes.
- Process RSS snapshots, native source/shared-library sizes, IR topology metrics, and optional runtime profiling counters.

`steps` means calls to `Simulator::step()`. Each counter iteration performs a high-clock and low-clock step, so it contributes two steps. `operations` is the sum of operation evaluations reported by the backend, not unique source components. `events` is the backend's processed event count. For dense backends, events are zero by definition.

### Reproduce the phase4 data

The committed baseline was generated in a Release build with GCC, Linux x86-64, size parameter 256, minimum 100 iterations, 20 measured repetitions, 3 cold compilation repetitions, 4 warmups, 5 ms adaptive sample target, and both native cache modes:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/hcs_bench \
  --scenario all --size 256 --iterations 100 \
  --runs 20 --cold-runs 3 --warmup 4 --min-sample-ms 5 \
  --compile-mode both --cache .bench-cache-phase4 \
  --output-prefix benchmark-results-phase4-reproduction
```

The command creates:

- `benchmark-results-phase4.csv` — 72 summary rows (12 scenarios × 6 backend modes).
- `benchmark-results-phase4-raw.csv` — all 1,236 measured samples.
- `benchmark-results-phase4.json` — structured summary including profiling/topology metadata.

The six modes are interpreted event, interpreted dense, optimized IR dense, optimized IR event, compiled native cold, and compiled native warm. The warm cache column is true only if every measured warm sample was a validated cache hit. Cold rows are required to be cache misses. Topology columns include signal count, observable signal count, observable operation outputs, forwarding operations introduced, initial/final IR operation counts, dependency edges, SCC/cyclic SCC counts, maximum fan-out, opcode histogram, and per-pass optimizer counts. Because all source signals are observable in the current API, operation reduction is normally zero; this is expected, not a failed DCE pass.

Run a single workload by name, for example:

```bash
./build/hcs_bench --scenario sparse-toggle --size 256 --runs 20
```

The aliases `sparse` (all non-counter scenarios) and `counter` remain available.

### Runtime profiling

Profiling is intentionally separate from throughput runs because per-operation clock reads are intrusive:

```bash
./build/hcs_bench --scenario all --size 256 --iterations 100 \
  --runs 3 --warmup 1 --min-sample-ms 0 --compile-mode warm \
  --profile --output-prefix profiling-results-phase4
```

This produces `profiling-results-phase4.csv`, `profiling-results-phase4-raw.csv`, and `profiling-results-phase4.json`. The event timing breakdown records dequeue/head bookkeeping and gate evaluation separately; it does not yet include a full end-to-end timing of fan-out traversal and queue insertion.

### Results from this environment

The committed phase4 data was collected in the project build container, not on the user's laptop. Representative median throughput (steps/s):

| Scenario | Interpreted event | Interpreted dense | Optimized IR dense | Optimized IR event | Native warm |
|---|---:|---:|---:|---:|---:|
| Small combinational | 5.329 M | 5.250 M | 12.523 M | 11.106 M | 44.065 M |
| Large combinational | 0.299 M | 0.236 M | 1.128 M | 1.501 M | 5.445 M |
| Large sequential | 0.568 M | 0.075 M | 0.247 M | 0.561 M | 0.416 M |
| Sparse toggle | 1.176 M | 0.564 M | 0.988 M | 1.677 M | 5.040 M |
| High switching | 0.452 M | 0.354 M | 0.739 M | 0.633 M | 3.222 M |
| Deep chain | 0.246 M | 0.162 M | 0.489 M | 0.750 M | 4.375 M |
| Wide fan-out | 0.672 M | 0.605 M | 0.985 M | 0.852 M | 4.465 M |
| Repeated expressions | 0.501 M | 0.244 M | 1.010 M | 1.021 M | 2.755 M |
| Redundant logic | 0.276 M | 0.275 M | 0.449 M | 0.833 M | 2.624 M |
| Feedback heavy | 60.067 M | 8.731 M | 13.429 M | 62.971 M | 47.916 M |
| Independent regions | 0.274 M | 0.167 M | 0.817 M | 0.847 M | 4.502 M |
| Binary counter | 2.162 M | 0.314 M | 0.982 M | 2.168 M | 1.583 M |

Interpretation:

- Event execution is strongly favored when only a small subset of operations needs to run, especially counters and stable feedback workloads.
- Dense execution can be competitive on small circuits and high-activity circuits; the best choice depends on switching density and topology.
- Optimized IR dense is faster than original dense in all 12 committed workloads, ranging from roughly 1.5× to 4.9× in this run. This reflects both the IR optimization passes and the direct inline IR evaluator; it is not an isolated measurement of one code change.
- Optimized IR event is close to the original event backend for sequential counters and faster on several combinational workloads, but it is not universally faster.
- Native warm is not universally fastest. It loses to event execution on large sequential, feedback-heavy, and binary-counter workloads because the current native evaluator uses dense passes; it is fastest in 9 of the 12 workloads.

### Cold compilation and break-even

Cold native compiler invocation times in this container ranged from about 41 ms to 380 ms for the tested size. Native compilation was slower than the interpreted event backend on large sequential, feedback-heavy, and binary-counter scenarios, so no finite break-even is reported for those comparisons. For workloads where native warm execution is faster, the estimated cold-preparation break-even ranged from roughly 30k to 256k simulation steps versus interpreted-event (about 101k–627k versus optimized-IR-event). The JSON/CSV contains per-scenario values and the exact formula inputs.

`N_break_even` is calculated as cold total preparation cost divided by the per-step runtime saving versus interpreted-event or optimized-IR-event. It is omitted when the native backend is not faster.

### Profiling and memory limitations

`perf` and Valgrind/Callgrind are not installed in the current container, so hardware counters (cycles, cache misses, branch misses) and call-graph profiling could not be collected. The project includes its own opt-in runtime counters and timing instrumentation instead. These timing subfields are diagnostic and intrusive; do not treat them as hardware-counter-quality profiling.

RSS is read from `/proc/self/status` and is process-wide. In the profiling run it was around 2.9 MiB after setup; this is not a per-circuit or per-IR allocation measurement. Native source files were typically around 2.5–73 KiB and generated shared libraries around 15–36 KiB for these workloads.

## Randomized correctness test controls

The default test suite generates 40 deterministic combinational circuits and 4 deterministic sequential circuits (12 DFFs each), then checks 128 sequential clock cycles per sequential circuit across reference, optimized dense, optimized event and native backends. Failure messages include the seed, generated circuit index, step and signal ID. CI can reduce the workload with:

```bash
HCS_RANDOM_CIRCUITS=8 HCS_RANDOM_SEQUENTIAL_CASES=2 HCS_RANDOM_SEQUENTIAL_STEPS=32 \
  ctest --test-dir build --output-on-failure
```

The defaults remain the extended local test set. Seeds are fixed in the test source so a reported seed/circuit index reproduces the same generated graph and input sequence.

## Benchmark workload construction

- `small-combinational`: 16-stage unary chain.
- `large-combinational`: many independent two-input Boolean operations driven by two changing inputs.
- `binary-counter`: up to 64 DFF bits with carry logic.
- `large-sequential`: up to 512 DFF bits (256 for the committed size parameter).
- `sparse-toggle`: two independent chains; one input toggles while the other stays fixed.
- `high-switching`: many per-lane NOT/BUFFER/NAND operations driven by one toggling input.
- `deep-chain`: long alternating BUFFER/NOT chain.
- `wide-fanout`: many independent BUFFER outputs driven by one input.
- `repeated-expressions`: repeated identical AND expressions plus XOR accumulation; both primary inputs vary.
- `redundant-logic`: constant-false AND branches plus a BUFFER chain.
- `feedback-heavy`: stable self-feedback `x = x OR input`, which transitions from zero to one and remains stable; this is not an oscillation benchmark.
- `independent-regions`: many independent unary operations driven by one input.

All backends receive the same per-step input pattern and fresh initial simulator state for each measured sample. The final checksum covers all public signal IDs. Randomized differential tests separately compare every signal after every step.

## Phase 4.1: calibration validation, feedback activity, and sequential scratch storage

Phase 4.1 keeps the Phase 4 architecture and historical files intact. New output files use the `phase4.1` suffix.

### Backend-aware calibration and sample acceptance

Calibration now probes each backend independently from a fresh simulator. The harness identifies the fastest measured backend and scales one common iteration count so that this fastest backend reaches `--min-sample-ms`. All backend variants then execute the same number of `Simulator::step()` calls and receive the same deterministic input sequence.

After the measured sample matrix is collected, every sample's elapsed duration is checked. If any sample is shorter than the requested target, the common iteration count is increased and the entire scenario's backend sample matrix is repeated with fresh measured simulator instances. This prevents mixing iteration counts between backends. `--max-calibration-iterations` (default 100,000,000) caps the common count. If the cap or retry limit is reached before every sample meets the target, affected samples remain in the output with `duration_target_reached=false` and `accepted=false`; they must not be treated as reliable throughput samples.

The requested target, actual `elapsed_ms`, common calibration iteration count, and acceptance flags are present in summary and raw CSV/JSON. Setup, IR preparation, native compilation, and library loading occur before the timed execution interval. Warmup always uses a different simulator from the measured instance. Cold and warm native modes remain separate.

The Phase 4.1 full suite used size 256, 20 interpreted/warm repetitions, 3 cold repetitions, 4 warmups, and a 1 ms requested minimum sample duration. The target was chosen to complete the full suite in this environment; the actual measured sample durations are recorded individually and were all accepted. A larger target is appropriate for slower hosts or when a longer run is practical.

### Feedback workloads

- `feedback-stable` preserves the old `feedback-heavy` equation `x = x OR set`. Its initial state is zero, the first true input changes it to one, and it remains one. It is a fixed-point/stability workload, not a continuously active switching workload.
- `feedback-dynamic` uses `x = (x AND hold) OR set`, starts from the simulator's defined zero initialization, fixes `hold=0`, and alternates `set` on every step. Thus `x` alternates every step. The benchmark records initial/final state, observed transitions of `x`, total public-signal checksum, and convergence. It rejects a feedback-dynamic sample with fewer than two state transitions.
- A separate `feedback-oscillation` throughput scenario was not added. Oscillation is not ordinary successful throughput; the existing inverter-loop safety tests were extended to verify event-limit detection and profiler reporting instead.

`feedback_state_changes` counts transitions of the designated feedback state, independently of the profiler. This is the activity check that prevents a nonzero aggregate checksum from being mistaken for active feedback. `signal_change_count` is the profiler's count of all actual signal changes. Native generated code does not expose internal profiler counters, so feedback-state transition counting and final checksums are used for native validation as well.

### Sequential step scratch storage

`Simulator` now owns a private `next_states_` byte vector sized for the circuit's DFF count during construction. `step()` reuses it; it resizes only if the DFF count differs. The existing two-phase semantics remain unchanged: calculate every next Q value from the old state, then commit all Q values, then propagate changed Q outputs. The vector is not cleared because every entry is overwritten before it is read. The circuit is immutable through the public API, so the normal path does not resize or allocate in subsequent steps. No reset pin/API exists in the current model; DFF `initial_state` remains supported.

A source-level allocation audit confirms that the per-step temporary vector and its repeated `reserve()` were removed. The container does not provide a reliable per-subsystem allocation profiler, so no exact allocation count is claimed. Controlled 20-run comparisons with the scratch buffer enabled/disabled found a modest, workload-dependent benefit: on the 64-bit binary counter, optimized-IR dense throughput changed from about 1.086 M to 1.148 M steps/s (+5.7%; estimated 2-SE relative spread about 3.9%). On the 256-DFF shift-register workload, optimized-IR dense throughput was effectively unchanged (about 285 k steps/s in both variants; the difference was below the estimated uncertainty). The buffer is retained because it removes a guaranteed allocation/reserve operation from every step without changing semantics, but it is not presented as a universal large-sequential speedup.

### Extended event/runtime profiler

Profiling remains disabled by default. The non-profiled optimized IR dense path uses a separate fast loop so optional timing code does not add per-operation instrumentation branches to the ordinary hot path. Profiling uses no per-event dynamic allocation.

New `ExecutionProfile` counters and timers are exported in summary/raw CSV and JSON:

- Queue: `queue_enqueue_time_ns`, `queue_dequeue_time_ns`, enqueue/dequeue counts, and `queue_max_length`. Enqueue time measures the vector append; dequeue time measures reading/advancing the queue and clearing the queued flag.
- Dependency traversal: `fanout_traversal_time_ns`, `fanout_edges_visited`, and `dependent_operations_found`. Fanout traversal time is exclusive of the nested scheduling and enqueue timings; the edge/operation counts include repeated dependent visits.
- Scheduling: `scheduling_time_ns`, `schedule_attempts`, `duplicate_schedule_attempts`, and `scheduled_operations`. Scheduling time covers the validity/duplicate checks and queue-state update, excluding vector append.
- Signal updates: `signal_update_time_ns`, `signal_write_count`, `signal_change_count`, and `unchanged_signal_write_count`. This covers primary input writes, gate/IR output writes, and DFF Q commits. A write is counted for evaluated outputs even when the value is unchanged; primary inputs are counted only when their value actually changes.
- Operation evaluation: `operation_evaluation_time_ns` and `operation_evaluation_count`. This times only the Boolean evaluator call; signal comparison/assignment is accounted separately.
- Settling: `settle_call_count`, `settle_iteration_count`, `maximum_settle_iterations`, and `oscillation_limit_hits`. Dense iterations are complete passes; event iterations are processed queue events. The maximum is per settle call. A limit hit means a settle returned non-converged because its configured pass/event budget was exhausted.

Timing fields are exclusive where practical: evaluator, signal-update, queue append, queue dequeue, and scheduling intervals do not intentionally include one another. Fanout traversal subtracts the nested scheduling and append intervals from its inclusive loop duration. Timer resolution and profiler instrumentation still make these diagnostic micro-timings, not substitutes for hardware counters. Initial event-queue seeding occurs during simulator construction and is included in queue counters for event backends; benchmark execution time itself excludes construction.

### Phase 4.1 artifacts and reproduction

The full 13-scenario × 6-backend run generated:

- `benchmark-results-phase4.1-summary.csv` — 78 summary rows.
- `benchmark-results-phase4.1-summary-raw.csv` — 1,339 measured samples.
- `benchmark-results-phase4.1-summary.json` — structured summary.
- `profiling-results-phase4.1.csv`, `profiling-results-phase4.1-raw.csv`, and `profiling-results-phase4.1.json` — 3 profile repetitions for each of 13 scenarios and 5 non-cold backends.
- `benchmark-comparison-phase4-phase4.1.csv` — 72 old/new scenario/backend pairs; `feedback-heavy` maps to `feedback-stable`.
- `benchmark-sequential-before-after-phase4.1.csv` — controlled fixed-iteration 20-run comparison for the binary counter and large sequential circuit.
- `benchmark-scratch-buffer-ab-phase4.1.csv` — scratch-enabled versus scratch-disabled comparison on both sequential workloads.
- `profiling-comparison-phase4-phase4.1.csv` — common Phase 4/4.1 profiler counters compared using the final raw sample from each three-run profile batch.

The full run used GCC 14.2.0, Linux x86-64, C++20 Release (`-O3 -DNDEBUG`). The Phase 4 throughput comparison file is directional, not a paired A/B: Phase 4 and Phase 4.1 were collected at different times and used different calibration targets (5 ms and 1 ms respectively). Its uncertainty column is an approximate two-standard-error filter based on sample standard deviation; it is not a formal paired significance test. Do not infer a general regression or speedup from that file alone. The controlled sequential comparison is the more useful direct before/after measurement.

Reproduce the Phase 4.1 throughput run:

```bash
./build/hcs_bench \
  --scenario all --size 256 --iterations 100 \
  --runs 20 --cold-runs 3 --warmup 4 --min-sample-ms 1 \
  --max-calibration-iterations 100000000 --compile-mode both \
  --cache .bench-cache-phase4.1 \
  --output-prefix benchmark-results-phase4.1-summary
```

Reproduce the profiler run (profiling is intentionally not a throughput run):

```bash
./build/hcs_bench \
  --scenario all --size 256 --iterations 100 \
  --runs 3 --cold-runs 1 --warmup 1 --min-sample-ms 0 \
  --compile-mode warm --profile \
  --cache .bench-cache-phase4.1-profile \
  --output-prefix profiling-results-phase4.1
```

`perf` and Valgrind/Callgrind were unavailable for the Phase 4.1 measurements described above. Hardware cycles, cache misses, branch misses, call graphs, and exact allocator counts were not claimed. At the time of Phase 4.1, AVX2/AVX-512/NEON, bit-slicing, multithreading, GPU execution, incremental compilation, and IR redesign were deferred. Phase 5.2 later added experimental AVX2 and multicore paths; see the section below.


## Phase 4.2: profiling-guided audit and controlled experiments

Phase 4.2 preserves all Phase 4/4.1 artifacts and keeps the 13 workload names and six backend configurations unchanged. The full run was generated with GCC 14.2 Release in a 3-vCPU x86-64 KVM environment, with size 256, 5 repetitions for interpreted/warm modes, 3 cold-native repetitions, 1 separate warmup and a 1 ms target. A full 20-run suite did not complete within the execution limit; the completed suite therefore uses 5/3 repetitions, visible in every result row. It contains 78 summary rows and 364 raw samples. Every scenario/backend row is accepted and each scenario has matching step counts and public-signal checksums across backends.

```bash
./build-gcc/hcs_bench --scenario all --size 256 --iterations 100 \
  --runs 5 --cold-runs 3 --warmup 1 --min-sample-ms 1 \
  --max-calibration-iterations 1000000 --compile-mode both \
  --cache .bench-cache-phase4.2-final \
  --output-prefix benchmark-results-phase4.2-summary
```

The full profiling suite was run separately (profiling is not a throughput benchmark):

```bash
./build-gcc/hcs_bench --scenario all --size 256 --iterations 100 \
  --runs 3 --cold-runs 1 --warmup 1 --min-sample-ms 0 \
  --compile-mode both --profile \
  --cache .bench-cache-phase4.2-profile \
  --output-prefix profiling-results-phase4.2
```

This creates 78 profile summary rows and 208 raw samples. Native per-operation counters remain zero because generated native execution is opaque to the current profiler. Profile on/off overhead is available in `profiling-overhead-phase4.2.csv` and `profiling-overhead-phase4.2-raw.csv`; each warm comparison uses 10 measurements per state and each cold comparison uses 3. All pairs use the same 1,000-step sequences and match checksums.

Three isolated candidates were tested and reverted: split more profiled/unprofiled loops; change interpreted-event queue flags from `vector<bool>` to bytes; flatten interpreted fanout to CSR. See `optimization-experiments-phase4.2.csv` and its raw companion. No candidate showed a repeatable gain without regressions. `benchmark-comparison-phase4.1-phase4.2.csv` is explicitly directional and unpaired because Phase 4.1 used 20 runs/4 warmups and a different calibration. Do not treat the percentage column as causal optimization evidence.

An additional 512-DFF run is in `benchmark-results-phase4.2-512dff.csv` and companions. Hardware counters and Callgrind/Cachegrind were unavailable; GCC vectorization diagnostics are discussed in the Phase 4.2 performance report. See [`docs/PHASE4.2_AUDIT.md`](docs/PHASE4.2_AUDIT.md) and [`docs/PERFORMANCE_REPORT_PHASE4.2.md`](docs/PERFORMANCE_REPORT_PHASE4.2.md).


## Phase 4.3: region-dirty hybrid executor

Phase 4.3 preserves all 13 Phase 4.2 scenario names and six backend configurations, adds `optimized-ir-hybrid` as a seventh backend, and adds two targeted scenarios: `region-sparse-chains` and `region-high-activity-chains`. The first has many independent small acyclic regions but toggles only one lane; the second toggles all lanes. Neither replaces any historical workload.

The completed full run was produced with GCC 14.2 Release on Linux x86-64/KVM (3 vCPUs), size 256, two warmups, five measured repetitions for interpreted/IR/warm-native modes, one cold-native repetition, and a 1 ms minimum sample target. It contains 105 summary rows (15 scenarios × 7 modes) and 465 raw samples. All rows were accepted; each scenario has the same step count and final all-public-signal checksum across all seven backends. The full raw and summary exports are `benchmark-results-phase4.3-raw.csv` and `benchmark-results-phase4.3-summary.csv`; JSON is `benchmark-results-phase4.3-summary.json`.

Two targeted size-512 region workloads were measured with 20 repetitions per backend. Their complete sample data is preserved in `benchmark-results-phase4.3-region-sparse-chains-20run-*` and `benchmark-results-phase4.3-region-high-activity-chains-20run-*`. Sparse independent chains showed 34.8% higher throughput for hybrid than optimized IR event (median 1.163 ms vs 1.568 ms). With all lanes active, hybrid was 32.7% slower than optimized IR dense (1.791 ms vs 1.205 ms) and 31.8% slower than native warm (1.791 ms vs 1.221 ms).

The profile-enabled data in `profiling-results-phase4.3.csv`, `profiling-results-phase4.3-raw.csv`, and `profiling-results-phase4.3.json` covers the sparse-region and feedback-stable scenarios. It is used for counts (region schedule attempts, regions processed, operation evaluations, fanout traversals, duplicate suppression and queue peaks), not for unprofiled speed ranking. Existing backend profilers take fine-grained clock samples and distort wall time. Hybrid's counters are count-based; timer fields not specifically instrumented by hybrid remain zero.

For hybrid, `StepResult.operations` counts operation evaluations. `StepResult.events` is backend-specific: one processed acyclic region counts as one event; each processed feedback operation from a local FIFO counts as one event. `max_events_per_settle` bounds this combined work unit. The profile distinguishes `regions_processed`, `region_operation_evaluations`, `maximum_dirty_region_queue`, and `maximum_queue_length` (the local feedback operation queue). Do not compare hybrid `events` directly to a pure event backend as though each unit had identical cost.

`benchmark-comparison-phase4.2-phase4.3.csv` contains 78 matched scenario/backend rows for the six existing modes. It is directional/unpaired: each phase calibrated iteration counts independently, so step counts and checksums differ. Do not treat it as a fixed-workload A/B experiment.

See [`docs/PHASE4.3_RESEARCH.md`](docs/PHASE4.3_RESEARCH.md) for algorithm and limitations and [`docs/PERFORMANCE_REPORT_PHASE4.3.md`](docs/PERFORMANCE_REPORT_PHASE4.3.md) for every scenario's timing and relative throughput. The backend is retained for research only; it is not auto-selected.


## Phase 5.2 hardware acceleration A/B

Build the additional hardware microbenchmark with the normal CMake build (it is included when `HCS_BUILD_BENCHMARKS=ON`) and run:

```bash
./build/hcs_hardware_bench 20
# or
make bench-hardware BENCH_RUNS=20 HCS_BUILD_GUI=OFF
```

The hardware benchmark runs four HybridIR workloads: 256 independent chains, a 4,096-operation same-level gate layer, 16 independent wide DAG regions, and 8 larger independent wide DAG regions. Each is measured in scalar, explicit AVX2, two-total-worker, and AVX2-plus-two-worker configurations (AVX2 configurations are skipped when unavailable). Configuration order is rotated by repetition. Each configuration uses a separate warm-up simulator and measured simulator with the same tick count and deterministic inputs. The harness rejects differing all-signal checksums or operation counts.

It records raw and summary CSV files: median/mean/min/max/stddev/P95 elapsed time, ticks/s, operations and operations/s, simulator setup time, process CPU time from `std::clock()` and the derived process CPU percentage, selected worker count, and actual SIMD batch/operation and parallel batch/region counters. Process CPU percentage is an approximate process-level metric, not per-worker utilization. The benchmark does not report GPU metrics because no GPU backend is implemented. It does not claim exact worker utilization, RSS by accelerator, or hardware PMU counters.

Current Phase 5.2 results were collected with 20 repetitions in a virtualized Intel Xeon Platinum 8573C x86-64 guest exposing 3 logical CPUs under a 2-CPU cgroup quota. All 320 raw samples across the four workloads had identical per-workload checksums and operation counts between configurations. Results did not establish reliable AVX2 or multicore improvements; therefore AVX2 Auto remains scalar and multicore stays Off by default. Raw data, summary distribution and paired scalar comparisons are in `hardware-bench-results-phase5.2-raw.csv`, `hardware-bench-results-phase5.2-summary.csv`, and `hardware-bench-results-phase5.2-paired.csv`. See `docs/SIMD_IMPLEMENTATION.md` and `docs/MULTICORE_EXECUTION.md` for paired per-workload conclusions and limitations.

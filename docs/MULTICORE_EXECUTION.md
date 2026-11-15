# Multicore Execution — Phase 5.2

## Scope

Multicore execution is optional and applies only to `Backend::HybridIR`. It does not change the Interpreted, OptimizedIR Dense/Event, or Compiled Native executors.

A `WorkerPool` is owned by each configured HybridIR `Simulator`. It creates a persistent set of background `std::thread` workers at simulator construction; the caller participates as one additional worker. There is no thread creation per simulation tick. Each dispatched batch uses deterministic contiguous static ranges. A condition-variable generation and completion barrier coordinate dispatch and completion.

`Auto` requests `hardware_concurrency()` capped at 32 total workers. `Custom` accepts 1–64 total workers (including the caller). `Off` is the default. When detailed profiling is enabled the pool is not created, and the selected HybridIR path remains serial; this avoids introducing races or profiler overhead into worker loops.

## Safe scheduling rules

The worker pool is used only when all of these conditions hold:

1. At least `2 * worker_count` regions are queued.
2. The entire queue fits inside `max_events_per_settle`.
3. Every queued region is acyclic.
4. The total operation count of the queued regions is at least 4,096.

If any queued region is cyclic/feedback, if the batch is too small, or if the event budget is insufficient, the original serial HybridIR scheduler is used.

Each region is a weakly connected component of the optimized operation dependency graph. There are no operation producer-to-consumer edges between separate regions. Regions can read shared external inputs, but those inputs are not written during the settle call. Every operation writes its own output signal; no two independent regions write the same output. Thus workers write distinct `uint8_t` elements in the signal array. Per-region `StepResult` and AVX2 counters are also stored in disjoint array entries and merged by the caller after the barrier.

Acyclic work inside one region still runs in its precomputed topological order (with optional same-level AVX2 batches). The implementation does not run dependent operations concurrently within a region.

## Feedback and sequential state

Feedback/cyclic regions are not parallelized. If even one cyclic region is present in the queued batch, the scheduler uses the serial FIFO region path for that batch. This preserves duplicate scheduling suppression, FIFO ordering and the configured event budget.

DFF next-state reads, the `next_states_` buffer, and simultaneous Q commits remain in `Simulator::step()` on the calling thread. Only after every DFF state has been captured and all Q states committed does the existing propagation path run. The worker pool does not parallelize DFF transitions.

## Tests

Differential tests compare every public signal after every step for randomized independent regions. A separate combined SIMD/multicore test creates 16 independent acyclic regions, each containing 256 same-level XOR operations and a reduction tree. Feedback tests verify that the multicore configuration falls back to serial execution and retains the same convergence/output behavior. Custom worker counts 0 and greater than 64 are rejected.

## Performance results

Twenty paired repetitions were collected on the virtualized host described in `HARDWARE_ACCELERATION_AUDIT.md`. Negative values indicate slower throughput than scalar HybridIR. Pair order was rotated.

| Workload | Threads | Paired median throughput change | Wins |
|---|---|---:|---:|
| independent-regions | 2 total workers | −4.79% | 9/20 |
| wide-independent-level | 2 total workers | +7.22% | 12/20 |
| hybrid-wide-independent-regions | 2 total workers | −63.98% | 0/20 |
| hybrid-large-independent-regions | 2 total workers | −17.44% | 8/20 |

`wide-independent-level` contains only one weakly connected operation region, so the parallel path does not run there (`parallel_regions=0`). Its small measured difference is run-to-run variation, not a multicore gain. In the other workloads, parallel-region counters confirmed that tasks were dispatched. On the 16-region workload, median end-to-end time was about 0.430 ms versus 0.148 ms for scalar, and 128 region activations were dispatched per run across eight ticks. The larger 8-region workload also did not show a paired speedup despite its higher work volume. Simulator setup time, including worker creation, is reported separately from timed execution.

The results do not justify automatic multicore activation. The feature remains explicitly opt-in. Static partitioning and the fixed 4,096-operation threshold are experimental and may need re-evaluation on physical systems with different core counts and scheduling costs. The benchmark does not show a reliable speedup from adding threads to these workloads.

## Measurement limits

The harness reports process CPU time via `std::clock()` and the derived process-CPU percentage. This is process-level CPU time, not per-worker utilization. Worker count, parallel batch count, and dispatched region count are recorded, but per-thread busy-time instrumentation is not added because it would perturb short workloads. Hardware PMU counters and cache/branch statistics were unavailable.

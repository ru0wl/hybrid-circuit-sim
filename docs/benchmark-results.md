# Reference benchmark run

These are sample measurements from the container used to build this project, not predictions for another computer. The target machine should be benchmarked locally before choosing defaults.

## Environment

- Linux x86-64
- GCC 14.2.0
- CMake Release build
- Native generated code compiled with `-O2`
- 2,000-gate sparse network; 32-bit synchronous counter

## Results

| Scenario | Backend | Sim steps/s | Operations/s | Setup time | Cache |
|---|---:|---:|---:|---:|---|
| Sparse toggle, 2,000 gates | Interpreted event | 130,285 | 130.3 M | 0.058 ms | n/a |
| Sparse toggle, 2,000 gates | Interpreted dense | 69,487 | 278.1 M | 0.049 ms | n/a |
| Sparse toggle, 2,000 gates | Native compiled, cold | 317,953 | 1,272.5 M | 1,530 ms | miss |
| Sparse toggle, 2,000 gates | Native compiled, warm | 349,352 | 1,398.1 M | 2.95 ms | hit |
| 32-bit counter | Interpreted event | 4,029,140 | 12.1 M | 0.0067 ms | n/a |
| 32-bit counter | Interpreted dense | 685,101 | 89.1 M | 0.0040 ms | n/a |
| 32-bit counter | Native compiled | 2,640,740 | 343.3 M | 80.4 ms | miss |

The sparse benchmark changes one of two independent logic chains per step. It demonstrates the advantage of not reevaluating unaffected logic. The counter benchmark uses 20,000 clock pulses; each pulse is represented by two simulator steps (clock high, then low). Its event-driven path executes far fewer operations than dense/native execution, so event-driven interpretation wins despite having a less optimized per-operation path.

The setup time for a native cache miss includes compiler startup and shared-library compilation. A cache hit only validates and loads the module. The first run's compile time is deliberately reported separately from steady-state simulation throughput.

## Reproduce

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/hcs_bench --scenario sparse --size 2000 --iterations 5000 --cache ./bench-cache
./build/hcs_bench --scenario sparse --size 2000 --iterations 5000 --cache ./bench-cache
./build/hcs_bench --scenario counter --size 32 --iterations 20000 --cache ./bench-cache
```

Measurements vary with CPU frequency scaling, thermal state, compiler version, background load, and cache state. Use multiple runs and compare medians on the actual target system. The current benchmark set is intentionally small; it is a scaffold for adding large sequential, feedback-heavy, high-switching, and hierarchical workloads.

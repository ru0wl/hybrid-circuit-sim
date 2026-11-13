# SIMD and bit-parallel feasibility — phase 4

## Current evidence

HCS currently models a single circuit instance with one-byte Boolean signal storage (`uint8_t` per signal), scalar operation IDs, and arbitrary dependencies. The committed benchmark suite includes independent regions and wide fan-out, but these are still operations inside one circuit instance. There is no batch-of-circuits API and no vector-lane abstraction.

## Bit-sliced execution

Bit-slicing multiple independent circuit instances into one machine word is the most promising future direction. A 64-bit word could represent one logical signal across 64 independent lanes, with AND/OR/XOR/NOT mapping naturally to word operations. This requires:

- A batch execution API and independent state per lane.
- Identical circuit topology or compatible operation schedules across lanes.
- Lane-aware input updates and output extraction.
- Correct per-lane DFF edge/state handling.
- A clear policy for lanes that converge or change at different times.

None of these contracts exists in the current public simulator, so implementing bit-slicing now would require a separate batch runtime rather than a local optimization of the current executor.

## AVX2 within one circuit

A possible single-circuit strategy is to group independent operations by opcode and topological level, then evaluate batches of byte-wide Boolean values. The independent-regions and wide-fanout workloads show available independent work, but current operation inputs are signal-ID gathers and outputs are signal-ID stores. AVX2 does not provide cheap arbitrary byte gathers; packing/unpacking and grouping may erase the arithmetic gain. Cyclic SCCs and sequential boundaries also limit batching.

A useful prototype would need to measure:

1. Number of independent operations per level and per opcode.
2. Cost of gathering operands and scattering outputs.
3. Batch width utilization and scalar tail frequency.
4. Extra scheduling/metadata memory.
5. Full-step throughput, not isolated gate throughput.

## Recommendation

Do not add AVX2 now. First add a batch-circuit API or a level/opcode execution plan and collect independent-operation histograms. Keep the scalar implementation as the mandatory fallback. AVX-512 and NEON should remain future backends behind runtime feature detection. Hand-written assembly is not justified by current measurements.

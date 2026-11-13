# Simulation semantics (prototype v0.1)

This document defines the behavior implemented by the initial simulator. It is deliberately narrower than a full hardware-description language.

## Signal model

- A signal is a one-bit value represented by `uint8_t` and normalized to `0` or `1`.
- Every signal has at most one driver. Primary inputs are externally driven and cannot also be gate outputs.
- All signals start at `0`, except D-flip-flop outputs with an explicitly supplied initial state.
- The initial implementation has no `X`, `Z`, strength, tri-state, or multi-driver resolution.

## Combinational gates

The supported operations are constants, buffer, NOT, AND, OR, XOR, NAND, NOR, and XNOR. Gate evaluation is deterministic and side-effect free. At circuit build time, the builder creates a stable topological execution order for acyclic dependencies (ties are ordered by gate ID). Gates belonging to unresolved cyclic dependencies are appended in stable gate-ID order and are settled iteratively by the dense interpreter and generated native function.

A settle operation repeats a complete dense pass until no output changes or `max_dense_passes` is reached. Reaching the limit reports `converged=false`; it does not silently claim a stable circuit. The event-driven interpreter uses a FIFO work queue, suppresses duplicate queued gate IDs, and reports `converged=false` if its per-settle event budget is exhausted.

This is a bounded fixed-point model, not a full multi-driver HDL event kernel. A combinational loop may converge, depend on its initial state, or fail to converge. Oscillation detection is currently limit-based rather than cycle-state based.

## Sequential logic

The only sequential primitive in v0.1 is a positive-edge-triggered D flip-flop.

One simulator `step()` does the following:

1. Settle combinational logic using current inputs and current flip-flop outputs.
2. Detect rising edges (`previous_clock == 0` and `clock == 1`) for every flip-flop.
3. Sample all rising-edge D inputs from the same settled signal snapshot.
4. Apply all sampled Q updates simultaneously.
5. If any Q changed, settle combinational logic again.

The previous clock level is updated on every step, including steps without a rising edge. Therefore a caller must run a step while the clock is low before the next high step can create another rising edge. There are no propagation delays, asynchronous reset/set pins, latches, or multiple clock domains with specialized scheduling yet.

## Backend equivalence

- Interpreted dense: repeated scalar passes over every combinational gate.
- Interpreted event-driven: FIFO scheduling of gates whose input signals changed.
- Compiled native: generated C++ shared object that evaluates the same combinational gate list in dense passes; flip-flop edge/state handling remains in the common simulator core.

The native backend is not a separate semantic implementation of sequential elements. It shares the exact edge-detection and simultaneous-state-update code with interpreted mode. The tests compare all three backends against the scalar dense reference for deterministic input sequences.

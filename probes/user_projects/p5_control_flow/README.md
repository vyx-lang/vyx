# p5_control_flow

Intent: class locals that live across a non-trivial CFG.

- `compute`: an `Acc` created before an if/else, mutated in both arms and in a
  while loop, then read after the join and through an early-return branch.
- `accumulate_in_loop`: a persistent class local plus a fresh per-iteration
  class local, to check the storage planner keeps them in distinct slots.

Expected exit 0 / `control_flow ok`. Wrong sums mean multi-block class storage
is aliasing or dropping updates.

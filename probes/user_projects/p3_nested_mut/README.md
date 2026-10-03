# p3_nested_mut

Intent: nested field mutation on value-type classes.

- `nested_writeback.vyx`: the supported idiom — copy the nested field into a
  standalone local `var`, mutate via methods, write it back. Expected exit 0.
- `nested_direct.vyx`: the first thing a real user tries,
  `squad.leader.damage(20)`. Acceptable outcomes are (a) in-place mutation or
  (b) a clear compile-time diagnostic. A silent no-op (compiles, runs, field
  unchanged) is a miscompile. We record which of the three happens.

# p1_generic_infer

Intent: a real user calls generic functions **without turbofish**
(`identity(11)`, `pick_first(7, 100i64)`), nests inference
(`identity(identity(5))`), and calls a method on a generic type where the
argument type is fixed by the receiver's instantiation.

Expected: exit 0, prints `generic_infer ok`. Any non-zero return code points
at a specific inference site (see the return value).

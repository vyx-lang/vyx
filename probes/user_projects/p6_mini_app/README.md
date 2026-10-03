# p6_mini_app

Intent: a real multi-file project, built with `--src=project`.

- `src/inventory.vyx` (`module app.inventory`): data model — `Item` and
  `Inventory`, backed by a `Vec<Item>` (Vec of class values). `restock` does a
  `get` -> mutate -> `set` read-modify-write on an element.
- `src/main.vyx` (`module app`): builds an inventory, loops to total value,
  restocks, formats with f-strings, and returns an exit code contract.

Expected: exit 0, prints totals `1000` then `1400` then `mini_app ok`.

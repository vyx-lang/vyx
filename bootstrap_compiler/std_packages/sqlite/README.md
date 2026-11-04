# SQLite

Add the explicit source package to a project:

```toml
[dependencies]
sqlite = { path = "std:sqlite", target = "std_sqlite" }
```

```vyx
use std.sqlite;

fn main() -> i32 {
    let db = Database.try_open("app.db").unwrap();
    db.try_exec("CREATE TABLE IF NOT EXISTS events (id INTEGER, text TEXT)").unwrap();
    let insert = db.try_query("INSERT INTO events VALUES (?1, ?2)").unwrap();
    insert.bindInt(1, 42);
    insert.bindText(2, "ready");
    insert.try_next().unwrap(); // false means SQLITE_DONE, not failure.
    let rows = db.query("SELECT id FROM events");
    while (rows.next()) { print(rows.getInt(0)); }
    return 0;
}
```

Install SQLite 3.8.7 or newer separately. The package propagates `sqlite3` to
the consumer's link command; it does not bundle the SQLite library. Linux
builds need `libsqlite3-dev`; macOS uses the system library. On Windows use
`vcpkg install sqlite3:x64-windows` and set `VCPKG_ROOT` to that installation.
Deploy its `sqlite3.dll` with the application. Android needs a SQLite library
and development/link environment appropriate to the target.

`Database.try_open`, `try_exec`, `try_query` and the cursor's `try_*` methods
return `Result<value, SQLiteError>`. The error contains a native result code
and owned message. Wrapper precondition failures use `SQLITE_MISUSE`,
`SQLITE_RANGE`, `SQLITE_MISMATCH` or `SQLITE_TOOBIG` as appropriate.
`Database.lastError()` replaces the former, unparseable `error()` method.
Convenience `open`, `query`, `next`, getters and binders unwrap their result;
use `try_*` when errors need recovery. Legacy `exec` returns a success bool.

`try_query` accepts exactly one statement, followed by optional whitespace or
comments. Empty input, malformed SQL and a second statement are errors.
Use `try_exec` for batches and explicit `BEGIN`/`COMMIT`/`ROLLBACK` transactions.
Tail checking follows SQLite's [comment rules](https://www.sqlite.org/lang_comment.html)
without preparing a second statement: some [PRAGMAs act during preparation](https://www.sqlite.org/pragma.html),
so preparing a rejected tail could otherwise change connection settings.
Paths and SQL containing NUL are rejected. Bounded string views are copied
and terminated before being passed to SQLite.

Text and BLOB binding copy their input into SQLite-owned storage. The caller
may change or destroy its storage immediately after binding; the native typed
destructor releases the copy even when binding fails. Text lengths preserve
embedded NUL; SQL string operations on such text follow SQLite's own rules.
Empty BLOB and SQL NULL are distinct. Integer binding supports all i64 values.

Read columns only after a successful `try_next() == Ok(true)`. Getters validate
row state, index and native column type. `getInt` rejects i64 values outside the
i32 range; `getInt64` preserves them. `getDouble` accepts integer or real columns.
NULL is inspected with `isNull` and is not silently converted into an empty
string or zero. Text and BLOB getters return independent copies that survive
stepping, reset and close. Returned primitive `string` values follow the
[standard string cleanup contract](../../../docs/STD_LIBRARY.md); use `str_drop`
when the caller owns such a result. Vec and resource handles have scope cleanup.

`reset` preserves bindings; `try_clear_bindings` explicitly removes them.
Exhausted or failed cursors never restart implicitly. Reset takes effect even
when SQLite returns the previous execution error. `Query.close()` returns that
last native status and frees the statement even on error; repeated close is 0.
Database close uses `sqlite3_close_v2`: the public database becomes closed while
outstanding statements keep the native connection alive until finalization.
Both classes close on scope exit and do not implement Clone. Do not use a single
wrapper concurrently without external synchronization.

The real-library consumer is
[`std_sqlite_contracts`](../../../tests/projects/std_sqlite_contracts/src/main.vyx).
It covers binding lifetimes, errors, cursor reuse, typed reads, NUL/BLOB data,
deferred connection destruction, native allocation balance and persisted
transactions at O0/O2 through the standard-library gate.
The allocation-balance check uses SQLite's own memory statistics; a native build
that disables those statistics cannot use this counter to establish cleanup.

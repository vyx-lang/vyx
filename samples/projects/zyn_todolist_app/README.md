# Task Deck recording and reply test

From this directory, run `./test_timeline.ps1`. The script builds Zyn and
the app through the SDK, runs `--self-test`, and then starts the app with
`--reply target/todo_self_test.zyns --headless --repeated 3`. Use
`./test_timeline.ps1 -Visual` to also exercise windowed reply. Use
`-SkipBuild` when the app and Zyn runtime have already been rebuilt.

The self-test records text input, a custom `AddTaskCommand`, and a
`tasks.toggle.*` behavior. It checks the resulting task list, action order,
export/load round trip, three fresh headless replays, and rejection of a
different initial state. The exported timeline is
`target/todo_self_test.zyns`.

For manual interaction recording, run:

```text
target\zyn_todolist_app.exe --recording -o target\todo_manual.zyns
target\zyn_todolist_app.exe --reply target\todo_manual.zyns
target\zyn_todolist_app.exe --reply target\todo_manual.zyns --headless --repeated 1000
```

The first command opens the app until the window closes. Windowed reply
renders the recorded command sequence; headless reply checks state without
creating a window.

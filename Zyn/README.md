# Zyn

Zyn is the Vyx application framework. Platform wraps SDL3, Nut renders through
Cacao, and App connects views, state, actions, and the event loop. The project
manifest is `Vyx.toml`; application examples are under `../samples/projects/`.

The [view composition guide](docs/COMPOSITION_ZH.md) explains sizing,
conditional children, surfaces, and stable list identities.

The 2026-10-01 regression used the self-hosted SDK compiler for a full
Windows AOT build of Zyn (`-j4 -O0`, 177 tasks including root chunks), then
built `zyn_recomposition_smoke` and ran `--self-test` with exit code `0`.
That gate checks layout and action/state contracts without opening a window.
It does not establish screenshot parity with another framework or complete
font fallback, multiline text layout, or arbitrary list row builders.

## Current limits

- Resource lists render fixed-height checkbox rows. `DataGrid` composes text
  and layout controls; it does not provide editable cells, sorting, or a
  variable-height row builder.
- Text sizing uses theme-based height estimates. Wrapped paragraph measurement
  and complete font fallback are not established by the layout self-test.
- Semantic nodes and bridge hooks do not establish native screen-reader
  support. Keyboard, nested gestures, and modal focus need application-level
  interaction checks.
- `.zyns` v1 records commands and resource snapshots. It does not automatically
  reproduce external I/O, clock values, randomness, or asynchronous completion
  order. Applications must control those inputs for repeatable replay.
- Effect request, cancellation, and timeout APIs need real service adapters;
  their existence does not verify a file, network, or database integration.
- Native window-handle support and publishing target names do not establish
  SDK or device validation. See the [platform guide](docs/CACAO_PLATFORM_ZH.md)
  for backend requirements and the tested scope.

## Create an application

From the repository root in PowerShell:

```powershell
$zyn = (Resolve-Path ./Zyn/bin/zyn.ps1).Path
New-Item -ItemType Directory hello | Out-Null
Set-Location hello
& $zyn new hello .
& $zyn run -- --visible
```

`zyn new` creates `Vyx.toml` and `src/main.vyx`. Change the package name,
application name and identifier in `Vyx.toml`; edit `src/main.vyx` for the
view and application behavior. `& $zyn build` builds without starting a
window. The SDK entry point uses its local Vyx compiler and stages the native
libraries and shaders beside the executable.

Zyn lays out views in display-independent units. SDL's window display scale
maps those units to framebuffer pixels; mouse and normalized touch events are
mapped back before hit testing. A 56-unit control therefore keeps the same
physical size across screens with different pixel densities. The viewport can
still change with the available window size, so flexible layouts remain useful.

For Android, set `ANDROID_HOME` and `ANDROID_NDK_HOME` to the installed SDK
and NDK, then run from the application directory:

```powershell
& $zyn init-android --language java
& $zyn build --target android-x64 -j4
& $zyn publish --target android-x64 --debug
adb install -r dist/android-x64/hello-android-x64.apk
```

Use `--language kotlin` for a Kotlin activity and `android-arm64` for a
physical arm64 device. The Android project lives under `android/`; `zyn publish`
packages the Vyx library, Zyn, Cacao, SDL3, text libraries and
application assets into the APK. Building from a clean checkout first requires
the [Android dependency builds](tools/android-deps/README.md) and the
[matching Cacao SDK](docs/CACAO_PLATFORM_ZH.md); their generated `.so` files
are not tracked by Git.

Place shaders and other application files under the project `assets/` directory
and refer to them as `assets/...` in Vyx code. Desktop publishing copies that
directory beside the executable. Android publishing bundles it in the APK, and
the generated Activity installs it under the same relative path before native
startup. Existing generated Android projects need the current `ZynActivity.java`
template to use project assets.

The APK contains `Zyn.runtime.toml` with only the package name/version and
`[zyn.app]` name, ID, version, organization and window size. The build manifest
`Vyx.toml` stays in the source project. For an Android runtime permission,
declare it in `android/app/src/main/AndroidManifest.xml`, call
`AndroidPermission.request("android.permission.CAMERA")` after the app starts,
then poll `AndroidPermission.status(id)` (`-1` pending, `0` denied, `1` granted).
Call `AndroidPermission.release(id)` after a final result. The permission API
is in `Zyn.Platform.Platform`.

## Application actions

A view names an action. The application registers the behavior that handles it
in `Component.configure`. A behavior receives the complete `AppAction` and the
current `AppWorld`, then returns the next world. It can validate input, update
several resources, and call application services; it is ordinary application
code, not a special command kind in Zyn.

```vyx
use AppAction = Zyn.App.AppAction;
use AppWorld = Zyn.App.AppWorld;

fn save(action: AppAction, world: AppWorld) -> AppWorld {
    let name = world.stringResource("editor.name", "").trim();
    if (name.len == 0) {
        return world.setStringResource("editor.error", "A name is required.");
    }
    return world.setStringResource("editor.saved", name)
        .setStringResource("editor.error", "");
}

// Inside a component's configure(world: AppWorld) method:
// return world.registerBehavior("editor.save", save);
// A button created with Button.filledAction("save.button", "Save", "editor.save")
// will trigger save after its interaction is processed.
```

`registerBehavior` matches an exact action name. `registerBehaviorPrefix`
matches a family of names such as `tasks.toggle.0`; an exact match wins over
prefixes, and the longest matching prefix wins among prefixes. A registered
behavior takes precedence over a built-in resource command with the same name.
Built-in commands remain shortcuts for simple resource updates.

For a behavior with its own configuration, implement `AppCommandHandler` in an
application class. `matches` selects actions, `execute` returns the next world,
and `dup` creates an independent copy for scene and runtime snapshots. Register
an instance with `world.registerCommandHandler(Box::<dyn AppCommandHandler>.new(command))`.
Command objects are checked in reverse registration order, then function
behaviors, then built-in resource commands. Command configuration is immutable;
mutable application state belongs in `AppWorld`.

## Dynamic views

`App.run(component)` and `App.run(components)` keep the component available
throughout the render loop. After a command changes `AppWorld`, Zyn calls
`Component.view(world)` again and binds actions on the new tree before drawing
the next frame. A structural choice can therefore come directly from state:

```vyx
// In Component.configure:
// world.registerBoolToggleCommand("details.toggle", "details.open")

// In Component.view:
return Disclosure.create("details", "Details", "details.open",
    "details.toggle", world, Text.body("More information"));
```

Views are rebuilt when state changes, while clock-only resources such as
`ui.frame.index` and caret blink phase do not trigger a structural rebuild.
For application-owned 2D drawing, implement `Zyn.UI.CustomView` and pass it to
`CanvasView.create(id, Box::<dyn CustomView>.new(custom_view))`. It receives
the laid-out bounds and a Nut `SceneBuilder`; its output is clipped to those
bounds. Use the usual `.onPointerDown(...)`, `.onPointerDrag(...)`, and
`.accessibility(...)` bindings. See [Custom views](docs/CUSTOM_VIEW_ZH.md) and
the [headless drawing/interaction smoke](../samples/projects/zyn_canvas_view_smoke/src/main.vyx).
`CustomView.paint` is the 2D path. For GPU content, implement
`Zyn.Nut.CustomRender.CustomRenderHandler`, register it through
`AppSpec.setRendererSetup`, and place a
[`CustomRenderView`](docs/CUSTOM_RENDER_VIEW_ZH.md) in the View tree. Nut
composites its Cacao output with the UI. The
[`zyn_custom_render_view_smoke`](../samples/projects/zyn_custom_render_view_smoke/src/main.vyx)
sample draws two GPU triangles alongside a button.
Run `Zyn/tests/android/Run.ps1 -CustomRender -DeviceSerial emulator-5554` from
the repository root to check the Android Vulkan rendering and pause/resume path.
`FormField`, `SearchBar`, `Disclosure`, `EmptyState`, `ChoiceGroup`, and
`DataGrid` compose the existing text, input, button, and layout primitives.
The application registers the actions they emit. The
[`zyn_recomposition_smoke`](../samples/projects/zyn_recomposition_smoke/src/main.vyx)
project exercises these controls and the timeline API.

Surfaces also compose visual depth without backend-specific code. Call
`View.elevation(logicalPixels)` on a panel or card; the value is carried into
the retained UI tree and rendered as a clipped, soft drop shadow. The MD3
surface helpers use this path by default (`Md3Surface.card`, `filledCard`, and
`primaryContainer`), while `.noStroke()` and `.backgroundRole(...)` remain
available for flat surfaces. Layout, interaction, accessibility, and visual
style stay in one declarative view value.

`View.childIf(condition, child)` is the composable conditional primitive. It
keeps the same tree ownership, action binding, hit testing, and accessibility
rules as `child`, so a state-driven branch does not need a separate imperative
mount path. Explicit `.gap(0.0)` and `.padding(0.0)` are preserved; omitted
values continue to use the theme defaults.

Nested layout containers measure their content when no positive height is
supplied, including rows containing compound controls. A column or panel sums
visible child heights and gaps; a row uses the largest measured child height.
Use `.height(...)` for a fixed height and `.fillHeight()` to consume the
remaining parent height. Text leaves still use theme-based height estimates;
this sizing path does not yet measure wrapped paragraphs from shaped glyphs.

Keyed lists use `.keyResource(...)` consistently for retained nodes, actions,
focus, and accessibility, so inserting or reordering rows preserves their
identities. Supply a unique, nonempty key for every row, aligned with its label
and checked-state resources. A missing or empty key falls back to its index.
The current list renders fixed-height checkbox rows; custom row builders and
variable-height virtualization remain future work.

`StatelessComponent` and `StatefulComponent` use the same `configure`, `view`,
and action pipeline. A stateful component gives its `AppWorld` resources a
stable prefix with `stateResource(name)`. Its `update(event, world)` receives
platform and frame events during the normal component run loop; returning a
changed world triggers a view rebuild for watched state. Store application
state in `AppWorld` so `view` and registered behaviors read the same values.

The Android interaction gate runs the same touch and permission checks for
both component types. Add `-StatefulInteraction` to
`Zyn/tests/android/Run.ps1 -Interaction -DeviceSerial emulator-5554` to run
the stateful case; it also checks frame and touch delivery to `update`.

## Record, export, and reply

Recording is an application policy. Enable it in code with
`App.manifestFromArgs().recordTo("timeline.zyns").run(component)` or
`automaticRecording()` for the default path. `withoutRecording()` turns an
application default off. Code can also choose
`App.manifestFromArgs().replyFrom("timeline.zyns", 3).run(component)` and append
`.headlessReply()` when no window is wanted. CLI flags override the application choice:

```text
zynApp --recording -o timeline.zyns
zynApp --no-recording
zynApp --reply timeline.zyns --repeated 3
zynApp --reply timeline.zyns --headless --repeated 1000
```

For apps started through the `*FromArgs` profile helpers, `--recording`
keeps the window open until it closes. An application can still choose a
fixed frame count in its own profile.

`--reply` opens a window by default and renders the recorded action sequence
through the component/view pipeline. `--headless` selects the faster
state-checking path, which needs no SDL, window, or renderer. Each repetition
starts from a fresh configured runtime. The `.zyns` v1 file contains the initial application
resource state, the frame count, and a sequenced list of executed actions, including their
source, payload, execution frame, action frame, input state before execution, and state after
execution. `AppTimeline.load(path)`, `AppTimeline.exportTo(path)`, and
`AppRuntime.replyTimeline(timeline, repetitions)` expose the same operations
to application code. Export failures and state mismatches return errors.

The command queue determines execution order, so a future parallel producer
must submit through that queue. Both reply modes re-execute registered commands
and check each resulting resource state. The visual mode renders each frame and
uses the recorded execution frame as its schedule. It does not replay physical
pointer hit tests, external I/O,
network responses, random values, or arbitrary function-pointer captures;
applications must inject deterministic inputs for those effects. The reserved
`ui.*` and view-binding resources are excluded from state comparison.

`AppCommand` is the built-in resource-command value type; applications extend
`AppCommandHandler` rather than adding a new `command_kind`. See
`../samples/projects/zyn_todolist_app/src/main.vyx` for `AddTaskCommand`, an
application-defined command class with resource configuration, plus a function
behavior for indexed toggle actions. Its
[`test_timeline.ps1`](../samples/projects/zyn_todolist_app/test_timeline.ps1)
checks recording, export, and headless reply; `-Visual` also runs windowed
reply.

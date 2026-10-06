"""Compile and run generated member bridges with real Qt context destruction."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qt", type=Path, required=True)
    parser.add_argument("--result", type=Path, required=True)
    args = parser.parse_args()
    probe = Path(__file__).resolve().parent
    repo = probe.parents[2]
    output = args.result.resolve().parent / "member-bridge"
    output.mkdir(parents=True, exist_ok=True)
    source = output / "lifetime.cpp"
    generated = probe / ".cache/dci/qt_widgets"
    report = json.loads((generated / "report.json").read_text())
    matches = [b for b in report["bridges"] if b["owner"] == "QAbstractButton" and b["member"] == "on_clicked"]
    assert len(matches) == 1, matches
    native_name = matches[0]["symbol"]
    source.write_text('''#include "producer.hpp"
#include <cassert>
#include <iostream>
struct State { int calls = 0; bool checked = false; };
void callback(void* p, bool checked) { auto& s = *static_cast<State*>(p); ++s.calls; s.checked = checked; }
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QPushButton sender;
    sender.setCheckable(true);
    State state;
    assert(!dci_connect_clicked(nullptr, &sender, callback, &state));
    assert(!dci_connect_clicked(&sender, nullptr, callback, &state));
    assert(!dci_connect_clicked(&sender, &sender, nullptr, &state));
    for (int i = 0; i < 1000; ++i) {
        auto* context = new QObject;
        assert(dci_connect_clicked(&sender, context, callback, &state));
        int before = state.calls;
        sender.click();
        assert(state.calls == before + 1);
        assert(state.checked == sender.isChecked());
        delete context;
        sender.click();
        assert(state.calls == before + 1);
    }
    assert(state.calls == 1000);
    // The sender can also die first; destroying its context remains harmless.
    QObject context;
    { QPushButton child; assert(dci_connect_clicked(&child, &context, callback, &state)); child.click(); }
    assert(state.calls == 1001);
    std::cout << "member bridge OK: typed bool payload, 1000 context disconnects, sender-first destruction\\n";
}
'''.replace("dci_connect_clicked", native_name), encoding="utf-8")
    commands, checks = [], []
    flags = ["-std=c++17", "-fexceptions", "-fcxx-exceptions", "-fms-runtime-lib=static",
             "-DQT_CORE_LIB", "-DQT_GUI_LIB", "-DQT_WIDGETS_LIB", "-DUNICODE", "-D_UNICODE",
             "-I", generated, "-I", probe / "native", "-I", args.qt / "include"]
    windows = Path(os.environ.get("WINDIR", "C:/Windows"))
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("QT_")}
    env.update(PATH=os.pathsep.join(map(str, (windows / "System32", windows))), QT_QPA_PLATFORM="offscreen")
    for level in ("O0", "O2"):
        exe = probe / "target" / ("member_bridge_" + level + ".exe")
        command = [repo / "clang/bin/clang++.exe", source, "-" + level, *flags,
                   *(args.qt / "lib" / (name + ".lib") for name in ("Qt6Widgets", "Qt6Gui", "Qt6Core")), "-o", exe]
        for name, argv in (("compile", command), ("run", [exe])):
            argv = list(map(str, argv))
            commands.append(argv)
            result = subprocess.run(argv, env=env if name == "run" else None, capture_output=True,
                                    text=True, errors="replace", timeout=90)
            (output / (level + "." + name + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
            assert result.returncode == 0, (argv, result.returncode, result.stdout, result.stderr)
        checks.append(level + ":1000-context-disconnects-and-sender-destruction")
    args.result.write_text(json.dumps({"status": "passed", "checks": checks, "commands": commands,
        "header_sha256": hashlib.sha256((generated / "producer.hpp").read_bytes()).hexdigest()}, indent=2) + "\n")
    print("Qt typed member bridge lifetime OK: O0/O2")


if __name__ == "__main__":
    main()

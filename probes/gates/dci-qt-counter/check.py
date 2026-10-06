"""AOT Qt event-loop check and negative DCI pointer/type binding checks."""
import argparse
import hashlib
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    for name in ("compiler", "program", "contract", "qt", "result"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    compiler, program, contract, qt, result = (
        getattr(args, name).resolve() for name in ("compiler", "program", "contract", "qt", "result"))
    result.parent.mkdir(parents=True, exist_ok=True)
    checks = []
    pointer_source = Path(__file__).resolve().parent / "checks/pointer_storage.vyx"
    completed = subprocess.run([str(compiler), "--src=file", str(pointer_source), "--run=aot"],
                               capture_output=True, text=True, errors="replace", timeout=45)
    (result.parent / "pointer-storage.log").write_text(completed.stdout + completed.stderr, encoding="utf-8")
    assert completed.returncode == 0 and "pointer generic storage OK" in completed.stdout, (
        completed.returncode, completed.stdout, completed.stderr)
    checks.append("generic-pointer-storage-and-native-address-stride")
    # Both programs type-check in Vyx. Reject only the incompatible DCI
    # signatures; never turn all pointer types into a universal ABI match.
    cases = {
        "wrong-pointer-depth": '''extern "dci" { class QApplication {
            QApplication(argc: &mut i32, argv: *char, flags: i32);
        }; }
        fn main() -> i32 { var argc: i32 = 1; var argv: *char = null;
            var app = QApplication(&mut argc, argv, 394243); return 0; }''',
        "wrong-character-identity": '''extern "dci" { class QString { QString(text: *i8); }; }
        fn main() -> i32 { var bytes: *i8 = null; var text = QString(bytes); return 0; }''',
        "wrong-callback-payload": '''extern "dci" {
            class QObject {};
            class QAbstractButton { public fn on_clicked(context: *QObject,
                callback: cfn(rawptr,i32), user: rawptr) -> bool; };
        }
        @[no_mangle] fn callback(user: rawptr, value: i32) { let unused = value; }
        fn main() -> i32 { var sender: *QAbstractButton = null; var context: *QObject = null;
            unsafe { (*sender).on_clicked(context, callback, null); } return 0; }''',
        "wrong-callback-return": '''extern "dci" {
            class QObject {};
            class QAbstractButton { public fn on_clicked(context: *QObject,
                callback: cfn(rawptr,bool) -> i32, user: rawptr) -> bool; };
        }
        @[no_mangle] fn callback(user: rawptr, value: bool) -> i32 { return 0; }
        fn main() -> i32 { var sender: *QAbstractButton = null; var context: *QObject = null;
            unsafe { (*sender).on_clicked(context, callback, null); } return 0; }''',
    }
    with tempfile.TemporaryDirectory(prefix="qt-binding-", dir=result.parent) as directory:
        for name, source in cases.items():
            path = Path(directory) / (name + ".vyx")
            path.write_text(source, encoding="utf-8")
            command = [str(compiler), "--emit=obj", "--src=file", str(path),
                       "-o", str(path.with_suffix(".obj")), "--dci", str(contract)]
            completed = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=30)
            log = completed.stdout + completed.stderr
            (result.parent / (name + ".log")).write_text(log, encoding="utf-8")
            assert completed.returncode != 0 and "DCI Descriptor has no matching symbol" in log, log
            checks.append(name + ":rejected")
        path = Path(directory) / "owned-subclass-field.vyx"
        path.write_text('''extern "dci" { class QWidget {}; }
            class OwnedState : QWidget { public text: string;
                OwnedState() { self.text = "owned"; } };
            fn main() -> i32 { var state = OwnedState(); return 0; }''', encoding="utf-8")
        command = [str(compiler), "--emit=dci-stubs", "--src=file", str(path),
                   "-o", str(path.with_suffix(".cpp")), "--dci", str(contract)]
        completed = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=30)
        log = completed.stdout + completed.stderr
        (result.parent / "owned-subclass-field.log").write_text(log, encoding="utf-8")
        assert completed.returncode != 0 and "scalar/borrowed-pointer consumer state" in log, log
        checks.append("owned-subclass-field:rejected-with-lifecycle-diagnostic")

    # Exercise the same project import scope as build. The native Qt subclass
    # thunk passes QObject* and QTimerEvent*, not two-word Vyx handles.
    project = program.parent.parent
    ir_path = result.parent / "qt-callback-abi.ll"
    command = [str(compiler), "--src=file", "src/main.vyx", "--emit=ir", "-O0",
               "--project-unit-sources", ".cache/unit_crate_dci_qt_counter.sources",
               "--dci", str(contract), "-o", str(ir_path)]
    completed = subprocess.run(command, cwd=project, capture_output=True, text=True,
                               errors="replace", timeout=60)
    assert completed.returncode == 0, completed.stdout + completed.stderr
    signatures = [line for line in ir_path.read_text(encoding="utf-8").splitlines()
                  if line.startswith("define ") and "_N_timerEvent_" in line]
    assert len(signatures) == 1, signatures
    assert re.search(r"\(ptr(?: [^,]+)?, ptr(?: [^)]+)?\)", signatures[0]), signatures[0]
    checks.append("native-virtual-callback-two-address-ABI")

    # Match a plain IDE launch: Qt must load from the deployed application,
    # without borrowing DLLs or platform plugins from the Qt installation.
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("QT_")}
    env["DCI_QT_COUNTER_SELFTEST"] = "1"
    windows = Path(env.get("WINDIR", "C:/Windows"))
    env["PATH"] = os.pathsep.join(map(str, (windows / "System32", windows)))
    fonts = windows / "Fonts"
    if fonts.is_dir():
        env["QT_QPA_FONTDIR"] = str(fonts)
    for platform in ("offscreen", "windows"):
        env["QT_QPA_PLATFORM"] = platform
        completed = subprocess.run([str(program), "--vyx-dci-test", "argument with spaces"], env=env, capture_output=True,
                                   text=True, errors="replace", timeout=15, cwd=program.parent)
        (result.parent / (platform + ".stdout.log")).write_text(completed.stdout, encoding="utf-8")
        (result.parent / (platform + ".stderr.log")).write_text(completed.stderr, encoding="utf-8")
        assert completed.returncode == 0, (completed.returncode, completed.stdout, completed.stderr)
        assert "buttons, held press, LCD, virtual timer" in completed.stdout, completed.stdout
        checks.append("deployed-Qt-" + platform + "-event-loop-and-clean-exit")
    report = {"status": "passed", "compiler": str(compiler), "compiler_sha256": digest(compiler),
              "program": str(program), "program_sha256": digest(program),
              "contract_sha256": digest(contract), "checks": checks, "qt": str(qt)}
    result.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(completed.stdout.strip())
    print("Qt binding negatives OK; " + str(result))


if __name__ == "__main__":
    main()

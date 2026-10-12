"""Real HTTP registries and hostile archives; no public network required."""
from __future__ import annotations

import functools
import hashlib
import http.server
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[3]
ENGINE = ROOT / "tools/packages/package_manager.py"


class Registry:
    def __init__(self, root: Path):
        self.root = root
        root.mkdir(parents=True)
        self.index = {"schema": 1, "packages": {}}
        self.requests = []
        self.views = {}
        self.redirect_to = None
        registry = self

        class Handler(http.server.SimpleHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                registry.requests.append((self.path, self.headers.get("Authorization")))
                if self.path.startswith("/denied"):
                    self.send_error(403)
                    return
                if self.path == "/redirect" and registry.redirect_to:
                    self.send_response(302)
                    self.send_header("Location", registry.redirect_to)
                    self.end_headers()
                    return
                if self.path == "/index.json" and registry.views:
                    view = registry.views.get(self.headers.get("Authorization"))
                    if view is None:
                        self.send_error(401)
                        return
                    data = json.dumps(view).encode()
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return
                super().do_GET()

        handler = functools.partial(Handler, directory=str(root))
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}/index.json"
        self.save()

    def save(self):
        (self.root / "index.json").write_text(json.dumps(self.index), encoding="utf-8")

    def add(self, name, version="1.0.0", *, dependencies="", files=None, kind="source", triplet=None, abi=None):
        manifest = (f'[package]\nname = "{name}"\nversion = "{version}"\n'
                    + (f'kind = "{kind}"\n' if kind != "source" else "")
                    + f'\n[target.{name}]\ntype = "source"\nsources = ["src/lib.vyx"]\n'
                    + dependencies)
        payload = {"Vyx.toml": manifest.encode(), "src/lib.vyx": f"module {name};\npublic fn answer() -> i32 {{ return 42; }}\n".encode()}
        payload.update(files or {})
        archive = self.root / f"{name}-{version}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
            for path, content in payload.items():
                out.writestr(path, content)
        artifact = {"url": archive.name, "sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
        release = artifact if kind == "source" else {"kind": kind, "artifacts": [{**artifact, "triplet": triplet, "abi": abi}]}
        self.index["packages"].setdefault(name, {"versions": {}})["versions"][version] = release
        self.save()
        return archive, payload

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


class SourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="vyx-packages-")
        self.work = Path(self.temp.name)
        self.app = self.work / "app"
        self.app.mkdir()
        self.a = Registry(self.work / "a")
        self.b = Registry(self.work / "b")
        self.env = dict(os.environ, VYX_PACKAGE_SOURCES=str(self.work / "sources.json"))
        self.env.pop("VYX_REGISTRY", None)
        self.env.pop("VYX_PACKAGE_PROJECT", None)
        (self.work / "sources.json").write_text('{"schema":1,"sources":[]}', encoding="utf-8")
        self.manifest()

    def tearDown(self):
        self.a.close()
        self.b.close()
        self.temp.cleanup()

    def manifest(self, deps="", extra="", sources=None):
        sources = sources or [("primary", self.a.url), ("secondary", self.b.url)]
        content = '[package]\nname = "app"\nversion = "0.1.0"\n'
        for name, url in sources:
            content += f'\n[sources.{name}]\nurl = {json.dumps(url)}\n'
        content += f"\n[dependencies]\n{deps}\n{extra}"
        (self.app / "Vyx.toml").write_text(content, encoding="utf-8")

    def cli(self, *args, ok=True):
        result = subprocess.run([sys.executable, str(ENGINE), *args], cwd=self.app, env=self.env,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=35)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def resolve(self, alias, *, owner=None, section="dependencies", args=()):
        output = self.work / "path.txt"
        self.cli("resolve-path", "--project", str(self.app), "--owner", str(owner or self.app),
                 "--section", section, "--name", alias, "--output", str(output), *args)
        path = Path(output.read_text(encoding="utf-8").strip())
        self.assertTrue(path.is_absolute())
        self.assertTrue((path / "Vyx.toml").is_file())
        return path

    def test_http_semver_transitive_target_and_module_lock(self):
        self.a.add("leaf", "1.1.0")
        self.a.add("leaf", "1.4.0")
        self.a.add("facade", dependencies='\n[dependencies]\nleaf = "^1.0"\n')
        self.manifest(extra='[target.app.dependencies]\nfacade = { version = "~1.0", source = "primary" }\n')
        module_lock = 'FieldMigration:2.0.0["_1"]\n'
        (self.app / "Vyx.lock").write_text(module_lock)
        facade = self.resolve("facade", section="target.app.dependencies")
        leaf = self.resolve("leaf", owner=facade)
        self.assertIn('version = "1.4.0"', (leaf / "Vyx.toml").read_text())
        self.assertEqual((self.app / "Vyx.lock").read_text(), module_lock)
        self.assertTrue((self.app / "Vyx.packages.lock").is_file())

    def test_explicit_source_and_source_cache_identity(self):
        self.a.add("same", files={"identity": b"a"})
        self.b.add("same", files={"identity": b"b"})
        self.manifest('same = { version = "1.0.0", source = "primary" }')
        first = self.resolve("same")
        self.manifest('same = { version = "1.0.0", source = "secondary" }')
        second = self.resolve("same")
        self.assertNotEqual(first, second)
        self.assertEqual((first / "identity").read_bytes(), b"a")
        self.assertEqual((second / "identity").read_bytes(), b"b")

    def test_source_crud_search(self):
        self.a.add("search_me")
        self.cli("source", "add", "office", self.a.url)
        self.assertIn("office", self.cli("source", "list").stdout)
        self.assertIn("search_me", self.cli("search", "search_me").stdout)
        self.cli("source", "remove", "office")
        config = json.loads((self.work / "sources.json").read_text())
        self.assertEqual(config["sources"], [])

    def test_install_inserts_into_dependencies_not_trailing_target(self):
        self.a.add("math")
        self.manifest(extra='[target.app]\ntype = "executable"\nentry = "src/main.vyx"\n')
        self.cli("install", "math@1.0.0", "--source", "primary")
        import tomllib
        actual = tomllib.loads((self.app / "Vyx.toml").read_text())
        self.assertIn("math", actual["dependencies"])
        self.assertNotIn("math", actual["target"]["app"])

    def test_failed_install_preserves_manifest(self):
        self.a.add("math")
        self.a.index["packages"]["math"]["versions"]["1.0.0"]["sha256"] = "0" * 64
        self.a.save()
        before = (self.app / "Vyx.toml").read_bytes()
        self.cli("install", "math@1.0.0", ok=False)
        self.assertEqual((self.app / "Vyx.toml").read_bytes(), before)
        self.assertFalse((self.app / "Vyx.packages.lock").exists())

    def test_offline_validated_cache_and_lock(self):
        self.a.add("math")
        self.manifest('math = "1.0.0"')
        installed = self.resolve("math")
        before = len(self.a.requests)
        self.assertEqual(self.resolve("math", args=("--offline", "--locked")), installed)
        self.assertEqual(len(self.a.requests), before)
        (installed / "src/lib.vyx").write_text("tampered")
        self.cli("lock", "--offline", ok=False)

    def test_offline_missing_lock_rejected(self):
        self.a.add("math")
        self.manifest('math = "1.0.0"')
        self.cli("lock", "--offline", ok=False)

    def test_locked_manifest_drift_rejected(self):
        self.a.add("math", "1.0.0")
        self.a.add("math", "2.0.0")
        self.manifest('math = "1.0.0"')
        self.cli("lock")
        prior = (self.app / "Vyx.packages.lock").read_bytes()
        self.manifest('math = "2.0.0"')
        self.cli("lock", "--locked", ok=False)
        self.assertEqual((self.app / "Vyx.packages.lock").read_bytes(), prior)

    def test_priority_source_version_mismatch_has_no_fallback(self):
        self.a.add("math", "1.0.0")
        self.b.add("math", "2.0.0")
        self.manifest('math = "2.0.0"')
        self.cli("lock", ok=False)
        self.assertEqual(self.b.requests, [])

    def test_denied_source_has_no_fallback(self):
        self.b.add("math")
        self.manifest('math = "1.0.0"', sources=[("denied", self.a.url.replace("index.json", "denied.json")), ("good", self.b.url)])
        self.cli("lock", ok=False)
        self.assertEqual(self.b.requests, [])

    def test_duplicate_json_key_has_no_fallback(self):
        self.b.add("math")
        (self.a.root / "index.json").write_text('{"schema":1,"schema":1,"packages":{}}')
        self.manifest('math = "1.0.0"')
        self.cli("lock", ok=False)
        self.assertEqual(self.b.requests, [])

    def test_checksum_error_preserves_existing_lock(self):
        self.a.add("math")
        self.manifest('math = "^1.0"')
        self.cli("lock")
        prior = (self.app / "Vyx.packages.lock").read_bytes()
        self.a.add("math", "1.1.0")
        self.a.index["packages"]["math"]["versions"]["1.1.0"]["sha256"] = "0" * 64
        self.a.save()
        self.cli("update", ok=False)
        self.assertEqual((self.app / "Vyx.packages.lock").read_bytes(), prior)

    def test_archive_traversal_rejected(self):
        self.a.add("math", files={"../escape": b"bad"})
        self.manifest('math = "1.0.0"')
        self.cli("lock", ok=False)
        self.assertFalse((self.app / "escape").exists())

    def test_tar_symlink_rejected(self):
        archive = self.a.root / "math.tar.gz"
        with tarfile.open(archive, "w:gz") as out:
            info = tarfile.TarInfo("src")
            info.type = tarfile.SYMTYPE
            info.linkname = "../../escape"
            out.addfile(info)
        self.a.index["packages"]["math"] = {"versions": {"1.0.0": {"url": archive.name, "sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}}}
        self.a.save()
        self.manifest('math = "1.0.0"')
        self.cli("lock", ok=False)

    def test_manifest_identity_mismatch_rejected(self):
        self.a.add("math", files={"Vyx.toml": b'[package]\nname="other"\nversion="1.0.0"\n'})
        self.manifest('math = "1.0.0"')
        self.cli("lock", ok=False)

    def test_transitive_constraint_conflict(self):
        self.a.add("leaf", "1.0.0")
        self.a.add("leaf", "2.0.0")
        self.a.add("facade", dependencies='\n[dependencies]\nleaf = "2.0.0"\n')
        self.manifest('leaf = "1.0.0"\nfacade = "1.0.0"')
        self.cli("lock", ok=False)

    def test_cycle_rejected(self):
        self.a.add("one", dependencies='\n[dependencies]\ntwo = "1.0.0"\n')
        self.a.add("two", dependencies='\n[dependencies]\none = "1.0.0"\n')
        self.manifest('one = "1.0.0"')
        self.cli("lock", ok=False)

    def test_file_index_relative_archive(self):
        self.a.add("math")
        self.manifest('math = "1.0.0"', sources=[("local", (self.a.root / "index.json").as_uri())])
        self.resolve("math")
        self.assertEqual(self.a.requests, [])

    def test_private_token_not_written_to_lock(self):
        self.a.add("math")
        self.manifest('math = "1.0.0"')
        manifest = self.app / "Vyx.toml"
        manifest.write_text(manifest.read_text().replace('[sources.primary]\n', '[sources.primary]\ntoken_env = "VYX_TEST_PACKAGE_TOKEN"\n'))
        self.env["VYX_TEST_PACKAGE_TOKEN"] = "private-do-not-persist"
        self.cli("lock")
        self.assertTrue(any(header == "Bearer private-do-not-persist" for _, header in self.a.requests))
        self.assertNotIn("private-do-not-persist", (self.app / "Vyx.packages.lock").read_text())

    def test_binary_wrong_platform_rejected(self):
        self.a.add("binary", kind="binary", triplet="aarch64-linux-android", abi="android")
        self.manifest('binary = "1.0.0"')
        self.cli("lock", "--triplet", "x86_64-pc-windows-msvc", ok=False)

    def test_cache_marker_and_payload_tamper_rejected(self):
        self.a.add("math")
        self.manifest('math = "1.0.0"')
        installed = self.resolve("math")
        path = installed / "src/lib.vyx"
        path.write_text("changed payload")
        marker = installed / ".vyx-package.json"
        record = json.loads(marker.read_text())
        record["files"]["src/lib.vyx"] = hashlib.sha256(path.read_bytes()).hexdigest()
        marker.write_text(json.dumps(record))
        self.cli("lock", "--offline", ok=False)

    def test_lock_missing_transitive_edge_rejected(self):
        self.a.add("leaf")
        self.a.add("facade", dependencies='\n[dependencies]\nleaf = "1.0.0"\n')
        self.manifest('facade = "1.0.0"')
        self.cli("lock")
        path = self.app / "Vyx.packages.lock"
        value = json.loads(path.read_text())
        value["edges"] = [edge for edge in value["edges"] if edge["alias"] != "leaf"]
        path.write_text(json.dumps(value))
        self.cli("lock", "--offline", ok=False)

    def test_lock_pins_and_explicit_update_selects_new_version(self):
        self.a.add("math")
        self.manifest('math = "^1.0"')
        old = self.resolve("math")
        self.a.add("math", "1.1.0")
        self.assertEqual(self.resolve("math"), old)
        self.cli("update")
        updated = self.resolve("math")
        self.assertNotEqual(updated, old)
        self.assertIn('version = "1.1.0"', (updated / "Vyx.toml").read_text())

    def test_same_source_version_cannot_change_digest(self):
        self.a.add("math")
        self.manifest('math = "1.0.0"')
        self.cli("lock")
        before = (self.app / "Vyx.packages.lock").read_bytes()
        self.a.add("math", files={"changed": b"replacement"})
        self.cli("update", ok=False)
        self.assertEqual((self.app / "Vyx.packages.lock").read_bytes(), before)

    def test_duplicate_binary_variant_rejected(self):
        self.a.add("binary", kind="binary", triplet="x86_64-pc-windows-msvc", abi="msvc")
        release = self.a.index["packages"]["binary"]["versions"]["1.0.0"]
        release["artifacts"].append(dict(release["artifacts"][0]))
        self.a.save()
        self.manifest('binary = "1.0.0"')
        self.cli("lock", "--triplet", "x86_64-pc-windows-msvc", ok=False)

    def test_concurrent_lock_writers(self):
        self.a.add("math")
        self.manifest('math = "1.0.0"')
        jobs = [subprocess.Popen([sys.executable, str(ENGINE), "lock"], cwd=self.app, env=self.env,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(4)]
        for process in jobs:
            out, err = process.communicate(timeout=35)
            self.assertEqual(process.returncode, 0, out + err)
        self.resolve("math", args=("--offline", "--locked"))

    def test_publish_filters_private_data_and_install_archive(self):
        publish = self.work / "publishing"
        publish.mkdir()
        self.manifest(sources=[("local", (publish / "index.json").as_uri())])
        (self.app / ".codex").mkdir()
        (self.app / ".codex/chat.json").write_text("private conversation")
        (self.app / ".env").write_text("SECRET=private")
        (self.app / "build.log").write_text("private log")
        (self.app / "src").mkdir()
        (self.app / "src/lib.vyx").write_text("module app;")
        self.cli("publish", "--source", "local")
        index = json.loads((publish / "index.json").read_text())
        release = index["packages"]["app"]["versions"]["0.1.0"]
        with tarfile.open(publish / release["url"], "r:gz") as archive:
            files = archive.getnames()
        self.assertIn("Vyx.toml", files)
        self.assertIn("src/lib.vyx", files)
        self.assertNotIn(".codex/chat.json", files)
        self.assertNotIn(".env", files)
        self.assertNotIn("build.log", files)

    def test_tar_with_explicit_strip_prefix(self):
        _, payload = self.a.add("math")
        path = self.a.root / "math.tar.gz"
        with tarfile.open(path, "w:gz") as archive:
            for name, content in payload.items():
                info = tarfile.TarInfo("math-1.0.0/" + name)
                info.size = len(content)
                archive.addfile(info, io.BytesIO(content))
        self.a.index["packages"]["math"]["versions"]["1.0.0"] = {
            "url": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "strip_prefix": "math-1.0.0"}
        self.a.save()
        self.manifest('math = "1.0.0"')
        installed = self.resolve("math")
        self.assertEqual((installed / "src/lib.vyx").read_bytes(), payload["src/lib.vyx"])

    def test_template_and_target_alias_overlay(self):
        self.a.add("math", "1.0.0")
        self.a.add("math", "2.0.0")
        self.manifest('math = "1.0.0"', extra='[template.base.dependencies]\nmath = "1.0.0"\n'
                      '[target.app]\nextends=["base"]\n[target.app.dependencies]\nmath = "2.0.0"\n')
        installed = self.resolve("math", section="target.app.dependencies")
        self.assertIn('version = "2.0.0"', (installed / "Vyx.toml").read_text())

    def test_solver_backtracks_transitive_version(self):
        self.a.add("leaf", "1.0.0")
        self.a.add("leaf", "2.0.0")
        self.a.add("facade", "1.0.0", dependencies='\n[dependencies]\nleaf = "1.0.0"\n')
        self.a.add("facade", "1.1.0", dependencies='\n[dependencies]\nleaf = "2.0.0"\n')
        self.manifest('leaf = "1.0.0"\nfacade = "^1.0"')
        installed = self.resolve("facade")
        self.assertIn('version = "1.0.0"', (installed / "Vyx.toml").read_text())

    def test_source_cache_reused_across_explicit_platform_update(self):
        self.a.add("math")
        self.manifest('math = "1.0.0"')
        self.cli("lock", "--triplet", "x86_64-pc-windows-msvc", "--abi", "msvc")
        cached = list((self.app / ".cache/packages").glob("*/math/1.0.0/*/Vyx.toml"))
        self.assertEqual(len(cached), 1)
        self.cli("lock", "--triplet", "aarch64-linux-android", "--abi", "android", ok=False)
        self.cli("update", "--triplet", "aarch64-linux-android", "--abi", "android")
        self.cli("lock", "--triplet", "aarch64-linux-android", "--abi", "android", "--offline", "--locked")
        self.assertEqual(list((self.app / ".cache/packages").glob("*/math/1.0.0/*/Vyx.toml")), cached)

    def test_same_url_named_private_sources_are_isolated(self):
        import shutil
        archive, _ = self.a.add("math", files={"identity": b"a"})
        shutil.copy2(archive, self.a.root / "view-a.zip")
        first_index = json.loads(json.dumps(self.a.index))
        first_index["packages"]["math"]["versions"]["1.0.0"]["url"] = "view-a.zip"
        archive, _ = self.a.add("math", files={"identity": b"b"})
        shutil.copy2(archive, self.a.root / "view-b.zip")
        second_index = json.loads(json.dumps(self.a.index))
        second_index["packages"]["math"]["versions"]["1.0.0"]["url"] = "view-b.zip"
        self.a.views = {"Bearer key-a": first_index, "Bearer key-b": second_index}
        self.env.update(VYX_PACKAGE_KEY_A="key-a", VYX_PACKAGE_KEY_B="key-b")
        self.manifest('first={package="math",version="1.0.0",source="first"}\n'
                      'second={package="math",version="1.0.0",source="second"}',
                      sources=[("first", self.a.url), ("second", self.a.url)])
        path = self.app / "Vyx.toml"
        path.write_text(path.read_text().replace('[sources.first]\n', '[sources.first]\ntoken_env="VYX_PACKAGE_KEY_A"\n')
                        .replace('[sources.second]\n', '[sources.second]\ntoken_env="VYX_PACKAGE_KEY_B"\n'))
        a = self.resolve("first")
        b = self.resolve("second", args=("--offline", "--locked"))
        self.assertNotEqual(a, b)
        self.assertEqual((a / "identity").read_bytes(), b"a")
        self.assertEqual((b / "identity").read_bytes(), b"b")

    def test_authenticated_cross_origin_redirect_does_not_leak_token(self):
        self.a.redirect_to = self.b.url
        self.env["VYX_PACKAGE_REDIRECT_KEY"] = "private-key"
        self.manifest('math="1.0.0"', sources=[("private", self.a.url.replace("index.json", "redirect"))])
        path = self.app / "Vyx.toml"
        path.write_text(path.read_text().replace('[sources.private]\n', '[sources.private]\ntoken_env="VYX_PACKAGE_REDIRECT_KEY"\n'))
        self.cli("lock", ok=False)
        self.assertEqual(self.b.requests, [])

    def test_implicit_directory_case_collision_rejected(self):
        self.a.add("math", files={"Src/other.vyx": b"different directory spelling"})
        self.manifest('math="1.0.0"')
        self.cli("lock", ok=False)


if __name__ == "__main__":
    unittest.main()

"""Named JSON indices, authenticated transport, and platform artifact selection."""
from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import platform
import re
from urllib.parse import urljoin, urlsplit, unquote
from urllib.request import Request, build_opener, HTTPRedirectHandler, url2pathname

from common import PackageError, atomic_json, check_name, digest, load_manifest, read_json
from versions import Version

MAX_INDEX = 16 * 1024 * 1024
MAX_ARCHIVE = 512 * 1024 * 1024


def normalize_url(value: str, base: Path | str) -> str:
    if not isinstance(value, str) or not value:
        raise PackageError("source and artifact URLs must be nonempty strings")
    if re.match(r"^[A-Za-z]:[/\\]", value):
        return Path(value).resolve().as_uri()
    parts = urlsplit(value)
    if not parts.scheme:
        if isinstance(base, str) and urlsplit(base).scheme:
            value = urljoin(base, value)
        else:
            value = (Path(base) / value).resolve().as_uri()
        parts = urlsplit(value)
    if parts.scheme not in ("file", "http", "https"):
        raise PackageError(f"unsupported package URL scheme {parts.scheme!r}")
    if parts.username or parts.password:
        raise PackageError("credentials in package URLs are forbidden; use token_env")
    if parts.fragment:
        raise PackageError("package URLs may not contain a fragment")
    if parts.scheme == "file" and parts.netloc not in ("", "localhost"):
        raise PackageError("remote file URL authorities are unsupported")
    if parts.scheme in ("http", "https") and not parts.hostname:
        raise PackageError("HTTP package URL has no hostname")
    if re.search(r"(?:^|[&?])(?:token|access_token|password|authorization|api_key)=", parts.query, re.I):
        raise PackageError("credentials in package URLs are forbidden; use token_env")
    return value


def file_path(url: str) -> Path:
    parts = urlsplit(url)
    if parts.scheme != "file":
        raise PackageError("this operation requires a local file source")
    return Path(url2pathname(parts.path))


def origin(url):
    parts = urlsplit(url)
    return parts.scheme, parts.hostname, parts.port or (443 if parts.scheme == "https" else 80)


class RestrictedRedirect(HTTPRedirectHandler):
    def __init__(self, authenticated: bool):
        self.authenticated = authenticated

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        normalize_url(newurl, req.full_url)
        if self.authenticated and origin(req.full_url) != origin(newurl):
            raise PackageError("authenticated package download redirects to a different origin")
        if urlsplit(req.full_url).scheme == "https" and urlsplit(newurl).scheme != "https":
            raise PackageError("package download redirects from HTTPS to an insecure URL")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def fetch(url: str, token_env: str | None = None, max_bytes: int = MAX_INDEX) -> bytes:
    token = None
    if token_env:
        token = os.environ.get(token_env)
        if not token:
            raise PackageError(f"source requires environment variable {token_env}")
        if "\r" in token or "\n" in token:
            raise PackageError(f"invalid bearer token in {token_env}")
    try:
        if urlsplit(url).scheme == "file":
            with file_path(url).open("rb") as stream:
                data = stream.read(max_bytes + 1)
        else:
            headers = {"User-Agent": "Vyx-Package-Manager/1", "Accept": "application/json, */*"}
            if token:
                headers["Authorization"] = "Bearer " + token
            opener = build_opener(RestrictedRedirect(bool(token)))
            with opener.open(Request(url, headers=headers), timeout=30) as stream:
                data = stream.read(max_bytes + 1)
        if len(data) > max_bytes:
            raise PackageError(f"package input exceeds {max_bytes} bytes")
        return data
    except PackageError:
        raise
    except Exception as exc:
        # HTTP exceptions may echo Authorization values through proxy messages.
        message = str(exc)
        if token:
            message = message.replace(token, "[redacted]")
        raise PackageError(f"cannot read {url}: {message}") from exc


def host_triplet() -> tuple[str, str]:
    arch = platform.machine().lower()
    arch = {"amd64": "x86_64", "arm64": "aarch64"}.get(arch, arch)
    if "ANDROID_ROOT" in os.environ or os.environ.get("PREFIX", "").startswith("/data/data/com.termux/"):
        return f"{arch}-linux-android", "android"
    if os.name == "nt":
        return f"{arch}-pc-windows-msvc", "msvc"
    if platform.system() == "Darwin":
        return f"{arch}-apple-darwin", "darwin"
    return f"{arch}-unknown-linux-gnu", "gnu"


def platform_identity(triplet: str | None, abi: str | None):
    default_triplet, default_abi = host_triplet()
    triple = triplet or default_triplet
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", triple) or "*" in triple:
        raise PackageError(f"invalid target triplet {triple!r}")
    inferred = "android" if "android" in triple else "msvc" if "msvc" in triple else "gnu" if "gnu" in triple else "darwin" if "darwin" in triple else None
    selected_abi = abi or inferred or (default_abi if triple == default_triplet else None)
    if not selected_abi:
        raise PackageError(f"cannot infer ABI for {triple}; specify --abi")
    check_name(selected_abi, "ABI")
    return triple, selected_abi


@dataclass(frozen=True)
class Source:
    name: str
    url: str
    token_env: str | None = None

    @property
    def identity(self):
        return digest((self.name + "\n" + self.url).encode())[:16]

    def record(self):
        return {"name": self.name, "url": self.url}

    def package_key(self, name: str):
        return self.name + ":" + self.identity + ":" + name


def sources_config_path():
    return Path(os.environ.get("VYX_PACKAGE_SOURCES", str(Path.home() / ".vyx" / "sources.json"))).resolve()


def global_config():
    path = sources_config_path()
    if not path.exists():
        return {"schema": 1, "sources": []}
    config = read_json(path.read_bytes(), str(path))
    if type(config.get("schema")) is not int or config.get("schema") != 1 or not isinstance(config.get("sources"), list):
        raise PackageError("sources.json requires schema=1 and a sources array")
    if set(config) - {"schema", "sources"}:
        raise PackageError("unknown sources.json fields")
    return config


def _source(value, base):
    if not isinstance(value, dict):
        raise PackageError("each source must be an object")
    if set(value) - {"name", "url", "token_env"}:
        raise PackageError(f"unknown source fields: {sorted(set(value) - {'name', 'url', 'token_env'})}")
    name = check_name(value.get("name"), "source")
    token_env = value.get("token_env")
    if "token_env" in value and (not isinstance(token_env, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", token_env)):
        raise PackageError("token_env must name an environment variable")
    return Source(name, normalize_url(value.get("url"), base), token_env)


def load_sources(project: Path) -> list[Source]:
    values = []
    seen = set()
    manifest = load_manifest(project)
    project_sources = manifest.get("sources", {})
    if not isinstance(project_sources, dict):
        raise PackageError("[sources] must contain named source tables")
    for name, table in project_sources.items():
        if not isinstance(table, dict):
            raise PackageError(f"[sources.{name}] must be a table")
        src = _source({**table, "name": name}, project)
        values.append(src)
        seen.add(src.name)
    config_path = sources_config_path()
    global_seen = set()
    for value in global_config()["sources"]:
        src = _source(value, config_path.parent)
        if src.name in global_seen:
            raise PackageError(f"duplicate source name {src.name!r}")
        global_seen.add(src.name)
        if src.name not in seen:
            values.append(src)
            seen.add(src.name)
    legacy = os.environ.get("VYX_REGISTRY")
    if legacy and "local" not in seen:
        url = normalize_url(legacy, project)
        if file_path(url).is_dir():
            url = (file_path(url) / "index.json").as_uri()
        values.append(Source("local", url))
    return values


class Indices:
    def __init__(self, sources: list[Source], triplet: str, abi: str):
        self.sources = sources
        self.triplet, self.abi = triplet, abi
        self.loaded = {}

    def get(self, source: Source):
        cache_key = source.name, source.url, source.token_env
        if cache_key in self.loaded:
            return self.loaded[cache_key]
        index = read_json(fetch(source.url, source.token_env), source.url)
        if type(index.get("schema")) is not int or index.get("schema") != 1 or not isinstance(index.get("packages"), dict):
            raise PackageError(f"source {source.name} requires schema=1 and packages object")
        if set(index) - {"schema", "packages"}:
            raise PackageError(f"unknown index fields in {source.name}")
        for name, package in index["packages"].items():
            check_name(name)
            if not isinstance(package, dict) or not isinstance(package.get("versions"), dict):
                raise PackageError(f"{name}: index package requires versions object")
            if set(package) - {"description", "versions"}:
                raise PackageError(f"{name}: unknown package index fields")
            if "description" in package and not isinstance(package["description"], str):
                raise PackageError(f"{name}: description must be a string")
            for version, release in package["versions"].items():
                Version(version)
                self.validate_release(release, source)
        self.loaded[cache_key] = index
        return index

    def validate_release(self, release, source):
        if not isinstance(release, dict):
            raise PackageError("version entry must be an object")
        kind = release.get("kind", "source")
        if kind not in ("source", "binary", "dci"):
            raise PackageError(f"unsupported package kind {kind!r}")
        fields = {"kind", "url", "sha256", "strip_prefix"} if kind == "source" else {"kind", "artifacts"}
        if set(release) - fields:
            raise PackageError(f"unknown {kind} release fields")
        if kind == "source":
            self.validate_artifact(release, source, binary=False)
        else:
            artifacts = release.get("artifacts")
            if not isinstance(artifacts, list) or not artifacts:
                raise PackageError(f"{kind} release requires nonempty artifacts array")
            pairs = set()
            for artifact in artifacts:
                self.validate_artifact(artifact, source, binary=True)
                pair = artifact["triplet"], artifact["abi"]
                if pair in pairs:
                    raise PackageError(f"overlapping binary artifacts for {pair[0]}/{pair[1]}")
                pairs.add(pair)

    def validate_artifact(self, artifact, source, binary):
        if not isinstance(artifact, dict):
            raise PackageError("artifact must be an object")
        fields = {"url", "sha256", "strip_prefix", "triplet", "abi"} if binary else {"kind", "url", "sha256", "strip_prefix"}
        if set(artifact) - fields:
            raise PackageError("unknown artifact fields")
        normalize_url(artifact.get("url"), source.url)
        if not isinstance(artifact.get("sha256"), str) or not re.fullmatch(r"[0-9a-fA-F]{64}", artifact["sha256"]):
            raise PackageError("artifact sha256 must contain 64 hexadecimal digits")
        if "strip_prefix" in artifact and not isinstance(artifact["strip_prefix"], str):
            raise PackageError("strip_prefix must be a string")
        if binary:
            platform_identity(artifact.get("triplet"), artifact.get("abi"))
            if not isinstance(artifact.get("triplet"), str) or not isinstance(artifact.get("abi"), str):
                raise PackageError("binary artifacts require explicit triplet and abi")

    def choose_source(self, name: str, selected: str | None):
        check_name(name)
        if selected:
            source = next((item for item in self.sources if item.name == selected), None)
            if not source:
                raise PackageError(f"unknown package source {selected!r}")
            if name not in self.get(source)["packages"]:
                raise PackageError(f"package {name!r} not found in source {selected!r}")
            return source
        for source in self.sources:
            if name in self.get(source)["packages"]:
                return source
        raise PackageError(f"package {name!r} not found in configured sources")

    def artifact(self, source: Source, name: str, version: str):
        release = self.get(source)["packages"][name]["versions"][version]
        kind = release.get("kind", "source")
        if kind == "source":
            artifact = release
        else:
            matches = [item for item in release["artifacts"] if item["triplet"] == self.triplet and item["abi"] == self.abi]
            if len(matches) != 1:
                raise PackageError(f"{name}@{version}: no unique {kind} artifact for {self.triplet}/{self.abi}")
            artifact = matches[0]
        return {"kind": kind, "url": normalize_url(artifact["url"], source.url),
                "sha256": artifact["sha256"].lower(), "strip_prefix": artifact.get("strip_prefix", ""),
                "triplet": self.triplet, "abi": self.abi}


def source_command(action: str, name=None, url=None, token_env=None):
    path = sources_config_path()
    config = global_config()
    sources = config["sources"]
    if action == "list":
        for value in sources:
            src = _source(value, path.parent)
            print(f"{src.name}\t{src.url}" + (f"\ttoken_env={src.token_env}" if src.token_env else ""))
        return
    check_name(name, "source")
    if action == "add":
        value = {"name": name, "url": normalize_url(url, Path.cwd())}
        if token_env:
            value["token_env"] = token_env
        _source(value, path.parent)
        old = next((i for i, item in enumerate(sources) if item.get("name") == name), None)
        if old is None:
            sources.append(value)
        else:
            sources[old] = value
    elif action == "remove":
        if not any(item.get("name") == name for item in sources):
            raise PackageError(f"source {name!r} is not configured")
        config["sources"] = [item for item in sources if item.get("name") != name]
    atomic_json(path, config)
    print(f"source {name} {'saved' if action == 'add' else 'removed'}")

"""Release inputs and GitHub operations shared by the SDK build commands."""
import hashlib
import json
import os
from pathlib import Path
import re
import tarfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile

REPO = Path(__file__).resolve().parents[3]
REPOSITORY = "vyx-lang/vyx"


def sha256(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def api(path, *, method="GET", data=None):
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    headers = {"User-Agent": "vyx-sdk-release", "Accept": "application/vnd.github+json",
               "X-GitHub-Api-Version": "2022-11-28"}
    if token:
        headers["Authorization"] = "Bearer " + token
    payload = None if data is None else json.dumps(data).encode()
    if payload is not None:
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request("https://api.github.com/" + path,
                                     data=payload, headers=headers, method=method)
    with urllib.request.urlopen(request, timeout=90) as response:
        content = response.read()
    return json.loads(content) if content else None


def download(url, path):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".partial")
    # Public download URLs deliberately receive no API token. Redirects to
    # release object storage must not carry credentials to another host.
    request = urllib.request.Request(url, headers={"User-Agent": "vyx-sdk-release"})
    with urllib.request.urlopen(request, timeout=120) as source, temporary.open("wb") as target:
        while chunk := source.read(1024 * 1024):
            target.write(chunk)
    temporary.replace(path)
    return path


def bootstrap_asset(tag, name, directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    endpoint = "latest" if tag == "latest" else "tags/" + urllib.parse.quote(tag, safe="")
    release = api(f"repos/{REPOSITORY}/releases/{endpoint}")
    assets = {item["name"]: item for item in release["assets"]}
    for key in (name, name + ".sha256"):
        if key not in assets:
            raise RuntimeError(f"Bootstrap {tag} is missing {key}")
        download(assets[key]["browser_download_url"], directory / key)
    checksum = (directory / (name + ".sha256")).read_text().strip()
    match = re.fullmatch(r"([a-fA-F0-9]{64})\s+\*?(.+)", checksum)
    if not match or match[2] != name or sha256(directory / name) != match[1].lower():
        raise RuntimeError(f"Bootstrap checksum mismatch: {name}")
    (directory / (name + ".input.json")).write_text(json.dumps({
        "tag": release["tag_name"], "release_id": release["id"], "asset_id": assets[name]["id"],
        "name": name, "sha256": match[1].lower(), "bytes": assets[name]["size"]
    }, indent=2) + "\n")
    return directory / name


def unpack_sdk(archive, destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    archive = Path(archive)
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as bundle:
            for name in bundle.namelist():
                if not (destination / name).resolve().is_relative_to(destination.resolve()):
                    raise ValueError("Archive member escapes SDK directory")
            bundle.extractall(destination)
    else:
        with tarfile.open(archive) as bundle:
            bundle.extractall(destination, filter="data")
    manifests = list(destination.glob("*/SDK-MANIFEST.json"))
    if len(manifests) != 1:
        raise RuntimeError("Expected one SDK root in bootstrap archive")
    return manifests[0].parent


def checksum_sidecar(archive):
    archive = Path(archive)
    archive.with_name(archive.name + ".sha256").write_text(
        sha256(archive) + "  " + archive.name + "\n", encoding="ascii")

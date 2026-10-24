#!/usr/bin/env python3
"""Publish a complete SDK set under a new version.yy.mm.dd[.N] tag."""
import argparse
from datetime import datetime, timezone, timedelta
import http.client
import json
import os
from pathlib import Path
import re
import tomllib
import urllib.error
import urllib.parse

from common import REPO, REPOSITORY, api, sha256

SDK_NAMES = (
    "vyx-sdk-windows-x86_64-llvm22.zip",
    "vyx-sdk-linux-x86_64-llvm22.tar.gz",
    "vyx-sdk-linux-aarch64-llvm22.tar.gz",
    "vyx-sdk-termux-aarch64.deb",
    "vyx-sdk-macos-aarch64-llvm22.tar.gz",
    "vyx-sdk-macos-x86_64-llvm22.tar.gz",
)
RECORD_NAMES = ("SDK-BUILD.windows-x86_64.json", "SDK-BUILD.linux-x86_64.json",
                "SDK-BUILD.linux-aarch64.json", "SDK-BUILD.android-aarch64.json",
                "SDK-BUILD.macos-aarch64.json", "SDK-BUILD.macos-x86_64.json")


def tag_candidate(version, day, ordinal):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?", version):
        raise ValueError(f"Unsupported package version: {version!r}")
    if ordinal < 1:
        raise ValueError("Tag ordinal must be positive")
    return version + "." + day.strftime("%y.%m.%d") + (f".{ordinal}" if ordinal > 1 else "")


def reserve_tag(version, day, commit, request=api):
    # The refs API creates atomically. A concurrent publisher can only make us
    # choose the next ordinal; existing refs are never updated or deleted.
    for ordinal in range(1, 10001):
        tag = tag_candidate(version, day, ordinal)
        try:
            request(f"repos/{REPOSITORY}/git/ref/tags/{tag}")
        except urllib.error.HTTPError as error:
            if error.code != 404:
                raise
        else:
            continue
        try:
            request(f"repos/{REPOSITORY}/git/refs", method="POST",
                    data={"ref": "refs/tags/" + tag, "sha": commit})
            return tag
        except urllib.error.HTTPError as error:
            if error.code != 422:
                raise
            # Distinguish a raced creation from permissions or validation errors.
            request(f"repos/{REPOSITORY}/git/ref/tags/{tag}")
    raise RuntimeError("Daily tag sequence exhausted")


def validate_assets(directory, commit):
    paths = []
    for name, record_name in zip(SDK_NAMES, RECORD_NAMES):
        package = directory / name
        sidecar = directory / (name + ".sha256")
        checksum = re.fullmatch(r"([0-9a-fA-F]{64})\s+\*?(.+)", sidecar.read_text().strip())
        if not checksum or checksum[2] != name or sha256(package) != checksum[1].lower():
            raise ValueError(f"Release checksum mismatch: {name}")
        record = directory / record_name
        identity = json.loads(record.read_text())
        if identity["source_commit"] != commit or identity["archive_sha256"] != checksum[1].lower():
            raise ValueError(f"Release source/build identity mismatch: {name}")
        if identity["archive_bytes"] != package.stat().st_size:
            raise ValueError(f"Release size mismatch: {name}")
        paths.extend((package, sidecar, record))
    return paths


def upload(upload_url, path):
    token = os.environ.get("GH_TOKEN") or os.environ["GITHUB_TOKEN"]
    url = urllib.parse.urlsplit(upload_url.split("{")[0])
    if url.scheme != "https" or url.hostname != "uploads.github.com":
        raise ValueError("Unexpected GitHub upload endpoint")
    connection = http.client.HTTPSConnection(url.hostname, timeout=600)
    try:
        connection.putrequest("POST", url.path + "?name=" + urllib.parse.quote(path.name))
        connection.putheader("Authorization", "Bearer " + token)
        connection.putheader("User-Agent", "vyx-sdk-release")
        connection.putheader("Content-Type", "application/octet-stream")
        connection.putheader("Content-Length", str(path.stat().st_size))
        connection.endheaders()
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                connection.send(chunk)
        response = connection.getresponse()
        payload = response.read()
        if response.status != 201:
            raise RuntimeError(f"Upload {path.name} failed: HTTP {response.status}: {payload[:1000]!r}")
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.commit):
        parser.error("--commit must be a full source commit SHA")
    paths = validate_assets(args.assets, args.commit)
    version = tomllib.loads((REPO / "bootstrap_compiler/Vyx.toml").read_text(encoding="utf-8"))["package"]["version"]
    day = datetime.now(timezone(timedelta(hours=8))).date()
    tag = reserve_tag(version, day, args.commit)
    release = api(f"repos/{REPOSITORY}/releases", method="POST", data={
        "tag_name": tag, "target_commitish": args.commit, "name": "Vyx " + tag,
        "draft": True, "prerelease": False,
        "body": f"SDK source: `{args.commit}`\n\n"
                "Windows x64, Linux x64 / AArch64, Android Termux AArch64, macOS Apple Silicon / Intel.\n"
                "SHA-256 sidecars and build identities are attached.\n\n"
                "Early access build; the date suffix identifies this build, not a new stability level.\n",
    })
    for path in paths:
        print("Uploading " + path.name, flush=True)
        upload(release["upload_url"], path)
    main_ref = api(f"repos/{REPOSITORY}/git/ref/heads/main")
    published = api(f"repos/{REPOSITORY}/releases/{release['id']}", method="PATCH",
                    data={"draft": False, "make_latest": "true" if main_ref["object"]["sha"] == args.commit else "false"})
    print(published["html_url"], flush=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Fetch and verify the pinned Debian Linux source subset; never install it.

Only an explicit invocation downloads data. A supplied package is left intact.
Outputs must be new and outside the checkout. Failed attempts remain marked
FAIL and cannot be reused as a completed source restoration.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.parse
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_targets

ROOT = Path(__file__).resolve().parents[1]
CHUNK = 1024 * 1024
MAX_MEMBERS = 250000
MAX_UNCOMPRESSED = 4 * 1024**3
MAX_SELECTED = 64 * 1024**2
DOWNLOAD_SECONDS = 900


def canonical_relative(value):
    if not isinstance(value, str) or not value or "\\" in value or "\0" in value:
        raise ValueError("Invalid relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in value.split("/")):
        raise ValueError("Noncanonical relative path: " + value)
    if str(path) != value:
        raise ValueError("Noncanonical relative path: " + value)
    return path


def validate_identity(value):
    if not isinstance(value, dict) or type(value.get("bytes")) is not int or value["bytes"] < 1:
        raise ValueError("Invalid pinned byte size")
    digest = value.get("sha256")
    if (
        not isinstance(digest, str)
        or len(digest) != 64
        or any(c not in "0123456789abcdef" for c in digest)
    ):
        raise ValueError("Invalid SHA256 pin")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key: " + key)
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError("Nonfinite JSON value: " + value)


def load_lock(path):
    raw = path.read_bytes()
    lock = json.loads(raw, object_pairs_hook=unique_object, parse_constant=reject_constant)
    json.dumps(lock, allow_nan=False)
    if not isinstance(lock, dict) or not all(
        key in lock for key in ("source_package", "source_archive", "linux_source_files")
    ):
        raise ValueError("Incomplete source lock")
    if lock.get("schema") != "poulsback-debian-kernel-lock-v1":
        raise ValueError("Unsupported source lock")
    package, archive, files = (
        lock["source_package"],
        lock["source_archive"],
        lock["linux_source_files"],
    )
    validate_identity(package)
    validate_identity(archive)
    for item in (package, archive):
        name = canonical_relative(item["filename"])
        if len(name.parts) != 1:
            raise ValueError("Pinned filename must be a basename")
    url = urllib.parse.urlsplit(package["url"])
    if url.scheme != "https" or not url.netloc or url.username or url.password:
        raise ValueError("Pinned source URL requires HTTPS without credentials")
    root = canonical_relative(archive["root"])
    if len(root.parts) != 1:
        raise ValueError("Pinned source root must be a basename")
    if not isinstance(files, dict) or not 1 <= len(files) <= MAX_MEMBERS:
        raise ValueError("A bounded nonempty pinned source inventory is required")
    for relative, expected in files.items():
        canonical_relative(relative)
        validate_identity(expected)
    if sum(item["bytes"] for item in files.values()) > MAX_SELECTED:
        raise ValueError("Selected source exceeds the extraction budget")
    return lock, {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def identity(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError("A regular file is required: " + str(path))
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(CHUNK), b""):
            digest.update(block)
            size += len(block)
    return {"bytes": size, "sha256": digest.hexdigest()}


def verify(path, expected):
    validate_identity(expected)
    if path.is_symlink() or not path.is_file() or path.stat().st_size != expected["bytes"]:
        raise ValueError("Pinned file size/type differs: " + path.name)
    actual = identity(path)
    if actual != {key: expected[key] for key in ("bytes", "sha256")}:
        raise ValueError("Pinned file SHA256 differs: " + path.name)
    return actual


class TLSRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, url):
        if urllib.parse.urlsplit(url).scheme != "https":
            raise ValueError("A source download redirect attempted to leave HTTPS")
        return super().redirect_request(request, fp, code, message, headers, url)


def download(package, destination):
    if urllib.parse.urlsplit(package["url"]).scheme != "https":
        raise ValueError("HTTPS source URL required")
    opener = urllib.request.build_opener(TLSRedirects())
    started = time.monotonic()
    total = 0
    with opener.open(package["url"], timeout=30) as response, destination.open("xb") as stream:
        if urllib.parse.urlsplit(response.geturl()).scheme != "https":
            raise ValueError("Source response is not HTTPS")
        while True:
            if time.monotonic() - started > DOWNLOAD_SECONDS:
                raise TimeoutError("Source download exceeded its time budget")
            block = response.read(min(CHUNK, package["bytes"] - total + 1))
            if not block:
                break
            total += len(block)
            if total > package["bytes"]:
                raise ValueError("Source download exceeded the pinned byte size")
            stream.write(block)
        stream.flush()
        os.fsync(stream.fileno())
    return verify(destination, package)


def extract_subset(archive, destination, files, *, root):
    """Stream only locked regular members; never extract tar links or metadata."""
    if destination.exists() or destination.is_symlink():
        raise ValueError("Source destination must be new")
    if len(canonical_relative(root).parts) != 1:
        raise ValueError("Pinned source root must be a basename")
    for relative, expected in files.items():
        canonical_relative(relative)
        validate_identity(expected)
    if sum(item["bytes"] for item in files.values()) > MAX_SELECTED:
        raise ValueError("Selected source exceeds the extraction budget")
    destination.mkdir(mode=0o700)
    found = {}
    members = declared = 0
    with tarfile.open(archive, mode="r|xz") as source:
        for member in source:
            members += 1
            declared += member.size
            if members > MAX_MEMBERS or declared > MAX_UNCOMPRESSED or member.size < 0:
                raise ValueError("Source archive exceeded its extraction budget")
            name = member.name.rstrip("/") if member.isdir() else member.name
            path = canonical_relative(name)
            if path.parts[0] != root:
                raise ValueError("Unexpected source archive root")
            relative = "/".join(path.parts[1:])
            if relative not in files:
                continue
            if relative in found:
                raise ValueError("Duplicate selected archive member: " + relative)
            if not member.isfile() or member.issym() or member.islnk():
                raise ValueError("Selected archive member is not a regular file: " + relative)
            expected = files[relative]
            if member.size != expected["bytes"]:
                raise ValueError("Selected member size differs: " + relative)
            stream = source.extractfile(member)
            if stream is None:
                raise ValueError("Selected member is unreadable: " + relative)
            raw = stream.read(expected["bytes"] + 1)
            actual = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
            if actual != {key: expected[key] for key in ("bytes", "sha256")}:
                raise ValueError("Selected member identity differs: " + relative)
            target = destination / relative
            target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            for parent in target.parents:
                if parent == destination.parent:
                    break
                if parent.is_symlink() or not parent.is_dir():
                    raise ValueError("Unsafe source destination ancestor")
            with target.open("xb") as output:
                output.write(raw)
            target.chmod(0o444)
            found[relative] = actual
    if set(found) != set(files):
        raise ValueError("Source archive is missing pinned members")
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-target", required=True)
    parser.add_argument(
        "--package", type=Path, help="Already downloaded pinned .deb; never modified"
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        target = build_targets.load(args.build_target)
        target_receipt = build_targets.receipt(target)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    lock_path = build_targets.input_path(target, "kernel_lock")
    lock, lock_identity = load_lock(lock_path)
    output = args.output.absolute()
    if (
        not output.parent.is_dir()
        or output.parent.resolve() != output.parent
        or output.exists()
        or output.is_symlink()
    ):
        parser.error("--output must be new beneath an existing canonical parent")
    if output.is_relative_to(ROOT):
        parser.error("--output must be outside the checkout")
    package = args.package.absolute() if args.package else None
    if package and (package.is_symlink() or not package.is_file()):
        parser.error("--package requires a regular file")
    required = (
        lock["source_archive"]["bytes"]
        + sum(p["bytes"] for p in lock["linux_source_files"].values())
        + 16 * CHUNK
    )
    if package is None:
        required += lock["source_package"]["bytes"]
    if shutil.disk_usage(output.parent).free < required:
        parser.error("Insufficient free space for the pinned restoration")
    output.mkdir(mode=0o700)
    report = {
        "schema": "poulsback-kernel-source-fetch-v1",
        "status": "RUNNING",
        "lock": lock_identity,
        "build_target": target_receipt,
        "recipe": identity(Path(__file__)),
        "package_source": "LOCAL_PINNED_FILE" if package else "EXPLICIT_HTTPS_DOWNLOAD",
        "source_package": lock["source_package"],
        "source_archive": lock["source_archive"],
        "package_installed": False,
        "package_scripts_executed": False,
        "hardware_accessed": False,
        "modules_built_or_loaded": False,
    }
    try:
        if package is None:
            package = output / lock["source_package"]["filename"]
            report["package_identity"] = download(lock["source_package"], package)
        else:
            report["package_identity"] = verify(package, lock["source_package"])
        unpacked = output / "package"
        command = ["dpkg-deb", "--extract", str(package), str(unpacked)]
        result = subprocess.run(command, capture_output=True, timeout=120)
        (output / "dpkg-deb.stdout").write_bytes(result.stdout)
        (output / "dpkg-deb.stderr").write_bytes(result.stderr)
        report["package_extraction"] = {"argv": command, "returncode": result.returncode}
        if result.returncode:
            raise RuntimeError("Pinned package extraction failed")
        archive = unpacked / "usr/src" / lock["source_archive"]["filename"]
        report["archive_identity"] = verify(archive, lock["source_archive"])
        report["verified_files"] = extract_subset(
            archive,
            output / "linux",
            lock["linux_source_files"],
            root=lock["source_archive"]["root"],
        )
        verify(package, lock["source_package"])
        verify(archive, lock["source_archive"])
        if identity(lock_path) != lock_identity or identity(Path(__file__)) != report["recipe"]:
            raise ValueError("Source lock or fetch recipe changed during restoration")
        if build_targets.receipt(target) != target_receipt:
            raise ValueError("Build target or its inputs changed during restoration")
        report["status"] = "PASS_" + str(len(lock["linux_source_files"])) + "_PINNED_SOURCE_FILES"
        report["source_directory"] = str(output / "linux")
    except Exception as error:
        report["status"] = "FAIL"
        report["error"] = str(error)
        raise
    finally:
        with (output / "report.json").open("x") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
    print(
        json.dumps(
            {"status": report["status"], "source_directory": report["source_directory"]}, indent=2
        )
    )


if __name__ == "__main__":
    try:
        main()
    except (
        OSError,
        ValueError,
        RuntimeError,
        tarfile.TarError,
        subprocess.TimeoutExpired,
    ) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)

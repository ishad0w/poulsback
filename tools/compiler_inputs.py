# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Read compiler-produced Make dependencies and stable file identities.

The one fixed -MT target is data. No Make expansion or shell parsing is used.
Ambiguous trailing whitespace/backslash filenames are explicitly unsupported.
GCC preserves supported literal backslashes. Clang can normalize them to slashes;
callers must reject that unsupported source naming rather than reconstruct it.
A depfile cannot recover a compiler-erased spelling; missing inputs always fail.
"""

import hashlib
import os
from pathlib import Path
import stat

TARGET = "poulsback_dependencies"


def parse_make_dependencies(raw, cwd):
    if not isinstance(raw, bytes) or len(raw) > 8 * 1024 * 1024 or b"\0" in raw:
        raise ValueError("Invalid dependency output")
    if raw.endswith((b"\\\n", b"\\\r\n")):
        raise ValueError("Unterminated dependency continuation")
    text = raw.decode("utf-8").replace("\\\r\n", "").replace("\\\n", "")
    text = text.replace("\r\n", "\n")
    if any(ord(char) < 32 and char not in "\t\n" for char in text):
        raise ValueError("Control character in dependency output")
    target, separator, body = text.partition(":")
    if separator != ":" or target.strip() != TARGET:
        raise ValueError("One fixed compiler dependency target required")
    body = body.rstrip(" \t\n")
    if "\n" in body:
        raise ValueError("Multiple or malformed dependency rules")
    cwd = Path(cwd)
    if not cwd.is_absolute():
        raise ValueError("Explicit absolute dependency working directory required")
    words, current = [], []

    def finish():
        if not current:
            return
        word = "".join(current)
        if word[-1] in " \t\\":
            raise ValueError("Ambiguous trailing dependency escape or whitespace")
        words.append(cwd / word)
        current.clear()

    i = 0
    while i < len(body):
        char = body[i]
        if char in " \t":
            finish()
            i += 1
        elif char == "\\":
            end = i
            while end < len(body) and body[end] == "\\":
                end += 1
            count = end - i
            following = body[end] if end < len(body) else None
            if following in (" ", "\t"):
                if not count % 2:
                    raise ValueError("Ambiguous trailing backslash dependency")
                current.extend("\\" * (count // 2))
                current.append(following)
                i = end + 1
            elif following == "#":
                current.extend("\\" * (count - 1))
                current.append("#")
                i = end + 1
            else:
                current.extend("\\" * count)
                i = end
        elif char == "$":
            if i + 1 >= len(body) or body[i + 1] != "$":
                raise ValueError("Make expansion is not a compiler dependency path")
            current.append("$")
            i += 2
        elif char == "#":
            raise ValueError("Unescaped dependency comment")
        else:
            # Quotes and colons in prerequisites are literal filename bytes.
            current.append(char)
            i += 1
    finish()
    if not words:
        raise ValueError("Empty compiler dependency closure")
    return list(dict.fromkeys(words))


def file_identity(path):
    """Retain lexical and resolved paths; ignore atime, refuse unstable reads."""
    path = Path(path)
    if not path.is_absolute():
        path = Path.cwd() / path

    def stable(value):
        return tuple(
            getattr(value, key)
            for key in (
                "st_dev",
                "st_ino",
                "st_mode",
                "st_nlink",
                "st_size",
                "st_mtime_ns",
                "st_ctime_ns",
            )
        )

    resolved = path.resolve(strict=True)
    lexical_before = path.lstat()
    before = path.stat()
    if not stat.S_ISREG(before.st_mode):
        raise ValueError("Regular compiler input required: " + str(path))
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        raw = stream.read()
        closed = os.fstat(stream.fileno())
    after = path.stat()
    lexical_after = path.lstat()
    if (
        not stable(before) == stable(opened) == stable(closed) == stable(after)
        or stable(lexical_before) != stable(lexical_after)
        or resolved != path.resolve(strict=True)
        or len(raw) != after.st_size
    ):
        raise ValueError("Compiler input changed during read: " + str(path))
    names = ("device", "inode", "mode", "links", "bytes", "mtime_ns", "ctime_ns")
    return {
        "path": str(path),
        "resolved": str(resolved),
        "bytes": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "stat": dict(zip(names, stable(after))),
        "lexical_stat": dict(zip(names, stable(lexical_after))),
    }

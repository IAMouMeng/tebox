# Minimal Blueprint parser for cc_binary / gki_custom / prebuilt_etc.
# Enough of Soong syntax to describe NDK vendor HALs. Not a Soong replacement.

from __future__ import annotations

import re
from typing import Any


class BpError(Exception):
    pass


def parse_file(path: str) -> list[dict[str, Any]]:
    with open(path, encoding="utf-8") as f:
        return parse(f.read(), filename=path)


def parse(text: str, filename: str = "<bp>") -> list[dict[str, Any]]:
    toks = list(_tokenize(text, filename))
    i = 0
    modules: list[dict[str, Any]] = []

    def peek():
        return toks[i] if i < len(toks) else None

    def eat(kind=None, val=None):
        nonlocal i
        t = peek()
        if t is None:
            raise BpError(f"{filename}: unexpected EOF")
        if kind and t[0] != kind:
            raise BpError(f"{filename}:{t[3]}: expected {kind}, got {t[0]} {t[1]!r}")
        if val is not None and t[1] != val:
            raise BpError(f"{filename}:{t[3]}: expected {val!r}, got {t[1]!r}")
        i += 1
        return t

    def parse_value() -> Any:
        t = peek()
        if t is None:
            raise BpError(f"{filename}: unexpected EOF in value")
        if t[0] == "str":
            eat()
            return t[1]
        if t[0] == "ident":
            eat()
            if t[1] == "true":
                return True
            if t[1] == "false":
                return False
            return t[1]
        if t[0] == "num":
            eat()
            return t[1]
        if t[0] == "sym" and t[1] == "[":
            eat()
            items = []
            while True:
                t = peek()
                if t is None:
                    raise BpError(f"{filename}: unclosed list")
                if t[0] == "sym" and t[1] == "]":
                    eat()
                    break
                if t[0] == "sym" and t[1] == ",":
                    eat()
                    continue
                items.append(parse_value())
            return items
        if t[0] == "sym" and t[1] == "{":
            return parse_map()
        raise BpError(f"{filename}:{t[3]}: bad value {t[1]!r}")

    def parse_map() -> dict[str, Any]:
        eat("sym", "{")
        m: dict[str, Any] = {}
        while True:
            t = peek()
            if t is None:
                raise BpError(f"{filename}: unclosed map")
            if t[0] == "sym" and t[1] == "}":
                eat()
                break
            if t[0] == "sym" and t[1] == ",":
                eat()
                continue
            key = eat("ident")[1]
            eat("sym", ":")
            m[key] = parse_value()
        return m

    while peek() is not None:
        kind = eat("ident")[1]
        body = parse_map()
        body["_type"] = kind
        body["_file"] = filename
        modules.append(body)
    return modules


def _tokenize(text: str, filename: str):
    i = 0
    n = len(text)
    line = 1
    while i < n:
        c = text[i]
        if c in " \t\r":
            i += 1
            continue
        if c == "\n":
            line += 1
            i += 1
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                raise BpError(f"{filename}:{line}: unclosed comment")
            line += text[i:j].count("\n")
            i = j + 2
            continue
        if c in "{}[],:+":
            yield ("sym", c, i, line)
            i += 1
            continue
        if c == '"':
            i += 1
            buf = []
            while i < n:
                if text[i] == "\\":
                    if i + 1 >= n:
                        raise BpError(f"{filename}:{line}: bad escape")
                    esc = text[i + 1]
                    buf.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(esc, esc))
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                if text[i] == "\n":
                    line += 1
                buf.append(text[i])
                i += 1
            else:
                raise BpError(f"{filename}:{line}: unclosed string")
            yield ("str", "".join(buf), i, line)
            continue
        m = re.match(r"-?[0-9]+", text[i:])
        if m and (i == 0 or not text[i - 1].isalpha()):
            # numbers only if not part of ident; ident can start with letter/_
            pass
        m = re.match(r"[A-Za-z_][A-Za-z0-9_.@+-]*", text[i:])
        if m:
            yield ("ident", m.group(0), i, line)
            i += m.end()
            continue
        m = re.match(r"-?[0-9]+(\.[0-9]+)?", text[i:])
        if m:
            raw = m.group(0)
            yield ("num", float(raw) if "." in raw else int(raw), i, line)
            i += m.end()
            continue
        raise BpError(f"{filename}:{line}: unexpected {c!r}")

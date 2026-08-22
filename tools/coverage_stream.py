#!/usr/bin/env python3
"""Incremental reader for ndsrecomp Tier-3 coverage manifests.

A player manifest is a single JSON object whose bulk sits in three arrays. The
first real submission was 202 MB, of which 196.5 MB was one array, and json.load
on that needs well over a gigabyte of heap to hand back a structure the ingest
walks once and throws away. Manifests only get bigger as sessions get longer, so
the reader streams: it walks the top-level object and yields the elements of the
big arrays one at a time, holding only the element currently being decoded.

Nothing here assumes how the writer laid the file out. The scanner respects JSON
string and escape rules and uses json.JSONDecoder.raw_decode for every value, so
a reformatted or hand-edited manifest reads identically to a freshly written one.
"""

from __future__ import annotations

import json
from typing import Iterator

_DECODER = json.JSONDecoder()
_WHITESPACE = " \t\r\n"


class _Window:
    """A sliding character window over a text file.

    Holds from the current parse position to as far ahead as the last decode
    needed. `need()` grows the window until a full JSON value fits, so the peak
    footprint is one element, not one file.
    """

    def __init__(self, handle, chunk: int = 1 << 20):
        self._handle = handle
        self._base_chunk = chunk
        self._chunk = chunk
        self._buf = ""
        self._pos = 0
        self._eof = False

    def _pull(self) -> bool:
        """Append more file. Reads grow geometrically while a value keeps not
        fitting, then reset once one decodes.

        A fixed read size is quadratic on a large element: the pre-cap MPH
        manifest holds a single 100 MB page object, and topping the buffer up a
        megabyte at a time both re-copies the accumulated string and re-scans it
        from the start on every retry. Doubling makes both costs amortized
        linear and caps the retries at O(log n).
        """
        if self._eof:
            return False
        data = self._handle.read(self._chunk)
        if not data:
            self._eof = True
            return False
        if self._pos:
            self._buf = self._buf[self._pos:]
            self._pos = 0
        self._buf += data
        self._chunk = min(self._chunk * 2, 1 << 28)
        return True

    def _settle(self) -> None:
        self._chunk = self._base_chunk

    def skip_space(self) -> None:
        while True:
            while self._pos < len(self._buf) and self._buf[self._pos] in _WHITESPACE:
                self._pos += 1
            if self._pos < len(self._buf) or not self._pull():
                return

    def peek(self) -> str:
        self.skip_space()
        if self._pos >= len(self._buf):
            return ""
        return self._buf[self._pos]

    def take(self, expected: str) -> None:
        got = self.peek()
        if got != expected:
            raise ValueError(f"expected {expected!r}, found {got!r}")
        self._pos += 1

    def value(self):
        """Decode one complete JSON value at the cursor."""
        self.skip_space()
        while True:
            try:
                obj, end = _DECODER.raw_decode(self._buf, self._pos)
            except ValueError:
                if self._pull():
                    continue
                raise
            # raw_decode succeeds on a truncated number or on a literal that a
            # further chunk would extend, so only trust a decode that did not
            # end exactly at the buffer edge -- unless there is no more file.
            if end < len(self._buf) or self._eof:
                self._pos = end
                self._settle()
                return obj
            if not self._pull():
                self._pos = end
                self._settle()
                return obj

    def skip_value(self) -> None:
        self.value()


def stream_manifest(path, array_keys: tuple[str, ...]) -> Iterator[tuple]:
    """Walk a manifest, yielding ('scalar', key, value) for small members and
    ('item', key, element) for each element of the arrays named in `array_keys`.

    `array_keys` may name a nested array as "pages.entries".
    """
    with open(path, "r", encoding="utf-8") as handle:
        window = _Window(handle)
        yield from _object(window, "", array_keys)


def _object(window: _Window, prefix: str, array_keys: tuple[str, ...]):
    window.take("{")
    if window.peek() == "}":
        window.take("}")
        return
    while True:
        key = window.value()
        if not isinstance(key, str):
            raise ValueError(f"object key is {key!r}, not a string")
        window.take(":")
        path = f"{prefix}{key}"
        if path in array_keys and window.peek() == "[":
            window.take("[")
            if window.peek() == "]":
                window.take("]")
            else:
                while True:
                    yield ("item", path, window.value())
                    nxt = window.peek()
                    if nxt == ",":
                        window.take(",")
                        continue
                    window.take("]")
                    break
        elif any(k.startswith(path + ".") for k in array_keys) and window.peek() == "{":
            yield from _object(window, path + ".", array_keys)
        else:
            yield ("scalar", path, window.value())
        nxt = window.peek()
        if nxt == ",":
            window.take(",")
            continue
        window.take("}")
        return

"""Canonical binary DCI container codec.

The payload is a typed JSON data model, not UTF-8 JSON text.  It is intended
for contract artifacts; JSON remains available only as an explicit diagnostic
representation.
"""

from __future__ import annotations

import argparse
import binascii
import json
import math
import struct
from pathlib import Path
from typing import Any

MAGIC = b"DCIB"
VERSION = 1
HEADER = struct.Struct("<4sHHQI")

NULL, FALSE, TRUE, I64, U64, F64, STRING, ARRAY, MAP = range(9)
CJSON_EXACT_INTEGER_MAX = 1 << 53


class DcibError(ValueError):
    pass


def validate_cjson_compatible(value: Any) -> None:
    """Reject numeric values the compiler's current cJSON DOM cannot retain."""
    if value is None or isinstance(value, bool) or isinstance(value, (float, str)):
        return
    if isinstance(value, int):
        if value < -CJSON_EXACT_INTEGER_MAX or value > CJSON_EXACT_INTEGER_MAX:
            raise DcibError("integer exceeds cJSON exact range")
        return
    if isinstance(value, list):
        for item in value: validate_cjson_compatible(item)
        return
    if isinstance(value, dict):
        for key, item in value.items():
            validate_cjson_compatible(key)
            validate_cjson_compatible(item)
        return
    raise DcibError(f"unsupported value: {type(value).__name__}")


def _encode(value: Any, out: bytearray) -> None:
    if value is None:
        out.append(NULL)
    elif value is False:
        out.append(FALSE)
    elif value is True:
        out.append(TRUE)
    elif isinstance(value, int):
        if value < 0:
            if value < -(1 << 63): raise DcibError("integer below i64")
            out.append(I64); out.extend(struct.pack("<q", value))
        else:
            if value > (1 << 64) - 1: raise DcibError("integer above u64")
            out.append(U64); out.extend(struct.pack("<Q", value))
    elif isinstance(value, float):
        if not math.isfinite(value): raise DcibError("non-finite float")
        out.append(F64); out.extend(struct.pack("<d", value))
    elif isinstance(value, str):
        if "\x00" in value: raise DcibError("string contains NUL")
        raw = value.encode("utf-8")
        out.append(STRING); out.extend(struct.pack("<I", len(raw))); out.extend(raw)
    elif isinstance(value, list):
        out.append(ARRAY); out.extend(struct.pack("<I", len(value)))
        for item in value: _encode(item, out)
    elif isinstance(value, dict):
        keys = sorted(value)
        if any(not isinstance(key, str) for key in keys): raise DcibError("map key is not string")
        out.append(MAP); out.extend(struct.pack("<I", len(keys)))
        for key in keys:
            _encode(key, out); _encode(value[key], out)
    else:
        raise DcibError(f"unsupported value: {type(value).__name__}")


def encode(document: Any) -> bytes:
    payload = bytearray(); _encode(document, payload)
    crc = binascii.crc32(payload) & 0xFFFFFFFF
    return HEADER.pack(MAGIC, VERSION, 0, len(payload), crc) + payload


def _take(data: memoryview, cursor: int, size: int) -> tuple[memoryview, int]:
    if size < 0 or cursor + size > len(data): raise DcibError("truncated payload")
    return data[cursor:cursor + size], cursor + size


def _decode(data: memoryview, cursor: int) -> tuple[Any, int]:
    tag, cursor = _take(data, cursor, 1); kind = tag[0]
    if kind == NULL: return None, cursor
    if kind == FALSE: return False, cursor
    if kind == TRUE: return True, cursor
    if kind == I64:
        raw, cursor = _take(data, cursor, 8); return struct.unpack("<q", raw)[0], cursor
    if kind == U64:
        raw, cursor = _take(data, cursor, 8); return struct.unpack("<Q", raw)[0], cursor
    if kind == F64:
        raw, cursor = _take(data, cursor, 8); value = struct.unpack("<d", raw)[0]
        if not math.isfinite(value): raise DcibError("non-finite float")
        return value, cursor
    if kind == STRING:
        size, cursor = _take(data, cursor, 4); length = struct.unpack("<I", size)[0]
        raw, cursor = _take(data, cursor, length)
        try: value = bytes(raw).decode("utf-8")
        except UnicodeDecodeError as exc: raise DcibError("invalid UTF-8") from exc
        if "\x00" in value: raise DcibError("string contains NUL")
        return value, cursor
    if kind in (ARRAY, MAP):
        size, cursor = _take(data, cursor, 4); count = struct.unpack("<I", size)[0]
        if kind == ARRAY:
            result = []
            for _ in range(count): value, cursor = _decode(data, cursor); result.append(value)
            return result, cursor
        result: dict[str, Any] = {}
        previous = ""
        for _ in range(count):
            key, cursor = _decode(data, cursor)
            if not isinstance(key, str) or (result and key <= previous): raise DcibError("noncanonical map key")
            previous = key; value, cursor = _decode(data, cursor); result[key] = value
        return result, cursor
    raise DcibError(f"unknown tag {kind}")


def decode(blob: bytes) -> Any:
    if len(blob) < HEADER.size: raise DcibError("truncated header")
    magic, version, flags, length, crc = HEADER.unpack_from(blob)
    if magic != MAGIC or version != VERSION or flags != 0: raise DcibError("unsupported DCIB header")
    if length != len(blob) - HEADER.size: raise DcibError("invalid payload length")
    payload = blob[HEADER.size:]
    if (binascii.crc32(payload) & 0xFFFFFFFF) != crc: raise DcibError("payload CRC mismatch")
    value, cursor = _decode(memoryview(payload), 0)
    if cursor != len(payload): raise DcibError("trailing payload bytes")
    return value


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Encode or inspect canonical DCIB contracts.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    encode_parser = subparsers.add_parser("encode", help="encode a JSON DCI diagnostic into DCIB")
    encode_parser.add_argument("input", type=Path)
    encode_parser.add_argument("output", type=Path)

    decode_parser = subparsers.add_parser("decode", help="decode DCIB into diagnostic JSON")
    decode_parser.add_argument("input", type=Path)
    decode_parser.add_argument("output", type=Path)

    args = parser.parse_args(argv)
    if args.command == "encode":
        document = json.loads(args.input.read_text(encoding="utf-8"))
        validate_cjson_compatible(document)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(encode(document))
        return 0

    document = decode(args.input.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

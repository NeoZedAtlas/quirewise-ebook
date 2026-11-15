#!/usr/bin/env python3
"""Generate the tiny MDict fixtures used by the CLI format matrix.

The writer below emits the engine-2.0 layout (MdxBuilder 3.x) with zlib
blocks and no encryption:

    header      4-byte big-endian length, UTF-16LE root element, 4-byte
                little-endian Adler-32 of the element bytes;
    section     five big-endian 64-bit numbers plus their Adler-32;
    key index   four-byte descriptor, Adler-32, zlib stream;
    key blocks  record offset + key text + NUL, per block;
    records     block table and concatenated article bytes.

Both files are deliberately small and contain only generated text, so they can
be committed and used as regression fixtures. Run this script from any
directory; it writes tiny.mdx and tiny.mdd next to itself.
"""

from __future__ import annotations

import pathlib
import struct
import zlib

HERE = pathlib.Path(__file__).resolve().parent

MDX_HEADER = (
    '<Dictionary GeneratedByEngineVersion="2.0" RequiredEngineVersion="2.0" '
    'Encrypted="0" Encoding="UTF-8" Format="Html" KeyCaseSensitive="No" '
    'StripKey="Yes" RegisterBy="EMail" Title="Quirewise sample" '
    'Description="Generated MDict fixture. /&gt; or &lt;br&gt; '
    'entities must stay escaped."/>'
)

MDD_HEADER = (
    '<Library_Data GeneratedByEngineVersion="2.0" RequiredEngineVersion="2.0" '
    'Encrypted="0" Format="" KeyCaseSensitive="No" StripKey="No" '
    'RegisterBy="EMail" Title="Quirewise sample resources" '
    'Description="Generated MDict resource fixture."/>'
)

MDX_ENTRIES = [
    ("alpha", "<p>Quirewise sample definition for alpha</p>"),
    ("beta", "<p>Quirewise sample definition for beta<br>second line</p>"),
    ("gamma", "<p>Quirewise sample definition for gamma</p>"),
]

MDD_ENTRIES = [
    ("\\sample.txt", b"Quirewise sample resource\n"),
    ("\\image.gif", b"GIF89a" + bytes(range(1, 16))),
]


def be16(value: int) -> bytes:
    return struct.pack(">H", value)


def be32(value: int) -> bytes:
    return struct.pack(">I", value)


def be64(value: int) -> bytes:
    return struct.pack(">Q", value)


def block(content: bytes, adler: int) -> bytes:
    payload = zlib.compress(content, 9)
    return struct.pack("<I", 2) + be32(adler) + payload


def build(resource_file: bool, header_text: str,
          entries: list[tuple[str, bytes]]) -> bytes:
    records = bytearray()
    offsets = []
    for _key, record in entries:
        assert isinstance(record, (bytes, bytearray))
        offsets.append(len(records))
        records += record

    key_block = bytearray()
    for index, (key, _record) in enumerate(entries):
        key_block += be64(offsets[index])
        if resource_file:
            key_block += key.encode("utf-16-le") + b"\x00\x00"
        else:
            key_block += key.encode("utf-8") + b"\x00"
    key_block = bytes(key_block)

    key_info = bytearray()
    key_info += be64(len(entries))
    for key in (entries[0][0], entries[-1][0]):
        if resource_file:
            encoded = key.encode("utf-16-le")
            key_info += be16(len(key)) + encoded + b"\x00\x00"
        else:
            encoded = key.encode("utf-8")
            key_info += be16(len(encoded)) + encoded + b"\x00"
    key_block_payload = block(key_block, zlib.adler32(key_block) & 0xFFFFFFFF)
    key_info += be64(len(key_block_payload))
    key_info += be64(len(key_block))
    key_info = bytes(key_info)
    key_info_payload = block(key_info, zlib.adler32(key_info) & 0xFFFFFFFF)

    numbers = (
        be64(1)
        + be64(len(entries))
        + be64(len(key_info))
        + be64(len(key_info_payload))
        + be64(len(key_block_payload))
    )

    record_payload = block(bytes(records), zlib.adler32(bytes(records)) & 0xFFFFFFFF)
    record_section = (
        be64(1)
        + be64(len(entries))
        + be64(16)
        + be64(len(record_payload))
        + be64(len(record_payload))
        + be64(len(records))
    )

    element = header_text.encode("utf-16-le")
    out = bytearray()
    out += be32(len(element))
    out += element
    out += struct.pack("<I", zlib.adler32(element) & 0xFFFFFFFF)
    out += numbers
    out += be32(zlib.adler32(numbers) & 0xFFFFFFFF)
    out += key_info_payload
    out += key_block_payload
    out += record_section
    out += record_payload
    return bytes(out)


def main() -> None:
    mdx = build(False, MDX_HEADER, [(key, record.encode("utf-8"))
                                   for key, record in MDX_ENTRIES])
    (HERE / "tiny.mdx").write_bytes(mdx)
    mdd = build(True, MDD_HEADER, MDD_ENTRIES)
    (HERE / "tiny.mdd").write_bytes(mdd)
    print(f"wrote tiny.mdx ({len(mdx)} bytes) and tiny.mdd ({len(mdd)} bytes)")


if __name__ == "__main__":
    main()

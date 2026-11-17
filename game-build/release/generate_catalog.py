# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a local item catalog from the player's own Fortune's Weave dump.

The release ships this program, never its output.  Only the first contiguous
IID string table in each Item*.bin is used; later occurrences are references
and unrelated strings, not new inventory records.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import unicodedata
from pathlib import Path

import zstandard


ITEM_FILES = ("Item.bin", "Item_P0.bin", "Item_P1.bin")
ITEM_SHA256 = {
    "Item.bin": "0895a35581369306623749f77d00701b12e98f4427ec9123b3edd3b84ab472a7",
    "Item_P0.bin": "debbd0d432c1ba2f82422a563391abfbfdd2697cdc5c4cab8b4273f84c306c4b",
    "Item_P1.bin": "349e6e3f8bffcaa9d280fdd1b23f2b211bf9d61cb6c85d565dc74c6a9159568a",
}
IID = re.compile(rb"IID_[^\x00]{1,120}\x00")
UPGRADE = re.compile(r"[＋+][０-９0-9]+$")
PUNCTUATION = re.compile(r"[\s・·\-()（）_]+")
# A few internal Item.bin IDs differ from the Japanese text actually shown
# by the game.  Each target is an exact name in the player's own JP message
# table; the Chinese spelling still comes from that same player's dump.
DISPLAY_ALIASES = {
    "アーカイヴT": "アーカイヴΤ",
    "アーカイヴβ": "アーカイヴΒ",
    "アーカイヴγ": "アーカイヴΓ",
    "ウォームZ": "ウォームΖ",
    "スライムB": "スライムΒ",
    "ドーラ△": "ドーラΔ",
}


def item_ids(path: Path) -> list[str]:
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != ITEM_SHA256[path.name]:
        raise ValueError(f"Item table differs from supported update 131072: {path}")
    found = list(IID.finditer(data))
    if not found:
        raise ValueError(f"No item ID table in {path}")
    end = 1
    while end < len(found) and found[end].start() == found[end - 1].end():
        end += 1
    return [m.group()[:-1].decode("utf-8") for m in found[:end]]


def messages(path: Path) -> dict[str, str]:
    data = zstandard.ZstdDecompressor().decompress(path.read_bytes())
    if not data.startswith(b"MsgStdBn") or data[8:10] != b"\xff\xfe":
        raise ValueError(f"Unsupported message table: {path}")
    label_section = data.find(b"LBL1")
    text_section = data.find(b"TXT2")
    if label_section < 0 or text_section < 0:
        raise ValueError(f"Missing message sections: {path}")
    labels_base = label_section + 16
    texts_base = text_section + 16
    buckets = struct.unpack_from("<I", data, labels_base)[0]
    text_count = struct.unpack_from("<I", data, texts_base)[0]
    if buckets > 4096 or text_count > 100000:
        raise ValueError(f"Invalid message table: {path}")
    result = {}
    for bucket in range(buckets):
        count, offset = struct.unpack_from("<II", data, labels_base + 4 + bucket * 8)
        position = labels_base + offset
        for _ in range(count):
            size = data[position]
            position += 1
            label = data[position:position + size].decode("utf-8")
            position += size
            index = struct.unpack_from("<I", data, position)[0]
            position += 4
            if index >= text_count:
                raise ValueError(f"Invalid message index: {path}")
            start = texts_base + struct.unpack_from("<I", data, texts_base + 4 + index * 4)[0]
            stop = data.find(b"\0", start)
            if stop < 0:
                raise ValueError(f"Unterminated message: {path}")
            # Controls embedded after the item name are formatting commands.
            result[label] = data[start:stop].split(b"\x0e", 1)[0].decode("utf-8", "replace")
    return result


def normalized(value: str) -> str:
    return PUNCTUATION.sub("", unicodedata.normalize("NFKC", value))


def label_index(japanese: dict[str, str]) -> dict[str, set[str]]:
    result: dict[str, set[str]] = {}
    for label, name in japanese.items():
        if label.startswith("MIID_") and not label.startswith("MIID_H_") and name:
            result.setdefault(normalized(name), set()).add(label)
    return result


def guess_label(iid: str, index: dict[str, set[str]]) -> tuple[str | None, str]:
    name = iid[4:]
    # Try the exact name first.  Many refinement rows use the same localized
    # name as the base weapon, so their +1/+2/+3 suffix is appended below.
    variants = [name, UPGRADE.sub("", name)]
    if "_" in name and not re.search(r"_SC[0-9]", name):
        stem = name.split("_", 1)[0]
        variants += [stem, UPGRADE.sub("", stem)]
    variants.append(re.split(r"[（(]", name, maxsplit=1)[0])
    for candidate in variants:
        matches = index.get(normalized(DISPLAY_ALIASES.get(candidate, candidate)), set())
        if len(matches) == 1:
            label = next(iter(matches))
            # Some resource names collide despite representing different item
            # sizes.  Retain the in-game IID when the label is suspicious.
            if label.endswith("StoneS") and "・大" in name:
                continue
            suffix = name[len(UPGRADE.sub("", name)):]
            return label, unicodedata.normalize("NFKC", suffix)
    return None, ""


def generate(romfs: Path) -> list[dict[str, object]]:
    japanese = messages(romfs / "Msg" / "JPja" / "Item.msbt.zst")
    chinese = messages(romfs / "Msg" / "CNzh" / "Item.msbt.zst")
    traditional = messages(romfs / "Msg" / "TWzh" / "Item.msbt.zst")
    names = label_index(japanese)
    output = []
    seen = set()
    for filename in ITEM_FILES:
        path = romfs / "GameData" / filename
        ids = item_ids(path)
        for item_index, iid in enumerate(ids):
            if iid == "IID_無し" or iid in seen:
                continue
            seen.add(iid)
            label, suffix = guess_label(iid, names)
            fallback = iid[4:]
            row: dict[str, object] = {
                "iid": iid,
                "item_index": item_index,
                "name_jp": (japanese.get(label, fallback) + suffix) if label else fallback,
                "name_zh_cn": (chinese.get(label, fallback) + suffix) if label else fallback,
                "name_zh_tw": (traditional.get(label, fallback) + suffix) if label else fallback,
            }
            output.append(row)
    output.sort(key=lambda row: str(row["iid"]))
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--romfs", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    rows = generate(args.romfs)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    temporary.write_text(json.dumps(rows, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(args.output)
    index = label_index(messages(args.romfs / "Msg" / "JPja" / "Item.msbt.zst"))
    localized = sum(guess_label(str(row["iid"]), index)[0] is not None for row in rows)
    print(f"Wrote {len(rows)} items to {args.output}; {localized} matched game text")


if __name__ == "__main__":
    main()

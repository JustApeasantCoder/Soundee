"""Decode the observed PEQb v3 layout without modifying the source profile.

This supports the layout verified in Yamaha HS8.swproj, not every SoundID version.
Curve roles, channel order, scalar fields, and the third point value are unverified.
"""

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PROFILE = ROOT / "assets" / "profiles" / "raw" / "Yamaha HS8.swproj"


def decode(path):
    data = Path(path).read_bytes()
    closing = b"</ProjectHeader>"
    xml_end = data.find(closing)
    if xml_end < 0:
        raise ValueError("ProjectHeader not found")
    xml_end += len(closing)
    header = ET.fromstring(data[:xml_end])
    ns = {"s": "http://www.sonarworks.com"}
    eq_parts = [part for part in header.findall("s:Parts/s:ProjectHeaderPart", ns)
                if part.findtext("s:Type", namespaces=ns) == "eqb"]
    if len(eq_parts) != 1:
        raise ValueError("Expected exactly one eqb part")
    size = int(eq_parts[0].findtext("s:Size", namespaces=ns))
    # The observed separator is one ESC byte; do not scan the opaque payload.
    offset = xml_end + 1
    if data[xml_end:offset] != b"\x1b" or data[offset:offset + 4] != b"PEQb":
        raise ValueError("Unsupported project separator or EQ block location")
    block = data[offset:offset + size]
    if len(block) != size or size < 12:
        raise ValueError("Truncated EQ block")
    version, count = struct.unpack_from("<II", block, 4)
    if version != 3 or count != 4:
        raise ValueError(f"Unsupported PEQb layout: version={version}, curves={count}")
    curves = []
    cursor = 12
    for _ in range(count):
        if cursor + 24 > size:
            raise ValueError("Truncated curve header")
        curve_id, scalar_1, scalar_2, points = struct.unpack_from("<IddI", block, cursor)
        cursor += 24
        if points != 355 or cursor + points * 24 > size:
            raise ValueError("Unexpected point count or truncated curve data")
        rows = [struct.unpack_from("<ddd", block, cursor + i * 24)
                for i in range(points)]
        if not all(math.isfinite(v) for row in rows for v in row):
            raise ValueError("Nonfinite curve data")
        if not all(row[0] > 0 for row in rows) or not all(
                a[0] < b[0] for a, b in zip(rows, rows[1:])):
            raise ValueError("Invalid frequency ordering")
        if not math.isfinite(scalar_1) or not math.isfinite(scalar_2):
            raise ValueError("Nonfinite scalar field")
        curves.append({
            "id": curve_id,
            "scalar_1_unverified": scalar_1,
            "scalar_2_unverified": scalar_2,
            "point_count": points,
            "points": [{"frequency_hz": f, "value_db": v,
                        "third_value_unverified": extra} for f, v, extra in rows],
        })
        cursor += points * 24
    if [c["id"] for c in curves] != [1, 2, 3, 4]:
        raise ValueError("Unexpected curve identifiers")
    if block[cursor:] != b"\x00" * 4:
        raise ValueError("Unexpected EQ trailer")
    pairs = []
    for a, b in [(curves[0], curves[2]), (curves[1], curves[3])]:
        pairs.append({
            "curve_ids": [a["id"], b["id"]],
            "identical_frequency_grid": all(x["frequency_hz"] == y["frequency_hz"]
                for x, y in zip(a["points"], b["points"])),
            "max_absolute_value_sum_db": max(abs(x["value_db"] + y["value_db"])
                for x, y in zip(a["points"], b["points"])),
            "max_absolute_third_value_sum": max(abs(x["third_value_unverified"]
                + y["third_value_unverified"]) for x, y in zip(a["points"], b["points"])),
        })
    return {
        "source_file": str(Path(path).resolve()),
        "source_bytes": len(data),
        "source_sha256": hashlib.sha256(data).hexdigest(),
        "project_header_version": header.findtext("s:Version", namespaces=ns),
        "header_flags": {key: header.findtext(f"s:{key}", namespaces=ns)
                         for key in ["Compressed", "Encrypted", "PasswordProtected"]},
        "eq_block": {"offset_bytes": offset, "size_bytes": size,
                     "version": version, "encoding": "little-endian IEEE754 float64",
                     "curves": curves},
        "inverse_pair_checks": pairs,
        "opaque_payload": {"offset_bytes": offset + size,
                           "size_bytes": len(data) - offset - size,
                           "decoded": False},
        "interpretation": {
            "verified": "Four curves; pairs 1/3 and 2/4 can be checked numerically for inversion.",
            "inferred": "Curves 1/2 likely represent correction, 3/4 measured response; IDs likely correspond to stereo channel pairs.",
            "unverified": "Left/right assignment, scalar meanings, third-value units, and SoundID runtime filtering, limits, gain and delay policy.",
            "playback_ready": False,
        },
    }, data[:xml_end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path, nargs="?", default=DEFAULT_PROFILE)
    parser.add_argument("--outdir", type=Path)
    args = parser.parse_args()
    slug = "-".join(args.profile.stem.lower().split())
    out = args.outdir or ROOT / "assets" / "profiles" / "decoded" / slug
    result, header_xml = decode(args.profile)
    out.mkdir(parents=True, exist_ok=True)
    (out / "profile.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    element = ET.fromstring(header_xml)
    ET.indent(element)
    (out / "project_header.xml").write_text(ET.tostring(element, encoding="unicode") + "\n", encoding="utf-8")
    for curve in result["eq_block"]["curves"]:
        with (out / f"curve_{curve['id']}.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=["frequency_hz", "value_db", "third_value_unverified"])
            writer.writeheader()
            writer.writerows(curve["points"])
    print(f"Decoded {len(result['eq_block']['curves'])} curves into {out.resolve()}")
    print(f"Source SHA256: {result['source_sha256']}")
    print(json.dumps(result["inverse_pair_checks"], indent=2))


if __name__ == "__main__":
    main()

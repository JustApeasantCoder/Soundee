"""Generate public synthetic PEQb v3 calibration and an independent baseline.

No personal calibration data is read. Both files are committed, so building
and running CTest does not require Python or private assets.
"""
import json
import math
from pathlib import Path
import struct

destination = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "raw"
destination.mkdir(parents=True, exist_ok=True)
block = bytearray(b"PEQb" + struct.pack("<II", 3, 4))
curves = []
for channel in range(4):
    points = []
    block += struct.pack("<IddI", channel + 1, 0.0, 0.0, 355)
    for index in range(355):
        frequency = 20.0 * 1100.0 ** (index / 354.0)
        x = math.log(frequency / 1000.0)
        value = 4.0 * math.exp(-((x + 2.1) / 0.7) ** 2) - 3.0 * math.exp(-((x - 0.6) / 0.9) ** 2)
        value += (1.0 if channel % 2 == 0 else -1.0) * math.exp(-(x / 1.2) ** 2)
        if channel >= 2:
            value = -value
        extra = -value / 4.0
        block += struct.pack("<ddd", frequency, value, extra)
        points.append({"frequency_hz": frequency, "value_db": value, "third_value_unverified": extra})
    curves.append({"curve_id": channel + 1, "points": points})
block += struct.pack("<I", 0)
assert len(block) == 34192
header = b"<ProjectHeader><Parts><ProjectHeaderPart><Type>eqb</Type><Size>34192</Size></ProjectHeaderPart></Parts></ProjectHeader>"
(destination / "stereo-v3.swproj").write_bytes(header + b"\x1b" + block)
(destination / "expected.json").write_text(json.dumps({"synthetic": True, "eq_block": {"curves": curves}}, separators=(",", ":")) + "\n", encoding="utf-8")

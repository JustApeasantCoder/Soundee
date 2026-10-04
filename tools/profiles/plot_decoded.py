"""Plot raw decoded EQ curves using ReportLab's chart library."""
import json
import math
from pathlib import Path

from reportlab.graphics import renderPDF, renderSVG
from reportlab.graphics.charts.lineplots import LinePlot
from reportlab.graphics.shapes import Drawing, Line, Rect, String
from reportlab.lib import colors

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "assets" / "profiles" / "decoded" / "yamaha-hs8"
profile = json.loads((OUT / "profile.json").read_text(encoding="utf-8"))
curves = profile["eq_block"]["curves"]
width, height = 1440, 1080
drawing = Drawing(width, height)
drawing.add(Rect(0, 0, width, height, fillColor=colors.HexColor("#f8fafc"), strokeColor=None))
ink = colors.HexColor("#17283b")
muted = colors.HexColor("#536579")
blue = colors.HexColor("#0075bd")
orange = colors.HexColor("#d26500")


def text(x, y, value, size=16, color=ink, anchor="start"):
    drawing.add(String(x, y, value, fontName="Helvetica", fontSize=size,
                       fillColor=color, textAnchor=anchor))


text(86, 1025, "Yamaha HS8 — decoded profile curves", 31)
text(86, 992, "Raw values from Yamaha HS8.swproj | logarithmic frequency axis", 18, muted)
ticks = [20, 30, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 22000]


def panel(y, pair, title, subtitle):
    text(100, y + 373, title, 22)
    text(100, y + 345, subtitle, 15, muted)
    plot = LinePlot()
    plot.x, plot.y, plot.width, plot.height = 110, y, 1230, 304
    plot.data = [[(math.log10(p["frequency_hz"]), p["value_db"])
                  for p in curves[i]["points"]] for i in pair]
    plot.joinedLines = 1
    plot.lines[0].strokeColor = blue
    plot.lines[1].strokeColor = orange
    plot.lines[0].strokeWidth = 2.5
    plot.lines[1].strokeWidth = 2.5
    for axis in [plot.xValueAxis, plot.yValueAxis]:
        axis.strokeColor = colors.HexColor("#9ba9b7")
        axis.strokeWidth = 0.8
        axis.labels.fontName = "Helvetica"
        axis.labels.fontSize = 14
        axis.labels.fillColor = muted
        axis.visibleGrid = 1
        axis.gridStrokeColor = colors.HexColor("#dce3ea")
        axis.gridStrokeWidth = 0.6
    plot.xValueAxis.valueMin = math.log10(20)
    plot.xValueAxis.valueMax = math.log10(22000)
    plot.xValueAxis.valueSteps = [math.log10(t) for t in ticks]
    plot.xValueAxis.labelTextFormat = lambda v: (f"{round(10**v / 1000):d}k"
                                               if 10**v >= 999 else str(round(10**v)))
    plot.xValueAxis.labels.dy = -9
    plot.yValueAxis.valueMin = -30
    plot.yValueAxis.valueMax = 30
    plot.yValueAxis.valueSteps = list(range(-30, 31, 10))
    plot.yValueAxis.labelTextFormat = "%d"
    drawing.add(plot)
    drawing.add(Line(110, y + 152, 1340, y + 152,
                     strokeColor=colors.HexColor("#8292a2"), strokeWidth=1))
    text(110, y + 319, "Raw value (dB)", 14, muted)
    text(725, y - 51, "Frequency (Hz)", 16, muted, "middle")
    for x, i, color in [(1030, pair[0], blue), (1200, pair[1], orange)]:
        drawing.add(Line(x, y + 370, x + 32, y + 370, strokeColor=color, strokeWidth=3))
        text(x + 42, y + 364, f"Curve {i+1}", 16)


panel(596, (0, 1), "Curves 1 and 2", "Likely correction curves — role and left/right order are unverified")
panel(155, (2, 3), "Curves 3 and 4", "Exact sign inverses of curves 1 and 2; likely measured response")
text(86, 53, "355 points per curve | 20 Hz–22 kHz | no smoothing, scalar offsets or gain limits applied", 15, muted)
text(86, 28, "The graph shows extracted data. It does not reproduce SoundID's final playback filtering.", 15, muted)
renderSVG.drawToFile(drawing, str(OUT / "calibration_curves.svg"))
renderPDF.drawToFile(drawing, str(OUT / "calibration_curves.pdf"))
print(OUT / "calibration_curves.svg")

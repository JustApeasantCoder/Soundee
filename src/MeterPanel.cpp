#include "MeterPanel.h"
#include "UiStyle.h"

namespace soundee {
void MeterPanel::reset() {
    heldDb = displayedDb = rmsDb = {-90, -90}; holdUntil = {}; clipped = {}; repaint();
}
void MeterPanel::clearClips() { clipped = {}; repaint(); }
void MeterPanel::update(const StereoReading& reading, const juce::String& source, bool connected) {
    const auto now = juce::Time::getMillisecondCounter(); online = connected;
    if (sourceName != source) { const auto previousClips = clipped; reset(); clipped = previousClips; }
    sourceName = source;
    const bool fresh = reading.updatedAt != 0 && now - reading.updatedAt < 250;
    for (size_t c = 0; c < 2; ++c) {
        const auto target = fresh ? juce::Decibels::gainToDecibels(reading.peak[c], -90.f) : -90.f;
        displayedDb[c] = std::max(target, displayedDb[c] - 0.7f);
        rmsDb[c] += 0.28f * ((fresh ? juce::Decibels::gainToDecibels(reading.rms[c], -90.f) : -90.f) - rmsDb[c]);
        if (target >= heldDb[c]) { heldDb[c] = target; holdUntil[c] = now + 1200; }
        else if (static_cast<juce::int32>(now - holdUntil[c]) > 0) heldDb[c] = std::max(displayedDb[c], heldDb[c] - 0.7f);
        clipped[c] = clipped[c] || reading.clipped[c];
    }
    repaint();
}
void MeterPanel::paint(juce::Graphics& g) {
    const auto green = ui::accent, grey = ui::muted;
    g.fillAll(ui::background); g.setColour(ui::border); g.drawVerticalLine(0, 0, static_cast<float>(getHeight()));
    g.setColour(juce::Colours::white); g.setFont(14.f); g.drawText("Output", 10, 12, getWidth() - 20, 22, juce::Justification::centred);
    const int top = 86, bottom = getHeight() - 63, height = std::max(1, bottom - top);
    const float left = 22.f, spacing = 30.f, width = 13.f;
    auto y = [&](float db) { return static_cast<float>(top) + juce::jlimit(0.f, 1.f, -db / 60.f) * height; };
    for (int db : {0, -6, -12, -24, -36, -48, -60}) {
        g.setColour(grey); g.setFont(11.f);
        g.drawText(juce::String(db), getWidth() - 35, static_cast<int>(y(static_cast<float>(db))) - 7, 31, 14, juce::Justification::centredLeft);
    }
    for (size_t c = 0; c < 2; ++c) {
        const float x = left + static_cast<float>(c) * spacing;
        g.setColour(clipped[c] ? juce::Colour(0xfffa3f46) : juce::Colour(0xff343934));
        g.fillRoundedRectangle(x - 2, 47.f, width + 4, 8, 2.f);
        g.setFont(11.f); g.setColour(clipped[c] ? juce::Colour(0xffff5c62) : grey);
        g.drawText(clipped[c] ? "CLIP" : (c == 0 ? "L" : "R"), static_cast<int>(x) - 11, 60, 36, 18, juce::Justification::centred);
        g.setColour(juce::Colour(0xff363b36)); g.fillRoundedRectangle(x, static_cast<float>(top), width, static_cast<float>(height), 2.f);
        for (const auto& zone : {std::pair<float, juce::Colour>{-60.f, green}, {-6.f, juce::Colour(0xffe6d858)}, {-1.f, juce::Colour(0xfff15c45)}}) {
            const float low = zone.first;
            const float high = low == -60.f ? -6.f : low == -6.f ? -1.f : 0.f;
            if (displayedDb[c] > low) {
                const float upper = y(std::min(high, displayedDb[c]));
                g.setColour(zone.second); g.fillRect(x, upper, width, y(low) - upper);
            }
        }
        g.setColour(juce::Colour(0xffcef5c3)); g.fillRect(x, y(heldDb[c]), width, 2.f);
        g.setColour(juce::Colours::white); g.setFont(11.f);
        g.drawText(displayedDb[c] <= -89 ? "-inf" : juce::String(displayedDb[c], 1), static_cast<int>(x) - 13, bottom + 10, 40, 18, juce::Justification::centred);
    }
    g.setColour(grey); g.setFont(11.f); g.drawText("True peak / dBTP", 5, getHeight() - 27, getWidth() - 10, 13, juce::Justification::centred);
    if (reductionDb > .05f) g.drawText("Limiting " + juce::String(reductionDb, 1) + " dB", 5, getHeight() - 42, getWidth() - 10, 13, juce::Justification::centred);
    g.setFont(11.f); g.drawText(online ? sourceName : "Meter unavailable", 5, getHeight() - 14, getWidth() - 10, 12, juce::Justification::centred);
}
void MeterPanel::mouseDown(const juce::MouseEvent& event) {
    if (event.y < 80 && onResetClips) onResetClips();
}
}

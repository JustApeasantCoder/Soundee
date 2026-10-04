#include "MainComponent.h"
#include <cmath>

namespace soundee {
namespace {
const auto black = ui::background, ink = ui::ink, muted = ui::muted,
    green = ui::accent, pale = juce::Colour(0xffb8e7a5), border = ui::border, pink = ui::pink;
constexpr int sidebar = ui::sidebar, meterWidth = ui::meterWidth;
}
juce::Rectangle<float> MainComponent::graphBounds() const {
    return ui::Layout(getWidth(), getHeight()).graph();
}
void MainComponent::paint(juce::Graphics& g) {
    const ui::Layout layout(getWidth(), getHeight());
    const int right = layout.right, bottom = layout.bottom;
    const auto graph = graphBounds();
    g.fillAll(black);
    g.setColour(black); g.fillRect(0, 0, sidebar, bottom);
    g.setColour(border); g.drawVerticalLine(sidebar, 0, float(bottom));
    g.drawHorizontalLine(90, 0, float(getWidth()));
    auto text = [&](const juce::String& value, juce::Rectangle<int> bounds, float size, juce::Colour colour) {
        g.setColour(colour); g.setFont(size); g.drawText(value, bounds, juce::Justification::centredLeft, true);
    };
    auto card = [&](juce::Rectangle<int> bounds) {
        g.setColour(ui::surface); g.fillRoundedRectangle(bounds.toFloat(), 10.f);
        g.setColour(border); g.drawRoundedRectangle(bounds.toFloat().reduced(.5f), 10.f, 1.f);
    };
    card(layout.custom()); card(layout.calibration()); card(layout.output());
    text("Soundee", {20, 16, 220, 36}, 28.f, ink);
    text("Speaker & headphone EQ", {22, 55, 220, 22}, 13.f, muted);
    text("Listening profiles", {20, 102, 220, 24}, 16.f, ink);
    const auto* preset = library.selected();
    text(preset ? preset->name : "Flat response", {sidebar + 24, 16, right - sidebar - 526, 34}, 24.f, ink);
    text("Compare", {right - 347, 20, 70, 30}, 12.f, muted);
    text("Playback device", {20, bottom - 230, sidebar - 40, 23}, 14.f, ink);
    text("Auto-select its linked profile", {20, bottom - 41, sidebar - 40, 19}, 12.f, muted);
    text("EQ response", {sidebar + 24, 101, 150, 25}, 17.f, ink);
    const int legend = right - 505;
    text("Left", {legend, 105, 42, 20}, 13.f, green);
    text("Right", {legend + 54, 105, 45, 20}, 13.f, pale);
    text("Custom EQ", {legend + 116, 105, 86, 20}, 13.f, pink);
    text("Applied EQ before output gain", {sidebar + 24, int(graph.getBottom()) + 5, 350, 20}, 12.f, muted);
    text("Click to toggle EQ", {right + 8, bottom - 31, meterWidth - 16, 20}, 11.f, muted);
    g.setColour(black); g.fillRect(graph);
    auto x = [&](double f) { return graph.getX() + static_cast<float>(std::log(f / 20) / std::log(1100.0)) * graph.getWidth(); };
    auto y = [&](double db) { return graph.getCentreY() - static_cast<float>(db / 48) * graph.getHeight(); };
    for (int db = -24; db <= 24; db += 3) {
        g.setColour(db == 0 ? muted.withAlpha(.7f) : border.withAlpha(db % 6 == 0 ? .75f : .35f));
        g.drawHorizontalLine(static_cast<int>(y(db)), graph.getX(), graph.getRight());
        if (db % 6 == 0) {
            g.setColour(muted); g.setFont(12.f); const auto label = (db > 0 ? "+" : "") + juce::String(db) + " dB";
            g.drawText(label, static_cast<int>(graph.getRight()) + 6, static_cast<int>(y(db)) - 9, 48, 18, juce::Justification::centredRight);
        }
    }
    for (double decade : {10., 100., 1000., 10000.}) for (int multiple = 1; multiple <= 9; ++multiple) {
        const double f = decade * multiple; if (f < 20 || f > 22000) continue;
        g.setColour(border.withAlpha(multiple == 1 ? .9f : .4f)); g.drawVerticalLine(static_cast<int>(x(f)), graph.getY(), graph.getBottom());
    }
    for (double f : {20., 100., 1000., 10000.}) {
        g.setColour(muted); g.setFont(12.f); const auto label = f >= 1000 ? juce::String(f / 1000, 0) + " kHz" : juce::String(f, 0) + " Hz";
        g.drawText(label, static_cast<int>(x(f)) - (f == 20 ? 0 : 27), static_cast<int>(graph.getY()) - 20, 60, 17, juce::Justification::left);
    }
    g.saveState(); g.reduceClipRegion(graph.toNearestInt());
    const auto s = settings(); const auto eq = eqCoefficients(customBands, rate.load(), s.customEnabled);
    if (bandCurvesButton.getToggleState() && s.customEnabled) for (int index = 0; index < static_cast<int>(customBands.size()); ++index) {
        const auto& band = customBands[static_cast<size_t>(index)]; if (!band.enabled) continue;
        const auto coefficients = bandCoefficients(band, rate.load()); juce::Path path;
        for (int i = 0; i <= 400; ++i) { const double f = 20 * std::pow(1100., i / 400.); const auto db = bandDb(coefficients, f, rate.load());
            if (i == 0) path.startNewSubPath(x(f), y(db)); else path.lineTo(x(f), y(db)); }
        g.setColour(juce::Colour::fromHSV(static_cast<float>(index) / 12 + .75f, .45f, .8f, index == selectedBand ? .65f : .3f));
        g.strokePath(path, juce::PathStrokeType(index == selectedBand ? 1.3f : 1.f));
    }
    if (profile) for (int c = 0; c < 2; ++c) {
        juce::Path applied;
        for (int i = 0; i <= 1000; ++i) {
            const double f = 20 * std::pow(1100., i / 1000.);
            const double value = bypassButton.getToggleState() ? 0 : correctionDb(*profile, c, f, s) + customEqDb(eq, f, rate.load());
            if (i == 0) applied.startNewSubPath(x(f), y(value)); else applied.lineTo(x(f), y(value));
        }
        auto fill = applied; fill.lineTo(graph.getRight(), y(0)); fill.lineTo(graph.getX(), y(0)); fill.closeSubPath();
        const auto colour = c == 0 ? green : pale; g.setColour(colour.withAlpha(0.055f)); g.fillPath(fill);
        g.setColour(colour); g.strokePath(applied, juce::PathStrokeType(1.8f));
    }
    if (s.customEnabled && !customBands.empty()) {
        juce::Path custom;
        for (int i = 0; i <= 800; ++i) {
            const double f = 20 * std::pow(1100., i / 800.);
            if (i == 0) custom.startNewSubPath(x(f), y(customEqDb(eq, f, rate.load()))); else custom.lineTo(x(f), y(customEqDb(eq, f, rate.load())));
        }
        g.setColour(pink.withAlpha(0.65f)); g.strokePath(custom, juce::PathStrokeType(1.4f));
        for (int i = 0; i < static_cast<int>(customBands.size()); ++i) {
            const auto& band = customBands[static_cast<size_t>(i)]; const float px = x(band.frequency), py = y(band.gain), radius = i == selectedBand ? 9.f : 7.f;
            g.setColour(band.enabled ? pink : muted); g.fillEllipse(px - radius, py - radius, radius * 2, radius * 2);
            if (i == selectedBand) { g.setColour(ink); g.drawEllipse(px - 12, py - 12, 24, 24, 1.f); }
            g.setColour(black); g.setFont(10.f); g.drawText(juce::String(i + 1), static_cast<int>(px) - 9, static_cast<int>(py) - 8, 18, 16, juce::Justification::centred);
        }
    }
    g.restoreState();
    const auto custom = layout.custom(), calibration = layout.calibration(), output = layout.output();
    text(customBands.empty() ? "Add a band to shape your sound" : "Click to add a band. Drag to tune. Scroll to change width.",
        {custom.getX() + 16, custom.getY() + 36, custom.getWidth() - 32, 20}, 12.f, muted);
    text("Filter type", {custom.getX() + 16, custom.getY() + 64, 132, 20}, 13.f, muted);
    text("Calibration", {calibration.getX() + 16, calibration.getY() + 8, 118, 25}, 16.f, ink);
    text(profile && !profile->flatResponse ? profile->name + "  /  correction only" : "No calibration loaded  /  add a calibration to use these controls",
        {calibration.getX() + 140, calibration.getY() + 10, calibration.getWidth() - 156, 22}, 12.f, muted);
    text("Output & protection", {output.getX() + 16, output.getY() + 7, 230, 24}, 16.f, ink);
    const int third = (output.getWidth() - 32) / 3;
    text(limiterButton.getToggleState() ? (systemState.stage == ApoStage::preMix ? "Limits each app before mixing" : "Adds ~6 ms of lookahead") : "Off / no limiter delay",
        {output.getX() + 16, output.getY() + 73, third - 12, 20}, 12.f, muted);
    text("Maximum sample level", {output.getX() + 16 + third, output.getY() + 73, third - 12, 20}, 12.f, muted);
    text("Auto " + juce::String(designedHeadroom - headroomOffset.getValue(), 1) + " dB  /  Applied " + juce::String(designedHeadroom, 1) + " dB",
        {output.getX() + 16 + 2 * third, output.getY() + 73, third - 4, 20}, 12.f, muted);
}
void MainComponent::resized() {
    const ui::Layout layout(getWidth(), getHeight());
    const int right = layout.right, bottom = layout.bottom;
    const auto custom = layout.custom(), calibration = layout.calibration(), output = layout.output();
    profileList.setBounds(8, 134, sidebar - 16, bottom - 384);
    linkedOutput.setBounds(20, bottom - 202, sidebar - 40, 34);
    importButton.setBounds(20, bottom - 154, sidebar - 40, 34);
    flatButton.setBounds(20, bottom - 112, sidebar - 40, 34);
    followWindows.setBounds(16, bottom - 73, sidebar - 32, 28);
    systemButton.setBounds(right - 182, 20, 150, 32);
    menuButton.setBounds(right + 14, 20, meterWidth - 28, 32);
    undoButton.setBounds(right - 490, 20, 58, 32); redoButton.setBounds(right - 426, 20, 58, 32);
    aButton.setBounds(right - 275, 20, 32, 32); bButton.setBounds(right - 237, 20, 32, 32);
    snapshotButton.setBounds(right - 342, 56, 136, 25);
    healthLabel.setBounds(sidebar + 22, 57, right - sidebar - 382, 24);
    bandCurvesButton.setBounds(right - 188, 101, 172, 27);
    meterPanel.setBounds(right, 91, meterWidth, bottom - 188);
    bypassButton.setBounds(right + 10, bottom - 84, meterWidth - 20, 46);
    customButton.setBounds(custom.getX() + 12, custom.getY() + 7, 150, 27);
    bandSelector.setBounds(custom.getRight() - 300, custom.getY() + 8, 174, 30);
    addBandButton.setBounds(custom.getRight() - 116, custom.getY() + 8, 100, 30);
    bandType.setBounds(custom.getX() + 16, custom.getY() + 88, 132, 30);
    const int fieldWidth = (custom.getWidth() - 296) / 3;
    juce::Slider* sliders[] = {&bandFrequency, &bandGain, &bandQ};
    juce::Label* labels[] = {&frequencyLabel, &gainLabel, &qLabel};
    for (int i = 0; i < 3; ++i) {
        const int left = custom.getX() + 164 + i * fieldWidth;
        labels[i]->setBounds(left, custom.getY() + 64, fieldWidth - 12, 20);
        sliders[i]->setBounds(left, custom.getY() + 88, fieldWidth - 12, 30);
    }
    bandEnabled.setBounds(custom.getRight() - 120, custom.getY() + 62, 108, 24);
    removeBandButton.setBounds(custom.getRight() - 116, custom.getY() + 88, 100, 30);
    const int limitsWidth = (calibration.getWidth() - 32) / 4;
    juce::Slider* limits[] = {&amount, &boost, &low, &high};
    juce::Label* limitLabels[] = {&amountLabel, &boostLabel, &lowLabel, &highLabel};
    const bool calibrated = profile && !profile->flatResponse;
    for (int i = 0; i < 4; ++i) {
        const int left = calibration.getX() + 16 + i * limitsWidth;
        limitLabels[i]->setBounds(left, calibration.getY() + 39, limitsWidth - 12, 20);
        limits[i]->setBounds(left, calibration.getY() + 64, limitsWidth - 12, 30);
        limits[i]->setEnabled(calibrated && libraryWritable);
        limitLabels[i]->setColour(juce::Label::textColourId, calibrated ? muted : muted.withAlpha(.5f));
    }
    const int third = (output.getWidth() - 32) / 3, outputLeft = output.getX() + 16;
    limiterButton.setBounds(outputLeft - 4, output.getY() + 39, third - 12, 30);
    limiterLabel.setBounds(outputLeft + third, output.getY() + 9, third - 12, 20);
    limiterCeiling.setBounds(outputLeft + third, output.getY() + 39, third - 12, 30);
    headroomOffsetLabel.setBounds(outputLeft + 2 * third, output.getY() + 9, third - 8, 20);
    headroomOffset.setBounds(outputLeft + 2 * third, output.getY() + 39, third - 8, 30);
    statusLabel.setBounds(sidebar + 18, bottom - 27, right - sidebar - 36, 22);
    exportButton.setVisible(false); impulseButton.setVisible(false); logsButton.setVisible(false); swapButton.setVisible(false);
}
}

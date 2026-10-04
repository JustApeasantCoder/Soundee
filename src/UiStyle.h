#pragma once
#include <JuceHeader.h>

namespace soundee::ui {
inline const juce::Colour background {0xff070907}, surface {0xff101410},
    field {0xff151a15}, ink {0xffeef2ee}, muted {0xffb1b8b1},
    accent {0xff75e355}, border {0xff353b35}, pink {0xffe65edd};
constexpr int sidebar = 260, meterWidth = 120;

struct Layout {
    explicit Layout(int width, int height) : right(width - meterWidth), bottom(height) {}
    int right, bottom;
    juce::Rectangle<int> custom() const { return {sidebar + 16, bottom - 388, right - sidebar - 32, 130}; }
    juce::Rectangle<int> calibration() const { return {sidebar + 16, bottom - 250, right - sidebar - 32, 104}; }
    juce::Rectangle<int> output() const { return {sidebar + 16, bottom - 138, right - sidebar - 32, 102}; }
    juce::Rectangle<float> graph() const { return {float(sidebar + 26), 144.f, float(right - sidebar - 84), float(bottom - 560)}; }
};

class LookAndFeel : public juce::LookAndFeel_V4 {
public:
    LookAndFeel() {
        setColourScheme(getDarkColourScheme());
        setColour(juce::TextButton::buttonColourId, juce::Colour(0xff171d17));
        setColour(juce::TextButton::textColourOffId, ink);
        setColour(juce::TextButton::buttonOnColourId, accent);
        setColour(juce::TextButton::textColourOnId, background);
        setColour(juce::Slider::thumbColourId, accent);
        setColour(juce::Slider::trackColourId, accent);
        setColour(juce::Slider::backgroundColourId, border);
        setColour(juce::Slider::textBoxTextColourId, ink);
        setColour(juce::Slider::textBoxBackgroundColourId, field);
        setColour(juce::Slider::textBoxOutlineColourId, border);
        setColour(juce::ToggleButton::textColourId, ink);
        setColour(juce::ToggleButton::tickColourId, accent);
        setColour(juce::ComboBox::backgroundColourId, field);
        setColour(juce::ComboBox::textColourId, ink);
        setColour(juce::ComboBox::outlineColourId, border);
        setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        setColour(juce::PopupMenu::backgroundColourId, surface);
        setColour(juce::PopupMenu::textColourId, ink);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, border);
        setColour(juce::TooltipWindow::backgroundColourId, surface);
        setColour(juce::TooltipWindow::textColourId, ink);
        setColour(juce::TooltipWindow::outlineColourId, border);
    }
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return juce::Font(14.f); }
    juce::Font getComboBoxFont(juce::ComboBox&) override { return juce::Font(14.f); }
    juce::Font getLabelFont(juce::Label&) override { return juce::Font(14.f); }
    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
        float position, float minimum, float maximum, juce::Slider::SliderStyle style, juce::Slider& slider) override {
        g.beginTransparencyLayer(slider.isEnabled() ? 1.f : .3f);
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, position, minimum, maximum, style, slider);
        g.endTransparencyLayer();
    }
    juce::Label* createSliderTextBox(juce::Slider& slider) override {
        auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
        label->setJustificationType(juce::Justification::centred);
        return label;
    }
};
}

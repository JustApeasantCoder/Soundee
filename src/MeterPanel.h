#pragma once
#include "AudioMeter.h"

namespace soundee {
class MeterPanel : public juce::Component, public juce::SettableTooltipClient {
public:
    void update(const StereoReading&, const juce::String& source, bool connected);
    void reset();
    void clearClips();
    bool isClipping(size_t channel) const { return channel < clipped.size() && clipped[channel]; }
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    std::function<void()> onResetClips;
    std::array<float, 2> displayedDb {-90, -90};
    float reductionDb = 0;
private:
    juce::String sourceName = "Output";
    std::array<float, 2> rmsDb {-90, -90}, heldDb {-90, -90};
    std::array<juce::uint32, 2> holdUntil{};
    std::array<bool, 2> clipped{};
    bool online = false;
};
}

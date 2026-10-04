#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <mutex>
#include <memory>
#include "NativeDsp.h"

namespace soundee {
class Diagnostics;
struct StereoReading {
    std::array<float, 2> peak{}, rms{};
    std::array<bool, 2> clipped{};
    juce::uint32 updatedAt = 0;
};
class StereoLevels {
public:
    void publish(const std::array<float, 2>& peak, const std::array<float, 2>& rms);
    void publish(const juce::AudioBuffer<float>&, int samples);
    StereoReading consume();
    void resetClips();
    void reset();
private:
    std::array<TruePeakDetector, 2> truePeak;
    std::array<std::atomic<float>, 2> peaks{}, rmsValues{};
    std::array<std::atomic<bool>, 2> clips{};
    std::atomic<juce::uint32> updated {0};
};
class SystemAudioMeter : private juce::Thread {
public:
    explicit SystemAudioMeter(Diagnostics&);
    ~SystemAudioMeter() override;
    void selectEndpoint(const juce::String& endpointId);
    StereoLevels levels;
    std::atomic<bool> online {false};
    std::atomic<double> sampleRate {0};
private:
    void run() override;
    struct Native;
    std::unique_ptr<Native> native;
    Diagnostics& logs;
    std::mutex commandMutex;
    juce::String requestedId;
    std::atomic<unsigned> generation {0};
};
}

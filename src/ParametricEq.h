#pragma once
#include <JuceHeader.h>
#include <array>
#include <vector>

namespace soundee {
enum class BandType { bell, lowShelf, highShelf };
struct EqBand {
    BandType type = BandType::bell;
    double frequency = 1000, gain = 0, q = 0.7071067811865476;
    bool enabled = true;
};
using Biquad = std::array<double, 6>;
juce::String bandTypeName(BandType);
Biquad bandCoefficients(const EqBand&, double sampleRate);
double bandDb(const Biquad&, double frequency, double sampleRate);
std::vector<Biquad> eqCoefficients(const std::vector<EqBand>&, double sampleRate, bool enabled);
double customEqDb(const std::vector<Biquad>&, double frequency, double sampleRate);
double customEqBoostBound(const std::vector<EqBand>&, double sampleRate, bool enabled);
}

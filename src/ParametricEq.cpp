#include "ParametricEq.h"
#include <complex>
#include <cmath>

namespace soundee {
juce::String bandTypeName(BandType type) {
    return type == BandType::lowShelf ? "Low Shelf" : type == BandType::highShelf ? "High Shelf" : "Bell";
}
Biquad bandCoefficients(const EqBand& band, double rate) {
    const auto f = juce::jlimit(2.0, rate * 0.49, band.frequency);
    const auto q = juce::jlimit(0.2, 12.0, band.q);
    const auto gain = juce::Decibels::decibelsToGain(juce::jlimit(-18.0, 18.0, band.gain));
    using Coefficients = juce::dsp::IIR::ArrayCoefficients<double>;
    if (band.type == BandType::lowShelf) return Coefficients::makeLowShelf(rate, f, q, gain);
    if (band.type == BandType::highShelf) return Coefficients::makeHighShelf(rate, f, q, gain);
    return Coefficients::makePeakFilter(rate, f, q, gain);
}
double bandDb(const Biquad& a, double f, double rate) {
    const auto z = std::polar(1.0, -juce::MathConstants<double>::twoPi * juce::jlimit(0.0, rate * 0.5, f) / rate);
    const auto response = (a[0] + a[1] * z + a[2] * z * z) / (a[3] + a[4] * z + a[5] * z * z);
    return 20 * std::log10(std::max(1e-12, std::abs(response)));
}
std::vector<Biquad> eqCoefficients(const std::vector<EqBand>& bands, double rate, bool enabled) {
    std::vector<Biquad> result;
    if (enabled) for (const auto& band : bands) if (band.enabled) result.push_back(bandCoefficients(band, rate));
    return result;
}
double customEqDb(const std::vector<Biquad>& coefficients, double f, double rate) {
    double db = 0; for (const auto& a : coefficients) db += bandDb(a, f, rate); return db;
}
double customEqBoostBound(const std::vector<EqBand>& bands, double rate, bool enabled) {
    double bound = 0;
    if (enabled) for (const auto& band : bands) if (band.enabled) {
        const auto a = bandCoefficients(band, rate);
        double peak = std::max({0.0, bandDb(a, 0, rate), bandDb(a, rate * 0.5, rate), bandDb(a, band.frequency, rate)});
        // Include shelf resonance, even for a shelf whose nominal gain is negative.
        for (int i = 0; i <= 2048; ++i) {
            const double f = juce::jlimit(0.0, rate * 0.5, band.frequency * std::pow(4.0, (i - 1024) / 1024.0));
            peak = std::max(peak, bandDb(a, f, rate));
        }
        bound += peak;
    }
    return bound;
}
}

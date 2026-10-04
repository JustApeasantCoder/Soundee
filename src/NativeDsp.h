#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace soundee {
// Four polyphase, Blackman-windowed sinc reconstructions. State spans packets;
// the original samples are included so reconstruction never hides sample overs.
class TruePeakDetector {
public:
    static constexpr int taps = 64, phases = 4;
    TruePeakDetector() {
        for (int p = 0; p < phases; ++p) {
            double sum = 0;
            for (int k = 0; k < taps; ++k) {
                const double x = k - (31.0 + p / 4.0);
                const double sinc = std::abs(x) < 1e-12 ? 1 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
                const double window = .42 - .5 * std::cos(2 * std::numbers::pi * k / (taps - 1))
                    + .08 * std::cos(4 * std::numbers::pi * k / (taps - 1));
                coefficients[p][k] = sinc * window; sum += coefficients[p][k];
            }
            for (auto& c : coefficients[p]) c /= sum;
        }
    }
    double push(double value) noexcept {
        if (!std::isfinite(value)) value = 0;
        history[position] = value;
        double peak = std::abs(value);
        for (int p = 0; p < phases; ++p) {
            double reconstructed = 0;
            for (int k = 0; k < taps; ++k) reconstructed += coefficients[p][k] * history[(position - k + taps) % taps];
            peak = std::max(peak, std::abs(reconstructed));
        }
        position = (position + 1) % taps; return peak;
    }
    void reset() noexcept { history.fill(0); position = 0; }
private:
    std::array<std::array<double, taps>, phases> coefficients{};
    std::array<double, taps> history{};
    int position = 0;
};

// Stereo-linked, true-peak-aware lookahead limiter. It has a sample ceiling;
// the reconstruction guard reduces inter-sample overs without claiming a
// certified true-peak ceiling for every downstream reconstruction filter.
class LookaheadLimiter {
public:
    void prepare(double sampleRate, double ceilingDb) noexcept {
        rate = std::clamp(sampleRate, 8000., 384000.);
        delay = static_cast<int>(std::ceil(rate * .005)) + TruePeakDetector::taps;
        ceiling = std::pow(10., std::clamp(ceilingDb, -12., 0.) / 20.);
        target = ceiling * std::pow(10., -.5 / 20.);
        release = 1 - std::exp(-1. / (.1 * rate)); reset();
    }
    void reset() noexcept {
        for (auto& channel : audio) channel.fill(0);
        for (auto& detector : detectors) detector.reset();
        head = tail = time = 0; gain = 1;
    }
    int latency() const noexcept { return delay; }
    double reductionDb() const noexcept { return -20 * std::log10(std::max(1e-12, gain)); }
    std::array<float, 2> process(float left, float right) noexcept {
        const std::array<double, 2> input {std::isfinite(left) ? left : 0., std::isfinite(right) ? right : 0.};
        const double peak = std::max(detectors[0].push(input[0]), detectors[1].push(input[1]));
        while (head != tail && maxima[(tail - 1) % capacity].value <= peak) --tail;
        maxima[tail++ % capacity] = {time, peak};
        while (head != tail && maxima[head % capacity].at + static_cast<uint64_t>(delay) < time) ++head;
        const double wanted = std::min(1., target / std::max(1e-12, maxima[head % capacity].value));
        gain = std::min(wanted, gain + release * (1 - gain));
        std::array<float, 2> output{};
        for (size_t c = 0; c < 2; ++c) {
            const auto index = time % capacity;
            const double delayed = time >= static_cast<uint64_t>(delay) ? audio[c][(time - delay) % capacity] : 0;
            audio[c][index] = input[c];
            output[c] = static_cast<float>(std::clamp(delayed * gain, -ceiling, ceiling));
        }
        ++time; return output;
    }
private:
    static constexpr size_t capacity = 4096;
    struct Maximum { uint64_t at = 0; double value = 0; };
    std::array<std::array<double, capacity>, 2> audio{};
    std::array<Maximum, capacity> maxima{};
    std::array<TruePeakDetector, 2> detectors;
    uint64_t head = 0, tail = 0, time = 0;
    int delay = 304;
    double rate = 48000, ceiling = .89125, target = .8414, release = .0002, gain = 1;
};
}

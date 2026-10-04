#include "Filters.h"
#include "NativeBridge.h"
#include <complex>
#include <cmath>

namespace soundee {
double correctionDb(const Profile& profile, int channel, double f, const Settings& s) {
    channel = s.swap ? 1 - channel : channel;
    const double raw = interpolate(profile.curves[static_cast<size_t>(channel)], f);
    double taper = 1.0;
    if (f < s.low) taper = juce::jlimit(0.0, 1.0, std::log2(std::max(f, 1.0) / (s.low / 2.0)));
    if (f > s.high) taper = juce::jlimit(0.0, 1.0, std::log2(s.high * 2.0 / f));
    taper = 0.5 - 0.5 * std::cos(juce::MathConstants<double>::pi * taper);
    return juce::jlimit(-24.0, s.maximumBoost, raw) * s.amount * taper;
}
DesignedFilters designFilters(const Profile& profile, const Settings& s, double rate, int order) {
    const int n = 1 << order;
    juce::dsp::FFT fft(order);
    using Complex = std::complex<float>;
    std::vector<Complex> spectrum(static_cast<size_t>(n)), cepstrum(static_cast<size_t>(n));
    DesignedFilters result; result.sampleRate = rate; result.impulse.setSize(2, n);
    const auto custom = eqCoefficients(s.bands, rate, s.customEnabled);
    double largestBoost = 0;
    for (int channel = 0; channel < 2; ++channel) {
        for (int bin = 0; bin <= n / 2; ++bin) {
            double f = bin * rate / n;
            double db = correctionDb(profile, channel, f, s) + customEqDb(custom, f, rate);
            largestBoost = std::max(largestBoost, db);
            spectrum[static_cast<size_t>(bin)] = Complex(static_cast<float>(db * std::log(10.0) / 20.0), 0);
            if (bin > 0 && bin < n / 2) spectrum[static_cast<size_t>(n - bin)] = spectrum[static_cast<size_t>(bin)];
        }
        fft.perform(spectrum.data(), cepstrum.data(), true);
        for (int i = 1; i < n / 2; ++i) cepstrum[static_cast<size_t>(i)] *= 2.0f;
        for (int i = n / 2 + 1; i < n; ++i) cepstrum[static_cast<size_t>(i)] = {};
        fft.perform(cepstrum.data(), spectrum.data(), false);
        for (auto& value : spectrum) value = std::exp(value);
        fft.perform(spectrum.data(), cepstrum.data(), true);
        for (int i = 0; i < n; ++i) result.impulse.setSample(channel, i, cepstrum[static_cast<size_t>(i)].real());
    }
    result.preampDb = appliedHeadroomDb(profile, s, rate);
    return result;
}
double combinedPeakDb(const Profile& p, const Settings& s, double rate) {
    rate = std::isfinite(rate) && rate >= 8000 ? rate : 48000;
    const double nyquist = rate * .5;
    const auto coefficients = eqCoefficients(s.bands, rate, s.customEnabled);
    auto response = [&](int c, double f) { return correctionDb(p, c, f, s) + customEqDb(coefficients, f, rate); };
    double peak = 0;
    for (int c = 0; c < 2; ++c) {
        peak = std::max({peak, response(c, 0), response(c, nyquist)});
        std::vector<double> frequencies {1., nyquist};
        for (int i = 0; i <= 4096; ++i) frequencies.push_back(std::pow(nyquist, i / 4096.));
        for (const auto& point : p.curves[c].points) if (point.frequency < nyquist) frequencies.push_back(point.frequency);
        for (double f : {s.low / 2, s.low, s.high, s.high * 2}) if (f >= 1 && f < nyquist) frequencies.push_back(f);
        for (const auto& band : s.bands) if (s.customEnabled && band.enabled) frequencies.push_back(std::min(band.frequency, rate * .49));
        std::sort(frequencies.begin(), frequencies.end());
        std::vector<double> values; values.reserve(frequencies.size());
        for (double f : frequencies) { const double db = response(c, f); values.push_back(db); peak = std::max(peak, db); }
        // Refine sampled local maxima, including the resonances of high-Q shelves.
        for (size_t i = 1; i + 1 < frequencies.size(); ++i) if (values[i] >= values[i-1] && values[i] > values[i+1]) {
            double a = std::log(frequencies[i-1]), b = std::log(frequencies[i+1]);
            for (int step = 0; step < 24; ++step) {
                const double l = a + (b-a) / 3, r = b - (b-a) / 3;
                const double vl = response(c, std::exp(l)), vr = response(c, std::exp(r));
                peak = std::max({peak, vl, vr}); if (vl < vr) a = l; else b = r;
            }
        }
    }
    return peak;
}
juce::AudioBuffer<float> nativeCalibrationImpulse(const Profile& profile, const Settings& settings, double rate) {
    // Reference for the pinned Equalizer APO 1.4.2 GraphicEQFilter: mirrored
    // 32768 gain samples, minimum-phase cepstrum, 16384-point cosine window.
    // Used to distinguish backend/configuration errors from finite-filter
    // deviation from the ideal calibration curve during offline verification.
    constexpr int length = 16384, n = length * 2;
    juce::AudioBuffer<float> result(2, length); result.clear();
    if (profile.flatResponse) { result.setSample(0, 0, 1); result.setSample(1, 0, 1); return result; }
    juce::dsp::FFT fft(15); using Complex = std::complex<float>;
    std::vector<Complex> spectrum(n), cepstrum(n);
    for (int c = 0; c < 2; ++c) {
        Curve exported;
        for (const auto& point : profile.curves[c].points) exported.points.push_back({point.frequency,
            std::round(correctionDb(profile, c, point.frequency, settings) * 1e6) / 1e6, 0});
        for (int i = 0; i < length; ++i) {
            const double f = i * rate / n;
            const double db = interpolate(exported, f);
            spectrum[i] = spectrum[n - 1 - i] = Complex(static_cast<float>(db * std::log(10.) / 20), 0);
        }
        fft.perform(spectrum.data(), cepstrum.data(), true);
        for (int i = 1; i < length; ++i) cepstrum[i] += std::conj(cepstrum[n-i]);
        for (int i = length + 1; i < n; ++i) cepstrum[i] = {};
        cepstrum[length] = std::conj(cepstrum[length]);
        fft.perform(cepstrum.data(), spectrum.data(), false); for (auto& v : spectrum) v = std::exp(v);
        fft.perform(spectrum.data(), cepstrum.data(), true);
        for (int i = 0; i < length; ++i) result.setSample(c, i, static_cast<float>(cepstrum[i].real() * .5 * (1 + std::cos(juce::MathConstants<double>::pi * i / length))));
    }
    return result;
}
double safeHeadroomDb(const Profile& p, const Settings& s, double rate) {
    const double boost = combinedPeakDb(p, s, rate);
    return boost > .001 ? -(boost + 1) : 0;
}
double appliedHeadroomDb(const Profile& p, const Settings& s, double rate) {
    return safeHeadroomDb(p, s, rate) + s.headroomOffset;
}
juce::String apoConfiguration(const Profile& p, const Settings& s, bool bypass, double rate) {
    const double preamp = appliedHeadroomDb(p, s, rate);
    juce::String text = "# Soundee stereo calibration\n# Enable using Include: soundee.txt in Equalizer APO.\n";
    text += "Channel: L R\nPreamp: " + juce::String(preamp, 5) + " dB\n";
    if (bypass) return text + "Channel: ALL\n";
    for (int c = 0; c < 2 && !p.flatResponse; ++c) {
        text += c == 0 ? "Channel: L\nGraphicEQ: " : "Channel: R\nGraphicEQ: ";
        bool first = true;
        for (const auto& point : p.curves[static_cast<size_t>(c)].points) {
            if (!first) text += "; "; first = false;
            text += juce::String(point.frequency, 5) + " " + juce::String(correctionDb(p, c, point.frequency, s), 6);
        }
        text += "\n";
    }
    text += "Channel: L R\n";
    if (s.customEnabled) for (const auto& band : s.bands) if (band.enabled) {
        const auto type = band.type == BandType::lowShelf ? "LSC" : band.type == BandType::highShelf ? "HSC" : "PK";
        // LSC/HSC specify the shelf centre, matching JUCE's RBJ coefficients.
        // The expression clamps the centre at the actual Windows sample rate.
        text += "Filter: ON " + juce::String(type) + " Fc `min(" + juce::String(band.frequency, 2)
            + ", sampleRate * 0.49)` Hz Gain " + juce::String(band.gain, 5) + " dB Q " + juce::String(band.q, 8) + "\n";
    }
    text += "Channel: ALL\n";
    return text;
}
juce::Result exportApo(const Profile& p, const Settings& s, const juce::File& file, double rate) {
    auto text = apoConfiguration(p, s, false, rate);
    if (s.limiterEnabled) {
        const auto source = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("SoundeeDSP.dll");
        if (!source.existsAsFile()) return juce::Result::fail("SoundeeDSP.dll is required to export the limiter.");
        const auto copy = file.getSiblingFile("SoundeeDSP-" + juce::SHA256(source).toHexString().substring(0, 16) + ".dll");
        if ((!copy.existsAsFile() && !source.copyFileTo(copy)) || juce::SHA256(source) != juce::SHA256(copy)) return juce::Result::fail("Unable to export the limiter module.");
        text += "Channel: L R\n" + NativeBridge::filterLine(copy, {}, 0, s, false) + "Channel: ALL\n";
    }
    return file.replaceWithText(text) ? juce::Result::ok() : juce::Result::fail("Unable to write the EQ configuration.");
}
juce::Result exportImpulse(const DesignedFilters& filters, const juce::File& file) {
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (!stream) return juce::Result::fail("Unable to create the impulse-response file.");
    // createOutputStream appends; truncate via the concrete file stream.
    auto* output = dynamic_cast<juce::FileOutputStream*>(stream.get());
    output->setPosition(0); output->truncate();
    juce::WavAudioFormat format;
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions()
        .withSampleRate(filters.sampleRate).withNumChannels(2).withBitsPerSample(32)
        .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    auto scaled = filters.impulse;
    scaled.applyGain(juce::Decibels::decibelsToGain(static_cast<float>(filters.preampDb)));
    if (!writer || !writer->writeFromAudioSampleBuffer(scaled, 0, scaled.getNumSamples()))
        return juce::Result::fail("Unable to write the stereo impulse response.");
    return juce::Result::ok();
}
}

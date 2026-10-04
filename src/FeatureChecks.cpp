#include "ListeningProfiles.h"
#include "Diagnostics.h"
#include "AudioMeter.h"
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>

namespace soundee {
void runFeatureChecks(const Profile& calibration, const juce::File& folder, juce::StringArray& passed) {
    auto ensure = [](bool condition, const char* message) { if (!condition) throw std::runtime_error(message); };
    auto flat = Profile::flat(); Settings custom;
    for (int c = 0; c < 4; ++c) for (const auto& point : flat.curves[static_cast<size_t>(c)].points)
        ensure(point.db == 0, "An uncalibrated profile is not flat");
    ensure(safeHeadroomDb(flat, custom) == 0, "Flat profile has unnecessary attenuation");
    ensure(!apoConfiguration(flat, custom).contains("GraphicEQ"), "Flat profile exports unnecessary room correction");
    passed.add("Uncalibrated profile is flat with unity gain");

    for (double rate : {44100., 48000., 96000.}) for (double gain : {-12., 6., 18.}) {
        EqBand band {BandType::bell, 1000, gain, 1.4, true};
        auto bell = bandCoefficients(band, rate);
        ensure(std::abs(bandDb(bell, 1000, rate) - gain) < 1e-8, "Bell centre gain is incorrect");
        ensure(std::abs(bandDb(bell, 0, rate)) < 1e-8 && std::abs(bandDb(bell, rate / 2, rate)) < 1e-8, "Bell endpoints are not flat");
        band.type = BandType::lowShelf; auto low = bandCoefficients(band, rate);
        ensure(std::abs(bandDb(low, 0, rate) - gain) < 1e-8 && std::abs(bandDb(low, rate / 2, rate)) < 1e-8, "Low shelf endpoints are incorrect");
        ensure(std::abs(bandDb(low, 1000, rate) - gain / 2) < 1e-8, "Low shelf centre convention is incorrect");
        band.type = BandType::highShelf; auto high = bandCoefficients(band, rate);
        ensure(std::abs(bandDb(high, 0, rate)) < 1e-8 && std::abs(bandDb(high, rate / 2, rate) - gain) < 1e-8, "High shelf endpoints are incorrect");
        ensure(std::abs(bandDb(high, 1000, rate) - gain / 2) < 1e-8, "High shelf centre convention is incorrect");
    }
    passed.add("Bell and shelf gains at 44.1, 48 and 96 kHz");
    custom.bands = {{BandType::bell, 1000, 4, 1.5, true}, {BandType::lowShelf, 95, -3, 0.71, true},
        {BandType::highShelf, 7200, 2, 0.71, true}, {BandType::bell, 300, 18, 12, false}};
    double maxError = 0;
    for (double rate : {44100., 48000., 96000.}) {
        const auto eq = eqCoefficients(custom.bands, rate, true); auto filters = designFilters(calibration, custom, rate);
        juce::dsp::FFT fft(14); std::vector<std::complex<float>> data(16384), spectrum(16384);
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < 16384; ++i) data[static_cast<size_t>(i)] = {filters.impulse.getSample(c, i), 0};
            fft.perform(data.data(), spectrum.data(), false);
            for (int bin = 1; bin < 8192; ++bin) {
                const double f = bin * rate / 16384; if (f < 40 || f > 20000) continue;
                const double target = correctionDb(calibration, c, f, custom) + customEqDb(eq, f, rate);
                maxError = std::max(maxError, std::abs(20 * std::log10(std::abs(spectrum[static_cast<size_t>(bin)])) - target));
                ensure(target + filters.preampDb <= -0.99, "Combined correction does not reserve safe headroom");
            }
        }
    }
    ensure(maxError < 0.1, "Correction plus custom EQ FIR does not match within 0.1 dB");
    auto noCorrection = custom; noCorrection.amount = 0; noCorrection.maximumBoost = 0;
    ensure(std::abs(customEqDb(eqCoefficients(noCorrection.bands, 48000, true), 1000, 48000)) > 3.9,
        "Correction limits attenuated the additional custom EQ");
    ensure(eqCoefficients(custom.bands, 48000, false).empty(), "Disabled custom EQ remains active");
    passed.add("Combined stereo FIR and additional EQ headroom");
    auto text = apoConfiguration(calibration, custom);
    ensure(text.contains("Filter: ON PK Fc `min(1000.00, sampleRate * 0.49)`") && text.contains("Filter: ON LSC")
        && text.contains("Filter: ON HSC") && !text.contains("min(300.00"), "APO custom filter types or disabled bands are incorrect");
    ensure(!apoConfiguration(calibration, custom, true).contains("Filter:"), "Bypass leaves custom EQ active");
    folder.getChildFile("custom-eq.txt").replaceWithText(text);
    passed.add("Stereo native APO Bell and centre-frequency shelf export");
    for (double offset : {-24., -3., 4., 24.}) {
        auto adjusted = custom; adjusted.headroomOffset = offset;
        for (double rate : {44100., 48000., 96000.}) {
            const auto original = designFilters(calibration, custom, rate); const auto changed = designFilters(calibration, adjusted, rate);
            ensure(std::abs(changed.preampDb - original.preampDb - offset) < 1e-9, "FIR export did not apply the headroom offset once");
            for (int c = 0; c < 2; ++c) for (int i = 0; i < original.impulse.getNumSamples(); ++i)
                ensure(original.impulse.getSample(c, i) == changed.impulse.getSample(c, i), "Headroom offset altered the EQ response");
        }
        ensure(std::abs(appliedHeadroomDb(calibration, adjusted) - safeHeadroomDb(calibration, custom) - offset) < 1e-9,
            "Windows headroom offset is incorrect");
        const auto wanted = "Preamp: " + juce::String(appliedHeadroomDb(calibration, adjusted), 5) + " dB";
        ensure(apoConfiguration(calibration, adjusted).contains(wanted) && apoConfiguration(calibration, adjusted, true).contains(wanted),
            "Headroom offset was lost in Windows export or bypass");
        folder.getChildFile("headroom-offset.txt").replaceWithText(apoConfiguration(calibration, adjusted));
    }
    Settings flatOffset; flatOffset.headroomOffset = -3;
    ensure(appliedHeadroomDb(flat, flatOffset) == -3, "Flat profile did not apply manual headroom offset");
    passed.add("Signed headroom offset preserves EQ and reaches Windows/FIR exports and bypass");

    Settings resonance; resonance.bands = {{BandType::lowShelf, 200, -6, 12, true}, {BandType::highShelf, 10000, 6, 12, true}};
    for (double rate : {44100., 48000., 96000.}) {
        const double bound = -safeHeadroomDb(flat, resonance, rate); const auto eq = eqCoefficients(resonance.bands, rate, true);
        for (int i = 0; i <= 12000; ++i) {
            const double f = rate / 2 * i / 12000;
            ensure(customEqDb(eq, f, rate) - bound < -0.95, "Shelf resonance escapes custom EQ headroom");
        }
    }
    passed.add("High-Q shelf resonance included in headroom");

    ListeningProfiles library; ListeningPreset speakers, headphones;
    speakers.name = "Yamaha HS8"; speakers.outputGuid = "{11111111-2222-3333-4444-555555555555}";
    speakers.calibrationPath = calibration.source.getFullPathName(); speakers.settings = custom; speakers.settings.headroomOffset = 4;
    headphones.name = "Flat response"; headphones.outputGuid = "{66666666-7777-8888-9999-AAAAAAAAAAAA}";
    library.presets = {speakers, headphones}; library.select(0);
    auto saved = library.serialize(); ListeningProfiles restored;
    ensure(restored.restore(saved).wasOk() && restored.selected()->settings.bands.size() == 4 && restored.selected()->settings.headroomOffset == 4,
        "Listening-profile settings failed to round trip");
    ensure(restored.presets[1].calibrationPath.isEmpty() && restored.presets[1].settings.bands.empty(), "Headphone inherited room calibration or speaker EQ");
    auto legacy = juce::JSON::parse(saved); legacy["presets"][0]["settings"].getDynamicObject()->removeProperty("headroom_offset");
    ListeningProfiles legacyRestore;
    ensure(legacyRestore.restore(juce::JSON::toString(legacy)).wasOk() && legacyRestore.selected()->settings.headroomOffset == 0,
        "Older listening profiles did not default to zero offset");
    restored.select(restored.forOutput(headphones.outputGuid.toLowerCase()));
    ensure(restored.selected()->id == headphones.id, "Endpoint lookup did not switch to headphones");
    const auto previous = restored.serialize();
    ensure(restored.restore("{}").failed() && restored.serialize() == previous, "Damaged library erased existing profiles");
    auto badOffset = juce::JSON::parse(saved); badOffset["presets"][0]["settings"].getDynamicObject()->setProperty("headroom_offset", 25);
    ensure(restored.restore(juce::JSON::toString(badOffset)).failed() && restored.serialize() == previous, "Invalid offset erased existing profiles");
    auto damaged = juce::JSON::parse(saved); damaged["presets"][0]["settings"]["bands"][0].getDynamicObject()->setProperty("q", -1);
    ensure(restored.restore(juce::JSON::toString(damaged)).failed() && restored.serialize() == previous, "Invalid EQ band erased existing profiles");
    folder.getChildFile("profile-library-fixture.json").replaceWithText(saved);
    passed.add("Per-device profile persistence, flat headphones and damaged-library preservation");

    StereoLevels levels; juce::AudioBuffer<float> samples(2, 2);
    samples.setSample(0, 0, 0.5f); samples.setSample(0, 1, -0.5f);
    samples.setSample(1, 0, 1.f); samples.setSample(1, 1, 0.f); levels.publish(samples, 2);
    auto reading = levels.consume();
    ensure(std::abs(juce::Decibels::gainToDecibels(reading.peak[0]) + 6.0206f) < 0.0001f && reading.rms[0] == 0.5f,
        "Stereo meter peak or RMS conversion is incorrect");
    ensure(!reading.clipped[0] && reading.clipped[1], "Stereo clipping indicator is incorrect");
    levels.publish({0.1f, 0.1f}, {0.1f, 0.1f}); ensure(levels.consume().clipped[1], "Clip indicator did not latch");
    levels.resetClips(); ensure(!levels.consume().clipped[1], "Clip reset failed");
    levels.publish({std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}, {0, 0});
    auto sanitized = levels.consume(); ensure(std::isfinite(sanitized.peak[0]) && std::isfinite(sanitized.peak[1]), "Nonfinite sample poisoned meter");
    passed.add("Stereo dBFS metering and independent latched clipping indicators");
    folder.getChildFile("feature-result.json").replaceWithText(juce::JSON::toString(fields({{"passed", true}, {"maximum_combined_fir_error_db", maxError}})));
}
}

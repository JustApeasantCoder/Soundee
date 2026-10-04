#include "ListeningProfiles.h"
#include "NativeDsp.h"
#include "NativeTelemetry.h"
#include "NativePluginAbi.h"
#include "NativeBridge.h"
#include "EqHistory.h"
#include "ProfileArchive.h"
#include "ResponseComparison.h"
#include "DesktopIntegration.h"
#include "Diagnostics.h"
#include <stdexcept>
#include <cmath>

namespace soundee {
void runAdvancedChecks(const Profile& calibration, const juce::File& folder, juce::StringArray& passed) {
    auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    Settings cancelled; cancelled.maximumBoost = 12;
    cancelled.bands = {{BandType::bell, 1000, 12, 2, true}, {BandType::bell, 1000, -12, 2, true}};
    require(std::abs(safeHeadroomDb(Profile::flat(), cancelled)) < 1e-8, "Cancelled EQ bands reserve unnecessary headroom");
    Settings measured; measured.maximumBoost = 12; measured.bands = {{BandType::bell, 90, 6, 1, true}, {BandType::bell, 4000, 6, 1, true}};
    const auto headroom = safeHeadroomDb(Profile::flat(), measured);
    require(headroom > -8 && headroom < -7, "Separated bands incorrectly sum their individual maxima");
    measured.amount = 0; measured.bands.clear();
    require(safeHeadroomDb(calibration, measured) == 0, "Zero correction still reserves the configured boost limit");
    passed.add("Actual combined headroom handles cancelling/separated bands and zero correction");

    for (double rate : {44100., 48000., 96000.}) {
        TruePeakDetector detector; double peak = 0, samples = 0;
        for (int i = 0; i < static_cast<int>(rate * .1); ++i) {
            const double value = 1.2 * std::sin(juce::MathConstants<double>::halfPi * i + juce::MathConstants<double>::pi / 4);
            const double actual = detector.push(value);
            if (i > 128) { peak = std::max(peak, actual); samples = std::max(samples, std::abs(value)); }
        }
        require(samples < 1 && std::abs(peak - 1.2) < .002, "True peak does not detect a known inter-sample over");
        LookaheadLimiter limiter; limiter.prepare(rate, -1); std::array<TruePeakDetector, 2> outputMeter;
        double maximum = 0, ratioError = 0;
        for (int i = 0; i < static_cast<int>(rate * .15); ++i) {
            const float input = static_cast<float>(2 * std::sin(juce::MathConstants<double>::halfPi * i + juce::MathConstants<double>::pi / 4));
            const auto output = limiter.process(input, input * .25f);
            maximum = std::max(maximum, outputMeter[0].push(output[0])); outputMeter[1].push(output[1]);
            ratioError = std::max(ratioError, std::abs(static_cast<double>(output[1] - output[0] * .25f)));
        }
        require(maximum <= std::pow(10., -1. / 20) + .002 && ratioError < 1e-7, "Limiter ceiling or stereo link is incorrect");
    }
    passed.add("Four-times reconstruction detects inter-sample overs; limiter preserves stereo balance at three sample rates");

    EqHistory history; Settings state; history.commit(state); state.headroomOffset = 4; history.commit(state);
    state.limiterEnabled = true; history.commit(state);
    require(history.undo() && !history.redo()->bands.size(), "Edit history unavailable");
    const auto* undone = history.undo(); require(undone && !undone->limiterEnabled && undone->headroomOffset == 4, "Undo did not restore full EQ controls");
    state.headroomOffset = -2; history.commit(state); require(!history.canRedo(), "Editing after undo did not clear redo branch");
    passed.add("Undo/redo restores limiter and headroom controls and branches correctly");

    ListeningProfiles library; ListeningPreset preset; preset.name = "Calibration backup fixture";
    const auto fixtureSource = folder.getChildFile("temporary-original.swproj");
    require(calibration.source.copyFileTo(fixtureSource), "Cannot create temporary calibration fixture");
    juce::File retained; require(ProfileArchive::retainCalibration(fixtureSource, folder.getChildFile("retained-test"), retained).wasOk(), "Calibration retention failed");
    require(fixtureSource.deleteFile() && retained.existsAsFile(), "Retained calibration depends on original file");
    preset.calibrationPath = retained.getFullPathName(); preset.settings.headroomOffset = 4; preset.settings.limiterEnabled = true;
    preset.snapshotA = preset.settings; preset.snapshotB = Settings(); library.presets = {preset}; library.select(0);
    const auto bundle = folder.getChildFile("fixture.soundee");
    require(ProfileArchive::write(library, bundle).wasOk(), "Portable profile backup failed");
    ListeningProfiles imported;
    require(ProfileArchive::read(bundle, folder.getChildFile("imported-test"), imported).wasOk(), "Portable profile restore failed");
    require(imported.selected()->snapshotA && imported.selected()->snapshotB && imported.selected()->settings.limiterEnabled
        && imported.selected()->snapshotA->headroomOffset == 4, "Backup lost limiter or A/B snapshots");
    require(juce::SHA256(juce::File(imported.selected()->calibrationPath)) == juce::SHA256(retained), "Backup altered calibration data");
    const auto previous = imported.serialize(); folder.getChildFile("broken.soundee").replaceWithText("bad archive");
    require(ProfileArchive::read(folder.getChildFile("broken.soundee"), folder.getChildFile("imported-test"), imported).failed()
        && imported.serialize() == previous, "Failed backup import altered profile library");
    passed.add("Portable backups include calibration bytes and snapshots, survive missing originals and preserve state on failure");

    TelemetryPage abandoned; abandoned.magic = telemetryMagic;
    abandoned.slots[0].owner = 42; abandoned.slots[0].sequence = 1; abandoned.slots[0].updated = GetTickCount64() - 6001;
    const auto abandonedFile = folder.getChildFile("abandoned-telemetry.bin");
    require(abandonedFile.replaceWithData(&abandoned, sizeof(abandoned)), "Unable to write abandoned telemetry fixture");
    NativeTelemetry recoveredWriter, recoveredReader;
    require(recoveredWriter.open(abandonedFile.getFullPathName().toWideCharPointer(), true)
        && recoveredReader.open(abandonedFile.getFullPathName().toWideCharPointer(), false), "Unable to reopen abandoned telemetry");
    recoveredWriter.publish(123, 111, 48000, {.5f, .25f}, {.1f, .05f}, {0, 0}, 0, 0);
    require(recoveredReader.read(123).connected && recoveredReader.read(123).frames == 111, "Telemetry did not recover a writer terminated during an update");
    passed.add("Native telemetry recovers a stale slot interrupted during publication");

    const auto dll = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("SoundeeDSP.dll");
    HMODULE module = LoadLibraryW(dll.getFullPathName().toWideCharPointer());
    require(module != nullptr, "Native DSP library cannot load");
    using Factory = NativeEffect* (__cdecl *)(NativeHost);
    const auto factory = reinterpret_cast<Factory>(GetProcAddress(module, "VSTPluginMain"));
    NativeEffect* effect = factory ? factory(nullptr) : nullptr;
    require(effect && effect->magic == 0x56737450, "Native DSP binary interface is invalid");
    struct Cleanup { HMODULE module; NativeEffect* effect; ~Cleanup() { if (effect) effect->dispatcher(effect, 1, 0, 0, nullptr, 0); FreeLibrary(module); } } cleanup {module, effect};
    const auto telemetryFile = folder.getChildFile("native-test-" + juce::Uuid().toString() + ".bin");
    const auto configuredLine = NativeBridge::filterLine(dll, telemetryFile, 0x1234, {}, false);
    juce::MemoryOutputStream decodedChunk;
    require(juce::Base64::convertFromBase64(decodedChunk, configuredLine.fromFirstOccurrenceOf("ChunkData ", false, false).trim().unquoted()),
        "Generated native DSP configuration is invalid");
    const auto chunk = decodedChunk.toUTF8();
    require(!telemetryFile.existsAsFile() && chunk.contains(telemetryFile.getFileName()), "First activation lost the native telemetry path");
    require(effect->dispatcher(effect, 24, 0, chunk.getNumBytesAsUTF8(), const_cast<char*>(chunk.toRawUTF8()), 0) == 1, "DSP chunk configuration failed");
    effect->dispatcher(effect, 10, 0, 0, nullptr, 48000);
    juce::AudioBuffer<float> audio(2, 480), output(2, 480); juce::Random random(42);
    for (int c = 0; c < 2; ++c) for (int i = 0; i < 480; ++i) audio.setSample(c, i, (random.nextFloat() - .5f) * .1f);
    float* inputChannels[] {audio.getWritePointer(0), audio.getWritePointer(1)};
    float* outputChannels[] {output.getWritePointer(0), output.getWritePointer(1)};
    effect->processReplacing(effect, inputChannels, outputChannels, 480);
    for (int c = 0; c < 2; ++c) for (int i = 0; i < 480; ++i) require(audio.getSample(c, i) == output.getSample(c, i), "Disabled limiter changes audio");
    require(effect->delay == 0, "Disabled limiter adds latency");
    NativeTelemetry reader; require(reader.open(telemetryFile.getFullPathName().toWideCharPointer(), false), "Native telemetry is unreadable");
    const auto live = reader.read(0x1234); require(live.connected && live.frames == 480 && live.sampleRate == 48000, "Native processing telemetry did not arrive");
    require(!reader.read(0x5678).connected, "Telemetry from an older configuration was accepted");
    const juce::String limitedChunk = "SOUNDEE1\n1\n-1\n" + telemetryFile.getFullPathName() + "\n5678\n";
    effect->dispatcher(effect, 24, 0, limitedChunk.getNumBytesAsUTF8(), const_cast<char*>(limitedChunk.toRawUTF8()), 0);
    require(effect->delay > 0, "Enabled native limiter does not report its latency");
    double nativeMaximum = 0; TruePeakDetector nativeOutput;
    for (int block = 0; block < 40; ++block) {
        for (int i = 0; i < 480; ++i) { const auto sample = static_cast<float>(2 * std::sin(juce::MathConstants<double>::halfPi * (i + block * 480) + juce::MathConstants<double>::pi / 4));
            audio.setSample(0, i, sample); audio.setSample(1, i, sample * .25f); }
        effect->processReplacing(effect, inputChannels, outputChannels, 480);
        for (int i = 0; i < 480; ++i) nativeMaximum = std::max(nativeMaximum, nativeOutput.push(output.getSample(0, i)));
    }
    const auto limitedReading = reader.read(0x5678);
    require(nativeMaximum < .894 && limitedReading.connected && limitedReading.reductionDb > 5 && !limitedReading.clipped[0],
        "Native limiter output or gain-reduction telemetry is incorrect");
    passed.add("Native host ABI, transparent bypass, enabled limiter, latency and configuration-matched telemetry");

    const auto compared = ResponseComparison::runChecks(folder);
    require(static_cast<bool>(compared["passed"]), "Recording comparison failed"); passed.add("Recording comparison and offline test WAV generation");
    const auto startup = StartupRegistration::runChecks(folder);
    require(static_cast<bool>(startup["passed"]), "Startup registration round trip failed"); passed.add("Windows startup quoting and reversible registration using a private test key");
    folder.getChildFile("advanced-result.json").replaceWithText(juce::JSON::toString(fields({{"passed", true}, {"checks", passed.size()}, {"native_processing_confirmed", live.connected}})));
}
}

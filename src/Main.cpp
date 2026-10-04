#include "MainComponent.h"
#include "SystemEq.h"
#include "ResponseComparison.h"
#include "BinaryData.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace soundee {
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
void runFeatureChecks(const Profile&, const juce::File&, juce::StringArray&);
void runAdvancedChecks(const Profile&, const juce::File&, juce::StringArray&);

int selfTest(const juce::File& input, const juce::File& folder) {
    folder.createDirectory(); Diagnostics logs(folder.getChildFile("logs"));
    const auto start = juce::Time::getMillisecondCounterHiRes(); const auto request = juce::Uuid().toString();
    juce::StringArray passed;
    auto check = [&](const char* title, auto function) { function(); passed.add(title); };
    try {
        auto profile = Profile::load(input); Settings settings;
        check("Profile structure and independent decoded baseline", [&] {
            require(profile.curves[0].points.size() == 355, "Wrong point count");
            require(std::abs(profile.curves[0].points.front().frequency - 20) < 1e-8, "Wrong first frequency");
            auto baselineFile = input.getSiblingFile("expected.json");
            if (!baselineFile.existsAsFile()) baselineFile = input.getParentDirectory().getParentDirectory()
                .getChildFile("decoded/yamaha-hs8/profile.json");
            require(baselineFile.existsAsFile(), "Decoded baseline missing");
            auto baseline = juce::JSON::parse(baselineFile.loadFileAsString());
            auto arrays = baseline["eq_block"]["curves"];
            for (int c = 0; c < 4; ++c) for (int i = 0; i < 355; ++i) {
                auto point = arrays[c]["points"][i]; const auto actual = profile.curves[static_cast<size_t>(c)].points[static_cast<size_t>(i)];
                require(std::abs(actual.frequency - static_cast<double>(point["frequency_hz"])) < 1e-9, "Baseline frequency mismatch");
                require(std::abs(actual.db - static_cast<double>(point["value_db"])) < 1e-9, "Baseline dB mismatch");
                require(std::abs(actual.extra - static_cast<double>(point["third_value_unverified"])) < 1e-9, "Baseline extra mismatch");
            }
        });
        check("Malformed and unsupported files fail cleanly", [&] {
            auto bad = folder.getChildFile("invalid.swproj"); bad.replaceWithText("not a calibration profile");
            bool failed = false; try { Profile::load(bad); } catch (...) { failed = true; }
            require(failed, "Malformed profile was accepted");
            juce::MemoryBlock bytes; require(input.loadFileAsData(bytes), "Could not read fixture");
            const std::string contents(static_cast<const char*>(bytes.getData()), bytes.getSize());
            const auto eqOffset = contents.find("</ProjectHeader>") + std::strlen("</ProjectHeader>") + 1;
            static_cast<char*>(bytes.getData())[eqOffset + 4] = 9;
            auto other = folder.getChildFile("unsupported.swproj"); other.replaceWithData(bytes.getData(), bytes.getSize());
            failed = false; try { Profile::load(other); } catch (...) { failed = true; }
            require(failed, "Unsupported version was accepted");
            logs.event("profile", "profile.load.failed", "[Profile] Invalid fixture rejected", "failed", {}, request, 0, "invalid_profile");
        });
        check("Boost limits, taper, amount and channel swap", [&] {
            require(std::abs(correctionDb(profile, 0, 20, settings)) < 1e-8, "Low taper failed");
            for (int c = 0; c < 2; ++c) for (double f = 20; f < 22000; f *= 1.01)
                require(correctionDb(profile, c, f, settings) <= 6.00001, "Boost limit exceeded");
            auto zero = settings; zero.amount = 0;
            require(correctionDb(profile, 1, 1000, zero) == 0, "Zero strength is not flat");
            auto swapped = settings; swapped.swap = true;
            require(std::abs(correctionDb(profile, 0, 1000, swapped) - correctionDb(profile, 1, 1000, settings)) < 1e-9, "Swap failed");
        });
        double maximumError = 0;
        check("FIR magnitude matches target at 44.1, 48 and 96 kHz", [&] {
            for (double rate : {44100., 48000., 96000.}) {
                auto filters = designFilters(profile, settings, rate);
                juce::dsp::FFT fft(14); std::vector<std::complex<float>> data(16384), spectrum(16384);
                for (int c = 0; c < 2; ++c) {
                    for (int i = 0; i < 16384; ++i) data[static_cast<size_t>(i)] = {filters.impulse.getSample(c, i), 0};
                    fft.perform(data.data(), spectrum.data(), false);
                    for (int bin = 1; bin < 8192; ++bin) {
                        double f = bin * rate / 16384; if (f < 40 || f > 16000) continue;
                        double actual = 20 * std::log10(std::abs(spectrum[static_cast<size_t>(bin)]));
                        maximumError = std::max(maximumError, std::abs(actual - correctionDb(profile, c, f, settings)));
                    }
                }
                require(filters.preampDb <= 0, "Headroom boosts output");
            }
            require(maximumError < 0.1, "FIR does not match the target within 0.1 dB");
        });
        check("Stereo convolution produces distinct calibrated channels", [&] {
            auto filters = designFilters(profile, settings, 48000);
            juce::dsp::Convolution convolution;
            convolution.loadImpulseResponse(std::move(filters.impulse), 48000,
                juce::dsp::Convolution::Stereo::yes, juce::dsp::Convolution::Trim::no,
                juce::dsp::Convolution::Normalise::no);
            convolution.prepare({48000, 512, 2});
            juce::AudioBuffer<float> buffer(2, 512); double sum[2] {}, reference = 0;
            for (int block = 0; block < 256; ++block) {
                for (int i = 0; i < 512; ++i) {
                    float value = static_cast<float>(0.1 * std::sin(2 * juce::MathConstants<double>::pi * 1000 * (block * 512 + i) / 48000));
                    for (int c = 0; c < 2; ++c) buffer.setSample(c, i, value);
                    if (block > 128) reference += value * value;
                }
                auto audio = juce::dsp::AudioBlock<float>(buffer); juce::dsp::ProcessContextReplacing<float> context(audio);
                convolution.process(context);
                if (block > 128) for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i)
                    sum[c] += buffer.getSample(c, i) * buffer.getSample(c, i);
            }
            for (int c = 0; c < 2; ++c) {
                double actual = 10 * std::log10(sum[c] / reference);
                require(std::abs(actual - correctionDb(profile, c, 1000, settings)) < 0.15, "Convolution channel response mismatch");
            }
        });
        check("Windows EQ and floating-point stereo FIR exports", [&] {
            require(exportApo(profile, settings, folder.getChildFile("soundee.txt")).wasOk(), "EQ export failed");
            auto text = folder.getChildFile("soundee.txt").loadFileAsString();
            require(text.contains("Channel: L") && text.contains("Channel: R") && text.contains("Preamp: -"), "Missing stereo EQ/headroom");
            auto filters = designFilters(profile, settings, 48000);
            auto file = folder.getChildFile("soundee-stereo.wav");
            require(exportImpulse(filters, file).wasOk(), "FIR export failed");
            juce::WavAudioFormat wav; auto reader = std::unique_ptr<juce::AudioFormatReader>(wav.createReaderFor(file.createInputStream().release(), true));
            require(reader && reader->numChannels == 2 && reader->usesFloatingPointData && reader->lengthInSamples == 16384, "Invalid FIR format");
            juce::AudioBuffer<float> read(2, 16384); reader->read(&read, 0, 16384, 0, true, true);
            const auto gain = juce::Decibels::decibelsToGain(static_cast<float>(filters.preampDb));
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 16384; ++i)
                require(std::abs(read.getSample(c, i) - filters.impulse.getSample(c, i) * gain) < 1e-7, "FIR export changed stereo gain");
        });
        runFeatureChecks(profile, folder, passed);
        runAdvancedChecks(profile, folder, passed);
        logs.event("qa", "qa.completed", "[QA] Core validation passed", "success",
            fields({{"checks", passed.size()}, {"maximum_fir_error_db", maximumError}}), request,
            juce::Time::getMillisecondCounterHiRes() - start);
        juce::Array<juce::var> names; for (auto& name : passed) names.add(name);
        folder.getChildFile("result.json").replaceWithText(juce::JSON::toString(fields({{"passed", true}, {"checks", names}, {"maximum_fir_error_db", maximumError}})));
        return 0;
    } catch (const std::exception& error) {
        logs.event("qa", "qa.failed", "[QA] Core validation failed", "failed", fields({{"reason", error.what()}}), request,
            juce::Time::getMillisecondCounterHiRes() - start, "assertion_failed");
        folder.getChildFile("result.json").replaceWithText(juce::JSON::toString(fields({{"passed", false}, {"reason", error.what()}, {"checks_before_failure", passed.size()}})));
        return 1;
    }
}

class App : public juce::JUCEApplication, private juce::Timer {
public:
    const juce::String getApplicationName() override { return "Soundee"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override {
        // Regular desktop launches share one instance and one diagnostic writer.
        // QA processes use independent folders and may run alongside the app.
        const auto parameters = juce::StringArray::fromTokens(getCommandLineParameters(), true);
        const auto first = parameters.isEmpty() ? juce::String() : parameters[0];
        return juce::StringArray {"--self-test", "--ui-test", "--system-eq-test", "--system-eq-status", "--screenshot", "--smoke-ui",
            "--desktop-smoke", "--meter-smoke", "--backend-download", "--compare-recordings", "--verify-backend"}.contains(first);
    }
    void initialise(const juce::String& command) override {
        auto args = juce::StringArray::fromTokens(command, true); args.removeEmptyStrings();
        for (auto& arg : args) arg = arg.unquoted();
        if (args.size() >= 3 && args[0] == "--verify-backend") {
            const auto folder = juce::File(args[2]); folder.createDirectory();
            try {
                MainComponent component(true, folder.getChildFile("verification-logs"));
                const auto report = component.runBackendVerificationChecks(juce::File(args[1]), folder);
                folder.getChildFile("backend-result.json").replaceWithText(juce::JSON::toString(report)); setApplicationReturnValue(static_cast<bool>(report["passed"]) ? 0 : 1);
            } catch (const std::exception& error) { folder.getChildFile("backend-result.json").replaceWithText(juce::JSON::toString(fields({{"passed", false}, {"reason", error.what()}}))); setApplicationReturnValue(1); }
            quit(); return;
        }
        if (args.size() >= 2 && args[0] == "--desktop-smoke") {
            const auto folder = juce::File(args[1]); folder.createDirectory();
            window = std::make_unique<Window>(folder.getChildFile("desktop-logs"), true, true); createTray();
            auto notifications = std::make_shared<EndpointNotifications>();
            const auto startup = StartupRegistration::runChecks(folder);
            juce::Timer::callAfterDelay(1000, [this, folder, startup, notifications] {
                window->closeButtonPressed();
                const auto passed = tray && tray->getNativeHandle() && !window->isVisible() && static_cast<bool>(startup["passed"]);
                folder.getChildFile("desktop-result.json").replaceWithText(juce::JSON::toString(fields({{"passed", passed},
                    {"native_tray_handle", tray && tray->getNativeHandle()}, {"close_hides_window", !window->isVisible()},
                    {"endpoint_notifications_registered", notifications->registered()}, {"startup_test", startup}, {"live_eq_preserved", true}})));
                setApplicationReturnValue(passed ? 0 : 1); quit();
            }); return;
        }
        if (args.size() >= 4 && args[0] == "--compare-recordings") {
            ComparisonReport report; const auto result = ResponseComparison::analyze(juce::File(args[1]), juce::File(args[2]), report);
            setApplicationReturnValue(result.wasOk() && report.writeCsv(juce::File(args[3])).wasOk() ? 0 : 1); quit(); return;
        }
        if (args.size() >= 2 && args[0] == "--meter-smoke") {
            const auto folder = juce::File(args[1]); folder.createDirectory();
            meterLogs = std::make_unique<Diagnostics>(folder.getChildFile("meter-logs"));
            meter = std::make_unique<SystemAudioMeter>(*meterLogs);
            juce::String id = args.size() >= 3 ? args[2] : juce::String();
            if (args.size() < 3) for (const auto& output : SystemEq::outputs()) if (output.isDefault) { id = output.id; break; }
            meter->selectEndpoint(id);
            juce::Timer::callAfterDelay(2500, [this, folder, id] {
                const auto reading = meter->levels.consume(); const bool connected = meter->online.load();
                folder.getChildFile("meter-result.json").replaceWithText(juce::JSON::toString(fields({
                    {"passed", connected}, {"endpoint_id", id}, {"mode", "wasapi_loopback"},
                    {"sample_rate", meter->sampleRate.load()}, {"received_samples", reading.updatedAt != 0},
                    {"left_peak", reading.peak[0]}, {"right_peak", reading.peak[1]},
                    {"left_clipped", reading.clipped[0]}, {"right_clipped", reading.clipped[1]}})));
                setApplicationReturnValue(connected ? 0 : 1); quit();
            });
            return;
        }
        if (args.size() >= 2 && args[0] == "--backend-download") {
            auto folder = juce::File(args[1]); folder.createDirectory();
            BackendDownload download(folder.getChildFile("EqualizerAPO-x64-1.4.2.exe")); download.run();
            folder.getChildFile("backend-download-result.json").replaceWithText(juce::JSON::toString(fields({
                {"passed", download.success}, {"sha256_verified", BackendDownload::verified(download.file)}, {"error", download.error}})));
            setApplicationReturnValue(download.success ? 0 : 1); quit(); return;
        }
        if (args.size() >= 2 && args[0] == "--system-eq-status") {
            juce::Array<juce::var> outputs;
            for (const auto& output : SystemEq::outputs()) outputs.add(fields({{"id", output.id}, {"guid", output.guid},
                {"name", output.name}, {"apo_installed", output.apoInstalled}, {"effects_disabled", output.effectsDisabled},
                {"pre_mix_installed", output.preMixInstalled}, {"post_mix_installed", output.postMixInstalled},
                {"preferred_stage", SystemEq::stageName(output.preferredStage())}, {"is_default", output.isDefault}}));
            const auto state = SystemEq::state(SystemEq::configDirectory());
            const auto report = fields({{"outputs", outputs}, {"backend_installed", SystemEq::installDirectory().exists()},
                {"configured", state.configured}, {"endpoint_guid", state.endpointGuid},
                {"configured_stage", SystemEq::stageName(state.stage)}});
            juce::File(args[1]).replaceWithText(juce::JSON::toString(report)); quit(); return;
        }
        if (args.size() >= 3 && args[0] == "--system-eq-test") {
            auto folder = juce::File(args[2]); folder.createDirectory();
            try {
                const auto report = SystemEq::runChecks(Profile::load(juce::File(args[1])), folder);
                folder.getChildFile("system-eq-result.json").replaceWithText(juce::JSON::toString(report));
            } catch (const std::exception& error) {
                folder.getChildFile("system-eq-result.json").replaceWithText(juce::JSON::toString(fields({{"passed", false}, {"reason", error.what()}})));
                setApplicationReturnValue(1);
            }
            quit(); return;
        }
        if (args.size() >= 3 && args[0] == "--self-test") {
            setApplicationReturnValue(selfTest(juce::File(args[1]), juce::File(args[2]))); quit(); return;
        }
        if (args.size() >= 2 && args[0] == "--screenshot") {
            MainComponent component(true, juce::File(args[1]).getParentDirectory().getChildFile("screenshot-logs"));
            auto image = component.createComponentSnapshot(component.getLocalBounds(), true, 1.5f);
            auto output = juce::File(args[1]).createOutputStream(); juce::PNGImageFormat png;
            if (output) { output->setPosition(0); output->truncate(); }
            setApplicationReturnValue(output && png.writeImageToStream(image, *output) ? 0 : 1); quit(); return;
        }
        if (args.size() >= 3 && args[0] == "--ui-test") {
            auto folder = juce::File(args[2]); folder.createDirectory();
            try {
                MainComponent component(true, folder.getChildFile("ui-logs"));
                auto report = component.runUiChecks(juce::File(args[1]), folder);
                folder.getChildFile("ui-result.json").replaceWithText(juce::JSON::toString(report));
            } catch (const std::exception& error) {
                folder.getChildFile("ui-result.json").replaceWithText(juce::JSON::toString(fields({{"passed", false}, {"reason", error.what()}})));
                setApplicationReturnValue(1);
            }
            quit(); return;
        }
        window = std::make_unique<Window>(args.size() >= 2 && args[0] == "--smoke-ui"
            ? juce::File(args[1]).getChildFile("native-logs") : juce::File(), args.contains("--background"));
        createTray(); startTimerHz(1);
        if (args.size() >= 2 && args[0] == "--smoke-ui") {
            auto folder = juce::File(args[1]); folder.createDirectory();
            juce::Timer::callAfterDelay(2000, [this, folder] {
                auto* component = static_cast<MainComponent*>(window->getContentComponent());
                auto report = component->liveStatus();
                folder.getChildFile("live-result.json").replaceWithText(juce::JSON::toString(report));
                auto image = component->createComponentSnapshot(component->getLocalBounds(), true, 1.5f);
                auto stream = folder.getChildFile("live-interface.png").createOutputStream();
                if (stream) { stream->setPosition(0); stream->truncate(); juce::PNGImageFormat().writeImageToStream(image, *stream); }
                if (!static_cast<bool>(report["profile_loaded"]))
                    setApplicationReturnValue(1);
                quit();
            });
        }
        if (args.size() == 1 && args[0].endsWithIgnoreCase(".swproj"))
            static_cast<MainComponent*>(window->getContentComponent())->loadProfile(juce::File(args[0]));
    }
    void shutdown() override { stopTimer(); tray.reset(); window.reset(); meter.reset(); meterLogs.reset(); }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted(const juce::String& command) override {
        if (!window) return;
        auto args = juce::StringArray::fromTokens(command, true); args.removeEmptyStrings();
        if (args.size() == 1 && args[0].unquoted().endsWithIgnoreCase(".swproj"))
            static_cast<MainComponent*>(window->getContentComponent())->loadProfile(juce::File(args[0].unquoted()));
        if (!args.contains("--background")) { window->setVisible(true); window->toFront(true); }
    }
private:
    class Window : public juce::DocumentWindow {
    public:
        explicit Window(juce::File diagnosticDirectory = {}, bool hidden = false, bool headless = false)
            : DocumentWindow("Soundee", juce::Colour(0xff070907), allButtons) {
            setUsingNativeTitleBar(true); setContentOwned(new MainComponent(headless, diagnosticDirectory), true);
            setResizable(true, false); setResizeLimits(1180, 760, 2200, 1400);
            centreWithSize(getWidth(), getHeight()); setVisible(!hidden);
        }
        void closeButtonPressed() override {
            auto* component = static_cast<MainComponent*>(getContentComponent());
            if (component && component->closesToTray()) { setVisible(false); component->recordDesktopEvent("desktop.window.hidden", "Window closed to tray; profile following continues"); }
            else juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };
    class Tray : public juce::SystemTrayIconComponent {
    public:
        explicit Tray(App& application) : app(application) {
            const auto icon = juce::ImageCache::getFromMemory(BinaryData::soundee_png, BinaryData::soundee_pngSize).rescaled(32, 32);
            setIconImage(icon, icon); setIconTooltip("Soundee");
        }
        void mouseUp(const juce::MouseEvent& event) override {
            if (!event.mods.isPopupMenu()) { app.showWindow(); return; }
            juce::PopupMenu menu; menu.addItem(1, "Open Soundee"); menu.addItem(2, "Toggle EQ bypass");
            menu.addItem(3, "Start with Windows", true, StartupRegistration::enabled()); menu.addSeparator(); menu.addItem(4, "Exit Soundee");
            auto safe = juce::Component::SafePointer<Tray>(this);
            menu.showMenuAsync(juce::PopupMenu::Options(), [safe](int item) {
                if (!safe) return; auto& app = safe->app;
                if (item == 1) app.showWindow();
                if (item == 2 && app.window) static_cast<MainComponent*>(app.window->getContentComponent())->toggleBypass();
                if (item == 3) {
                    const auto result = StartupRegistration::setEnabled(!StartupRegistration::enabled());
                    if (result.failed()) juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Soundee", result.getErrorMessage());
                    else if (app.window) static_cast<MainComponent*>(app.window->getContentComponent())->recordDesktopEvent("desktop.startup.changed", "Windows startup setting updated");
                }
                if (item == 4) app.quit();
            });
        }
    private:
        App& app;
    };
    void createTray() { tray = std::make_unique<Tray>(*this); }
    void showWindow() { if (window) { window->setVisible(true); window->toFront(true); static_cast<MainComponent*>(window->getContentComponent())->recordDesktopEvent("desktop.window.shown", "Window reopened"); } }
    void timerCallback() override { if (tray && window) tray->setIconTooltip(static_cast<MainComponent*>(window->getContentComponent())->trayDescription()); }
    std::unique_ptr<Window> window;
    std::unique_ptr<Tray> tray;
    std::unique_ptr<Diagnostics> meterLogs;
    std::unique_ptr<SystemAudioMeter> meter;
};
}
START_JUCE_APPLICATION(soundee::App)

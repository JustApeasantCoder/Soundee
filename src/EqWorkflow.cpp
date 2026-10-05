#include "MainComponent.h"
#include "ProfileArchive.h"
#include "ResponseComparison.h"
#include <cmath>
#include <complex>
#include <stdexcept>

namespace soundee {
void MainComponent::applySettings(const Settings& s) {
    {
        juce::ScopedValueSetter<bool> updating(updatingControls, true);
        amount.setValue(s.amount * 100, juce::dontSendNotification); boost.setValue(s.maximumBoost, juce::dontSendNotification);
        low.setValue(s.low, juce::dontSendNotification); high.setValue(s.high, juce::dontSendNotification);
        swapButton.setToggleState(s.swap, juce::dontSendNotification);
        headroomOffset.setValue(s.headroomOffset, juce::dontSendNotification);
        limiterButton.setToggleState(s.limiterEnabled, juce::dontSendNotification);
        limiterCeiling.setValue(s.limiterCeiling, juce::dontSendNotification); limiterCeiling.setEnabled(s.limiterEnabled);
        customBands = s.bands; customButton.setToggleState(s.customEnabled, juce::dontSendNotification);
        selectedBand = customBands.empty() ? -1 : juce::jlimit(0, static_cast<int>(customBands.size()) - 1, selectedBand);
    }
    refreshBandControls(); dirty = true; rebuildAt = 0; rebuild();
}
void MainComponent::undoEq(bool redo) {
    if (dirty) { gestureInProgress = false; rebuild(); }
    auto& history = histories[library.selectedId];
    const auto* saved = redo ? history.redo() : history.undo(); if (!saved) return;
    const auto snapshot = *saved;
    juce::ScopedValueSetter<bool> restoring(historyRestoring, true); applySettings(snapshot);
    if (auto* p = library.selected()) p->activeSnapshot = -1;
    saveSettings(); status = redo ? "EQ change redone" : "EQ change undone";
    logs.event("eq", redo ? "eq.redone" : "eq.undone", "[EQ] Edit history restored", "success", fields({{"profile_id", library.selectedId}}));
}
bool MainComponent::keyPressed(const juce::KeyPress& key) {
    if (!libraryWritable || !key.getModifiers().isCtrlDown()) return false;
    if (key.getKeyCode() == 'Z') { undoEq(key.getModifiers().isShiftDown()); return true; }
    if (key.getKeyCode() == 'Y') { undoEq(true); return true; }
    return false;
}
void MainComponent::storeSnapshot(int slot) {
    if (auto* p = library.selected()) {
        if (slot == 0) p->snapshotA = settings(); else p->snapshotB = settings();
        p->activeSnapshot = slot; saveSettings(); status = slot == 0 ? "Current EQ stored in A" : "Current EQ stored in B";
        logs.event("eq", "eq.snapshot.stored", "[EQ] A/B snapshot stored", "success", fields({{"profile_id", p->id}, {"slot", slot == 0 ? "A" : "B"}}));
    }
}
void MainComponent::selectSnapshot(int slot) {
    auto* p = library.selected(); if (!p) return;
    const auto snapshot = slot == 0 ? p->snapshotA : p->snapshotB;
    if (!snapshot) { storeSnapshot(slot); return; }
    applySettings(*snapshot); p = library.selected(); p->activeSnapshot = slot; saveSettings();
    status = slot == 0 ? "Snapshot A applied" : "Snapshot B applied";
    logs.event("eq", "eq.snapshot.applied", "[EQ] A/B snapshot applied", "success", fields({{"profile_id", p->id}, {"slot", slot == 0 ? "A" : "B"}}));
}
void MainComponent::showSnapshotMenu() {
    juce::PopupMenu menu; menu.addItem(1, "Store current EQ in A"); menu.addItem(2, "Store current EQ in B");
    auto safe = juce::Component::SafePointer<MainComponent>(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&snapshotButton), [safe](int item) { if (safe && item) safe->storeSnapshot(item - 1); });
}
void MainComponent::duplicateProfile() {
    if (!library.selected() || getNumRows() >= 128) return;
    const auto writable = recoverProfileLibrary(); if (writable.failed()) { showError(writable.getErrorMessage()); return; }
    saveSelectedPreset(); auto copy = *library.selected(); copy.id = juce::Uuid().toString(); copy.name = (copy.name + " copy").substring(0, 256);
    library.presets.push_back(std::move(copy)); selectPreset(getNumRows() - 1);
    logs.event("profile", "profile.duplicated", "[Profile] Listening profile duplicated", "success", fields({{"profile_id", library.selectedId}}));
}
void MainComponent::exportProfiles(bool all) {
    saveSelectedPreset(); ListeningProfiles backup = library;
    if (!all) { backup.presets = {*library.selected()}; backup.selectedId = backup.presets.front().id; }
    chooser = std::make_unique<juce::FileChooser>(all ? "Back up all profiles" : "Export profile", juce::File(), "*.soundee");
    auto safe = juce::Component::SafePointer<MainComponent>(this);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe, backup](const juce::FileChooser& dialog) {
            if (!safe || dialog.getResult() == juce::File()) return;
            const auto start = juce::Time::getMillisecondCounterHiRes(); const auto id = juce::Uuid().toString();
            const auto result = ProfileArchive::write(backup, dialog.getResult().withFileExtension(".soundee"));
            safe->logs.event("profile", result.wasOk() ? "profile.backup.completed" : "profile.backup.failed", "[Profile] Portable backup finished",
                result.wasOk() ? "success" : "failed", fields({{"profiles", static_cast<int>(backup.presets.size())}}), id,
                juce::Time::getMillisecondCounterHiRes() - start, result.wasOk() ? "" : "backup_write_failed");
            if (result.failed()) safe->showError(result.getErrorMessage()); else safe->status = "Backup saved with calibration files and A/B snapshots";
        });
}
juce::Result MainComponent::importBackup(const juce::File& file) {
    ListeningProfiles restored; const auto result = ProfileArchive::read(file, calibrationStore, restored);
    if (result.failed()) return result;
    if (library.presets.size() + restored.presets.size() > 128) return juce::Result::fail("Import would exceed 128 profiles. Existing profiles were preserved.");
    const auto writable = recoverProfileLibrary(); if (writable.failed()) return writable;
    saveSelectedPreset(); int selected = getNumRows();
    for (auto& p : restored.presets) {
        if (p.id == restored.selectedId) selected = getNumRows();
        p.id = juce::Uuid().toString(); library.presets.push_back(std::move(p));
    }
    selectPreset(selected); status = "Backup imported; existing profiles preserved";
    logs.event("profile", "profile.backup.imported", "[Profile] Portable backup imported", "success", fields({{"profiles", static_cast<int>(restored.presets.size())}}));
    return juce::Result::ok();
}
void MainComponent::chooseBackup() {
    chooser = std::make_unique<juce::FileChooser>("Import Soundee profile or backup", juce::File(), "*.soundee");
    auto safe = juce::Component::SafePointer<MainComponent>(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& dialog) { if (safe && dialog.getResult() != juce::File()) {
            const auto result = safe->importBackup(dialog.getResult()); if (result.failed()) safe->showError(result.getErrorMessage()); } });
}
void MainComponent::showComparison() {
    juce::DialogWindow::LaunchOptions dialog; dialog.dialogTitle = "SoundID / Soundee response comparison";
    dialog.dialogBackgroundColour = juce::Colour(0xff070907); dialog.content.setOwned(new ComparisonPanel);
    dialog.useNativeTitleBar = true; dialog.resizable = false; dialog.launchAsync();
}
void MainComponent::toggleBypass() {
    bypassButton.setToggleState(!bypassButton.getToggleState(), juce::dontSendNotification); bypassButton.onClick();
}
juce::String MainComponent::trayDescription() const {
    return "Soundee | " + (library.selected() ? library.selected()->name : juce::String("Flat response")) + " | "
        + (bypassButton.getToggleState() ? "bypassed" : nativeReading.connected ? "processing" : systemState.configured ? "configured" : "off");
}
void MainComponent::recordDesktopEvent(const juce::String& name, const juce::String& message) { logs.event("desktop", name, "[Desktop] " + message); }
void MainComponent::setCloseToTray(bool enabled) { closeToTray = enabled; saveSettings(); }
juce::String MainComponent::processingDescription() const {
    if (!systemState.configured) return "System EQ off";
    if (!systemOutput.apoInstalled || systemOutput.effectsDisabled || !systemOutput.supports(systemState.stage)) return "Output needs setup | configuration saved";
    const auto state = systemState.bypassed ? "Bypass processing" : "EQ processing";
    const auto confirmation = nativeReading.connected ? juce::String(state) + " confirmed" : windowsAudioActive ? "Audio detected | EQ unconfirmed" : "Configured | awaiting audio";
    const auto sampleRate = nativeReading.connected ? nativeReading.sampleRate : rate.load();
    return confirmation + " | " + systemOutput.name.substring(0, 28) + " | " + juce::String(sampleRate / 1000, 1) + " kHz | " + SystemEq::stageName(systemState.stage);
}
void MainComponent::refreshNativeMeter() {
    if (verification && !verification->isThreadRunning()) {
        verificationSummary = verification->description; status = verificationSummary;
        logs.event("system_eq", verification->result.wasOk() ? "system_eq.verification.completed" : "system_eq.verification.failed", "[System EQ] Offline backend verification finished",
            verification->result.wasOk() ? "success" : "failed", fields({{"maximum_error_db", verification->maximumError}, {"maximum_ideal_curve_deviation_db", verification->maximumTargetError}, {"audible_playback", false}}),
            verification->requestId, juce::Time::getMillisecondCounterHiRes() - verification->startedAt, verification->result.wasOk() ? "" : "backend_verification_failed");
        verification.reset();
    }
    if (noDevice) return;
    const auto path = systemState.configured ? NativeBridge::telemetryFile(systemDirectory, systemState.endpointGuid) : juce::File();
    if (path != telemetryPath) { nativeMeter.close(); telemetryPath = path; nativeClipResetAt = 0; }
    if (path != juce::File() && !nativeMeter.isOpen()) nativeMeter.open(SystemEq::nativePath(path).toWideCharPointer(), false);
    const bool wasConnected = nativeReading.connected; nativeReading = nativeMeter.read(systemState.processingToken);
    if (nativeReading.connected != wasConnected) logs.event("system_eq", nativeReading.connected ? "system_eq.processing.confirmed" : "system_eq.processing.idle",
        nativeReading.connected ? "[System EQ] Audio processing confirmed" : "[System EQ] Awaiting audio processing", "success",
        fields({{"endpoint_guid", systemState.endpointGuid}, {"processing_token", juce::String::toHexString(static_cast<juce::int64>(systemState.processingToken))}}));
    windowsAudioActive = false;
    if (nativeReading.connected && !meterWindowsMix) {
        StereoReading reading; reading.peak = nativeReading.peak; reading.rms = nativeReading.rms;
        reading.updatedAt = juce::Time::getMillisecondCounter();
        for (size_t c = 0; c < 2; ++c) reading.clipped[c] = nativeReading.lastClipAt[c] > nativeClipResetAt;
        meterPanel.reductionDb = nativeReading.reductionDb;
        meterPanel.update(reading, systemState.stage == ApoStage::preMix ? "EQ stream output" : "EQ output", true);
    } else {
        meterPanel.reductionDb = 0;
        if (windowsMeter && meterEndpointId.isNotEmpty()) {
            const auto reading = windowsMeter->levels.consume();
            windowsAudioActive = reading.updatedAt != 0 && juce::Time::getMillisecondCounter() - reading.updatedAt < 250 && std::max(reading.peak[0], reading.peak[1]) > .0001;
            meterPanel.update(reading, "Windows mix", windowsMeter->online.load());
        }
        else meterPanel.update({}, "Windows mix", false);
    }
}
MainComponent::VerificationJob::VerificationJob(Profile p, Settings s, OutputEndpoint o, juce::File f, double r)
    : Thread("Soundee offline backend verification"), profile(std::move(p)), controls(std::move(s)), output(std::move(o)), folder(f), sampleRate(r) { startThread(); }
MainComponent::VerificationJob::~VerificationJob() { signalThreadShouldExit(); if (child.isRunning()) child.kill(); stopThread(5000); }
void MainComponent::VerificationJob::run() {
    auto fail = [&](juce::String message) { result = juce::Result::fail(message); description = "Backend verification: " + message; };
    try {
        const auto helper = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("SoundeeBackendProbe.exe");
        if (!helper.existsAsFile()) { fail("SoundeeBackendProbe.exe is missing."); return; }
        if (folder.createDirectory().failed()) { fail("Cannot create verification folder."); return; }
        const auto engineFolder = folder.getChildFile("engine");
        if (engineFolder.createDirectory().failed()) { fail("Cannot create isolated backend folder."); return; }
        const auto curve = apoConfiguration(profile, controls, false, sampleRate);
        const auto plugin = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("SoundeeDSP.dll");
        const auto config = "Stage: " + SystemEq::stageName(output.preferredStage()) + "\nDevice: ALL\n" + curve
            + "Channel: L R\n" + NativeBridge::filterLine(plugin, folder.getChildFile("offline-telemetry.bin"), 0, controls, false) + "Channel: ALL\n";
        if (!engineFolder.getChildFile("config.txt").replaceWithText(config)) { fail("Cannot write isolated configuration."); return; }
        const auto csv = folder.getChildFile("native-response.csv");
        const juce::StringArray args {helper.getFullPathName(), output.id, csv.getFullPathName(), engineFolder.getFullPathName(), juce::String(sampleRate, 0),
            SystemEq::stageName(output.preferredStage()), folder.getChildFile("offline-telemetry.bin").getFullPathName()};
        if (!child.start(args) || !child.waitForProcessToFinish(30000) || child.getExitCode() != 0) {
            if (child.isRunning()) child.kill();
            const auto detail = child.readAllProcessOutput().trim().substring(0, 512);
            fail("Installed Equalizer APO could not process the isolated test." + (detail.isEmpty() ? juce::String() : " " + detail)); return;
        }
        auto lines = juce::StringArray::fromLines(csv.loadFileAsString()); int measured = 0;
        const auto coefficients = eqCoefficients(controls.bands, sampleRate, controls.customEnabled);
        const auto nativeImpulse = nativeCalibrationImpulse(profile, controls, sampleRate);
        const double preamp = appliedHeadroomDb(profile, controls, sampleRate);
        for (int i = 1; i < lines.size(); ++i) {
            auto columns = juce::StringArray::fromTokens(lines[i], ",", ""); if (columns.size() != 3) continue;
            const double f = columns[0].getDoubleValue(); if (f <= 0) continue;
            for (int c = 0; c < 2; ++c) {
                const double actual = columns[c+1].getDoubleValue();
                const double expected = correctionDb(profile, c, f, controls) + customEqDb(coefficients, f, sampleRate) + preamp;
                std::complex<double> realized{};
                for (int sample = 0; sample < nativeImpulse.getNumSamples(); ++sample)
                    realized += static_cast<double>(nativeImpulse.getSample(c, sample)) * std::polar(1., -juce::MathConstants<double>::twoPi * f * sample / sampleRate);
                const double backendExpected = 20 * std::log10(std::max(1e-12, std::abs(realized))) + customEqDb(coefficients, f, sampleRate) + preamp;
                if (!std::isfinite(actual)) { fail("Backend returned a nonfinite response."); return; }
                maximumError = std::max(maximumError, std::abs(actual - backendExpected));
                maximumTargetError = std::max(maximumTargetError, std::abs(actual - expected));
            }
            ++measured;
        }
        if (measured < 20 || maximumError > .03) { fail("Measured response differs from the configured native filters by " + juce::String(maximumError, 3) + " dB."); }
        else description = "Backend verified | native error " + juce::String(maximumError, 3) + " dB | ideal-curve deviation " + juce::String(maximumTargetError, 2) + " dB";
        folder.getChildFile("verification-result.json").replaceWithText(juce::JSON::toString(fields({{"passed", result.wasOk()}, {"maximum_error_db", maximumError},
            {"maximum_ideal_curve_deviation_db", maximumTargetError}, {"frequencies", measured}, {"native_dsp_confirmed", true}, {"method", "installed APO with isolated process-local configuration"}, {"audible_playback", false}})));
    } catch (const std::exception& error) { fail(error.what()); }
}
void MainComponent::verifyBackend() {
    if (verification || !profile) return;
    OutputEndpoint selected = systemOutput;
    if (selected.id.isEmpty()) for (const auto& o : availableOutputs) if (o.isDefault) { selected = o; break; }
    if (selected.id.isEmpty() || !selected.apoInstalled) { showError("Enable Soundee on an output before verifying the backend."); return; }
    verification = std::make_unique<VerificationJob>(*profile, settings(), selected, logs.directory.getChildFile("verification-" + juce::Uuid().toString()), rate.load());
    status = "Verifying installed backend in an isolated process; no audio will play";
    logs.event("system_eq", "system_eq.verification.started", "[System EQ] Offline backend verification started", "running", fields({{"endpoint_guid", selected.guid}}), verification->requestId);
}
juce::var MainComponent::runBackendVerificationChecks(const juce::File& input, const juce::File& folder) {
    const auto directory = SystemEq::configDirectory();
    const auto root = directory.getChildFile("config.txt"), active = directory.getChildFile("Soundee/active.txt");
    const auto rootBefore = juce::SHA256(root), activeBefore = juce::SHA256(active);
    OutputEndpoint selected; for (const auto& o : SystemEq::outputs()) if (o.isDefault) { selected = o; break; }
    if (selected.id.isEmpty() || !selected.apoInstalled) return fields({{"passed", false}, {"reason", "Default output has no installed APO"}});
    VerificationJob job(Profile::load(input), {}, selected, folder, 48000);
    if (!job.waitForThreadToExit(40000)) return fields({{"passed", false}, {"reason", "Verification timed out"}});
    const bool preserved = juce::SHA256(root) == rootBefore && juce::SHA256(active) == activeBefore;
    return fields({{"passed", job.result.wasOk() && preserved}, {"maximum_error_db", job.maximumError},
        {"maximum_ideal_curve_deviation_db", job.maximumTargetError},
        {"description", job.description}, {"live_eq_preserved", preserved}, {"audible_playback", false}});
}
}

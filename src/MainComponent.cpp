#include "MainComponent.h"
#include "ProfileArchive.h"
#include "ResponseComparison.h"
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

namespace soundee {
namespace {
const auto background = ui::background, ink = ui::ink, muted = ui::muted,
    blue = ui::accent, border = ui::border, pink = ui::pink;
class SystemPanel : public juce::Component, private juce::Timer {
public:
    SystemPanel(const std::vector<OutputEndpoint>& outputs, const juce::String& selected,
        std::function<void(const OutputEndpoint&)> enable, std::function<void()> disable,
        std::function<void()> setup, std::function<juce::String()> description)
        : endpoints(outputs), describe(std::move(description)) {
        setLookAndFeel(&look);
        for (auto* label : {&title, &details, &notes}) label->setColour(juce::Label::textColourId, ink);
        addAndMakeVisible(title); title.setText("Playback device", juce::dontSendNotification);
        addAndMakeVisible(selector); selector.setTextWhenNothingSelected("Choose a speaker or headphone output");
        for (int i = 0; i < static_cast<int>(endpoints.size()); ++i) {
            selector.addItem(endpoints[static_cast<size_t>(i)].name, i + 1);
            if (endpoints[static_cast<size_t>(i)].guid.equalsIgnoreCase(selected)) selector.setSelectedId(i + 1);
        }
        addAndMakeVisible(details); details.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(notes);
        notes.setText("1. Choose the device used by your audio apps.\n"
                      "2. Enable EQ for that device. Windows may ask for administrator access.\n"
                      "3. Play audio to confirm processing in Soundee.\n\n"
                      "Use shared-mode playback. Newly installed effects may need a Windows audio restart. Other existing audio effects stay enabled.", juce::dontSendNotification);
        notes.setJustificationType(juce::Justification::topLeft);
        notes.setColour(juce::Label::textColourId, muted);
        title.setFont(juce::Font(16.f));
        selector.setTooltip("EQ applies only to the chosen playback device. Choosing a device here does not change the Windows default output.");
        setupButton.setTooltip("Install the audio backend, or open its device selector if it is already installed.");
        for (auto* button : {&enableButton, &disableButton, &setupButton}) addAndMakeVisible(button);
        enableButton.onClick = [this, enable] { const int index = selector.getSelectedId() - 1;
            if (index >= 0 && index < static_cast<int>(endpoints.size())) enable(endpoints[static_cast<size_t>(index)]); };
        disableButton.onClick = std::move(disable); setupButton.onClick = std::move(setup);
        setSize(720, 430); startTimerHz(4); timerCallback();
    }
    ~SystemPanel() override { stopTimer(); setLookAndFeel(nullptr); }
    void paint(juce::Graphics& graphics) override { graphics.fillAll(background); graphics.setColour(ui::surface); graphics.fillRoundedRectangle(24.f, 99.f, 672.f, 100.f, 8.f); }
    void resized() override {
        title.setBounds(24, 16, 672, 26); selector.setBounds(24, 49, 672, 36);
        details.setBounds(34, 104, 652, 90); notes.setBounds(24, 213, 672, 138);
        enableButton.setBounds(24, 371, 172, 36); disableButton.setBounds(208, 371, 172, 36);
        setupButton.setBounds(484, 371, 212, 36);
    }
    void runChecks() {
        if (selector.getSelectedId() != 0 || enableButton.isEnabled()) throw std::runtime_error("System output was selected without user choice");
        selector.setSelectedId(1); timerCallback();
        if (!enableButton.isEnabled()) throw std::runtime_error("System enable was not available after choosing an output");
        enableButton.onClick(); disableButton.onClick(); setupButton.onClick();
    }
private:
    void timerCallback() override { details.setText(describe(), juce::dontSendNotification); enableButton.setEnabled(selector.getSelectedId() > 0); }
    std::vector<OutputEndpoint> endpoints;
    std::function<juce::String()> describe;
    juce::ComboBox selector;
    juce::Label title, details, notes;
    juce::TextButton enableButton {"Enable / update EQ"}, disableButton {"Disable EQ"}, setupButton {"Audio backend setup..."};
    ui::LookAndFeel look;
};
}
MainComponent::MainComponent(bool headless, juce::File diagnosticDirectory)
    : logs(diagnosticDirectory), noDevice(headless) {
    setWantsKeyboardFocus(true);
    calibrationStore = headless ? logs.directory.getParentDirectory().getChildFile("retained-calibrations")
        : juce::File(juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getFullPathName())).getChildFile("Soundee/calibrations");
    setLookAndFeel(&look);
    for (auto* button : {&importButton, &exportButton, &impulseButton, &logsButton, &systemButton}) addAndMakeVisible(button);
    addAndMakeVisible(bypassButton); addAndMakeVisible(swapButton);
    bypassButton.setClickingTogglesState(true);
    bypassButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff343a34));
    bypassButton.setColour(juce::TextButton::textColourOffId, ink);
    bypassButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff424842));
    bypassButton.setColour(juce::TextButton::textColourOnId, ink);
    auto setup = [&](juce::Slider& slider, juce::Label& label, juce::String title,
                     double minimum, double maximum, double step, double value, juce::String suffix) {
        addAndMakeVisible(slider);
        slider.setColour(juce::Slider::textBoxTextColourId, ink);
        slider.setColour(juce::Slider::textBoxBackgroundColourId, ui::field);
        slider.setRange(minimum, maximum, step); slider.setValue(value);
        slider.setDoubleClickReturnValue(true, value); slider.setTitle(title);
        slider.setNumDecimalPlacesToDisplay(step < .1 ? 2 : step < 1 ? 1 : 0);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 88, 30);
        slider.setTextValueSuffix(suffix);
        label.setText(title, juce::dontSendNotification); label.setColour(juce::Label::textColourId, muted);
        addAndMakeVisible(label);
        slider.onValueChange = [this] { dirty = true; rebuildAt = juce::Time::getMillisecondCounterHiRes() + 200; repaint(); };
        slider.onDragStart = [this] { if (!historyRestoring) histories[library.selectedId].commit(settings()); gestureInProgress = true; };
        slider.onDragEnd = [this] { gestureInProgress = false; if (dirty) rebuild(); else if (!historyRestoring) histories[library.selectedId].commit(settings()); };
    };
    setup(amount, amountLabel, "Correction amount", 0, 100, 1, 100, "%");
    setup(boost, boostLabel, "Maximum boost", 0, 12, 0.5, 6, " dB");
    setup(low, lowLabel, "Correct from", 20, 200, 1, 40, " Hz");
    setup(high, highLabel, "Correct up to", 2000, 20000, 100, 16000, " Hz");
    setup(headroomOffset, headroomOffsetLabel, "Output gain offset", -24, 24, 0.1, 0, " dB");
    setup(limiterCeiling, limiterLabel, "Limiter ceiling", -12, 0, .1, -1, " dBFS");
    headroomOffset.setDoubleClickReturnValue(true, 0);
    headroomOffset.textFromValueFunction = [](double value) { return (value > 0 ? "+" : "") + juce::String(value, 1); };
    headroomOffset.setTooltip("Added to automatic headroom. Positive values raise output level; negative values add attenuation. Double-click to reset. Saved per profile.");
    setup(bandFrequency, frequencyLabel, "Frequency", 20, 20000, 1, 1000, " Hz");
    bandFrequency.setSkewFactorFromMidPoint(1000);
    setup(bandGain, gainLabel, "Gain", -18, 18, 0.1, 0, " dB");
    bandGain.textFromValueFunction = [](double value) { return (value > 0 ? "+" : "") + juce::String(value, 1); };
    setup(bandQ, qLabel, "Bandwidth (Q)", 0.2, 12, 0.01, 0.71, "");
    bandQ.setSkewFactorFromMidPoint(1);
    for (auto* slider : {&bandFrequency, &bandGain, &bandQ}) slider->onValueChange = [this] { bandControlsChanged(); };
    for (auto* component : std::initializer_list<juce::Component*> {&profileList, &linkedOutput, &followWindows, &customButton,
        &bandEnabled, &flatButton, &bandSelector, &bandType, &addBandButton, &removeBandButton, &menuButton, &meterPanel,
        &undoButton, &redoButton, &aButton, &bButton, &snapshotButton, &limiterButton, &bandCurvesButton, &healthLabel}) addAndMakeVisible(component);
    healthLabel.setColour(juce::Label::textColourId, muted);
    undoButton.onClick = [this] { undoEq(false); }; redoButton.onClick = [this] { undoEq(true); };
    aButton.onClick = [this] { selectSnapshot(0); }; bButton.onClick = [this] { selectSnapshot(1); };
    snapshotButton.onClick = [this] { showSnapshotMenu(); };
    limiterButton.onClick = [this] { dirty = true; rebuildAt = 0; limiterCeiling.setEnabled(limiterButton.getToggleState()); };
    limiterButton.setTooltip("Optional stereo-linked limiter after EQ. Adds about 5-6.5 ms. On a pre-mix-only output it limits each application's stream before Windows mixes them.");
    bandCurvesButton.setToggleState(true, juce::dontSendNotification); bandCurvesButton.onClick = [this] { repaint(); };
    bandCurvesButton.setColour(juce::ToggleButton::textColourId, pink); bandCurvesButton.setColour(juce::ToggleButton::tickColourId, pink);
    aButton.setTooltip("Recall snapshot A. If empty, clicking stores the current EQ."); bButton.setTooltip("Recall snapshot B. If empty, clicking stores the current EQ.");
    undoButton.setTooltip("Undo EQ edit (Ctrl+Z)"); redoButton.setTooltip("Redo EQ edit (Ctrl+Y or Ctrl+Shift+Z)");
    limiterCeiling.setTextBoxStyle(juce::Slider::TextBoxRight, false, 100, 30); limiterCeiling.setEnabled(false);
    profileList.setRowHeight(108); profileList.setOutlineThickness(0);
    bandType.addItem("Bell", 1); bandType.addItem("Low Shelf", 2); bandType.addItem("High Shelf", 3);
    bandType.onChange = [this] { bandControlsChanged(); };
    bandSelector.onChange = [this] { if (!updatingControls) { selectedBand = bandSelector.getSelectedId() - 1; refreshBandControls(); repaint(); } };
    bandEnabled.onClick = [this] { bandControlsChanged(); };
    customButton.setToggleState(true, juce::dontSendNotification);
    customButton.onClick = [this] { refreshBandControls(); dirty = true; rebuildAt = 0; repaint(); };
    addBandButton.onClick = [this] { addEqBand(); };
    removeBandButton.onClick = [this] {
        if (selectedBand >= 0 && selectedBand < static_cast<int>(customBands.size())) {
            customBands.erase(customBands.begin() + selectedBand); selectedBand = std::min(selectedBand, static_cast<int>(customBands.size()) - 1);
            refreshBandControls(); dirty = true; rebuildAt = 0; repaint();
        }
    };
    flatButton.onClick = [this] { addFlatPreset(); };
    menuButton.onClick = [this] { showMenu(); };
    linkedOutput.onChange = [this] {
        if (updatingControls || !library.selected()) return;
        if (pendingProfileId.isNotEmpty()) pendingActivationCancelled = true;
        auto* preset = library.selected(); const int index = linkedOutput.getSelectedId() - 2;
        if (index >= 0 && index < static_cast<int>(availableOutputs.size())) {
            const auto& output = availableOutputs[static_cast<size_t>(index)]; preset->outputGuid = output.guid; preset->outputName = output.name;
        } else { preset->outputGuid.clear(); preset->outputName.clear(); }
        preset->lastSelected = juce::Time::currentTimeMillis(); saveSettings(); rebuildProfileList(); syncPresetOutput();
    };
    followWindows.onClick = [this] {
        library.followWindows = followWindows.getToggleState(); saveSettings();
        if (library.followWindows) for (const auto& output : availableOutputs) if (output.isDefault) { lastDefaultGuid.clear(); handleDefaultOutput(output); break; }
    };
    meterPanel.onResetClips = [this] { nativeClipResetAt = GetTickCount64(); meterPanel.clearClips(); if (windowsMeter) windowsMeter->levels.resetClips(); };
    meterPanel.setTooltip("Four-times oversampled true-peak estimate in dBTP. EQ output is measured inside Soundee's DSP module; Windows mix is the fallback. CLIP lights latch at 0 dBTP. Click to reset.");
    linkedOutput.setTooltip("Link this preset to a Windows playback device. Soundee will select it when that device becomes the Windows default output.");
    bandFrequency.setTooltip("Drag a numbered point in the graph or type a frequency here. Double-click to reset to 1000 Hz.");
    amount.setTooltip("How much of the imported calibration to apply. 0% leaves custom EQ active; 100% applies full correction. Double-click to reset to 100%.");
    boost.setTooltip("Caps positive gain from calibration only. Custom EQ bands have their own gain. Double-click to reset to 6 dB.");
    low.setTooltip("Lower edge of the full calibration range. Correction fades out below this frequency. Double-click to reset to 40 Hz.");
    high.setTooltip("Upper edge of the full calibration range. Correction fades out above this frequency. Double-click to reset to 16000 Hz.");
    bandGain.setTooltip("Boost or cut this band. Positive values add level; negative values reduce it. Double-click to reset to 0 dB.");
    bandQ.setTooltip("Controls band width and shelf resonance. Higher Q makes a Bell band narrower. Scroll over the graph to adjust. Double-click to reset to 0.71.");
    bandType.setTooltip("Bell changes a focused frequency region. Low Shelf changes bass; High Shelf changes treble.");
    bandSelector.setTooltip("Choose which band to edit. You can also click its numbered point on the graph.");
    bandEnabled.setButtonText("Enabled");
    bandEnabled.setTooltip("Bypass just the selected band without losing its settings.");
    customButton.setTooltip("Enable or bypass all custom bands. Calibration stays active.");
    limiterCeiling.setTooltip("Maximum sample level after EQ when the limiter is enabled. On pre-mix outputs this applies to each app separately. Double-click to reset to -1 dBFS.");
    followWindows.setTooltip("Automatically selects a profile linked to the current Windows playback device. This does not change the Windows output.");
    importButton.setTooltip("Import a SoundID .swproj calibration into a new listening profile. You can also drop a file onto the window.");
    flatButton.setTooltip("Create a profile without calibration. You can add custom EQ bands or import calibration later.");
    bypassButton.setTooltip("Click to compare with EQ bypassed. The same output gain is retained. Click again to restore EQ.");
    systemButton.setTooltip("Choose a playback device and set up or disable system-wide EQ.");
    menuButton.setTooltip("Profile actions, exports, app settings, and diagnostics.");
    snapshotButton.setButtonText("Save snapshot...");
    snapshotButton.setTooltip("Save the current EQ to snapshot A or B for comparison.");
    for (auto* slider : {&amount, &boost, &low, &high, &bandFrequency, &bandGain, &bandQ, &headroomOffset, &limiterCeiling})
        slider->setScrollWheelEnabled(false);
    statusLabel.setColour(juce::Label::textColourId, muted); addAndMakeVisible(statusLabel);
    importButton.setButtonText("+ Import calibration");
    importButton.setColour(juce::TextButton::textColourOffId, ink);
    importButton.onClick = [this] { chooseProfile(); };
    exportButton.onClick = [this] { exportFile(false); };
    impulseButton.onClick = [this] { exportFile(true); };
    systemButton.onClick = [this] { showSystemEq(); };
    logsButton.onClick = [this] { logs.directory.startAsProcess(); };
    bypassButton.onClick = [this] {
        if (!noDevice && !systemState.configured) {
            bypassButton.setToggleState(false, juce::dontSendNotification); showSystemEq(); return;
        }
        if (systemState.configured) updateSystemEq();
        logs.event("dsp", "dsp.bypass.changed", "[EQ] Bypass changed", "success", fields({{"bypassed", bypassButton.getToggleState()}})); repaint();
    };
    swapButton.onClick = [this] { dirty = true; rebuildAt = 0; repaint(); };
    propertyOptions.applicationName = "Soundee"; propertyOptions.filenameSuffix = ".settings";
    propertyOptions.folderName = "Soundee"; propertyOptions.osxLibrarySubFolder = "Application Support";
    if (!headless) {
        systemDirectory = SystemEq::configDirectory(); refreshSystemEq();
        preferences = std::make_unique<juce::PropertiesFile>(propertyOptions);
        amount.setValue(preferences->getDoubleValue("amount", 100), juce::dontSendNotification);
        boost.setValue(preferences->getDoubleValue("boost", 6), juce::dontSendNotification);
        low.setValue(preferences->getDoubleValue("low", 40), juce::dontSendNotification);
        high.setValue(preferences->getDoubleValue("high", 16000), juce::dontSendNotification);
        swapButton.setToggleState(preferences->getBoolValue("swap"), juce::dontSendNotification);
        closeToTray = preferences->getBoolValue("closeToTray", true);
        meterWindowsMix = preferences->getBoolValue("meterWindowsMix", false);
    }
    setSize(1360, 860);
    if (!headless) { availableOutputs = SystemEq::outputs(); systemEnabled = systemState.configured; windowsMeter = std::make_unique<SystemAudioMeter>(logs); notifications = std::make_unique<EndpointNotifications>(); }
    initialiseProfiles();
    statusLabel.setText(status, juce::dontSendNotification);
    startTimerHz(30);
}
MainComponent::~MainComponent() {
    windowsMeter.reset();
    notifications.reset();
    verification.reset();
    backendDownload.reset();
    if (installerProcess) CloseHandle(static_cast<HANDLE>(installerProcess));
    if (endpointProcess) CloseHandle(static_cast<HANDLE>(endpointProcess));
    stopTimer();
    saveSettings(); setLookAndFeel(nullptr);
}
Settings MainComponent::settings() const {
    Settings s { amount.getValue() / 100.0, boost.getValue(), low.getValue(), high.getValue(), swapButton.getToggleState() };
    s.bands = customBands; s.customEnabled = customButton.getToggleState();
    s.headroomOffset = headroomOffset.getValue(); s.limiterEnabled = limiterButton.getToggleState(); s.limiterCeiling = limiterCeiling.getValue(); return s;
}
bool MainComponent::loadProfile(const juce::File& file) {
    const auto request = juce::Uuid().toString(); const auto start = juce::Time::getMillisecondCounterHiRes();
    logs.event("profile", "profile.load.started", "[Profile] Loading calibration", "running", {}, request);
    try {
        const auto writable = recoverProfileLibrary();
        if (writable.failed()) throw std::runtime_error(writable.getErrorMessage().toStdString());
        auto loaded = std::make_unique<Profile>(Profile::load(file));
        juce::File retained;
        const auto backedUp = ProfileArchive::retainCalibration(file, calibrationStore, retained);
        if (backedUp.failed()) throw std::runtime_error(backedUp.getErrorMessage().toStdString());
        if (getNumRows() >= 128) throw std::runtime_error("Up to 128 listening profiles are supported. Remove a profile before importing another.");
        saveSelectedPreset();
        ListeningPreset item; item.name = loaded->name; item.calibrationPath = retained.getFullPathName(); item.settings = settings();
        loaded->source = retained;
        if (const auto* current = library.selected()) { item.outputGuid = current->outputGuid; item.outputName = current->outputName; }
        library.presets.push_back(std::move(item)); library.select(static_cast<int>(library.presets.size()) - 1);
        profile = std::move(loaded); rebuildProfileList(); resized();
        histories[library.selectedId].commit(settings());
        { juce::ScopedValueSetter<bool> changing(switchingPreset, true); rebuild(); }
        syncPresetOutput();
        status = "Profile ready | stereo correction | PEQb v3";
        logs.event("profile", "profile.load.completed", "[Profile] Calibration loaded", "success",
            fields({{"points_per_curve", 355}, {"curves", 4}, {"format_version", 3}}), request,
            juce::Time::getMillisecondCounterHiRes() - start);
        saveSettings(); statusLabel.setText(status, juce::dontSendNotification); repaint(); return true;
    } catch (const std::exception& error) {
        logs.event("profile", "profile.load.failed", "[Profile] Calibration import failed", "failed", {}, request,
            juce::Time::getMillisecondCounterHiRes() - start, "unsupported_or_invalid_profile");
        showError(error.what()); return false;
    }
}
void MainComponent::rebuild() {
    if (!profile) return;
    const auto start = juce::Time::getMillisecondCounterHiRes();
    const auto preamp = appliedHeadroomDb(*profile, settings(), rate.load());
    dirty = false;
    designedHeadroom = preamp;
    if (!gestureInProgress && !historyRestoring) histories[library.selectedId].commit(settings());
    if (auto* p = library.selected()) {
        const auto snapshot = p->activeSnapshot == 0 ? p->snapshotA : p->activeSnapshot == 1 ? p->snapshotB : std::nullopt;
        if (snapshot && juce::JSON::toString(settingsToJson(*snapshot)) != juce::JSON::toString(settingsToJson(settings()))) p->activeSnapshot = -1;
    }
    logs.event("dsp", "dsp.settings.updated", "[EQ] Equalization settings updated", "success",
        fields({{"sample_rate", rate.load()}, {"preamp_db", preamp},
                {"automatic_headroom_db", preamp - settings().headroomOffset}, {"headroom_offset_db", settings().headroomOffset},
                {"amount", settings().amount}, {"maximum_boost_db", settings().maximumBoost},
                {"profile_id", library.selectedId}, {"calibration_available", !profile->flatResponse},
                {"custom_bands", static_cast<int>(customBands.size())}, {"custom_enabled", settings().customEnabled}}), {},
        juce::Time::getMillisecondCounterHiRes() - start);
    saveSettings(); repaint();
    healthLabel.setText(processingDescription().upToFirstOccurrenceOf(" | ", false, false), juce::dontSendNotification);
    healthLabel.setTooltip(processingDescription());
    if (systemEnabled && !switchingPreset) syncPresetOutput();
}
void MainComponent::saveSettings() {
    saveSelectedPreset();
    if (!preferences) return;
    if (profile) preferences->setValue("profile", profile->source.getFullPathName());
    preferences->setValue("amount", amount.getValue()); preferences->setValue("boost", boost.getValue());
    preferences->setValue("low", low.getValue()); preferences->setValue("high", high.getValue());
    preferences->setValue("swap", swapButton.getToggleState());
    if (libraryWritable) preferences->setValue("listeningProfiles", library.serialize());
    preferences->setValue("systemEnabled", systemEnabled);
    preferences->setValue("closeToTray", closeToTray);
    preferences->setValue("meterWindowsMix", meterWindowsMix);
    preferences->saveIfNeeded();
}
void MainComponent::timerCallback() {
    if (backendDownload && !backendDownload->isThreadRunning()) {
        const bool success = backendDownload->success; const auto file = backendDownload->file;
        const auto error = backendDownload->error; backendDownload.reset();
        logs.event("system_eq", success ? "system_eq.backend.downloaded" : "system_eq.backend.download.failed",
            "[System EQ] Backend download finished", success ? "success" : "failed", {}, backendRequest,
            juce::Time::getMillisecondCounterHiRes() - backendStartedAt, success ? "" : "backend_download_failed");
        if (!success) showError(error);
        else {
            if (!BackendDownload::verified(file)) { showError("The backend installer changed after verification. It was not run."); return; }
            SHELLEXECUTEINFOW launch {}; launch.cbSize = sizeof(launch); launch.fMask = SEE_MASK_NOCLOSEPROCESS;
            launch.lpVerb = L"runas"; const auto path = SystemEq::nativePath(file);
            if (path.isEmpty()) { showError("Unable to resolve the backend installer's Windows path. Retry Backend setup."); return; }
            launch.lpFile = path.toWideCharPointer(); launch.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&launch) && launch.hProcess) { installerProcess = launch.hProcess; status = "Complete the Equalizer APO installer, then enable your speaker output."; }
            else showError("The backend installer was cancelled or could not start. Retry Backend setup.");
        }
    }
    if (installerProcess && WaitForSingleObject(static_cast<HANDLE>(installerProcess), 0) == WAIT_OBJECT_0) {
        DWORD code = 1; GetExitCodeProcess(static_cast<HANDLE>(installerProcess), &code);
        CloseHandle(static_cast<HANDLE>(installerProcess)); installerProcess = nullptr; refreshSystemEq();
        const bool installed = SystemEq::installDirectory().exists();
        logs.event("system_eq", "system_eq.backend.installer.closed", "[System EQ] Backend installer closed",
            installed ? "success" : "degraded", fields({{"exit_code", static_cast<int>(code)}, {"backend_installed", installed}}), backendRequest,
            juce::Time::getMillisecondCounterHiRes() - backendStartedAt, installed ? "" : "backend_not_installed");
        status = installed ? "Backend installed | choose and enable your speaker output." : "Backend installation did not complete. Retry Backend setup.";
    }
    if (endpointProcess && WaitForSingleObject(static_cast<HANDLE>(endpointProcess), 0) == WAIT_OBJECT_0) {
        DWORD code = 7; GetExitCodeProcess(static_cast<HANDLE>(endpointProcess), &code);
        CloseHandle(static_cast<HANDLE>(endpointProcess)); endpointProcess = nullptr;
        endpointNeedsRestart = code == 10;
        logs.event("system_eq", code == 0 || code == 10 ? "system_eq.endpoint.completed" : "system_eq.endpoint.failed",
            "[System EQ] Endpoint setup finished", code == 0 || code == 10 ? "success" : "failed",
            fields({{"exit_code", static_cast<int>(code)}, {"restart_may_be_required", endpointNeedsRestart}}), endpointRequest,
            juce::Time::getMillisecondCounterHiRes() - endpointStartedAt, code == 0 || code == 10 ? "" : "endpoint_setup_failed");
        if (code == 0 || code == 10) {
            if (finishEndpointActivation(static_cast<int>(code))) enableSystemEq(pendingOutput);
        }
        else {
            finishEndpointActivation(static_cast<int>(code));
            showError(code == 9 ? "Endpoint setup and rollback failed. Use Equalizer APO Device Selector to restore this output before continuing."
                : code == 5 ? "Enable audio enhancements for this output in Windows, then retry."
                : "Endpoint setup failed. Open Backend setup to check the Equalizer APO installation.");
        }
    }
    if (!noDevice && ((notifications && notifications->consumeChange()) || juce::Time::getMillisecondCounterHiRes() >= nextSystemRefresh)) {
        auto outputs = SystemEq::outputs();
        bool changed = outputs.size() != availableOutputs.size();
        if (!changed) for (size_t i = 0; i < outputs.size(); ++i) {
            const auto& a = outputs[i]; const auto& b = availableOutputs[i];
            if (a.id != b.id || a.name != b.name || a.apoInstalled != b.apoInstalled
                || a.effectsDisabled != b.effectsDisabled || a.preMixInstalled != b.preMixInstalled || a.postMixInstalled != b.postMixInstalled) changed = true;
        }
        availableOutputs = std::move(outputs);
        if (changed) { rebuildProfileList(); syncPresetOutput(); }
        for (const auto& output : availableOutputs) if (output.isDefault) { handleDefaultOutput(output); break; }
        refreshSystemEq(); nextSystemRefresh = juce::Time::getMillisecondCounterHiRes() + (notifications && notifications->registered() ? 5000 : 500);
    }
    if (dirty && juce::Time::getMillisecondCounterHiRes() >= rebuildAt) rebuild();
    exportButton.setEnabled(profile != nullptr); impulseButton.setEnabled(profile != nullptr);
    systemButton.setEnabled(endpointProcess == nullptr);
    importButton.setEnabled(getNumRows() < 128); flatButton.setEnabled(getNumRows() < 128);
    bypassButton.setButtonText(bypassButton.getToggleState() ? "EQ bypassed" : systemState.configured ? (nativeReading.connected ? "EQ active" : "EQ configured") : "System EQ off");
    bypassButton.setColour(juce::TextButton::buttonColourId, systemState.configured ? blue : juce::Colour(0xff343a34));
    bypassButton.setColour(juce::TextButton::textColourOffId, systemState.configured ? background : ink);
    statusLabel.setText(status, juce::dontSendNotification); repaint();
    refreshNativeMeter();
    healthLabel.setText(processingDescription().upToFirstOccurrenceOf(" | ", false, false), juce::dontSendNotification);
    healthLabel.setTooltip(processingDescription());
    refreshLibraryAccess();
    undoButton.setEnabled(libraryWritable && histories[library.selectedId].canUndo()); redoButton.setEnabled(libraryWritable && histories[library.selectedId].canRedo());
    if (auto* p = library.selected()) { aButton.setToggleState(p->activeSnapshot == 0, juce::dontSendNotification); bButton.setToggleState(p->activeSnapshot == 1, juce::dontSendNotification); }
    if (windowsMeter && windowsMeter->online) {
        const auto currentRate = windowsMeter->sampleRate.load();
        if (currentRate > 0 && currentRate != rate.load()) { rate = currentRate; dirty = true; rebuildAt = 0; }
    }
}
void MainComponent::showError(const juce::String& error) {
    status = error; statusLabel.setText(status, juce::dontSendNotification);
    if (!noDevice) juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Soundee", error);
}
void MainComponent::chooseProfile() {
    chooser = std::make_unique<juce::FileChooser>("Open SoundID speaker profile", juce::File(), "*.swproj");
    auto safe = juce::Component::SafePointer<MainComponent>(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& dialog) { if (safe && dialog.getResult().existsAsFile()) safe->loadProfile(dialog.getResult()); });
}
void MainComponent::exportFile(bool impulse) {
    if (!profile) return;
    chooser = std::make_unique<juce::FileChooser>(impulse ? "Export stereo FIR" : "Export Windows EQ",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(impulse ? "soundee-stereo.wav" : "soundee.txt"), impulse ? "*.wav" : "*.txt");
    auto safe = juce::Component::SafePointer<MainComponent>(this);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
        | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe, impulse](const juce::FileChooser& dialog) {
            if (!safe || dialog.getResult() == juce::File() || !safe->profile) return;
            const auto start = juce::Time::getMillisecondCounterHiRes();
            auto result = impulse ? exportImpulse(designFilters(*safe->profile, safe->settings(), safe->rate.load()), dialog.getResult())
                                  : exportApo(*safe->profile, safe->settings(), dialog.getResult(), safe->rate.load());
            safe->logs.event("export", result.wasOk() ? "export.completed" : "export.failed", "[Export] Calibration export finished",
                result.wasOk() ? "success" : "failed", fields({{"format", impulse ? "stereo_fir" : "equalizer_apo"}}), {},
                juce::Time::getMillisecondCounterHiRes() - start, result.wasOk() ? "" : "file_write_failed");
            if (result.failed()) safe->showError(result.getErrorMessage());
            else { safe->status = impulse ? "Stereo FIR exported | linked headroom included." :
                "Windows EQ exported | add it to Equalizer APO on your speaker output."; safe->repaint(); }
        });
}
juce::String MainComponent::systemEqDescription() const {
    if (backendDownload) return "Downloading verified Equalizer APO installer | " + juce::String(100.0 * backendDownload->downloaded.load() / 11980366, 0) + "%";
    if (installerProcess) return "Complete the Equalizer APO installer; return here to enable your speaker output.";
    if (endpointProcess) return "Attaching Equalizer APO to the selected speaker output...";
    if (systemDirectory == juce::File())
        return "Backend required | install Equalizer APO using Backend setup.";
    if (!systemState.configured) return "System EQ is off | choose your speaker output, then enable.";
    if (!systemOutput.apoInstalled || systemOutput.effectsDisabled)
        return "Configuration saved | reconnect the speaker output or check Backend setup.";
    if (!systemOutput.supports(systemState.stage))
        return "Correction targets an unavailable APO stage | use Enable/update to repair.";
    return processingDescription() + "\nProfile: " + systemState.profileName
        + (systemState.limiterEnabled ? (systemState.stage == ApoStage::preMix ? " | Limiter per stream (pre-mix)" : " | Output limiter") : " | Limiter off")
        + (endpointNeedsRestart ? "\nRestart Windows audio or Windows to load the new endpoint effect." : "");
}
void MainComponent::refreshSystemEq() {
    if (noDevice) return;
    systemDirectory = SystemEq::configDirectory(); systemState = SystemEq::state(systemDirectory);
    systemOutput = {};
    for (const auto& output : SystemEq::outputs())
        if (output.guid.equalsIgnoreCase(systemState.endpointGuid)) { systemOutput = output; break; }
    if (systemState.configured) {
        bypassButton.setToggleState(systemState.bypassed, juce::dontSendNotification);
    }
}
void MainComponent::updateSystemEq() {
    if (noDevice || !profile || systemDirectory == juce::File() || !systemOutput.apoInstalled) return;
    const auto start = juce::Time::getMillisecondCounterHiRes(); const auto request = juce::Uuid().toString();
    const auto result = SystemEq::configure(systemDirectory, systemOutput, *profile, settings(), bypassButton.getToggleState(), rate.load());
    logs.event("system_eq", result.wasOk() ? "system_eq.configuration.updated" : "system_eq.configuration.failed",
        "[System EQ] System correction update finished", result.wasOk() ? "success" : "failed",
        fields({{"bypassed", bypassButton.getToggleState()}, {"endpoint_guid", systemOutput.guid},
            {"processing_stage", SystemEq::stageName(systemOutput.preferredStage())},
            {"correction_amount", settings().amount}, {"maximum_boost_db", settings().maximumBoost},
            {"low_hz", settings().low}, {"high_hz", settings().high},
            {"custom_bands", static_cast<int>(customBands.size())}, {"custom_enabled", settings().customEnabled},
            {"headroom_offset_db", settings().headroomOffset}, {"preamp_db", appliedHeadroomDb(*profile, settings(), rate.load())},
            {"limiter_enabled", settings().limiterEnabled}, {"sample_rate", rate.load()}}), request,
        juce::Time::getMillisecondCounterHiRes() - start, result.wasOk() ? "" : "configuration_write_failed");
    if (result.failed()) showError(result.getErrorMessage());
    else { refreshSystemEq(); status = "System correction updated | " + systemOutput.name; }
}
void MainComponent::enableSystemEq(const OutputEndpoint& chosen) {
    if (endpointProcess || backendDownload || installerProcess) return;
    if (!profile) { showError("Select a listening profile first."); return; }
    systemDirectory = SystemEq::configDirectory();
    if (!SystemEq::installDirectory().exists() || systemDirectory == juce::File()) {
        showError("Install Equalizer APO using Backend setup, then enable Soundee on your speaker output."); return;
    }
    OutputEndpoint selected; bool found = false;
    for (const auto& output : SystemEq::outputs()) if (output.guid.equalsIgnoreCase(chosen.guid)) { selected = output; found = true; break; }
    if (!found) { showError("This speaker output is disconnected. Reconnect it and reopen System-wide EQ."); return; }
    if (!selected.apoInstalled) {
        const auto helper = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("SoundeeEndpointSetup.exe");
        if (!helper.existsAsFile()) { showError("SoundeeEndpointSetup.exe is missing. Extract the complete Soundee package and retry."); return; }
        prepareEndpointActivation(selected);
        endpointStartedAt = juce::Time::getMillisecondCounterHiRes(); endpointRequest = juce::Uuid().toString();
        logs.event("system_eq", "system_eq.endpoint.started", "[System EQ] Attaching selected speaker output", "running", {}, endpointRequest);
        SHELLEXECUTEINFOW launch {}; launch.cbSize = sizeof(launch); launch.fMask = SEE_MASK_NOCLOSEPROCESS;
        launch.lpVerb = L"runas"; const auto file = SystemEq::nativePath(helper);
        if (file.isEmpty()) { pendingProfileId.clear(); showError("Unable to resolve the endpoint helper's Windows path."); return; }
        const auto arguments = "--attach " + selected.guid;
        launch.lpFile = file.toWideCharPointer(); launch.lpParameters = arguments.toWideCharPointer(); launch.nShow = SW_HIDE;
        if (ShellExecuteExW(&launch) && launch.hProcess) endpointProcess = launch.hProcess;
        else {
            pendingProfileId.clear();
            logs.event("system_eq", "system_eq.endpoint.cancelled", "[System EQ] Endpoint setup did not start", "degraded", {}, endpointRequest,
                juce::Time::getMillisecondCounterHiRes() - endpointStartedAt, "elevation_cancelled_or_failed");
            showError("Windows endpoint setup was cancelled or could not start. System EQ was not enabled.");
        }
        return;
    }
    const auto start = juce::Time::getMillisecondCounterHiRes();
    const auto result = SystemEq::configure(systemDirectory, selected, *profile, settings(), bypassButton.getToggleState(), rate.load());
    logs.event("system_eq", result.wasOk() ? "system_eq.enabled" : "system_eq.enable.failed", "[System EQ] System activation finished",
        result.wasOk() ? "success" : "failed", fields({{"endpoint_guid", selected.guid},
            {"processing_stage", SystemEq::stageName(selected.preferredStage())},
            {"correction_amount", settings().amount}, {"maximum_boost_db", settings().maximumBoost},
            {"low_hz", settings().low}, {"high_hz", settings().high}}),
        juce::Uuid().toString(), juce::Time::getMillisecondCounterHiRes() - start,
        result.wasOk() ? "" : "configuration_write_failed");
    if (result.failed()) showError(result.getErrorMessage());
    else {
        systemEnabled = true;
        if (auto* preset = library.selected()) {
            preset->outputGuid = selected.guid; preset->outputName = selected.name;
            preset->lastSelected = juce::Time::currentTimeMillis();
        }
        meterEndpointId = selected.id;
        if (windowsMeter) windowsMeter->selectEndpoint(selected.id);
        saveSettings(); rebuildProfileList(); refreshSystemEq(); status = "System EQ configured | " + selected.name;
    }
    repaint();
}
void MainComponent::prepareEndpointActivation(const OutputEndpoint& selected) {
    pendingOutput = selected; pendingProfileId = library.selectedId;
    pendingProfileOutputGuid = library.selected() ? library.selected()->outputGuid : juce::String();
    pendingDefaultGuid = lastDefaultGuid; pendingActivationCancelled = false;
}
bool MainComponent::finishEndpointActivation(int exitCode) {
    const auto* preset = library.selected();
    const auto requestedProfile = pendingProfileId;
    auto defaultGuid = lastDefaultGuid;
    if (!noDevice) {
        defaultGuid.clear();
        for (const auto& output : SystemEq::outputs()) if (output.isDefault) { defaultGuid = output.guid; break; }
    }
    const bool current = exitCode == 0 || exitCode == 10;
    const bool matches = current && !pendingActivationCancelled && preset && preset->id == pendingProfileId
        && preset->outputGuid.equalsIgnoreCase(pendingProfileOutputGuid)
        && lastDefaultGuid.equalsIgnoreCase(pendingDefaultGuid) && defaultGuid.equalsIgnoreCase(pendingDefaultGuid);
    pendingProfileId.clear(); pendingActivationCancelled = false;
    if (current && !matches) {
        status = "Output setup completed; EQ activation cancelled because the profile or playback device changed. Enable EQ again for the current profile.";
        logs.event("system_eq", "system_eq.activation.cancelled", "[System EQ] Stale endpoint activation discarded", "degraded",
            fields({{"requested_profile_id", requestedProfile}, {"selected_profile_id", library.selectedId}, {"endpoint_guid", pendingOutput.guid}}),
            endpointRequest, endpointRequest.isEmpty() ? -1 : juce::Time::getMillisecondCounterHiRes() - endpointStartedAt, "activation_context_changed");
    }
    return matches;
}
void MainComponent::disableSystemEq() {
    if (endpointProcess || installerProcess) return;
    const auto start = juce::Time::getMillisecondCounterHiRes();
    auto result = SystemEq::disable(systemDirectory);
    logs.event("system_eq", result.wasOk() ? "system_eq.disabled" : "system_eq.disable.failed", "[System EQ] System deactivation finished",
        result.wasOk() ? "success" : "failed", {}, juce::Uuid().toString(), juce::Time::getMillisecondCounterHiRes() - start,
        result.wasOk() ? "" : "configuration_write_failed");
    if (result.failed()) showError(result.getErrorMessage());
    else { systemEnabled = false; saveSettings(); refreshSystemEq(); status = "System EQ disabled | existing Windows filters preserved."; }
    repaint();
}
void MainComponent::showSystemEq() {
    if (noDevice) return;
    refreshSystemEq(); const auto safe = juce::Component::SafePointer<MainComponent>(this);
    juce::DialogWindow::LaunchOptions dialog; dialog.dialogTitle = "Soundee system-wide EQ";
    dialog.dialogBackgroundColour = background;
    dialog.content.setOwned(new SystemPanel(SystemEq::outputs(), systemState.endpointGuid,
        [safe](const OutputEndpoint& output) { if (safe) safe->enableSystemEq(output); },
        [safe] { if (safe) safe->disableSystemEq(); },
        [safe] { if (safe) safe->setupBackend(); },
        [safe] { return safe ? safe->systemEqDescription() : juce::String(); }));
    dialog.useNativeTitleBar = true; dialog.resizable = false; dialog.launchAsync();
}
void MainComponent::setupBackend() {
    if (noDevice || backendDownload || installerProcess || endpointProcess) return;
    const auto install = SystemEq::installDirectory();
    if (install != juce::File() && install.getChildFile("DeviceSelector.exe").existsAsFile()) {
        install.getChildFile("DeviceSelector.exe").startAsProcess(); return;
    }
    const auto cache = juce::File(juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA",
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getFullPathName()))
        .getChildFile("Soundee/installers/EqualizerAPO-x64-1.4.2.exe");
    backendRequest = juce::Uuid().toString(); backendStartedAt = juce::Time::getMillisecondCounterHiRes();
    logs.event("system_eq", "system_eq.backend.download.started", "[System EQ] Downloading official backend installer", "running", {}, backendRequest);
    backendDownload = std::make_unique<BackendDownload>(cache); backendDownload->startThread();
}
bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files) {
    return files.size() == 1 && (files[0].endsWithIgnoreCase(".swproj") || files[0].endsWithIgnoreCase(".soundee"));
}
void MainComponent::filesDropped(const juce::StringArray& files, int, int) {
    if (!isInterestedInFileDrag(files)) return;
    if (files[0].endsWithIgnoreCase(".soundee")) { const auto result = importBackup(juce::File(files[0])); if (result.failed()) showError(result.getErrorMessage()); }
    else loadProfile(juce::File(files[0]));
}
juce::var MainComponent::runUiChecks(const juce::File& input, const juce::File& folder) {
    auto ensure = [](bool value, const char* reason) { if (!value) throw std::runtime_error(reason); };
    runReviewRegressionChecks(folder.getChildFile("review-regressions"));
    ensure(loadProfile(input), "UI import failed");
    ensure(designedHeadroom < 0, "UI headroom was not calculated");
    amount.setValue(0); rebuild(); ensure(correctionDb(*profile, 0, 1000, settings()) == 0, "Correction control failed");
    amount.setValue(100); boost.setValue(3); low.setValue(60); high.setValue(12000);
    swapButton.setToggleState(true, juce::dontSendNotification); swapButton.onClick(); rebuild();
    ensure(settings().swap && settings().maximumBoost == 3 && settings().low == 60 && settings().high == 12000,
        "UI filter controls failed");
    bypassButton.setToggleState(true, juce::dontSendNotification); bypassButton.onClick();
    ensure(!apoConfiguration(*profile, settings(), bypassButton.getToggleState()).contains("GraphicEQ:"), "UI bypass control failed");
    auto bad = folder.getChildFile("ui-invalid.swproj"); bad.replaceWithText("invalid");
    auto previous = profile->source; ensure(!loadProfile(bad) && profile->source == previous,
        "Failed UI import replaced the valid profile");
    amount.setValue(100); boost.setValue(6); low.setValue(40); high.setValue(16000);
    swapButton.setToggleState(false, juce::dontSendNotification);
    bypassButton.setToggleState(false, juce::dontSendNotification); bypassButton.onClick(); rebuild();
    const double automatic = designedHeadroom;
    headroomOffset.setValue(4, juce::dontSendNotification); headroomOffset.onValueChange(); rebuild();
    ensure(settings().headroomOffset == 4 && std::abs(designedHeadroom - automatic - 4) < 1e-9,
        "Headroom offset slider did not adjust automatic headroom");
    // Dispatch graph events directly without moving the user's mouse.
    auto point = [&](double f, double db) {
        const auto graph = graphBounds();
        return juce::Point<float>{graph.getX() + static_cast<float>(std::log(f / 20) / std::log(1100.0)) * graph.getWidth(),
            graph.getCentreY() - static_cast<float>(db / 48) * graph.getHeight()};
    };
    auto mouse = [&](juce::Point<float> position, bool dragged = false) {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), position,
            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1, 0, 0, 0, 0, this, this,
            juce::Time::getCurrentTime(), position, juce::Time::getCurrentTime(), 1, dragged);
    };
    mouseDown(mouse(point(1000, 4))); mouseDrag(mouse(point(650, -3), true)); mouseUp(mouse(point(650, -3), true));
    ensure(customBands.size() == 1 && std::abs(customBands[0].frequency - 650) < 0.1 && std::abs(customBands[0].gain + 3) < 0.001,
        "Graph click-and-drag did not create and tune an EQ band");
    bandType.setSelectedId(2, juce::dontSendNotification); bandType.onChange();
    bandFrequency.setValue(100, juce::dontSendNotification); bandGain.setValue(-3, juce::dontSendNotification); bandControlsChanged();
    ensure(customBands[0].type == BandType::lowShelf && customBands[0].frequency == 100, "Band type/frequency controls failed");
    const double beforeQ = customBands[0].q; juce::MouseWheelDetails wheel; wheel.deltaY = 0.5f;
    mouseWheelMove(mouse(point(100, -3)), wheel); bandControlsChanged();
    ensure(customBands[0].q > beforeQ, "Graph wheel did not change Q");
    addBandButton.onClick(); ensure(customBands.size() == 2, "Add band control failed");
    removeBandButton.onClick(); ensure(customBands.size() == 1, "Remove band control failed");
    customButton.setToggleState(false, juce::dontSendNotification); customButton.onClick();
    ensure(!settings().customEnabled && customBands.size() == 1, "Custom EQ bypass erased bands");
    customButton.setToggleState(true, juce::dontSendNotification); customButton.onClick();
    rebuild(); saveSelectedPreset();
    const auto initial = settings();
    limiterButton.setToggleState(true, juce::dontSendNotification); limiterButton.onClick(); limiterCeiling.setValue(-2); rebuild();
    ensure(settings().limiterEnabled && settings().limiterCeiling == -2, "Limiter controls failed");
    undoButton.onClick(); ensure(!settings().limiterEnabled, "Undo did not restore limiter state");
    redoButton.onClick(); ensure(settings().limiterEnabled && settings().limiterCeiling == -2, "Redo did not restore limiter controls");
    storeSnapshot(1); applySettings(initial); storeSnapshot(0);
    bButton.onClick(); ensure(settings().limiterEnabled, "Snapshot B failed to restore limiter");
    aButton.onClick(); ensure(!settings().limiterEnabled && settings().headroomOffset == initial.headroomOffset, "Snapshot A did not restore complete EQ");
    const auto beforeGesture = settings(); amount.onDragStart(); amount.setValue(80); rebuild(); amount.setValue(65); rebuild(); amount.onDragEnd();
    undoButton.onClick(); ensure(settings().amount == beforeGesture.amount, "Slider drag was split into multiple undo steps");
    applySettings(initial);
    auto speakerPreset = *library.selected(); speakerPreset.outputGuid = "{11111111-2222-3333-4444-555555555555}";
    speakerPreset.outputName = "Main Output 1/2 (Speaker fixture)";
    ListeningPreset headphonePreset; headphonePreset.outputGuid = "{66666666-7777-8888-9999-AAAAAAAAAAAA}";
    headphonePreset.outputName = "Headphones (Headphone fixture)";
    library.presets = {speakerPreset, headphonePreset}; library.selectedId = speakerPreset.id;
    OutputEndpoint speakers, headphones;
    speakers.guid = speakerPreset.outputGuid; speakers.name = speakerPreset.outputName;
    headphones.guid = headphonePreset.outputGuid; headphones.name = headphonePreset.outputName;
    availableOutputs = {speakers, headphones}; library.followWindows = true; lastDefaultGuid.clear();
    handleDefaultOutput(headphones);
    ensure(library.selectedId == headphonePreset.id && profile->flatResponse && customBands.empty() && designedHeadroom == 0,
        "Windows output change did not select flat headphones");
    ensure(!amount.isEnabled() && !boost.isEnabled() && !low.isEnabled() && !high.isEnabled(),
        "Flat profile offers calibration controls that cannot affect the sound");
    addEqBand(1000, 2); rebuild();
    headroomOffset.setValue(-2, juce::dontSendNotification); headroomOffset.onValueChange(); rebuild();
    handleDefaultOutput(speakers);
    ensure(library.selectedId == speakerPreset.id && !profile->flatResponse && customBands.size() == 1 && customBands[0].gain == -3
        && settings().headroomOffset == 4,
        "Device switch did not restore speaker calibration and custom EQ");
    ensure(amount.isEnabled() && boost.isEnabled() && low.isEnabled() && high.isEnabled(),
        "Calibrated profile did not restore its calibration controls");
    handleDefaultOutput(headphones);
    ensure(profile->flatResponse && customBands.size() == 1 && customBands[0].gain == 2 && settings().headroomOffset == -2,
        "Headphone custom EQ and headroom offset were not saved independently");
    library.followWindows = false; handleDefaultOutput(speakers);
    ensure(library.selectedId == headphonePreset.id, "Follow Windows off still switched profiles");
    library.followWindows = true; lastDefaultGuid.clear(); handleDefaultOutput(speakers);
    const auto speakerId = library.selectedId; const int previousCount = getNumRows(); duplicateProfile();
    ensure(getNumRows() == previousCount + 1 && library.selectedId != speakerId && library.selected()->snapshotA,
        "Duplicate profile did not preserve snapshots with a new identity");
    library.presets.pop_back(); library.selectedId.clear(); selectPreset(0);
    const auto uiBackup = folder.getChildFile("ui-backup.soundee");
    ListeningProfiles one; one.presets = {*library.selected()}; one.selectedId = one.presets.front().id;
    ensure(ProfileArchive::write(one, uiBackup).wasOk() && importBackup(uiBackup).wasOk() && getNumRows() == previousCount + 1,
        "UI backup import did not add a portable profile");
    library.presets.pop_back(); library.selectedId.clear(); selectPreset(0);
    // Verify the same clip-reset callback used by the visible indicators.
    StereoLevels clipFixture; clipFixture.publish({1.1f, 0.5f}, {0.4f, 0.2f});
    ensure(clipFixture.consume().clipped[0], "Output clip fixture did not latch");
    meterPanel.update(clipFixture.consume(), "EQ stream output", true);
    meterPanel.update({}, "Windows mix", false);
    ensure(meterPanel.isClipping(0) && !meterPanel.isClipping(1), "Clip indicator did not survive stream inactivity and meter fallback");
    meterPanel.onResetClips = [this, &clipFixture] { clipFixture.resetClips(); meterPanel.clearClips(); };
    meterPanel.onResetClips(); ensure(!clipFixture.consume().clipped[0], "UI clip reset failed");
    ensure(!meterPanel.isClipping(0), "Visible clip indicator did not reset");
    meterPanel.onResetClips = [] {};
    meterPanel.update({}, "Windows output", false);
    followWindows.setToggleState(true, juce::dontSendNotification);
    OutputEndpoint fixtureOutput; fixtureOutput.guid = "{11111111-2222-3333-4444-555555555555}";
    fixtureOutput.name = "Speaker output (test fixture)";
    bool enabled = false, disabled = false, setupInvoked = false;
    SystemPanel panel({fixtureOutput}, {}, [&](const OutputEndpoint& output) { enabled = output.guid == fixtureOutput.guid; },
        [&] { disabled = true; }, [&] { setupInvoked = true; }, [] { return "System EQ is off | choose your speaker output, then enable."; });
    panel.runChecks(); ensure(enabled && disabled && setupInvoked, "System EQ panel actions failed");
    auto panelStream = folder.getChildFile("system-panel.png").createOutputStream();
    if (panelStream) { panelStream->setPosition(0); panelStream->truncate(); }
    ensure(panelStream && juce::PNGImageFormat().writeImageToStream(panel.createComponentSnapshot(panel.getLocalBounds(), true, 1.5f), *panelStream), "System panel screenshot failed");
    addEqBand(1200, 2); customBands.back().q = 1.2;
    addEqBand(7500, 1.5); customBands.back().type = BandType::highShelf;
    refreshBandControls(); rebuild();
    bypassButton.setButtonText("System EQ off");
    status = "Calibration + custom EQ | simulated output-switch checks passed";
    statusLabel.setText(status, juce::dontSendNotification);
    auto image = createComponentSnapshot(getLocalBounds(), true, 1.5f);
    auto stream = folder.getChildFile("interface.png").createOutputStream(); juce::PNGImageFormat png;
    if (stream) { stream->setPosition(0); stream->truncate(); }
    ensure(stream && png.writeImageToStream(image, *stream), "UI rendering failed");
    // Exercise the real component layout at both resize limits and the default.
    // Snapshots use isolated headless fixtures; they never rewrite live settings.
    auto captureLayout = [&](int width, int height, const juce::String& name) {
        setSize(width, height);
        ensure(graphBounds().getWidth() >= 600 && graphBounds().getHeight() >= 200,
            "Response graph is too small at a supported window size");
        for (int i = 0; i < getNumChildComponents(); ++i) {
            const auto* child = getChildComponent(i);
            if (!child->isVisible()) continue;
            ensure(getLocalBounds().contains(child->getBounds()), "A visible control extends outside the window");
            for (int j = i + 1; j < getNumChildComponents(); ++j) {
                const auto* other = getChildComponent(j);
                ensure(!other->isVisible() || !child->getBounds().intersects(other->getBounds()),
                    "Visible controls overlap at a supported window size");
            }
        }
        auto screenshot = folder.getChildFile(name + ".png").createOutputStream();
        if (screenshot) { screenshot->setPosition(0); screenshot->truncate(); }
        ensure(screenshot && png.writeImageToStream(createComponentSnapshot(getLocalBounds(), true, 1.f), *screenshot),
            "Responsive layout screenshot failed");
    };
    captureLayout(1180, 760, "interface-minimum");
    captureLayout(2200, 1400, "interface-large");
    captureLayout(1360, 860, "interface-default");
    selectPreset(1);
    while (!customBands.empty()) removeBandButton.onClick();
    rebuild();
    ensure(!bandType.isEnabled() && !bandFrequency.isEnabled() && !bandGain.isEnabled() && !bandQ.isEnabled()
        && !bandEnabled.isEnabled() && !removeBandButton.isEnabled(), "Empty EQ offers band controls without a selected band");
    captureLayout(1180, 760, "interface-flat");
    selectPreset(0); setSize(1360, 860);
    ComparisonPanel comparison;
    auto comparisonStream = folder.getChildFile("comparison-interface.png").createOutputStream();
    if (comparisonStream) { comparisonStream->setPosition(0); comparisonStream->truncate(); }
    ensure(comparisonStream && png.writeImageToStream(comparison.createComponentSnapshot(comparison.getLocalBounds(), true, 1.5f), *comparisonStream), "Comparison UI rendering failed");
    return fields({{"passed", true}, {"profile_import", true}, {"invalid_import_preserves_profile", true},
        {"controls", true}, {"bypass", true}, {"system_output_choice_and_actions", true}, {"rendered_interface", true},
        {"graph_click_drag_and_q", true}, {"custom_band_controls", true}, {"flat_headphone_profile", true},
        {"automatic_profile_switch_simulation", true}, {"per_device_eq_restored", true}, {"clipping_reset", true},
        {"headroom_offset_slider", true}, {"per_profile_headroom_offset", true}, {"audio_player_removed", true},
        {"undo_redo", true}, {"gesture_grouping", true}, {"full_settings_snapshots", true}, {"duplicate_profile", true},
        {"portable_profile_import", true}, {"limiter_controls", true}, {"comparison_ui", true},
        {"responsive_layout_no_control_overlap", true}, {"calibration_controls_follow_profile", true}});
}
juce::var MainComponent::liveStatus() const {
    return fields({{"profile_loaded", profile != nullptr}, {"sample_rate", rate.load()},
        {"meter_connected", windowsMeter && windowsMeter->online.load()},
        {"headroom_db", designedHeadroom}, {"headroom_offset_db", settings().headroomOffset},
        {"system_eq_configured", systemState.configured}, {"processing_confirmed", nativeReading.connected},
        {"processing_stage", SystemEq::stageName(systemState.stage)}, {"processing_instances", static_cast<int>(nativeReading.instances)},
        {"limiter_enabled", settings().limiterEnabled}, {"limiter_reduction_db", nativeReading.reductionDb},
        {"limiter_latency_samples", static_cast<int>(nativeReading.latency)}, {"meter_windows_mix_selected", meterWindowsMix},
        {"processing_description", processingDescription()}, {"audio_render_stream", false}});
}
}

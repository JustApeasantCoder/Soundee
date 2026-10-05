#include "MainComponent.h"
#include "ProfileArchive.h"
#include <cmath>

namespace soundee {
int MainComponent::getNumRows() { return static_cast<int>(library.presets.size()); }
juce::String MainComponent::getTooltipForRow(int index) {
    if (index < 0 || index >= getNumRows()) return {};
    const auto& preset = library.presets[static_cast<size_t>(index)];
    return preset.name + "\n" + (preset.outputName.isEmpty() ? "No playback device linked" : preset.outputName)
        + "\n" + (preset.calibrationPath.isEmpty() ? "Flat response with optional custom EQ" : "Calibration with optional custom EQ");
}
void MainComponent::paintListBoxItem(int index, juce::Graphics& g, int width, int height, bool selected) {
    if (index < 0 || index >= getNumRows()) return;
    const auto& preset = library.presets[static_cast<size_t>(index)];
    const auto bounds = juce::Rectangle<float>(4.f, 3.f, float(width - 8), float(height - 6));
    g.setColour(selected ? juce::Colour(0xff363c36) : ui::surface); g.fillRoundedRectangle(bounds, 8.f);
    if (selected) { g.setColour(ui::accent); g.fillRoundedRectangle(4.f, 15.f, 3.f, float(height - 30), 1.5f); }
    g.setFont(16.f); g.setColour(ui::ink);
    g.drawText(preset.name, 16, 12, width - 32, 25, juce::Justification::centredLeft, true);
    g.setFont(13.f); g.setColour(ui::muted);
    g.drawText(preset.outputName.isEmpty() ? "No playback device linked" : preset.outputName, 16, 41, width - 32, 22, juce::Justification::centredLeft, true);
    const bool calibrated = preset.calibrationPath.isNotEmpty();
    g.setColour(selected ? ui::accent : ui::muted); g.setFont(12.f);
    g.drawText(calibrated ? "Calibrated / stereo" : "Flat / stereo", 16, 72, width - 32, 21, juce::Justification::centredLeft);

}
void MainComponent::selectedRowsChanged(int index) { if (!updatingControls) selectPreset(index); }
void MainComponent::saveSelectedPreset() {
    if (profile && !updatingControls) if (auto* preset = library.selected()) preset->settings = settings();
}
juce::Result MainComponent::recoverProfileLibrary() {
    if (libraryWritable) return juce::Result::ok();
    if (!preferences) return juce::Result::fail("Profile storage is unavailable.");
    const auto damaged = preferences->getValue("listeningProfiles");
    const auto folder = preferences->getFile().getParentDirectory().getChildFile("profile-recovery");
    const auto backup = folder.getChildFile("listening-profiles-" + juce::SHA256(damaged.toRawUTF8(), damaged.getNumBytesAsUTF8()).toHexString() + ".json");
    if (folder.createDirectory().failed()) return juce::Result::fail("Unable to save damaged profile data. Profiles are read-only until the settings folder is writable.");
    if (!backup.existsAsFile()) {
        juce::TemporaryFile temporary(backup);
        if (!temporary.getFile().replaceWithData(damaged.toRawUTF8(), damaged.getNumBytesAsUTF8())
            || !temporary.overwriteTargetFileWithTemporary())
            return juce::Result::fail("Unable to back up damaged profile data. Profile changes were not saved.");
    }
    if (backup.loadFileAsString() != damaged) return juce::Result::fail("The profile recovery backup could not be verified. Profile changes were not saved.");
    libraryWritable = true;
    logs.event("profile", "profile.library.recovered", "[Profile] Damaged profile data preserved before recovery", "degraded",
        fields({{"recovery_file", backup.getFullPathName()}}));
    return juce::Result::ok();
}
void MainComponent::initialiseProfiles() {
    const auto saved = preferences ? preferences->getValue("listeningProfiles") : juce::String();
    if (saved.isNotEmpty()) {
        const auto result = library.restore(saved);
        if (result.failed()) {
            libraryWritable = false;
            const auto recovery = recoverProfileLibrary();
            showError(recovery.failed() ? recovery.getErrorMessage()
                : result.getErrorMessage() + " The original data was saved in profile-recovery beside your settings. New profiles and imported backups will be saved normally.");
        }
    }
    if (library.presets.empty()) {
        ListeningPreset first; first.settings = settings();
        first.calibrationPath = preferences ? preferences->getValue("profile") : juce::String();
        auto calibration = first.calibrationPath.isEmpty() ? juce::File() : juce::File(first.calibrationPath);
        if (calibration.existsAsFile()) {
            try { first.name = Profile::load(calibration).name; }
            catch (...) { first.name = "Flat response"; }
        }
        first.outputGuid = systemState.endpointGuid;
        if (first.outputGuid.isEmpty()) for (const auto& output : availableOutputs) if (output.isDefault) {
            first.outputGuid = output.guid; break;
        }
        for (const auto& output : availableOutputs) if (output.guid.equalsIgnoreCase(first.outputGuid)) first.outputName = output.name;
        library.presets.push_back(std::move(first)); library.select(0);
        // Discover headphone outputs as flat placeholders; never assign a room
        // calibration to headphones merely because they become the default.
        for (const auto& output : availableOutputs) {
            if (library.forOutput(output.guid) < 0 && (output.name.containsIgnoreCase("headphone") || output.name.containsIgnoreCase("buds"))) {
                ListeningPreset headphone; headphone.name = "Flat response"; headphone.outputGuid = output.guid; headphone.outputName = output.name;
                library.presets.push_back(std::move(headphone));
            }
        }
    }
    if (preferences) systemEnabled = preferences->getBoolValue("systemEnabled", systemState.configured);
    followWindows.setToggleState(library.followWindows, juce::dontSendNotification);
    int selected = 0;
    for (int i = 0; i < getNumRows(); ++i) if (library.presets[static_cast<size_t>(i)].id == library.selectedId) selected = i;
    selectPreset(selected);
    for (const auto& output : availableOutputs) if (output.isDefault) { handleDefaultOutput(output); break; }
    refreshLibraryAccess();
}
void MainComponent::refreshLibraryAccess() {
    for (auto* component : std::initializer_list<juce::Component*> {&headroomOffset,
        &swapButton, &limiterButton, &customButton, &linkedOutput, &followWindows, &aButton, &bButton, &snapshotButton})
        component->setEnabled(libraryWritable);
    for (auto* component : std::initializer_list<juce::Component*> {&amount, &boost, &low, &high})
        component->setEnabled(libraryWritable && profile && !profile->flatResponse);
    limiterCeiling.setEnabled(libraryWritable && limiterButton.getToggleState());
    const bool bandActive = libraryWritable && selectedBand >= 0 && selectedBand < static_cast<int>(customBands.size()) && customButton.getToggleState();
    for (auto* component : std::initializer_list<juce::Component*> {&bandType, &bandFrequency, &bandGain, &bandQ, &bandEnabled, &removeBandButton})
        component->setEnabled(bandActive);
    bandSelector.setEnabled(libraryWritable);
    addBandButton.setEnabled(libraryWritable && customBands.size() < 12);
}
void MainComponent::rebuildProfileList() {
    juce::ScopedValueSetter<bool> updating(updatingControls, true);
    profileList.updateContent();
    for (int i = 0; i < getNumRows(); ++i) if (library.presets[static_cast<size_t>(i)].id == library.selectedId) profileList.selectRow(i);
    linkedOutput.clear(juce::dontSendNotification); linkedOutput.addItem("Not linked to an output", 1);
    int selected = 1;
    const auto* preset = library.selected();
    for (int i = 0; i < static_cast<int>(availableOutputs.size()); ++i) {
        const auto& output = availableOutputs[static_cast<size_t>(i)]; linkedOutput.addItem(output.name, i + 2);
        if (preset && output.guid.equalsIgnoreCase(preset->outputGuid)) selected = i + 2;
    }
    if (preset && preset->outputGuid.isNotEmpty() && selected == 1) {
        linkedOutput.addItem(preset->outputName + " (disconnected)", 1000); selected = 1000;
    }
    linkedOutput.setSelectedId(selected, juce::dontSendNotification); repaint();
}
void MainComponent::selectPreset(int index, bool automatic) {
    if (index < 0 || index >= getNumRows()) return;
    const auto previousProfileId = library.selectedId;
    if (pendingProfileId.isNotEmpty() && library.presets[static_cast<size_t>(index)].id != pendingProfileId)
        pendingActivationCancelled = true;
    saveSelectedPreset(); auto chosen = library.presets[static_cast<size_t>(index)];
    std::unique_ptr<Profile> loaded; bool missing = false;
    if (chosen.calibrationPath.isNotEmpty()) {
        try { loaded = std::make_unique<Profile>(Profile::load(juce::File(chosen.calibrationPath))); }
        catch (...) { missing = true; }
    }
    if (loaded && !loaded->source.isAChildOf(calibrationStore)) {
        juce::File retained; const auto result = ProfileArchive::retainCalibration(loaded->source, calibrationStore, retained);
        if (result.wasOk()) { chosen.calibrationPath = retained.getFullPathName(); loaded->source = retained; library.presets[static_cast<size_t>(index)].calibrationPath = chosen.calibrationPath; }
        else logs.event("profile", "profile.calibration.backup.failed", "[Profile] Existing calibration could not be backed up", "degraded", {}, {}, -1, "calibration_backup_failed");
    }
    if (!loaded) loaded = std::make_unique<Profile>(Profile::flat(missing ? "Flat response - calibration unavailable" : chosen.name));
    else loaded->name = chosen.name;
    {
        juce::ScopedValueSetter<bool> updating(updatingControls, true);
        amount.setValue(chosen.settings.amount * 100, juce::dontSendNotification);
        boost.setValue(chosen.settings.maximumBoost, juce::dontSendNotification);
        low.setValue(chosen.settings.low, juce::dontSendNotification); high.setValue(chosen.settings.high, juce::dontSendNotification);
        swapButton.setToggleState(chosen.settings.swap, juce::dontSendNotification);
        headroomOffset.setValue(chosen.settings.headroomOffset, juce::dontSendNotification);
        limiterButton.setToggleState(chosen.settings.limiterEnabled, juce::dontSendNotification);
        limiterCeiling.setValue(chosen.settings.limiterCeiling, juce::dontSendNotification); limiterCeiling.setEnabled(chosen.settings.limiterEnabled);
        customBands = chosen.settings.bands; customButton.setToggleState(chosen.settings.customEnabled, juce::dontSendNotification);
        selectedBand = customBands.empty() ? -1 : 0; draggedBand = -1;
    }
    library.select(index); profile = std::move(loaded);
    histories[library.selectedId].commit(settings()); nativeClipResetAt = 0;
    refreshBandControls(); rebuildProfileList(); resized();
    { juce::ScopedValueSetter<bool> changing(switchingPreset, true); rebuild(); }
    syncPresetOutput(); saveSettings();
    status = missing ? "Calibration unavailable. Flat response + custom EQ is active for this profile."
        : profile->flatResponse ? "Flat response | add calibration later or create a custom EQ."
        : "Calibration ready | " + profile->name;
    logs.event("profile", "profile.selection.changed", "[Profile] Listening profile selected", missing ? "degraded" : "success",
        fields({{"profile_id", chosen.id}, {"endpoint_guid", chosen.outputGuid}, {"automatic", automatic},
            {"calibration_available", !profile->flatResponse}, {"custom_bands", static_cast<int>(customBands.size())}}), {}, -1,
        missing ? "calibration_unavailable" : "");
    if (automatic && library.selectedId != previousProfileId && onAutomaticProfileChanged)
        onAutomaticProfileChanged(missing ? chosen.name + " (calibration unavailable)" : chosen.name, chosen.outputName);
}
void MainComponent::addFlatPreset(const OutputEndpoint& output, bool automatic) {
    if (getNumRows() >= 128) { status = "Up to 128 listening profiles are supported."; return; }
    const auto writable = recoverProfileLibrary(); if (writable.failed()) { showError(writable.getErrorMessage()); return; }
    saveSelectedPreset(); ListeningPreset item; item.outputGuid = output.guid; item.outputName = output.name;
    library.presets.push_back(std::move(item)); selectPreset(getNumRows() - 1, automatic);
}
void MainComponent::handleDefaultOutput(const OutputEndpoint& output) {
    if (lastDefaultGuid.equalsIgnoreCase(output.guid)) return;
    const auto previousGuid = lastDefaultGuid;
    lastDefaultGuid = output.guid;
    if (pendingProfileId.isNotEmpty()) pendingActivationCancelled = true;
    logs.event("output", "output.windows.default.changed", "[Output] Windows default playback device changed", "success",
        fields({{"endpoint_guid", output.guid}, {"previous_endpoint_guid", previousGuid}, {"auto_switch", library.followWindows}}));
    if (library.followWindows) {
        const int index = library.forOutput(output.guid);
        if (index >= 0) selectPreset(index, true); else addFlatPreset(output, true);
    }
    rebuildProfileList();
}
void MainComponent::syncPresetOutput() {
    if (noDevice || !profile) return;
    const auto* preset = library.selected(); OutputEndpoint selected;
    if (preset) for (const auto& output : availableOutputs) if (output.guid.equalsIgnoreCase(preset->outputGuid)) { selected = output; break; }
    auto meterOutput = selected;
    if (meterOutput.id.isEmpty()) for (const auto& output : availableOutputs) if (output.isDefault) { meterOutput = output; break; }
    if (meterEndpointId != meterOutput.id) meterPanel.reset();
    meterEndpointId = meterOutput.id;
    if (windowsMeter) windowsMeter->selectEndpoint(meterEndpointId);
    if (!systemEnabled) { refreshSystemEq(); return; }
    systemDirectory = SystemEq::configDirectory();
    if (!selected.apoInstalled || selected.effectsDisabled || selected.guid.isEmpty()) {
        if (SystemEq::state(systemDirectory).configured) {
            const auto disabled = SystemEq::disable(systemDirectory);
            if (disabled.failed()) { showError(disabled.getErrorMessage()); return; }
        }
        refreshSystemEq(); status = "Profile selected | use System-wide EQ to enable this output."; return;
    }
    systemOutput = selected; updateSystemEq();
}
void MainComponent::addEqBand(double f, double gain) {
    if (customBands.size() >= 12) { status = "A custom EQ supports up to 12 bands."; return; }
    customBands.push_back({BandType::bell, juce::jlimit(20.0, 20000.0, f), juce::jlimit(-18.0, 18.0, gain), 0.7071067811865476, true});
    selectedBand = static_cast<int>(customBands.size()) - 1;
    customButton.setToggleState(true, juce::dontSendNotification); refreshBandControls(); dirty = true; rebuildAt = 0; repaint();
}
void MainComponent::refreshBandControls() {
    juce::ScopedValueSetter<bool> updating(updatingControls, true);
    bandSelector.clear(juce::dontSendNotification);
    for (int i = 0; i < static_cast<int>(customBands.size()); ++i) bandSelector.addItem(juce::String(i + 1) + " - " + bandTypeName(customBands[static_cast<size_t>(i)].type), i + 1);
    const bool selected = selectedBand >= 0 && selectedBand < static_cast<int>(customBands.size());
    if (selected) {
        const auto& band = customBands[static_cast<size_t>(selectedBand)]; bandSelector.setSelectedId(selectedBand + 1, juce::dontSendNotification);
        bandType.setSelectedId(static_cast<int>(band.type) + 1, juce::dontSendNotification);
        bandFrequency.setValue(band.frequency, juce::dontSendNotification); bandGain.setValue(band.gain, juce::dontSendNotification);
        bandQ.setValue(band.q, juce::dontSendNotification); bandEnabled.setToggleState(band.enabled, juce::dontSendNotification);
    } else {
        bandSelector.setTextWhenNothingSelected("No bands yet"); bandType.setSelectedId(1, juce::dontSendNotification);
        bandFrequency.setValue(1000, juce::dontSendNotification); bandGain.setValue(0, juce::dontSendNotification);
        bandQ.setValue(.71, juce::dontSendNotification); bandEnabled.setToggleState(false, juce::dontSendNotification);
    }
    const bool active = libraryWritable && selected && customButton.getToggleState();
    for (auto* component : std::initializer_list<juce::Component*> {&bandType, &bandFrequency, &bandGain, &bandQ, &bandEnabled, &removeBandButton}) component->setEnabled(active);
    addBandButton.setEnabled(libraryWritable && customBands.size() < 12);
}
void MainComponent::bandControlsChanged() {
    if (updatingControls || selectedBand < 0 || selectedBand >= static_cast<int>(customBands.size())) return;
    auto& band = customBands[static_cast<size_t>(selectedBand)];
    band.type = static_cast<BandType>(std::max(1, bandType.getSelectedId()) - 1);
    band.frequency = bandFrequency.getValue(); band.gain = bandGain.getValue(); band.q = bandQ.getValue(); band.enabled = bandEnabled.getToggleState();
    dirty = true; rebuildAt = juce::Time::getMillisecondCounterHiRes() + 70; repaint();
    // Keep the selector's band type in sync after changing Bell/Shelf.
    refreshBandControls();
}
void MainComponent::moveBand(juce::Point<float> point) {
    if (selectedBand < 0 || selectedBand >= static_cast<int>(customBands.size())) return;
    const auto graph = graphBounds();
    auto& band = customBands[static_cast<size_t>(selectedBand)];
    band.frequency = juce::jlimit(20.0, 20000.0, 20 * std::pow(1100.0, (point.x - graph.getX()) / graph.getWidth()));
    band.gain = juce::jlimit(-18.0, 18.0, static_cast<double>((graph.getCentreY() - point.y) * 48 / graph.getHeight()));
    refreshBandControls(); dirty = true; rebuildAt = juce::Time::getMillisecondCounterHiRes() + 70; repaint();
}
void MainComponent::mouseDown(const juce::MouseEvent& event) {
    if (!libraryWritable) return;
    const auto graph = graphBounds(); if (!graph.contains(event.position)) return;
    histories[library.selectedId].commit(settings()); gestureInProgress = true;
    if (isShowing()) grabKeyboardFocus();
    const auto toPoint = [&](const EqBand& band) {
        return juce::Point<float>{graph.getX() + static_cast<float>(std::log(band.frequency / 20) / std::log(1100.0)) * graph.getWidth(),
            graph.getCentreY() - static_cast<float>(band.gain / 48) * graph.getHeight()};
    };
    selectedBand = -1;
    if (customButton.getToggleState()) for (int i = static_cast<int>(customBands.size()) - 1; i >= 0; --i)
        if (toPoint(customBands[static_cast<size_t>(i)]).getDistanceFrom(event.position) < 15) { selectedBand = i; break; }
    if (selectedBand < 0) {
        const auto f = 20 * std::pow(1100.0, (event.position.x - graph.getX()) / graph.getWidth());
        const auto gain = (graph.getCentreY() - event.position.y) * 48 / graph.getHeight(); addEqBand(f, gain);
    }
    draggedBand = selectedBand; refreshBandControls(); repaint();
}
void MainComponent::mouseDrag(const juce::MouseEvent& event) { if (draggedBand >= 0) { selectedBand = draggedBand; moveBand(event.position); } }
void MainComponent::mouseUp(const juce::MouseEvent&) {
    const bool completedGesture = gestureInProgress;
    draggedBand = -1; gestureInProgress = false;
    if (dirty) rebuildAt = 0;
    else if (completedGesture && !historyRestoring) histories[library.selectedId].commit(settings());
}
void MainComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& details) {
    if (!libraryWritable) return;
    if (!graphBounds().contains(event.position) || selectedBand < 0) return;
    bandQ.setValue(juce::jlimit(0.2, 12.0, bandQ.getValue() * std::pow(1.2, details.deltaY * 4)));
}
void MainComponent::showMenu() {
    juce::PopupMenu menu; menu.addSectionHeader("Export & backup"); menu.addItem(1, "Export Windows EQ"); menu.addItem(2, "Export stereo FIR");
    menu.addItem(8, "Export this profile (.soundee)"); menu.addItem(10, "Back up all profiles (.soundee)");
    menu.addItem(9, "Import profile or backup (.soundee)");
    menu.addSeparator(); menu.addSectionHeader("Current profile"); menu.addItem(5, "Rename profile", libraryWritable); menu.addItem(6, "Remove profile", libraryWritable && getNumRows() > 1);
    menu.addItem(7, "Duplicate profile", libraryWritable && getNumRows() < 128);
    menu.addItem(4, "Swap left / right channels", libraryWritable, swapButton.getToggleState());
    menu.addSeparator(); menu.addSectionHeader("Tools & diagnostics"); menu.addItem(3, "Open diagnostic logs"); menu.addItem(11, "Compare SoundID / Soundee recordings"); menu.addItem(12, "Verify installed EQ backend", !verification);
    menu.addSeparator(); menu.addSectionHeader("App settings"); menu.addItem(13, "Start with Windows", true, StartupRegistration::enabled()); menu.addItem(14, "Minimize and close to tray", true, closeToTray);
    menu.addItem(15, "Meter combined Windows output", true, meterWindowsMix);
    auto safe = juce::Component::SafePointer<MainComponent>(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&menuButton), [safe](int item) {
        if (!safe) return;
        if (item == 1) safe->exportFile(false); if (item == 2) safe->exportFile(true); if (item == 3) safe->logs.directory.startAsProcess();
        if (item == 4) { safe->swapButton.setToggleState(!safe->swapButton.getToggleState(), juce::dontSendNotification); safe->dirty = true; safe->rebuildAt = 0; }
        if (item == 7) safe->duplicateProfile(); if (item == 8) safe->exportProfiles(false); if (item == 9) safe->chooseBackup();
        if (item == 10) safe->exportProfiles(true); if (item == 11) safe->showComparison(); if (item == 12) safe->verifyBackend();
        if (item == 13 && !safe->noDevice) {
            const auto result = StartupRegistration::setEnabled(!StartupRegistration::enabled());
            if (result.failed()) safe->showError(result.getErrorMessage()); else safe->recordDesktopEvent("desktop.startup.changed", "Windows startup setting updated");
        }
        if (item == 14) safe->setCloseToTray(!safe->closesToTray());
        if (item == 15) { safe->meterWindowsMix = !safe->meterWindowsMix; safe->meterPanel.reset(); safe->saveSettings(); }
        if (item == 5 && safe->library.selected()) {
            auto* dialog = new juce::AlertWindow("Rename profile", "Choose a name for this listening profile.", juce::MessageBoxIconType::NoIcon);
            dialog->addTextEditor("name", safe->library.selected()->name); dialog->addButton("Save", 1); dialog->addButton("Cancel", 0);
            const auto id = safe->library.selectedId;
            dialog->enterModalState(true, juce::ModalCallbackFunction::create([safe, dialog, id](int answer) {
                if (safe && answer == 1) {
                    const auto name = dialog->getTextEditorContents("name").trim().substring(0, 256);
                    if (name.isNotEmpty()) for (auto& preset : safe->library.presets) if (preset.id == id) { preset.name = name; break; }
                    safe->saveSettings(); safe->rebuildProfileList();
                }
            }), true);
        }
        if (item == 6 && safe->getNumRows() > 1) {
            for (auto i = safe->library.presets.begin(); i != safe->library.presets.end(); ++i) if (i->id == safe->library.selectedId) { safe->library.presets.erase(i); break; }
            safe->library.selectedId.clear(); safe->selectPreset(0);
        }
    });
}
}

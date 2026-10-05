#include "MainComponent.h"
#include "ProfileArchive.h"
#include <cmath>
#include <stdexcept>

namespace soundee {
void MainComponent::runReviewRegressionChecks(const juce::File& folder) {
    auto require = [](bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); };
    require(noDevice, "Review regressions require isolated headless components");
    folder.createDirectory();
    {
        MainComponent switching(true, folder.getChildFile("notifications/logs")); switching.stopTimer();
        OutputEndpoint speakers, headphones, newOutput;
        speakers.guid = "speakers"; speakers.name = "Speakers";
        headphones.guid = "headphones"; headphones.name = "Headphones";
        newOutput.guid = "new-output"; newOutput.name = "New output";
        switching.library.selected()->outputGuid = speakers.guid;
        switching.library.selected()->outputName = speakers.name;
        switching.addFlatPreset(headphones); switching.selectPreset(0);
        switching.lastDefaultGuid = speakers.guid; switching.library.followWindows = true;
        int notifications = 0; juce::String notifiedProfile, notifiedOutput;
        switching.onAutomaticProfileChanged = [&](const juce::String& profileName, const juce::String& outputName) {
            ++notifications; notifiedProfile = profileName; notifiedOutput = outputName;
            require(switching.library.selected()->name == profileName && switching.profile->name == profileName,
                "Automatic notification arrived before profile selection completed");
        };
        switching.handleDefaultOutput(headphones);
        require(notifications == 1 && notifiedProfile == "Flat response" && notifiedOutput == "Headphones", "Automatic profile notification missing or wrong");
        switching.handleDefaultOutput(headphones); switching.selectPreset(1, true);
        require(notifications == 1, "Unchanged profile emitted another notification");
        switching.selectPreset(0); switching.addFlatPreset();
        require(notifications == 1, "Manual profile changes emitted automatic notifications");
        switching.handleDefaultOutput(newOutput);
        require(notifications == 2 && notifiedOutput == "New output", "Automatically created flat profile did not notify");
        switching.library.followWindows = false; switching.handleDefaultOutput(speakers);
        require(notifications == 2, "Follow Windows off emitted a profile notification");
        switching.onAutomaticProfileChanged = {};
    }
    {
        MainComponent edit(true, folder.getChildFile("gesture/logs")); edit.stopTimer();
        edit.addEqBand(1000, 0); edit.rebuild();
        const auto graph = edit.graphBounds();
        const auto start = juce::Point<float>{graph.getX() + static_cast<float>(std::log(1000.0 / 20) / std::log(1100.0)) * graph.getWidth(), graph.getCentreY()};
        const auto end = juce::Point<float>{start.x, start.y - graph.getHeight() / 8};
        const auto event = [&](juce::Point<float> p, bool dragged) {
            return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, juce::ModifierKeys::leftButtonModifier,
                1, 0, 0, 0, 0, &edit, &edit, juce::Time::getCurrentTime(), start, juce::Time::getCurrentTime(), 1, dragged);
        };
        edit.mouseDown(event(start, false)); edit.mouseDrag(event(end, true));
        edit.rebuildAt = 0; edit.timerCallback(); // Apply while the mouse is held.
        require(edit.gestureInProgress && !edit.dirty && std::abs(edit.customBands[0].gain - 6) < .001, "Graph gesture fixture failed");
        edit.mouseUp(event(end, true)); edit.undoEq(false);
        require(edit.customBands.size() == 1 && std::abs(edit.customBands[0].gain) < .001, "Paused graph drag was not recorded as one undo step");
        edit.undoEq(true);
        require(edit.customBands.size() == 1 && std::abs(edit.customBands[0].gain - 6) < .001, "Graph drag redo lost the completed edit");
        edit.mouseDown(event(end, false)); edit.mouseDrag(event(start, true)); edit.mouseUp(event(start, true)); edit.timerCallback(); edit.undoEq(false);
        require(edit.customBands.size() == 1 && std::abs(edit.customBands[0].gain - 6) < .001, "Quick graph drag undo failed");
    }
    const auto recoveryFolder = folder.getChildFile("recovery-" + juce::Uuid().toString()); recoveryFolder.createDirectory();
    const auto settingsFile = recoveryFolder.getChildFile("Soundee.settings");
    const auto archive = recoveryFolder.getChildFile("flat.soundee");
    {
        MainComponent recovery(true, recoveryFolder.getChildFile("logs")); recovery.stopTimer();
        recovery.preferences = std::make_unique<juce::PropertiesFile>(settingsFile, recovery.propertyOptions);
        recovery.preferences->setValue("listeningProfiles", "{}"); recovery.preferences->saveIfNeeded();
        recovery.library = {}; recovery.profile.reset(); recovery.initialiseProfiles();
        require(recovery.libraryWritable, "Damaged profile library remained read-only after recovery");
        const auto backups = recoveryFolder.getChildFile("profile-recovery").findChildFiles(juce::File::findFiles, false, "*.json");
        require(backups.size() == 1 && backups[0].loadFileAsString() == "{}", "Damaged original profile data was not preserved exactly");
        require(ProfileArchive::write(recovery.library, archive).wasOk() && recovery.importBackup(archive).wasOk(), "Backup recovery import failed");
        recovery.addFlatPreset(); recovery.duplicateProfile(); recovery.addEqBand(900, -2); recovery.rebuild(); recovery.saveSettings();
        require(recovery.getNumRows() == 4, "Profile additions after recovery failed");
    }
    {
        MainComponent reopened(true, recoveryFolder.getChildFile("restart-logs")); reopened.stopTimer();
        reopened.preferences = std::make_unique<juce::PropertiesFile>(settingsFile, reopened.propertyOptions);
        reopened.library = {}; reopened.profile.reset(); reopened.initialiseProfiles();
        require(reopened.getNumRows() == 4 && reopened.customBands.size() == 1 && reopened.customBands[0].gain == -2,
            "Recovered imports and EQ settings disappeared across sessions");
    }
    {
        const auto blockedFolder = folder.getChildFile("blocked-" + juce::Uuid().toString()); blockedFolder.createDirectory();
        require(blockedFolder.getChildFile("profile-recovery").replaceWithText("not a directory"), "Cannot create blocked recovery fixture");
        MainComponent blocked(true, blockedFolder.getChildFile("logs")); blocked.stopTimer();
        blocked.preferences = std::make_unique<juce::PropertiesFile>(blockedFolder.getChildFile("Soundee.settings"), blocked.propertyOptions);
        blocked.preferences->setValue("listeningProfiles", "{}"); blocked.library = {}; blocked.profile.reset(); blocked.initialiseProfiles();
        require(!blocked.libraryWritable && !blocked.headroomOffset.isEnabled(), "Failed recovery allowed unsaved EQ edits");
        require(blocked.importBackup(archive).failed(), "Import claimed success without a writable profile library");
        const auto count = blocked.getNumRows(); blocked.addFlatPreset(); blocked.duplicateProfile();
        require(blocked.getNumRows() == count && blocked.preferences->getValue("listeningProfiles") == "{}", "Failed recovery discarded original data or accepted unsaved additions");
    }
    {
        MainComponent activation(true, folder.getChildFile("activation/logs")); activation.stopTimer();
        OutputEndpoint speakers, headphones; speakers.guid = "{11111111-2222-3333-4444-555555555555}"; speakers.name = "Speakers";
        headphones.guid = "{66666666-7777-8888-9999-AAAAAAAAAAAA}"; headphones.name = "Headphones";
        activation.library.selected()->outputGuid = speakers.guid;
        activation.lastDefaultGuid = speakers.guid;
        activation.endpointRequest = "review-activation"; activation.endpointStartedAt = juce::Time::getMillisecondCounterHiRes();
        activation.prepareEndpointActivation(speakers);
        require(activation.finishEndpointActivation(0), "Unchanged activation request was rejected");
        activation.prepareEndpointActivation(speakers); activation.addFlatPreset(headphones);
        const auto headphoneId = activation.library.selectedId;
        require(!activation.finishEndpointActivation(0) && activation.library.selectedId == headphoneId
            && activation.library.selected()->outputGuid == headphones.guid, "Stale activation changed the current profile's output");
        activation.selectPreset(0); activation.prepareEndpointActivation(speakers); activation.selectPreset(1); activation.selectPreset(0);
        require(!activation.finishEndpointActivation(10), "Changing profiles then returning accepted a stale activation");
        activation.prepareEndpointActivation(speakers); activation.library.followWindows = false; activation.handleDefaultOutput(headphones);
        require(!activation.finishEndpointActivation(0), "Windows output change accepted an obsolete activation request");
        activation.prepareEndpointActivation(speakers); activation.library.selected()->outputGuid = headphones.guid;
        require(!activation.finishEndpointActivation(0), "Relinking a profile accepted an obsolete activation request");
        activation.prepareEndpointActivation(speakers);
        require(!activation.finishEndpointActivation(7) && activation.pendingProfileId.isEmpty(), "Failed attachment retained a pending activation request");
        activation.systemDirectory = folder;
        activation.systemOutput = speakers; activation.systemOutput.apoInstalled = activation.systemOutput.postMixInstalled = true;
        activation.systemState.configured = true; activation.systemState.stage = ApoStage::postMix;
        activation.systemState.bypassed = true; activation.systemState.limiterEnabled = false;
        activation.limiterButton.setToggleState(true, juce::dontSendNotification);
        require(activation.systemEqDescription().contains("Limiter off"), "Bypass described a limiter that is not configured");
        activation.systemState.bypassed = false; activation.systemState.limiterEnabled = true;
        require(activation.systemEqDescription().contains("Output limiter"), "Active limiter status was lost");
    }
    require(folder.getChildFile("result.json").replaceWithText(juce::JSON::toString(fields({{"passed", true},
        {"paused_graph_drag_undo_redo", true}, {"profile_recovery_survives_restart", true}, {"failed_recovery_blocks_mutations", true},
        {"stale_endpoint_activation_rejected", true}, {"limiter_status_matches_configuration", true},
        {"automatic_profile_notifications", true}}))), "Cannot save review regression results");
}
}

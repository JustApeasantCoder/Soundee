#pragma once
#include "Filters.h"
#include "Diagnostics.h"
#include "SystemEq.h"
#include "ListeningProfiles.h"
#include "MeterPanel.h"
#include "DesktopIntegration.h"
#include "EqHistory.h"
#include "NativeBridge.h"
#include "UiStyle.h"
#include <functional>
#include <map>

namespace soundee {
class MainComponent : public juce::Component, public juce::FileDragAndDropTarget,
                      private juce::Timer, private juce::ListBoxModel {
public:
    explicit MainComponent(bool headless = false, juce::File diagnosticDirectory = {});
    ~MainComponent() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray&, int, int) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool loadProfile(const juce::File&);
    juce::var runUiChecks(const juce::File&, const juce::File&);
    juce::var liveStatus() const;
    juce::var runBackendVerificationChecks(const juce::File&, const juce::File&);
    bool keyPressed(const juce::KeyPress&) override;
    bool closesToTray() const { return closeToTray; }
    void setCloseToTray(bool enabled);
    std::function<void(const juce::String& profileName, const juce::String& outputName)> onAutomaticProfileChanged;
    void toggleBypass();
    juce::String trayDescription() const;
    void recordDesktopEvent(const juce::String&, const juce::String&);
private:
    void timerCallback() override;
    void chooseProfile();
    void exportFile(bool impulse);
    void showSystemEq();
    void enableSystemEq(const OutputEndpoint&);
    void disableSystemEq();
    void updateSystemEq();
    void refreshSystemEq();
    void setupBackend();
    juce::String systemEqDescription() const;
    void rebuild();
    Settings settings() const;
    void showError(const juce::String&);
    void saveSettings();
    juce::Rectangle<float> graphBounds() const;
    int getNumRows() override;
    void paintListBoxItem(int, juce::Graphics&, int, int, bool) override;
    juce::String getTooltipForRow(int) override;
    void selectedRowsChanged(int) override;
    void initialiseProfiles();
    juce::Result recoverProfileLibrary();
    bool finishEndpointActivation(int exitCode);
    void prepareEndpointActivation(const OutputEndpoint&);
    void refreshLibraryAccess();
    void runReviewRegressionChecks(const juce::File&);
    void saveSelectedPreset();
    void selectPreset(int, bool automatic = false);
    void addFlatPreset(const OutputEndpoint& = {}, bool automatic = false);
    void rebuildProfileList();
    void handleDefaultOutput(const OutputEndpoint&);
    void syncPresetOutput();
    void addEqBand(double frequency = 1000, double gain = 0);
    void refreshBandControls();
    void bandControlsChanged();
    void moveBand(juce::Point<float>);
    void showMenu();
    void applySettings(const Settings&);
    void undoEq(bool redo);
    void storeSnapshot(int slot);
    void selectSnapshot(int slot);
    void showSnapshotMenu();
    void duplicateProfile();
    void exportProfiles(bool all);
    void chooseBackup();
    juce::Result importBackup(const juce::File&);
    void showComparison();
    void verifyBackend();
    juce::String processingDescription() const;
    void refreshNativeMeter();
    Diagnostics logs;
    std::unique_ptr<Profile> profile;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::PropertiesFile::Options propertyOptions;
    std::unique_ptr<juce::PropertiesFile> preferences;
    juce::TextButton importButton {"Open profile"}, exportButton {"Export Windows EQ"},
        impulseButton {"Export FIR"},
        logsButton {"Diagnostics"}, systemButton {"System-wide EQ"};
    juce::TextButton bypassButton {"EQ enabled"};
    juce::ToggleButton swapButton {"Swap left / right"};
    juce::Slider amount, boost, low, high, headroomOffset;
    juce::Label amountLabel, boostLabel, lowLabel, highLabel, headroomOffsetLabel, statusLabel;
    juce::String status;
    std::atomic<double> rate {48000};
    double rebuildAt = 0;
    bool dirty = false, noDevice = false;
    double designedHeadroom = 0;
    ui::LookAndFeel look;
    juce::File systemDirectory;
    SystemEqState systemState;
    OutputEndpoint systemOutput, pendingOutput;
    juce::String pendingProfileId, pendingProfileOutputGuid, pendingDefaultGuid;
    bool pendingActivationCancelled = false;
    void* endpointProcess = nullptr;
    void* installerProcess = nullptr;
    std::unique_ptr<BackendDownload> backendDownload;
    juce::String backendRequest;
    double backendStartedAt = 0;
    bool endpointNeedsRestart = false;
    double endpointStartedAt = 0, nextSystemRefresh = 0;
    juce::String endpointRequest;
    ListeningProfiles library;
    std::vector<OutputEndpoint> availableOutputs;
    juce::ListBox profileList {"Listening profiles", this};
    juce::ComboBox linkedOutput, bandSelector, bandType;
    juce::ToggleButton followWindows {"Follow Windows output"}, customButton {"Custom EQ"}, bandEnabled {"Band enabled"};
    juce::TextButton flatButton {"+ Flat profile"}, addBandButton {"+ Add band"}, removeBandButton {"Remove"}, menuButton {"More"};
    juce::Slider bandFrequency, bandGain, bandQ;
    juce::Label frequencyLabel, gainLabel, qLabel;
    std::vector<EqBand> customBands;
    int selectedBand = -1, draggedBand = -1;
    bool updatingControls = false, switchingPreset = false, libraryWritable = true, systemEnabled = false;
    juce::String lastDefaultGuid, meterEndpointId;
    std::unique_ptr<SystemAudioMeter> windowsMeter;
    MeterPanel meterPanel;
    juce::TextButton undoButton {"Undo"}, redoButton {"Redo"}, aButton {"A"}, bButton {"B"}, snapshotButton {"Store"};
    juce::ToggleButton limiterButton {"Enable limiter"}, bandCurvesButton {"Show band curves"};
    juce::Slider limiterCeiling;
    juce::Label limiterLabel, healthLabel;
    juce::File calibrationStore, telemetryPath;
    NativeTelemetry nativeMeter;
    NativeReading nativeReading;
    uint64_t nativeClipResetAt = 0;
    std::unique_ptr<EndpointNotifications> notifications;
    std::map<juce::String, EqHistory> histories;
    bool gestureInProgress = false, historyRestoring = false, closeToTray = true;
    bool windowsAudioActive = false, meterWindowsMix = false;
    struct VerificationJob : juce::Thread {
        VerificationJob(Profile, Settings, OutputEndpoint, juce::File, double rate);
        ~VerificationJob() override;
        void run() override;
        Profile profile; Settings controls; OutputEndpoint output; juce::File folder;
        double sampleRate = 48000, maximumError = 0, maximumTargetError = 0;
        juce::Result result = juce::Result::ok();
        juce::String description;
        juce::String requestId = juce::Uuid().toString();
        double startedAt = juce::Time::getMillisecondCounterHiRes();
        juce::ChildProcess child;
    };
    std::unique_ptr<VerificationJob> verification;
    juce::String verificationSummary;
    juce::TooltipWindow tooltips {this, 500};
};
}

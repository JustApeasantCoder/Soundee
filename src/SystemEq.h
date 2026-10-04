#pragma once
#include "Filters.h"

namespace soundee {
enum class ApoStage { none, preMix, postMix };
struct OutputEndpoint {
    juce::String id, guid, name;
    bool apoInstalled = false, effectsDisabled = false;
    bool preMixInstalled = false, postMixInstalled = false;
    bool isDefault = false;
    ApoStage preferredStage() const {
        return postMixInstalled ? ApoStage::postMix : preMixInstalled ? ApoStage::preMix : ApoStage::none;
    }
    bool supports(ApoStage stage) const {
        return (stage == ApoStage::preMix && preMixInstalled) || (stage == ApoStage::postMix && postMixInstalled);
    }
};
struct SystemEqState {
    bool configured = false, bypassed = false;
    juce::String endpointGuid;
    ApoStage stage = ApoStage::none;
    juce::String profileName;
    double sampleRate = 0, preampDb = 0;
    uint64_t processingToken = 0;
    bool limiterEnabled = false, dspPresent = false;
};
class SystemEq {
public:
    static juce::File installDirectory();
    static juce::File configDirectory();
    static std::vector<OutputEndpoint> outputs();
    static bool validGuid(const juce::String&);
    static juce::String stageName(ApoStage);
    static juce::String nativePath(const juce::File&);
    static SystemEqState state(const juce::File&);
    static juce::Result configure(const juce::File&, const OutputEndpoint&,
                                  const Profile&, const Settings&, bool bypassed, double sampleRate = 48000);
    static juce::Result disable(const juce::File&);
    // Pure editing operation, shared by production and preservation checks.
    static juce::Result editRoot(const juce::MemoryBlock&, bool enable, juce::MemoryBlock&,
                                 ApoStage = ApoStage::postMix);
    static juce::var runChecks(const Profile&, const juce::File&);
};
class BackendDownload : public juce::Thread {
public:
    explicit BackendDownload(juce::File target) : juce::Thread("Soundee backend download"), file(std::move(target)) {}
    ~BackendDownload() override { signalThreadShouldExit(); stopThread(35000); }
    static bool verified(const juce::File&);
    void run() override;
    juce::File file;
    bool success = false;
    juce::String error;
    std::atomic<juce::int64> downloaded {0};
};
}

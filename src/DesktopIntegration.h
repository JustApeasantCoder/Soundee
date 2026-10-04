#pragma once
#include <JuceHeader.h>
#include <memory>

namespace soundee {
class EndpointNotifications {
public:
    EndpointNotifications();
    ~EndpointNotifications();
    bool consumeChange();
    bool registered() const;
private:
    struct Native;
    std::unique_ptr<Native> native;
};
class StartupRegistration {
public:
    static bool enabled();
    static juce::Result setEnabled(bool);
    static juce::String commandFor(const juce::File& executable);
    static juce::var runChecks(const juce::File& folder);
};
}

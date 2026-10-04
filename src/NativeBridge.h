#pragma once
#include "Filters.h"
#include "NativeTelemetry.h"

namespace soundee {
class NativeBridge {
public:
    static juce::Result deploy(const juce::File& managedDirectory, juce::File& installed);
    static juce::String filterLine(const juce::File& plugin, const juce::File& telemetry, uint64_t token, const Settings&, bool bypass);
    static juce::File telemetryFile(const juce::File& configDirectory, const juce::String& endpointGuid);
    static uint64_t tokenFor(const juce::String& configuration);
};
}

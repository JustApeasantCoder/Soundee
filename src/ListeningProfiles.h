#pragma once
#include "Filters.h"
#include <optional>

namespace soundee {
struct ListeningPreset {
    juce::String id = juce::Uuid().toString(), name = "Flat response", calibrationPath, outputGuid, outputName;
    Settings settings;
    std::optional<Settings> snapshotA, snapshotB;
    int activeSnapshot = -1;
    juce::int64 lastSelected = 0;
};
class ListeningProfiles {
public:
    std::vector<ListeningPreset> presets;
    juce::String selectedId;
    bool followWindows = true;
    ListeningPreset* selected();
    const ListeningPreset* selected() const;
    int forOutput(const juce::String&) const;
    int select(int index);
    juce::String serialize() const;
    juce::Result restore(const juce::String&);
};
juce::var settingsToJson(const Settings&);
juce::Result settingsFromJson(const juce::var&, Settings&);
}

#pragma once
#include "ListeningProfiles.h"

namespace soundee {
class ProfileArchive {
public:
    static juce::Result retainCalibration(const juce::File& source, const juce::File& store, juce::File& retained);
    static juce::Result write(const ListeningProfiles&, const juce::File& archive);
    static juce::Result read(const juce::File& archive, const juce::File& store, ListeningProfiles&);
};
}

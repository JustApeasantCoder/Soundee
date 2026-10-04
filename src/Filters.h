#pragma once
#include "Profile.h"
#include "ParametricEq.h"

namespace soundee {
struct Settings {
    double amount = 1.0, maximumBoost = 6.0, low = 40.0, high = 16000.0;
    bool swap = false;
    std::vector<EqBand> bands;
    bool customEnabled = true;
    double headroomOffset = 0.0;
    bool limiterEnabled = false;
    double limiterCeiling = -1.0;
};
struct DesignedFilters {
    juce::AudioBuffer<float> impulse;
    double sampleRate = 48000.0;
    double preampDb = 0.0;
};
double correctionDb(const Profile&, int channel, double frequency, const Settings&);
double safeHeadroomDb(const Profile&, const Settings&, double sampleRate = 48000);
double combinedPeakDb(const Profile&, const Settings&, double sampleRate = 48000);
double appliedHeadroomDb(const Profile&, const Settings&, double sampleRate = 48000);
DesignedFilters designFilters(const Profile&, const Settings&, double sampleRate, int order = 14);
juce::AudioBuffer<float> nativeCalibrationImpulse(const Profile&, const Settings&, double sampleRate);
juce::Result exportApo(const Profile&, const Settings&, const juce::File&, double sampleRate = 48000);
juce::String apoConfiguration(const Profile&, const Settings&, bool bypass = false, double sampleRate = 48000);
juce::Result exportImpulse(const DesignedFilters&, const juce::File&);

}

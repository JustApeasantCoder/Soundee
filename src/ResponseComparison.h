#pragma once
#include "Profile.h"
namespace soundee {
struct ComparisonPoint { double frequency = 0, left = 0, right = 0; };
struct ComparisonReport {
    std::vector<ComparisonPoint> points;
    double levelOffset = 0, rmsDifference = 0, maximumDifference = 0;
    juce::String description;
    juce::Result writeCsv(const juce::File&) const;
};
class ResponseComparison {
public:
    static juce::Result analyze(const juce::File& reference, const juce::File& soundee, ComparisonReport&);
    static juce::Result createTestSignal(const juce::File&, double rate = 48000);
    static juce::var runChecks(const juce::File& folder);
};
class ComparisonPanel : public juce::Component, private juce::Timer {
public:
    ComparisonPanel();
    ~ComparisonPanel() override;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    void choose(bool reference);
    void analyze();
    void timerCallback() override;
    struct Job;
    std::unique_ptr<Job> job;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File referenceFile, soundeeFile;
    ComparisonReport report;
    juce::Label instructions, result;
    juce::TextButton referenceButton {"1. SoundID recording"}, soundeeButton {"2. Soundee recording"},
        generateButton {"Create test WAV"}, exportButton {"Export comparison CSV"};
    juce::LookAndFeel_V4 look;
};
}

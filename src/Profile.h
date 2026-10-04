#pragma once
#include <JuceHeader.h>
#include <array>
#include <vector>

namespace soundee {
struct Point { double frequency{}, db{}, extra{}; };
struct Curve { int id{}; double scalar1{}, scalar2{}; std::vector<Point> points; };
struct Profile {
    juce::String name;
    juce::File source;
    std::array<Curve, 4> curves;
    bool flatResponse = false;
    static Profile load(const juce::File&);
    static Profile flat(const juce::String& name = "Flat response");
};
double interpolate(const Curve&, double frequency);
}

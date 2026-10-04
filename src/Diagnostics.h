#pragma once
#include <JuceHeader.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace soundee {
class Diagnostics {
public:
    explicit Diagnostics(juce::File directory = {});
    ~Diagnostics();
    // Control/worker threads only; never call from the audio callback.
    void event(juce::String subsystem, juce::String event, juce::String message,
        juce::String status = "success", juce::var fields = {},
        juce::String request = {}, double duration = -1,
        juce::String errorKind = {});
    juce::File directory;
    juce::String session = juce::Uuid().toString();
private:
    juce::String makeRecord(juce::String, juce::String, juce::String, juce::String,
        juce::var, juce::String, double, juce::String);
    void run();
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<juce::String> queue;
    size_t dropped = 0;
    bool closing = false;
    std::thread worker;
};
juce::var fields(std::initializer_list<std::pair<juce::String, juce::var>> values);
}

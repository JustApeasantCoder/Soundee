#include "Diagnostics.h"

namespace soundee {
juce::var fields(std::initializer_list<std::pair<juce::String, juce::var>> values) {
    auto* object = new juce::DynamicObject;
    for (const auto& [key, value] : values) object->setProperty(key, value);
    return juce::var(object);
}
Diagnostics::Diagnostics(juce::File folder) {
    directory = folder == juce::File() ? juce::File(juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA",
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getFullPathName()))
        .getChildFile("Soundee/logs") : folder;
    directory.createDirectory();
    worker = std::thread([this] { run(); });
    event("app", "app.started", "[App] Soundee started", "success", fields({{"version", JUCE_APPLICATION_VERSION_STRING}}));
}
Diagnostics::~Diagnostics() {
    event("app", "app.stopped", "[App] Soundee stopped");
    { std::lock_guard lock(mutex); closing = true; }
    wake.notify_one(); worker.join();
}
juce::String Diagnostics::makeRecord(juce::String subsystem, juce::String name,
    juce::String message, juce::String status, juce::var values, juce::String request,
    double duration, juce::String errorKind) {
    auto* object = new juce::DynamicObject;
    object->setProperty("schema_version", 1);
    object->setProperty("timestamp", juce::Time::currentTimeMillis());
    object->setProperty("level", status == "failed" ? "error" : (status == "degraded" ? "warn" : "info"));
    object->setProperty("source", "desktop"); object->setProperty("subsystem", subsystem);
    object->setProperty("event", name); object->setProperty("message", message);
    object->setProperty("app_session_id", session); object->setProperty("status", status);
    if (request.isNotEmpty()) object->setProperty("request_id", request);
    if (duration >= 0) object->setProperty("duration_ms", duration);
    if (errorKind.isNotEmpty()) object->setProperty("error_kind", errorKind);
    if (values.isObject()) object->setProperty("fields", values);
    return juce::JSON::toString(juce::var(object), true) + "\n";
}
void Diagnostics::event(juce::String subsystem, juce::String name, juce::String message,
    juce::String status, juce::var values, juce::String request, double duration, juce::String errorKind) {
    auto record = makeRecord(subsystem, name, message, status, values, request, duration, errorKind);
    { std::lock_guard lock(mutex); if (queue.size() < 2048) queue.push_back(std::move(record)); else ++dropped; }
    wake.notify_one();
}
void Diagnostics::run() {
    auto file = directory.getChildFile("Soundee.jsonl");
    std::unique_ptr<juce::FileOutputStream> stream;
    for (;;) {
        std::deque<juce::String> batch; size_t lost = 0;
        { std::unique_lock lock(mutex); wake.wait(lock, [&] { return closing || !queue.empty() || dropped; });
          batch.swap(queue); lost = std::exchange(dropped, 0);
          if (closing && batch.empty() && lost == 0) break; }
        if (lost) batch.push_back(makeRecord("logger", "logger.events_dropped", "[Logger] Event queue was full",
            "degraded", fields({{"count", static_cast<juce::int64>(lost)}}), {}, -1, {}));
        for (const auto& record : batch) {
            if (file.getSize() + record.getNumBytesAsUTF8() > 50 * 1024 * 1024) {
                stream.reset(); auto previous = directory.getChildFile("Soundee.previous.jsonl");
                if (!previous.existsAsFile() || previous.deleteFile()) file.moveFileTo(previous);
            }
            if (!stream) { stream = file.createOutputStream(); if (stream) stream->setPosition(file.getSize()); }
            if (stream && !stream->writeText(record, false, false, nullptr)) stream.reset();
        }
        if (stream) stream->flush();
    }
}
}

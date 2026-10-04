#include "ListeningProfiles.h"
#include "Diagnostics.h"
#include <cmath>
#include <set>

namespace soundee {
namespace {
bool number(const juce::var& v, double low, double high) {
    return (v.isDouble() || v.isInt() || v.isInt64()) && std::isfinite(static_cast<double>(v))
        && static_cast<double>(v) >= low && static_cast<double>(v) <= high;
}
}
juce::var settingsToJson(const Settings& s) {
    juce::Array<juce::var> bands;
    for (const auto& b : s.bands) bands.add(fields({{"type", static_cast<int>(b.type)}, {"frequency", b.frequency},
        {"gain", b.gain}, {"q", b.q}, {"enabled", b.enabled}}));
    return fields({{"amount", s.amount}, {"maximum_boost", s.maximumBoost}, {"low", s.low}, {"high", s.high},
        {"swap", s.swap}, {"custom_enabled", s.customEnabled}, {"bands", bands}, {"headroom_offset", s.headroomOffset},
        {"limiter_enabled", s.limiterEnabled}, {"limiter_ceiling", s.limiterCeiling}});
}
juce::Result settingsFromJson(const juce::var& s, Settings& target) {
    if (!s.isObject() || !number(s["amount"], 0, 1) || !number(s["maximum_boost"], 0, 12)
        || !number(s["low"], 20, 200) || !number(s["high"], 2000, 20000) || !s["bands"].isArray()
        || s["bands"].getArray()->size() > 12 || !s["swap"].isBool() || !s["custom_enabled"].isBool())
        return juce::Result::fail("Invalid listening-profile EQ controls.");
    Settings restored;
    restored.amount = s["amount"]; restored.maximumBoost = s["maximum_boost"]; restored.low = s["low"]; restored.high = s["high"];
    restored.swap = s["swap"]; restored.customEnabled = s["custom_enabled"];
    if (s.hasProperty("headroom_offset")) {
        if (!number(s["headroom_offset"], -24, 24)) return juce::Result::fail("Invalid headroom offset.");
        restored.headroomOffset = s["headroom_offset"];
    }
    if (s.hasProperty("limiter_enabled")) {
        if (!s["limiter_enabled"].isBool()) return juce::Result::fail("Invalid limiter setting.");
        restored.limiterEnabled = s["limiter_enabled"];
    }
    if (s.hasProperty("limiter_ceiling")) {
        if (!number(s["limiter_ceiling"], -12, 0)) return juce::Result::fail("Invalid limiter ceiling.");
        restored.limiterCeiling = s["limiter_ceiling"];
    }
    for (const auto& v : *s["bands"].getArray()) {
        if (!v.isObject() || !number(v["type"], 0, 2) || !number(v["frequency"], 20, 20000)
            || static_cast<double>(v["type"]) != static_cast<int>(v["type"])
            || !number(v["gain"], -18, 18) || !number(v["q"], .2, 12) || !v["enabled"].isBool())
            return juce::Result::fail("Invalid custom EQ band.");
        restored.bands.push_back({static_cast<BandType>(static_cast<int>(v["type"])),
            static_cast<double>(v["frequency"]), static_cast<double>(v["gain"]), static_cast<double>(v["q"]), static_cast<bool>(v["enabled"])});
    }
    target = std::move(restored); return juce::Result::ok();
}
ListeningPreset* ListeningProfiles::selected() {
    for (auto& item : presets) if (item.id == selectedId) return &item; return nullptr;
}
const ListeningPreset* ListeningProfiles::selected() const {
    for (const auto& item : presets) if (item.id == selectedId) return &item; return nullptr;
}
int ListeningProfiles::forOutput(const juce::String& guid) const {
    if (guid.isEmpty()) return -1;
    int index = -1; juce::int64 latest = -1;
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
        const auto& item = presets[static_cast<size_t>(i)];
        if (item.outputGuid.equalsIgnoreCase(guid) && item.lastSelected >= latest) { latest = item.lastSelected; index = i; }
    }
    return index;
}
int ListeningProfiles::select(int index) {
    if (index < 0 || index >= static_cast<int>(presets.size())) return -1;
    auto& item = presets[static_cast<size_t>(index)]; selectedId = item.id;
    item.lastSelected = juce::Time::currentTimeMillis(); return index;
}
juce::String ListeningProfiles::serialize() const {
    juce::Array<juce::var> items;
    for (const auto& item : presets) items.add(fields({{"id", item.id}, {"name", item.name}, {"calibration_path", item.calibrationPath},
        {"output_guid", item.outputGuid}, {"output_name", item.outputName}, {"last_selected", item.lastSelected},
        {"settings", settingsToJson(item.settings)}, {"snapshot_a", item.snapshotA ? settingsToJson(*item.snapshotA) : juce::var()},
        {"snapshot_b", item.snapshotB ? settingsToJson(*item.snapshotB) : juce::var()}, {"active_snapshot", item.activeSnapshot}}));
    return juce::JSON::toString(fields({{"version", 1}, {"selected_id", selectedId}, {"follow_windows", followWindows}, {"presets", items}}));
}
juce::Result ListeningProfiles::restore(const juce::String& text) {
    juce::var root; const auto parsed = juce::JSON::parse(text, root);
    if (parsed.failed() || !root.isObject() || !root["version"].isInt() || static_cast<int>(root["version"]) != 1
        || !root["presets"].isArray() || !root["selected_id"].isString() || !root["follow_windows"].isBool())
        return juce::Result::fail("Unsupported or damaged profile library. The saved library was preserved.");
    ListeningProfiles restored; restored.selectedId = root["selected_id"].toString(); restored.followWindows = root["follow_windows"];
    std::set<juce::String> ids;
    for (const auto& value : *root["presets"].getArray()) {
        if (!value.isObject() || restored.presets.size() >= 128 || !value["id"].isString() || !value["name"].isString()
            || !value["calibration_path"].isString() || !value["output_guid"].isString() || !value["output_name"].isString()
            || !number(value["last_selected"], 0, 9000000000000000.0)) return juce::Result::fail("Invalid profile library.");
        ListeningPreset item; item.id = value["id"].toString(); item.name = value["name"].toString();
        item.calibrationPath = value["calibration_path"].toString(); item.outputGuid = value["output_guid"].toString();
        item.outputName = value["output_name"].toString(); item.lastSelected = static_cast<juce::int64>(value["last_selected"]);
        if (item.id.isEmpty() || item.id.length() > 128 || item.name.trim().isEmpty() || item.name.length() > 256 || !ids.insert(item.id).second
            || (item.calibrationPath.isNotEmpty() && !juce::File::isAbsolutePath(item.calibrationPath))) return juce::Result::fail("Invalid profile identity or path.");
        const auto valid = settingsFromJson(value["settings"], item.settings); if (valid.failed()) return valid;
        for (auto slot : {"snapshot_a", "snapshot_b"}) if (value.hasProperty(slot) && !value[slot].isVoid()) {
            Settings snapshot; const auto result = settingsFromJson(value[slot], snapshot); if (result.failed()) return result;
            if (juce::String(slot) == "snapshot_a") item.snapshotA = snapshot; else item.snapshotB = snapshot;
        }
        if (value.hasProperty("active_snapshot")) {
            if (!value["active_snapshot"].isInt() || !number(value["active_snapshot"], -1, 1)) return juce::Result::fail("Invalid A/B selection.");
            item.activeSnapshot = value["active_snapshot"];
        }
        restored.presets.push_back(std::move(item));
    }
    if (!restored.selected()) return juce::Result::fail("Selected profile is missing.");
    *this = std::move(restored); return juce::Result::ok();
}
}

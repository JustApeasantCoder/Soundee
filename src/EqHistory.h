#pragma once
#include "ListeningProfiles.h"

namespace soundee {
class EqHistory {
public:
    void commit(const Settings& s) {
        const auto key = encode(s);
        if (!states.empty() && encode(states[position]) == key) return;
        if (!states.empty()) states.resize(position + 1);
        states.push_back(s); if (states.size() > 101) states.erase(states.begin()); position = states.size() - 1;
    }
    const Settings* undo() { return canUndo() ? &states[--position] : nullptr; }
    const Settings* redo() { return canRedo() ? &states[++position] : nullptr; }
    bool canUndo() const { return !states.empty() && position > 0; }
    bool canRedo() const { return !states.empty() && position + 1 < states.size(); }
private:
    static juce::String encode(const Settings& s) {
        ListeningProfiles library; ListeningPreset p; p.id = "settings"; p.settings = s;
        library.presets.push_back(p); library.selectedId = p.id; return library.serialize();
    }
    std::vector<Settings> states;
    size_t position = 0;
};
}

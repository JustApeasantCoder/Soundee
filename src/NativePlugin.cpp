#include "NativeDsp.h"
#include "NativeTelemetry.h"
#include <cstring>
#include <sstream>
#include <string>
#include <memory>

#include "NativePluginAbi.h"
namespace {
struct Plugin {
    NativeEffect effect;
    soundee::LookaheadLimiter limiter;
    std::array<soundee::TruePeakDetector, 2> detectors;
    soundee::NativeTelemetry telemetry;
    double rate = 48000, ceiling = -1;
    bool enabled = false;
    uint64_t token = 0, frames = 0;
    std::array<uint64_t, 2> lastClipAt{};
    std::array<float, 2> heldPeak{};
    uint64_t holdStartedAt = 0;
    std::string chunk = "SOUNDEE1\n0\n-1\n\n0\n";
    void prepare() noexcept {
        limiter.prepare(rate, ceiling); effect.delay = enabled ? limiter.latency() : 0;
        for (auto& d : detectors) d.reset(); lastClipAt = {}; heldPeak = {}; holdStartedAt = 0; frames = 0;
    }
    void setChunk(const char* data, size_t size) {
        if (!data || size > 32768) return;
        std::istringstream stream(std::string(data, size)); std::string header, flag, db, path, key;
        std::getline(stream, header); std::getline(stream, flag); std::getline(stream, db); std::getline(stream, path); std::getline(stream, key);
        if (header != "SOUNDEE1" || (flag != "0" && flag != "1")) return;
        const double value = std::stod(db); if (!std::isfinite(value) || value < -12 || value > 0) return;
        const auto fingerprint = std::stoull(key, nullptr, 16);
        enabled = flag == "1"; ceiling = value; token = fingerprint; chunk.assign(data, size);
        telemetry.close();
        if (!path.empty()) {
            const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
            if (count > 0) { std::wstring wide(count, L'\0'); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), wide.data(), count); telemetry.open(wide, true); }
        }
        prepare();
    }
};
Plugin& plugin(NativeEffect* effect) { return *static_cast<Plugin*>(effect->object); }
void text(void* target, const char* value, size_t capacity = 32) noexcept { if (target) { std::strncpy(static_cast<char*>(target), value, capacity - 1); static_cast<char*>(target)[capacity - 1] = 0; } }
intptr_t __cdecl dispatchEffect(NativeEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* pointer, float option) {
    try {
        auto& p = plugin(effect);
        switch (opcode) {
        case 0: return 1;
        case 1: delete &p; return 1;
        case 6: text(pointer, index == 0 ? "" : "dBFS", 8); return 1;
        case 7: { const auto valueText = std::to_string(index == 0 ? p.enabled : p.ceiling); text(pointer, valueText.c_str(), 8); return 1; }
        case 8: text(pointer, index == 0 ? "Enabled" : "Ceiling", 8); return 1;
        case 10: if (std::isfinite(option) && option >= 8000 && option <= 384000) { p.rate = option; p.prepare(); } return 1;
        case 11: return 1;
        case 12: if (value) p.prepare(); return 1;
        case 23: if (pointer) *static_cast<void**>(pointer) = p.chunk.data(); return static_cast<intptr_t>(p.chunk.size());
        case 24: p.setChunk(static_cast<const char*>(pointer), static_cast<size_t>(value)); return 1;
        case 35: return 1;
        case 45: text(pointer, "Soundee DSP"); return 1;
        case 47: text(pointer, "Soundee"); return 1;
        case 48: text(pointer, "Soundee EQ Monitor and Limiter"); return 1;
        case 49: return 100;
        case 51: return 0;
        case 58: return 2400;
        case 71: p.prepare(); return 1;
        default: return 0;
        }
    } catch (...) { return 0; }
}
void __cdecl setParameter(NativeEffect* effect, int32_t index, float value) {
    if (!std::isfinite(value)) return;
    auto& p = plugin(effect);
    if (index == 0) p.enabled = value >= .5f;
    if (index == 1) p.ceiling = std::clamp(static_cast<double>(value), 0., 1.) * 12 - 12;
    p.prepare();
}
float __cdecl getParameter(NativeEffect* effect, int32_t index) {
    auto& p = plugin(effect); return index == 0 ? (p.enabled ? 1.f : 0.f) : static_cast<float>((p.ceiling + 12) / 12);
}
void __cdecl processEffect(NativeEffect* effect, float** inputs, float** outputs, int32_t count) {
    if (!inputs || !outputs || count <= 0) return;
    auto& p = plugin(effect); std::array<float, 2> peak{}, rms{}; std::array<double, 2> energy{};
    for (int32_t i = 0; i < count; ++i) {
        std::array<float, 2> value {inputs[0] ? inputs[0][i] : 0, inputs[1] ? inputs[1][i] : 0};
        for (auto& v : value) if (!std::isfinite(v)) v = 0;
        if (p.enabled) value = p.limiter.process(value[0], value[1]);
        for (size_t c = 0; c < 2; ++c) {
            if (outputs[c]) outputs[c][i] = value[c];
            peak[c] = std::max(peak[c], static_cast<float>(p.detectors[c].push(value[c]))); energy[c] += value[c] * value[c];
        }
    }
    for (size_t c = 0; c < 2; ++c) rms[c] = static_cast<float>(std::sqrt(energy[c] / count));
    const auto now = GetTickCount64();
    if (now - p.holdStartedAt > 100) { p.heldPeak = {}; p.holdStartedAt = now; }
    for (size_t c = 0; c < 2; ++c) { p.heldPeak[c] = std::max(p.heldPeak[c], peak[c]); if (peak[c] >= 1) p.lastClipAt[c] = now; }
    p.frames += count;
    p.telemetry.publish(p.token, p.frames, p.rate, p.heldPeak, rms, p.lastClipAt,
        p.enabled ? static_cast<float>(p.limiter.reductionDb()) : 0, p.effect.delay);
}
}
extern "C" __declspec(dllexport) NativeEffect* __cdecl VSTPluginMain(NativeHost) {
    try {
        auto p = std::make_unique<Plugin>(); auto* result = &p->effect;
        result->object = p.get(); result->dispatcher = dispatchEffect; result->process = result->processReplacing = processEffect;
        result->setParameter = setParameter; result->getParameter = getParameter;
        p->prepare(); p.release(); return result;
    } catch (...) { return nullptr; }
}

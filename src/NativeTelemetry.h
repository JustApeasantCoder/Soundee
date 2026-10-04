#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <bit>
#include <cstdint>
#include <string>
#include <algorithm>

namespace soundee {
constexpr LONG telemetryMagic = 0x53454532;
struct alignas(8) TelemetrySlot {
    volatile LONG owner = 0, sequence = 0;
    uint64_t updated = 0, token = 0, frames = 0;
    uint64_t lastClipAt[2]{};
    double sampleRate = 0;
    float peaks[2]{}, rms[2]{}, reductionDb = 0;
    uint32_t clips = 0, latency = 0, pid = 0;
};
struct TelemetryPage { LONG magic = 0; uint32_t version = 2; TelemetrySlot slots[64]{}; };
struct NativeReading {
    bool connected = false;
    std::array<float, 2> peak{}, rms{};
    std::array<bool, 2> clipped{};
    double sampleRate = 0;
    float reductionDb = 0;
    uint32_t latency = 0, instances = 0;
    uint64_t frames = 0;
    std::array<uint64_t, 2> lastClipAt{};
};
// File-backed shared pages cross the audio service/user session boundary.
// File handles and mappings are opened outside processing; the audio callback
// only updates an already mapped slot. The UI never creates a telemetry file.
class NativeTelemetry {
public:
    NativeTelemetry() = default;
    ~NativeTelemetry() { close(); }
    NativeTelemetry(const NativeTelemetry&) = delete;
    NativeTelemetry& operator=(const NativeTelemetry&) = delete;
    bool isOpen() const noexcept { return page != nullptr; }
    bool open(const std::wstring& path, bool writer) noexcept {
        close(); writing = writer;
        file = CreateFileW(path.c_str(), writer ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, writer ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size)) { close(); return false; }
        if (size.QuadPart != sizeof(TelemetryPage)) {
            if (!writer || size.QuadPart != 0) { close(); return false; }
            size.QuadPart = sizeof(TelemetryPage);
            if (!SetFilePointerEx(file, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) { close(); return false; }
        }
        mapping = CreateFileMappingW(file, nullptr, writer ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) { close(); return false; }
        page = static_cast<TelemetryPage*>(MapViewOfFile(mapping, writer ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, sizeof(TelemetryPage)));
        if (!page) { close(); return false; }
        if (writer && page->magic == 0) { page->version = 2; InterlockedExchange(&page->magic, telemetryMagic); }
        if (page->magic != telemetryMagic || page->version != 2) { close(); return false; }
        owner = static_cast<LONG>((GetCurrentProcessId() * 2654435761u) ^ GetTickCount() ^ reinterpret_cast<uintptr_t>(this));
        if (!owner) owner = 1;
        return true;
    }
    void close() noexcept {
        if (slot && slot->owner == owner) InterlockedCompareExchange(&slot->owner, 0, owner);
        slot = nullptr;
        if (page) UnmapViewOfFile(page); page = nullptr;
        if (mapping) CloseHandle(mapping); mapping = nullptr;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file); file = INVALID_HANDLE_VALUE;
    }
    void publish(uint64_t token, uint64_t frames, double rate, const std::array<float, 2>& peaks,
        const std::array<float, 2>& rms, const std::array<uint64_t, 2>& lastClipAt, float reduction, uint32_t latency) noexcept {
        if (!page || !writing) return;
        const auto now = GetTickCount64();
        if (!slot || slot->owner != owner) {
            slot = nullptr;
            for (auto& candidate : page->slots) {
                const LONG existing = candidate.owner;
                if ((existing == 0 || now - candidate.updated > 5000)
                    && InterlockedCompareExchange(&candidate.owner, owner, existing) == existing) {
                    slot = &candidate;
                    // A terminated audio process may have left an odd sequence.
                    InterlockedExchange(&slot->sequence, 0); break;
                }
            }
        }
        if (!slot) return;
        InterlockedIncrement(&slot->sequence);
        slot->updated = now; slot->token = token; slot->frames = frames; slot->sampleRate = rate;
        for (size_t c = 0; c < 2; ++c) { slot->peaks[c] = peaks[c]; slot->rms[c] = rms[c]; slot->lastClipAt[c] = lastClipAt[c]; }
        slot->reductionDb = reduction; slot->latency = latency; slot->pid = GetCurrentProcessId();
        InterlockedIncrement(&slot->sequence);
    }
    NativeReading read(uint64_t token) const noexcept {
        NativeReading result;
        if (!page) return result;
        const auto now = GetTickCount64();
        for (const auto& source : page->slots) {
            const LONG before = source.sequence, sourceOwner = source.owner; MemoryBarrier();
            if (!sourceOwner || (before & 1)) continue;
            const uint64_t updated = source.updated, fingerprint = source.token, frames = source.frames;
            const double rate = source.sampleRate;
            const std::array<float, 2> peak {source.peaks[0], source.peaks[1]}, rms {source.rms[0], source.rms[1]};
            const std::array<uint64_t, 2> clippedAt {source.lastClipAt[0], source.lastClipAt[1]};
            const auto latency = source.latency; const float reduction = source.reductionDb;
            MemoryBarrier(); if (before != source.sequence || sourceOwner != source.owner || fingerprint != token || now - updated > 1500) continue;
            result.connected = true; ++result.instances; result.frames += frames; result.sampleRate = rate;
            result.latency = std::max(result.latency, latency); result.reductionDb = std::max(result.reductionDb, reduction);
            for (size_t c = 0; c < 2; ++c) {
                result.peak[c] = std::max(result.peak[c], peak[c]); result.rms[c] = std::max(result.rms[c], rms[c]);
                result.lastClipAt[c] = std::max(result.lastClipAt[c], clippedAt[c]);
                result.clipped[c] = result.lastClipAt[c] != 0;
            }
        }
        return result;
    }
private:
    HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr;
    TelemetryPage* page = nullptr;
    TelemetrySlot* slot = nullptr;
    LONG owner = 1;
    bool writing = false;
};
}

#include "AudioMeter.h"
#include "Diagnostics.h"
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>

namespace soundee {
void StereoLevels::publish(const std::array<float, 2>& peak, const std::array<float, 2>& rms) {
    for (size_t c = 0; c < 2; ++c) {
        const float value = std::isfinite(peak[c]) ? std::max(0.f, peak[c]) : 0.f;
        auto previous = peaks[c].load(std::memory_order_relaxed);
        while (value > previous && !peaks[c].compare_exchange_weak(previous, value, std::memory_order_relaxed)) {}
        rmsValues[c].store(std::isfinite(rms[c]) ? std::max(0.f, rms[c]) : 0.f, std::memory_order_relaxed);
        if (value >= 1.f) clips[c].store(true, std::memory_order_relaxed);
    }
    updated.store(juce::Time::getMillisecondCounter(), std::memory_order_release);
}
void StereoLevels::publish(const juce::AudioBuffer<float>& buffer, int samples) {
    std::array<float, 2> peak{}, rms{};
    for (int c = 0; c < std::min(2, buffer.getNumChannels()); ++c) {
        double energy = 0; const auto* values = buffer.getReadPointer(c);
        for (int i = 0; i < std::min(samples, buffer.getNumSamples()); ++i) {
            const float value = std::isfinite(values[i]) ? values[i] : 0;
            peak[c] = std::max(peak[c], static_cast<float>(truePeak[c].push(value))); energy += value * value;
        }
        rms[static_cast<size_t>(c)] = static_cast<float>(std::sqrt(energy / std::max(1, samples)));
    }
    publish(peak, rms);
}
StereoReading StereoLevels::consume() {
    StereoReading result; result.updatedAt = updated.load(std::memory_order_acquire);
    for (size_t c = 0; c < 2; ++c) {
        result.peak[c] = peaks[c].exchange(0, std::memory_order_relaxed);
        result.rms[c] = rmsValues[c].load(std::memory_order_relaxed); result.clipped[c] = clips[c].load(std::memory_order_relaxed);
    }
    return result;
}
void StereoLevels::resetClips() { for (auto& value : clips) value.store(false, std::memory_order_relaxed); }
void StereoLevels::reset() {
    for (auto& detector : truePeak) detector.reset();
    for (auto& value : peaks) value.store(0, std::memory_order_relaxed);
    for (auto& value : rmsValues) value.store(0, std::memory_order_relaxed);
    updated = 0; resetClips();
}
struct SystemAudioMeter::Native {
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Native() { if (wake) CloseHandle(wake); }
};
SystemAudioMeter::SystemAudioMeter(Diagnostics& diagnostics)
    : juce::Thread("Soundee Windows output meter"), native(std::make_unique<Native>()), logs(diagnostics) { startThread(); }
SystemAudioMeter::~SystemAudioMeter() { signalThreadShouldExit(); SetEvent(native->wake); stopThread(5000); }
void SystemAudioMeter::selectEndpoint(const juce::String& id) {
    std::lock_guard lock(commandMutex);
    if (requestedId == id) return;
    requestedId = id; generation.fetch_add(1); SetEvent(native->wake);
}
void SystemAudioMeter::run() {
    using Microsoft::WRL::ComPtr;
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        unsigned seen = 0; juce::String id; HRESULT lastFailure = S_OK;
        ComPtr<IMMDeviceEnumerator> enumerator; ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client; ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* format = nullptr; bool floating = false;
        std::array<TruePeakDetector, 2> detectors;
        auto close = [&] {
            online = false; if (client) client->Stop(); capture.Reset(); client.Reset(); device.Reset();
            if (format) { CoTaskMemFree(format); format = nullptr; } sampleRate = 0; levels.reset();
            for (auto& detector : detectors) detector.reset();
        };
        auto fail = [&](HRESULT hr) {
            close(); if (hr != lastFailure) logs.event("meter", "meter.output.failed", "[Meter] Windows output metering unavailable",
                "degraded", fields({{"hresult", juce::String::toHexString(static_cast<int>(hr))}}), {}, -1, "loopback_unavailable");
            lastFailure = hr;
        };
        while (!threadShouldExit()) {
            const auto current = generation.load();
            if (current != seen) {
                close(); { std::lock_guard lock(commandMutex); id = requestedId; seen = generation.load(); } lastFailure = S_OK;
            }
            if (!client && id.isNotEmpty()) {
                HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.ReleaseAndGetAddressOf()));
                if (SUCCEEDED(hr)) hr = enumerator->GetDevice(id.toWideCharPointer(), device.GetAddressOf());
                if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
                if (SUCCEEDED(hr)) hr = client->GetMixFormat(&format);
                if (SUCCEEDED(hr)) {
                    floating = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
                    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
                        const auto* extended = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format);
                        const GUID floatGuid {3, 0, 0x0010, {0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
                        const GUID pcmGuid {1, 0, 0x0010, {0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
                        floating = extended->SubFormat == floatGuid;
                        if (!floating && extended->SubFormat != pcmGuid) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
                    } else if (!floating && format->wFormatTag != WAVE_FORMAT_PCM) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
                    const auto bits = format->wBitsPerSample;
                    if (format->nChannels == 0 || format->nBlockAlign < format->nChannels * (bits / 8)
                        || (floating && bits != 32 && bits != 64) || (!floating && bits != 16 && bits != 24 && bits != 32)) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
                }
                if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, format, nullptr);
                if (SUCCEEDED(hr)) hr = client->SetEventHandle(native->wake);
                if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(capture.GetAddressOf()));
                if (SUCCEEDED(hr)) hr = client->Start();
                if (FAILED(hr)) { fail(hr); WaitForSingleObject(native->wake, 1000); continue; }
                sampleRate = format->nSamplesPerSec; online = true; lastFailure = S_OK;
                logs.event("meter", "meter.output.started", "[Meter] Windows output meter connected", "success",
                    fields({{"sample_rate", static_cast<int>(format->nSamplesPerSec)}, {"channels", static_cast<int>(format->nChannels)},
                        {"mode", "wasapi_loopback"}}));
            }
            WaitForSingleObject(native->wake, 100);
            if (!capture || threadShouldExit() || generation.load() != seen) continue;
            UINT32 frames = 0; HRESULT hr = capture->GetNextPacketSize(&frames);
            while (SUCCEEDED(hr) && frames > 0) {
                BYTE* data = nullptr; DWORD flags = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr); if (FAILED(hr)) break;
                std::array<float, 2> peaks{}, rms{}; double energy[2]{};
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) for (auto& detector : detectors) detector.reset();
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data) {
                    const unsigned bytes = format->wBitsPerSample / 8;
                    for (UINT32 frame = 0; frame < frames; ++frame) for (size_t c = 0; c < 2; ++c) {
                        const auto* sample = data + frame * format->nBlockAlign + std::min<unsigned>(static_cast<unsigned>(c), format->nChannels - 1) * bytes;
                        float value = 0;
                        if (floating && bytes == 4) std::memcpy(&value, sample, 4);
                        else if (floating && bytes == 8) { double v; std::memcpy(&v, sample, 8); value = static_cast<float>(v); }
                        else if (bytes == 2) { int16_t v; std::memcpy(&v, sample, 2); value = v / 32768.f; }
                        else if (bytes == 3) { const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(sample[0]) | static_cast<uint32_t>(sample[1]) << 8 | static_cast<uint32_t>(sample[2]) << 16) << 8); value = v / 2147483648.f; }
                        else if (bytes == 4) { int32_t v; std::memcpy(&v, sample, 4); value = v / 2147483648.f; }
                        if (!std::isfinite(value)) value = 0;
                        peaks[c] = std::max(peaks[c], static_cast<float>(detectors[c].push(value))); energy[c] += value * value;
                    }
                }
                else for (UINT32 frame = 0; frame < frames; ++frame) for (size_t c = 0; c < 2; ++c)
                    peaks[c] = std::max(peaks[c], static_cast<float>(detectors[c].push(0)));
                for (size_t c = 0; c < 2; ++c) rms[c] = static_cast<float>(std::sqrt(energy[c] / std::max<UINT32>(1, frames)));
                levels.publish(peaks, rms);
                hr = capture->ReleaseBuffer(frames); if (SUCCEEDED(hr)) hr = capture->GetNextPacketSize(&frames);
            }
            if (FAILED(hr)) { fail(hr); WaitForSingleObject(native->wake, 500); }
        }
        close();
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
}
}

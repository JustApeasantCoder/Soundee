// Offline diagnostic only. Calls the installed APO with synthesized memory
// buffers; never opens a render stream, captures audio, or plays a test tone.
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiomediatype.h>
#include <mmreg.h>
#include <audioenginebaseapo.h>
#include <propsys.h>
#include <wrl/client.h>
#include <vector>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <memory>
#include "../../src/NativeTelemetry.h"
using Microsoft::WRL::ComPtr;
void checked(HRESULT hr, const char* operation) {
    if (FAILED(hr)) { std::cerr << operation << " HRESULT=0x" << std::hex << hr << '\n'; throw std::runtime_error(operation); }
}
class MediaType final : public IAudioMediaType {
public:
    explicit MediaType(const UNCOMPRESSEDAUDIOFORMAT& data) : uncompressed(data) {
        wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; wave.Format.nChannels = 2;
        wave.Format.nSamplesPerSec = static_cast<DWORD>(data.fFramesPerSecond); wave.Format.nAvgBytesPerSec = wave.Format.nSamplesPerSec * 8;
        wave.Format.nBlockAlign = 8; wave.Format.wBitsPerSample = 32; wave.Format.cbSize = 22;
        wave.Samples.wValidBitsPerSample = 32; wave.dwChannelMask = 3; wave.SubFormat = data.guidFormatType;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override {
        if (!value) return E_POINTER; *value = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IAudioMediaType)) return E_NOINTERFACE;
        *value = static_cast<IAudioMediaType*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references); }
    ULONG STDMETHODCALLTYPE Release() override { auto count = InterlockedDecrement(&references); if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE IsCompressedFormat(BOOL* value) override { if (!value) return E_POINTER; *value = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE IsEqual(IAudioMediaType* other, DWORD* flags) override {
        if (!other || !flags) return E_POINTER; UNCOMPRESSEDAUDIOFORMAT data{};
        auto hr = other->GetUncompressedAudioFormat(&data); if (FAILED(hr)) return hr;
        const bool equal = data.guidFormatType == uncompressed.guidFormatType
            && data.dwSamplesPerFrame == 2 && data.dwBytesPerSampleContainer == 4
            && data.dwValidBitsPerSample == 32 && data.fFramesPerSecond == uncompressed.fFramesPerSecond && data.dwChannelMask == 3;
        *flags = equal ? 14 : 0; return equal ? S_OK : S_FALSE;
    }
    const WAVEFORMATEX* STDMETHODCALLTYPE GetAudioFormat() override { return &wave.Format; }
    HRESULT STDMETHODCALLTYPE GetUncompressedAudioFormat(UNCOMPRESSEDAUDIOFORMAT* data) override {
        if (!data) return E_POINTER; *data = uncompressed; return S_OK;
    }
private:
    LONG references = 1; UNCOMPRESSEDAUDIOFORMAT uncompressed{}; WAVEFORMATEXTENSIBLE wave{};
};

// Registry redirection is process-local. The system audio engine keeps its
// real HKLM and original APO configuration throughout this offline test.
class RegistrySandbox {
    HKEY root = nullptr;
    std::wstring key;
public:
    explicit RegistrySandbox(const wchar_t* configuration) {
        key = L"Software\\Soundee\\OfflineApoQA\\" + std::to_wstring(GetCurrentProcessId());
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) != ERROR_SUCCESS)
            throw std::runtime_error("Create isolated QA registry");
        HKEY settings = nullptr;
        if (RegCreateKeyExW(root, L"SOFTWARE\\EqualizerAPO", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &settings, nullptr) != ERROR_SUCCESS)
            throw std::runtime_error("Create isolated backend key");
        const auto path = std::filesystem::absolute(configuration).wstring();
        auto result = RegSetValueExW(settings, L"ConfigPath", 0, REG_SZ, reinterpret_cast<const BYTE*>(path.c_str()), static_cast<DWORD>((path.size()+1)*sizeof(wchar_t)));
        RegCloseKey(settings);
        if (result != ERROR_SUCCESS || RegOverridePredefKey(HKEY_LOCAL_MACHINE, root) != ERROR_SUCCESS)
            throw std::runtime_error("Activate process-local QA registry");
    }
    ~RegistrySandbox() {
        RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
        if (root) RegCloseKey(root);
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    }
};
int wmain(int argc, wchar_t** argv) {
    if (argc != 7) return 2;
    try {
        checked(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        std::unique_ptr<RegistrySandbox> registry;
        ComPtr<IMMDeviceEnumerator> enumerator; ComPtr<IMMDevice> device;
        checked(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(enumerator.GetAddressOf())), "Device enumerator");
        checked(enumerator->GetDevice(argv[1], device.GetAddressOf()), "GetDevice");
        ComPtr<IPropertyStore> properties, fx;
        checked(device->OpenPropertyStore(STGM_READ, properties.GetAddressOf()), "Endpoint properties");
        checked(PSCreateMemoryPropertyStore(IID_PPV_ARGS(fx.GetAddressOf())), "Empty FX properties");
        GUID clsid;
        checked(CLSIDFromString(argc >= 6 && std::wstring(argv[5]) == L"post-mix" ? L"{EC1CC9CE-FAED-4822-828A-82A81A6F018F}" : L"{EACD2258-FCAC-4FF4-B36D-419E924A6D79}", &clsid), "APO CLSID");
        ComPtr<IAudioProcessingObject> apo; ComPtr<IAudioProcessingObjectConfiguration> config;
        ComPtr<IAudioProcessingObjectRT> rt;
        checked(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(apo.GetAddressOf())), "Installed Equalizer APO pre-mix");
        registry = std::make_unique<RegistrySandbox>(argv[3]);
        APOInitSystemEffects init{}; init.APOInit.cbSize = sizeof(init); init.APOInit.clsid = clsid;
        init.pAPOEndpointProperties = properties.Get(); init.pAPOSystemEffectsProperties = fx.Get();
        checked(apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)), "APO Initialize");
        checked(apo.As(&config), "Configuration interface"); checked(apo.As(&rt), "Processing interface");
        // Stereo IEEE float with explicit FL/FR channel mask.
        UNCOMPRESSEDAUDIOFORMAT format{};
        checked(CLSIDFromString(L"{00000003-0000-0010-8000-00AA00389B71}", &format.guidFormatType), "Float format GUID");
        format.dwSamplesPerFrame = 2; format.dwBytesPerSampleContainer = 4;
        format.dwValidBitsPerSample = 32; format.fFramesPerSecond = argc >= 5 ? std::stod(argv[4]) : 48000; format.dwChannelMask = 3;
        ComPtr<IAudioMediaType> media;
        media.Attach(new MediaType(format));
        constexpr unsigned frames = 480; const double rate = format.fFramesPerSecond;
        std::vector<float> in(frames * 2), out(frames * 2);
        APO_CONNECTION_DESCRIPTOR input{}, output{};
        input.Type = output.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL;
        input.pBuffer = reinterpret_cast<UINT_PTR>(in.data()); output.pBuffer = reinterpret_cast<UINT_PTR>(out.data());
        input.u32MaxFrameCount = output.u32MaxFrameCount = frames;
        input.pFormat = output.pFormat = media.Get();
        input.u32Signature = output.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;
        auto* inputPointer = &input; auto* outputPointer = &output;
        checked(config->LockForProcess(1, &inputPointer, 1, &outputPointer), "LockForProcess");
        APO_CONNECTION_PROPERTY inputProperty{}, outputProperty{};
        inputProperty.pBuffer = input.pBuffer; outputProperty.pBuffer = output.pBuffer;
        inputProperty.u32Signature = outputProperty.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
        auto* inputPropertyPointer = &inputProperty; auto* outputPropertyPointer = &outputProperty;
        auto processBlock = [&] {
            inputProperty.u32BufferFlags = BUFFER_VALID; inputProperty.u32ValidFrameCount = frames;
            outputProperty.u32BufferFlags = BUFFER_INVALID; outputProperty.u32ValidFrameCount = 0;
            rt->APOProcess(1, &inputPropertyPointer, 1, &outputPropertyPointer);
            if (outputProperty.u32BufferFlags != BUFFER_VALID || outputProperty.u32ValidFrameCount != frames)
                throw std::runtime_error("Invalid APO output");
        };
        // Require actual DSP publication before collecting a response. The
        // report and telemetry live outside the watched configuration directory:
        // otherwise their creation triggers a reload during the first tone.
        soundee::NativeTelemetry telemetry;
        const auto telemetryPath = std::filesystem::absolute(argv[6]).wstring();
        // A low-level memory-only tone exercises a nonsilent processing path.
        for (unsigned i = 0; i < frames; ++i) in[i * 2] = in[i * 2 + 1]
            = static_cast<float>(.001 * std::sin(2 * std::numbers::pi * 1000 * i / rate));
        bool ready = false;
        for (int attempt = 0; attempt < 200; ++attempt) {
            processBlock();
            if ((telemetry.isOpen() || telemetry.open(telemetryPath, false)) && telemetry.read(0).connected) { ready = true; break; }
            Sleep(10);
        }
        if (!ready) throw std::runtime_error("Native DSP did not process the isolated configuration");
        for (int block = 0; block < 100; ++block) processBlock();
        std::ofstream report{std::filesystem::path(argv[2])}; report << "frequency_hz,left_db,right_db\n";
        for (double frequency : {30., 40., 60., 80., 100., 125., 160., 250., 350., 500., 750., 1000., 1500., 2000., 3000., 4000., 6000., 8000., 10000., 12000., 16000.}) {
            double reference = 0, sums[2]{};
            for (unsigned block = 0; block < 200; ++block) {
                for (unsigned i = 0; i < frames; ++i) {
                    const float value = static_cast<float>(0.001 * std::sin(2 * std::numbers::pi * frequency * (block * frames + i) / rate));
                    in[i * 2] = in[i * 2 + 1] = value;
                    if (block >= 100) reference += value * value;
                }
                processBlock();
                if (block >= 100) for (unsigned i = 0; i < frames; ++i)
                    for (unsigned channel = 0; channel < 2; ++channel) sums[channel] += out[i * 2 + channel] * out[i * 2 + channel];
            }
            report << frequency << ',' << 10 * std::log10(sums[0] / reference) << ',' << 10 * std::log10(sums[1] / reference) << '\n';
        }
        checked(config->UnlockForProcess(), "UnlockForProcess");
        return report.good() ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

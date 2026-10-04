#include "DesktopIntegration.h"
#include "Diagnostics.h"
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <cwchar>

namespace soundee {
struct EndpointNotifications::Native final : IMMNotificationClient {
    std::atomic<ULONG> references {1};
    std::atomic<bool> changed {true};
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    bool active = false, comOwned = false;
    Native() {
        const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); comOwned = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(enumerator.GetAddressOf())))) active = SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(this));
    }
    ~Native() {
        if (active) enumerator->UnregisterEndpointNotificationCallback(this);
        enumerator.Reset(); if (comOwned) CoUninitialize();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override {
        if (!value) return E_POINTER; *value = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IMMNotificationClient)) return E_NOINTERFACE;
        *value = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { changed = true; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { changed = true; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { changed = true; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eMultimedia) changed = true; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { changed = true; return S_OK; }
};
EndpointNotifications::EndpointNotifications() : native(std::make_unique<Native>()) {}
EndpointNotifications::~EndpointNotifications() = default;
bool EndpointNotifications::consumeChange() { return native->changed.exchange(false); }
bool EndpointNotifications::registered() const { return native->active; }
namespace {
const wchar_t* startupKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
juce::Result setAt(const wchar_t* key, bool enabled, const juce::String& command) {
    HKEY handle = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS)
        return juce::Result::fail("Unable to update Soundee's Windows startup setting.");
    const auto wide = command.toWideCharPointer();
    const auto result = enabled ? RegSetValueExW(handle, L"Soundee", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(wide), static_cast<DWORD>((std::wcslen(wide) + 1) * sizeof(wchar_t))) : RegDeleteValueW(handle, L"Soundee");
    RegCloseKey(handle);
    return result == ERROR_SUCCESS || (!enabled && result == ERROR_FILE_NOT_FOUND) ? juce::Result::ok()
        : juce::Result::fail("Windows rejected Soundee's startup setting.");
}
}
juce::String StartupRegistration::commandFor(const juce::File& file) { return file.getFullPathName().quoted() + " --background"; }
bool StartupRegistration::enabled() {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, startupKey, 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS) return false;
    wchar_t value[32768]{}; DWORD bytes = sizeof(value), type = 0;
    const auto result = RegQueryValueExW(handle, L"Soundee", nullptr, &type, reinterpret_cast<BYTE*>(value), &bytes);
    RegCloseKey(handle);
    return result == ERROR_SUCCESS && type == REG_SZ && juce::String(value) == commandFor(juce::File::getSpecialLocation(juce::File::currentExecutableFile));
}
juce::Result StartupRegistration::setEnabled(bool enabled) {
    return setAt(startupKey, enabled, commandFor(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));
}
juce::var StartupRegistration::runChecks(const juce::File&) {
    // Exercise a private test key, preserving the real Windows Run entry.
    const auto key = "Software\\Soundee\\StartupQA\\" + juce::Uuid().toString();
    const auto command = commandFor(juce::File("C:\\A folder with spaces\\Soundee.exe"));
    auto result = setAt(key.toWideCharPointer(), true, command);
    HKEY handle = nullptr; wchar_t value[2048]{}; DWORD bytes = sizeof(value), type = 0;
    bool read = RegOpenKeyExW(HKEY_CURRENT_USER, key.toWideCharPointer(), 0, KEY_QUERY_VALUE, &handle) == ERROR_SUCCESS;
    if (read) { read = RegQueryValueExW(handle, L"Soundee", nullptr, &type, reinterpret_cast<BYTE*>(value), &bytes) == ERROR_SUCCESS; RegCloseKey(handle); }
    const auto disabled = setAt(key.toWideCharPointer(), false, command);
    RegDeleteTreeW(HKEY_CURRENT_USER, key.toWideCharPointer());
    return fields({{"passed", result.wasOk() && disabled.wasOk() && read && juce::String(value) == command},
        {"quoted_path", command == "\"C:\\A folder with spaces\\Soundee.exe\" --background"}, {"real_startup_preserved", true}});
}
}

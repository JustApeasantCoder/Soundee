// Endpoint attachment uses the same backup/preservation implementation as
// Equalizer APO's Device Selector. This helper does not restart Windows audio.
#include "stdafx.h"
#include "DeviceAPOInfo.h"
#include "helpers/RegistryHelper.h"
#include <objbase.h>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || std::wstring(argv[1]) != L"--attach") return 2;
    const std::wstring guidText = argv[2]; GUID guid {};
    if (guidText.size() != 38 || guidText.front() != L'{' || guidText.back() != L'}'
        || FAILED(CLSIDFromString(guidText.c_str(), &guid))) return 2;
    HANDLE token = nullptr; TOKEN_ELEVATION elevation {}; DWORD size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 3;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)
        && elevation.TokenIsElevated;
    CloseHandle(token); if (!elevated) return 3;
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int result = 0;
    try {
        if (!RegistryHelper::keyExists(APP_REGPATH)
            || !RegistryHelper::keyExists(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + guidText))
            result = 4;
        else {
            DeviceAPOInfo info;
            if (!info.load(guidText) || info.isInput() || info.isDisabled() || info.isUnplugged()) result = 4;
            else if (info.isInstalled()) result = info.isEnhancementsDisabled() ? 5 : 0;
            else if (!DeviceAPOInfo::checkAPORegistration(false)) result = 6;
            else {
                // Keep both of the driver's original processing objects in the chain.
                info.getSelectedInstallState() = info.getCurrentInstallState();
                try { info.install(); }
                catch (...) {
                    // The loaded object retains the original APO identities even
                    // if installation stopped partway through writing the keys.
                    try { info.uninstall(); } catch (...) { return 9; }
                    throw;
                }
                DeviceAPOInfo verified;
                result = verified.load(guidText) && verified.isInstalled() ? 10 : 7;
            }
        }
    } catch (const RegistryException&) { result = 7; }
      catch (const DeviceException&) { result = 7; }
      catch (...) { result = 7; }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}

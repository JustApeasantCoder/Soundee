#include "SystemEq.h"
#include "NativeBridge.h"
#include "Diagnostics.h"
#include <juce_cryptography/juce_cryptography.h>
#include <stdexcept>
#include <string>
#include <cstring>
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

namespace soundee {
namespace {
constexpr const char* beginMarker = "# BEGIN SOUNDEE SYSTEM EQ";
constexpr const char* endMarker = "# END SOUNDEE SYSTEM EQ";
constexpr const char* includeLine = "Include: Soundee/active.txt";
const juce::String apoKey = "HKEY_LOCAL_MACHINE\\SOFTWARE\\EqualizerAPO\\";
juce::File registryFolder(const char* value) {
    const auto path = juce::WindowsRegistry::getValue(apoKey + value);
    return path.isNotEmpty() && juce::File::isAbsolutePath(path) ? juce::File(path) : juce::File();
}
bool atomicWrite(const juce::File& file, const juce::MemoryBlock& data) {
    juce::TemporaryFile temporary(file);
    return temporary.getFile().replaceWithData(data.getData(), data.getSize())
        && temporary.overwriteTargetFileWithTemporary();
}
juce::MemoryBlock utf8(const juce::String& text) { return { text.toRawUTF8(), text.getNumBytesAsUTF8() }; }
juce::String metadata(const juce::String& text, const juce::String& key) {
    for (const auto& line : juce::StringArray::fromLines(text))
        if (line.startsWith(key)) return line.substring(key.length()).trim();
    return {};
}
}
juce::File SystemEq::installDirectory() { return registryFolder("InstallPath"); }
juce::File SystemEq::configDirectory() { return registryFolder("ConfigPath"); }
juce::String SystemEq::nativePath(const juce::File& file) {
    // Resolve app-package file redirection before launching an elevated process,
    // which may not share the caller's virtualized LOCALAPPDATA view.
    if (file == juce::File()) return {};
    const HANDLE handle = CreateFileW(file.getFullPathName().toWideCharPointer(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        // Telemetry files are created by the DSP on first use. Resolve their
        // existing ancestor directory without creating an empty file here.
        const auto parent = file.getParentDirectory();
        if (file.exists() || parent == file) return {};
        const auto resolvedParent = nativePath(parent);
        return resolvedParent.isEmpty() ? juce::String() : juce::File(resolvedParent).getChildFile(file.getFileName()).getFullPathName();
    }
    std::vector<wchar_t> buffer(32768);
    const DWORD count = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    CloseHandle(handle);
    if (count == 0 || count >= buffer.size()) return {};
    auto path = juce::String(buffer.data());
    if (path.startsWith("\\\\?\\UNC\\")) return "\\\\" + path.substring(8);
    return path.startsWith("\\\\?\\") ? path.substring(4) : path;
}
bool SystemEq::validGuid(const juce::String& text) {
    if (text.length() != 38 || text[0] != '{' || text[37] != '}') return false;
    for (int i = 1; i < 37; ++i) {
        if (i == 9 || i == 14 || i == 19 || i == 24) { if (text[i] != '-') return false; }
        else if (!juce::String("0123456789abcdefABCDEF").containsChar(text[i])) return false;
    }
    return true;
}
juce::String SystemEq::stageName(ApoStage stage) {
    return stage == ApoStage::preMix ? "pre-mix" : stage == ApoStage::postMix ? "post-mix" : "none";
}
std::vector<OutputEndpoint> SystemEq::outputs() {
    std::vector<OutputEndpoint> result;
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IMMDeviceEnumerator> enumerator; ComPtr<IMMDeviceCollection> collection;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                IID_PPV_ARGS(enumerator.GetAddressOf())))
            && SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.GetAddressOf()))) {
            UINT count = 0; collection->GetCount(&count);
            juce::String defaultId; ComPtr<IMMDevice> defaultDevice;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, defaultDevice.GetAddressOf()))) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(defaultDevice->GetId(&id))) { defaultId = juce::String(id); CoTaskMemFree(id); }
            }
            for (UINT i = 0; i < count; ++i) {
                ComPtr<IMMDevice> device; ComPtr<IPropertyStore> properties;
                if (FAILED(collection->Item(i, device.GetAddressOf()))) continue;
                LPWSTR id = nullptr; if (FAILED(device->GetId(&id))) continue;
                OutputEndpoint output; output.id = juce::String(id); CoTaskMemFree(id);
                output.isDefault = output.id == defaultId;
                output.guid = output.id.substring(output.id.lastIndexOfChar('{'));
                if (!validGuid(output.guid)) continue;
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.GetAddressOf()))) {
                    PROPVARIANT value; PropVariantInit(&value);
                    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
                        output.name = juce::String(value.pwszVal);
                    PropVariantClear(&value);
                }
                const auto fx = "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\"
                    + output.guid + "\\FxProperties\\";
                for (int slot : {1, 2, 5, 6, 7}) {
                    const auto guid = juce::WindowsRegistry::getValue(fx + "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}," + juce::String(slot));
                    output.preMixInstalled |= guid.equalsIgnoreCase("{EACD2258-FCAC-4FF4-B36D-419E924A6D79}");
                    output.postMixInstalled |= guid.equalsIgnoreCase("{EC1CC9CE-FAED-4822-828A-82A81A6F018F}");
                }
                output.apoInstalled = output.preMixInstalled || output.postMixInstalled;
                output.effectsDisabled = juce::WindowsRegistry::getValue(fx + "{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5", "0").getIntValue() != 0;
                result.push_back(std::move(output));
            }
        }
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}
juce::Result SystemEq::editRoot(const juce::MemoryBlock& bytes, bool enable, juce::MemoryBlock& edited, ApoStage stage) {
    if (enable && stage == ApoStage::none) return juce::Result::fail("This output has no Equalizer APO processing stage.");
    std::string text(static_cast<const char*>(bytes.getData()), bytes.getSize());
    if (text.find('\0') != std::string::npos) return juce::Result::fail("The APO configuration uses an unsupported encoding. Convert config.txt to UTF-8 first.");
    const auto begin = text.find(beginMarker), end = text.find(endMarker);
    if ((begin == std::string::npos) != (end == std::string::npos)
        || (begin != std::string::npos && (end < begin || (begin > 0 && text[begin - 1] != '\n')
            || (end > 0 && text[end - 1] != '\n') || text.find(beginMarker, begin + 1) != std::string::npos
            || text.find(endMarker, end + 1) != std::string::npos)))
        return juce::Result::fail("Soundee's configuration block is damaged or duplicated. Restore config.txt from its backup first.");
    const auto insertion = begin;
    juce::String restoreStage = stageName(stage);
    if (begin == std::string::npos) {
        // Keep an explicit user scope when the managed block is first appended.
        for (const auto& line : juce::StringArray::fromLines(juce::String::fromUTF8(text.data(), static_cast<int>(text.size()))))
            if (line.trimStart().startsWith("Stage:")) restoreStage = line.fromFirstOccurrenceOf(":", false, false).trim();
    }
    if (begin != std::string::npos) {
        const auto oldBlock = juce::String::fromUTF8(text.data() + begin, static_cast<int>(end - begin));
        restoreStage = metadata(oldBlock, "# soundee_restore_stage: ");
        // Older versions left the selected stage in scope after the block.
        // Retain that scope when upgrading, even if the output's stage changes.
        if (restoreStage.isEmpty()) restoreStage = metadata(oldBlock, "Stage: ");
        if (restoreStage.isEmpty()) return juce::Result::fail("Soundee's configuration stage is missing. Restore config.txt from its backup first.");
        auto after = end + std::strlen(endMarker);
        if (after < text.size() && text[after] != '\r' && text[after] != '\n')
            return juce::Result::fail("Soundee's configuration end marker is damaged.");
        if (after < text.size() && text[after] == '\r') ++after;
        if (after < text.size() && text[after] == '\n') ++after;
        text.erase(begin, after - begin);
    }
    // Unmanaged includes would apply the calibration twice; do not rewrite them.
    const auto lower = juce::String::fromUTF8(text.data(), static_cast<int>(text.size())).toLowerCase();
    for (const auto& line : juce::StringArray::fromLines(lower))
        if (line.trimStart().startsWith("include:") && line.replaceCharacter('\\', '/').contains("soundee/active.txt"))
            return juce::Result::fail("An unmanaged Soundee include already exists in config.txt. Remove that include before enabling Soundee.");
    if (enable) {
        const std::string newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        if (insertion == std::string::npos && !text.empty() && text.back() != '\n') text += newline;
        const auto block = std::string(beginMarker) + newline + "# soundee_restore_stage: " + restoreStage.toStdString() + newline
            + "Device: ALL" + newline + "Stage: " + stageName(stage).toStdString() + newline
            + "Channel: ALL" + newline + includeLine + newline + "Device: ALL" + newline + "Stage: " + restoreStage.toStdString()
            + newline + "Channel: ALL" + newline + endMarker + newline;
        text.insert(insertion == std::string::npos ? text.size() : insertion, block);
    }
    edited = juce::MemoryBlock(text.data(), text.size());
    return juce::Result::ok();
}
SystemEqState SystemEq::state(const juce::File& directory) {
    SystemEqState result;
    if (directory == juce::File()) return result;
    const auto root = directory.getChildFile("config.txt").loadFileAsString();
    juce::MemoryBlock bytes, edited;
    if (!directory.getChildFile("config.txt").loadFileAsData(bytes) || editRoot(bytes, false, edited).failed()
        || !root.contains(beginMarker) || !root.contains(includeLine)) return result;
    const auto active = directory.getChildFile("Soundee/active.txt").loadFileAsString();
    result.endpointGuid = metadata(active, "# endpoint_guid: ");
    result.bypassed = metadata(active, "# correction_bypassed: ") == "1";
    result.profileName = metadata(active, "# profile_name: ");
    result.sampleRate = metadata(active, "# sample_rate: ").getDoubleValue();
    result.preampDb = metadata(active, "# applied_headroom_db: ").getDoubleValue();
    result.processingToken = static_cast<uint64_t>(metadata(active, "# processing_token: ").getHexValue64());
    result.limiterEnabled = metadata(active, "# limiter_enabled: ") == "1";
    const auto dspPath = metadata(active, "# dsp_path: ");
    result.dspPresent = dspPath.isNotEmpty() && juce::File(dspPath).existsAsFile();
    const auto stage = metadata(active, "Stage: ");
    result.stage = stage == "pre-mix" ? ApoStage::preMix : stage == "post-mix" ? ApoStage::postMix : ApoStage::none;
    result.configured = validGuid(result.endpointGuid) && active.contains("Device: " + result.endpointGuid)
        && active.contains("# SOUNDEE MANAGED CORRECTION") && result.stage != ApoStage::none;
    return result;
}
juce::Result SystemEq::configure(const juce::File& directory, const OutputEndpoint& output,
    const Profile& profile, const Settings& settings, bool bypassed, double rate) {
    if (directory == juce::File() || !directory.isDirectory()) return juce::Result::fail("Install Equalizer APO first using Backend setup.");
    if (!validGuid(output.guid) || !output.apoInstalled || output.preferredStage() == ApoStage::none || output.effectsDisabled)
        return juce::Result::fail("Equalizer APO must be attached to this output, with audio enhancements enabled.");
    auto root = directory.getChildFile("config.txt"); juce::MemoryBlock original, edited;
    if (!root.loadFileAsData(original)) return juce::Result::fail("Unable to read Equalizer APO's config.txt.");
    // The official installer supplies an audible example EQ. Preserve its bytes
    // in the backup, but do not stack that factory demonstration with calibration.
    const auto factory = juce::String("Preamp: -6 dB\nInclude: example.txt\nGraphicEQ: 25 0; 40 0; 63 0; 100 0; 160 0; 250 0; 400 0; 630 0; 1000 0; 1600 0; 2500 0; 4000 0; 6300 0; 10000 0; 16000 0");
    auto toEdit = original;
    if (juce::String::fromUTF8(static_cast<const char*>(original.getData()), static_cast<int>(original.getSize())).replace("\r", "").trim() == factory)
        toEdit = utf8("# Equalizer APO factory demo disabled by Soundee; original saved in Soundee/backups.\n");
    // Explicit Stage commands do not use Equalizer APO's automatic pre-mix
    // fallback. Prefer the installed post-mix class when both exist, so the
    // profile is applied once rather than once per processing stage.
    const auto stage = output.preferredStage();
    const auto edit = editRoot(toEdit, true, edited, stage); if (edit.failed()) return edit;
    auto managed = directory.getChildFile("Soundee");
    if (managed.createDirectory().failed()) return juce::Result::fail("Soundee cannot write to the APO configuration folder.");
    juce::File dsp;
    const auto deployed = NativeBridge::deploy(managed, dsp); if (deployed.failed()) return deployed;
    if (nativePath(dsp).isEmpty() || nativePath(NativeBridge::telemetryFile(directory, output.guid)).isEmpty())
        return juce::Result::fail("Unable to resolve Soundee's native processing paths; the existing correction was preserved.");
    // Back up the exact bytes before each root change, including other tools' edits.
    if (original != edited) {
        auto backups = managed.getChildFile("backups"); if (backups.createDirectory().failed()) return juce::Result::fail("Unable to create the configuration backup folder.");
        const auto backup = backups.getChildFile("config-" + juce::String(juce::Time::currentTimeMillis()) + "-" + juce::Uuid().toString() + ".txt");
        if (!backup.replaceWithData(original.getData(), original.getSize())) return juce::Result::fail("Unable to back up config.txt; activation was cancelled.");
    }
    const auto active = managed.getChildFile("active.txt"); juce::MemoryBlock previous;
    const bool existed = active.existsAsFile();
    if (existed && !active.loadFileAsData(previous)) return juce::Result::fail("Unable to preserve the previous calibration.");
    const auto correction = apoConfiguration(profile, settings, bypassed, rate);
    const auto token = NativeBridge::tokenFor(correction + output.guid + juce::String(settings.limiterEnabled ? 1 : 0) + juce::String(settings.limiterCeiling, 3) + stageName(stage));
    const auto text = "# SOUNDEE MANAGED CORRECTION\n# endpoint_guid: " + output.guid
        + "\n# correction_bypassed: " + juce::String(bypassed ? 1 : 0)
        + "\n# profile_name: " + profile.name.replaceCharacters("\r\n", "  ")
        + "\n# sample_rate: " + juce::String(rate, 0) + "\n# applied_headroom_db: " + juce::String(appliedHeadroomDb(profile, settings, rate), 6)
        + "\n# processing_token: " + juce::String::toHexString(static_cast<juce::int64>(token))
        + "\n# limiter_enabled: " + juce::String(settings.limiterEnabled && !bypassed ? 1 : 0)
        + "\n# dsp_path: " + SystemEq::nativePath(dsp)
        + "\nDevice: " + output.guid + "\nStage: " + stageName(stage) + "\n" + correction
        + "Channel: L R\n" + NativeBridge::filterLine(dsp, NativeBridge::telemetryFile(directory, output.guid), token, settings, bypassed)
        + "Device: ALL\nChannel: ALL\n";
    if (!atomicWrite(active, utf8(text))) return juce::Result::fail("Unable to update the system correction; the previous file was preserved.");
    juce::MemoryBlock currentRoot;
    const bool rootUnchanged = root.loadFileAsData(currentRoot) && currentRoot == original;
    if (!rootUnchanged || (original != edited && !atomicWrite(root, edited))) {
        const bool restored = existed ? atomicWrite(active, previous) : active.deleteFile();
        return juce::Result::fail(restored ? "Unable to update config.txt; the previous correction was restored."
            : "Unable to update config.txt or restore the correction. Disable Soundee and restore its configuration backup.");
    }
    return juce::Result::ok();
}
juce::Result SystemEq::disable(const juce::File& directory) {
    if (directory == juce::File()) return juce::Result::ok();
    auto root = directory.getChildFile("config.txt"); juce::MemoryBlock original, edited;
    if (!root.loadFileAsData(original)) return juce::Result::fail("Unable to read config.txt; Soundee may still be enabled.");
    auto result = editRoot(original, false, edited); if (result.failed()) return result;
    if (original == edited) return juce::Result::ok();
    auto backups = directory.getChildFile("Soundee/backups"); if (backups.createDirectory().failed()) return juce::Result::fail("Unable to back up config.txt before disabling.");
    if (!backups.getChildFile("config-disabled-" + juce::Uuid().toString() + ".txt").replaceWithData(original.getData(), original.getSize()))
        return juce::Result::fail("Unable to back up config.txt before disabling.");
    return atomicWrite(root, edited) ? juce::Result::ok() : juce::Result::fail("Unable to disable system EQ; the original configuration was preserved.");
}
juce::var SystemEq::runChecks(const Profile& profile, const juce::File& folder) {
    Diagnostics logs(folder.getChildFile("system-eq-logs")); const auto request = juce::Uuid().toString();
    logs.event("system_eq", "system_eq.qa.started", "[System EQ] Configuration validation started", "running", {}, request);
    auto require = [](bool ok, const char* error) { if (!ok) throw std::runtime_error(error); };
    auto directory = folder.getChildFile("system-eq-fixture"); directory.createDirectory();
    const auto original = juce::String("# Existing user settings\r\nDevice: another-output\r\nChannel: R\r\nPreamp: -2 dB\r\n");
    auto root = directory.getChildFile("config.txt"); root.replaceWithText(original);
    require(nativePath(root).isNotEmpty() && juce::File(nativePath(root)).existsAsFile(), "Native file path resolution failed");
    const auto future = directory.getChildFile("future-" + juce::Uuid().toString()).getChildFile("telemetry.bin");
    require(nativePath(directory).isNotEmpty() && nativePath(future).isNotEmpty() && !future.exists(), "Future native telemetry path resolution failed or created a file");
    OutputEndpoint output; output.guid = "{11111111-2222-3333-4444-555555555555}";
    output.apoInstalled = output.postMixInstalled = true;
    require(configure(directory, output, profile, {}, false).wasOk(), "System EQ activation failed");
    require(root.loadFileAsString().startsWith(original), "Existing configuration changed");
    require(state(directory).configured && state(directory).endpointGuid == output.guid, "System EQ state was not recovered");
    const auto firstRoot = root.loadFileAsString(); Settings changed; changed.maximumBoost = 3;
    require(configure(directory, output, profile, changed, true).wasOk(), "System EQ live update failed");
    require(root.loadFileAsString() == firstRoot && state(directory).bypassed, "Repeated activation duplicated the include");
    const auto active = directory.getChildFile("Soundee/active.txt").loadFileAsString();
    require(active.contains("Device: " + output.guid) && active.contains("Preamp: -") && !active.contains("GraphicEQ:"), "System bypass did not retain headroom");
    auto unattached = output; unattached.apoInstalled = false;
    require(configure(directory, unattached, profile, changed, false).failed()
        && active == directory.getChildFile("Soundee/active.txt").loadFileAsString(), "Unattached output changed the calibration");
    require(disable(directory).wasOk() && root.loadFileAsString() == original && !state(directory).configured, "Disable changed unrelated settings");
    // Regression: SFX-only attachments must not target an unavailable post-mix
    // stage, which makes the real backend skip the entire include.
    auto preOnly = output; preOnly.postMixInstalled = false; preOnly.preMixInstalled = true;
    require(configure(directory, preOnly, profile, {}, false).wasOk(), "Pre-mix-only activation failed");
    require(state(directory).stage == ApoStage::preMix && root.loadFileAsString().contains("Stage: pre-mix")
        && !root.loadFileAsString().contains("Stage: post-mix"), "Pre-mix-only output targeted an unavailable stage");
    auto both = preOnly; both.postMixInstalled = true;
    require(configure(directory, both, profile, {}, false).wasOk() && state(directory).stage == ApoStage::postMix
        && root.loadFileAsString().replace("\r", "").contains("Stage: post-mix\nChannel: ALL\nInclude: Soundee/active.txt"), "Dual-stage attachment could apply correction twice");
    auto unknownStage = output; unknownStage.postMixInstalled = false;
    require(configure(directory, unknownStage, profile, {}, false).failed(), "Unidentified APO stage was accepted");
    require(disable(directory).wasOk() && root.loadFileAsString() == original, "Stage migration changed unrelated configuration");
    juce::MemoryBlock base = utf8(original), added;
    require(editRoot(base, true, added).wasOk(), "Root edit failed");
    auto withFollowingEdits = utf8(juce::String::fromUTF8(static_cast<const char*>(added.getData()), static_cast<int>(added.getSize())) + "Preamp: -1 dB\r\n");
    require(editRoot(withFollowingEdits, true, added).wasOk() && added == withFollowingEdits, "An update reordered the user's later filters");
    require(editRoot(withFollowingEdits, false, added).wasOk() && added == utf8(original + "Preamp: -1 dB\r\n"), "Disable deleted later user edits");
    const auto legacyRoot = utf8("Stage: post-mix\n# BEGIN SOUNDEE SYSTEM EQ\nDevice: ALL\nStage: post-mix\nChannel: ALL\nInclude: Soundee/active.txt\nDevice: ALL\nChannel: ALL\n# END SOUNDEE SYSTEM EQ\nPreamp: -6 dB\n");
    require(editRoot(legacyRoot, true, added, ApoStage::preMix).wasOk(), "Legacy stage migration failed");
    const auto migrated = juce::String::fromUTF8(static_cast<const char*>(added.getData()), static_cast<int>(added.getSize()));
    require(migrated.contains("Stage: pre-mix\nChannel: ALL\nInclude: Soundee/active.txt\nDevice: ALL\nStage: post-mix\n")
        && migrated.endsWith("# END SOUNDEE SYSTEM EQ\nPreamp: -6 dB\n"), "Switching APO stage changed the scope of later filters");
    require(folder.getChildFile("stage-scope-before.txt").replaceWithData(legacyRoot.getData(), legacyRoot.getSize())
        && folder.getChildFile("stage-scope-after.txt").replaceWithData(added.getData(), added.getSize()), "Cannot save stage-scope regression fixtures");
    juce::MemoryBlock migratedAgain;
    require(editRoot(added, true, migratedAgain, ApoStage::postMix).wasOk()
        && editRoot(migratedAgain, true, migratedAgain, ApoStage::preMix).wasOk() && migratedAgain == added,
        "Repeated stage changes lost the original trailing filter scope");
    require(editRoot(added, false, migratedAgain).wasOk() && migratedAgain == utf8("Stage: post-mix\nPreamp: -6 dB\n"),
        "Disabling migrated correction changed the later filters");
    juce::MemoryBlock malformed = utf8("# BEGIN SOUNDEE SYSTEM EQ\n"), edited;
    require(editRoot(malformed, true, edited).failed(), "Damaged marker was accepted");
    const char utf16[] = {'#', '\0', 'a', '\0'}; malformed = juce::MemoryBlock(utf16, sizeof(utf16));
    require(editRoot(malformed, true, edited).failed(), "Unsupported encoding was accepted");
    require(!validGuid("{11111111}; Device: ALL"), "Device command injection was accepted");
    juce::Array<juce::File> backups; directory.getChildFile("Soundee/backups").findChildFiles(backups, juce::File::findFiles, false);
    require(backups.size() >= 2, "Configuration backups were not created");
    const auto demo = juce::String("Preamp: -6 dB\nInclude: example.txt\nGraphicEQ: 25 0; 40 0; 63 0; 100 0; 160 0; 250 0; 400 0; 630 0; 1000 0; 1600 0; 2500 0; 4000 0; 6300 0; 10000 0; 16000 0");
    root.replaceWithText(demo);
    juce::MemoryBlock demoBytes; root.loadFileAsData(demoBytes);
    require(configure(directory, output, profile, {}, false).wasOk() && !root.loadFileAsString().contains("Include: example.txt"), "Factory demo was stacked with calibration");
    juce::Array<juce::File> allBackups; directory.getChildFile("Soundee/backups").findChildFiles(allBackups, juce::File::findFiles, false);
    bool demoBackedUp = false; for (const auto& file : allBackups) { juce::MemoryBlock bytes; file.loadFileAsData(bytes); demoBackedUp |= bytes == demoBytes; }
    require(demoBackedUp, "Factory demo configuration was not backed up");
    require(!BackendDownload::verified(root), "An invalid installer passed verification");
    logs.event("system_eq", "system_eq.qa.completed", "[System EQ] Configuration validation passed", "success",
        fields({{"unrelated_configuration_preserved", true}, {"factory_demo_backed_up", true}, {"installer_integrity_check", true}}), request);
    return fields({{"passed", true}, {"configuration_preserved", true}, {"single_include", true}, {"scoped_output", true},
        {"linked_bypass", true}, {"disable_restores_configuration", true}, {"invalid_output_rejected", true},
        {"factory_demo_not_stacked", true}, {"installer_hash_check", true}, {"pre_mix_only_stage", true},
        {"dual_stage_applied_once", true}, {"stage_migration_preserves_configuration", true}, {"trailing_filter_stage_preserved", true}, {"backups", allBackups.size()}});
}
bool BackendDownload::verified(const juce::File& file) {
    return file.getSize() == 11980366 && juce::SHA256(file).toHexString().equalsIgnoreCase(
        "7403be7427bbe1936a40dded082829b6e217fc4f5990fee5cba501f0ae055afa");
}
void BackendDownload::run() {
    if (verified(file)) { success = true; return; }
    if (file.getParentDirectory().createDirectory().failed()) { error = "Unable to create the backend download folder."; return; }
    juce::TemporaryFile temporary(file);
    int status = 0;
    auto stream = juce::URL("https://downloads.sourceforge.net/project/equalizerapo/1.4.2/EqualizerAPO-x64-1.4.2.exe")
        .createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs(15000).withNumRedirectsToFollow(5).withStatusCode(&status));
    if (!stream || status != 200) { error = "The official backend download failed. Check your connection and retry."; return; }
    auto output = temporary.getFile().createOutputStream();
    if (!output) { error = "Unable to save the backend installer."; return; }
    char buffer[65536];
    while (!threadShouldExit() && !stream->isExhausted()) {
        const int count = stream->read(buffer, sizeof(buffer)); if (count <= 0) break;
        if (downloaded.load() + count > 11980366 || !output->write(buffer, static_cast<size_t>(count))) {
            error = "The backend download was incomplete or unexpected."; return;
        }
        downloaded += count;
    }
    output->flush(); output.reset();
    if (threadShouldExit()) { error = "Backend download cancelled."; return; }
    if (!verified(temporary.getFile())) { error = "The backend installer did not pass its integrity check. It was not run."; return; }
    success = temporary.overwriteTargetFileWithTemporary();
    if (!success) error = "Unable to finalize the verified backend installer.";
}
}

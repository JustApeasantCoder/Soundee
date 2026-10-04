#include "NativeBridge.h"
#include "SystemEq.h"

namespace soundee {
juce::Result NativeBridge::deploy(const juce::File& managed, juce::File& installed) {
    const auto source = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("SoundeeDSP.dll");
    if (!source.existsAsFile()) return juce::Result::fail("SoundeeDSP.dll is missing. Extract the complete Soundee package.");
    const auto digest = juce::SHA256(source).toHexString();
    installed = managed.getChildFile("SoundeeDSP-" + digest.substring(0, 16) + ".dll");
    if (installed.existsAsFile()) return juce::SHA256(installed).toHexString() == digest ? juce::Result::ok() : juce::Result::fail("The installed Soundee DSP module failed its integrity check.");
    juce::TemporaryFile temporary(installed);
    if (!source.copyFileTo(temporary.getFile()) || juce::SHA256(temporary.getFile()).toHexString() != digest
        || !temporary.overwriteTargetFileWithTemporary()) return juce::Result::fail("Unable to install Soundee's DSP module; the existing correction was preserved.");
    return juce::Result::ok();
}
uint64_t NativeBridge::tokenFor(const juce::String& text) { return static_cast<uint64_t>(juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString().substring(0, 16).getHexValue64()); }
juce::File NativeBridge::telemetryFile(const juce::File& directory, const juce::String& guid) {
    return directory.getChildFile("Soundee/telemetry-" + guid.retainCharacters("0123456789abcdefABCDEF-").toLowerCase() + ".bin");
}
juce::String NativeBridge::filterLine(const juce::File& plugin, const juce::File& telemetry, uint64_t token, const Settings& s, bool bypass) {
    const auto path = telemetry == juce::File() ? juce::String() : SystemEq::nativePath(telemetry);
    const auto chunk = "SOUNDEE1\n" + juce::String(s.limiterEnabled && !bypass ? "1" : "0") + "\n"
        + juce::String(s.limiterCeiling, 3) + "\n" + path + "\n" + juce::String::toHexString(static_cast<juce::int64>(token)) + "\n";
    return "VSTPlugin: Library " + SystemEq::nativePath(plugin).quoted() + " ChunkData "
        + juce::Base64::toBase64(chunk.toRawUTF8(), chunk.getNumBytesAsUTF8()).quoted() + "\n";
}
}

#include "ProfileArchive.h"
#include "Diagnostics.h"
#include <set>

namespace soundee {
juce::Result ProfileArchive::retainCalibration(const juce::File& source, const juce::File& store, juce::File& retained) {
    try { Profile::load(source); } catch (const std::exception& error) { return juce::Result::fail(error.what()); }
    const auto digest = juce::SHA256(source).toHexString();
    auto target = store.getChildFile(digest).getChildFile(source.getFileName());
    if (target == source || (target.existsAsFile() && juce::SHA256(target).toHexString() == digest)) { retained = target; return juce::Result::ok(); }
    if (target.getParentDirectory().createDirectory().failed()) return juce::Result::fail("Unable to create the calibration backup folder.");
    juce::TemporaryFile temporary(target);
    if (!source.copyFileTo(temporary.getFile()) || juce::SHA256(temporary.getFile()).toHexString() != digest
        || !temporary.overwriteTargetFileWithTemporary()) return juce::Result::fail("Unable to preserve this calibration file. Import was cancelled.");
    retained = target; return juce::Result::ok();
}
juce::Result ProfileArchive::write(const ListeningProfiles& library, const juce::File& archive) {
    auto manifestLibrary = juce::JSON::parse(library.serialize());
    juce::Array<juce::var> calibrations; juce::ZipFile::Builder builder; std::set<juce::String> included;
    for (int i = 0; i < static_cast<int>(library.presets.size()); ++i) {
        const auto& preset = library.presets[static_cast<size_t>(i)];
        manifestLibrary["presets"][i].getDynamicObject()->setProperty("calibration_path", "");
        if (preset.calibrationPath.isEmpty()) continue;
        const juce::File file(preset.calibrationPath);
        try { Profile::load(file); } catch (...) { return juce::Result::fail("Cannot back up " + preset.name + ": its calibration is unavailable."); }
        const auto hash = juce::SHA256(file).toHexString(), entry = "calibrations/" + hash + ".swproj";
        manifestLibrary["presets"][i].getDynamicObject()->setProperty("calibration_hash", hash);
        if (included.insert(entry).second) builder.addFile(file, 6, entry);
        calibrations.add(fields({{"preset_id", preset.id}, {"sha256", hash}, {"filename", file.getFileName()}, {"entry", entry}}));
    }
    const auto manifest = juce::JSON::toString(fields({{"format", "soundee-backup"}, {"version", 1},
        {"library", manifestLibrary}, {"calibrations", calibrations}}));
    builder.addEntry(new juce::MemoryInputStream(manifest.toRawUTF8(), manifest.getNumBytesAsUTF8(), true), 6, "manifest.json", juce::Time::getCurrentTime());
    juce::TemporaryFile temporary(archive);
    auto output = temporary.getFile().createOutputStream();
    if (!output || !builder.writeToStream(*output, nullptr)) return juce::Result::fail("Unable to write the Soundee backup.");
    output->flush(); output.reset();
    return temporary.overwriteTargetFileWithTemporary() ? juce::Result::ok() : juce::Result::fail("Unable to finish the backup; the previous file was preserved.");
}
juce::Result ProfileArchive::read(const juce::File& archive, const juce::File& store, ListeningProfiles& target) {
    if (!archive.existsAsFile() || archive.getSize() > 256 * 1024 * 1024) return juce::Result::fail("This backup is missing or exceeds 256 MB.");
    juce::ZipFile zip(archive);
    if (zip.getNumEntries() > 129) return juce::Result::fail("This backup contains too many files.");
    const auto* manifestEntry = zip.getEntry("manifest.json");
    if (!manifestEntry || manifestEntry->uncompressedSize > 4 * 1024 * 1024) return juce::Result::fail("The backup manifest is missing or too large.");
    std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(*manifestEntry));
    juce::var manifest; juce::MemoryBlock manifestBytes;
    if (!stream || stream->readIntoMemoryBlock(manifestBytes, 4 * 1024 * 1024 + 1) > 4 * 1024 * 1024
        || juce::JSON::parse(manifestBytes.toString(), manifest).failed()
        || manifest["format"].toString() != "soundee-backup" || !manifest["version"].isInt() || static_cast<int>(manifest["version"]) != 1
        || !manifest["calibrations"].isArray()) return juce::Result::fail("Unsupported or damaged Soundee backup.");
    ListeningProfiles restored;
    const auto valid = restored.restore(juce::JSON::toString(manifest["library"])); if (valid.failed()) return valid;
    for (const auto& p : restored.presets) if (p.calibrationPath.isNotEmpty()) return juce::Result::fail("A portable backup cannot refer to an external calibration path.");
    if (manifest["calibrations"].getArray()->size() > 128) return juce::Result::fail("Invalid calibration list.");
    // Read named entries explicitly. Never extract ZIP paths to the filesystem.
    struct Imported { juce::String id, hash, name; juce::MemoryBlock data; };
    std::vector<Imported> imports; std::set<juce::String> identities; juce::int64 total = 0;
    for (const auto& item : *manifest["calibrations"].getArray()) {
        Imported imported; imported.id = item["preset_id"].toString(); imported.hash = item["sha256"].toString(); imported.name = item["filename"].toString();
        const auto entryName = item["entry"].toString();
        if (!item.isObject() || imported.hash.length() != 64 || !imported.hash.containsOnly("0123456789abcdef")
            || entryName != "calibrations/" + imported.hash + ".swproj" || imported.name.isEmpty() || imported.name.containsAnyOf("/\\:")
            || !imported.name.endsWithIgnoreCase(".swproj") || !identities.insert(imported.id).second)
            return juce::Result::fail("Invalid calibration entry in backup.");
        bool found = false; for (const auto& p : restored.presets) found |= p.id == imported.id;
        const auto* entry = zip.getEntry(entryName);
        if (!found || !entry || entry->uncompressedSize <= 0 || entry->uncompressedSize > 128 * 1024 * 1024
            || (total += entry->uncompressedSize) > 256 * 1024 * 1024) return juce::Result::fail("The backup has a missing or oversized calibration.");
        std::unique_ptr<juce::InputStream> data(zip.createStreamForEntry(*entry));
        if (!data || data->readIntoMemoryBlock(imported.data, static_cast<ssize_t>(entry->uncompressedSize + 1)) != entry->uncompressedSize
            || juce::SHA256(imported.data).toHexString() != imported.hash) return juce::Result::fail("A calibration failed its integrity check.");
        imports.push_back(std::move(imported));
    }
    for (const auto& p : *manifest["library"]["presets"].getArray()) if (p.hasProperty("calibration_hash")) {
        bool found = false;
        for (const auto& item : imports) found |= item.id == p["id"].toString() && item.hash == p["calibration_hash"].toString();
        if (!found) return juce::Result::fail("A calibrated profile is missing its calibration file.");
    }
    const auto staging = store.getChildFile("import-" + juce::Uuid().toString());
    if (staging.createDirectory().failed()) return juce::Result::fail("Unable to stage the backup import.");
    struct Cleanup { juce::File folder, root; ~Cleanup() { if (folder.isAChildOf(root)) folder.deleteRecursively(); } } cleanup {staging, store};
    for (const auto& item : imports) {
        const auto file = staging.getChildFile(item.hash).getChildFile(item.name);
        if (file.getParentDirectory().createDirectory().failed() || !file.replaceWithData(item.data.getData(), item.data.getSize())) return juce::Result::fail("Unable to stage a calibration.");
        try { Profile::load(file); } catch (...) { return juce::Result::fail("The backup contains an unsupported calibration."); }
    }
    // Only publish paths after every calibration passed validation.
    for (const auto& item : imports) {
        juce::File retained;
        const auto result = retainCalibration(staging.getChildFile(item.hash).getChildFile(item.name), store, retained);
        if (result.failed()) return result;
        for (auto& p : restored.presets) if (p.id == item.id) p.calibrationPath = retained.getFullPathName();
    }
    target = std::move(restored); return juce::Result::ok();
}
}

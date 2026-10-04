#include "Profile.h"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace soundee {
Profile Profile::flat(const juce::String& name) {
    Profile result; result.name = name; result.flatResponse = true;
    for (int channel = 0; channel < 4; ++channel) {
        auto& curve = result.curves[static_cast<size_t>(channel)]; curve.id = channel + 1;
        for (int i = 0; i < 355; ++i) curve.points.push_back({20 * std::pow(1100.0, i / 354.0), 0, 0});
    }
    return result;
}
Profile Profile::load(const juce::File& file) {
    auto stream = file.createInputStream();
    if (!stream || file.getSize() > 128 * 1024 * 1024)
        throw std::runtime_error("Cannot read this profile, or it exceeds 128 MB.");
    juce::MemoryBlock prefix;
    stream->readIntoMemoryBlock(prefix, 65536);
    // Search bytes: opaque binary may contain NULs and cannot be treated as text.
    std::string bytes(static_cast<const char*>(prefix.getData()), prefix.getSize());
    const auto close = bytes.find("</ProjectHeader>");
    if (close == std::string::npos) throw std::runtime_error("No SoundID project header found.");
    const auto end = close + std::string("</ProjectHeader>").size();
    auto xml = juce::parseXML(juce::String::fromUTF8(bytes.data(), static_cast<int>(end)));
    if (!xml || !xml->hasTagName("ProjectHeader") || end + 5 > bytes.size()
        || static_cast<unsigned char>(bytes[end]) != 0x1b || bytes.substr(end + 1, 4) != "PEQb")
        throw std::runtime_error("Unsupported SoundID container format.");
    auto parts = xml->getChildByName("Parts");
    int size = 0;
    if (parts) for (auto* part : parts->getChildIterator())
        if (part->getChildElementAllSubText("Type", {}) == "eqb")
            size = part->getChildElementAllSubText("Size", {}).getIntValue();
    if (size != 34192 || end + 1 + static_cast<size_t>(size) > static_cast<size_t>(file.getSize()))
        throw std::runtime_error("This version of Soundee supports PEQb v3 stereo profiles with 355 points.");
    stream->setPosition(static_cast<juce::int64>(end + 1));
    juce::MemoryBlock block;
    stream->readIntoMemoryBlock(block, size);
    if (block.getSize() != static_cast<size_t>(size)) throw std::runtime_error("Truncated EQ data.");
    size_t cursor = 4;
    auto u32 = [&]() {
        if (cursor + 4 > block.getSize()) throw std::runtime_error("Truncated EQ number.");
        auto v = juce::ByteOrder::littleEndianInt(static_cast<const char*>(block.getData()) + cursor);
        cursor += 4; return v;
    };
    auto f64 = [&]() {
        if (cursor + 8 > block.getSize()) throw std::runtime_error("Truncated EQ number.");
        auto bits = juce::ByteOrder::littleEndianInt64(static_cast<const char*>(block.getData()) + cursor);
        cursor += 8;
        auto value = std::bit_cast<double>(bits);
        if (!std::isfinite(value)) throw std::runtime_error("The profile contains a nonfinite value.");
        return value;
    };
    if (u32() != 3 || u32() != 4) throw std::runtime_error("Unsupported PEQb version or curve count.");
    Profile profile; profile.name = file.getFileNameWithoutExtension(); profile.source = file;
    for (int c = 0; c < 4; ++c) {
        auto& curve = profile.curves[static_cast<size_t>(c)];
        curve.id = static_cast<int>(u32()); curve.scalar1 = f64(); curve.scalar2 = f64();
        if (curve.id != c + 1 || u32() != 355) throw std::runtime_error("Unexpected EQ curve layout.");
        for (int i = 0; i < 355; ++i) {
            Point point { f64(), f64(), f64() };
            if (point.frequency <= 0 || point.frequency > 100000 || std::abs(point.db) > 120
                || (!curve.points.empty() && point.frequency <= curve.points.back().frequency))
                throw std::runtime_error("Invalid frequency curve.");
            curve.points.push_back(point);
        }
    }
    if (u32() != 0 || cursor != block.getSize()) throw std::runtime_error("Unexpected EQ trailer.");
    for (size_t c = 0; c < 2; ++c)
        for (size_t i = 0; i < 355; ++i) {
            auto a = profile.curves[c].points[i], b = profile.curves[c + 2].points[i];
            if (std::abs(a.frequency - b.frequency) > 1e-7 || std::abs(a.db + b.db) > 1e-7)
                throw std::runtime_error("This profile does not have the expected stereo inverse pairs.");
        }
    return profile;
}
double interpolate(const Curve& curve, double frequency) {
    if (curve.points.empty()) return 0;
    if (frequency <= curve.points.front().frequency) return curve.points.front().db;
    if (frequency >= curve.points.back().frequency) return curve.points.back().db;
    auto upper = std::lower_bound(curve.points.begin(), curve.points.end(), frequency,
        [](const Point& p, double f) { return p.frequency < f; });
    const auto& lower = *(upper - 1);
    auto t = std::log(frequency / lower.frequency) / std::log(upper->frequency / lower.frequency);
    return lower.db + t * (upper->db - lower.db);
}
}

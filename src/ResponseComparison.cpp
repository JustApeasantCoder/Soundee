#include "ResponseComparison.h"
#include "Diagnostics.h"
#include <complex>
#include <cmath>
#include <stdexcept>
namespace soundee {
juce::Result ComparisonReport::writeCsv(const juce::File& file) const {
    juce::String text = "# Soundee minus reference; global level offset " + juce::String(levelOffset, 6) + " dB removed equally from L/R\nfrequency_hz,left_difference_db,right_difference_db\n";
    for (const auto& p : points) text += juce::String(p.frequency, 5) + "," + juce::String(p.left, 6) + "," + juce::String(p.right, 6) + "\n";
    return file.replaceWithText(text) ? juce::Result::ok() : juce::Result::fail("Unable to export the comparison.");
}
namespace {
constexpr int order = 14, size = 1 << order;
struct Spectrum { std::array<std::vector<double>, 2> power; double rate = 0; int windows = 0; };
juce::Result spectrum(const juce::File& file, Spectrum& result) {
    juce::WavAudioFormat wav;
    auto stream = file.createInputStream();
    if (!stream) return juce::Result::fail("The selected recording cannot be opened.");
    auto reader = std::unique_ptr<juce::AudioFormatReader>(wav.createReaderFor(stream.release(), true));
    if (!reader || reader->numChannels != 2 || reader->sampleRate < 32000 || reader->sampleRate > 192000
        || reader->lengthInSamples < size * 8 || reader->lengthInSamples > 8000000)
        return juce::Result::fail("Use a stereo WAV at 32-192 kHz, at least 131072 frames long, and no more than 8 million frames.");
    result.rate = reader->sampleRate;
    for (auto& channel : result.power) channel.assign(size / 2 + 1, 0);
    juce::AudioBuffer<float> audio(2, size); juce::dsp::FFT fft(order);
    std::vector<std::complex<float>> input(size), output(size);
    for (juce::int64 start = 0; start + size <= reader->lengthInSamples; start += size / 2) {
        if (!reader->read(&audio, 0, size, start, true, true)) return juce::Result::fail("A recording could not be read.");
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < size; ++i) {
                const double sample = audio.getSample(c, i);
                if (!std::isfinite(sample)) return juce::Result::fail("A recording contains nonfinite audio samples.");
                const double window = .5 - .5 * std::cos(juce::MathConstants<double>::twoPi * i / (size - 1));
                input[i] = static_cast<float>(sample * window);
            }
            fft.perform(input.data(), output.data(), false);
            for (int k = 0; k <= size / 2; ++k) result.power[c][k] += std::norm(output[k]);
        }
        ++result.windows;
    }
    return juce::Result::ok();
}
}
juce::Result ResponseComparison::analyze(const juce::File& reference, const juce::File& soundee, ComparisonReport& target) {
    Spectrum a, b; auto result = spectrum(reference, a); if (result.failed()) return result;
    result = spectrum(soundee, b); if (result.failed()) return result;
    if (a.rate != b.rate) return juce::Result::fail("Both recordings must use the same sample rate.");
    ComparisonReport report; std::vector<double> offsets;
    for (double f = 30; f <= std::min(16000., a.rate * .45); f *= std::pow(2., 1. / 12)) {
        const int first = std::max(1, static_cast<int>(std::ceil(f / std::pow(2., 1. / 24) * size / a.rate)));
        const int last = std::max(first, std::min(size / 2, static_cast<int>(std::floor(f * std::pow(2., 1. / 24) * size / a.rate))));
        ComparisonPoint point; point.frequency = f;
        for (int c = 0; c < 2; ++c) {
            double pa = 0, pb = 0; for (int k = first; k <= last; ++k) { pa += a.power[c][k]; pb += b.power[c][k]; }
            pa /= a.windows; pb /= b.windows;
            if (pa < 1e-12 || pb < 1e-12) return juce::Result::fail("A recording has insufficient broadband energy. Use the same test WAV for both captures.");
            const double db = 10 * std::log10(pb / pa); (c == 0 ? point.left : point.right) = db; offsets.push_back(db);
        }
        report.points.push_back(point);
    }
    std::sort(offsets.begin(), offsets.end()); report.levelOffset = offsets[offsets.size() / 2];
    double sum = 0;
    for (auto& p : report.points) {
        p.left -= report.levelOffset; p.right -= report.levelOffset;
        sum += p.left * p.left + p.right * p.right; report.maximumDifference = std::max({report.maximumDifference, std::abs(p.left), std::abs(p.right)});
    }
    report.rmsDifference = std::sqrt(sum / (2 * report.points.size()));
    report.description = "Level difference " + juce::String(report.levelOffset, 2) + " dB | EQ difference RMS "
        + juce::String(report.rmsDifference, 2) + " dB | largest " + juce::String(report.maximumDifference, 2) + " dB";
    target = std::move(report); return juce::Result::ok();
}
juce::Result ResponseComparison::createTestSignal(const juce::File& file, double rate) {
    if (rate < 32000 || rate > 192000) return juce::Result::fail("Unsupported test signal sample rate.");
    juce::TemporaryFile temporary(file); std::unique_ptr<juce::OutputStream> stream = temporary.getFile().createOutputStream();
    juce::WavAudioFormat format;
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions().withSampleRate(rate).withNumChannels(2)
        .withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (!writer) return juce::Result::fail("Unable to create the test WAV.");
    juce::Random random(0x534f554e444545); juce::AudioBuffer<float> audio(2, 1024);
    const int frames = static_cast<int>(rate * 10);
    for (int start = 0; start < frames; start += 1024) {
        const int count = std::min(1024, frames - start);
        for (int i = 0; i < count; ++i) {
            const float value = (random.nextFloat() * 2 - 1) * .063095734f;
            audio.setSample(0, i, value); audio.setSample(1, i, value);
        }
        if (!writer->writeFromAudioSampleBuffer(audio, 0, count)) return juce::Result::fail("Unable to write the test signal.");
    }
    writer.reset();
    return temporary.overwriteTargetFileWithTemporary() ? juce::Result::ok() : juce::Result::fail("Unable to finish the test signal.");
}
juce::var ResponseComparison::runChecks(const juce::File& folder) {
    const auto reference = folder.getChildFile("comparison-reference.wav");
    if (createTestSignal(reference).failed()) throw std::runtime_error("Test WAV export failed");
    ComparisonReport report; const auto result = analyze(reference, reference, report);
    if (result.failed() || report.maximumDifference > 1e-8) throw std::runtime_error("Identical recordings do not compare flat");
    if (report.writeCsv(folder.getChildFile("comparison.csv")).failed()) throw std::runtime_error("Comparison CSV export failed");
    // Known response fixtures independently exercise level normalization,
    // frequency-dependent gain and preservation of stereo balance.
    juce::WavAudioFormat wav;
    auto input = reference.createInputStream();
    std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(input.release(), true));
    if (!reader) throw std::runtime_error("Comparison fixture could not be read");
    juce::AudioBuffer<float> source(2, static_cast<int>(reader->lengthInSamples));
    if (!reader->read(&source, 0, source.getNumSamples(), 0, true, true)) throw std::runtime_error("Comparison fixture read failed");
    auto writeFixture = [&](const juce::File& file, const juce::AudioBuffer<float>& data) {
        auto fileStream = file.createOutputStream();
        if (fileStream) { fileStream->setPosition(0); fileStream->truncate(); }
        std::unique_ptr<juce::OutputStream> stream = std::move(fileStream);
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2)
            .withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (!writer || !writer->writeFromAudioSampleBuffer(data, 0, data.getNumSamples())) throw std::runtime_error("Comparison fixture write failed");
    };
    juce::AudioBuffer<float> quieter(source); quieter.applyGain(.5f);
    const auto gainFile = folder.getChildFile("comparison-gain.wav"); writeFixture(gainFile, quieter);
    ComparisonReport gain;
    if (analyze(reference, gainFile, gain).failed() || gain.maximumDifference > 1e-4 || std::abs(gain.levelOffset + 6.020599913) > 1e-4)
        throw std::runtime_error("Global gain was mistaken for an EQ difference");
    juce::AudioBuffer<float> filtered(source);
    const double w = juce::MathConstants<double>::twoPi * 1000 / 48000, a = std::pow(10., 6. / 40), alpha = std::sin(w) / 2;
    const double a0 = 1 + alpha / a, b0 = (1 + alpha * a) / a0, b1 = -2 * std::cos(w) / a0,
        b2 = (1 - alpha * a) / a0, a1 = b1, a2 = (1 - alpha / a) / a0;
    for (int c = 0; c < 2; ++c) {
        double z1 = 0, z2 = 0;
        for (int i = 0; i < filtered.getNumSamples(); ++i) {
            const double sample = filtered.getSample(c, i), output = b0 * sample + z1;
            z1 = b1 * sample - a1 * output + z2; z2 = b2 * sample - a2 * output;
            filtered.setSample(c, i, static_cast<float>(output * (c == 1 ? std::pow(10., 2. / 20) : 1.)));
        }
    }
    const auto filterFile = folder.getChildFile("comparison-bell-balance.wav"); writeFixture(filterFile, filtered);
    ComparisonReport bell;
    if (analyze(reference, filterFile, bell).failed()) throw std::runtime_error("Known bell response could not be compared");
    auto nearest = [&](double frequency) -> const ComparisonPoint& {
        return *std::min_element(bell.points.begin(), bell.points.end(), [=](const auto& x, const auto& y) {
            return std::abs(std::log(x.frequency / frequency)) < std::abs(std::log(y.frequency / frequency)); });
    };
    const auto& centre = nearest(1000); const auto& low = nearest(100);
    if (std::abs((centre.left - low.left) - 6) > .25 || std::abs((centre.right - centre.left) - 2) > .02)
        throw std::runtime_error("Comparison missed a known EQ response or stereo imbalance");
    if (bell.writeCsv(folder.getChildFile("comparison-known-response.csv")).failed()) throw std::runtime_error("Known comparison CSV export failed");
    ComparisonReport preserved = report;
    if (analyze(folder.getChildFile("missing.wav"), reference, report).wasOk() || report.points.size() != preserved.points.size())
        throw std::runtime_error("Failed comparison replaced the previous result");
    return fields({{"passed", true}, {"frequency_bands", static_cast<int>(report.points.size())}, {"identical_recordings_flat", true},
        {"global_gain_removed", true}, {"known_bell_detected", true}, {"stereo_balance_preserved", true}, {"no_playback_stream", true}});
}
struct ComparisonPanel::Job : juce::Thread {
    Job(juce::File a, juce::File b) : Thread("Soundee recording comparison"), reference(a), soundee(b) { startThread(); }
    ~Job() override { stopThread(10000); }
    void run() override { result = ResponseComparison::analyze(reference, soundee, report); }
    juce::File reference, soundee; ComparisonReport report; juce::Result result = juce::Result::ok();
};
ComparisonPanel::ComparisonPanel() {
    look.setColourScheme(juce::LookAndFeel_V4::getDarkColourScheme()); setLookAndFeel(&look);
    for (auto* component : std::initializer_list<juce::Component*> {&instructions, &result, &referenceButton, &soundeeButton, &generateButton, &exportButton}) addAndMakeVisible(component);
    instructions.setText("Play the same test WAV through SoundID and Soundee separately, with the other correction bypassed. Capture the same output point into two stereo WAVs, then import them here.\nOne global level difference is removed from both channels. The green/pale curves show EQ or channel balance differences. This tool does not play audio.", juce::dontSendNotification);
    instructions.setJustificationType(juce::Justification::topLeft);
    result.setText("Choose both recordings to compare. Capture the complete 10-second test.", juce::dontSendNotification);
    referenceButton.onClick = [this] { choose(true); }; soundeeButton.onClick = [this] { choose(false); };
    generateButton.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser>("Create comparison test WAV", juce::File(), "*.wav");
        auto safe = juce::Component::SafePointer<ComparisonPanel>(this);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe](const juce::FileChooser& dialog) { if (!safe || dialog.getResult() == juce::File()) return;
                const auto written = ResponseComparison::createTestSignal(dialog.getResult());
                safe->result.setText(written.wasOk() ? "Test WAV created. Play it in another application and capture each correction separately." : written.getErrorMessage(), juce::dontSendNotification); });
    };
    exportButton.setEnabled(false); exportButton.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser>("Export comparison CSV", juce::File(), "*.csv"); auto safe = juce::Component::SafePointer<ComparisonPanel>(this);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe](const juce::FileChooser& dialog) { if (safe && dialog.getResult() != juce::File()) { const auto written = safe->report.writeCsv(dialog.getResult());
                safe->result.setText(written.wasOk() ? "Comparison CSV exported." : written.getErrorMessage(), juce::dontSendNotification); } });
    };
    setSize(850, 570); startTimerHz(5);
}
ComparisonPanel::~ComparisonPanel() { stopTimer(); job.reset(); setLookAndFeel(nullptr); }
void ComparisonPanel::choose(bool reference) {
    chooser = std::make_unique<juce::FileChooser>(reference ? "SoundID reference recording" : "Soundee recording", juce::File(), "*.wav");
    auto safe = juce::Component::SafePointer<ComparisonPanel>(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe, reference](const juce::FileChooser& dialog) { if (!safe || dialog.getResult() == juce::File()) return;
            (reference ? safe->referenceFile : safe->soundeeFile) = dialog.getResult(); safe->analyze(); });
}
void ComparisonPanel::analyze() {
    if (referenceFile == juce::File() || soundeeFile == juce::File()) { result.setText("Choose the other recording to continue.", juce::dontSendNotification); return; }
    job = std::make_unique<Job>(referenceFile, soundeeFile); result.setText("Analyzing recordings...", juce::dontSendNotification);
    referenceButton.setEnabled(false); soundeeButton.setEnabled(false); exportButton.setEnabled(false);
}
void ComparisonPanel::timerCallback() {
    if (!job || job->isThreadRunning()) return;
    if (job->result.wasOk()) { report = std::move(job->report); result.setText(report.description, juce::dontSendNotification); }
    else result.setText(job->result.getErrorMessage(), juce::dontSendNotification);
    job.reset(); referenceButton.setEnabled(true); soundeeButton.setEnabled(true); exportButton.setEnabled(!report.points.empty()); repaint();
}
void ComparisonPanel::resized() {
    instructions.setBounds(20, 15, getWidth() - 40, 90); generateButton.setBounds(20, 115, 145, 30);
    referenceButton.setBounds(177, 115, 160, 30); soundeeButton.setBounds(349, 115, 160, 30); exportButton.setBounds(521, 115, 230, 30);
    result.setBounds(20, getHeight() - 53, getWidth() - 40, 37);
}
void ComparisonPanel::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff070907)); const juce::Rectangle<float> plot {52.f, 172.f, static_cast<float>(getWidth() - 83), static_cast<float>(getHeight() - 246)};
    auto x = [&](double f) { return plot.getX() + static_cast<float>(std::log(f / 30) / std::log(16000. / 30)) * plot.getWidth(); };
    auto y = [&](double db) { return plot.getCentreY() - static_cast<float>(db / 24) * plot.getHeight(); };
    for (int db = -12; db <= 12; db += 3) { g.setColour(juce::Colour(db == 0 ? 0xff929b92 : 0xff343b34)); g.drawHorizontalLine(static_cast<int>(y(db)), plot.getX(), plot.getRight());
        g.setColour(juce::Colour(0xffa2aaa2)); g.drawText(juce::String(db), 10, static_cast<int>(y(db)) - 8, 35, 17, juce::Justification::right); }
    for (double f : {30., 100., 1000., 10000.}) { g.setColour(juce::Colour(0xff343b34)); g.drawVerticalLine(static_cast<int>(x(f)), plot.getY(), plot.getBottom());
        g.setColour(juce::Colour(0xffa2aaa2)); g.drawText(juce::String(f, 0) + " Hz", static_cast<int>(x(f)) - 20, static_cast<int>(plot.getBottom()) + 5, 70, 20, juce::Justification::left); }
    g.saveState(); g.reduceClipRegion(plot.toNearestInt());
    for (int c = 0; c < 2; ++c) { juce::Path path; bool first = true;
        for (const auto& point : report.points) { const auto py = y(c == 0 ? point.left : point.right); if (first) path.startNewSubPath(x(point.frequency), py); else path.lineTo(x(point.frequency), py); first = false; }
        g.setColour(juce::Colour(c == 0 ? 0xff75e355 : 0xffb8e7a5)); g.strokePath(path, juce::PathStrokeType(2)); }
    g.restoreState();
}
}

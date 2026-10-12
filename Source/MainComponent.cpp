#include "MainComponent.h"
#include "Arranger/MidiStyleConverter.h"
#include "Arranger/StyleLoader.h"
#include "Arranger/SongLoader.h"
#include "Audio/ChordAnalysis.h"
#include "Audio/MidiChannel.h"
#include "Midi/LiveMelodyVoice.h"
#include "Midi/GmInstruments.h"
#include "Midi/ChordRecognizer.h"
#include "Arranger/OtsRecall.h"
#include "Arranger/SectionButtons.h"
#include "Arranger/SectionFlow.h"
#include "UI/CadenzaLookAndFeel.h"
#include "UI/NativePanel.h"
#include "UI/StylePartEditor.h"
#include "UI/FirstLaunchAssistant.h"
#include "Ai/StyleGenerator.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
// Defined further down (next to the MIDI settings UI); declared here so the
// constructor can restore the saved chord-detection mode.
arranger::ChordDetectionMode chordModeFromId(const std::string& id) noexcept;

// User "Imported" style library: %APPDATA%/Cadenza/imported-styles, organized in
// genre subfolders. Bundled "Factory" styles live in resources/factory/styles;
// this is the separate, user-side library (Genos imports, etc.).
juce::File importedStylesDir()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Cadenza")
        .getChildFile("imported-styles");
}

const char* midiImportRootName(int root) noexcept
{
    static const char* names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    return names[((root % 12) + 12) % 12];
}

int midiImportRootIndex(const juce::String& name) noexcept
{
    for (int i = 0; i < 12; ++i)
        if (name == midiImportRootName(i))
            return i;
    if (name == "Eb") return 3;
    if (name == "Bb") return 10;
    return 0;
}

juce::String midiImportQualityLabel(const juce::String& suffix)
{
    if (suffix == "m") return "min";
    if (suffix == "Maj7" || suffix == "maj7") return "maj7";
    if (suffix == "m7") return "min7";
    if (suffix == "7") return "7";
    if (suffix == "sus4") return "sus4";
    if (suffix == "dim") return "dim";
    if (suffix == "aug") return "aug";
    return "maj";
}

juce::String midiImportQualitySuffix(const juce::String& label)
{
    if (label == "min") return "m";
    if (label == "maj7") return "Maj7";
    if (label == "min7") return "m7";
    if (label == "7") return "7";
    if (label == "sus4") return "sus4";
    if (label == "dim") return "dim";
    if (label == "aug") return "aug";
    return {};
}

int midiImportQualityId(const juce::String& label) noexcept
{
    if (label == "min") return 2;
    if (label == "7") return 3;
    if (label == "maj7") return 4;
    if (label == "min7") return 5;
    if (label == "sus4") return 6;
    if (label == "dim") return 7;
    if (label == "aug") return 8;
    return 1;
}

juce::String midiImportConfidenceLabel(cadenza::arranger::MidiStyleChordConfidence confidence)
{
    switch (confidence) {
        case cadenza::arranger::MidiStyleChordConfidence::High: return "High";
        case cadenza::arranger::MidiStyleChordConfidence::Medium: return "Medium";
        case cadenza::arranger::MidiStyleChordConfidence::Low: return "Low - please check";
    }
    return "Low - please check";
}

cadenza::midi::ChordQuality midiImportQualityForSuffix(const juce::String& suffix) noexcept
{
    if (suffix == "m") return cadenza::midi::ChordQuality::Minor;
    if (suffix == "7") return cadenza::midi::ChordQuality::Dominant7;
    if (suffix == "Maj7" || suffix == "maj7") return cadenza::midi::ChordQuality::Major7;
    if (suffix == "m7") return cadenza::midi::ChordQuality::Minor7;
    if (suffix == "sus4") return cadenza::midi::ChordQuality::Sus4;
    if (suffix == "dim") return cadenza::midi::ChordQuality::Diminished;
    if (suffix == "aug") return cadenza::midi::ChordQuality::Augmented;
    return cadenza::midi::ChordQuality::Major;
}

int keyNameToPitchClass(const std::string& key) noexcept
{
    static const std::string names[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    for (int i = 0; i < 12; ++i)
        if (names[i] == key)
            return i;
    if (key == "Eb") return 3;
    if (key == "Bb") return 10;
    return 0;
}

class MidiStyleImportDialog final : public juce::Component
{
public:
    using ConvertCallback = std::function<void(std::vector<cadenza::arranger::MidiStyleSectionSpec>, bool, bool)>;
    using PreviewCallback = std::function<bool(cadenza::arranger::MidiStyleConvertOptions,
                                               cadenza::midi::Chord)>;
    using StopPreviewCallback = std::function<void()>;

    MidiStyleImportDialog(juce::File midiFile,
                          cadenza::arranger::MidiStyleImportInfo initialInfo,
                          ConvertCallback callback,
                          PreviewCallback previewCallback,
                          StopPreviewCallback stopPreviewCallback)
        : m_midiFile(std::move(midiFile)),
          m_info(std::move(initialInfo)),
          m_onConvert(std::move(callback)),
          m_onPreview(std::move(previewCallback)),
          m_onStopPreview(std::move(stopPreviewCallback))
    {
        addAndMakeVisible(m_fileHeading);
        addAndMakeVisible(m_summary);
        addAndMakeVisible(m_detected);
        addAndMakeVisible(m_chordWarning);
        addAndMakeVisible(m_normalizeHint);
        addAndMakeVisible(m_startLabel);
        addAndMakeVisible(m_countLabel);
        addAndMakeVisible(m_rootLabel);
        addAndMakeVisible(m_qualityLabel);
        addAndMakeVisible(m_startBar);
        addAndMakeVisible(m_barCount);
        addAndMakeVisible(m_root);
        addAndMakeVisible(m_quality);
        addAndMakeVisible(m_slotLabel);
        addAndMakeVisible(m_sectionSlot);
        addAndMakeVisible(m_addToStyle);
        addAndMakeVisible(m_autoSplit);
        addAndMakeVisible(m_sectionsLabel);
        addAndMakeVisible(m_capturedSummary);
        addAndMakeVisible(m_capturedSections);
        addAndMakeVisible(m_removeSection);
        addAndMakeVisible(m_normalizeToC);
        addAndMakeVisible(m_aiAddFillsAfterConvert);
        addAndMakeVisible(m_preview);
        addAndMakeVisible(m_prevSection);
        addAndMakeVisible(m_nextSection);
        addAndMakeVisible(m_detect);
        addAndMakeVisible(m_convert);
        addAndMakeVisible(m_cancel);

        m_fileHeading.setText("Importing: " + m_midiFile.getFileName(), juce::dontSendNotification);
        m_startLabel.setText("Start bar", juce::dontSendNotification);
        m_countLabel.setText("Bars", juce::dontSendNotification);
        m_rootLabel.setText("Root", juce::dontSendNotification);
        m_qualityLabel.setText("Quality", juce::dontSendNotification);
        m_slotLabel.setText("Section", juce::dontSendNotification);

        m_startBar.setSliderStyle(juce::Slider::IncDecButtons);
        m_startBar.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 64, 22);
        m_startBar.setNumDecimalPlacesToDisplay(0);
        m_barCount.setSliderStyle(juce::Slider::IncDecButtons);
        m_barCount.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 64, 22);
        m_barCount.setNumDecimalPlacesToDisplay(0);

        for (int i = 0; i < 12; ++i)
            m_root.addItem(midiImportRootName(i), i + 1);
        const char* qualities[] = { "maj", "min", "7", "maj7", "min7", "sus4", "dim", "aug" };
        for (int i = 0; i < 8; ++i)
            m_quality.addItem(qualities[i], i + 1);
        m_sectionSlot.addItem("Intro", 1);
        m_sectionSlot.addItem("Main A", 2);
        m_sectionSlot.addItem("Main B", 3);
        m_sectionSlot.addItem("Main C", 4);
        m_sectionSlot.addItem("Main D", 5);
        m_sectionSlot.addItem("Ending", 6);
        m_sectionSlot.addItem("Fill AA", 7);
        m_sectionSlot.addItem("Fill AB", 8);
        m_sectionSlot.addItem("Fill BA", 9);
        m_sectionSlot.addItem("Fill BB", 10);
        m_sectionSlot.setSelectedId(2, juce::dontSendNotification);

        m_detect.setButtonText("Detect");
        m_addToStyle.setButtonText("Add to style");
        m_autoSplit.setButtonText("Auto-split into sections");
        m_removeSection.setButtonText("Remove");
        m_sectionsLabel.setText("Style sections", juce::dontSendNotification);
        m_chordWarning.setVisible(false);
        m_capturedSummary.setReadOnly(true);
        m_capturedSummary.setMultiLine(true);
        m_capturedSummary.setScrollbarsShown(true);
        m_capturedSummary.setCaretVisible(false);
        m_preview.setButtonText("Preview");
        m_prevSection.setButtonText("Prev");
        m_nextSection.setButtonText("Next");
        m_convert.setButtonText("Convert");
        m_cancel.setButtonText("Cancel");
        m_normalizeToC.setButtonText("Play in C (easy white-key chords)");
        m_normalizeToC.setToggleState(true, juce::dontSendNotification);
        m_aiAddFillsAfterConvert.setButtonText("AI: add fills + intro/ending after convert");
        m_aiAddFillsAfterConvert.setToggleState(false, juce::dontSendNotification);

        applyThemeColours();

        m_detect.onClick = [this] { refreshDetection(); };
        m_preview.onClick = [this] { togglePreview(); };
        m_prevSection.onClick = [this] { stepSection(-1); };
        m_nextSection.onClick = [this] { stepSection(1); };
        m_root.onChange = [this] { updateNormalizeHint(); };
        m_quality.onChange = [this] { updateNormalizeHint(); };
        m_normalizeToC.onClick = [this] { updateNormalizeHint(); };
        m_addToStyle.onClick = [this] { addCurrentRangeToStyle(); };
        m_autoSplit.onClick = [this] { autoSplitIntoSections(); };
        m_removeSection.onClick = [this] { removeSelectedCapturedSection(); };
        m_convert.onClick = [this] {
            stopPreview();
            if (m_onConvert) {
                auto sections = m_capturedSpecs;
                if (sections.empty()) {
                    auto fallback = currentSectionSpec();
                    fallback.sectionId = "mainA";
                    sections.push_back(std::move(fallback));
                }
                m_onConvert(std::move(sections),
                            m_normalizeToC.getToggleState(),
                            m_aiAddFillsAfterConvert.getToggleState());
            }
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState(1);
        };
        m_cancel.onClick = [this] {
            stopPreview();
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState(0);
        };

        configureRangeControls();
        refreshDetection();
        refreshCapturedSectionList();
        setSize(680, 592);
    }

    ~MidiStyleImportDialog() override
    {
        stopPreview();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(cadenza::ui::CadenzaLookAndFeel::panel());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(16);
        m_fileHeading.setBounds(r.removeFromTop(26));
        m_summary.setBounds(r.removeFromTop(34));
        m_detected.setBounds(r.removeFromTop(28));
        if (m_chordWarning.isVisible())
            m_chordWarning.setBounds(r.removeFromTop(24));
        r.removeFromTop(8);

        auto row = r.removeFromTop(34);
        m_startLabel.setBounds(row.removeFromLeft(96));
        m_startBar.setBounds(row.removeFromLeft(120));
        row.removeFromLeft(16);
        m_countLabel.setBounds(row.removeFromLeft(48));
        m_barCount.setBounds(row.removeFromLeft(120));

        r.removeFromTop(12);
        row = r.removeFromTop(34);
        m_rootLabel.setBounds(row.removeFromLeft(96));
        m_root.setBounds(row.removeFromLeft(120));
        row.removeFromLeft(16);
        m_qualityLabel.setBounds(row.removeFromLeft(64));
        m_quality.setBounds(row.removeFromLeft(120));

        r.removeFromTop(10);
        row = r.removeFromTop(34);
        m_slotLabel.setBounds(row.removeFromLeft(96));
        m_sectionSlot.setBounds(row.removeFromLeft(140));
        row.removeFromLeft(12);
        m_addToStyle.setBounds(row.removeFromLeft(122));
        row.removeFromLeft(10);
        m_autoSplit.setBounds(row.removeFromLeft(188));

        r.removeFromTop(8);
        m_sectionsLabel.setBounds(r.removeFromTop(22));
        m_capturedSummary.setBounds(r.removeFromTop(58));
        r.removeFromTop(6);
        row = r.removeFromTop(32);
        m_capturedSections.setBounds(row.removeFromLeft(430));
        row.removeFromLeft(10);
        m_removeSection.setBounds(row.removeFromLeft(96));

        r.removeFromTop(10);
        m_normalizeToC.setBounds(r.removeFromTop(28));
        m_aiAddFillsAfterConvert.setBounds(r.removeFromTop(28));
        m_normalizeHint.setBounds(r.removeFromTop(34));

        r.removeFromTop(8);
        row = r.removeFromTop(32);
        m_preview.setBounds(row.removeFromLeft(104));
        row.removeFromLeft(10);
        m_prevSection.setBounds(row.removeFromLeft(72));
        row.removeFromLeft(8);
        m_nextSection.setBounds(row.removeFromLeft(72));

        r.removeFromTop(10);
        row = r.removeFromTop(32);
        m_detect.setBounds(row.removeFromLeft(92));
        row.removeFromRight(8);
        m_cancel.setBounds(row.removeFromRight(92));
        row.removeFromRight(8);
        m_convert.setBounds(row.removeFromRight(92));
    }

private:
    void applyThemeColours()
    {
        const auto text = cadenza::ui::CadenzaLookAndFeel::textMain();
        const auto value = cadenza::ui::CadenzaLookAndFeel::cream();
        const auto accent = cadenza::ui::CadenzaLookAndFeel::accent();
        const auto dim = cadenza::ui::CadenzaLookAndFeel::textDim();

        m_fileHeading.setColour(juce::Label::textColourId, value);
        m_summary.setColour(juce::Label::textColourId, value);
        m_detected.setColour(juce::Label::textColourId, accent);
        m_chordWarning.setColour(juce::Label::textColourId,
                                 cadenza::ui::CadenzaLookAndFeel::led().withMultipliedAlpha(0.92f));
        m_normalizeHint.setColour(juce::Label::textColourId, dim);
        m_startLabel.setColour(juce::Label::textColourId, text);
        m_countLabel.setColour(juce::Label::textColourId, text);
        m_rootLabel.setColour(juce::Label::textColourId, text);
        m_qualityLabel.setColour(juce::Label::textColourId, text);
        m_slotLabel.setColour(juce::Label::textColourId, text);
        m_sectionsLabel.setColour(juce::Label::textColourId, text);
        m_capturedSummary.setColour(juce::TextEditor::backgroundColourId,
                                    cadenza::ui::CadenzaLookAndFeel::panelRaised());
        m_capturedSummary.setColour(juce::TextEditor::textColourId, value);
        m_capturedSummary.setColour(juce::TextEditor::outlineColourId, dim.withAlpha(0.35f));
        m_normalizeToC.setColour(juce::ToggleButton::textColourId, text);
        m_aiAddFillsAfterConvert.setColour(juce::ToggleButton::textColourId, text);
    }

    void configureRangeControls()
    {
        const int total = std::max(1, m_info.totalBars);
        const int recommendedStart = std::clamp(m_info.recommendedRange.barStart + 1, 1, total);
        const int recommendedCount = std::clamp(
            std::max(1, m_info.recommendedRange.barCount), 1, std::max(1, total - recommendedStart + 1));
        m_startBar.setRange(1.0, static_cast<double>(total), 1.0);
        m_startBar.setValue(static_cast<double>(recommendedStart), juce::dontSendNotification);
        m_barCount.setRange(1.0, static_cast<double>(total), 1.0);
        m_barCount.setValue(static_cast<double>(recommendedCount), juce::dontSendNotification);
        m_startBar.onValueChange = [this] { handleRangeChanged(); };
        m_barCount.onValueChange = [this] { handleRangeChanged(); };
    }

    void clampBarCountToAvailable()
    {
        const int total = std::max(1, m_info.totalBars);
        const int start = std::clamp(juce::roundToInt(m_startBar.getValue()), 1, total);
        const int maxCount = std::max(1, total - start + 1);
        if (juce::roundToInt(m_startBar.getValue()) != start)
            m_startBar.setValue(start, juce::dontSendNotification);
        m_barCount.setRange(1.0, static_cast<double>(maxCount), 1.0);
        if (juce::roundToInt(m_barCount.getValue()) > maxCount)
            m_barCount.setValue(maxCount, juce::dontSendNotification);
    }

    void handleRangeChanged()
    {
        clampBarCountToAvailable();
        refreshDetection();
        if (m_previewing)
            startPreview();
    }

    void refreshDetection()
    {
        clampBarCountToAvailable();
        m_info = cadenza::arranger::inspectMidiFileForStyleImport(
            m_midiFile,
            juce::roundToInt(m_startBar.getValue()) - 1,
            juce::roundToInt(m_barCount.getValue()));
        applyInfoToLabels(true);
        updateStepButtons();
    }

    void applyInfoToLabels(bool updateSelectors)
    {
        m_summary.setText(
            "Tempo " + juce::String(m_info.tempo)
            + " BPM, " + juce::String(m_info.beatsPerBar) + "/"
            + juce::String(m_info.beatUnit)
            + ", " + juce::String(std::max(1, m_info.totalBars)) + " bars available",
            juce::dontSendNotification);

        const auto root = m_info.detectedChord.rootName.isNotEmpty()
            ? m_info.detectedChord.rootName
            : juce::String(midiImportRootName(m_info.detectedChord.root));
        const auto quality = midiImportQualityLabel(m_info.detectedChord.chordSuffix);
        const auto confidence = midiImportConfidenceLabel(m_info.detectedChord.confidence);
        m_detected.setText(
            juce::String("Detected source chord: ") + root + " " + quality
            + "  (confidence: " + confidence + ")"
            + (m_info.detectedChord.fallback ? " (fallback)" : ""),
            juce::dontSendNotification);
        const auto detectedColour =
            m_info.detectedChord.confidence == cadenza::arranger::MidiStyleChordConfidence::Low
                ? cadenza::ui::CadenzaLookAndFeel::led().withMultipliedAlpha(0.88f)
                : m_info.detectedChord.confidence == cadenza::arranger::MidiStyleChordConfidence::Medium
                    ? cadenza::ui::CadenzaLookAndFeel::goldBright()
                    : cadenza::ui::CadenzaLookAndFeel::accent();
        m_detected.setColour(juce::Label::textColourId, detectedColour);
        m_chordWarning.setText(
            m_info.rangeChangesChord
                ? "WARNING: range changes chords - use ONE chord per style section. Try fewer bars (1-2)."
                : juce::String(),
            juce::dontSendNotification);
        m_chordWarning.setVisible(m_info.rangeChangesChord);
        resized();

        if (updateSelectors) {
            m_root.setSelectedId(midiImportRootIndex(root) + 1, juce::dontSendNotification);
            m_quality.setSelectedId(midiImportQualityId(quality), juce::dontSendNotification);
        }
        updateNormalizeHint();
    }

    cadenza::arranger::MidiStyleConvertOptions currentConvertOptions(bool normalizeToC) const
    {
        cadenza::arranger::MidiStyleConvertOptions options;
        options.barStart = juce::roundToInt(m_startBar.getValue()) - 1;
        options.barCount = juce::roundToInt(m_barCount.getValue());
        options.sectionName = currentSectionId();
        options.overrideSourceRoot = std::max(0, m_root.getSelectedId() - 1);
        options.overrideSourceChord = midiImportQualitySuffix(m_quality.getText());
        options.normalizeToC = normalizeToC;
        return options;
    }

    juce::String currentSectionId() const
    {
        switch (m_sectionSlot.getSelectedId()) {
            case 1: return "intro";
            case 3: return "mainB";
            case 4: return "mainC";
            case 5: return "mainD";
            case 6: return "ending";
            case 7: return "fillAA";
            case 8: return "fillAB";
            case 9: return "fillBA";
            case 10: return "fillBB";
            case 2:
            default: return "mainA";
        }
    }

    cadenza::arranger::MidiStyleSectionSpec currentSectionSpec() const
    {
        cadenza::arranger::MidiStyleSectionSpec spec;
        spec.sectionId = currentSectionId();
        spec.barStart = juce::roundToInt(m_startBar.getValue()) - 1;
        spec.barCount = juce::roundToInt(m_barCount.getValue());
        spec.overrideRoot = m_root.getText();
        spec.overrideQuality = m_quality.getText();
        return spec;
    }

    juce::String capturedSectionText(const cadenza::arranger::MidiStyleSectionSpec& spec) const
    {
        const int start = spec.barStart + 1;
        const int end = spec.barStart + std::max(1, spec.barCount);
        const auto rangeText = spec.barCount == 1
            ? "bar " + juce::String(start)
            : "bars " + juce::String(start) + "-" + juce::String(end);
        auto text = sectionLabelForSpec(spec.sectionId) + ": " + rangeText
            + "  " + spec.overrideRoot + " " + spec.overrideQuality;
        const auto hint = m_autoHints.find(spec.sectionId.toStdString());
        if (hint != m_autoHints.end())
            text += "  (auto, " + hint->second + ")";
        return text;
    }

    void refreshCapturedSectionList()
    {
        const auto selectedId = m_capturedSections.getSelectedId();
        m_capturedSections.clear(juce::dontSendNotification);
        for (int i = 0; i < static_cast<int>(m_capturedSpecs.size()); ++i)
            m_capturedSections.addItem(capturedSectionText(m_capturedSpecs[static_cast<std::size_t>(i)]), i + 1);
        juce::StringArray lines;
        for (const auto& spec : m_capturedSpecs)
            lines.add(capturedSectionText(spec));
        m_capturedSummary.setText(lines.isEmpty() ? "Current range will convert as Main A" : lines.joinIntoString("\n"),
                                  juce::dontSendNotification);
        if (!m_capturedSpecs.empty())
            m_capturedSections.setSelectedId(std::clamp(selectedId, 1, static_cast<int>(m_capturedSpecs.size())),
                                             juce::dontSendNotification);
        m_removeSection.setEnabled(!m_capturedSpecs.empty());
    }

    void addCurrentRangeToStyle()
    {
        auto spec = currentSectionSpec();
        const auto sectionId = spec.sectionId;
        m_autoHints.erase(sectionId.toStdString());
        auto it = std::find_if(m_capturedSpecs.begin(), m_capturedSpecs.end(),
                               [&](const auto& existing) { return existing.sectionId == sectionId; });
        if (it != m_capturedSpecs.end())
            *it = std::move(spec);
        else
            m_capturedSpecs.push_back(std::move(spec));
        refreshCapturedSectionList();
    }

    static juce::String sectionLabelForSpec(const juce::String& id)
    {
        if (id == "intro") return "Intro";
        if (id == "mainB") return "Main B";
        if (id == "mainC") return "Main C";
        if (id == "mainD") return "Main D";
        if (id.startsWith("fill") && id.length() >= 6)
            return "Fill " + id.substring(4, 6).toUpperCase();
        if (id == "ending") return "Ending";
        return "Main A";
    }

    static juce::String foundAtHintFor(const cadenza::arranger::MidiStyleSectionSpec& spec,
                                       const juce::StringArray& warnings)
    {
        const auto prefix = sectionLabelForSpec(spec.sectionId) + ": found at bars ";
        for (const auto& warning : warnings)
            if (warning.startsWith(prefix))
                return "found at bars " + warning.fromFirstOccurrenceOf(prefix, false, false).trim();
        const auto singlePrefix = sectionLabelForSpec(spec.sectionId) + ": found at bar ";
        for (const auto& warning : warnings)
            if (warning.startsWith(singlePrefix))
                return "found at bar " + warning.fromFirstOccurrenceOf(singlePrefix, false, false).trim();
        return "auto";
    }

    void applyAutoSplitResult(cadenza::arranger::MidiStyleAutoSplitResult result)
    {
        stopPreview();
        if (!result.ok || result.sections.empty()) {
            const auto message = result.warnings.isEmpty()
                ? juce::String("Auto-split could not find style sections")
                : result.warnings.joinIntoString("; ");
            m_capturedSummary.setText(message, juce::dontSendNotification);
            return;
        }

        m_capturedSpecs = std::move(result.sections);
        m_autoHints.clear();
        for (const auto& spec : m_capturedSpecs)
            m_autoHints[spec.sectionId.toStdString()] = foundAtHintFor(spec, result.warnings);
        refreshCapturedSectionList();
    }

    void runAutoSplitReplacingCurrent()
    {
        cadenza::arranger::MidiStyleAutoSplitOptions options;
        options.normalizeToC = m_normalizeToC.getToggleState();
        applyAutoSplitResult(cadenza::arranger::autoSplitMidiFileForStyleImport(m_midiFile, options));
    }

    void autoSplitIntoSections()
    {
        if (m_capturedSpecs.empty()) {
            runAutoSplitReplacingCurrent();
            return;
        }

        juce::Component::SafePointer<MidiStyleImportDialog> safe(this);
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::QuestionIcon,
            "Replace captured sections?",
            "Auto-split will replace the current captured section list.",
            "Replace",
            "Keep",
            this,
            juce::ModalCallbackFunction::create([safe](int result) {
                if (result != 0)
                    if (auto* dialog = safe.getComponent())
                        dialog->runAutoSplitReplacingCurrent();
            }));
    }

    void removeSelectedCapturedSection()
    {
        const int index = m_capturedSections.getSelectedId() - 1;
        if (index < 0 || index >= static_cast<int>(m_capturedSpecs.size()))
            return;
        m_autoHints.erase(m_capturedSpecs[static_cast<std::size_t>(index)].sectionId.toStdString());
        m_capturedSpecs.erase(m_capturedSpecs.begin() + index);
        refreshCapturedSectionList();
    }

    void updateNormalizeHint()
    {
        const auto root = m_root.getText().isNotEmpty()
            ? m_root.getText()
            : juce::String(midiImportRootName(std::max(0, m_root.getSelectedId() - 1)));
        const auto quality = m_quality.getText().isNotEmpty() ? m_quality.getText() : "maj";
        if (m_normalizeToC.getToggleState()) {
            m_normalizeHint.setText(
                "You'll play this style in C (easy). Use Transpose to reach the song's key (" + root + ").",
                juce::dontSendNotification);
        } else {
            m_normalizeHint.setText(
                "You'll play this style in the song's key: " + root + " " + quality + ".",
                juce::dontSendNotification);
        }
    }

    cadenza::midi::Chord selectedPreviewChord() const
    {
        cadenza::midi::Chord chord;
        chord.rootPitchClass = std::max(0, m_root.getSelectedId() - 1) % 12;
        chord.quality = midiImportQualityForSuffix(midiImportQualitySuffix(m_quality.getText()));
        return chord;
    }

    void togglePreview()
    {
        if (m_previewing)
            stopPreview();
        else
            startPreview();
    }

    void startPreview()
    {
        if (!m_onPreview)
            return;
        m_previewing = m_onPreview(currentConvertOptions(false), selectedPreviewChord());
        m_preview.setButtonText(m_previewing ? "Stop" : "Preview");
    }

    void stopPreview()
    {
        if (!m_previewing)
            return;
        if (m_onStopPreview)
            m_onStopPreview();
        m_previewing = false;
        m_preview.setButtonText("Preview");
    }

    void stepSection(int direction)
    {
        const int total = std::max(1, m_info.totalBars);
        const int count = std::max(1, juce::roundToInt(m_barCount.getValue()));
        const int current = std::clamp(juce::roundToInt(m_startBar.getValue()), 1, total);
        const int maxStart = std::max(1, total - count + 1);
        const int next = std::clamp(current + direction * count, 1, maxStart);
        if (next == current)
            return;
        m_startBar.setValue(next, juce::sendNotificationSync);
    }

    void updateStepButtons()
    {
        const int total = std::max(1, m_info.totalBars);
        const int count = std::max(1, juce::roundToInt(m_barCount.getValue()));
        const int current = std::clamp(juce::roundToInt(m_startBar.getValue()), 1, total);
        const int maxStart = std::max(1, total - count + 1);
        m_prevSection.setEnabled(current > 1);
        m_nextSection.setEnabled(current < maxStart);
    }

    juce::File m_midiFile;
    cadenza::arranger::MidiStyleImportInfo m_info;
    ConvertCallback m_onConvert;
    PreviewCallback m_onPreview;
    StopPreviewCallback m_onStopPreview;
    bool m_previewing = false;
    juce::Label m_fileHeading;
    juce::Label m_summary;
    juce::Label m_detected;
    juce::Label m_chordWarning;
    juce::Label m_normalizeHint;
    juce::Label m_startLabel;
    juce::Label m_countLabel;
    juce::Label m_rootLabel;
    juce::Label m_qualityLabel;
    juce::Label m_slotLabel;
    juce::Label m_sectionsLabel;
    juce::Slider m_startBar;
    juce::Slider m_barCount;
    juce::ComboBox m_root;
    juce::ComboBox m_quality;
    juce::ComboBox m_sectionSlot;
    juce::ComboBox m_capturedSections;
    juce::TextEditor m_capturedSummary;
    juce::ToggleButton m_normalizeToC;
    juce::ToggleButton m_aiAddFillsAfterConvert;
    juce::TextButton m_addToStyle;
    juce::TextButton m_autoSplit;
    juce::TextButton m_removeSection;
    juce::TextButton m_preview;
    juce::TextButton m_prevSection;
    juce::TextButton m_nextSection;
    juce::TextButton m_detect;
    juce::TextButton m_convert;
    juce::TextButton m_cancel;
    std::vector<cadenza::arranger::MidiStyleSectionSpec> m_capturedSpecs;
    std::map<std::string, juce::String> m_autoHints;
};

// Section to start a freshly-loaded style on. Yamaha styles run sparse (Main A)
// to full (Main D) by design, so loading on Main A makes a rich style sound
// empty when auditioning a library (e.g. a Bossa whose Main A is drums-only).
// Pick the fullest Main A..D variation so each style is heard at its best; the
// player can still step down to Main A for a sparser feel.
std::string bestStartSection(const cadenza::arranger::Style& style)
{
    const char* mains[] = { "mainA", "mainB", "mainC", "mainD" };
    const cadenza::arranger::Section* best = nullptr;
    for (const char* m : mains) {
        const auto* s = style.findSection(m);
        if (s != nullptr && (best == nullptr || s->parts.size() > best->parts.size()))
            best = s;
    }
    if (best != nullptr)
        return best->name;
    return style.sections.empty() ? std::string() : style.sections.front().name;
}

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool equalsIgnoreCase(const std::string& a, const std::string& b)
{
    return lowercase(a) == lowercase(b);
}

bool isSupportedStyleFile(const juce::File& file)
{
    // Cadenza's own JSON style, plus every Yamaha SFF container extension (all
    // share the same SMF+CASM payload): .sty user, .prs preset, .sst session,
    // .fps free play, .bcs. Genos/Genos 2 preset styles ship as .prs.
    const auto ext = lowercase(file.getFileExtension().toStdString());
    return ext == ".cstyle" || ext == ".sty" || ext == ".prs"
        || ext == ".sst" || ext == ".fps" || ext == ".bcs";
}

bool isSupportedAudioAnalysisFile(const juce::File& file)
{
    const auto ext = lowercase(file.getFileExtension().toStdString());
    return ext == ".wav" || ext == ".mp3" || ext == ".flac"
        || ext == ".ogg" || ext == ".m4a" || ext == ".aif" || ext == ".aiff"
        || ext == ".aac";
}

bool isSupportedSoundFontFile(const juce::File& file)
{
    const auto ext = lowercase(file.getFileExtension().toStdString());
    return ext == ".sf2" || ext == ".sf3";
}

bool isSupportedSongFile(const juce::File& file)
{
    return lowercase(file.getFileExtension().toStdString()) == ".csong";
}

bool isSupportedPluginFile(const juce::File& file)
{
    return lowercase(file.getFileExtension().toStdString()) == ".vst3";
}

juce::String jsString(const juce::String& value)
{
    return juce::JSON::toString(value);
}

juce::String boolLiteral(bool value)
{
    return value ? "true" : "false";
}

juce::String aiAddFillsPrompt()
{
    return R"PROMPT(Return ONLY a JSON object of NEW sections to ADD to this style:
{"sections":{"fillAA":{..},"fillBB":{..},"fillAB":{..},"intro":{..},"ending":{..}}}.
Include fillXX for each existing Main, transition fills fillAB/fillBA where both Mains exist, and intro/ending ONLY if missing.
Each section uses the same schema as the style's sections (barCount + parts + notes, drums absolute).
Do NOT include existing sections. Output JSON only.

FILL-WRITING:
- Timing is ticksPerBeat=960. In 4/4: bar=3840, beat=960, 8th=480, 16th=240.
- Drums must use role "absolute" and GM drum pitches. Reuse each Main section's drum kit/instrument/programs and the existing bass/harmony instruments.
- A fill is a 1-bar drum-led section that builds tension and announces the section change. It should clearly differ from the steady groove with more movement, crescendo, toms/snare rolls, or pickups, then resolve with CRASH 49 + KICK 36 on beat 1 (tick 0) so the new section lands hard.
- Build velocity through the bar from about 70/80 up to about 118. Ghost notes can be softer.
- Transition fills such as fillAB/fillBA may be slightly busier than self-fills such as fillAA/fillBB, but both should still feel genre-aware and playable.

FILL RECIPES (choose and adapt to the style):
- Rock/pop: stay steady through beats 1-2, then play a descending tom roll over beats 3-4 using toms 48,47,45,43,41 as 8ths/16ths, for example ticks 1920,2160,2400,2640,2880,3120,3360,3600, rising roughly velocity 80->116. Put CRASH 49 + KICK 36 at tick 0 as the resolving downbeat.
- Funk: busy 16th SNARE 38 with ghost notes around velocity 55 and accents around 110, syncopated kick, a couple of toms near the end, and crash on the resolving downbeat.
- Disco/dance: keep four-on-the-floor kick, add tom hits plus OPEN HAT 46 on the &s through beat 4, snare 16ths in beat 4, and crash on the resolving downbeat.
- Ballad: sparse. Use a soft tom or snare pickup on beat 4 around velocity 70-90 plus a cymbal swell/crash on the downbeat.

INTRO/ENDING:
- Intro should be a short 1-2 bar pickup/build using the style's kit plus a light bass/chord lead-in.
- Ending should be a final cadence: a fill into a strong downbeat hit with CRASH 49 + KICK 36 plus a sustained chord-root/chord-3/chord-5, then space.

WORKED EXAMPLE - sections-only output shape. Adapt this to the current style's tempo, genre, kit, and instruments; do not copy it verbatim if the style calls for a different feel:
{"sections":{"fillAA":{"barCount":1,"parts":[
  {"name":"drums","channel":10,"instrument":"Standard Kit","program":0,"notes":[
    {"tick":0,"duration":120,"pitch":49,"velocity":116,"role":"absolute"},
    {"tick":0,"duration":120,"pitch":36,"velocity":116,"role":"absolute"},
    {"tick":1920,"duration":120,"pitch":48,"velocity":92,"role":"absolute"},
    {"tick":2160,"duration":120,"pitch":47,"velocity":96,"role":"absolute"},
    {"tick":2400,"duration":120,"pitch":45,"velocity":100,"role":"absolute"},
    {"tick":2640,"duration":120,"pitch":43,"velocity":104,"role":"absolute"},
    {"tick":2880,"duration":120,"pitch":41,"velocity":110,"role":"absolute"},
    {"tick":3360,"duration":120,"pitch":38,"velocity":114,"role":"absolute"} ]}]}}})PROMPT";
}

juce::String aiPolishSectionPrompt(const std::string& sectionId)
{
    return juce::String("Polish ONLY section \"") + juce::String(sectionId) + "\" in the style reference below. "
           "Fix obvious out-of-key or stray notes and odd/misplaced drum hits in that one section. "
           "Keep the same section id, barCount, parts, instruments, channels, and automation; notes may change. "
           "Drums stay absolute with GM drum pitches. Return ONLY JSON shaped exactly like "
           "{\"sections\":{\"" + juce::String(sectionId) + "\":{...polished section...}}}. "
           "Do not include any other section, prose, markdown, or code fences.";
}

cadenza::arranger::Style oneSectionStyleForPrompt(const cadenza::arranger::Style& style,
                                                  const cadenza::arranger::Section& section)
{
    auto oneSection = style;
    oneSection.sections.clear();
    oneSection.sections.push_back(section);
    return oneSection;
}
}

MainComponent::MainComponent()
{
    setSize(1280, 800);

    // Load persisted settings BEFORE starting audio so saved BPM / device take effect.
    m_settings = std::make_unique<cadenza::settings::SettingsStore>(settingsFilePath());
    loadSettings();
    loadVoiceMap();

    // Start audio before installing hooks so noteOn arrivals have somewhere to go.
    // Restore the user's saved output-device choice (avoids a coloured virtual
    // device being silently re-selected each launch).
    std::unique_ptr<juce::XmlElement> audioState(
        juce::XmlDocument::parse(juce::File(audioStateFilePath())));
    m_audio.startAudioDevice(audioState.get());
    m_audio.deviceManager().addChangeListener(this);   // persist future changes
    m_audio.setBpm(static_cast<double>(m_state.bpm()));
    if (m_settings) {
        const auto& st = m_settings->state();
        m_audio.setEqGains(static_cast<float>(st.eqLowDb),
                           static_cast<float>(st.eqMidDb),
                           static_cast<float>(st.eqHighDb));
        m_audio.setCompAmount(st.compAmount);
        m_audio.setMasterVolume(st.masterVolume);
        m_audio.setReverbLevel(st.reverbLevel);
        m_audio.setPolishedMaster(st.polishedMaster);
        m_midi.setSplitPoint(st.splitNote);
        m_styleEngine.setHumanizeAmount(st.humanizeAmount);
        // Restore the saved MIDI input selection ("" = auto: main port, skip aux
        // ports) before opening any device, so we don't briefly open all ports.
        m_midi.setSelectedInput(juce::String(st.midiInputDevice));
    }
    juce::Logger::writeToLog("[Cadenza] Synth engine: " + juce::String(m_audio.synthEngineName()));
    if (!m_audio.supportsSoundFonts()) {
        juce::Logger::writeToLog("[Cadenza] WARNING: NullSynthEngine is active; SoundFont loading is unavailable and playback will be silent/log-only.");
    }

    // Wire the style engine into the audio thread's tick callback.
    m_styleEngine.install();

    // Sample-tight section sequencing fires these from the audio thread; marshal
    // to the message thread for UI + mixer re-assert + stop.
    m_styleEngine.setSectionChangedCallback([this](const std::string& name) {
        juce::Component::SafePointer<MainComponent> safe(this);
        const juce::String n(name);
        juce::MessageManager::callAsync([safe, n] {
            if (auto* self = safe.getComponent()) {
                self->applyMixerState();
                if (self->m_panel) self->m_panel->setActiveSection(n);
                self->applyOtsLinkForSection(n.toStdString());
            }
        });
    });
    m_styleEngine.setStopRequestedCallback([this] {
        juce::Component::SafePointer<MainComponent> safe(this);
        juce::MessageManager::callAsync([safe] {
            if (auto* self = safe.getComponent(); self && self->m_state.playing()) {
                self->m_state.setPlaying(false);
                if (self->m_panel) self->m_panel->setPlaying(false);
                self->pushToWeb("window.JuceBridge && window.JuceBridge.onPlayStateChanged(false);");
            }
        });
    });

    m_styleEngine.setGlobalTranspose(m_state.transpose());
    m_midi.setLiveOctave(m_state.octave());   // Octave affects live right-hand only, not style parts
    applyRuntimeStateToEngines();

    // Build the native control panel before loading content so style loads can
    // populate its section buttons. This panel — not the WebView — is the source
    // of truth for the core live-arranger controls.
    buildNativePanel();

    // Try optional factory content (fail silently if absent — no SF2 yet etc.).
    tryLoadFactorySoundFont();
    if (!tryLoadLastStyle())
        tryLoadFactoryStyle();

    // Assert the live melody voice program on its dedicated channel AFTER the
    // style's channel setup, so the right-hand voice isn't left on the style's
    // channel-1 program.
    applyRightHand();

    // Wire web bridge messages -> audio / style / midi.
    installBridgeHooks();

    // Connect hardware MIDI: notes through ChordRecognizer + AudioEngine + web piano.
    m_midi.setNoteCallback([this](int channel, int note, int velocity, bool isOn) {
        if (isOn) m_audio.noteOn(channel, note, velocity);
        else      m_audio.noteOff(channel, note);
        const auto js = juce::String("window.JuceBridge && window.JuceBridge.")
                       + (isOn ? "onNoteReceived(" : "onNoteOff(")
                       + juce::String(note) + ");";
        pushToWeb(js);
    });
    // Hardware pitch-bend / mod wheels -> the manual voice channels (Right + Left).
    m_midi.setPitchBendCallback([this](int v14) { sendPitchBendToManual(v14); });
    m_midi.setModulationCallback([this](int v)  { sendModToManual(v); });
    m_midi.setChordCallback([this](const std::optional<cadenza::midi::Chord>& chord,
                                   const std::string& chordName) {
        // 1) Push display name to the web UI (empty string when no chord held).
        const auto js = juce::String("window.JuceBridge && window.JuceBridge.onChordChanged(\"")
                      + juce::String(chordName) + "\");";
        pushToWeb(js);

        // 2) Feed the chord into the style engine so auto-accompaniment follows.
        if (chord) m_styleEngine.setChord(*chord);

        // 3) Update the native panel's chord display (marshalled to the UI thread).
        juce::Component::SafePointer<MainComponent> safe(this);
        const juce::String name(chordName);
        juce::MessageManager::callAsync([safe, name] {
            if (auto* p = safe.getComponent(); p && p->m_panel)
                p->m_panel->setChord(name);
        });
    });

    // Style Recorder capture (MIDI thread): audition the played note on the
    // target part's channel, and write it into the take while armed.
    m_midi.setCaptureCallback([this](int note, int velocity, bool isOn) {
        const auto& info = cadenza::arranger::recorderPartInfo(m_recorder.targetPart());
        if (isOn) m_audio.noteOn(info.midiChannel, note, velocity);
        else      m_audio.noteOff(info.midiChannel, note);

        if (m_recordArmed.load() && m_audio.transport().playing()) {
            const int tick = m_audio.transport().positionTickInt();
            if (isOn) m_recorder.noteOn(note, velocity, tick);
            else      m_recorder.noteOff(note, tick);
        }
    });

    // Syncro Start / Stop: when the first chord-zone note arrives, start playback;
    // when the last one is released, stop. This is what makes the "S.Start" UI pill
    // meaningful — playing a chord left-hand triggers the backing band automatically.
    m_midi.setSyncCallback([this](bool started) {
        if (started) {
            m_audio.play();
        } else {
            m_audio.stop();
        }
        m_state.setPlaying(started);
        // Mirror to the web UI so the Play button reflects reality.
        const auto js = juce::String("window.JuceBridge && window.JuceBridge.onPlayStateChanged(")
                      + (started ? "true" : "false") + ");";
        pushToWeb(js);
        // Update the native panel's Play/Stop button (marshalled to the UI thread).
        juce::Component::SafePointer<MainComponent> safe(this);
        juce::MessageManager::callAsync([safe, started] {
            if (auto* p = safe.getComponent(); p && p->m_panel)
                p->m_panel->setPlaying(started);
        });
    });

    // MIDI input debug observability — fires on every incoming MIDI message.
    // Logs to the debug output AND pushes the same info to the web UI's debug panel.
    m_midi.setDebugCallback([this](const cadenza::midi::MidiDebugEvent& ev) {
        // 1) Plain log (visible in DbgView or the VS Output window).
        juce::String line = juce::String::formatted(
            "[MIDI] dev=\"%s\" status=0x%02X note=%d vel=%d route=%s chord=%s sync=%s",
            ev.deviceName.c_str(),
            ev.status,
            ev.note,
            ev.velocity,
            ev.route.c_str(),
            ev.chordName.empty() ? "-" : ev.chordName.c_str(),
            ev.sync.c_str());
        juce::Logger::writeToLog(line);

        // 2) Push the same info into the web UI as a JS-safe object literal.
        auto esc = [](const std::string& s) {
            std::string out;
            out.reserve(s.size() + 2);
            for (char c : s) {
                if (c == '\\' || c == '"') out += '\\';
                if (c >= ' ' && static_cast<unsigned char>(c) < 127) out += c;
            }
            return out;
        };
        juce::String js = juce::String::formatted(
            "window.JuceBridge && window.JuceBridge.onMidiDebug && "
            "window.JuceBridge.onMidiDebug({"
            "device:\"%s\",status:%d,note:%d,velocity:%d,"
            "route:\"%s\",chord:\"%s\",sync:\"%s\"});",
            esc(ev.deviceName).c_str(),
            ev.status,
            ev.note,
            ev.velocity,
            esc(ev.route).c_str(),
            esc(ev.chordName).c_str(),
            esc(ev.sync).c_str());
        pushToWeb(js);
    });

    // MIDI control mapping: hardware buttons -> arranger commands (sections / play).
    if (m_settings)
        m_midi.setControlMap(m_settings->state().midiControlMap);
    m_midi.setControlCallback([this](const std::string& command) {
        juce::Component::SafePointer<MainComponent> safe(this);
        juce::MessageManager::callAsync([safe, command] {
            if (auto* self = safe.getComponent()) self->executeControlCommand(command);
        });
    });
    m_midi.setControlLearnCallback([this](int trigger) {
        juce::Component::SafePointer<MainComponent> safe(this);
        juce::MessageManager::callAsync([safe, trigger] {
            auto* self = safe.getComponent();
            if (!self || !self->m_settings || self->m_learnCommand.empty()) return;
            cadenza::midi::MidiControlMap map;
            map.setEntries(self->m_settings->state().midiControlMap);
            map.assign(trigger, self->m_learnCommand);          // one button per action
            self->m_settings->state().midiControlMap = map.entries();
            self->m_midi.setControlMap(map.entries());
            self->m_learnCommand.clear();
            self->saveSettings();   // the MIDI Settings window re-reads the map on its own timer
        });
    });

    m_midi.refreshInputs();   // open the selected / main MIDI input(s) (logs the device list)

    // Always-on timer: hot-plug MIDI rescan (~every 2s) + song-mode auto-stepping.
    startTimerHz(20);

    resized();

    // First-launch Quick Start: shown once, after the window is on screen, so a
    // new user isn't dropped straight into the dense arranger UI with no guidance.
    if (m_settings && !m_settings->state().setupAssistantSeen) {
        juce::Component::SafePointer<MainComponent> safe(this);
        juce::MessageManager::callAsync([safe] {
            if (auto* self = safe.getComponent()) self->showFirstLaunchAssistant();
        });
    }
}

MainComponent::~MainComponent()
{
    stopTimer();
    saveSettings();
    m_styleEngine.allNotesOff();
    m_audio.deviceManager().removeChangeListener(this);
    m_audio.stopAudioDevice();
    m_midi.closeInputs();
}

std::string MainComponent::settingsFilePath() const
{
    const auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Cadenza");
    if (!dir.isDirectory()) dir.createDirectory();
    return dir.getChildFile("settings.json").getFullPathName().toStdString();
}

std::string MainComponent::audioStateFilePath() const
{
    return juce::File(settingsFilePath()).getParentDirectory()
        .getChildFile("audio-device.xml").getFullPathName().toStdString();
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // The audio device changed (user picked a different output) — persist it.
    if (auto xml = m_audio.deviceManager().createStateXml())
        xml->writeTo(juce::File(audioStateFilePath()));
}

void MainComponent::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent(
        m_audio.deviceManager(),
        /*minInputCh*/  0, /*maxInputCh*/  0,
        /*minOutputCh*/ 1, /*maxOutputCh*/ 2,
        /*showMidiIn*/  false, /*showMidiOut*/ false,
        /*stereoPairs*/ true,  /*hideAdvanced*/ false);
    selector->setSize(520, 360);

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(selector);
    opts.dialogTitle = "Audio Output Settings";
    opts.dialogBackgroundColour = juce::Colour(0xff2a2f3a);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = false;
    opts.launchAsync();
}

namespace
{
// A DialogWindow whose title-bar close (X) routes to a callback, so closing the
// Quick Start panel marks it "seen" exactly like the "Get Started" button does.
class FirstLaunchWindow final : public juce::DialogWindow
{
public:
    explicit FirstLaunchWindow(std::function<void()> onClose)
        : juce::DialogWindow("Welcome to Cadenza", juce::Colour(0xff20242e),
                             /*escapeCloses*/ true, /*nativeTitleBar*/ true),
          m_onClose(std::move(onClose)) {}

    void closeButtonPressed() override { if (m_onClose) m_onClose(); }

private:
    std::function<void()> m_onClose;
};
}

void MainComponent::showFirstLaunchAssistant()
{
    if (m_firstLaunchWindow != nullptr)
        return;   // already open

    // Live status provider — the assistant polls this so its dots update as the
    // user fixes things (loads a SoundFont, picks an audio device, etc.).
    auto provider = [this]() {
        cadenza::ui::FirstLaunchInputs in;
        auto* dev = m_audio.deviceManager().getCurrentAudioDevice();
        in.audioReady      = dev != nullptr;
        in.audioDeviceName = dev ? dev->getName().toStdString() : std::string();
        in.synthAvailable  = m_audio.supportsSoundFonts();

        const auto sfPath = m_settings ? m_settings->state().lastSoundFontPath : std::string();
        in.soundFontLoaded = m_audio.supportsSoundFonts() && !sfPath.empty();
        in.soundFontName   = sfPath.empty()
            ? std::string()
            : juce::File(juce::String(sfPath)).getFileName().toStdString();

        const auto style = m_styleEngine.currentStyle();
        in.styleLoaded = style != nullptr;
        in.styleName   = style ? style->name : std::string();

        const auto midis = juce::MidiInput::getAvailableDevices();
        in.midiConnected   = !midis.isEmpty();
        in.midiDeviceName  = midis.isEmpty() ? std::string() : midis[0].name.toStdString();
        return in;
    };

    cadenza::ui::FirstLaunchAssistant::Callbacks cb;
    cb.openAudioSettings = [this] { showAudioSettings(); };
    cb.chooseSoundFont   = [this] { openSoundFontFileChooser(); };
    cb.loadStyle         = [this] { openStyleFileChooser(); };
    cb.testSound         = [this] { playTestChord(); };
    cb.requestClose      = [this] { closeFirstLaunchAssistant(); };

    auto* content = new cadenza::ui::FirstLaunchAssistant(std::move(cb), std::move(provider));

    auto window = std::make_unique<FirstLaunchWindow>([this] { closeFirstLaunchAssistant(); });
    window->setContentOwned(content, true);
    window->centreAroundComponent(this, content->getWidth(), content->getHeight());
    window->setVisible(true);
    m_firstLaunchWindow = std::move(window);
}

void MainComponent::closeFirstLaunchAssistant()
{
    if (m_settings) {
        m_settings->state().setupAssistantSeen = true;
        saveSettings();
    }
    // Defer destroying the window — this can be called from inside the window's own
    // closeButtonPressed(), so we must not delete it synchronously.
    juce::Component::SafePointer<MainComponent> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (auto* self = safe.getComponent()) self->m_firstLaunchWindow.reset();
    });
}

void MainComponent::playTestChord()
{
    if (!m_audio.supportsSoundFonts()) {
        juce::Logger::writeToLog("[Cadenza] Test Sound: synth is the silent Null fallback - no audio.");
        return;
    }
    // A short C-major triad on the manual right-hand channel so the user hears
    // something immediately. This is a direct synth note, not arranger playback.
    static constexpr int kTestNotes[3] = { 60, 64, 67 };
    for (int n : kTestNotes)
        m_audio.noteOn(1, n, 100);

    juce::Component::SafePointer<MainComponent> safe(this);
    juce::Timer::callAfterDelay(700, [safe] {
        if (auto* self = safe.getComponent())
            for (int n : kTestNotes)
                self->m_audio.noteOff(1, n);
    });
}

namespace {
// Chord-detection mode <-> persisted string. Keep in sync with the combo items
// and SettingsState::midiChordMode.
struct ChordModeChoice { const char* id; const char* label; arranger::ChordDetectionMode mode; };
const ChordModeChoice kChordModeChoices[] = {
    { "fingered", "Fingered (play full chords)",  arranger::ChordDetectionMode::Fingered },
    { "single",   "Single Finger (1-key chords)", arranger::ChordDetectionMode::SingleFinger },
    { "multi",    "Multi Finger",                 arranger::ChordDetectionMode::MultiFinger },
    { "onbass",   "Fingered on Bass (slash)",     arranger::ChordDetectionMode::FingeredOnBass },
    { "full",     "Full Keyboard",                arranger::ChordDetectionMode::FullKeyboard },
};
constexpr int kNumChordModes = (int) (sizeof(kChordModeChoices) / sizeof(kChordModeChoices[0]));

arranger::ChordDetectionMode chordModeFromId(const std::string& id) noexcept
{
    for (const auto& c : kChordModeChoices)
        if (id == c.id) return c.mode;
    return arranger::ChordDetectionMode::Fingered;
}

juce::String noteName(int midiNote)
{
    static const char* n[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    return juce::String(n[((midiNote % 12) + 12) % 12]) + juce::String(midiNote / 12 - 1);
}

// MIDI Settings / button-mapping window: input-port picker, chord-detection mode,
// split point, plus one row per arranger action (current mapping + Learn + Clear).
// Refreshes the displayed mappings on a timer so a MIDI-learn capture shows live.
class MidiSettingsComponent : public juce::Component, private juce::Timer
{
public:
    using MappingTextFn = std::function<juce::String(const std::string&)>;
    using CommandFn     = std::function<void(const std::string&)>;
    using SelectFn      = std::function<void(juce::String)>;
    using SplitFn       = std::function<void(int)>;

    MidiSettingsComponent(const juce::StringArray& devices,
                          const std::vector<std::pair<juce::String, std::string>>& actions,
                          MappingTextFn mappingText, CommandFn onLearn, CommandFn onClear,
                          const juce::String& selectedDevice, const std::string& chordModeId, int splitNote,
                          int humanizeAmount, bool useProVoices,
                          SelectFn onSelectDevice, CommandFn onChordMode, SplitFn onSplit, SplitFn onHumanize,
                          std::function<void(bool)> onProVoices, std::function<void()> onMasterInstrument)
        : m_mappingText(std::move(mappingText)), m_onLearn(std::move(onLearn)), m_onClear(std::move(onClear)),
          m_onSelectDevice(std::move(onSelectDevice)), m_onChordMode(std::move(onChordMode)), m_onSplit(std::move(onSplit)),
          m_onHumanize(std::move(onHumanize)), m_onProVoices(std::move(onProVoices)),
          m_onMasterInstrument(std::move(onMasterInstrument))
    {
        // --- Input port picker (item 1 = Auto, then each detected device) ---
        m_inputLabel.setText("Input", juce::dontSendNotification);
        m_inputLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(m_inputLabel);
        m_inputBox.addItem("Auto (main keyboard, skip aux ports)", 1);
        int sel = 1;
        for (int i = 0; i < devices.size(); ++i) {
            m_inputBox.addItem(devices[i], i + 2);
            if (devices[i] == selectedDevice) sel = i + 2;
        }
        m_inputBox.setSelectedId(sel, juce::dontSendNotification);
        m_inputBox.onChange = [this] {
            if (!m_onSelectDevice) return;
            const int id = m_inputBox.getSelectedId();
            m_onSelectDevice(id <= 1 ? juce::String() : m_inputBox.getText());
        };
        addAndMakeVisible(m_inputBox);

        // --- Chord-detection mode ---
        m_modeLabel.setText("Chord mode", juce::dontSendNotification);
        m_modeLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(m_modeLabel);
        for (int i = 0; i < kNumChordModes; ++i) {
            m_modeBox.addItem(kChordModeChoices[i].label, i + 1);
            if (chordModeId == kChordModeChoices[i].id) m_modeBox.setSelectedId(i + 1, juce::dontSendNotification);
        }
        if (m_modeBox.getSelectedId() == 0) m_modeBox.setSelectedId(1, juce::dontSendNotification);
        m_modeBox.onChange = [this] {
            const int idx = m_modeBox.getSelectedId() - 1;
            if (m_onChordMode && idx >= 0 && idx < kNumChordModes)
                m_onChordMode(kChordModeChoices[idx].id);
        };
        addAndMakeVisible(m_modeBox);

        // --- Split point ---
        m_splitLabel.setText("Split", juce::dontSendNotification);
        m_splitLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(m_splitLabel);
        m_split.setSliderStyle(juce::Slider::IncDecButtons);
        m_split.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 64, 22);
        m_split.setRange(36, 84, 1);
        m_split.setValue(splitNote, juce::dontSendNotification);
        m_split.textFromValueFunction = [](double v) { return noteName((int) v); };
        m_split.setValue(splitNote, juce::dontSendNotification);   // re-apply so text uses note name
        m_split.onValueChange = [this] { if (m_onSplit) m_onSplit((int) m_split.getValue()); };
        addAndMakeVisible(m_split);

        // --- Humanize (accompaniment velocity/timing variation) ---
        m_humanizeLabel.setText("Humanize", juce::dontSendNotification);
        m_humanizeLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(m_humanizeLabel);
        m_humanize.setSliderStyle(juce::Slider::LinearHorizontal);
        m_humanize.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 22);
        m_humanize.setRange(0, 100, 1);
        m_humanize.setValue(humanizeAmount, juce::dontSendNotification);
        m_humanize.textFromValueFunction = [](double v) { return v <= 0 ? juce::String("Off") : juce::String((int) v) + "%"; };
        m_humanize.setValue(humanizeAmount, juce::dontSendNotification);
        m_humanize.onValueChange = [this] { if (m_onHumanize) m_onHumanize((int) m_humanize.getValue()); };
        addAndMakeVisible(m_humanize);

        // --- Use pro voices (auto-SFZ via sforzando) ---
        m_proVoices.setButtonText("Use pro voices (SFZ)");
        m_proVoices.setColour(juce::ToggleButton::textColourId, juce::Colours::lightgrey);
        m_proVoices.setToggleState(useProVoices, juce::dontSendNotification);
        m_proVoices.onClick = [this] { if (m_onProVoices) m_onProVoices(m_proVoices.getToggleState()); };
        addAndMakeVisible(m_proVoices);

        // --- Master instrument (one multitimbral plugin for the whole band) ---
        m_masterInstrument.setButtonText("Master Instrument (whole band)...");
        m_masterInstrument.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3a3320));
        m_masterInstrument.onClick = [this] { if (m_onMasterInstrument) m_onMasterInstrument(); };
        addAndMakeVisible(m_masterInstrument);

        m_devices.setText(devices.isEmpty() ? "No MIDI inputs detected"
                                            : juce::String(devices.size()) + " port(s) detected",
                          juce::dontSendNotification);
        m_devices.setColour(juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible(m_devices);

        for (const auto& a : actions) {
            auto row = std::make_unique<Row>();
            row->command = a.second;
            row->name.setText(a.first, juce::dontSendNotification);
            row->name.setColour(juce::Label::textColourId, juce::Colours::white);
            row->mapping.setColour(juce::Label::textColourId, juce::Colours::skyblue);
            row->mapping.setJustificationType(juce::Justification::centred);
            row->learn.setButtonText("Learn");
            row->clear.setButtonText("X");
            const std::string cmd = a.second;
            auto* mapLabel = &row->mapping;
            row->learn.onClick = [this, cmd, mapLabel] {
                m_pending = cmd;
                m_pendingPrev = m_mappingText ? m_mappingText(cmd) : juce::String();
                mapLabel->setText("press a button...", juce::dontSendNotification);
                if (m_onLearn) m_onLearn(cmd);
            };
            row->clear.onClick = [this, cmd] { if (m_onClear) m_onClear(cmd); };
            addAndMakeVisible(row->name);
            addAndMakeVisible(row->mapping);
            addAndMakeVisible(row->learn);
            addAndMakeVisible(row->clear);
            m_rows.push_back(std::move(row));
        }
        refresh();
        startTimerHz(5);
        setSize(460, 70 + kHeaderHeight + static_cast<int>(m_rows.size()) * 30);
    }

    void resized() override
    {
        auto a = getLocalBounds().reduced(10);

        // Top settings block: input port, chord mode, split point.
        auto labelled = [&a](juce::Label& lab, juce::Component& ctl, int ctlWidth) {
            auto row = a.removeFromTop(28);
            lab.setBounds(row.removeFromLeft(80));
            ctl.setBounds(row.removeFromLeft(ctlWidth));
            a.removeFromTop(4);
        };
        labelled(m_inputLabel,    m_inputBox,  a.getWidth() - 80);
        labelled(m_modeLabel,     m_modeBox,   a.getWidth() - 80);
        labelled(m_splitLabel,    m_split,     150);
        labelled(m_humanizeLabel, m_humanize,  a.getWidth() - 80);
        m_proVoices.setBounds(a.removeFromTop(26));
        m_masterInstrument.setBounds(a.removeFromTop(26));
        a.removeFromTop(2);

        m_devices.setBounds(a.removeFromTop(20));
        a.removeFromTop(8);
        for (auto& r : m_rows) {
            auto row = a.removeFromTop(26);
            a.removeFromTop(4);
            r->name.setBounds(row.removeFromLeft(120));
            r->clear.setBounds(row.removeFromRight(30));
            row.removeFromRight(6);
            r->learn.setBounds(row.removeFromRight(64));
            row.removeFromRight(8);
            r->mapping.setBounds(row);
        }
    }

private:
    struct Row {
        std::string command;
        juce::Label name, mapping;
        juce::TextButton learn, clear;
    };

    void refresh()
    {
        for (auto& r : m_rows) {
            const juce::String t = m_mappingText ? m_mappingText(r->command) : juce::String();
            if (m_pending == r->command) {
                if (t != m_pendingPrev) m_pending.clear();   // a new mapping was captured
                else { r->mapping.setText("press a button...", juce::dontSendNotification); continue; }
            }
            r->mapping.setText(t.isEmpty() ? juce::String("--") : t, juce::dontSendNotification);
        }
    }

    void timerCallback() override { refresh(); }

    static constexpr int kHeaderHeight = 28 * 4 + 4 * 4 + 26 + 26 + 2 + 20 + 8;  // 4 rows + toggle + master btn + device line

    MappingTextFn m_mappingText;
    CommandFn m_onLearn, m_onClear;
    SelectFn  m_onSelectDevice;
    CommandFn m_onChordMode;
    SplitFn   m_onSplit;
    SplitFn   m_onHumanize;
    std::function<void(bool)> m_onProVoices;
    std::function<void()> m_onMasterInstrument;

    juce::Label m_inputLabel, m_modeLabel, m_splitLabel, m_humanizeLabel, m_devices;
    juce::ComboBox m_inputBox, m_modeBox;
    juce::Slider m_split, m_humanize;
    juce::ToggleButton m_proVoices;
    juce::TextButton m_masterInstrument;

    std::vector<std::unique_ptr<Row>> m_rows;
    std::string m_pending;
    juce::String m_pendingPrev;
};
}

void MainComponent::showMidiSettings()
{
    if (!m_settings) return;

    const std::vector<std::pair<juce::String, std::string>> actions = {
        { "Start / Stop", "play" },
        { "Intro",   "intro"  }, { "Intro B", "introB" }, { "Intro C", "introC" },
        { "Main A",  "mainA"  }, { "Main B",  "mainB"  }, { "Main C", "mainC" }, { "Main D", "mainD" },
        { "Fill A",  "fillAA" }, { "Fill B",  "fillBB" }, { "Fill C", "fillCC" }, { "Fill D", "fillDD" },
        { "Ending",  "ending" }, { "Ending B","endingB" }, { "Ending C","endingC" },
    };

    juce::Component::SafePointer<MainComponent> safe(this);
    auto mappingText = [safe](const std::string& cmd) -> juce::String {
        auto* self = safe.getComponent();
        if (!self || !self->m_settings) return {};
        cadenza::midi::MidiControlMap m;
        m.setEntries(self->m_settings->state().midiControlMap);
        if (auto t = m.triggerFor(cmd)) return juce::String(cadenza::midi::describeTrigger(*t));
        return {};
    };
    auto onLearn = [safe](const std::string& cmd) {
        if (auto* self = safe.getComponent()) { self->m_learnCommand = cmd; self->m_midi.armControlLearn(true); }
    };
    auto onClear = [safe](const std::string& cmd) {
        auto* self = safe.getComponent();
        if (!self || !self->m_settings) return;
        cadenza::midi::MidiControlMap m;
        m.setEntries(self->m_settings->state().midiControlMap);
        m.clearCommand(cmd);
        self->m_settings->state().midiControlMap = m.entries();
        self->m_midi.setControlMap(m.entries());
        self->saveSettings();
    };

    auto onSelectDevice = [safe](juce::String name) {
        auto* self = safe.getComponent();
        if (!self || !self->m_settings) return;
        self->m_midi.setSelectedInput(name);
        self->m_settings->state().midiInputDevice = name.toStdString();
        self->saveSettings();
    };
    auto onChordMode = [safe](const std::string& id) {
        auto* self = safe.getComponent();
        if (!self || !self->m_settings) return;
        self->m_midi.setChordDetectionMode(chordModeFromId(id));
        self->m_settings->state().midiChordMode = id;
        self->saveSettings();
    };
    auto onSplit = [safe](int note) {
        auto* self = safe.getComponent();
        if (!self || !self->m_settings) return;
        self->m_midi.setSplitPoint(note);
        if (self->m_panel) self->m_panel->setSplitPoint(note);
        self->m_settings->state().splitNote = note;
        self->saveSettings();
    };
    auto onHumanize = [safe](int amount) {
        auto* self = safe.getComponent();
        if (!self || !self->m_settings) return;
        self->m_styleEngine.setHumanizeAmount(amount);
        self->m_settings->state().humanizeAmount = amount;
        self->saveSettings();
    };

    auto onProVoices = [safe](bool on) {
        auto* self = safe.getComponent();
        if (!self || !self->m_settings) return;
        self->m_settings->state().useProVoices = on;
        self->saveSettings();
        self->updateNativePanelStyle();   // re-run the reconcile so voices load (on) or revert to GM (off)
    };

    auto onMasterInstrument = [safe] {
        if (auto* self = safe.getComponent()) self->chooseMasterInstrument();
    };

    const auto& st = m_settings->state();
    auto* comp = new MidiSettingsComponent(m_midi.availableInputs(), actions,
                                           std::move(mappingText), std::move(onLearn), std::move(onClear),
                                           juce::String(st.midiInputDevice), st.midiChordMode, st.splitNote,
                                           st.humanizeAmount, st.useProVoices,
                                           std::move(onSelectDevice), std::move(onChordMode), std::move(onSplit),
                                           std::move(onHumanize), std::move(onProVoices),
                                           std::move(onMasterInstrument));
    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(comp);
    opts.dialogTitle = "MIDI Settings & Button Mapping";
    opts.dialogBackgroundColour = juce::Colour(0xff2a2f3a);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = false;
    opts.launchAsync();
}

void MainComponent::loadSettings()
{
    if (!m_settings) return;
    if (!m_settings->load()) return;   // file missing or corrupt → keep defaults

    const auto& s = m_settings->state();
    m_state.setBpm(s.bpm);
    m_state.setTranspose(s.transpose);
    m_state.setOctave(s.octave);
    m_state.setKey(s.key);
    m_state.setBankMemory(s.bankMemory);
    m_state.setStyleMemory(s.styleMemory);
    m_state.setCrossfade(s.crossfade);
    m_state.setSyncroStopOnRelease(s.syncroStopOnRelease);
    m_state.setAutoFillEnabled(s.autoFillEnabled);
    m_state.setChordSourceEnabled("bass", s.chordBassEnabled);
    m_state.setChordSourceEnabled("arranger", s.chordArrangerEnabled);
    m_state.setChordSourceEnabled("memory", s.chordMemoryEnabled);

    juce::Logger::writeToLog("[Cadenza] Loaded settings from " + juce::String(m_settings->path()));
}

juce::File MainComponent::voiceMapFile() const
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Cadenza").getChildFile("voicemap.json");
}

void MainComponent::loadVoiceMap()
{
    const auto f = voiceMapFile();
    if (f.existsAsFile() && m_voiceMap.loadFromJson(f.loadFileAsString().toStdString()))
        juce::Logger::writeToLog("[Cadenza] Loaded voice map from " + f.getFullPathName());
}

void MainComponent::applyVoiceMapToStyle()
{
    if (!m_settings || !m_settings->state().useProVoices || m_voiceMap.empty())
        return;

    // Per-style explicit plugin overrides win — collect them so we don't stomp them.
    std::set<int> overridden;
    const auto it = m_settings->state().styleMixes.find(m_settings->state().lastStyleId);
    if (it != m_settings->state().styleMixes.end())
        for (const auto& m : it->second)
            if (!m.pluginPath.empty())
                overridden.insert(m.channel);

    for (const auto& c : m_mixer.channels()) {
        const int ch = c.channel;
        if (overridden.count(ch))
            continue;
        const bool drums = (ch == 9 || ch == 10);
        const auto* entry = drums ? m_voiceMap.forDrums()
                                  : m_voiceMap.forProgram(m_mixer.program(ch));
        if (entry == nullptr)
            continue;   // unmapped -> keep the GM SoundFont voice
        std::string err;
        if (m_audio.loadPartInstrument(ch, entry->pluginPath, entry->presetState, err)) {
            if (entry->gain >= 0)
                m_audio.controlChange(ch, 7, entry->gain);
            if (m_panel)
                m_panel->setMixerInstrumentName(ch, juce::String(m_audio.partInstrumentName(ch)));
        } else {
            juce::Logger::writeToLog("[Cadenza] VoiceMap voice missing/failed ch="
                                     + juce::String(ch) + ": " + juce::String(err));   // GM fallback
        }
    }
}

void MainComponent::setDefaultVoice(int channel)
{
    const std::string path  = m_audio.partInstrumentPath(channel);
    if (path.empty()) {
        juce::NativeMessageBox::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
            "Set Default Voice", "Load a VST instrument (e.g. sforzando) on this part first.");
        return;
    }
    const std::string state = m_audio.capturePartInstrumentState(channel);

    juce::PopupMenu menu;
    menu.addItem(1, "Drums");
    menu.addItem(2, "Bass");
    menu.addItem(3, "Piano / Comp");
    juce::Component::SafePointer<MainComponent> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options(),
        [safe, channel, path, state](int choice) {
            auto* self = safe.getComponent();
            if (self == nullptr || choice == 0)
                return;
            cadenza::audio::VoiceMapEntry e;
            e.pluginPath  = path;
            e.presetState = state;
            if (choice == 1)      self->m_voiceMap.setDrums(e);
            else if (choice == 2) self->m_voiceMap.setFamily(4, e);   // GM 32..39 bass
            else if (choice == 3) self->m_voiceMap.setFamily(0, e);   // GM 0..7 piano
            const auto f = self->voiceMapFile();
            f.getParentDirectory().createDirectory();
            f.replaceWithText(juce::String(self->m_voiceMap.toJson()));
            if (self->m_settings) {
                self->m_settings->state().useProVoices = true;
                self->saveSettings();
            }
            juce::Logger::writeToLog("[Cadenza] Saved default voice ch=" + juce::String(channel)
                                     + " slot=" + juce::String(choice));
        });
}

void MainComponent::saveSettings()
{
    if (!m_settings) return;

    auto& s = m_settings->state();
    s.bpm         = m_state.bpm();
    s.transpose   = m_state.transpose();
    s.octave      = m_state.octave();
    s.key         = m_state.key();
    s.bankMemory  = m_state.bankMemory();
    s.styleMemory = m_state.styleMemory();
    s.crossfade   = m_state.crossfade();
    s.syncroStopOnRelease = m_state.syncroStopOnRelease();
    s.autoFillEnabled = m_state.autoFillEnabled();
    s.chordBassEnabled = m_state.chordSourceEnabled("bass");
    s.chordArrangerEnabled = m_state.chordSourceEnabled("arranger");
    s.chordMemoryEnabled = m_state.chordSourceEnabled("memory");
    // lastStyleId / lastStylePath / lastSoundFontPath / midiInputDevice are updated as those change.

    m_settings->save();
}

void MainComponent::resized()
{
    if (m_panel)
        m_panel->setBounds(getLocalBounds());
}

juce::File MainComponent::findResourcesRoot()
{
    const auto exeResources = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
        .getParentDirectory()
        .getChildFile("resources");

    if (exeResources.getChildFile("factory").isDirectory()
        || exeResources.getChildFile("sf2").isDirectory())
        return exeResources;

    const auto workingResources = juce::File::getCurrentWorkingDirectory()
        .getChildFile("resources");

    if (workingResources.getChildFile("factory").isDirectory()
        || workingResources.getChildFile("sf2").isDirectory())
        return workingResources;

    return juce::File::getCurrentWorkingDirectory();
}


void MainComponent::pushToWeb(const juce::String&)
{
    // The WebUI has been removed; the native panel is the sole UI. This is kept as
    // a no-op so the existing mirror-to-web call sites compile without churn.
}

void MainComponent::applyRuntimeStateToEngines()
{
    m_styleEngine.setGlobalTranspose(m_state.transpose());
    m_midi.setLiveTranspose(m_state.transpose());   // transpose shifts the right-hand melody too
    m_midi.setLiveOctave(m_state.octave());   // Octave affects live right-hand only, not style parts
    m_styleEngine.setEnabled(m_state.chordSourceEnabled("arranger"));
    // Bass chord-source forces slash detection (FingeredOnBass); otherwise use the
    // mode the player picked in MIDI Settings (default Fingered).
    m_midi.setChordDetectionMode(m_state.chordSourceEnabled("bass")
        ? arranger::ChordDetectionMode::FingeredOnBass
        : chordModeFromId(m_settings ? m_settings->state().midiChordMode : std::string("fingered")));
    m_midi.setChordMemory(m_state.chordSourceEnabled("memory"));
    m_midi.setSyncroStopOnRelease(m_state.syncroStopOnRelease());
}

juce::Array<juce::File> MainComponent::factoryStyleFiles() const
{
    const auto stylesDir = findResourcesRoot()
        .getChildFile("factory")
        .getChildFile("styles");

    juce::Array<juce::File> files;
    if (!stylesDir.isDirectory())
        return files;

    files.addArray(stylesDir.findChildFiles(juce::File::findFiles, false, "*.cstyle"));
    files.addArray(stylesDir.findChildFiles(juce::File::findFiles, false, "*.sty"));
    return files;
}

const std::vector<cadenza::arranger::StyleMetadata>& MainComponent::factoryStyleMetadata() const
{
    const auto files = factoryStyleFiles();
    std::vector<std::string> paths;
    paths.reserve(static_cast<std::size_t>(files.size()));
    for (const auto& file : files)
        paths.push_back(file.getFullPathName().toStdString());
    return m_factoryStyleIndex.entriesForFiles(paths);
}

void MainComponent::pushFactoryStylesToWeb()
{
    const auto& styles = factoryStyleMetadata();

    juce::String js = "window.JuceBridge && window.JuceBridge.onFactoryStyles([";
    for (std::size_t i = 0; i < styles.size(); ++i) {
        const auto& style = styles[i];

        if (i > 0) js += ",";
        js += "{";
        js += "id:" + jsString(juce::String(style.id));
        js += ",name:" + jsString(juce::String(style.name));
        js += ",path:" + jsString(juce::String(style.path));
        js += ",kind:" + jsString(juce::String(style.kind));
        js += "}";
    }
    js += "]);";
    pushToWeb(js);
}

void MainComponent::pushRuntimeStateToWeb()
{
    const auto current = m_styleEngine.currentStyle();
    const auto styleName = current ? juce::String(current->name) : juce::String();
    const auto styleId = current ? juce::String(current->id) : juce::String();

    juce::String js = "window.JuceBridge && window.JuceBridge.onRuntimeState({";
    js += "bpm:" + juce::String(m_state.bpm());
    js += ",transpose:" + juce::String(m_state.transpose());
    js += ",octave:" + juce::String(m_state.octave());
    js += ",syncroStop:" + boolLiteral(m_state.syncroStopOnRelease());
    js += ",activeBass:" + boolLiteral(m_state.chordSourceEnabled("bass"));
    js += ",activeArranger:" + boolLiteral(m_state.chordSourceEnabled("arranger"));
    js += ",activeMemory:" + boolLiteral(m_state.chordSourceEnabled("memory"));
    js += ",styleName:" + jsString(styleName);
    js += ",styleId:" + jsString(styleId);
    const auto soundFontPath = m_settings ? juce::String(m_settings->state().lastSoundFontPath) : juce::String();
    const auto soundFontName = soundFontPath.isNotEmpty()
        ? juce::File(soundFontPath).getFileName()
        : juce::String("None");
    js += ",synthEngine:" + jsString(m_audio.synthEngineName());
    js += ",soundFontName:" + jsString(soundFontName);
    js += ",soundFontPath:" + jsString(soundFontPath);
    js += "});";
    pushToWeb(js);
}

void MainComponent::installBridgeHooks()
{
    cadenza::BridgeHooks hooks;
    hooks.onNoteOn = [this](int channel, int note, int velocity) {
        // On-screen piano notes go through the SAME live melody voice as hardware
        // keys, so the Octave control and the melody instrument apply to right-hand
        // (above-split) notes. Below-split notes sound directly (unshifted).
        const auto events = m_midi.handleVirtualMelodyNote(note, velocity, true);
        if (!events.empty()) {
            for (const auto& ev : events)            // one per enabled Right layer
                m_audio.noteOn(ev.channel, ev.note, ev.velocity);
        } else {
            m_audio.noteOn(channel == 0 ? 1 : channel, note, velocity);
        }
    };
    hooks.onNoteOff = [this](int channel, int note) {
        const auto events = m_midi.handleVirtualMelodyNote(note, 0, false);
        if (!events.empty()) {
            for (const auto& ev : events)
                m_audio.noteOff(ev.channel, ev.note);
        } else {
            m_audio.noteOff(channel == 0 ? 1 : channel, note);
        }
    };
    hooks.onPlayStateChanged = [this](bool playing) {
        if (playing) {
            // Restart the chord chart from the current bar when song mode is on.
            if (m_songModeActive) {
                m_songPlayer.reset();
                m_lastSongBar = -1;
                applySongStepForBar(1);
                queueSongSectionForBar(2);
            }
            m_audio.play();
        } else {
            m_audio.stop();
        }
    };
    hooks.onBpmChanged = [this](int bpm) {
        m_audio.setBpm(static_cast<double>(bpm));
        saveSettings();
    };
    hooks.onTransposeChanged = [this](int semitones) {
        m_styleEngine.setGlobalTranspose(semitones);
        m_midi.setLiveTranspose(semitones);   // shift the right-hand melody too
        saveSettings();
    };
    hooks.onOctaveChanged = [this](int octaves) {
        // Octave is a live right-hand feature only — it must not move style parts.
        m_midi.setLiveOctave(octaves);
        juce::Logger::writeToLog("[Cadenza] octave changed -> setLiveOctave(" + juce::String(octaves)
                                 + "); MidiRouter now reports " + juce::String(m_midi.liveOctave()));
        saveSettings();
    };
    hooks.onKeyChanged = [this](const std::string& key) {
        // map "C" .. "B" to pitch class. Accept "#" but not "b".
        static const std::string names[12] = {
            "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
        };
        for (int i = 0; i < 12; ++i)
            if (names[i] == key) { m_styleEngine.setKeyTonic(i); break; }
        saveSettings();
    };
    hooks.onBankMemoryChanged = [this](const std::string& bankName) {
        // The "Bank Memory" voice picker selects Right 1's instrument: map the
        // voice name to a GM program for the primary right-hand layer.
        if (m_settings) {
            const int program = cadenza::midi::gmProgramForBankName(bankName);
            m_settings->state().melodyProgram = program;
            m_settings->state().rightLayers[0].program = program;
        }
        applyRightHand();
        saveSettings();
    };
    hooks.onStyleMemoryChanged = [this](int slot) {
        // Map the 4 Style Memory pads to the 4 most useful sections.
        static const char* slotSection[4] = { "mainA", "mainB", "intro", "ending" };
        if (slot >= 1 && slot <= 4) {
            // Manual section control overrides the song chart.
            if (m_songModeActive)
                setSongMode(false);
            m_styleEngine.allNotesOff();
            m_styleEngine.setSection(slotSection[slot - 1]);
            saveSettings();
        }
    };
    hooks.onPadChanged = [this](int /*padIndex*/, bool /*on*/) {
        // Future: pad sample triggers (one-shot SF2 hits / audio loops).
    };
    hooks.onSelectStyle = [this](const std::string& name) {
        const juce::File directFile { juce::String(name) };
        if (directFile.existsAsFile() && isSupportedStyleFile(directFile)) {
            loadAndApplyStyleFile(directFile);
            return;
        }

        const auto files = factoryStyleFiles();
        std::vector<std::string> paths;
        paths.reserve(static_cast<std::size_t>(files.size()));
        for (const auto& file : files)
            paths.push_back(file.getFullPathName().toStdString());

        if (const auto* style = m_factoryStyleIndex.findByIdNameOrStem(paths, name)) {
            loadAndApplyStyleFile(juce::File(juce::String(style->path)));
            return;
        }
        juce::Logger::writeToLog("[Cadenza] No style file matches: " + juce::String(name));
    };
    hooks.onOpenStyleFile = [this] {
        openStyleFileChooser();
    };
    hooks.onOpenSoundFontFile = [this] {
        openSoundFontFileChooser();
    };
    hooks.onOpenSongFile = [this] {
        openSongFileChooser();
    };
    hooks.onSongModeChanged = [this](bool enabled) {
        setSongMode(enabled);
    };
    hooks.onOpenPluginFile = [this] {
        openPluginFileChooser();
    };
    hooks.onClearPlugin = [this] {
        clearMasterEffect();
    };
    hooks.onExportPlaybackDiagnostics = [this] {
        exportPlaybackDiagnostics();
    };

    hooks.onChordSourceChanged = [this](const std::string& source, bool enabled) {
        if (source == "bass") {
            m_midi.setChordDetectionMode(enabled
                ? arranger::ChordDetectionMode::FingeredOnBass
                : arranger::ChordDetectionMode::Fingered);
        } else if (source == "arranger") {
            if (!enabled)
                m_styleEngine.allNotesOff();
            m_styleEngine.setEnabled(enabled);
        } else if (source == "memory") {
            m_midi.setChordMemory(enabled);
        }
        saveSettings();
    };

    hooks.onSyncroStopChanged = [this](bool enabled) {
        m_midi.setSyncroStopOnRelease(enabled);
        saveSettings();
        juce::Logger::writeToLog(juce::String("[Cadenza] Syncro Stop on release: ")
                                 + (enabled ? "ON" : "OFF"));
    };

    hooks.onExitApp = [this] {
        juce::JUCEApplicationBase::quit();
    };

    hooks.onPageReady = [this] {
        // The web page just (re)loaded its JS. Push current state so the UI
        // reflects whatever the C++ engine already knows.
        const auto chordName = m_midi.currentChordDisplayName();
        const auto chordJs = juce::String("window.JuceBridge && window.JuceBridge.onChordChanged(\"")
                           + juce::String(chordName) + "\");";
        pushToWeb(chordJs);
        pushFactoryStylesToWeb();
        pushRuntimeStateToWeb();
        pushPluginStateToWeb();
        juce::Logger::writeToLog("[Cadenza] pageReady: pushed chord = "
                                 + (chordName.empty() ? juce::String("(none)") : juce::String(chordName)));
    };

    m_router.setHooks(std::move(hooks));
}

void MainComponent::tryLoadFactoryStyle()
{
    for (const auto& file : factoryStyleFiles()) {
        if (file.getFileNameWithoutExtension().equalsIgnoreCase("8-beat-pop")) {
            loadAndApplyStyleFile(file);
            return;
        }
    }

    const auto files = factoryStyleFiles();
    if (!files.isEmpty()) {
        loadAndApplyStyleFile(files.getFirst());
        return;
    }
}

bool MainComponent::tryLoadLastStyle()
{
    if (!m_settings)
        return false;

    const auto path = m_settings->state().lastStylePath;
    if (path.empty())
        return false;

    const juce::File file { juce::String(path) };
    return loadAndApplyStyleFile(file);
}

void MainComponent::tryLoadFactorySoundFont()
{
    if (!m_audio.supportsSoundFonts())
        return;

    if (m_settings && !m_settings->state().lastSoundFontPath.empty()) {
        const juce::File saved { juce::String(m_settings->state().lastSoundFontPath) };
        if (saved.existsAsFile() && isSupportedSoundFontFile(saved)) {
            if (loadAndApplySoundFontFile(saved, false))
                return;
        } else {
            juce::Logger::writeToLog("[Cadenza] Saved SoundFont is missing or unsupported: " + saved.getFullPathName());
        }
    }

    // Look for any .sf2/.sf3 in resources/sf2 (factory or user-dropped).
    const auto sf2Dir = findResourcesRoot()
        .getChildFile("sf2");
    if (!sf2Dir.isDirectory()) return;

    auto results = sf2Dir.findChildFiles(juce::File::findFiles, false, "*.sf2");
    results.addArray(sf2Dir.findChildFiles(juce::File::findFiles, false, "*.sf3"));
    if (results.isEmpty()) return;

    // Prefer the LARGEST SoundFont in the folder — bigger almost always means a
    // higher-quality sample set, so simply dropping a better .sf2 here upgrades
    // the sound with no further steps.
    juce::File best = results.getReference(0);
    for (const auto& f : results)
        if (f.getSize() > best.getSize())
            best = f;

    juce::Logger::writeToLog("[Cadenza] Auto-loading largest SoundFont in resources/sf2: "
                             + best.getFileName() + " (" + juce::String(best.getSize() / (1024 * 1024)) + " MB)");
    loadAndApplySoundFontFile(best, true);
}

void MainComponent::openStyleFileChooser()
{
    // Prefer the user's Imported library (genre folders) when it exists, so the
    // chooser lands on the full style collection; fall back to the bundled
    // Factory styles, then Documents.
    const auto imported = importedStylesDir();
    const auto factory  = findResourcesRoot().getChildFile("factory").getChildFile("styles");
    const auto startDir = imported.isDirectory() ? imported
                        : (factory.isDirectory() ? factory
                        : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory));

    m_styleChooser = std::make_unique<juce::FileChooser>(
        "Choose a Cadenza or Yamaha style",
        startDir,
        "*.cstyle;*.sty;*.prs;*.sst;*.fps;*.bcs");

    juce::Component::SafePointer<MainComponent> safe(this);
    m_styleChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& chooser) {
            if (auto* self = safe.getComponent()) {
                const auto file = chooser.getResult();
                if (file.existsAsFile())
                    self->loadAndApplyStyleFile(file);
                self->m_styleChooser.reset();
            }
        });
}

bool MainComponent::previewMidiStyleImport(const juce::File& file,
                                           const cadenza::arranger::MidiStyleConvertOptions& options,
                                           const cadenza::midi::Chord& sourceChord)
{
    stopMidiStyleImportPreview();

    auto previewOptions = options;
    previewOptions.normalizeToC = false;
    if (previewOptions.sectionName.isEmpty())
        previewOptions.sectionName = "mainA";

    auto converted = cadenza::arranger::convertMidiFileToNativeStyle(file, previewOptions);
    if (!converted.ok || converted.style == nullptr) {
        juce::String status = "MIDI style preview failed";
        if (!converted.warnings.isEmpty())
            status += ": " + converted.warnings.joinIntoString("; ");
        if (m_panel)
            m_panel->setRecorderState(false, false, status);
        juce::Logger::writeToLog("[Cadenza] " + status);
        return false;
    }

    m_midiImportPreviewPreviousStyle = m_styleEngine.currentStyle();
    m_midiImportPreviewPreviousSection = m_styleEngine.currentSection();
    m_midiImportPreviewPreviousMain = m_currentMain;
    m_midiImportPreviewWasPlaying = m_audio.transport().playing();
    m_midiImportPreviewActive = true;

    auto previewStyle = std::shared_ptr<const cadenza::arranger::Style>(std::move(converted.style));
    const auto previewSection = previewOptions.sectionName.toStdString();

    m_audio.stop();
    m_styleEngine.allNotesOff();
    m_audio.allNotesOff();
    m_styleEngine.setGlobalTranspose(0);
    m_styleEngine.setKeyTonic(0);
    m_styleEngine.setChord(sourceChord);
    m_styleEngine.setStyle(std::move(previewStyle));
    m_styleEngine.setSection(previewSection);
    m_audio.play();
    return true;
}

void MainComponent::stopMidiStyleImportPreview()
{
    if (!m_midiImportPreviewActive)
        return;

    const auto previousStyle = std::move(m_midiImportPreviewPreviousStyle);
    const auto previousSection = m_midiImportPreviewPreviousSection;
    const auto previousMain = m_midiImportPreviewPreviousMain;
    const bool wasPlaying = m_midiImportPreviewWasPlaying;

    m_midiImportPreviewActive = false;
    m_midiImportPreviewPreviousSection.clear();
    m_midiImportPreviewPreviousMain.clear();
    m_midiImportPreviewWasPlaying = false;

    m_audio.stop();
    m_styleEngine.allNotesOff();
    m_audio.allNotesOff();
    m_styleEngine.setStyle(previousStyle);
    if (previousStyle != nullptr && !previousSection.empty())
        m_styleEngine.setSection(previousSection);
    m_currentMain = previousMain.empty() ? m_currentMain : previousMain;

    m_styleEngine.setGlobalTranspose(m_state.transpose());
    m_styleEngine.setKeyTonic(keyNameToPitchClass(m_state.key()));
    if (auto chord = cadenza::midi::parseChordSymbol(m_state.chord()))
        m_styleEngine.setChord(*chord);
    else
        m_styleEngine.setChord({});

    if (wasPlaying && previousStyle != nullptr)
        m_audio.play();
}

void MainComponent::openMidiStyleImportChooser()
{
    const auto startDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    m_midiStyleChooser = std::make_unique<juce::FileChooser>(
        "Import a MIDI file as a Cadenza style",
        startDir,
        "*.mid;*.midi");

    juce::Component::SafePointer<MainComponent> safe(this);
    m_midiStyleChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& chooser) {
            auto* self = safe.getComponent();
            if (self == nullptr)
                return;

            const auto midiFile = chooser.getResult();
            if (midiFile.existsAsFile()) {
                auto info = cadenza::arranger::inspectMidiFileForStyleImport(midiFile, 0, 2);
                if (!info.ok) {
                    juce::String status = "MIDI style import failed";
                    if (!info.warnings.isEmpty())
                        status += ": " + info.warnings.joinIntoString("; ");
                    if (self->m_panel)
                        self->m_panel->setRecorderState(false, false, status);
                    juce::Logger::writeToLog("[Cadenza] " + status);
                } else {
                    juce::Component::SafePointer<MainComponent> dialogSafe(self);
                    auto* dialog = new MidiStyleImportDialog(
                        midiFile, info,
                        [dialogSafe, midiFile](std::vector<cadenza::arranger::MidiStyleSectionSpec> sections,
                                               bool normalizeToC,
                                               bool aiAddFillsAfterConvert) {
                            auto* owner = dialogSafe.getComponent();
                            if (owner == nullptr)
                                return;

                            auto converted = cadenza::arranger::convertMidiFileToNativeStyleMultiSection(
                                midiFile, sections, normalizeToC);
                            if (converted.ok && converted.style != nullptr) {
                                auto dir = importedStylesDir().getChildFile("MIDI Imports");
                                dir.createDirectory();
                                auto out = dir.getNonexistentChildFile(
                                    midiFile.getFileNameWithoutExtension(), ".cstyle");
                                const bool saved = cadenza::arranger::saveStyleToFile(
                                    *converted.style, out.getFullPathName().toStdString());
                                if (saved && owner->loadAndApplyStyleFile(out)) {
                                    owner->m_styleEngine.setEnabled(true);
                                    owner->m_state.setChordSourceEnabled("arranger", true);
                                    owner->m_midi.setChordMemory(true);
                                    owner->m_state.setChordSourceEnabled("memory", true);
                                    if (owner->m_recorder.sessionActive())
                                        owner->recorderRefreshStyle();
                                    if (owner->m_panel) {
                                        owner->m_panel->setToggleStates(
                                            owner->m_state.chordSourceEnabled("arranger"),
                                            owner->m_state.chordSourceEnabled("memory"),
                                            owner->m_state.syncroStopOnRelease(),
                                            owner->m_state.chordSourceEnabled("bass"),
                                            owner->m_state.autoFillEnabled());
                                    }
                                    owner->saveSettings();

                                    juce::String status = "Imported MIDI style: " + out.getFileName();
                                    if (!converted.warnings.isEmpty())
                                        status += " - " + converted.warnings.joinIntoString("; ");
                                    if (owner->m_panel)
                                        owner->m_panel->setRecorderState(true, false, status);
                                    juce::Logger::writeToLog("[Cadenza] " + status);
                                    if (aiAddFillsAfterConvert)
                                        owner->generateAiFillsIntroEnding();
                                } else {
                                    const auto status = "MIDI style import save/load failed: "
                                        + out.getFullPathName();
                                    if (owner->m_panel)
                                        owner->m_panel->setRecorderState(false, false, status);
                                    juce::Logger::writeToLog("[Cadenza] " + status);
                                }
                            } else {
                                juce::String status = "MIDI style import failed";
                                if (!converted.warnings.isEmpty())
                                    status += ": " + converted.warnings.joinIntoString("; ");
                                if (owner->m_panel)
                                    owner->m_panel->setRecorderState(false, false, status);
                                juce::Logger::writeToLog("[Cadenza] " + status);
                            }
                        },
                        [dialogSafe, midiFile](cadenza::arranger::MidiStyleConvertOptions options,
                                               cadenza::midi::Chord sourceChord) {
                            if (auto* owner = dialogSafe.getComponent())
                                return owner->previewMidiStyleImport(midiFile, options, sourceChord);
                            return false;
                        },
                        [dialogSafe] {
                            if (auto* owner = dialogSafe.getComponent())
                                owner->stopMidiStyleImportPreview();
                        });

                    juce::DialogWindow::LaunchOptions opts;
                    opts.dialogTitle = "Import MIDI as Style";
                    opts.content.setOwned(dialog);
                    opts.componentToCentreAround = self;
                    opts.escapeKeyTriggersCloseButton = true;
                    opts.useNativeTitleBar = true;
                    opts.resizable = false;
                    opts.dialogBackgroundColour = cadenza::ui::CadenzaLookAndFeel::panel();
                    opts.launchAsync();
                }
            }
            self->m_midiStyleChooser.reset();
        });
}

void MainComponent::openSoundFontFileChooser()
{
    const auto sf2Dir = findResourcesRoot()
        .getChildFile("sf2");

    m_soundFontChooser = std::make_unique<juce::FileChooser>(
        "Choose a SoundFont",
        sf2Dir.isDirectory() ? sf2Dir : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        "*.sf2;*.sf3");

    juce::Component::SafePointer<MainComponent> safe(this);
    m_soundFontChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& chooser) {
            if (auto* self = safe.getComponent()) {
                const auto file = chooser.getResult();
                if (file.existsAsFile())
                    self->loadAndApplySoundFontFile(file, true);
                self->m_soundFontChooser.reset();
            }
        });
}

void MainComponent::chooseMasterInstrument()
{
    juce::PopupMenu menu;
    const bool has = m_audio.hasMasterInstrument();
    menu.addItem(1, "Load VST3 instrument (plays the WHOLE band)...");
    menu.addItem(2, "Open instrument editor", has);
    menu.addItem(3, "Turn off (back to built-in engine)", has);

    juce::Component::SafePointer<MainComponent> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options(), [safe](int choice) {
        auto* self = safe.getComponent();
        if (self == nullptr || choice == 0)
            return;
        if (choice == 2) { self->m_audio.showMasterInstrumentEditor(); return; }
        if (choice == 3) { self->m_audio.clearMasterInstrument(); return; }

        juce::File vst3Dir("C:\\Program Files\\Common Files\\VST3");
        self->m_masterPluginChooser = std::make_unique<juce::FileChooser>(
            "Choose a multitimbral instrument (SampleTank, Sound Canvas, Kontakt)",
            vst3Dir.isDirectory() ? vst3Dir
                                  : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
            "*.vst3");
        self->m_masterPluginChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::canSelectDirectories,
            [safe](const juce::FileChooser& chooser) {
                auto* s = safe.getComponent();
                if (s == nullptr) return;
                const auto file = chooser.getResult();
                if (file.exists()) {
                    std::string err;
                    if (s->m_audio.loadMasterInstrument(file.getFullPathName().toStdString(), err)) {
                        juce::Logger::writeToLog("[Cadenza] Master instrument loaded: "
                                                 + juce::String(s->m_audio.masterInstrumentName()));
                        s->m_audio.showMasterInstrumentEditor();   // so the user assigns sounds per part
                    } else {
                        juce::Logger::writeToLog("[Cadenza] Master instrument load failed: " + juce::String(err));
                    }
                }
                s->m_masterPluginChooser.reset();
            });
    });
}

void MainComponent::choosePartInstrument(int channel)
{
    juce::File vst3Dir("C:\\Program Files\\Common Files\\VST3");
    m_partPluginChooser = std::make_unique<juce::FileChooser>(
        "Choose a VST3 instrument for this part",
        vst3Dir.isDirectory() ? vst3Dir
                              : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        "*.vst3");

    juce::Component::SafePointer<MainComponent> safe(this);
    m_partPluginChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::canSelectDirectories,   // .vst3 is a bundle dir on Windows
        [safe, channel](const juce::FileChooser& chooser) {
            if (auto* self = safe.getComponent()) {
                const auto file = chooser.getResult();
                if (file.exists()) {
                    std::string err;
                    if (self->m_audio.loadPartInstrument(channel, file.getFullPathName().toStdString(), err)) {
                        juce::Logger::writeToLog("[Cadenza] Loaded part instrument ch=" + juce::String(channel)
                                                 + ": " + juce::String(self->m_audio.partInstrumentName(channel)));
                        if (self->m_panel)
                            self->m_panel->setMixerInstrumentName(
                                channel, juce::String(self->m_audio.partInstrumentName(channel)));
                        self->m_audio.showPartInstrumentEditor(channel);   // pop the plugin's GUI
                        self->persistStyleMix();   // remember this plugin for the style
                    } else {
                        juce::Logger::writeToLog("[Cadenza] Part instrument load failed ch="
                                                 + juce::String(channel) + ": " + juce::String(err));
                    }
                }
                self->m_partPluginChooser.reset();
            }
        });
}

void MainComponent::chooseRightLayerInstrument(int layer)
{
    if (!m_settings || layer < 0 || layer >= 3)
        return;
    juce::File vst3Dir("C:\\Program Files\\Common Files\\VST3");
    m_partPluginChooser = std::make_unique<juce::FileChooser>(
        "Choose a VST3 instrument for Right " + juce::String(layer + 1),
        vst3Dir.isDirectory() ? vst3Dir
                              : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        "*.vst3");

    juce::Component::SafePointer<MainComponent> safe(this);
    m_partPluginChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::canSelectDirectories,
        [safe, layer](const juce::FileChooser& chooser) {
            if (auto* self = safe.getComponent()) {
                const auto file = chooser.getResult();
                if (file.exists()) {
                    const int channel = self->m_midi.rightLayerChannel(layer);
                    std::string err;
                    if (self->m_audio.loadPartInstrument(channel, file.getFullPathName().toStdString(), err)) {
                        auto& L = self->m_settings->state().rightLayers[layer];
                        L.pluginPath = file.getFullPathName().toStdString();
                        self->m_audio.controlChange(channel, 7, L.volume);   // apply the layer volume
                        if (self->m_panel)
                            self->m_panel->setRightVoiceName(
                                layer, juce::String(self->m_audio.partInstrumentName(channel)), true);
                        self->m_audio.showPartInstrumentEditor(channel);     // pop the plugin GUI
                        self->saveSettings();
                    } else {
                        juce::Logger::writeToLog("[Cadenza] Right " + juce::String(layer + 1)
                                                 + " VST load failed: " + juce::String(err));
                    }
                }
                self->m_partPluginChooser.reset();
            }
        });
}

bool MainComponent::loadAndApplySoundFontFile(const juce::File& file, bool persist)
{
    if (!file.existsAsFile() || !isSupportedSoundFontFile(file)) {
        juce::Logger::writeToLog("[Cadenza] Unsupported SoundFont file: " + file.getFullPathName());
        return false;
    }

    if (!m_audio.supportsSoundFonts()) {
        juce::Logger::writeToLog("[Cadenza] WARNING: " + juce::String(m_audio.synthEngineName())
                                 + " is active; cannot load SoundFont: " + file.getFullPathName());
        return false;
    }

    const bool ok = m_audio.loadSoundFont(file.getFullPathName().toStdString());
    if (!ok) {
        juce::Logger::writeToLog("[Cadenza] SoundFont failed or was rejected by FluidSynth: "
                                 + file.getFullPathName());
        return false;
    }

    if (m_settings) {
        m_settings->state().lastSoundFontPath = file.getFullPathName().toStdString();
        if (persist)
            saveSettings();
    }

    juce::Logger::writeToLog("[Cadenza] SoundFont loaded via " + juce::String(m_audio.synthEngineName())
                             + ": " + file.getFileName() + " (" + file.getFullPathName() + ")");
    m_styleEngine.reapplyCurrentSectionChannelSetup();
    applyMixerState();
    applyRightHand();
    applyLeftVoice();
    pushRuntimeStateToWeb();
    return true;
}

void MainComponent::exportPlaybackDiagnostics()
{
    const auto diagnosticsDir = juce::File::getCurrentWorkingDirectory()
        .getChildFile("diagnostics");
    const auto result = m_styleEngine.exportCurrentSectionDiagnostics(
        diagnosticsDir.getFullPathName().toStdString());

    if (!result.ok) {
        juce::Logger::writeToLog("[Cadenza] Playback diagnostic export failed: "
                                 + juce::String(result.error));
        return;
    }

    juce::Logger::writeToLog("[Cadenza] Playback diagnostics exported: "
                             + juce::String(result.csvPath)
                             + ", " + juce::String(result.midiPath)
                             + ", " + juce::String(result.summaryPath)
                             + " events=" + juce::String(result.eventCount));
}

bool MainComponent::loadAndApplyStyleFile(const juce::File& file)
{
    if (!file.existsAsFile() || !isSupportedStyleFile(file)) {
        juce::Logger::writeToLog("[Cadenza] Unsupported style file: " + file.getFullPathName());
        return false;
    }

    auto loaded = cadenza::arranger::loadStyleFromFile(file.getFullPathName().toStdString());
    if (!loaded.ok) {
        juce::Logger::writeToLog("[Cadenza] Failed to load style: " + juce::String(loaded.error));
        return false;
    }

    const auto initialSection = bestStartSection(loaded.style);
    const bool editableCadenzaStyle = file.hasFileExtension("cstyle")
        && m_recorder.loadSession(loaded.style, initialSection);
    if (!editableCadenzaStyle)
        m_recorder.endSession();
    m_recordArmed.store(false);

    auto sharedStyle = std::make_shared<const cadenza::arranger::Style>(
        std::move(loaded.style));

    auto appliedBpm = m_state.bpm();
    if (sharedStyle->defaultTempo > 0) {
        appliedBpm = m_state.setBpm(sharedStyle->defaultTempo);
        m_audio.setBpm(static_cast<double>(appliedBpm));
    }
    m_styleEngine.allNotesOff();
    m_styleEngine.setStyle(sharedStyle);
    if (editableCadenzaStyle)
        m_styleEngine.setStyle(m_recorder.snapshotStyle());
    if (!initialSection.empty()) {
        m_styleEngine.setSection(initialSection);
        if (initialSection.rfind("main", 0) == 0)
            m_currentMain = initialSection;   // fills return to the loaded Main
    }

    // Refresh the native panel's style name + sections + mixer, and (re)assign the
    // live-melody channel/program so it never collides with this style's channels.
    updateNativePanelStyle();
    if (editableCadenzaStyle) {
        // Loading a native .cstyle makes it editable, but must not turn the
        // performance keyboard into the recorder's target part while the user
        // is on Home/Sound/etc. Capture belongs only to the Style and Editor
        // pages; the page callback below keeps it in sync afterwards.
        const int page = m_panel ? m_panel->activePage() : 0;
        const bool recorderPage = page == cadenza::ui::NativePanel::kStylePage
            || page == cadenza::ui::NativePanel::kEditorPage;
        m_midi.setCaptureMode(recorderPage);
        m_audio.setMetronomeEnabled(recorderPage && m_metronomeOn);
        recorderPrepareTargetChannel();
        if (m_panel) {
            const auto cfg = m_recorder.config();
            int partIndex = 0;
            for (int i = 0; i < cadenza::arranger::kNumRecorderParts; ++i)
                if (cadenza::arranger::recorderPartInfo(i).part
                    == m_recorder.targetPart()) {
                    partIndex = i;
                    break;
                }
            m_panel->setRecorderBarCount(cfg.bars);
            m_panel->setRecorderPart(partIndex);
            m_panel->setRecorderState(
                true, false,
                recorderStatusText() + " - ready to Record or Edit");
        }
        recorderReloadEditor();
    } else {
        m_midi.setCaptureMode(false);
        m_audio.setMetronomeEnabled(false);
        recorderCloseEditor();
        if (m_panel)
            m_panel->setRecorderState(
                false, false, "Press New to record your own style");
    }

    if (m_settings) {
        m_settings->state().lastStyleId = sharedStyle->id;
        m_settings->state().lastStylePath = file.getFullPathName().toStdString();
    }

    pushRuntimeStateToWeb();

    saveSettings();

    juce::Logger::writeToLog("[Cadenza] Loaded style: " + juce::String(sharedStyle->name)
                             + " bpm=" + juce::String(appliedBpm)
                             + " ppq=" + juce::String(sharedStyle->ticksPerBeat)
                             + " timeSig=" + juce::String(sharedStyle->beatsPerBar)
                             + "/" + juce::String(sharedStyle->beatUnit)
                             + " from " + file.getFullPathName());
    return true;
}

void MainComponent::openSongFileChooser()
{
    const auto songsDir = findResourcesRoot()
        .getChildFile("factory")
        .getChildFile("songs");

    m_songChooser = std::make_unique<juce::FileChooser>(
        "Choose a Cadenza song (chord chart)",
        songsDir.isDirectory() ? songsDir : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        "*.csong");

    juce::Component::SafePointer<MainComponent> safe(this);
    m_songChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& chooser) {
            if (auto* self = safe.getComponent()) {
                const auto file = chooser.getResult();
                if (file.existsAsFile())
                    self->loadAndApplySongFile(file);
                self->m_songChooser.reset();
            }
        });
}

void MainComponent::openChordAnalysisChooser()
{
    const auto songsDir = findResourcesRoot()
        .getChildFile("factory")
        .getChildFile("songs");

    m_analysisChooser = std::make_unique<juce::FileChooser>(
        "Choose an audio file to analyse",
        songsDir.isDirectory() ? songsDir : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        "*.wav;*.mp3;*.flac;*.ogg;*.m4a;*.aif;*.aiff;*.aac");

    juce::Component::SafePointer<MainComponent> safe(this);
    m_analysisChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& chooser) {
            if (auto* self = safe.getComponent()) {
                const auto file = chooser.getResult();
                if (file.existsAsFile())
                    self->analyzeAudioFile(file);
                self->m_analysisChooser.reset();
            }
        });
}

bool MainComponent::selectStyleById(const std::string& styleId)
{
    if (styleId.empty())
        return false;

    const auto files = factoryStyleFiles();
    std::vector<std::string> paths;
    paths.reserve(static_cast<std::size_t>(files.size()));
    for (const auto& file : files)
        paths.push_back(file.getFullPathName().toStdString());

    if (const auto* style = m_factoryStyleIndex.findByIdNameOrStem(paths, styleId))
        return loadAndApplyStyleFile(juce::File(juce::String(style->path)));

    return false;
}

bool MainComponent::loadAndApplySongFile(const juce::File& file)
{
    if (!file.existsAsFile() || !isSupportedSongFile(file)) {
        juce::Logger::writeToLog("[Cadenza] Unsupported song file: " + file.getFullPathName());
        return false;
    }

    auto loaded = cadenza::arranger::loadSongFromFile(file.getFullPathName().toStdString());
    if (!loaded.ok) {
        juce::Logger::writeToLog("[Cadenza] Failed to load song: " + juce::String(loaded.error));
        return false;
    }

    // Load the style the song references (best-effort; keep current style if missing).
    if (!loaded.song.styleId.empty() && !selectStyleById(loaded.song.styleId)) {
        juce::Logger::writeToLog("[Cadenza] Song references style '" + juce::String(loaded.song.styleId)
                                 + "' which was not found; keeping current style.");
    }

    if (loaded.song.defaultTempo > 0) {
        const auto bpm = m_state.setBpm(loaded.song.defaultTempo);
        m_audio.setBpm(static_cast<double>(bpm));
    }

    // Apply the song's key so scale-tone parts reference the right tonic.
    if (!loaded.song.key.empty()) {
        static const std::string keyNames[12] = {
            "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
        };
        for (int i = 0; i < 12; ++i) {
            if (keyNames[i] == loaded.song.key) {
                m_styleEngine.setKeyTonic(i);
                m_state.setKey(loaded.song.key);
                break;
            }
        }
    }

    const auto songName = loaded.song.name.empty() ? file.getFileNameWithoutExtension().toStdString()
                                                    : loaded.song.name;
    m_songPlayer.setSong(std::make_shared<const cadenza::arranger::Song>(std::move(loaded.song)));

    juce::Logger::writeToLog("[Cadenza] Loaded song: " + juce::String(songName)
                             + " (" + juce::String(m_songPlayer.lastEventBar()) + " bars) from "
                             + file.getFullPathName());

    setSongMode(true);
    if (m_panel) m_panel->setActivePage(1);
    pushRuntimeStateToWeb();
    return true;
}

bool MainComponent::analyzeAudioFile(const juce::File& file)
{
    if (!file.existsAsFile() || !isSupportedAudioAnalysisFile(file)) {
        juce::Logger::writeToLog("[Cadenza] Unsupported audio file: " + file.getFullPathName());
        return false;
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(formatManager.createReaderFor(file));
    if (!reader) {
        juce::Logger::writeToLog("[Cadenza] Could not open audio file: " + file.getFullPathName());
        if (m_panel)
            m_panel->setChordAnalysisSummary("Could not open audio file. Try WAV or MP3.");
        return false;
    }

    const auto maxSamples = static_cast<juce::int64>(std::min<double>(reader->lengthInSamples, reader->sampleRate * 180.0));
    if (maxSamples <= 0) {
        juce::Logger::writeToLog("[Cadenza] Audio file is empty: " + file.getFullPathName());
        if (m_panel)
            m_panel->setChordAnalysisSummary("Audio file is empty.");
        return false;
    }

    const int channels = juce::jmax(1, static_cast<int>(reader->numChannels));
    const int blockSize = 16384;
    juce::AudioBuffer<float> buffer(channels, blockSize);
    std::vector<float> mono;
    mono.reserve(static_cast<std::size_t>(maxSamples));

    for (juce::int64 pos = 0; pos < maxSamples; pos += blockSize) {
        const int num = static_cast<int>(std::min<juce::int64>(blockSize, maxSamples - pos));
        buffer.setSize(channels, num, false, false, true);
        reader->read(&buffer, 0, num, pos, true, true);
        for (int i = 0; i < num; ++i) {
            double sum = 0.0;
            for (int ch = 0; ch < channels; ++ch)
                sum += buffer.getSample(ch, i);
            mono.push_back(static_cast<float>(sum / static_cast<double>(channels)));
        }
    }

    const int sampleRate = static_cast<int>(std::lround(reader->sampleRate));
    auto result = cadenza::audio::analyzeChordProgression(mono, sampleRate, file.getFileNameWithoutExtension().toStdString());
    if (!result.ok) {
        juce::Logger::writeToLog("[Cadenza] Chord analysis failed: " + juce::String(result.error));
        if (m_panel)
            m_panel->setChordAnalysisSummary(juce::String(result.error));
        return false;
    }

    const auto summary = juce::String(cadenza::audio::formatChordAnalysisSummary(result));
    juce::Logger::writeToLog("[Cadenza] Chord analysis completed for " + file.getFileNameWithoutExtension()
                             + ": " + juce::String(result.detectedKeyName)
                             + " transpose=" + juce::String(result.transposeToC));
    if (m_panel) {
        m_panel->setChordAnalysisSummary(summary);
        m_panel->setActivePage(1);
    }
    return true;
}

void MainComponent::setSongMode(bool enabled)
{
    m_songModeActive = enabled && m_songPlayer.hasSong();
    m_lastSongBar = -1;

    if (m_songModeActive) {
        m_songPlayer.reset();
        // Prime the active section/chord immediately so the UI/engine reflect
        // the chart even before the transport rolls. (The always-on timer in the
        // ctor drives the per-bar stepping.)
        applySongStepForBar(std::max(1, m_audio.transport().positionBar() + 1));
    } else {
        m_styleEngine.cancelSectionRequest();
    }

    const auto js = juce::String("window.JuceBridge && window.JuceBridge.onSongModeChanged && ")
                  + "window.JuceBridge.onSongModeChanged(" + boolLiteral(m_songModeActive) + ");";
    pushToWeb(js);
}

void MainComponent::applySongStepForBar(int bar, bool applySection)
{
    auto step = m_songPlayer.updateToBar(bar);

    if (applySection && step.sectionChanged) {
        m_styleEngine.allNotesOff();
        m_styleEngine.setSection(step.section);
        if (m_panel) m_panel->setActiveSection(juce::String(step.section));
    }

    if (step.chordChanged) {
        m_styleEngine.setChord(step.chord);
        const auto chordName = step.chord.toString();
        pushToWeb(juce::String("window.JuceBridge && window.JuceBridge.onChordChanged(")
                  + jsString(juce::String(chordName)) + ");");
    }
}

void MainComponent::queueSongSectionForBar(int bar)
{
    if (m_songPlayer.shouldStopAtBar(bar)) {
        m_styleEngine.requestStopAtBarBoundary();
        return;
    }

    const auto step = m_songPlayer.previewToBar(bar);
    if (step.sectionChanged)
        m_styleEngine.requestSection(step.section, false, {});
}

// ---- Style Recorder ----------------------------------------------------

juce::String MainComponent::recorderStatusText() const
{
    const auto& info = cadenza::arranger::recorderPartInfo(m_recorder.targetPart());
    const auto cfg = m_recorder.config();
    return juce::String(cfg.bars) + " bars at " + juce::String(cfg.tempo)
         + " BPM, part: " + info.label;
}

void MainComponent::recorderNewSession(int bars)
{
    if (m_songModeActive) setSongMode(false);
    if (m_state.playing()) togglePlayback();   // stop cleanly first

    cadenza::arranger::RecorderConfig cfg;
    cfg.name = "My Style";
    cfg.tempo = m_state.bpm();
    cfg.beatsPerBar = m_audio.transport().beatsPerBar();
    cfg.beatUnit = m_audio.transport().beatUnit();
    cfg.ticksPerBeat = m_audio.transport().ticksPerBeat();
    cfg.bars = std::max(1, bars);
    m_recorder.startSession(cfg);
    m_recordArmed.store(false);

    m_styleEngine.setStyle(m_recorder.snapshotStyle());
    m_styleEngine.setSection("mainA");
    m_currentMain = "mainA";
    m_audio.setMetronomeEnabled(m_metronomeOn);
    m_midi.setCaptureMode(true);   // whole keyboard plays/records the target part
    recorderPrepareTargetChannel();
    updateNativePanelStyle();
    if (m_panel)
        m_panel->setRecorderState(true, false,
            recorderStatusText() + " - press Record, then play");
    juce::Logger::writeToLog("[Cadenza] Style Recorder: new session, "
                             + juce::String(cfg.bars) + " bars at "
                             + juce::String(cfg.tempo) + " BPM");
}

void MainComponent::recorderSetBars(int bars)
{
    if (!m_recorder.sessionActive() || m_recordArmed.load())
        return;
    if (!m_recorder.setBarCount(bars))
        return;

    recorderRefreshStyle();
    recorderReloadEditor();
    if (m_panel) {
        m_panel->setRecorderBarCount(m_recorder.config().bars);
        m_panel->setRecorderState(
            true, false,
            recorderStatusText() + " - pattern length updated");
    }
}

void MainComponent::recorderSetPart(int partIndex)
{
    m_recorder.setTargetPart(cadenza::arranger::recorderPartInfo(partIndex).part);
    if (m_recorder.sessionActive())
        recorderPrepareTargetChannel();   // audible voice on the newly-selected channel
    recorderReloadEditor();
    if (m_recorder.sessionActive() && m_panel)
        m_panel->setRecorderState(true, m_recordArmed.load(),
            recorderStatusText() + " - press Record, then play");
}

void MainComponent::recorderArm(bool on)
{
    if (!m_recorder.sessionActive()) {
        if (m_panel) m_panel->setRecorderState(false, false, "Press New to record your own style");
        return;
    }

    if (on) {
        m_recorder.discardTake();
        m_recordArmed.store(true);
        if (!m_state.playing()) togglePlayback();   // loop the section + metronome
        if (m_panel)
            m_panel->setRecorderState(true, true,
                "RECORDING " + juce::String(cadenza::arranger::recorderPartInfo(
                                   m_recorder.targetPart()).label)
                + " - the loop overdubs; click Record again to keep the take");
    } else {
        m_recordArmed.store(false);
        const bool added = m_recorder.commitTake();
        recorderRefreshStyle();
        recorderReloadEditor();
        if (m_panel)
            m_panel->setRecorderState(true, false,
                added ? juce::String("Take kept - pick another part, Record again, or Save")
                      : juce::String("No notes captured - press Record and play"));
        juce::Logger::writeToLog(juce::String("[Cadenza] Style Recorder: take ")
                                 + (added ? "committed" : "empty"));
    }
}

void MainComponent::makeLoadedStyleEditable()
{
    auto current = m_styleEngine.currentStyle();
    if (!current)
        return;

    cadenza::arranger::Style editable = *current;   // edit a copy; the loaded file is untouched
    std::vector<std::string> dropped;
    const bool alreadyEditable = cadenza::arranger::isEditableCadenzaStyle(editable);
    if (!alreadyEditable)
        cadenza::arranger::makeStyleEditable(editable, &dropped);

    const auto section = bestStartSection(editable);
    if (!m_recorder.loadSession(editable, section)) {
        juce::Logger::writeToLog("[Cadenza] Make Editable failed: style not acceptable");
        return;
    }
    m_recordArmed.store(false);

    recorderRefreshStyle();   // push the editable style to the engine + refresh the panel
    if (!section.empty()) {
        m_styleEngine.setSection(section);
        if (section.rfind("main", 0) == 0)
            m_currentMain = section;
    }
    recorderReloadEditor();

    juce::String status = alreadyEditable
        ? "Editable session reopened - edit parts, Record, or Save as .cstyle"
        : "Editable - edit parts in the piano roll, then Save as .cstyle";
    if (!dropped.empty()) {
        juce::String list;
        for (const auto& d : dropped) { if (list.isNotEmpty()) list += ", "; list += juce::String(d); }
        juce::Logger::writeToLog("[Cadenza] Make Editable dropped " + juce::String((int) dropped.size())
                                 + " extra part(s): " + list);
        status = "Editable (dropped " + juce::String((int) dropped.size())
               + " extra part(s)) - Save as .cstyle";
    }
    if (m_panel) {
        m_panel->setRecorderState(true, false, status);
        m_panel->setMakeEditableAvailable(false);
    }
    juce::Logger::writeToLog("[Cadenza] Make Editable: \"" + juce::String(editable.name)
                             + (alreadyEditable ? "\" reopened for editing (section "
                                                : "\" is now editable (section ")
                             + juce::String(section) + ")");
}

void MainComponent::recorderRefreshStyle()
{
    if (!m_recorder.sessionActive()) return;

    m_styleEngine.setStyle(m_recorder.snapshotStyle());
    updateNativePanelStyle();
    applyMixerState();

    // While playing, the style swap lands on the next audio block; refresh the
    // panel once more shortly after so new mixer strips appear.
    juce::Component::SafePointer<MainComponent> safe(this);
    juce::Timer::callAfterDelay(150, [safe] {
        if (auto* self = safe.getComponent(); self && self->m_recorder.sessionActive()) {
            self->updateNativePanelStyle();
            self->applyMixerState();
        }
    });
}

void MainComponent::recorderClearPart()
{
    if (!m_recorder.sessionActive()) return;
    const bool removed = m_recorder.clearTargetPart();
    if (removed) {
        recorderRefreshStyle();
        recorderReloadEditor();
    }
    if (m_panel)
        m_panel->setRecorderState(true, false,
            removed ? juce::String("Cleared ")
                          + cadenza::arranger::recorderPartInfo(m_recorder.targetPart()).label
                    : juce::String("Nothing recorded on this part yet"));
}

void MainComponent::recorderSave()
{
    if (!m_recorder.sessionActive()) return;

    const auto defaultDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    m_recSaveChooser = std::make_unique<juce::FileChooser>(
        "Save your style (.cstyle)",
        defaultDir.getChildFile("My Style.cstyle"), "*.cstyle");

    juce::Component::SafePointer<MainComponent> safe(this);
    m_recSaveChooser->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe](const juce::FileChooser& chooser) {
            auto* self = safe.getComponent();
            if (self == nullptr) return;

            auto file = chooser.getResult();
            if (file == juce::File{}) { self->m_recSaveChooser.reset(); return; }
            if (!file.hasFileExtension("cstyle"))
                file = file.withFileExtension("cstyle");

            self->m_recorder.setStyleName(
                file.getFileNameWithoutExtension().toStdString());
            const bool ok = self->m_recorder.save(file.getFullPathName().toStdString());

            if (ok) {
                self->recorderRefreshStyle();   // pick up the new name
                if (self->m_settings) {         // come back to this style next launch
                    auto& st = self->m_settings->state();
                    st.lastStyleId = self->m_recorder.snapshotStyle()->id;
                    st.lastStylePath = file.getFullPathName().toStdString();
                    self->saveSettings();
                }
                juce::Logger::writeToLog("[Cadenza] Style Recorder: saved "
                                         + file.getFullPathName());
            }
            if (self->m_panel)
                self->m_panel->setRecorderState(true, false,
                    ok ? "Saved " + file.getFileName() + " - keep recording or Exit"
                       : "SAVE FAILED: " + file.getFullPathName());
            self->m_recSaveChooser.reset();
        });
}

void MainComponent::recorderOpenEditor()
{
    if (!m_recorder.sessionActive()) return;

    if (!m_partEditor) {
        cadenza::ui::StylePartEditorWindow::Callbacks cb;
        cb.onNotesEdited = [this](std::vector<cadenza::arranger::PatternNote> notes) {
            m_recorder.replacePartNotes(std::move(notes));
            recorderRefreshStyle();   // hear the edit immediately while looping
            recorderReloadEditor();   // reflect normalized quantize/merge result
        };
        cb.onAudition = [this](int note, int velocity) {
            const auto& info = cadenza::arranger::recorderPartInfo(m_recorder.targetPart());
            const int channel = info.midiChannel;
            if (velocity > 0) {
                m_audio.noteOn(channel, note, velocity);
                // The editor only reports presses; release the audition note shortly.
                juce::Component::SafePointer<MainComponent> safe(this);
                juce::Timer::callAfterDelay(220, [safe, channel, note] {
                    if (auto* self = safe.getComponent())
                        self->m_audio.noteOff(channel, note);
                });
            } else {
                m_audio.noteOff(channel, note);
            }
        };
        cb.onTogglePlayback = [this] {
            togglePlayback();
        };
        cb.onToggleRecord = [this] {
            recorderArm(!m_recordArmed.load());
        };
        cb.onSnapDivisionChanged = [this](int division) {
            m_recorder.setQuantizeDivision(division);
        };
        cb.onClosed = [this] {
            juce::Component::SafePointer<MainComponent> safe(this);
            juce::MessageManager::callAsync([safe] {
                if (auto* self = safe.getComponent())
                    self->m_partEditor.reset();   // not from inside the window's callback
            });
        };
        m_partEditor = std::make_unique<cadenza::ui::StylePartEditorWindow>(std::move(cb));
    }

    recorderReloadEditor();
    m_partEditor->setVisible(true);
    m_partEditor->toFront(true);
    m_partEditor->grabKeyboardFocus();
}

void MainComponent::showAiSettingsDialog()
{
    auto aw = std::make_unique<juce::AlertWindow>(
        "AI Settings",
        "Paste your Anthropic API key and choose a model. The key is stored locally on this PC.",
        juce::MessageBoxIconType::NoIcon);

    aw->addTextEditor("key", m_settings ? juce::String(m_settings->state().anthropicApiKey) : juce::String(),
                      "Anthropic API key");
    if (auto* ed = aw->getTextEditor("key"))
        ed->setPasswordCharacter((juce::juce_wchar) 0x2022);

    const juce::StringArray models { "claude-haiku-4-5", "claude-sonnet-4-6", "claude-opus-4-8" };
    aw->addComboBox("model", models, "Model (cheapest -> best)");
    if (auto* cb = aw->getComboBoxComponent("model")) {
        const juce::String cur = m_settings ? juce::String(m_settings->state().aiModel) : juce::String("claude-haiku-4-5");
        const int idx = models.indexOf(cur);
        cb->setSelectedItemIndex(idx >= 0 ? idx : 0, juce::dontSendNotification);
    }

    aw->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainComponent> safe(this);
    aw->enterModalState(true, juce::ModalCallbackFunction::create([safe](int r) {
        auto* self = safe.getComponent();
        if (self == nullptr) return;
        if (r == 1 && self->m_aiWindow && self->m_settings) {
            const juce::String key = self->m_aiWindow->getTextEditorContents("key").trim();
            juce::String model = "claude-haiku-4-5";
            if (auto* cb = self->m_aiWindow->getComboBoxComponent("model"))
                model = cb->getText();
            self->m_settings->state().anthropicApiKey = key.toStdString();
            self->m_settings->state().aiModel = model.toStdString();
            self->saveSettings();
        }
        self->m_aiWindow.reset();
    }), false);
    m_aiWindow = std::move(aw);
}

void MainComponent::showGenerateStyleDialog()
{
    if (!m_settings || m_settings->state().anthropicApiKey.empty()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "AI Style",
            "First add your Anthropic API key in AI Settings (on the Setting page).");
        return;
    }

    const bool editing = m_recorder.sessionActive();
    const juce::String title = editing ? "AI: Edit Style" : "AI Style";
    const juce::String msg = editing
        ? juce::String("Describe a change to the current style, e.g.\n"
                       "\"make the bass busier in Main B\" or \"add a clap on beat 4\".\n"
                       "(Or start with \"new \" to create a fresh style instead.)")
        : juce::String("Describe a style or name a song, e.g.\n"
                       "\"reggae like UB40 Kingston Town, 75 bpm, add claps\".");

    auto aw = std::make_unique<juce::AlertWindow>(title, msg, juce::MessageBoxIconType::NoIcon);

    aw->addTextEditor("prompt", juce::String(), editing ? "Change" : "Description");
    if (auto* ed = aw->getTextEditor("prompt")) {
        ed->setMultiLine(true);
        ed->setReturnKeyStartsNewLine(true);
        ed->setSize(380, 70);
    }
    aw->addButton("Generate", 1);
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainComponent> safe(this);
    aw->enterModalState(true, juce::ModalCallbackFunction::create([safe](int r) {
        auto* self = safe.getComponent();
        if (self == nullptr) return;
        juce::String prompt;
        if (r == 1 && self->m_aiWindow)
            prompt = self->m_aiWindow->getTextEditorContents("prompt").trim();
        self->m_aiWindow.reset();
        if (prompt.isNotEmpty())
            self->generateStyleFromText(prompt);
    }), false);
    m_aiWindow = std::move(aw);
}

void MainComponent::generateStyleFromText(const juce::String& prompt)
{
    if (!m_settings) return;
    const juce::String key   = juce::String(m_settings->state().anthropicApiKey);
    const juce::String model = juce::String(m_settings->state().aiModel);
    if (key.trim().isEmpty()) {
        if (m_panel)
            m_panel->finishAiWorking("AI skipped: add your Anthropic API key in AI Settings.");
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "AI Style",
            "First add your Anthropic API key in AI Settings (on the Setting page).");
        return;
    }

    // If an editable style is loaded, the instruction EDITS it (unless it starts
    // with "new ", which forces a fresh style). Otherwise it creates from scratch.
    const bool wantsNew = prompt.trim().startsWithIgnoreCase("new ");
    const bool editing = m_recorder.sessionActive() && !wantsNew;
    juce::String currentStyleJson;
    if (editing)
        if (auto snap = m_recorder.snapshotStyle())
            currentStyleJson = juce::String(cadenza::arranger::saveStyleToJson(*snap, true));

    const juce::String work = "AI is working - generating style... (this can take a few seconds)";
    if (!beginAiWorking(work, "AI Style..."))
        return;

    if (m_panel)
        m_panel->setRecorderState(m_recorder.sessionActive(), false, work + " Model: " + model);

    juce::Component::SafePointer<MainComponent> safe(this);
    std::thread([safe, key, model, prompt, currentStyleJson] {
        cadenza::ai::StyleGenResult result;
        try {
            result = cadenza::ai::generateStyle(key, model, prompt, currentStyleJson);
        } catch (const std::exception& e) {
            result.ok = false;
            result.error = e.what();
        } catch (...) {
            result.ok = false;
            result.error = "unknown error.";
        }
        juce::MessageManager::callAsync([safe, result] {
            if (auto* self = safe.getComponent())
                self->applyGeneratedStyle(result);
        });
    }).detach();
}

void MainComponent::applyGeneratedStyle(const cadenza::ai::StyleGenResult& result)
{
    applyGeneratedStyle(result, nullptr, AiStyleAction::None);
}

void MainComponent::applyPolishedStyle(const cadenza::ai::StyleGenResult& result,
                                       int polishedSections,
                                       int totalSections,
                                       int warningCount)
{
    const int tokens = result.inputTokens + result.outputTokens;
    if (!result.ok) {
        finishAiWorking("AI failed: " + juce::String(result.error));
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
            "AI Style", juce::String(result.error));
        return;
    }

    if (polishedSections <= 0) {
        juce::String message = "AI: nothing to polish / kept original (0 of "
            + juce::String(totalSections) + " sections polished, used "
            + juce::String(tokens) + " tokens)";
        if (warningCount > 0)
            message += " - " + juce::String(warningCount) + " section(s) kept after warnings";
        finishAiWorking(message);
        return;
    }

    auto loaded = cadenza::arranger::loadStyleFromJson(result.cstyleJson);
    if (!loaded.ok) {
        finishAiWorking("AI returned an invalid polished style; kept your original.");
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
            "AI Style", "The AI returned an invalid polished style: " + juce::String(loaded.error));
        return;
    }

    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                   .getChildFile("Cadenza AI Styles");
    dir.createDirectory();
    juce::String name = juce::String(loaded.style.name);
    if (name.isEmpty()) name = "AI Style";
    auto file = dir.getChildFile(juce::File::createLegalFileName(name) + ".cstyle");
    file.replaceWithText(juce::String(result.cstyleJson));

    if (loadAndApplyStyleFile(file)) {
        if (m_panel)
            m_panel->setActivePage(cadenza::ui::NativePanel::kEditorPage);

        juce::String message = "AI: polished " + juce::String(polishedSections)
            + " of " + juce::String(totalSections) + " sections (used "
            + juce::String(tokens) + " tokens)";
        if (warningCount > 0)
            message += " - kept " + juce::String(warningCount) + " section(s)";
        message += " - \"" + name + "\" loaded; edit & Save";
        finishAiWorking(message);
        juce::Logger::writeToLog("[Cadenza] AI style polished: " + name
                                 + " (" + juce::String(tokens) + " tokens, saved "
                                 + file.getFullPathName() + ")");
    } else {
        finishAiWorking("AI failed: could not load the polished style.");
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
            "AI Style", "Could not load the polished style.");
    }
}

void MainComponent::generateAiFillsIntroEnding()
{
    generateStyleEditAction(aiAddFillsPrompt(), "AI: generating fills...", AiStyleAction::AddFillsIntroEnding);
}

void MainComponent::polishStyleWithAi()
{
    generateStyleEditAction({}, "AI: polishing style...", AiStyleAction::Polish);
}

void MainComponent::generateStyleEditAction(const juce::String& prompt,
                                            const juce::String& status,
                                            AiStyleAction action)
{
    if (!m_settings || m_settings->state().anthropicApiKey.empty()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "AI Style",
            "First add your Anthropic API key in AI Settings (on the Setting page).");
        if (m_panel)
            m_panel->finishAiWorking("AI skipped: add your Anthropic API key in AI Settings.");
        return;
    }

    if (!m_recorder.sessionActive()) {
        if (m_panel)
            m_panel->setRecorderState(false, false,
                "AI needs an editable .cstyle loaded; import, record, or Make Editable first.");
        return;
    }

    auto originalStyle = m_recorder.snapshotStyle();
    if (originalStyle == nullptr)
        return;

    const juce::String key = juce::String(m_settings->state().anthropicApiKey);
    const juce::String model = juce::String(m_settings->state().aiModel);
    const juce::String currentStyleJson = juce::String(cadenza::arranger::saveStyleToJson(*originalStyle, true));

    const bool polishing = action == AiStyleAction::Polish;
    const juce::String work = polishing
        ? "AI is working - polishing style... (this can take a few seconds)"
        : "AI is working - generating fills... (this can take a few seconds)";
    if (!beginAiWorking(work, polishing ? "AI: Polish" : "AI: Add Fills"))
        return;

    if (m_panel)
        m_panel->setRecorderState(true, false, status + " (" + model + ")...");

    juce::Component::SafePointer<MainComponent> safe(this);
    std::thread([safe, key, model, prompt, currentStyleJson, originalStyle, action] {
        if (action == AiStyleAction::Polish) {
            cadenza::ai::StyleGenResult finalResult;
            int polishedSections = 0;
            int warningCount = 0;
            const int totalSections = static_cast<int>(originalStyle->sections.size());

            try {
                auto working = *originalStyle;
                for (int i = 0; i < totalSections; ++i) {
                    const auto sectionId = originalStyle->sections[static_cast<std::size_t>(i)].name;
                    const juce::String progress = "AI: polishing section "
                        + juce::String(i + 1) + " of " + juce::String(totalSections) + "...";
                    juce::MessageManager::callAsync([safe, progress, model] {
                        if (auto* self = safe.getComponent()) {
                            if (self->m_panel) {
                                self->m_panel->beginAiWorking(progress, "AI: Polish");
                                self->m_panel->setRecorderState(true, false, progress + " (" + model + ")");
                            }
                        }
                    });

                    const auto* section = working.findSection(sectionId);
                    if (section == nullptr) {
                        ++warningCount;
                        continue;
                    }

                    const auto promptStyle = oneSectionStyleForPrompt(working, *section);
                    const juce::String sectionJson(cadenza::arranger::saveStyleToJson(promptStyle, true));
                    auto sectionResult = cadenza::ai::generateStyleSectionsOnly(
                        key, model, aiPolishSectionPrompt(sectionId), sectionJson);
                    finalResult.inputTokens += sectionResult.inputTokens;
                    finalResult.outputTokens += sectionResult.outputTokens;

                    if (!sectionResult.ok) {
                        ++warningCount;
                        juce::Logger::writeToLog("[Cadenza] AI polish kept original "
                            + juce::String(sectionId) + ": " + juce::String(sectionResult.error));
                        continue;
                    }

                    auto merged = cadenza::ai::mergeAiPolishedSection(
                        working, sectionId, sectionResult.cstyleJson);
                    if (!merged.ok) {
                        ++warningCount;
                        juce::Logger::writeToLog("[Cadenza] AI polish rejected "
                            + juce::String(sectionId) + ": " + juce::String(merged.error));
                        continue;
                    }

                    polishedSections += merged.replacedSections;
                    working = std::move(merged.style);
                }

                finalResult.ok = true;
                if (polishedSections > 0)
                    finalResult.cstyleJson = cadenza::arranger::saveStyleToJson(working, true);
            } catch (const std::exception& e) {
                finalResult.ok = false;
                finalResult.error = e.what();
            } catch (...) {
                finalResult.ok = false;
                finalResult.error = "unknown error.";
            }

            juce::MessageManager::callAsync(
                [safe, finalResult, polishedSections, totalSections, warningCount] {
                    if (auto* self = safe.getComponent())
                        self->applyPolishedStyle(finalResult, polishedSections, totalSections, warningCount);
                });
            return;
        }

        cadenza::ai::StyleGenResult result;
        try {
            result = action == AiStyleAction::AddFillsIntroEnding
                ? cadenza::ai::generateStyleSectionsOnly(key, model, prompt, currentStyleJson)
                : cadenza::ai::generateStyle(key, model, prompt, currentStyleJson);
        } catch (const std::exception& e) {
            result.ok = false;
            result.error = e.what();
        } catch (...) {
            result.ok = false;
            result.error = "unknown error.";
        }
        juce::MessageManager::callAsync([safe, result, originalStyle, action] {
            if (auto* self = safe.getComponent())
                self->applyGeneratedStyle(result, originalStyle, action);
        });
    }).detach();
}

void MainComponent::applyGeneratedStyle(const cadenza::ai::StyleGenResult& result,
                                        std::shared_ptr<const cadenza::arranger::Style> originalStyle,
                                        AiStyleAction action)
{
    if (!result.ok) {
        finishAiWorking("AI failed: " + juce::String(result.error));
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
            "AI Style", juce::String(result.error));
        return;
    }

    cadenza::arranger::Style finalStyle;
    std::string finalJson;
    if (action == AiStyleAction::AddFillsIntroEnding) {
        if (originalStyle == nullptr) {
            finishAiWorking("AI fill generation had no original style snapshot; kept your original.");
            return;
        }

        auto merged = cadenza::ai::mergeAiGeneratedSections(*originalStyle, result.cstyleJson);
        if (!merged.ok) {
            finishAiWorking("AI fill generation returned invalid sections; kept your original.");
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                "AI Style", "The AI returned invalid sections: " + juce::String(merged.error));
            return;
        }

        finalStyle = std::move(merged.style);
        finalJson = cadenza::arranger::saveStyleToJson(finalStyle, true);
    } else {
        auto loaded = cadenza::arranger::loadStyleFromJson(result.cstyleJson);
        if (!loaded.ok) {
            finishAiWorking("AI returned an invalid style; kept your original.");
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                "AI Style", "The AI returned an invalid style: " + juce::String(loaded.error));
            return;
        }

        if (action == AiStyleAction::Polish
            && (originalStyle == nullptr
                || !cadenza::ai::validatePolishKeptStructure(*originalStyle, loaded.style))) {
            finishAiWorking("AI polish changed the style structure; kept your original.");
            return;
        }

        finalStyle = std::move(loaded.style);
        finalJson = result.cstyleJson;
    }

    // Persist it as a real .cstyle and load it through the normal path (so it
    // becomes an editable recorder session), then jump to the Editor page.
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                   .getChildFile("Cadenza AI Styles");
    dir.createDirectory();
    juce::String name = juce::String(finalStyle.name);
    if (name.isEmpty()) name = "AI Style";
    auto file = dir.getChildFile(juce::File::createLegalFileName(name) + ".cstyle");
    file.replaceWithText(juce::String(finalJson));

    const int tokens = result.inputTokens + result.outputTokens;
    if (loadAndApplyStyleFile(file)) {
        if (m_panel)
            m_panel->setActivePage(cadenza::ui::NativePanel::kEditorPage);

        juce::String resultText = "AI: style generated";
        if (action == AiStyleAction::AddFillsIntroEnding)
            resultText = "AI: fills added";
        else if (action == AiStyleAction::Polish)
            resultText = "AI: style polished";

        finishAiWorking(resultText + " (used " + juce::String(tokens) + " tokens) - \""
                        + name + "\" loaded; edit & Save");
        juce::Logger::writeToLog("[Cadenza] AI style generated: " + name
                                 + " (" + juce::String(tokens) + " tokens, saved " + file.getFullPathName() + ")");
    } else {
        finishAiWorking("AI failed: could not load the generated style.");
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
            "AI Style", "Could not load the generated style.");
    }
}

bool MainComponent::beginAiWorking(const juce::String& workingMessage, const juce::String& activeButtonText)
{
    if (m_aiInFlight) {
        if (m_panel)
            m_panel->setRecorderState(m_recorder.sessionActive(), false,
                "AI is already working - please wait for the current request to finish.");
        return false;
    }

    m_aiInFlight = true;
    if (m_panel)
        m_panel->beginAiWorking(workingMessage, activeButtonText);
    return true;
}

void MainComponent::finishAiWorking(const juce::String& resultMessage)
{
    m_aiInFlight = false;
    if (m_panel)
        m_panel->finishAiWorking(resultMessage);
}

void MainComponent::recorderReloadEditor()
{
    // Legacy pop-up window (still works if it happens to be open).
    if (m_partEditor && m_recorder.sessionActive()) {
        const auto& info = cadenza::arranger::recorderPartInfo(m_recorder.targetPart());
        const auto cfg = m_recorder.config();
        m_partEditor->setPart(info.label,
                              m_recorder.targetPartNotes(),
                              m_recorder.sectionLengthTicks(),
                              cfg.ticksPerBeat,
                              cfg.beatsPerBar,
                              cfg.beatUnit,
                              info.percussion);
        const int len = m_recorder.sectionLengthTicks();
        const bool playing = m_audio.transport().playing();
        const int tick = (playing && len > 0)
            ? m_audio.transport().positionTickInt() % len
            : 0;
        m_partEditor->setTransportState(tick, playing, m_recordArmed.load());
    }

    // Embedded Editor page (primary home).
    refreshStyleEditorPage();
}

void MainComponent::refreshStyleEditorPage()
{
    if (!m_panel) return;

    if (!m_recorder.sessionActive()) {
        m_panel->editorSetEnabled(false,
            "Open a style - for Yamaha press Make Editable - then edit it here.");
        return;
    }
    m_panel->editorSetEnabled(true);

    auto style = m_recorder.snapshotStyle();
    std::vector<std::pair<std::string, std::string>> pairs;
    if (style)
        for (const auto& b : cadenza::arranger::sectionButtonsForStyle(*style))
            pairs.emplace_back(b.sectionId, b.label);
    m_panel->editorSetSections(pairs);

    const auto cfg = m_recorder.config();
    m_panel->editorSetActiveSection(cfg.section);

    int slot = 0;
    for (int i = 0; i < cadenza::arranger::kNumRecorderParts; ++i)
        if (cadenza::arranger::recorderPartInfo(i).part == m_recorder.targetPart()) { slot = i; break; }
    m_panel->editorSetActivePart(slot);

    const auto& info = cadenza::arranger::recorderPartInfo(m_recorder.targetPart());
    m_panel->editorSetPart(m_recorder.targetPartNotes(),
                           m_recorder.sectionLengthTicks(),
                           cfg.ticksPerBeat, cfg.beatsPerBar, cfg.beatUnit,
                           info.percussion);

    const int len = m_recorder.sectionLengthTicks();
    const bool playing = m_audio.transport().playing();
    const int tick = (playing && len > 0)
        ? m_audio.transport().positionTickInt() % len
        : 0;
    m_panel->editorSetTransport(tick, playing, m_recordArmed.load());
}

void MainComponent::recorderPrepareTargetChannel()
{
    // Give the target part's channel an audible voice so playing the keyboard
    // (audition) and the recorded loop are heard. An empty recorder section
    // sends no channel setup of its own, so without this the channel can be
    // silent or stuck on a stale/wrong preset.
    const auto& info = cadenza::arranger::recorderPartInfo(m_recorder.targetPart());
    if (info.percussion) {
        // SynthEngine forces the drum bank for the drum synth channels, so a
        // program change here selects the kit (0 = standard).
        m_audio.programChange(info.midiChannel, 0);
    } else {
        m_audio.controlChange(info.midiChannel, 0, 0);    // bank MSB 0 (GM)
        m_audio.controlChange(info.midiChannel, 32, 0);   // bank LSB 0
        m_audio.programChange(info.midiChannel,
                              cadenza::midi::defaultGmProgramForRole(info.partName));
    }
    m_audio.controlChange(info.midiChannel, 7, 100);      // audible volume
}

void MainComponent::recorderCloseEditor()
{
    m_partEditor.reset();
}

void MainComponent::recorderExit()
{
    const bool hadEditableStyle = m_recorder.sessionActive();
    std::shared_ptr<const cadenza::arranger::Style> editableStyle;
    if (hadEditableStyle)
        editableStyle = m_recorder.snapshotStyle();

    m_recordArmed.store(false);
    m_midi.setCaptureMode(false);
    m_audio.setMetronomeEnabled(false);
    if (m_state.playing()) togglePlayback();
    recorderCloseEditor();
    m_recorder.endSession();

    // Back to the regular arranger while keeping the in-progress style loaded,
    // so Make Editable can reopen it without requiring a save/reload cycle.
    if (editableStyle) {
        m_styleEngine.setStyle(editableStyle);
        if (editableStyle->findSection(m_currentMain) == nullptr) {
            const auto section = bestStartSection(*editableStyle);
            if (!section.empty()) {
                m_styleEngine.setSection(section);
                if (section.rfind("main", 0) == 0)
                    m_currentMain = section;
            }
        }
    } else if (!tryLoadLastStyle()) {
        tryLoadFactoryStyle();
    }

    m_midi.setCaptureMode(false);
    m_audio.setMetronomeEnabled(false);
    updateNativePanelStyle();
    if (m_panel) {
        m_panel->setRecorderState(false, false,
            editableStyle ? "Recorder closed - press Make Editable to keep editing this style"
                          : "Press New to record your own style");
        m_panel->setMakeEditableAvailable(editableStyle != nullptr);
    }
    juce::Logger::writeToLog("[Cadenza] Style Recorder: session closed");
}

namespace {
// Classify a section id by its name prefix.
std::string sectionType(const std::string& id)
{
    auto starts = [&id](const char* p) { return id.rfind(p, 0) == 0; };
    if (starts("intro"))  return "intro";
    if (starts("fill"))   return "fill";
    if (starts("ending")) return "ending";
    return "main";
}

std::vector<std::string> sectionIdsForStyle(const cadenza::arranger::Style& style)
{
    std::vector<std::string> ids;
    ids.reserve(style.sections.size());
    for (const auto& section : style.sections)
        ids.push_back(section.name);
    return ids;
}
}

void MainComponent::startFadeOut()
{
    if (!m_state.playing())
        return;
    m_audio.startFadeOut(8.0);
    juce::Logger::writeToLog("[Cadenza] Fade out started");
}

void MainComponent::togglePlayback()
{
    const bool play = !m_state.playing();
    m_state.setPlaying(play);
    m_audio.cancelFadeOut();   // a manual start/stop overrides a running fade
    if (play) {
        if (m_songModeActive) {
            m_songPlayer.reset();
            m_lastSongBar = -1;
            applySongStepForBar(1);
            queueSongSectionForBar(2);
        }
        m_audio.play();   // restarts the transport from bar 0
    } else {
        m_audio.stop();
    }
    if (m_panel) m_panel->setPlaying(play);
    pushToWeb(juce::String("window.JuceBridge && window.JuceBridge.onPlayStateChanged(")
              + (play ? "true" : "false") + ");");
}

void MainComponent::executeControlCommand(const std::string& command)
{
    if (command == "play") {
        togglePlayback();
    } else {
        if (m_songModeActive) setSongMode(false);
        triggerSection(command);   // section id ("mainA", "fillAA", "intro", "ending", ...)
    }
}

void MainComponent::triggerSection(const std::string& id)
{
    const std::string type = sectionType(id);
    const std::string previousMain = m_currentMain.empty() ? std::string("mainA") : m_currentMain;

    // Auto Fill-In (Yamaha behavior): pressing a Main button while the band is
    // playing inserts the transition fill from the previous Main to the target
    // Main when the style has one, then lands on the selected Main.
    std::optional<std::string> autoFillId;
    if (type == "main" && m_state.autoFillEnabled() && m_audio.transport().playing()) {
        if (const auto style = m_styleEngine.currentStyle()) {
            autoFillId = cadenza::arranger::chooseAutoFill(
                previousMain, id, sectionIdsForStyle(*style));
        }
    }

    if (type == "main")
        m_currentMain = id;   // remember as the return target for fills/intros

    if (autoFillId.has_value()) {
        m_styleEngine.requestSection(*autoFillId, true, id);
        if (m_panel) m_panel->setActiveSection(juce::String(*autoFillId));
        return;
    }

    const bool once = (type != "main");   // intro / fill / ending are one-shots
    const std::string returnTo = (type == "ending")
        ? std::string()
        : (m_currentMain.empty() ? std::string("mainA") : m_currentMain);

    if (m_audio.transport().playing()) {
        // Quantized: the engine switches exactly at the next bar boundary, and
        // handles the one-shot return / ending stop itself (sample-tight).
        m_styleEngine.requestSection(id, once, returnTo);
    } else {
        // Stopped: set it up immediately so Play starts on the chosen section.
        m_styleEngine.setSection(id, once, returnTo);
        applyMixerState();
        if (type == "main")
            applyOtsLinkForSection(id);
    }
    if (m_panel) m_panel->setActiveSection(juce::String(id));   // show intent now
}

void MainComponent::timerCallback()
{
    // Hot-plug MIDI: rescan ~every 2s (20Hz timer) so a keyboard plugged in after
    // launch is picked up automatically.
    if (++m_midiRescanTicks >= 40) {
        m_midiRescanTicks = 0;
        m_midi.refreshInputs();
    }

    // Section one-shot returns / ending stops are now handled sample-tight in the
    // StyleEngine (audio thread) via its section-changed / stop callbacks.

    // Piano-roll editor: sweep the playback marker across the looping section
    // (both the legacy pop-up and the embedded Editor page).
    if (m_recorder.sessionActive()) {
        const int len = m_recorder.sectionLengthTicks();
        const bool playing = m_audio.transport().playing();
        const int tick = (playing && len > 0)
            ? m_audio.transport().positionTickInt() % len
            : 0;
        if (m_partEditor)
            m_partEditor->setTransportState(tick, playing, m_recordArmed.load());
        if (m_panel)
            m_panel->editorSetTransport(tick, playing, m_recordArmed.load());
    }

    // A fade-out that reached silence stopped the transport on the audio thread;
    // sync the UI play state here.
    if (m_audio.consumeFadeCompleted() && m_state.playing()) {
        m_state.setPlaying(false);
        if (m_panel) m_panel->setPlaying(false);
        pushToWeb("window.JuceBridge && window.JuceBridge.onPlayStateChanged(false);");
        juce::Logger::writeToLog("[Cadenza] Fade out complete; stopped");
    }

    if (!m_songModeActive)
        return;

    auto& transport = m_audio.transport();
    if (!transport.playing())
        return;

    const int bar = transport.positionBar() + 1;  // Transport is 0-based; chart is 1-based.
    if (bar == m_lastSongBar)
        return;

    m_lastSongBar = bar;
    // Consume the current chart state without re-switching its section after the
    // boundary, then queue the following section one full bar ahead.
    applySongStepForBar(bar, false);
    queueSongSectionForBar(bar + 1);
}

void MainComponent::openPluginFileChooser()
{
    // Default to the project root so the bundled VST3/ and NeuralPi/ folders are visible.
    const auto projectRoot = findResourcesRoot().getParentDirectory();
    const auto startDir = projectRoot.isDirectory()
        ? projectRoot
        : juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory);

    m_pluginChooser = std::make_unique<juce::FileChooser>(
        "Choose a VST3 plugin (.vst3)", startDir, "*.vst3");

    juce::Component::SafePointer<MainComponent> safe(this);
    m_pluginChooser->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::canSelectDirectories,
        [safe](const juce::FileChooser& chooser) {
            if (auto* self = safe.getComponent()) {
                const auto file = chooser.getResult();
                if (file.exists())
                    self->loadAndApplyPluginFile(file);
                self->m_pluginChooser.reset();
            }
        });
}

bool MainComponent::loadAndApplyPluginFile(const juce::File& file)
{
    if (!file.exists() || !isSupportedPluginFile(file)) {
        juce::Logger::writeToLog("[Cadenza] Not a .vst3 plugin: " + file.getFullPathName());
        return false;
    }

    std::string error;
    const bool ok = m_audio.loadMasterEffect(file.getFullPathName().toStdString(), error);
    if (!ok) {
        juce::Logger::writeToLog("[Cadenza] VST3 load failed: " + juce::String(error));
        pushPluginStateToWeb();
        return false;
    }

    juce::Logger::writeToLog("[Cadenza] Master effect loaded: "
                             + juce::String(m_audio.masterEffectName())
                             + " (" + file.getFullPathName() + ")");
    pushPluginStateToWeb();
    return true;
}

void MainComponent::clearMasterEffect()
{
    m_audio.clearMasterEffect();
    juce::Logger::writeToLog("[Cadenza] Master effect cleared");
    pushPluginStateToWeb();
}

void MainComponent::pushPluginStateToWeb()
{
    const auto name = m_audio.hasMasterEffect()
        ? juce::String(m_audio.masterEffectName())
        : juce::String();
    pushToWeb(juce::String("window.JuceBridge && window.JuceBridge.onPluginChanged && ")
              + "window.JuceBridge.onPluginChanged(" + jsString(name) + ");");
}

void MainComponent::applyRightHand()
{
    if (!m_settings) return;
    const auto& st = m_settings->state();
    for (int i = 0; i < cadenza::midi::MidiRouter::kNumRightLayers; ++i) {
        const auto& layer = st.rightLayers[i];
        m_midi.setRightLayerEnabled(i, layer.enabled);
        m_midi.setRightLayerOctave(i, layer.octave);
        const int channel = m_midi.rightLayerChannel(i);
        if (!layer.pluginPath.empty()) {
            // VST3 voice for this layer (notes route to the plugin by channel).
            std::string err;
            if (!m_audio.loadPartInstrument(channel, layer.pluginPath, err))   // idempotent
                juce::Logger::writeToLog("[Cadenza] Right " + juce::String(i + 1)
                                         + " VST load failed: " + juce::String(err));
        } else {
            m_audio.clearPartInstrument(channel);            // back to the GM SoundFont
            // Right voices are GM programs, so reset any XG/style bank state
            // inherited by a reused channel before selecting their instrument.
            m_audio.controlChange(channel, 0, 0);             // bank MSB 0 (GM)
            m_audio.controlChange(channel, 32, 0);            // bank LSB 0
            m_audio.programChange(channel, layer.program);   // GM voice
        }
        m_audio.controlChange(channel, 7 /*CC7 volume/gain*/, layer.volume);
        juce::Logger::writeToLog("[Cadenza] Right " + juce::String(i + 1)
            + (layer.enabled ? " ON" : " off")
            + (layer.pluginPath.empty() ? (" program=" + juce::String(layer.program)) : juce::String(" VST"))
            + " vol=" + juce::String(layer.volume)
            + " oct=" + juce::String(layer.octave)
            + " ch=" + juce::String(channel));
    }
}

void MainComponent::applyLeftVoice()
{
    m_midi.setLeftEnabled(m_leftEnabled);
    m_midi.setLeftOctave(m_leftOctave);
    const int ch = m_midi.leftChannel();
    m_audio.clearPartInstrument(ch);                 // GM SoundFont voice
    m_audio.controlChange(ch, 0, 0);                 // bank MSB 0 (GM)
    m_audio.controlChange(ch, 32, 0);                // bank LSB 0
    m_audio.programChange(ch, m_leftProgram);
    m_audio.controlChange(ch, 7 /*volume*/, m_leftVolume);
}

void MainComponent::sendPitchBendToManual(int value14)
{
    for (int i = 0; i < cadenza::midi::MidiRouter::kNumRightLayers; ++i)
        m_audio.pitchBend(m_midi.rightLayerChannel(i), value14);
    m_audio.pitchBend(m_midi.leftChannel(), value14);
}

void MainComponent::sendModToManual(int value)
{
    for (int i = 0; i < cadenza::midi::MidiRouter::kNumRightLayers; ++i)
        m_audio.controlChange(m_midi.rightLayerChannel(i), 1 /*mod*/, value);
    m_audio.controlChange(m_midi.leftChannel(), 1 /*mod*/, value);
}

void MainComponent::buildNativePanel()
{
    m_panel = std::make_unique<cadenza::ui::NativePanel>();
    addAndMakeVisible(*m_panel);

    cadenza::ui::NativePanel::Callbacks cb;

    cb.togglePlay   = [this] { togglePlayback(); };
    cb.openStyle     = [this] { openStyleFileChooser(); };
    cb.importMidiStyle = [this] { openMidiStyleImportChooser(); };
    cb.openSoundFont = [this] { openSoundFontFileChooser(); };
    cb.openAudioSettings = [this] { showAudioSettings(); };
    cb.openMidiSettings  = [this] { showMidiSettings(); };
    cb.openChordAnalysis = [this] { openChordAnalysisChooser(); };

    cb.onLoadInstrumentPlugin = [this](int channel) { choosePartInstrument(channel); };
    cb.onOpenInstrumentEditor = [this](int channel) { m_audio.showPartInstrumentEditor(channel); };
    cb.onSetDefaultVoice      = [this](int channel) { setDefaultVoice(channel); };
    cb.onClearInstrumentPlugin = [this](int channel) {
        m_audio.clearPartInstrument(channel);   // back to the GM SoundFont
        if (m_panel) {
            const juce::String insName = (channel == 10)
                ? juce::String("Drum Kit")
                : juce::String(cadenza::midi::gmInstrumentName(m_mixer.program(channel)));
            m_panel->setMixerInstrumentName(channel, insName);
        }
        persistStyleMix();   // remember "GM" for this channel
    };
    cb.onSetTempo = [this](int target) {
        const int bpm = m_state.setBpm(target);
        m_audio.setBpm(static_cast<double>(bpm));
        if (m_panel) m_panel->setBpm(bpm);
        pushToWeb("window.JuceBridge && window.JuceBridge.setBPM(" + juce::String(bpm) + ");");
        saveSettings();
    };
    cb.nudgeTempo = [this](int delta) {
        const int bpm = m_state.setBpm(m_state.bpm() + delta);
        m_audio.setBpm(static_cast<double>(bpm));
        if (m_panel) m_panel->setBpm(bpm);
        pushToWeb("window.JuceBridge && window.JuceBridge.setBPM(" + juce::String(bpm) + ");");
        saveSettings();
    };
    cb.onKeyboardNote = [this](int note, int velocity, bool isOn) {
        m_midi.injectNote(note, velocity, isOn);   // full path: chord detect + live melody
    };
    cb.onPageChanged = [this](int page) {
        // An editable .cstyle keeps a recorder session available in the
        // background. Only its Style/Editor workspaces should redirect the
        // whole keyboard to the selected recorder part.
        const bool recorderPage = page == cadenza::ui::NativePanel::kStylePage
            || page == cadenza::ui::NativePanel::kEditorPage;
        const bool recorderActive = m_recorder.sessionActive() && recorderPage;
        m_midi.setCaptureMode(recorderActive);
        m_audio.setMetronomeEnabled(recorderActive && m_metronomeOn);
    };
    cb.onMixerVolume = [this](int channel, int volume) {
        m_mixer.setVolume(channel, volume);
        applyMixerState();
        persistStyleMix();
    };
    cb.onMixerMute = [this](int channel, bool mute) {
        m_mixer.setMute(channel, mute);
        applyMixerState();
        persistStyleMix();
    };
    cb.onMixerSolo = [this](int channel, bool solo) {
        m_mixer.setSolo(channel, solo);
        applyMixerState();
        persistStyleMix();
    };
    cb.onMixerInstrument = [this](int channel, int program) {
        m_mixer.setProgram(channel, program);
        applyMixerState();   // re-asserts program + volume on the channel
        persistStyleMix();
        if (m_panel)
            m_panel->setMixerInstrumentName(channel, juce::String(cadenza::midi::gmInstrumentName(program)));
    };
    cb.onPad = [this](int index) {
        // Trigger a one-shot GM percussion hit on the drum channel (10).
        static const int padNotes[4] = { 49 /*crash*/, 39 /*hand clap*/, 54 /*tambourine*/, 56 /*cowbell*/ };
        if (index >= 0 && index < 4)
            m_audio.noteOn(10, padNotes[index], 112);
    };

    cb.onEqChanged = [this](int lowDb, int midDb, int highDb) {
        m_audio.setEqGains(static_cast<float>(lowDb),
                           static_cast<float>(midDb),
                           static_cast<float>(highDb));
        if (m_settings) {
            m_settings->state().eqLowDb  = lowDb;
            m_settings->state().eqMidDb  = midDb;
            m_settings->state().eqHighDb = highDb;
            saveSettings();
        }
    };

    cb.onCompChanged = [this](int amount) {
        m_audio.setCompAmount(amount);
        if (m_settings) {
            m_settings->state().compAmount = amount;
            saveSettings();
        }
    };

    cb.onMasterChanged = [this](int volume) {
        m_audio.setMasterVolume(volume);
        if (m_settings) {
            m_settings->state().masterVolume = volume;
            saveSettings();
        }
    };

    cb.onReverbChanged = [this](int amount) {
        m_audio.setReverbLevel(amount);
        if (m_settings) {
            m_settings->state().reverbLevel = amount;
            saveSettings();
        }
    };

    cb.onPolishedMaster = [this](bool on) {
        m_audio.setPolishedMaster(on);
        if (m_settings) {
            m_settings->state().polishedMaster = on;
            saveSettings();
        }
    };

    cb.onDrumsChanged = [this](int volume) {
        // Quick drum-channel level (cadenza channel 10): mirror the drum mixer strip.
        if (!m_mixer.has(10)) return;
        m_mixer.setVolume(10, volume);
        applyMixerState();
        if (m_panel) m_panel->updateMixerStrip(10, volume, m_mixer.mute(10), m_mixer.solo(10));
        persistStyleMix();
    };

    cb.onSplitChanged = [this](int note) {
        m_midi.setSplitPoint(note);                 // notes < split drive chords; >= play melody
        if (m_settings) {
            m_settings->state().splitNote = note;
            saveSettings();
        }
    };

    // --- Right 1/2/3 layered right-hand voices ---
    cb.onRightEnabled = [this](int layer, bool on) {
        if (!m_settings || layer < 0 || layer >= 3) return;
        m_settings->state().rightLayers[layer].enabled = on;
        applyRightHand();
        if (!on) m_audio.allNotesOff();   // release anything the layer was holding
        saveSettings();
    };
    cb.onRightInstrument = [this](int layer, int program) {
        if (!m_settings || layer < 0 || layer >= 3) return;
        auto& L = m_settings->state().rightLayers[layer];
        L.program = program;
        L.pluginPath.clear();                                           // switch back to a GM voice
        if (layer == 0) m_settings->state().melodyProgram = program;   // keep the mirror in sync
        applyRightHand();                                              // clears any VST, sets the GM program
        if (m_panel)
            m_panel->setRightVoiceName(layer, juce::String(cadenza::midi::gmInstrumentName(program)), false);
        saveSettings();
    };
    cb.onRightLoadPlugin = [this](int layer) { chooseRightLayerInstrument(layer); };
    cb.onRightOpenEditor = [this](int layer) {
        m_audio.showPartInstrumentEditor(m_midi.rightLayerChannel(layer));
    };
    cb.onRightVolume = [this](int layer, int volume) {
        if (!m_settings || layer < 0 || layer >= 3) return;
        m_settings->state().rightLayers[layer].volume = volume;
        applyRightHand();
        saveSettings();
    };
    cb.onRightOctave = [this](int layer, int delta) {
        if (!m_settings || layer < 0 || layer >= 3) return;
        auto& L = m_settings->state().rightLayers[layer];
        L.octave = juce::jlimit(-2, 2, L.octave + delta);
        applyRightHand();
        if (m_panel)
            m_panel->setRightVoice(layer, L.enabled, L.program, L.volume, L.octave);
        saveSettings();
    };

    // --- Left split voice + pitch/mod wheels ---
    cb.onLeftEnabled = [this](bool on) {
        m_leftEnabled = on;
        applyLeftVoice();
        if (!on) m_audio.allNotesOff();   // release anything the left voice was holding
    };
    cb.onLeftInstrument = [this](int program) {
        m_leftProgram = program;
        applyLeftVoice();
        if (m_panel) m_panel->setLeftVoice(m_leftEnabled, m_leftProgram, m_leftVolume, m_leftOctave);
    };
    cb.onLeftVolume = [this](int volume) {
        m_leftVolume = volume;
        applyLeftVoice();
    };
    cb.onLeftOctave = [this](int delta) {
        m_leftOctave = juce::jlimit(-2, 2, m_leftOctave + delta);
        m_midi.setLeftOctave(m_leftOctave);
        if (m_panel) m_panel->setLeftVoice(m_leftEnabled, m_leftProgram, m_leftVolume, m_leftOctave);
    };
    cb.onPitchBend  = [this](int v14) { sendPitchBendToManual(v14); };
    cb.onModulation = [this](int v)   { sendModToManual(v); };

    cb.onAiStyle    = [this] { showGenerateStyleDialog(); };
    cb.onAiAddFills = [this] { generateAiFillsIntroEnding(); };
    cb.onAiPolish   = [this] { polishStyleWithAi(); };
    cb.onAiSettings = [this] { showAiSettingsDialog(); };

    cb.onStoreRegistration  = [this](int slot) { captureRegistration(slot); };
    cb.onRecallRegistration = [this](int slot) { recallRegistration(slot); };

    cb.onRecNew   = [this](int bars) { recorderNewSession(bars); };
    cb.onRecBars  = [this](int bars) { recorderSetBars(bars); };
    cb.onRecPart  = [this](int idx)  { recorderSetPart(idx); };
    cb.onRecMetronome = [this](bool on) {
        m_metronomeOn = on;
        if (!m_recorder.sessionActive()) return;
        const int page = m_panel ? m_panel->activePage() : 0;
        const bool recorderPage = page == cadenza::ui::NativePanel::kStylePage
            || page == cadenza::ui::NativePanel::kEditorPage;
        m_audio.setMetronomeEnabled(on && recorderPage);
    };
    cb.onRecArm   = [this](bool on)  { recorderArm(on); };
    cb.onRecEdit  = [this] {
        recorderReloadEditor();   // make sure the page reflects the current part/section
        if (m_panel) m_panel->setActivePage(cadenza::ui::NativePanel::kEditorPage);
    };
    cb.onRecClear = [this] { recorderClearPart(); };
    cb.onRecSave  = [this] { recorderSave(); };
    cb.onRecExit  = [this] { recorderExit(); };
    cb.onMakeEditable = [this] { makeLoadedStyleEditable(); };

    // --- Style Editor page ---
    cb.onEditorNotesEdited = [this](std::vector<cadenza::arranger::PatternNote> notes) {
        m_recorder.replacePartNotes(std::move(notes));
        recorderRefreshStyle();   // hear the edit immediately while looping
        recorderReloadEditor();   // reflect normalized quantize/merge result
    };
    cb.onEditorAudition = [this](int note, int velocity) {
        const auto& info = cadenza::arranger::recorderPartInfo(m_recorder.targetPart());
        const int channel = info.midiChannel;
        if (velocity > 0) {
            m_audio.noteOn(channel, note, velocity);
            juce::Component::SafePointer<MainComponent> safe(this);
            juce::Timer::callAfterDelay(220, [safe, channel, note] {
                if (auto* self = safe.getComponent())
                    self->m_audio.noteOff(channel, note);
            });
        } else {
            m_audio.noteOff(channel, note);
        }
    };
    cb.onEditorTogglePlay   = [this] { togglePlayback(); };
    cb.onEditorToggleRecord = [this] { recorderArm(!m_recordArmed.load()); };
    cb.onEditorSnapDivisionChanged = [this](int division) {
        m_recorder.setQuantizeDivision(division);
    };
    cb.onEditorPickSection = [this](std::string id) {
        if (m_recorder.setEditSection(id))
            recorderReloadEditor();
    };
    cb.onEditorPickPart = [this](int slot) { recorderSetPart(slot); };
    cb.onEditorSave = [this] { recorderSave(); };

    cb.onOts = [this](int slot) { applyOts(slot); };
    cb.setOtsLink = [this](bool on) {
        if (!m_settings) return;
        m_settings->state().otsLinkEnabled = on;
        saveSettings();
    };

    cb.nudgeTranspose = [this](int delta) {
        const int t = m_state.setTranspose(m_state.transpose() + delta);
        m_styleEngine.setGlobalTranspose(t);                 // transpose affects style
        m_midi.setLiveTranspose(t);                          // ...and the right-hand melody
        if (m_panel) m_panel->setTranspose(t);
        pushToWeb("window.JuceBridge && window.JuceBridge.setTranspose(" + juce::String(t) + ");");
        saveSettings();
    };
    cb.nudgeOctave = [this](int delta) {
        const int o = m_state.setOctave(m_state.octave() + delta);
        m_midi.setLiveOctave(o);                             // octave affects live melody only
        if (m_panel) m_panel->setOctave(o);
        pushToWeb("window.JuceBridge && window.JuceBridge.setOctave(" + juce::String(o) + ");");
        juce::Logger::writeToLog("[Cadenza] native octave -> " + juce::String(o));
        saveSettings();
    };

    cb.setArranger = [this](bool on) {
        if (!on) m_styleEngine.allNotesOff();
        m_styleEngine.setEnabled(on);
        m_state.setChordSourceEnabled("arranger", on);
        saveSettings();
    };
    cb.setChordMemory = [this](bool on) {
        m_midi.setChordMemory(on);
        m_state.setChordSourceEnabled("memory", on);
        saveSettings();
    };
    cb.setSyncroStop = [this](bool on) {
        m_midi.setSyncroStopOnRelease(on);
        m_state.setSyncroStopOnRelease(on);
        saveSettings();
    };
    cb.setAutoFill = [this](bool on) {
        m_state.setAutoFillEnabled(on);
        saveSettings();
    };
    cb.fadeOut = [this] { startFadeOut(); };
    cb.setFingeredOnBass = [this](bool on) {
        m_midi.setChordDetectionMode(on ? arranger::ChordDetectionMode::FingeredOnBass
                                        : arranger::ChordDetectionMode::Fingered);
        m_state.setChordSourceEnabled("bass", on);
        saveSettings();
    };
    cb.selectSection = [this](const std::string& id) {
        if (m_songModeActive) setSongMode(false);   // manual section control overrides song mode
        triggerSection(id);   // switch section + handle one-shot intro/fill/ending transitions
        saveSettings();
    };

    m_panel->setCallbacks(std::move(cb));

    // Reflect current persisted state on the panel.
    m_panel->setTranspose(m_state.transpose());
    m_panel->setOctave(m_state.octave());
    m_panel->setBpm(m_state.bpm());
    m_panel->setPlaying(m_state.playing());
    m_panel->setToggleStates(m_state.chordSourceEnabled("arranger"),
                             m_state.chordSourceEnabled("memory"),
                             m_state.syncroStopOnRelease(),
                             m_state.chordSourceEnabled("bass"),
                             m_state.autoFillEnabled());
    if (m_settings) {
        const auto& st = m_settings->state();
        m_panel->setEqGains(st.eqLowDb, st.eqMidDb, st.eqHighDb);
        m_panel->setCompAmount(st.compAmount);
        m_panel->setMasterVolume(st.masterVolume);
        m_panel->setReverbAmount(st.reverbLevel);
        m_panel->setPolishedMaster(st.polishedMaster);
        m_panel->setSplitPoint(st.splitNote);
        for (int i = 0; i < 3; ++i) {
            const auto& L = st.rightLayers[i];
            m_panel->setRightVoice(i, L.enabled, L.program, L.volume, L.octave);
        }
        m_panel->setLeftVoice(m_leftEnabled, m_leftProgram, m_leftVolume, m_leftOctave);
        for (int i = 0; i < cadenza::settings::Settings::kNumRegistrations; ++i)
            m_panel->setRegistrationUsed(i, st.registrations[i].used);
        m_panel->setOtsLinkEnabled(st.otsLinkEnabled);
    }
    refreshOtsAvailability();
    resized();
}

void MainComponent::updateNativePanelStyle()
{
    if (!m_panel) return;

    auto style = m_styleEngine.currentStyle();
    const juce::String editorHint =
        "Open a style - for Yamaha press Make Editable - then edit it here.";

    if (!style) {
        m_panel->setStyleName({});
        m_panel->setSections({});
        m_panel->setMakeEditableAvailable(false);
        m_panel->editorSetEnabled(false, editorHint);
        refreshOtsAvailability();
        m_lastLinkedMain.clear();
        return;
    }

    // Offer "Make Editable" for an imported Yamaha style, or to reopen an
    // inactive native Cadenza style that is still loaded after recorder Exit.
    m_panel->setMakeEditableAvailable(
        (style->yamahaFormat != cadenza::arranger::YamahaStyleFormat::Unknown
         || cadenza::arranger::isEditableCadenzaStyle(*style))
        && !m_recorder.sessionActive());

    // Disable the Editor page when nothing editable is loaded. (When a session is
    // active, the page is reloaded via recorderReloadEditor, not here, so live note
    // edits don't reset the piano-roll view.)
    if (!m_recorder.sessionActive())
        m_panel->editorSetEnabled(false, editorHint);

    m_panel->setStyleName(juce::String(style->name.empty() ? style->id : style->name));
    m_panel->setBpm(m_state.bpm());   // reflect the style's tempo (set in loadAndApplyStyleFile)

    std::vector<std::pair<std::string, std::string>> pairs;
    for (const auto& b : cadenza::arranger::sectionButtonsForStyle(*style))
        pairs.emplace_back(b.sectionId, b.label);
    m_panel->setSections(pairs);
    m_panel->setActiveSection(juce::String(m_styleEngine.currentSection()));
    refreshOtsAvailability();
    m_lastLinkedMain.clear();   // a new style means OTS Link starts fresh

    // --- Assign a free channel to each Right 1/2/3 layer that the style does NOT
    //     use, so the right-hand voices never collide with style parts. ---
    bool used[17] = { false };
    for (const auto& section : style->sections)
        for (const auto& part : section.parts)
            if (part.midiChannel >= 1 && part.midiChannel <= 16)
                used[part.midiChannel] = true;

    constexpr int kLayers = cadenza::midi::MidiRouter::kNumRightLayers;
    int oldRightCh[kLayers];
    for (int i = 0; i < kLayers; ++i)
        oldRightCh[i] = m_midi.rightLayerChannel(i);

    int layerChannel[kLayers];
    int lastFree = 1;
    for (int i = 0; i < kLayers; ++i) {
        int ch = lastFree;
        for (int c = lastFree; c <= 16; ++c) {
            if (c == 9 || c == 10 || used[c]) continue;   // skip both drum channels (RHY1/RHY2) and style parts
            ch = c; break;
        }
        used[ch] = true;                        // don't hand the same channel to two layers
        layerChannel[i] = ch;
        lastFree = ch + 1;
        m_midi.setRightLayerChannel(i, ch);
    }
    // If a layer moved to a new channel, clear any VST left on its old channel so
    // it doesn't linger (applyRightHand reloads the VST on the new channel).
    for (int i = 0; i < kLayers; ++i)
        if (oldRightCh[i] != layerChannel[i])
            m_audio.clearPartInstrument(oldRightCh[i]);
    const int melodyChannel = layerChannel[0];   // Right 1 = the primary "Melody" strip
    applyRightHand();   // assert each layer's enable/program/volume/octave on its channel

    // Left split voice gets its own free channel too (never a style/drum channel).
    {
        int leftCh = lastFree;
        for (int c = lastFree; c <= 16; ++c) {
            if (c == 9 || c == 10 || used[c]) continue;
            leftCh = c; break;
        }
        used[leftCh] = true;
        const int oldLeft = m_midi.leftChannel();
        if (oldLeft != leftCh) m_audio.clearPartInstrument(oldLeft);
        m_midi.setLeftChannel(leftCh);
        applyLeftVoice();
    }

    // --- Mixer: live melody + one strip per distinct style channel ---
    struct StripSeed { int channel; int volume; int program; };
    std::vector<int> channels { melodyChannel };
    std::vector<std::pair<int, std::string>> labels { { melodyChannel, "Melody" } };
    std::vector<StripSeed> seeds;   // channel -> style part volume + instrument program
    const int melodyProgram = m_settings ? m_settings->state().melodyProgram : 0;
    seeds.push_back({ melodyChannel, 100, melodyProgram });
    for (const auto& section : style->sections) {
        for (const auto& part : section.parts) {
            const int ch = part.midiChannel;
            if (std::find(channels.begin(), channels.end(), ch) != channels.end())
                continue;
            channels.push_back(ch);
            labels.emplace_back(ch, part.name.empty() ? ("Ch " + std::to_string(ch)) : part.name);
            // Instrument: the style's own program, else the standard GM role default.
            const int program = part.program.value_or(cadenza::midi::defaultGmProgramForRole(part.name));
            seeds.push_back({ ch, part.volume.value_or(100), program });
        }
    }

    m_mixer.setChannels(channels);
    for (const auto& s : seeds) {
        m_mixer.setVolume(s.channel, s.volume);
        m_mixer.setProgram(s.channel, s.program);
    }

    // Re-apply the player's saved per-style tweaks on top of the defaults.
    // Reconcile per-part VST instruments instead of tearing every plugin down and
    // rebuilding it on each style switch (slow, froze the UI): keep a channel's
    // plugin if the new style wants the same one, and only clear ones that change
    // or are no longer used. applyStyleMix then loads the rest (loadPartInstrument
    // is idempotent, so unchanged plugins are a no-op).
    if (m_settings) {
        std::map<int, std::string> desired;
        const auto it = m_settings->state().styleMixes.find(m_settings->state().lastStyleId);
        if (it != m_settings->state().styleMixes.end())
            for (const auto& m : it->second)
                if (!m.pluginPath.empty())
                    desired[m.channel] = m.pluginPath;

        for (int ch = 1; ch <= 16; ++ch) {
            const auto d = desired.find(ch);
            const std::string want = (d == desired.end()) ? std::string() : d->second;
            if (m_audio.partInstrumentPath(ch) != want)
                m_audio.clearPartInstrument(ch);   // changed or no longer wanted
        }
        applyStyleMix(m_settings->state().lastStyleId);
        applyVoiceMapToStyle();   // auto-SFZ voices on channels with no per-style override
    } else {
        m_audio.clearAllPartInstruments();
    }

    m_panel->setMixerChannels(labels);
    for (int ch : channels) {
        m_panel->updateMixerStrip(ch, m_mixer.volume(ch), m_mixer.mute(ch), m_mixer.solo(ch));
        const juce::String insName = (ch == 10)
            ? juce::String("Drum Kit")
            : (ch == 9)
                ? juce::String("Drum Kit (Rhythm 2)")
                : juce::String(cadenza::midi::gmInstrumentName(m_mixer.program(ch)));
        m_panel->setMixerInstrumentName(ch, insName);
    }

    // Sync the quick Drums knob to the drum channel's volume.
    m_panel->setDrumsLevel(m_mixer.has(10) ? m_mixer.volume(10) : 100);

    // Refresh the Right 1/2/3 voice strips (VST name if a plugin is loaded on the
    // layer's new channel, else the GM voice name).
    if (m_settings) {
        for (int i = 0; i < 3; ++i) {
            const auto& L = m_settings->state().rightLayers[i];
            m_panel->setRightVoice(i, L.enabled, L.program, L.volume, L.octave);
            if (!L.pluginPath.empty())
                m_panel->setRightVoiceName(i, juce::String(m_audio.partInstrumentName(m_midi.rightLayerChannel(i))), true);
            else
                m_panel->setRightVoiceName(i, juce::String(cadenza::midi::gmInstrumentName(L.program)), false);
        }
    }

    applyMixerState();
}

void MainComponent::applyMixerState()
{
    for (const auto& c : m_mixer.channels()) {
        // Re-assert the strip's instrument (program) over the style's own setup,
        // then its effective volume. On the drum channel a program selects the kit.
        m_audio.programChange(c.channel, c.program);
        m_audio.controlChange(c.channel, 7 /*CC7 volume*/, m_mixer.effectiveVolume(c.channel));
    }
}

void MainComponent::applyStyleMix(const std::string& styleId)
{
    if (!m_settings || styleId.empty())
        return;
    const auto it = m_settings->state().styleMixes.find(styleId);
    if (it == m_settings->state().styleMixes.end())
        return;

    // Apply the player's saved tweaks on top of the style's own defaults.
    for (const auto& m : it->second) {
        if (!m_mixer.has(m.channel))
            continue;   // only channels this style actually uses
        if (m.program >= 0) m_mixer.setProgram(m.channel, m.program);
        if (m.volume  >= 0) m_mixer.setVolume(m.channel, m.volume);
        m_mixer.setMute(m.channel, m.mute);
        m_mixer.setSolo(m.channel, m.solo);

        // Restore a saved per-part VST3 instrument for this channel.
        if (!m.pluginPath.empty()) {
            std::string err;
            if (m_audio.loadPartInstrument(m.channel, m.pluginPath, err)) {
                if (m_panel)
                    m_panel->setMixerInstrumentName(
                        m.channel, juce::String(m_audio.partInstrumentName(m.channel)));
            } else {
                juce::Logger::writeToLog("[Cadenza] Saved part instrument missing/failed ch="
                                         + juce::String(m.channel) + ": " + juce::String(err));
            }
        }
    }
}

void MainComponent::persistStyleMix()
{
    if (!m_settings)
        return;
    const std::string styleId = m_settings->state().lastStyleId;
    if (styleId.empty())
        return;

    const int melodyChannel = m_midi.melodyChannel();  // persists via melodyProgram, skip here
    std::vector<cadenza::settings::StyleChannelMix> mix;
    for (const auto& c : m_mixer.channels()) {
        if (c.channel == melodyChannel)
            continue;
        cadenza::settings::StyleChannelMix m;
        m.channel = c.channel;
        m.program = m_mixer.program(c.channel);
        m.volume  = m_mixer.volume(c.channel);
        m.mute    = m_mixer.mute(c.channel);
        m.solo    = m_mixer.solo(c.channel);
        m.pluginPath = m_audio.partInstrumentPath(c.channel);
        mix.push_back(m);
    }
    m_settings->state().styleMixes[styleId] = std::move(mix);
    saveSettings();
}

void MainComponent::captureRegistration(int slot)
{
    if (!m_settings || slot < 0 || slot >= cadenza::settings::Settings::kNumRegistrations)
        return;
    auto& st = m_settings->state();
    auto& r = st.registrations[slot];
    r.used      = true;
    r.styleId   = st.lastStyleId;
    r.stylePath = st.lastStylePath;
    r.bpm       = m_state.bpm();
    r.transpose = m_state.transpose();
    r.octave    = m_state.octave();
    r.splitNote = st.splitNote;
    r.eqLowDb = st.eqLowDb; r.eqMidDb = st.eqMidDb; r.eqHighDb = st.eqHighDb; r.compAmount = st.compAmount;
    r.bankMemory = st.bankMemory;
    r.chordArrangerEnabled = m_state.chordSourceEnabled("arranger");
    r.chordMemoryEnabled   = m_state.chordSourceEnabled("memory");
    r.chordBassEnabled     = m_state.chordSourceEnabled("bass");
    r.syncroStopOnRelease  = m_state.syncroStopOnRelease();
    r.autoFillEnabled      = m_state.autoFillEnabled();
    for (int i = 0; i < 3; ++i) r.rightLayers[i] = st.rightLayers[i];
    saveSettings();
    if (m_panel) m_panel->setRegistrationUsed(slot, true);
    juce::Logger::writeToLog("[Cadenza] Stored registration " + juce::String(slot + 1));
}

void MainComponent::recallRegistration(int slot)
{
    if (!m_settings || slot < 0 || slot >= cadenza::settings::Settings::kNumRegistrations)
        return;
    const auto r = m_settings->state().registrations[slot];   // copy: loading a style mutates settings
    if (!r.used)
        return;

    // 1) Reload the style first (resets tempo/mix), then override below.
    if (!r.styleId.empty()) {
        if (!selectStyleById(r.styleId) && !r.stylePath.empty())
            loadAndApplyStyleFile(juce::File(juce::String(r.stylePath)));
    }

    auto& st = m_settings->state();
    // 2) Tempo / transpose / octave.
    m_audio.setBpm(m_state.setBpm(r.bpm));
    m_state.setTranspose(r.transpose);
    m_state.setOctave(r.octave);
    // 3) Split / EQ / compressor.
    st.splitNote = r.splitNote;
    m_midi.setSplitPoint(r.splitNote);
    st.eqLowDb = r.eqLowDb; st.eqMidDb = r.eqMidDb; st.eqHighDb = r.eqHighDb; st.compAmount = r.compAmount;
    m_audio.setEqGains(static_cast<float>(r.eqLowDb), static_cast<float>(r.eqMidDb), static_cast<float>(r.eqHighDb));
    m_audio.setCompAmount(r.compAmount);
    // 4) Chord modes.
    m_state.setChordSourceEnabled("arranger", r.chordArrangerEnabled);
    m_state.setChordSourceEnabled("memory",   r.chordMemoryEnabled);
    m_state.setChordSourceEnabled("bass",     r.chordBassEnabled);
    m_state.setSyncroStopOnRelease(r.syncroStopOnRelease);
    m_state.setAutoFillEnabled(r.autoFillEnabled);
    // 5) Right-hand layers + bank.
    st.bankMemory = r.bankMemory;
    for (int i = 0; i < 3; ++i) st.rightLayers[i] = r.rightLayers[i];
    st.melodyProgram = st.rightLayers[0].program;

    applyRuntimeStateToEngines();
    applyRightHand();

    if (m_panel) {
        m_panel->setBpm(r.bpm);
        m_panel->setTranspose(r.transpose);
        m_panel->setOctave(r.octave);
        m_panel->setSplitPoint(r.splitNote);
        m_panel->setEqGains(r.eqLowDb, r.eqMidDb, r.eqHighDb);
        m_panel->setCompAmount(r.compAmount);
        m_panel->setToggleStates(r.chordArrangerEnabled, r.chordMemoryEnabled,
                                 r.syncroStopOnRelease, r.chordBassEnabled,
                                 r.autoFillEnabled);
        for (int i = 0; i < 3; ++i)
            m_panel->setRightVoice(i, r.rightLayers[i].enabled, r.rightLayers[i].program,
                                   r.rightLayers[i].volume, r.rightLayers[i].octave);
    }
    saveSettings();
    juce::Logger::writeToLog("[Cadenza] Recalled registration " + juce::String(slot + 1));
}

void MainComponent::applyOts(int slot)
{
    if (!m_settings || slot < 0 || slot >= 4)
        return;
    const auto style = m_styleEngine.currentStyle();
    if (!style)
        return;
    const auto& setting = style->ots[static_cast<std::size_t>(slot)];
    if (!setting.present) {
        juce::Logger::writeToLog("[Cadenza] OTS " + juce::String(slot + 1)
                                 + ": style defines no setting");
        return;
    }

    auto& st = m_settings->state();
    const auto targets = cadenza::arranger::otsRecallTargets(setting);
    bool anyLayerDisabled = false;
    for (int i = 0; i < 3; ++i) {
        auto& L = st.rightLayers[i];
        if (L.enabled && !targets[i].enabled)
            anyLayerDisabled = true;
        L.enabled = targets[i].enabled;
        if (targets[i].setProgram) {
            L.program = targets[i].program;
            L.pluginPath.clear();   // OTS voices are GM; drop any per-layer VST
        }
        if (targets[i].setVolume)
            L.volume = targets[i].volume;
    }
    st.melodyProgram = st.rightLayers[0].program;

    applyRightHand();
    if (anyLayerDisabled)
        m_audio.allNotesOff();   // release anything a now-disabled layer held

    if (m_panel) {
        for (int i = 0; i < 3; ++i) {
            const auto& L = st.rightLayers[i];
            m_panel->setRightVoice(i, L.enabled, L.program, L.volume, L.octave);
            if (L.pluginPath.empty())
                m_panel->setRightVoiceName(
                    i, juce::String(cadenza::midi::gmInstrumentName(L.program)), false);
        }
    }
    saveSettings();
    juce::Logger::writeToLog("[Cadenza] Applied OTS " + juce::String(slot + 1));
}

void MainComponent::applyOtsLinkForSection(const std::string& name)
{
    if (!m_settings || !m_settings->state().otsLinkEnabled)
        return;
    const auto slot = cadenza::arranger::mainSectionToOtsSlot(name);
    if (!slot.has_value())
        return;                        // fills/intros/endings never retrigger
    if (name == m_lastLinkedMain)
        return;                        // fill returning to the same Main
    m_lastLinkedMain = name;
    applyOts(*slot);
}

void MainComponent::refreshOtsAvailability()
{
    if (!m_panel)
        return;
    std::array<bool, 4> available { false, false, false, false };
    if (const auto style = m_styleEngine.currentStyle())
        for (std::size_t i = 0; i < available.size(); ++i)
            available[i] = style->ots[i].present;
    m_panel->setOtsAvailable(available);
}


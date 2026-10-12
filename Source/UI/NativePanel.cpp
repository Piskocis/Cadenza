#include "NativePanel.h"
#include "CadenzaLookAndFeel.h"
#include "../Midi/GmInstruments.h"

namespace cadenza::ui
{
namespace
{
// Show the instrument / drum-kit picker for a mixer strip. Drum channel (10) gets
// GM drum kits; melodic channels get the 128 GM voices grouped by family.
constexpr int kMenuLoadPlugin = 900001;
constexpr int kMenuUseGm       = 900002;
constexpr int kMenuEditPlugin  = 900003;
constexpr int kMenuSetDefault  = 900004;

// Keyboard split point (middle C). Must match MidiRouter's default splitNote so
// the on-screen tint lines up with where the host actually routes chord notes.
constexpr int kKeyboardSplitNote = 60;

juce::String formatTransposeValue(int semitones)
{
    static constexpr const char* noteNames[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };

    const int pitchClass = ((semitones % 12) + 12) % 12;
    const auto signedValue = (semitones > 0 ? juce::String("+") : juce::String())
                           + juce::String(semitones);
    return signedValue + "  (" + noteNames[pitchClass] + ")";
}

void showInstrumentMenu(juce::Component* anchor, int channel,
                        std::function<void(int)> onPick,
                        std::function<void()> onLoadPlugin,
                        std::function<void()> onClearPlugin,
                        std::function<void()> onOpenEditor,
                        std::function<void()> onSetDefault,
                        bool hasPlugin)
{
    juce::PopupMenu menu;
    menu.addItem(kMenuLoadPlugin, "Load VST3 Instrument...");
    menu.addItem(kMenuEditPlugin, "Open Plugin Editor...", hasPlugin);
    menu.addItem(kMenuSetDefault, "Set as default voice...", hasPlugin);
    menu.addItem(kMenuUseGm, "Use GM SoundFont");
    menu.addSeparator();
    if (channel == 10) {
        static const std::pair<int, const char*> kits[] = {
            { 0, "Standard Kit" }, { 8, "Room Kit" }, { 16, "Power Kit" },
            { 24, "Electronic Kit" }, { 25, "TR-808 Kit" }, { 32, "Jazz Kit" },
            { 40, "Brush Kit" }, { 48, "Orchestra Kit" }
        };
        for (const auto& k : kits)
            menu.addItem(k.first + 1, k.second);            // id = program + 1
    } else {
        for (int fam = 0; fam < 16; ++fam) {
            juce::PopupMenu sub;
            for (int i = 0; i < 8; ++i) {
                const int prog = fam * 8 + i;
                sub.addItem(prog + 1, juce::String(prog) + "  " + cadenza::midi::gmInstrumentName(prog));
            }
            menu.addSubMenu(cadenza::midi::gmFamilyName(fam), sub);
        }
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(anchor),
                       [onPick, onLoadPlugin, onClearPlugin, onOpenEditor, onSetDefault](int result) {
        if (result == kMenuLoadPlugin) { if (onLoadPlugin)  onLoadPlugin();  return; }
        if (result == kMenuEditPlugin) { if (onOpenEditor)  onOpenEditor();  return; }
        if (result == kMenuSetDefault) { if (onSetDefault)  onSetDefault();  return; }
        if (result == kMenuUseGm)      { if (onClearPlugin) onClearPlugin(); return; }
        if (result > 0) onPick(result - 1);
    });
}

// Voice picker for the Right 1/2/3 layers: VST3 options on top, then the 16 GM
// families of 8 programs each. Picking a GM program switches the layer back to GM.
constexpr int kRightLoadPlugin = 900101;
constexpr int kRightEditPlugin = 900103;

void showRightVoiceMenu(juce::Component* anchor, bool hasPlugin,
                        std::function<void(int)> onPickGm,
                        std::function<void()> onLoadPlugin,
                        std::function<void()> onOpenEditor)
{
    juce::PopupMenu menu;
    menu.addItem(kRightLoadPlugin, "Load VST3 Instrument...");
    menu.addItem(kRightEditPlugin, "Open Plugin Editor...", hasPlugin);
    menu.addSeparator();
    for (int fam = 0; fam < 16; ++fam) {
        juce::PopupMenu sub;
        for (int i = 0; i < 8; ++i) {
            const int prog = fam * 8 + i;
            sub.addItem(prog + 1, juce::String(prog) + "  " + cadenza::midi::gmInstrumentName(prog));
        }
        menu.addSubMenu(cadenza::midi::gmFamilyName(fam), sub);
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(anchor),
                       [onPickGm, onLoadPlugin, onOpenEditor](int result) {
        if (result == kRightLoadPlugin) { if (onLoadPlugin) onLoadPlugin(); return; }
        if (result == kRightEditPlugin) { if (onOpenEditor) onOpenEditor(); return; }
        if (result > 0 && onPickGm) onPickGm(result - 1);   // GM voice
    });
}
}

void AiWorkingOverlay::showWorking(const juce::String& message)
{
    m_message = message;
    m_working = true;
    m_animFrame = 0;
    m_hideAtMs = 0;
    setVisible(true);
    toFront(false);
    startTimerHz(6);
    repaint();
}

void AiWorkingOverlay::showResult(const juce::String& message)
{
    m_message = message;
    m_working = false;
    m_animFrame = 0;
    m_hideAtMs = juce::Time::getMillisecondCounter() + 3500u;
    setVisible(true);
    toFront(false);
    startTimer(120);
    repaint();
}

void AiWorkingOverlay::hide()
{
    stopTimer();
    setVisible(false);
    m_message.clear();
    m_working = false;
    m_hideAtMs = 0;
}

void AiWorkingOverlay::timerCallback()
{
    if (m_working) {
        ++m_animFrame;
        repaint();
        return;
    }

    if (m_hideAtMs != 0 && juce::Time::getMillisecondCounter() >= m_hideAtMs)
        hide();
}

void AiWorkingOverlay::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    g.setColour(juce::Colours::black.withAlpha(0.42f));
    g.fillRect(bounds);

    auto panel = bounds.withSizeKeepingCentre(juce::jmax(240, juce::jmin(520, bounds.getWidth() - 36)),
                                             juce::jmax(112, juce::jmin(150, bounds.getHeight() - 36)));

    auto pf = panel.toFloat();
    g.setColour(CadenzaLookAndFeel::panel().withAlpha(0.98f));
    g.fillRoundedRectangle(pf, 8.0f);
    g.setColour(CadenzaLookAndFeel::accent().withAlpha(m_working ? 0.95f : 0.75f));
    g.drawRoundedRectangle(pf, 8.0f, 2.0f);
    g.setColour(CadenzaLookAndFeel::cream().withAlpha(0.06f));
    g.drawLine(pf.getX() + 10.0f, pf.getY() + 1.0f, pf.getRight() - 10.0f, pf.getY() + 1.0f, 1.0f);

    auto content = panel.reduced(24, 18);
    const auto title = m_working ? juce::String("AI is working") : juce::String("AI result");
    g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    g.setColour(m_working ? CadenzaLookAndFeel::goldBright() : CadenzaLookAndFeel::accent());
    g.drawText(title, content.removeFromTop(28), juce::Justification::centred, false);

    if (m_working) {
        auto dots = content.removeFromTop(24).withSizeKeepingCentre(86, 18);
        for (int i = 0; i < 3; ++i) {
            const auto phase = (m_animFrame + i * 2) % 6;
            const auto radius = 4.0f + (phase < 3 ? (float) phase : (float) (6 - phase));
            const auto cx = (float) dots.getX() + 18.0f + (float) i * 26.0f;
            const auto cy = (float) dots.getCentreY();
            g.setColour(CadenzaLookAndFeel::goldBright().withAlpha(
                juce::jlimit(0.0f, 1.0f, 0.45f + radius * 0.08f)));
            g.fillEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
        }
    } else {
        content.removeFromTop(10);
    }

    g.setFont(juce::Font(juce::FontOptions(14.0f)));
    g.setColour(CadenzaLookAndFeel::cream());
    g.drawFittedText(m_message, content, juce::Justification::centred, 3);
}

NativePanel::NativePanel()
{
    auto styleCaption = [](juce::Label& l, const juce::String& text) {
        l.setText(text.toUpperCase(), juce::dontSendNotification);
        l.setColour(juce::Label::textColourId, CadenzaLookAndFeel::textDim());
        l.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        l.setJustificationType(juce::Justification::centredLeft);
    };

    addAndMakeVisible(m_play);
    addAndMakeVisible(m_openStyle);
    addAndMakeVisible(m_importMidiStyle);
    addAndMakeVisible(m_openSf);
    addAndMakeVisible(m_openAudio);
    addAndMakeVisible(m_openMidi);
    addAndMakeVisible(m_openAnalyze);

    // Left hardware faceplate: CADENZA wordmark.
    addAndMakeVisible(m_logoMain);
    m_logoMain.setText("CADENZA", juce::dontSendNotification);
    m_logoMain.setColour(juce::Label::textColourId, CadenzaLookAndFeel::accent());
    m_logoMain.setFont(juce::Font(juce::FontOptions("Georgia", 34.0f, juce::Font::bold)));
    m_logoMain.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(m_logoSub);
    m_logoSub.setText("A R R A N G E R", juce::dontSendNotification);
    m_logoSub.setColour(juce::Label::textColourId, CadenzaLookAndFeel::textDim());
    m_logoSub.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    m_logoSub.setJustificationType(juce::Justification::centred);

    // Master volume + balance knobs (master drives the engine; balance is chrome).
    auto setupHwKnob = [this](juce::Slider& s, double lo, double hi, double val) {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setRange(lo, hi, 1.0);
        s.setValue(val, juce::dontSendNotification);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        addAndMakeVisible(s);
    };
    setupHwKnob(m_hwMasterVol, 0.0, 127.0, 100.0);
    setupHwKnob(m_hwBalance,   0.0, 127.0, 64.0);
    m_hwMasterVol.onValueChange = [this] {
        if (m_cb.onMasterChanged) m_cb.onMasterChanged((int) m_hwMasterVol.getValue());
    };

    for (auto* b : { &m_assign1, &m_assign2, &m_dbeamL, &m_dbeamR,
                     &m_scIntro, &m_scVari, &m_scFill, &m_scBreak, &m_scEnd,
                     &m_syncroStart, &m_resetTempo })
        addAndMakeVisible(*b);

    // Wire the obvious style-control buttons to real section triggers.
    m_scIntro.onClick = [this] { if (m_cb.selectSection) m_cb.selectSection("intro"); };
    m_scEnd.onClick   = [this] { if (m_cb.selectSection) m_cb.selectSection("ending"); };
    m_scFill.onClick  = [this] { if (m_cb.selectSection) m_cb.selectSection("fillAA"); };

    // Tempo wheel (rotary dial) — replaces the -/+ tempo nudgers.
    m_tempoWheel.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    m_tempoWheel.setRange(40.0, 300.0, 1.0);
    m_tempoWheel.setValue(120.0, juce::dontSendNotification);
    m_tempoWheel.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_tempoWheel.onValueChange = [this] {
        const int bpm = (int) m_tempoWheel.getValue();
        m_bpmVal.setText(juce::String(bpm), juce::dontSendNotification);
        if (m_cb.onSetTempo) m_cb.onSetTempo(bpm);
    };
    addAndMakeVisible(m_tempoWheel);
    m_bpmDown.setVisible(false);   // replaced by the wheel
    m_bpmUp.setVisible(false);

    // Top status-bar master-volume slider.
    m_topMaster.setSliderStyle(juce::Slider::LinearHorizontal);
    m_topMaster.setRange(0.0, 127.0, 1.0);
    m_topMaster.setValue(110.0, juce::dontSendNotification);
    m_topMaster.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_topMaster.onValueChange = [this] {
        if (m_cb.onMasterChanged) m_cb.onMasterChanged((int) m_topMaster.getValue());
    };
    addAndMakeVisible(m_topMaster);

    m_chordAnalysis.setMultiLine(true);
    m_chordAnalysis.setReturnKeyStartsNewLine(true);
    m_chordAnalysis.setReadOnly(true);
    m_chordAnalysis.setScrollbarsShown(true);
    m_chordAnalysis.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff11131a));
    m_chordAnalysis.setColour(juce::TextEditor::textColourId, juce::Colours::white);
    m_chordAnalysis.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff2c3544));
    m_chordAnalysis.setText("Open an audio file to analyse chords.", juce::dontSendNotification);
    addAndMakeVisible(m_chordAnalysis);

    // Bottom hardware strip.
    setupHwKnob(m_hwLeft,  0.0, 127.0, 64.0);
    setupHwKnob(m_hwRight, 0.0, 127.0, 64.0);
    setupHwKnob(m_xfader,  0.0, 127.0, 64.0);
    for (auto* b : { &m_splitSet, &m_exitBtn, &m_menuBtn })
        addAndMakeVisible(*b);

    const char* partNames[] = { "DRUM", "PERC", "BASS", "ACC 1", "ACC 2", "ACC 3", "ACC 4", "ACC 5" };
    for (int i = 0; i < 8; ++i) {
        auto b = std::make_unique<juce::TextButton>(partNames[i]);
        b->setClickingTogglesState(true);
        b->setToggleState(i < 6, juce::dontSendNotification);            // first six lit, like the render
        b->setColour(juce::TextButton::buttonOnColourId, CadenzaLookAndFeel::led());
        addAndMakeVisible(*b);
        m_partButtons.push_back(std::move(b));
    }
    for (int i = 0; i < 8; ++i) {
        auto b = std::make_unique<juce::TextButton>(juce::String(i + 1));
        b->setToggleState(i == 0, juce::dontSendNotification);           // slot 1 active (gold)
        const int idx = i;
        b->onClick = [this, idx] {
            for (std::size_t k = 0; k < m_regMemButtons.size(); ++k)
                m_regMemButtons[k]->setToggleState(static_cast<int>(k) == idx, juce::dontSendNotification);
        };
        addAndMakeVisible(*b);
        m_regMemButtons.push_back(std::move(b));
    }

    const char* navNames[] = { "Home", "Song", "Style", "Sound", "Mixer", "Effect", "Setting", "Editor" };
    for (int i = 0; i < 8; ++i) {
        auto b = std::make_unique<juce::TextButton>(navNames[i]);
        b->setToggleState(i == 0, juce::dontSendNotification);
        b->onClick = [this, i] {
            m_navActive = i;
            for (std::size_t k = 0; k < m_navButtons.size(); ++k)
                m_navButtons[k]->setToggleState(static_cast<int>(k) == i, juce::dontSendNotification);
            resized();   // re-layout for the selected page
            repaint();
            if (m_cb.onPageChanged) m_cb.onPageChanged(i);
        };
        addAndMakeVisible(*b);
        m_navButtons.push_back(std::move(b));
    }

    styleCaption(m_bpmCaption, "Tempo");
    addAndMakeVisible(m_bpmCaption);
    addAndMakeVisible(m_bpmDown);
    addAndMakeVisible(m_bpmUp);
    addAndMakeVisible(m_bpmVal);
    m_bpmTap.setTooltip("Tap a few beats to set the tempo");
    m_bpmTap.onClick = [this] {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (!m_tapTimesMs.empty() && now - m_tapTimesMs.back() > 2000.0)
            m_tapTimesMs.clear();                       // gap > 2s -> start over
        m_tapTimesMs.push_back(now);
        if (m_tapTimesMs.size() > 4)
            m_tapTimesMs.erase(m_tapTimesMs.begin());   // average the last few taps
        if (m_tapTimesMs.size() >= 2) {
            const double avgInterval = (m_tapTimesMs.back() - m_tapTimesMs.front())
                                       / static_cast<double>(m_tapTimesMs.size() - 1);
            if (avgInterval > 0.0) {
                const int bpm = juce::jlimit(40, 300, juce::roundToInt(60000.0 / avgInterval));
                m_bpmVal.setText(juce::String(bpm), juce::dontSendNotification);
                if (m_cb.onSetTempo) m_cb.onSetTempo(bpm);
            }
        }
    };
    addAndMakeVisible(m_bpmTap);
    m_bpmVal.setJustificationType(juce::Justification::centred);
    m_bpmVal.setColour(juce::Label::textColourId, CadenzaLookAndFeel::cream());
    m_bpmVal.setFont(juce::Font(juce::FontOptions(19.0f, juce::Font::bold)));
    m_bpmVal.setText("120", juce::dontSendNotification);

    addAndMakeVisible(m_styleName);
    m_styleName.setText("(no style)", juce::dontSendNotification);
    m_styleName.setColour(juce::Label::textColourId, CadenzaLookAndFeel::cream());
    m_styleName.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));

    addAndMakeVisible(m_chord);
    m_chord.setText("--", juce::dontSendNotification);
    m_chord.setColour(juce::Label::textColourId, CadenzaLookAndFeel::accent());
    m_chord.setFont(juce::Font(juce::FontOptions(34.0f, juce::Font::bold)));
    m_chord.setJustificationType(juce::Justification::centred);

    styleCaption(m_transposeCaption, "Transpose");
    addAndMakeVisible(m_transposeCaption);
    addAndMakeVisible(m_transposeDown);
    addAndMakeVisible(m_transposeUp);
    addAndMakeVisible(m_transposeVal);
    m_transposeVal.setJustificationType(juce::Justification::centred);
    m_transposeVal.setColour(juce::Label::textColourId, CadenzaLookAndFeel::cream());
    m_transposeVal.setText(formatTransposeValue(0), juce::dontSendNotification);

    styleCaption(m_octaveCaption, "Octave (live melody)");
    addAndMakeVisible(m_octaveCaption);
    addAndMakeVisible(m_octaveDown);
    addAndMakeVisible(m_octaveUp);
    addAndMakeVisible(m_octaveVal);
    m_octaveVal.setJustificationType(juce::Justification::centred);
    m_octaveVal.setColour(juce::Label::textColourId, CadenzaLookAndFeel::cream());
    m_octaveVal.setText("0", juce::dontSendNotification);

    addAndMakeVisible(m_arranger);
    addAndMakeVisible(m_chordMemory);
    addAndMakeVisible(m_syncroStop);
    addAndMakeVisible(m_autoFill);
    addAndMakeVisible(m_fingeredOnBass);
    m_autoFill.setTooltip("Pressing a Main button while playing inserts its fill-in first");

    styleCaption(m_sectionsCaption, "Sections");
    addAndMakeVisible(m_sectionsCaption);

    // On-screen keyboard (split-aware via injectNote on the host side). The left
    // chord zone (below the split) is tinted blue so the split is visible.
    m_splitNote = kKeyboardSplitNote;
    m_keyboard = std::make_unique<ChordSplitKeyboard>(
        m_keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard, m_splitNote);
    m_keyboard->setAvailableRange(36, 96);   // C2..C7
    m_keyboard->setLowestVisibleKey(36);     // show the chord zone too
    m_keyboard->setScrollButtonsVisible(false);
    addAndMakeVisible(*m_keyboard);
    m_keyboardState.addListener(this);

    // Draggable split marker ("Chords | Melody") above the keys.
    m_splitBar = std::make_unique<SplitBar>();
    m_splitBar->setKeyboard(m_keyboard.get());
    m_splitBar->setSplitNote(m_splitNote);
    m_splitBar->onSplitChanged = [this](int note) {
        m_splitNote = note;
        m_keyboard->setSplitNote(note);
        if (m_cb.onSplitChanged) m_cb.onSplitChanged(note);
    };
    addAndMakeVisible(*m_splitBar);

    styleCaption(m_mixerCaption, "Mixer");
    addAndMakeVisible(m_mixerCaption);

    styleCaption(m_padsCaption, "Pads");
    addAndMakeVisible(m_padsCaption);
    for (int i = 0; i < 4; ++i) {
        auto pad = std::make_unique<juce::TextButton>("Pad " + juce::String(i + 1));
        const int index = i;
        pad->onClick = [this, index] { if (m_cb.onPad) m_cb.onPad(index); };
        addAndMakeVisible(*pad);
        m_pads.push_back(std::move(pad));
    }

    // Master EQ: three knobs (Low / Mid / High), -12..+12 dB.
    styleCaption(m_eqCaption, "Master EQ (dB)");
    addAndMakeVisible(m_eqCaption);
    auto setupEqKnob = [this](juce::Slider& s) {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setRange(-12.0, 12.0, 1.0);
        s.setValue(0.0, juce::dontSendNotification);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 46, 16);
        s.onValueChange = [this] {
            if (m_cb.onEqChanged)
                m_cb.onEqChanged((int) m_eqLow.getValue(),
                                 (int) m_eqMid.getValue(),
                                 (int) m_eqHigh.getValue());
        };
        addAndMakeVisible(s);
    };
    setupEqKnob(m_eqLow); setupEqKnob(m_eqMid); setupEqKnob(m_eqHigh);
    auto eqLabel = [this](juce::Label& l, const juce::String& t) {
        l.setText(t.toUpperCase(), juce::dontSendNotification);
        l.setColour(juce::Label::textColourId, CadenzaLookAndFeel::textDim());
        l.setFont(juce::Font(juce::FontOptions(9.5f, juce::Font::bold)));
        l.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(l);
    };
    eqLabel(m_eqLowCap, "Low"); eqLabel(m_eqMidCap, "Mid"); eqLabel(m_eqHighCap, "High");

    // Master compressor amount knob (0..100).
    m_comp.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    m_comp.setRange(0.0, 100.0, 1.0);
    m_comp.setValue(55.0, juce::dontSendNotification);
    m_comp.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 46, 16);
    m_comp.onValueChange = [this] { if (m_cb.onCompChanged) m_cb.onCompChanged((int) m_comp.getValue()); };
    addAndMakeVisible(m_comp);
    eqLabel(m_compCap, "Comp");

    m_master.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    m_master.setRange(0.0, 127.0, 1.0);
    m_master.setValue(100.0, juce::dontSendNotification);
    m_master.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 46, 16);
    m_master.onValueChange = [this] { if (m_cb.onMasterChanged) m_cb.onMasterChanged((int) m_master.getValue()); };
    addAndMakeVisible(m_master);
    eqLabel(m_masterCap, "Master");

    m_drums.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    m_drums.setRange(0.0, 127.0, 1.0);
    m_drums.setValue(100.0, juce::dontSendNotification);
    m_drums.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 46, 16);
    m_drums.onValueChange = [this] { if (m_cb.onDrumsChanged) m_cb.onDrumsChanged((int) m_drums.getValue()); };
    addAndMakeVisible(m_drums);
    eqLabel(m_drumsCap, "Drums");

    m_reverb.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    m_reverb.setRange(0.0, 100.0, 1.0);
    m_reverb.setValue(80.0, juce::dontSendNotification);
    m_reverb.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 46, 16);
    m_reverb.onValueChange = [this] { if (m_cb.onReverbChanged) m_cb.onReverbChanged((int) m_reverb.getValue()); };
    addAndMakeVisible(m_reverb);
    eqLabel(m_reverbCap, "Reverb");

    // Polished Master toggle: cleaner brickwall limiter + light glue/air/width.
    m_polished.setColour(juce::ToggleButton::textColourId, CadenzaLookAndFeel::textDim());
    m_polished.setTooltip("Polished Master: cleaner, slightly louder, less harsh master output "
                          "(clean brickwall limiter + gentle glue). Toggle off to A/B the raw chain.");
    m_polished.onClick = [this] {
        if (m_cb.onPolishedMaster) m_cb.onPolishedMaster(m_polished.getToggleState());
    };
    addAndMakeVisible(m_polished);

    // --- Right 1/2/3 layered right-hand voices ---
    styleCaption(m_rightCaption, "Voices (Left + Right 1-3)");
    addAndMakeVisible(m_rightCaption);
    for (int i = 0; i < 3; ++i) {
        auto& s = m_rightVoices[static_cast<std::size_t>(i)];
        const int layer = i;

        s.enable = std::make_unique<juce::ToggleButton>("Right " + juce::String(i + 1));
        s.enable->setColour(juce::ToggleButton::textColourId, CadenzaLookAndFeel::textDim());
        s.enable->onClick = [this, layer] {
            if (m_cb.onRightEnabled)
                m_cb.onRightEnabled(layer, m_rightVoices[static_cast<std::size_t>(layer)].enable->getToggleState());
        };
        addAndMakeVisible(*s.enable);

        s.instrument = std::make_unique<juce::TextButton>("Grand Piano");
        s.instrument->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333a47));
        s.instrument->onClick = [this, layer] {
            showRightVoiceMenu(m_rightVoices[static_cast<std::size_t>(layer)].instrument.get(),
                               m_rightHasPlugin[static_cast<std::size_t>(layer)],
                               [this, layer](int program) { if (m_cb.onRightInstrument) m_cb.onRightInstrument(layer, program); },
                               [this, layer] { if (m_cb.onRightLoadPlugin) m_cb.onRightLoadPlugin(layer); },
                               [this, layer] { if (m_cb.onRightOpenEditor) m_cb.onRightOpenEditor(layer); });
        };
        addAndMakeVisible(*s.instrument);

        s.volume = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
        s.volume->setRange(0.0, 127.0, 1.0);
        s.volume->setValue(100.0, juce::dontSendNotification);
        s.volume->onValueChange = [this, layer] {
            if (m_cb.onRightVolume)
                m_cb.onRightVolume(layer, static_cast<int>(m_rightVoices[static_cast<std::size_t>(layer)].volume->getValue()));
        };
        addAndMakeVisible(*s.volume);

        s.octDown = std::make_unique<juce::TextButton>("-");
        s.octDown->onClick = [this, layer] { if (m_cb.onRightOctave) m_cb.onRightOctave(layer, -1); };
        addAndMakeVisible(*s.octDown);

        s.octVal = std::make_unique<juce::Label>();
        s.octVal->setText("0", juce::dontSendNotification);
        s.octVal->setJustificationType(juce::Justification::centred);
        s.octVal->setColour(juce::Label::textColourId, CadenzaLookAndFeel::textDim());
        addAndMakeVisible(*s.octVal);

        s.octUp = std::make_unique<juce::TextButton>("+");
        s.octUp->onClick = [this, layer] { if (m_cb.onRightOctave) m_cb.onRightOctave(layer, +1); };
        addAndMakeVisible(*s.octUp);
    }

    // --- Left split voice: sounds the keys BELOW the split point ---
    {
        auto& s = m_leftStrip;
        s.enable = std::make_unique<juce::ToggleButton>("Left");
        s.enable->setColour(juce::ToggleButton::textColourId, CadenzaLookAndFeel::textDim());
        s.enable->setTooltip("Play an instrument with your left hand (keys below the split point)");
        s.enable->onClick = [this] {
            if (m_cb.onLeftEnabled) m_cb.onLeftEnabled(m_leftStrip.enable->getToggleState());
        };
        addAndMakeVisible(*s.enable);

        s.instrument = std::make_unique<juce::TextButton>("Fingered Bass");
        s.instrument->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333a47));
        s.instrument->onClick = [this] {
            juce::PopupMenu menu;
            for (int fam = 0; fam < 16; ++fam) {
                juce::PopupMenu sub;
                for (int i = 0; i < 8; ++i) {
                    const int prog = fam * 8 + i;
                    sub.addItem(prog + 1, juce::String(prog) + "  " + cadenza::midi::gmInstrumentName(prog));
                }
                menu.addSubMenu(cadenza::midi::gmFamilyName(fam), sub);
            }
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(m_leftStrip.instrument.get()),
                [this](int result) {
                    if (result > 0 && m_cb.onLeftInstrument) m_cb.onLeftInstrument(result - 1);
                });
        };
        addAndMakeVisible(*s.instrument);

        s.volume = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
        s.volume->setRange(0.0, 127.0, 1.0);
        s.volume->setValue(100.0, juce::dontSendNotification);
        s.volume->onValueChange = [this] {
            if (m_cb.onLeftVolume) m_cb.onLeftVolume(static_cast<int>(m_leftStrip.volume->getValue()));
        };
        addAndMakeVisible(*s.volume);

        s.octDown = std::make_unique<juce::TextButton>("-");
        s.octDown->onClick = [this] { if (m_cb.onLeftOctave) m_cb.onLeftOctave(-1); };
        addAndMakeVisible(*s.octDown);

        s.octVal = std::make_unique<juce::Label>();
        s.octVal->setText("0", juce::dontSendNotification);
        s.octVal->setJustificationType(juce::Justification::centred);
        s.octVal->setColour(juce::Label::textColourId, CadenzaLookAndFeel::textDim());
        addAndMakeVisible(*s.octVal);

        s.octUp = std::make_unique<juce::TextButton>("+");
        s.octUp->onClick = [this] { if (m_cb.onLeftOctave) m_cb.onLeftOctave(+1); };
        addAndMakeVisible(*s.octUp);
    }

    // --- Pitch / Mod wheels (real controls over the painted wheel zone) ---
    m_pitchWheel.setSliderStyle(juce::Slider::LinearVertical);
    m_pitchWheel.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_pitchWheel.setRange(0.0, 16383.0, 1.0);
    m_pitchWheel.setValue(8192.0, juce::dontSendNotification);
    m_pitchWheel.setDoubleClickReturnValue(true, 8192.0);
    m_pitchWheel.setTooltip("Pitch bend (springs back to centre)");
    m_pitchWheel.onValueChange = [this] {
        if (m_cb.onPitchBend) m_cb.onPitchBend(static_cast<int>(m_pitchWheel.getValue()));
    };
    m_pitchWheel.onDragEnd = [this] {
        m_pitchWheel.setValue(8192.0, juce::sendNotificationSync);   // spring back to centre
    };
    addAndMakeVisible(m_pitchWheel);

    m_modWheel.setSliderStyle(juce::Slider::LinearVertical);
    m_modWheel.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_modWheel.setRange(0.0, 127.0, 1.0);
    m_modWheel.setValue(0.0, juce::dontSendNotification);
    m_modWheel.setTooltip("Modulation (CC1)");
    m_modWheel.onValueChange = [this] {
        if (m_cb.onModulation) m_cb.onModulation(static_cast<int>(m_modWheel.getValue()));
    };
    addAndMakeVisible(m_modWheel);

    // --- Registrations (one-button performance snapshots) ---
    styleCaption(m_regCaption, "Registrations");
    addAndMakeVisible(m_regCaption);
    m_regStore.setClickingTogglesState(true);
    m_regStore.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffb04a4a));
    m_regStore.setTooltip("Arm, then click a slot to save the current setup");
    m_regStore.onClick = [this] { m_regStoreArmed = m_regStore.getToggleState(); };
    addAndMakeVisible(m_regStore);
    for (int i = 0; i < 4; ++i) {
        auto b = std::make_unique<juce::TextButton>(juce::String(i + 1));
        b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333a47));
        const int slot = i;
        b->onClick = [this, slot] {
            if (m_regStoreArmed) {
                if (m_cb.onStoreRegistration) m_cb.onStoreRegistration(slot);
                m_regStoreArmed = false;
                m_regStore.setToggleState(false, juce::dontSendNotification);
            } else if (m_cb.onRecallRegistration) {
                m_cb.onRecallRegistration(slot);
            }
        };
        addAndMakeVisible(*b);
        m_regButtons.push_back(std::move(b));
        m_regUsed.push_back(false);
    }

    // --- One Touch Settings (per-style right-hand voice presets) ---
    styleCaption(m_otsCaption, "One Touch Settings");
    addAndMakeVisible(m_otsCaption);
    m_otsLink.setTooltip("Recall OTS 1-4 automatically when Main A-D starts");
    m_otsLink.onClick = [this] { if (m_cb.setOtsLink) m_cb.setOtsLink(m_otsLink.getToggleState()); };
    addAndMakeVisible(m_otsLink);
    for (int i = 0; i < 4; ++i) {
        auto b = std::make_unique<juce::TextButton>("OTS " + juce::String(i + 1));
        b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333a47));
        b->setEnabled(false);   // until a style with OTS data is loaded
        const int slot = i;
        b->onClick = [this, slot] { if (m_cb.onOts) m_cb.onOts(slot); };
        addAndMakeVisible(*b);
        m_otsButtons[static_cast<std::size_t>(i)] = std::move(b);
    }

    // --- Style Recorder (record your own style patterns) ---
    styleCaption(m_recCaption, "Style Recorder");
    addAndMakeVisible(m_recCaption);
    m_recBars.addItem("1 bar", 1);
    m_recBars.addItem("2 bars", 2);
    m_recBars.addItem("4 bars", 4);
    m_recBars.addItem("8 bars", 8);
    m_recBars.setSelectedId(4, juce::dontSendNotification);
    m_recBars.onChange = [this] {
        if (m_cb.onRecBars)
            m_cb.onRecBars(m_recBars.getSelectedId());
    };
    addAndMakeVisible(m_recBars);
    const char* recPartNames[] = { "Drums", "Bass", "Chord 1", "Chord 2",
                                   "Pad", "Phrase 1", "Phrase 2" };
    for (int i = 0; i < 7; ++i)
        m_recPart.addItem(recPartNames[i], i + 1);
    m_recPart.setSelectedId(1, juce::dontSendNotification);
    m_recPart.onChange = [this] {
        if (m_cb.onRecPart) m_cb.onRecPart(m_recPart.getSelectedId() - 1);
    };
    addAndMakeVisible(m_recPart);

    m_recNew.setTooltip("Start a new style: an empty looping section at the current tempo");
    m_recNew.onClick = [this] {
        if (m_cb.onRecNew) m_cb.onRecNew(m_recBars.getSelectedId());
    };
    addAndMakeVisible(m_recNew);

    m_recArm.setTooltip("Record the selected part while the section loops; "
                        "click again to keep the take (it overdubs onto the part)");
    m_recArm.setClickingTogglesState(true);
    m_recArm.setColour(juce::TextButton::buttonOnColourId, juce::Colours::firebrick);
    m_recArm.onClick = [this] {
        if (m_cb.onRecArm) m_cb.onRecArm(m_recArm.getToggleState());
    };
    addAndMakeVisible(m_recArm);

    m_recClick.setTooltip("Metronome click while recording");
    m_recClick.setToggleState(true, juce::dontSendNotification);
    m_recClick.onClick = [this] {
        if (m_cb.onRecMetronome) m_cb.onRecMetronome(m_recClick.getToggleState());
    };
    addAndMakeVisible(m_recClick);

    m_recEdit.setTooltip("Open the selected part in the piano-roll editor");
    m_recEdit.onClick = [this] { if (m_cb.onRecEdit) m_cb.onRecEdit(); };
    addAndMakeVisible(m_recEdit);

    m_recClear.setTooltip("Delete everything recorded on the selected part");
    m_recClear.onClick = [this] { if (m_cb.onRecClear) m_cb.onRecClear(); };
    addAndMakeVisible(m_recClear);

    m_recSave.setTooltip("Save the recorded style as a .cstyle file and load it");
    m_recSave.onClick = [this] { if (m_cb.onRecSave) m_cb.onRecSave(); };
    addAndMakeVisible(m_recSave);

    m_recExit.setTooltip("Leave the recorder without saving");
    m_recExit.onClick = [this] { if (m_cb.onRecExit) m_cb.onRecExit(); };
    addAndMakeVisible(m_recExit);

    m_recMakeEditable.setTooltip("Turn the loaded Yamaha style into an editable copy "
                                 "you can change in the piano roll and save as .cstyle");
    m_recMakeEditable.onClick = [this] { if (m_cb.onMakeEditable) m_cb.onMakeEditable(); };
    addAndMakeVisible(m_recMakeEditable);

    m_aiStyle.setTooltip("Describe a style or name a song and let AI generate it (needs an API key in AI Settings)");
    m_aiStyle.onClick = [this] { if (m_cb.onAiStyle) m_cb.onAiStyle(); };
    addAndMakeVisible(m_aiStyle);

    m_aiAddFills.setTooltip("Use AI to add one-bar fills and missing intro/ending sections to the editable style");
    m_aiAddFills.onClick = [this] { if (m_cb.onAiAddFills) m_cb.onAiAddFills(); };
    addAndMakeVisible(m_aiAddFills);

    m_aiPolish.setTooltip("Use AI to minimally fix wrong notes and stray drum hits in the editable style");
    m_aiPolish.onClick = [this] { if (m_cb.onAiPolish) m_cb.onAiPolish(); };
    addAndMakeVisible(m_aiPolish);

    m_aiSettings.setTooltip("Set your Anthropic API key and choose the AI model");
    m_aiSettings.onClick = [this] { if (m_cb.onAiSettings) m_cb.onAiSettings(); };
    addAndMakeVisible(m_aiSettings);

    m_recStatus.setColour(juce::Label::textColourId, CadenzaLookAndFeel::textDim());
    m_recStatus.setFont(juce::Font(juce::FontOptions(12.0f)));
    addAndMakeVisible(m_recStatus);
    setRecorderState(false, false, "Press New to record your own style");
    setMakeEditableAvailable(false);

    // --- Style Editor page: embedded piano-roll editor (hidden until page 7) ---
    {
        StylePartEditorView::Callbacks ecb;
        ecb.onNotesEdited = [this](std::vector<cadenza::arranger::PatternNote> notes) {
            if (m_cb.onEditorNotesEdited) m_cb.onEditorNotesEdited(std::move(notes));
        };
        ecb.onAudition = [this](int note, int vel) {
            if (m_cb.onEditorAudition) m_cb.onEditorAudition(note, vel);
        };
        ecb.onTogglePlayback = [this] { if (m_cb.onEditorTogglePlay) m_cb.onEditorTogglePlay(); };
        ecb.onToggleRecord   = [this] { if (m_cb.onEditorToggleRecord) m_cb.onEditorToggleRecord(); };
        ecb.onSnapDivisionChanged = [this](int division) {
            if (m_cb.onEditorSnapDivisionChanged)
                m_cb.onEditorSnapDivisionChanged(division);
        };
        ecb.onPickSection = [this](std::string id) {
            if (m_cb.onEditorPickSection) m_cb.onEditorPickSection(std::move(id));
        };
        ecb.onPickPart = [this](int slot) {
            if (m_cb.onEditorPickPart) m_cb.onEditorPickPart(slot);
        };
        ecb.onSave = [this] { if (m_cb.onEditorSave) m_cb.onEditorSave(); };

        m_editor = std::make_unique<StylePartEditorView>(std::move(ecb));
        m_editor->setStyleControlsVisible(true);
        m_editor->setEditorEnabled(false,
            "Open a style - for Yamaha press Make Editable - then edit it here.");
        addChildComponent(*m_editor);
    }

    // --- wire control callbacks (message thread) ---
    m_play.onClick          = [this] { if (m_cb.togglePlay)    m_cb.togglePlay(); };
    m_fade.setTooltip("Fade the band out over a few seconds, then stop");
    m_fade.onClick          = [this] { if (m_cb.fadeOut)       m_cb.fadeOut(); };
    addAndMakeVisible(m_fade);
    m_openStyle.onClick     = [this] { if (m_cb.openStyle)     m_cb.openStyle(); };
    m_importMidiStyle.onClick = [this] { if (m_cb.importMidiStyle) m_cb.importMidiStyle(); };
    m_openSf.onClick        = [this] { if (m_cb.openSoundFont) m_cb.openSoundFont(); };
    m_openAudio.onClick     = [this] { if (m_cb.openAudioSettings) m_cb.openAudioSettings(); };
    m_openMidi.onClick      = [this] { if (m_cb.openMidiSettings)  m_cb.openMidiSettings(); };
    m_openAnalyze.onClick   = [this] { if (m_cb.openChordAnalysis) m_cb.openChordAnalysis(); };
    m_transposeDown.onClick = [this] { if (m_cb.nudgeTranspose) m_cb.nudgeTranspose(-1); };
    m_transposeUp.onClick   = [this] { if (m_cb.nudgeTranspose) m_cb.nudgeTranspose(+1); };
    m_octaveDown.onClick    = [this] { if (m_cb.nudgeOctave)    m_cb.nudgeOctave(-1); };
    m_octaveUp.onClick      = [this] { if (m_cb.nudgeOctave)    m_cb.nudgeOctave(+1); };
    m_bpmDown.onClick       = [this] { if (m_cb.nudgeTempo)     m_cb.nudgeTempo(-1); };
    m_bpmUp.onClick         = [this] { if (m_cb.nudgeTempo)     m_cb.nudgeTempo(+1); };

    m_arranger.onClick       = [this] { if (m_cb.setArranger)       m_cb.setArranger(m_arranger.getToggleState()); };
    m_chordMemory.onClick    = [this] { if (m_cb.setChordMemory)    m_cb.setChordMemory(m_chordMemory.getToggleState()); };
    m_syncroStop.onClick     = [this] { if (m_cb.setSyncroStop)     m_cb.setSyncroStop(m_syncroStop.getToggleState()); };
    m_autoFill.onClick       = [this] { if (m_cb.setAutoFill)       m_cb.setAutoFill(m_autoFill.getToggleState()); };
    m_fingeredOnBass.onClick = [this] { if (m_cb.setFingeredOnBass) m_cb.setFingeredOnBass(m_fingeredOnBass.getToggleState()); };

    addAndMakeVisible(m_aiOverlay);
    m_aiOverlay.setInterceptsMouseClicks(false, false);
    m_aiOverlay.hide();
}

NativePanel::~NativePanel()
{
    m_keyboardState.removeListener(this);
}

void NativePanel::handleNoteOn(juce::MidiKeyboardState*, int, int midiNoteNumber, float velocity)
{
    if (m_cb.onKeyboardNote)
        m_cb.onKeyboardNote(midiNoteNumber, juce::jlimit(1, 127, (int)(velocity * 127.0f)), true);
}

void NativePanel::handleNoteOff(juce::MidiKeyboardState*, int, int midiNoteNumber, float)
{
    if (m_cb.onKeyboardNote)
        m_cb.onKeyboardNote(midiNoteNumber, 0, false);
}

void NativePanel::setBpm(int bpm)
{
    m_bpmVal.setText(juce::String(bpm), juce::dontSendNotification);
    m_tempoWheel.setValue(bpm, juce::dontSendNotification);
}

void NativePanel::setMixerChannels(const std::vector<std::pair<int, std::string>>& channelLabels)
{
    m_mixer.clear();
    for (const auto& [channel, label] : channelLabels) {
        MixerStrip strip;
        strip.channel = channel;

        strip.name = std::make_unique<juce::Label>();
        strip.name->setText(juce::String(label), juce::dontSendNotification);
        strip.name->setJustificationType(juce::Justification::centred);
        strip.name->setColour(juce::Label::textColourId, CadenzaLookAndFeel::textDim());
        strip.name->setFont(juce::Font(juce::FontOptions(12.0f)));
        addAndMakeVisible(*strip.name);

        const int ch = channel;

        strip.instrument = std::make_unique<juce::TextButton>("--");
        strip.instrument->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333a47));
        strip.instrument->setTooltip("Choose the instrument for this part");
        strip.instrument->onClick = [this, ch] {
            juce::Component* anchor = nullptr;
            for (auto& s : m_mixer) if (s.channel == ch) { anchor = s.instrument.get(); break; }
            showInstrumentMenu(anchor, ch,
                [this, ch](int program) { if (m_cb.onMixerInstrument) m_cb.onMixerInstrument(ch, program); },
                [this, ch] { if (m_cb.onLoadInstrumentPlugin)  m_cb.onLoadInstrumentPlugin(ch); },
                [this, ch] { if (m_cb.onClearInstrumentPlugin) m_cb.onClearInstrumentPlugin(ch); },
                [this, ch] { if (m_cb.onOpenInstrumentEditor)  m_cb.onOpenInstrumentEditor(ch); },
                [this, ch] { if (m_cb.onSetDefaultVoice)       m_cb.onSetDefaultVoice(ch); },
                /*hasPlugin*/ true);
        };
        addAndMakeVisible(*strip.instrument);

        strip.volume = std::make_unique<juce::Slider>(juce::Slider::LinearVertical,
                                                      juce::Slider::NoTextBox);
        strip.volume->setRange(0.0, 127.0, 1.0);
        strip.volume->setValue(100.0, juce::dontSendNotification);
        strip.volume->onValueChange = [this, ch] {
            if (!m_cb.onMixerVolume) return;
            for (auto& s : m_mixer)
                if (s.channel == ch) { m_cb.onMixerVolume(ch, (int) s.volume->getValue()); break; }
        };
        addAndMakeVisible(*strip.volume);

        strip.mute = std::make_unique<juce::TextButton>("M");
        strip.mute->setClickingTogglesState(true);
        strip.mute->setColour(juce::TextButton::buttonOnColourId, juce::Colours::firebrick);
        strip.mute->onClick = [this, ch] {
            if (!m_cb.onMixerMute) return;
            for (auto& s : m_mixer)
                if (s.channel == ch) { m_cb.onMixerMute(ch, s.mute->getToggleState()); break; }
        };
        addAndMakeVisible(*strip.mute);

        strip.solo = std::make_unique<juce::TextButton>("S");
        strip.solo->setClickingTogglesState(true);
        strip.solo->setColour(juce::TextButton::buttonOnColourId, juce::Colours::orange);
        strip.solo->onClick = [this, ch] {
            if (!m_cb.onMixerSolo) return;
            for (auto& s : m_mixer)
                if (s.channel == ch) { m_cb.onMixerSolo(ch, s.solo->getToggleState()); break; }
        };
        addAndMakeVisible(*strip.solo);

        m_mixer.push_back(std::move(strip));
    }
    resized();
}

void NativePanel::updateMixerStrip(int channel, int volume, bool mute, bool solo)
{
    for (auto& s : m_mixer) {
        if (s.channel != channel) continue;
        s.volume->setValue(volume, juce::dontSendNotification);
        s.mute->setToggleState(mute, juce::dontSendNotification);
        s.solo->setToggleState(solo, juce::dontSendNotification);
        return;
    }
}

void NativePanel::setMixerInstrumentName(int channel, const juce::String& instrumentName)
{
    for (auto& s : m_mixer)
        if (s.channel == channel) { s.instrument->setButtonText(instrumentName); return; }
}

void NativePanel::setStyleName(const juce::String& name)
{
    m_styleName.setText(name.isEmpty() ? "(no style)" : name, juce::dontSendNotification);
}

void NativePanel::setSections(const std::vector<std::pair<std::string, std::string>>& idAndLabel)
{
    m_sections.clear();
    for (const auto& [id, label] : idAndLabel) {
        auto button = std::make_unique<juce::TextButton>(juce::String(label));
        const std::string sectionId = id;
        button->onClick = [this, sectionId] { if (m_cb.selectSection) m_cb.selectSection(sectionId); };
        addAndMakeVisible(*button);
        m_sections.push_back({ std::move(button), id });
    }
    refreshSectionHighlights();
    resized();
}

void NativePanel::setActiveSection(const juce::String& sectionId)
{
    m_activeSection = sectionId;
    refreshSectionHighlights();
}

void NativePanel::refreshSectionHighlights()
{
    for (auto& s : m_sections) {
        const bool active = (m_activeSection == juce::String(s.id));
        // Toggle state drives the gold-gradient "active" look in CadenzaLookAndFeel.
        s.button->setToggleState(active, juce::dontSendNotification);
    }
}

void NativePanel::setChord(const juce::String& chord)
{
    m_chord.setText(chord.isEmpty() ? "--" : chord, juce::dontSendNotification);
}

void NativePanel::setTranspose(int semitones)
{
    m_transposeVal.setText(formatTransposeValue(semitones), juce::dontSendNotification);
}

void NativePanel::setOctave(int octaves)
{
    m_octaveVal.setText((octaves > 0 ? "+" : "") + juce::String(octaves), juce::dontSendNotification);
}

void NativePanel::setPlaying(bool playing)
{
    m_playing = playing;
    m_play.setButtonText(playing ? "Stop" : "Play");
    m_play.setColour(juce::TextButton::buttonColourId,
                     playing ? juce::Colours::firebrick : juce::Colours::darkgreen);
}

void NativePanel::setActivePage(int page)
{
    m_navActive = juce::jlimit(0, 7, page);
    for (std::size_t k = 0; k < m_navButtons.size(); ++k)
        m_navButtons[k]->setToggleState(static_cast<int>(k) == m_navActive, juce::dontSendNotification);
    resized();
    repaint();
    if (m_cb.onPageChanged) m_cb.onPageChanged(m_navActive);
}

void NativePanel::setChordAnalysisSummary(const juce::String& text)
{
    m_chordAnalysis.setText(text.isEmpty() ? "Open an audio file to analyse chords." : text, juce::dontSendNotification);
}

void NativePanel::setToggleStates(bool arranger, bool chordMemory, bool syncroStop, bool fingeredOnBass,
                                  bool autoFill)
{
    m_arranger.setToggleState(arranger, juce::dontSendNotification);
    m_chordMemory.setToggleState(chordMemory, juce::dontSendNotification);
    m_syncroStop.setToggleState(syncroStop, juce::dontSendNotification);
    m_fingeredOnBass.setToggleState(fingeredOnBass, juce::dontSendNotification);
    m_autoFill.setToggleState(autoFill, juce::dontSendNotification);
}

void NativePanel::setRecorderState(bool sessionActive, bool armed, const juce::String& status)
{
    m_recArmed = armed;
    m_recArm.setToggleState(armed, juce::dontSendNotification);
    m_recArm.setEnabled(sessionActive);
    m_recEdit.setEnabled(sessionActive && !armed);
    m_recClear.setEnabled(sessionActive && !armed);
    m_recSave.setEnabled(sessionActive && !armed);
    m_recExit.setEnabled(sessionActive);
    m_recPart.setEnabled(sessionActive && !armed);
    m_recBars.setEnabled(!armed);
    m_recStatus.setText(status, juce::dontSendNotification);
}

void NativePanel::setAiButtonsBusy(bool busy, const juce::String& activeButtonText)
{
    m_aiBusy = busy;

    m_aiStyle.setButtonText(activeButtonText == "AI Style..." ? "Working..." : "AI Style...");
    m_aiAddFills.setButtonText(activeButtonText == "AI: Add Fills" ? "Working..." : "AI: Add Fills");
    m_aiPolish.setButtonText(activeButtonText == "AI: Polish" ? "Working..." : "AI: Polish");

    m_aiStyle.setEnabled(!busy);
    m_aiAddFills.setEnabled(!busy);
    m_aiPolish.setEnabled(!busy);
}

void NativePanel::beginAiWorking(const juce::String& message, const juce::String& activeButtonText)
{
    setAiButtonsBusy(true, activeButtonText);
    m_recStatus.setText(message, juce::dontSendNotification);
    m_aiOverlay.showWorking(message);
}

void NativePanel::finishAiWorking(const juce::String& resultMessage)
{
    setAiButtonsBusy(false);
    if (resultMessage.isNotEmpty()) {
        m_recStatus.setText(resultMessage, juce::dontSendNotification);
        m_aiOverlay.showResult(resultMessage);
    } else {
        m_aiOverlay.hide();
    }
}

void NativePanel::setMakeEditableAvailable(bool available)
{
    m_recMakeEditable.setEnabled(available);
}

void NativePanel::editorSetPart(const std::vector<cadenza::arranger::PatternNote>& notes,
                                int sectionTicks, int ticksPerBeat, int beatsPerBar,
                                int beatUnit, bool percussion)
{
    if (m_editor)
        m_editor->setPart(notes, sectionTicks, ticksPerBeat, beatsPerBar, beatUnit, percussion);
}

void NativePanel::editorSetTransport(int tickInSection, bool playing, bool recordArmed)
{
    if (m_editor) m_editor->setTransportState(tickInSection, playing, recordArmed);
}

void NativePanel::editorSetSections(
    const std::vector<std::pair<std::string, std::string>>& idAndLabel)
{
    if (m_editor) m_editor->setSections(idAndLabel);
}

void NativePanel::editorSetActiveSection(const std::string& sectionId)
{
    if (m_editor) m_editor->setActiveSection(sectionId);
}

void NativePanel::editorSetActivePart(int slot)
{
    if (m_editor) m_editor->setActivePart(slot);
}

void NativePanel::editorSetEnabled(bool enabled, const juce::String& hint)
{
    if (m_editor) m_editor->setEditorEnabled(enabled, hint);
}

void NativePanel::setRecorderBarCount(int bars)
{
    if (m_recBars.indexOfItemId(bars) >= 0)
        m_recBars.setSelectedId(bars, juce::dontSendNotification);
}

void NativePanel::setRecorderPart(int partIndex)
{
    m_recPart.setSelectedId(
        juce::jlimit(0, 6, partIndex) + 1,
        juce::dontSendNotification);
}

void NativePanel::setEqGains(int lowDb, int midDb, int highDb)
{
    m_eqLow.setValue(lowDb,  juce::dontSendNotification);
    m_eqMid.setValue(midDb,  juce::dontSendNotification);
    m_eqHigh.setValue(highDb, juce::dontSendNotification);
}

void NativePanel::setCompAmount(int percent)
{
    m_comp.setValue(percent, juce::dontSendNotification);
}

void NativePanel::setMasterVolume(int percent)
{
    m_master.setValue(percent, juce::dontSendNotification);
}

void NativePanel::setDrumsLevel(int volume)
{
    m_drums.setValue(volume, juce::dontSendNotification);
}

void NativePanel::setReverbAmount(int percent)
{
    m_reverb.setValue(percent, juce::dontSendNotification);
}

void NativePanel::setPolishedMaster(bool on)
{
    m_polished.setToggleState(on, juce::dontSendNotification);
}

void NativePanel::setRightVoice(int layer, bool enabled, int program, int volume, int octave)
{
    if (layer < 0 || layer >= 3)
        return;
    auto& s = m_rightVoices[static_cast<std::size_t>(layer)];
    if (s.enable)     s.enable->setToggleState(enabled, juce::dontSendNotification);
    if (s.volume)     s.volume->setValue(volume, juce::dontSendNotification);
    if (s.octVal)     s.octVal->setText(juce::String(octave), juce::dontSendNotification);
    // Only show the GM name when no VST is loaded on this layer.
    if (!m_rightHasPlugin[static_cast<std::size_t>(layer)] && s.instrument)
        s.instrument->setButtonText(juce::String(cadenza::midi::gmInstrumentName(program)));
}

void NativePanel::setRightVoiceName(int layer, const juce::String& name, bool isPlugin)
{
    if (layer < 0 || layer >= 3)
        return;
    m_rightHasPlugin[static_cast<std::size_t>(layer)] = isPlugin;
    if (auto& b = m_rightVoices[static_cast<std::size_t>(layer)].instrument)
        b->setButtonText(name);
}

void NativePanel::setLeftVoice(bool enabled, int program, int volume, int octave)
{
    auto& s = m_leftStrip;
    if (s.enable)     s.enable->setToggleState(enabled, juce::dontSendNotification);
    if (s.volume)     s.volume->setValue(volume, juce::dontSendNotification);
    if (s.octVal)     s.octVal->setText(juce::String(octave), juce::dontSendNotification);
    if (s.instrument) s.instrument->setButtonText(juce::String(cadenza::midi::gmInstrumentName(program)));
}

void NativePanel::setRegistrationUsed(int slot, bool used)
{
    if (slot < 0 || slot >= static_cast<int>(m_regButtons.size()))
        return;
    m_regUsed[static_cast<std::size_t>(slot)] = used;
    m_regButtons[static_cast<std::size_t>(slot)]->setColour(
        juce::TextButton::buttonColourId,
        used ? juce::Colour(0xff2f6b3a) : juce::Colour(0xff333a47));
}

void NativePanel::setOtsAvailable(const std::array<bool, 4>& available)
{
    for (std::size_t i = 0; i < m_otsButtons.size(); ++i)
        if (m_otsButtons[i])
            m_otsButtons[i]->setEnabled(available[i]);
}

void NativePanel::setOtsLinkEnabled(bool enabled)
{
    m_otsLink.setToggleState(enabled, juce::dontSendNotification);
}

void NativePanel::setSplitPoint(int midiNote)
{
    m_splitNote = midiNote;
    if (m_keyboard) m_keyboard->setSplitNote(midiNote);
    if (m_splitBar) m_splitBar->setSplitNote(midiNote);
}

void NativePanel::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();

    // Body: deep charcoal gradient (design: #1b1d22 -> #101216 -> #0a0b0d).
    juce::ColourGradient bg(CadenzaLookAndFeel::backgroundHi(), b.getCentreX(), b.getY(),
                            CadenzaLookAndFeel::background(),   b.getCentreX(), b.getBottom(), false);
    bg.addColour(0.36, juce::Colour(0xff101216));
    g.setGradientFill(bg);
    g.fillRect(b);

    // Cards: the design's #1c1f27 -> #0e1015 gradient, white-7% border, inset top
    // highlight. Controls paint on top.
    for (const auto& c : m_cards)
    {
        auto r = c.toFloat();
        g.setGradientFill(juce::ColourGradient(CadenzaLookAndFeel::cardTop(), r.getX(), r.getY(),
                                               CadenzaLookAndFeel::cardBot(), r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle(r, 8.0f);
        g.setColour(CadenzaLookAndFeel::cream().withAlpha(0.05f));
        g.drawLine(r.getX() + 8.0f, r.getY() + 1.0f, r.getRight() - 8.0f, r.getY() + 1.0f, 1.0f);
        g.setColour(CadenzaLookAndFeel::cream().withAlpha(0.07f));
        g.drawRoundedRectangle(r, 8.0f, 1.0f);
    }

    // Recessed "LCD" panel for the big chord readout, with a gold bezel.
    if (! m_chordCard.isEmpty())
    {
        auto r = m_chordCard.toFloat();
        g.setColour(juce::Colour(0xff0b0d11));
        g.fillRoundedRectangle(r, 7.0f);
        // Soft gold glow from the top so it reads like a lit display.
        juce::ColourGradient glow(CadenzaLookAndFeel::accent().withAlpha(0.12f), r.getCentreX(), r.getY(),
                                  juce::Colours::transparentBlack, r.getCentreX(),
                                  r.getY() + r.getHeight() * 0.62f, false);
        g.setGradientFill(glow);
        g.fillRoundedRectangle(r.reduced(2.0f), 6.0f);
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.drawRoundedRectangle(r.reduced(1.0f), 6.0f, 1.5f);    // inner shadow
        g.setColour(CadenzaLookAndFeel::accent().withAlpha(0.5f));
        g.drawRoundedRectangle(r, 7.0f, 1.4f);
        // "CHORD" caption in the corner.
        g.setColour(CadenzaLookAndFeel::textDim());
        g.setFont(juce::Font(juce::FontOptions(8.5f, juce::Font::bold)));
        g.drawText("CHORD", r.toNearestInt().reduced(9, 5).removeFromTop(12),
                   juce::Justification::topLeft, false);
    }

    // Style header detail line (bank / meter / tempo), like the design's Style card.
    if (! m_styleDetail.isEmpty())
    {
        g.setColour(CadenzaLookAndFeel::textDim());
        g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        g.drawText("FACTORY     4/4     " + m_bpmVal.getText() + " BPM",
                   m_styleDetail, juce::Justification::centredLeft, false);
    }

    // Chord card mini-piano: two octaves with the held chord's tones lit gold.
    if (! m_chordPiano.isEmpty())
    {
        auto pz = m_chordPiano;
        std::array<bool, 12> lit { {} };
        const auto chordText = m_chord.getText().trim();
        if (chordText.isNotEmpty() && chordText != "--") {
            const auto c0 = chordText[0];
            static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
            if (c0 >= 'A' && c0 <= 'G') {
                int root = base[(int) (c0 - 'A')];
                int i = 1;
                if (i < chordText.length() && chordText[i] == '#') { root = (root + 1) % 12; ++i; }
                else if (i < chordText.length() && chordText[i] == 'b') { root = (root + 11) % 12; ++i; }
                const bool minor = (i < chordText.length() && chordText[i] == 'm'
                                    && ! chordText.substring(i).startsWith("maj"));
                lit[(std::size_t) root] = true;
                lit[(std::size_t) ((root + (minor ? 3 : 4)) % 12)] = true;
                lit[(std::size_t) ((root + 7) % 12)] = true;
            }
        }

        const int whiteCount = 14;
        const float ww = (float) pz.getWidth() / (float) whiteCount;
        const float x0 = (float) pz.getX(), y0 = (float) pz.getY(), h = (float) pz.getHeight();
        static const int whitePc[7]    = { 0, 2, 4, 5, 7, 9, 11 };
        static const int blackAfter[7] = { 1, 1, 0, 1, 1, 1, 0 };
        static const int blackPc[7]    = { 1, 3, 0, 6, 8, 10, 0 };

        for (int k = 0; k < whiteCount; ++k) {
            juce::Rectangle<float> key(x0 + k * ww, y0, ww - 1.0f, h);
            g.setColour(lit[(std::size_t) whitePc[k % 7]] ? CadenzaLookAndFeel::accent()
                                                          : juce::Colour(0xffe9e6dd));
            g.fillRoundedRectangle(key, 1.5f);
            g.setColour(juce::Colours::black.withAlpha(0.35f));
            g.drawRoundedRectangle(key, 1.5f, 0.6f);
        }
        const float bw = ww * 0.62f, bh = h * 0.62f;
        for (int k = 0; k < whiteCount; ++k) {
            if (! blackAfter[k % 7]) continue;
            juce::Rectangle<float> key(x0 + (k + 1) * ww - bw * 0.5f, y0, bw, bh);
            g.setColour(lit[(std::size_t) blackPc[k % 7]] ? CadenzaLookAndFeel::goldDeep()
                                                          : juce::Colour(0xff15171c));
            g.fillRoundedRectangle(key, 1.2f);
        }
    }

    // Left hardware-panel chrome: section captions + the glowing D-Beam sensor.
    if (! m_hwPanel.isEmpty())
    {
        auto cap = [&g](juce::Rectangle<int> rr, const juce::String& t) {
            g.setColour(CadenzaLookAndFeel::textDim());
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            g.drawText(t, rr, juce::Justification::centred, false);
        };
        cap(m_hwMasterCap,   "MASTER VOLUME");
        cap(m_hwBalanceCap,  "BALANCE");
        cap(m_hwAssignCap,   "ASSIGNABLE");
        cap(m_hwStyleCtlCap, "STYLE CONTROL");
        cap(m_dbeamSensor.translated(0, -15).withHeight(12), "D-BEAM");

        auto s = m_dbeamSensor.toFloat();
        g.setColour(juce::Colour(0xff05060a));
        g.fillRoundedRectangle(s, 6.0f);
        g.setColour(juce::Colours::black.withAlpha(0.7f));
        g.drawRoundedRectangle(s.reduced(0.5f), 6.0f, 1.0f);
        const auto c = s.getCentre();
        for (float rad = 11.0f; rad >= 3.0f; rad -= 2.0f) {
            g.setColour(juce::Colour(0xff3aa0ff).withAlpha(rad <= 3.5f ? 0.95f : 0.10f));
            g.fillEllipse(juce::Rectangle<float>(rad * 2.0f, rad * 2.0f).withCentre(c));
        }
    }

    // Bottom hardware-strip captions.
    {
        auto cap = [&g](juce::Rectangle<int> rr, const juce::String& t) {
            if (rr.isEmpty()) return;
            g.setColour(CadenzaLookAndFeel::textDim());
            g.setFont(juce::Font(juce::FontOptions(8.5f, juce::Font::bold)));
            g.drawText(t, rr, juce::Justification::centred, false);
        };
        cap(m_hwLeftCap, "LEFT");   cap(m_hwRightCap, "RIGHT 1");
        cap(m_splitCap,  "SPLIT POINT");
        cap(m_partCap,   "PART ON / OFF");
        cap(m_regMemCap, "REGISTRATION MEMORY");
        cap(m_exitCap,   "EXIT");   cap(m_menuCap, "MENU");   cap(m_xfaderCap, "X-FADER");
    }

    // Pitch-bend + modulation wheel labels (the real sliders render above them).
    if (! m_wheelsZone.isEmpty())
    {
        const int labelH = 12;
        g.setColour(CadenzaLookAndFeel::textDim());
        g.setFont(juce::Font(juce::FontOptions(7.0f, juce::Font::bold)));
        auto lab = juce::Rectangle<int>(m_wheelsZone.getX(), m_wheelsZone.getBottom() - labelH,
                                        m_wheelsZone.getWidth(), labelH);
        g.drawText("PITCH", lab.removeFromLeft(m_wheelsZone.getWidth() / 2),
                   juce::Justification::centred, false);
        g.drawText("MOD", lab, juce::Justification::centred, false);
    }

    // Status-bar readouts: BPM / TRANSPOSE / TIME SIGNATURE / CPU / MIDI.
    if (! m_statusReadout.isEmpty())
    {
        auto r = m_statusReadout;
        auto readout = [&g, &r](const juce::String& label, const juce::String& value, int w) {
            auto col = r.removeFromLeft(w);
            g.setColour(CadenzaLookAndFeel::textDim());
            g.setFont(juce::Font(juce::FontOptions(8.5f, juce::Font::bold)));
            g.drawText(label, col.removeFromTop(13), juce::Justification::centredLeft, false);
            g.setColour(CadenzaLookAndFeel::cream());
            g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
            g.drawText(value, col, juce::Justification::centredLeft, false);
        };
        readout("BPM", m_bpmVal.getText(), 58);
        readout("TRANSPOSE", m_transposeVal.getText(), 112);
        readout("TIME SIGNATURE", "4/4", 116);

        auto cpu = r.removeFromLeft(56);
        g.setColour(CadenzaLookAndFeel::textDim());
        g.setFont(juce::Font(juce::FontOptions(8.5f, juce::Font::bold)));
        g.drawText("CPU", cpu.removeFromTop(13), juce::Justification::centredLeft, false);
        int bx = cpu.getX();
        for (int i = 0; i < 3; ++i) {
            const float h = 5.0f + i * 4.0f;
            g.setColour(i < 2 ? CadenzaLookAndFeel::accent() : CadenzaLookAndFeel::outline());
            g.fillRoundedRectangle(juce::Rectangle<float>((float) bx, cpu.getBottom() - h, 6.0f, h), 1.0f);
            bx += 9;
        }

        auto midi = r.removeFromLeft(54);
        g.setColour(CadenzaLookAndFeel::textDim());
        g.setFont(juce::Font(juce::FontOptions(8.5f, juce::Font::bold)));
        g.drawText("MIDI", midi.removeFromTop(13), juce::Justification::centredLeft, false);
        g.setColour(CadenzaLookAndFeel::accent());
        g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f)
                          .withCentre({ (float) midi.getX() + 8.0f, (float) midi.getCentreY() + 3.0f }));
    }
    if (! m_topMasterCap.isEmpty())
    {
        g.setColour(CadenzaLookAndFeel::textDim());
        g.setFont(juce::Font(juce::FontOptions(8.5f, juce::Font::bold)));
        g.drawText("MASTER VOLUME", m_topMasterCap, juce::Justification::centredRight, false);
    }
}

void NativePanel::resized()
{
    auto full = getLocalBounds().reduced(10);
    const int row = 34;
    const int gap = 6;

    m_cards.clear();
    auto cardHere = [this](const juce::Rectangle<int>& a, int y0) {
        m_cards.push_back(juce::Rectangle<int>(a.getX() - 5, y0 - 4, a.getWidth() + 10, (a.getY() - y0) + 8));
    };

    // On-screen keyboard full-width at the very bottom; pitch/mod wheels on its left.
    if (m_keyboard) {
        auto kbRow = full.removeFromBottom(126);
        m_wheelsZone = kbRow.removeFromLeft(96);
        {
            auto wz = m_wheelsZone.reduced(10, 10);
            wz.removeFromBottom(12);   // room for the PITCH / MOD labels
            auto w1 = wz.removeFromLeft(wz.getWidth() / 2 - 3);
            wz.removeFromLeft(6);
            m_pitchWheel.setBounds(w1);
            m_modWheel.setBounds(wz);
        }
        auto kb = kbRow.reduced(0, 4);
        auto bar = kb.removeFromTop(16);
        if (m_splitBar) m_splitBar->setBounds(bar);
        m_keyboard->setBounds(kb);
        // Stretch the keys so the visible C2..C7 range fills the width (no blank gap).
        int whiteKeys = 0;
        for (int n = 36; n <= 96; ++n) {
            const int pc = n % 12;
            if (pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11)
                ++whiteKeys;
        }
        if (whiteKeys > 0)
            m_keyboard->setKeyWidth((float) kb.getWidth() / (float) whiteKeys);
        full.removeFromBottom(gap);
    }

    // Bottom hardware strip (full width): LEFT/RIGHT knobs + split, PART ON/OFF,
    // registration memory, EXIT/MENU, X-Fader.
    {
        auto strip = full.removeFromBottom(96);
        m_bottomStrip = strip;
        m_cards.push_back(strip);
        auto s = strip.reduced(12, 9);

        auto left = s.removeFromLeft(232);
        {
            auto k1 = left.removeFromLeft(56);
            m_hwLeftCap = k1.removeFromBottom(13);
            m_hwLeft.setBounds(k1.reduced(8, 2));
            auto k2 = left.removeFromLeft(56);
            m_hwRightCap = k2.removeFromBottom(13);
            m_hwRight.setBounds(k2.reduced(8, 2));
            left.removeFromLeft(8);
            m_splitCap = left.removeFromTop(26);
            m_splitSet.setBounds(left.removeFromTop(30).reduced(6, 1));
        }

        auto right = s.removeFromRight(190);
        {
            auto xf = right.removeFromRight(62);
            m_xfaderCap = xf.removeFromBottom(13);
            m_xfader.setBounds(xf.reduced(8, 2));
            right.removeFromRight(12);
            auto ex = right.removeFromLeft(right.getWidth() / 2);
            m_exitCap = ex.removeFromTop(14); m_exitBtn.setBounds(ex.removeFromTop(28).reduced(2, 1));
            m_menuCap = right.removeFromTop(14); m_menuBtn.setBounds(right.removeFromTop(28).reduced(2, 1));
        }

        auto partZone = s.removeFromLeft(s.getWidth() * 46 / 100);
        m_partCap = partZone.removeFromTop(14);
        {
            auto r = partZone.removeFromTop(30);
            const int pw = (r.getWidth() - 7 * 3) / 8;
            for (auto& b : m_partButtons) { b->setBounds(r.removeFromLeft(pw)); r.removeFromLeft(3); }
        }
        s.removeFromLeft(14);
        m_regMemCap = s.removeFromTop(14);
        {
            auto r = s.removeFromTop(30);
            const int rw = (r.getWidth() - 7 * 3) / 8;
            for (auto& b : m_regMemButtons) { b->setBounds(r.removeFromLeft(rw)); r.removeFromLeft(3); }
        }
        full.removeFromBottom(gap);
    }

    // Left HARDWARE faceplate (full height of the remaining area).
    {
        auto hw = full.removeFromLeft(286);
        m_hwPanel = hw;
        m_cards.push_back(hw);
        auto p = hw.reduced(16, 14);

        m_logoMain.setBounds(p.removeFromTop(40));
        m_logoSub.setBounds(p.removeFromTop(16));
        p.removeFromTop(20);

        // Master Volume + Balance brass knobs (captions painted in paint()).
        {
            auto kr = p.removeFromTop(86);
            auto left = kr.removeFromLeft(kr.getWidth() / 2);
            m_hwMasterCap  = left.removeFromTop(14);
            m_hwMasterVol.setBounds(left.reduced(10, 2));
            m_hwBalanceCap = kr.removeFromTop(14);
            m_hwBalance.setBounds(kr.reduced(10, 2));
        }
        p.removeFromTop(14);

        // D-Beam sensor (arrows are buttons; the glowing middle is painted).
        {
            p.removeFromTop(14);   // caption space
            auto db = p.removeFromTop(46);
            m_dbeamL.setBounds(db.removeFromLeft(42).reduced(2, 6));
            m_dbeamR.setBounds(db.removeFromRight(42).reduced(2, 6));
            m_dbeamSensor = db.reduced(4, 4);
        }
        p.removeFromTop(14);

        // Assignable buttons.
        {
            m_hwAssignCap = p.removeFromTop(14);
            auto ar = p.removeFromTop(32);
            m_assign1.setBounds(ar.removeFromLeft(ar.getWidth() / 2 - 4));
            ar.removeFromLeft(8);
            m_assign2.setBounds(ar);
        }

        // Style Control + transport, anchored to the bottom.
        {
            auto bottom = p.removeFromBottom(108);
            m_hwStyleCtlCap = bottom.removeFromTop(14);
            auto sc = bottom.removeFromTop(38);
            juce::TextButton* scs[] = { &m_scIntro, &m_scVari, &m_scFill, &m_scBreak, &m_scEnd };
            const int scw = (sc.getWidth() - 4 * 4) / 5;
            for (auto* b : scs) { b->setBounds(sc.removeFromLeft(scw)); sc.removeFromLeft(4); }
            bottom.removeFromTop(8);
            auto tr = bottom.removeFromTop(44);
            const int tw = (tr.getWidth() - 3 * 4) / 4;
            m_syncroStart.setBounds(tr.removeFromLeft(tw)); tr.removeFromLeft(4);
            m_play.setBounds(tr.removeFromLeft(tw));        tr.removeFromLeft(4);  // START/STOP
            m_bpmTap.setBounds(tr.removeFromLeft(tw));      tr.removeFromLeft(4);  // TAP TEMPO
            m_resetTempo.setBounds(tr);
        }
        full.removeFromLeft(gap);
    }

    // Top status bar (card): painted readouts (left), editable tempo + master right.
    {
        auto sb = full.removeFromTop(48);
        m_cards.push_back(sb);
        sb.reduce(12, 7);

        auto mv = sb.removeFromRight(196);
        m_topMasterCap = mv.removeFromLeft(96);
        m_topMaster.setBounds(mv.reduced(2, 11));
        sb.removeFromRight(18);

        auto tg = sb.removeFromRight(168);
        m_bpmCaption.setBounds(tg.removeFromLeft(46));
        m_tempoWheel.setBounds(tg.removeFromLeft(42));
        tg.removeFromLeft(6);
        m_bpmVal.setBounds(tg);
        sb.removeFromRight(14);

        m_statusReadout = sb;   // painted BPM / TRANSPOSE / TIME SIGNATURE / CPU / MIDI
    }
    full.removeFromTop(gap);

    // Nav rail (icon column, right of the hardware panel).
    {
        auto nav = full.removeFromLeft(150);
        m_cards.push_back(nav);
        nav.reduce(10, 10);
        for (auto& b : m_navButtons) { b->setBounds(nav.removeFromTop(40)); nav.removeFromTop(6); }
        full.removeFromLeft(gap);
    }

    auto area = full;   // content area, right of the nav rail

    // Nav paging: nav order = Home, Song, Style, Sound, Mixer, Effect, Setting, Editor.
    // The upper card band is always shown; full-width groups switch per page.
    const int page = m_navActive;
    auto onPage = [page](std::initializer_list<int> ps) {
        for (int p : ps) if (p == page) return true;
        return false;
    };
    auto setVis = [](bool v, std::initializer_list<juce::Component*> cs) {
        for (auto* c : cs) if (c != nullptr) c->setVisible(v);
    };
    const bool showSections = onPage({ 0, 1, 2 });   // Home, Song, Style
    const bool showRec      = onPage({ 2 });         // Style
    const bool showVoices   = onPage({ 3 });         // Sound
    const bool showEQ       = onPage({ 3, 5 });      // Sound, Effect
    const bool showMixer    = onPage({ 0, 4 });      // Home, Mixer
    const bool showSettings = onPage({ 6 });         // Setting (file/device actions)
    const bool showEditor   = onPage({ kEditorPage });   // Editor (full-area piano roll)
    setVis(showSettings, { &m_fade, &m_openStyle, &m_importMidiStyle, &m_openSf,
                            &m_openAudio, &m_openMidi, &m_openAnalyze, &m_aiSettings });

    // On the Editor page the embedded piano roll owns the whole content area, so the
    // always-on upper band (chord LCD, transpose, registrations, OTS, pads) is hidden.
    setVis(!showEditor, { &m_styleName, &m_chord,
        &m_transposeCaption, &m_transposeDown, &m_transposeVal, &m_transposeUp,
        &m_octaveCaption, &m_octaveDown, &m_octaveVal, &m_octaveUp,
        &m_arranger, &m_chordMemory, &m_syncroStop, &m_autoFill, &m_fingeredOnBass,
        &m_regCaption, &m_regStore, &m_otsCaption, &m_otsLink, &m_padsCaption });
    for (auto& b : m_regButtons) if (b) b->setVisible(!showEditor);
    for (auto& b : m_otsButtons) if (b) b->setVisible(!showEditor);
    for (auto& p : m_pads)       if (p) p->setVisible(!showEditor);

    // Mixer band above the keyboard (inside the content area).
    if (showMixer) {
        auto mixerArea = area.removeFromBottom(168);
        m_cards.push_back(mixerArea.expanded(5, 4));
        m_mixerCaption.setBounds(mixerArea.removeFromTop(18));
        const int sw = 78, sgap = 6;
        int mx = mixerArea.getX();
        for (auto& s : m_mixer) {
            juce::Rectangle<int> col(mx, mixerArea.getY(), sw, mixerArea.getHeight());
            s.name->setBounds(col.removeFromTop(15));
            s.instrument->setBounds(col.removeFromTop(18).reduced(1, 0));
            auto btns = col.removeFromBottom(20);
            s.volume->setBounds(col.reduced(6, 2));
            s.mute->setBounds(btns.removeFromLeft(sw / 2 - 2));
            btns.removeFromLeft(4);
            s.solo->setBounds(btns);
            mx += sw + sgap;
        }
        area.removeFromBottom(gap);
    }
    m_mixerCaption.setVisible(showMixer);
    for (auto& s : m_mixer)
        setVis(showMixer, { s.name.get(), s.instrument.get(), s.volume.get(), s.mute.get(), s.solo.get() });

    // Two-column upper band: a performance card (left) and a memory card (right).
    if (!showEditor) {
        auto band = area.removeFromTop(222);
        auto bandL = band.removeFromLeft((band.getWidth() - gap) * 56 / 100);
        band.removeFromLeft(gap);
        auto bandR = band;

        // LEFT card: style header, chord LCD (name + mini-piano), transpose/octave, toggles.
        {
            m_cards.push_back(bandL);
            auto col = bandL.reduced(9, 9);
            m_styleName.setBounds(col.removeFromTop(20));
            m_styleDetail = col.removeFromTop(13);
            col.removeFromTop(3);
            auto lcd = col.removeFromTop(72);
            m_chordCard = lcd.reduced(1, 1);
            m_chord.setBounds(lcd.removeFromTop(44));
            m_chordPiano = lcd.reduced(10, 3);
            col.removeFromTop(8);

            auto tr = col.removeFromTop(28);
            m_transposeCaption.setBounds(tr.removeFromLeft(70));
            m_transposeDown.setBounds(tr.removeFromLeft(30)); tr.removeFromLeft(3);
            m_transposeVal.setBounds(tr.removeFromLeft(72));  tr.removeFromLeft(3);
            m_transposeUp.setBounds(tr.removeFromLeft(30));   tr.removeFromLeft(gap * 2);
            m_octaveCaption.setBounds(tr.removeFromLeft(58));
            m_octaveDown.setBounds(tr.removeFromLeft(30));    tr.removeFromLeft(3);
            m_octaveVal.setBounds(tr.removeFromLeft(40));     tr.removeFromLeft(3);
            m_octaveUp.setBounds(tr.removeFromLeft(30));
            col.removeFromTop(gap);

            juce::ToggleButton* tgs[] = { &m_arranger, &m_chordMemory, &m_syncroStop,
                                          &m_autoFill, &m_fingeredOnBass };
            const int tgColW = col.getWidth() / 2;
            for (int i = 0; i < 5; ++i) {
                const int rr = i / 2, cc = i % 2;
                tgs[i]->setBounds(col.getX() + cc * tgColW, col.getY() + rr * 24, tgColW, 22);
            }
        }

        // RIGHT card: registrations, one-touch settings, pads.
        {
            m_cards.push_back(bandR);
            auto col = bandR.reduced(9, 9);
            m_regCaption.setBounds(col.removeFromTop(16));
            {
                auto r = col.removeFromTop(26);
                m_regStore.setBounds(r.removeFromLeft(58)); r.removeFromLeft(6);
                const int bw = 34, bgap = 5;
                for (auto& b : m_regButtons) { b->setBounds(r.removeFromLeft(bw)); r.removeFromLeft(bgap); }
            }
            col.removeFromTop(gap);
            m_otsCaption.setBounds(col.removeFromTop(16));
            {
                auto r = col.removeFromTop(26);
                m_otsLink.setBounds(r.removeFromLeft(72)); r.removeFromLeft(6);
                const int bw = 44, bgap = 5;
                for (auto& b : m_otsButtons) { if (b) b->setBounds(r.removeFromLeft(bw)); r.removeFromLeft(bgap); }
            }
            col.removeFromTop(gap);
            m_padsCaption.setBounds(col.removeFromTop(16));
            {
                auto r = col.removeFromTop(28);
                const int pw = (r.getWidth() - 3 * 6) / 4;
                for (auto& p : m_pads) { p->setBounds(r.removeFromLeft(pw)); r.removeFromLeft(6); }
            }
        }
        area.removeFromTop(gap);
    }

    // Setting page: file / device actions (keeps the top bar clean).
    if (showSettings) {
        auto r = area.removeFromTop(72);
        auto fileRow = r.removeFromTop(30);
        m_fade.setBounds(fileRow.removeFromLeft(82));       fileRow.removeFromLeft(gap);
        m_openStyle.setBounds(fileRow.removeFromLeft(112)); fileRow.removeFromLeft(gap);
        m_importMidiStyle.setBounds(fileRow.removeFromLeft(162)); fileRow.removeFromLeft(gap);
        m_openSf.setBounds(fileRow.removeFromLeft(132));    fileRow.removeFromLeft(gap);
        m_openAnalyze.setBounds(fileRow.removeFromLeft(118));
        r.removeFromTop(8);
        auto deviceRow = r.removeFromTop(30);
        m_openAudio.setBounds(deviceRow.removeFromLeft(90));  deviceRow.removeFromLeft(gap);
        m_openMidi.setBounds(deviceRow.removeFromLeft(80));   deviceRow.removeFromLeft(gap);
        m_aiSettings.setBounds(deviceRow.removeFromLeft(120));
        area.removeFromTop(gap);
    }

    // Master EQ row: caption + 3 labelled knobs.
    setVis(showEQ, { &m_eqCaption, &m_eqLowCap, &m_eqMidCap, &m_eqHighCap, &m_compCap,
                     &m_masterCap, &m_drumsCap, &m_reverbCap, &m_eqLow, &m_eqMid, &m_eqHigh,
                     &m_comp, &m_master, &m_drums, &m_reverb, &m_polished });
    if (showEQ) {
        const int y0 = area.getY();
        auto r = area.removeFromTop(64);
        m_eqCaption.setBounds(r.removeFromLeft(110).withTrimmedTop(24));
        const int kw = 64;
        auto placeKnob = [&](juce::Slider& s, juce::Label& cap) {
            auto col = r.removeFromLeft(kw);
            cap.setBounds(col.removeFromTop(14));
            s.setBounds(col);
            r.removeFromLeft(6);
        };
        placeKnob(m_eqLow, m_eqLowCap);
        placeKnob(m_eqMid, m_eqMidCap);
        placeKnob(m_eqHigh, m_eqHighCap);
        r.removeFromLeft(14);
        placeKnob(m_comp, m_compCap);
        placeKnob(m_master, m_masterCap);
        placeKnob(m_drums, m_drumsCap);
        placeKnob(m_reverb, m_reverbCap);
        // Polished Master toggle sits to the right of the knobs (Effect/Sound page).
        if (r.getWidth() > 40)
            m_polished.setBounds(r.removeFromLeft(std::min(160, r.getWidth())).withTrimmedTop(24).withHeight(24));
        cardHere(area, y0);
        area.removeFromTop(gap);
    }

    // Right Voices row: three layer columns (enable + instrument / volume + octave).
    {
        auto& v0 = m_rightVoices[0]; auto& v1 = m_rightVoices[1]; auto& v2 = m_rightVoices[2];
        auto& vL = m_leftStrip;
        setVis(showVoices, { &m_rightCaption,
            vL.enable.get(), vL.instrument.get(), vL.volume.get(), vL.octDown.get(), vL.octUp.get(), vL.octVal.get(),
            v0.enable.get(), v0.instrument.get(), v0.volume.get(), v0.octDown.get(), v0.octUp.get(), v0.octVal.get(),
            v1.enable.get(), v1.instrument.get(), v1.volume.get(), v1.octDown.get(), v1.octUp.get(), v1.octVal.get(),
            v2.enable.get(), v2.instrument.get(), v2.volume.get(), v2.octDown.get(), v2.octUp.get(), v2.octVal.get() });
    }
    if (showVoices) {
        const int y0 = area.getY();
        m_rightCaption.setBounds(area.removeFromTop(18));
        auto r = area.removeFromTop(50);
        const int colGap = 10;
        // Four columns: Left, Right 1, Right 2, Right 3.
        RightVoiceStrip* strips[4] = { &m_leftStrip, &m_rightVoices[0], &m_rightVoices[1], &m_rightVoices[2] };
        const int colW = (r.getWidth() - 3 * colGap) / 4;
        for (int i = 0; i < 4; ++i) {
            auto& s = *strips[i];
            auto col = r.removeFromLeft(colW);
            if (i < 3) r.removeFromLeft(colGap);

            auto top = col.removeFromTop(24);
            if (s.enable)     s.enable->setBounds(top.removeFromLeft(74));
            if (s.instrument) s.instrument->setBounds(top.reduced(2, 1));

            auto bot = col;
            auto oct = bot.removeFromRight(84);
            if (s.octDown) s.octDown->setBounds(oct.removeFromLeft(24));
            if (s.octVal)  s.octVal->setBounds(oct.removeFromLeft(36));
            if (s.octUp)   s.octUp->setBounds(oct.removeFromLeft(24));
            if (s.volume)  s.volume->setBounds(bot.reduced(2, 4));
        }
        cardHere(area, y0);
        area.removeFromTop(gap);
    }

    // Style Recorder row: bars + part pickers, transport-style buttons, status.
    setVis(showRec, { &m_recCaption, &m_recNew, &m_recBars, &m_recPart, &m_recArm, &m_recClick,
                      &m_recEdit, &m_recClear, &m_recSave, &m_recExit, &m_recStatus,
                      &m_recMakeEditable, &m_aiStyle, &m_aiAddFills, &m_aiPolish });
    if (showRec) {
        const int y0 = area.getY();
        auto capRow = area.removeFromTop(20);
        m_aiPolish.setBounds(capRow.removeFromRight(82));
        capRow.removeFromRight(6);
        m_aiAddFills.setBounds(capRow.removeFromRight(104));
        capRow.removeFromRight(6);
        m_aiStyle.setBounds(capRow.removeFromRight(90));
        capRow.removeFromRight(6);
        m_recMakeEditable.setBounds(capRow.removeFromRight(120));
        m_recCaption.setBounds(capRow);
        auto r = area.removeFromTop(28);
        m_recNew.setBounds(r.removeFromLeft(54));    r.removeFromLeft(6);
        m_recBars.setBounds(r.removeFromLeft(76));   r.removeFromLeft(6);
        m_recPart.setBounds(r.removeFromLeft(92));   r.removeFromLeft(6);
        m_recArm.setBounds(r.removeFromLeft(76));    r.removeFromLeft(6);
        m_recClick.setBounds(r.removeFromLeft(58));  r.removeFromLeft(6);
        m_recEdit.setBounds(r.removeFromLeft(56));   r.removeFromLeft(6);
        m_recClear.setBounds(r.removeFromLeft(82));  r.removeFromLeft(6);
        m_recSave.setBounds(r.removeFromLeft(64));   r.removeFromLeft(6);
        m_recExit.setBounds(r.removeFromLeft(50));   r.removeFromLeft(10);
        m_recStatus.setBounds(r);
        cardHere(area, y0);
        area.removeFromTop(gap);
    }

    // Song page analysis card.
    const bool showAnalysis = (page == 1);
    m_chordAnalysis.setVisible(showAnalysis);
    if (showAnalysis) {
        const int y0 = area.getY();
        auto analysisCard = area.removeFromTop(120);
        m_cards.push_back(analysisCard.expanded(5, 4));
        m_chordAnalysis.setBounds(analysisCard.reduced(10, 10));
        cardHere(area, y0);
        area.removeFromTop(gap);
    }

    // Sections caption + flowing buttons fill the rest.
    m_sectionsCaption.setVisible(showSections);
    for (auto& s : m_sections) s.button->setVisible(showSections);
    if (showSections) {
        m_cards.push_back(area.expanded(5, 4));
        m_sectionsCaption.setBounds(area.removeFromTop(22));
        const int bw = 92, bh = 30, bgap = 6;
        int x = area.getX(), y = area.getY();
        for (auto& s : m_sections) {
            if (x + bw > area.getRight()) { x = area.getX(); y += bh + bgap; }
            s.button->setBounds(x, y, bw, bh);
            x += bw + bgap;
        }
    }

    // Editor page: the embedded piano-roll editor fills the whole content area
    // (nothing else above consumed it on this page).
    if (m_editor) {
        m_editor->setVisible(showEditor);
        if (showEditor)
            m_editor->setBounds(area);
    }

    m_aiOverlay.setBounds(getLocalBounds());
    if (m_aiOverlay.isVisible())
        m_aiOverlay.toFront(false);
}
}

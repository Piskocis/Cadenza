#include "SynthEngine.h"

#if defined(CADENZA_HAVE_FLUIDSYNTH)
  #include <fluidsynth.h>
  #include <mutex>
#endif

namespace cadenza::audio
{
namespace
{
constexpr bool kLogResolvedPresets = false;
}

// ============================================================
// NullSynthEngine — logs to JUCE Logger; produces silence.
// ============================================================
void NullSynthEngine::renderBlock(juce::AudioBuffer<float>& buffer)
{
    buffer.clear();
}

void NullSynthEngine::noteOn(int channel, int note, int velocity)
{
    juce::ignoreUnused(channel, note, velocity);
}

void NullSynthEngine::noteOff(int channel, int note)
{
    juce::ignoreUnused(channel, note);
}

void NullSynthEngine::programChange(int channel, int program)
{
    juce::ignoreUnused(channel, program);
}

void NullSynthEngine::controlChange(int channel, int controller, int value)
{
    juce::ignoreUnused(channel, controller, value);
}

void NullSynthEngine::allNotesOff()
{
}

// ============================================================
// FluidSynthEngine — wraps fluidsynth (if available)
// ============================================================
#if defined(CADENZA_HAVE_FLUIDSYNTH)

class FluidSynthEngine final : public SynthEngine
{
public:
    FluidSynthEngine() {
        m_settings = new_fluid_settings();
        if (m_settings) {
            // Configure for offline-rendering style (we render into JUCE's buffer ourselves).
            fluid_settings_setstr(m_settings, "audio.driver", "file");
            fluid_settings_setnum(m_settings, "synth.gain", 0.5);   // headroom; master limiter catches peaks
            fluid_settings_setint(m_settings, "synth.audio-channels", 1);
            fluid_settings_setint(m_settings, "synth.lock-memory", 0);

            // Yamaha styles (and the Timbres of Heaven XGM SoundFont) are XG-based.
            // FluidSynth defaults to GS bank-select, under which the styles' XG
            // bank addresses (e.g. MSB 8 voice variations, MSB 127 drum kits) do
            // not resolve and every voice collapses to GM bank 0 (piano). XG mode
            // makes those banks resolve to the SoundFont's richer XG presets.
            fluid_settings_setstr(m_settings, "synth.midi-bank-select", "xg");

            // Plenty of voices: drums + bass + chords + 3 right-hand layers on a
            // busy style can exceed the 256 default and steal/cut notes.
            fluid_settings_setint(m_settings, "synth.polyphony", 512);

            // Ambience: a moderate hall reverb + gentle chorus so the band has
            // depth and air without sounding washed-out/muddy. Pulled back from
            // the old very-wet defaults. Parts still control send via CC91/CC93,
            // and the user's Reverb knob overrides the level live.
            fluid_settings_setint(m_settings, "synth.reverb.active", 1);
            fluid_settings_setint(m_settings, "synth.chorus.active", 1);
            fluid_settings_setnum(m_settings, "synth.reverb.room-size", 0.60);
            fluid_settings_setnum(m_settings, "synth.reverb.damp", 0.40);
            fluid_settings_setnum(m_settings, "synth.reverb.width", 0.80);
            fluid_settings_setnum(m_settings, "synth.reverb.level", 0.55);
            // Gentle chorus (defaults are too deep/fast and make pads sea-sick).
            fluid_settings_setint(m_settings, "synth.chorus.nr", 3);
            fluid_settings_setnum(m_settings, "synth.chorus.level", 1.0);
            fluid_settings_setnum(m_settings, "synth.chorus.speed", 0.30);
            fluid_settings_setnum(m_settings, "synth.chorus.depth", 5.0);

            m_synth = new_fluid_synth(m_settings);
            if (m_synth) {
                // Highest-quality sample interpolation on every channel: smoother,
                // less "digital"/aliased tone than the 4th-order default.
                fluid_synth_set_interp_method(m_synth, -1, FLUID_INTERP_HIGHEST);

                // RHY1 (synth ch 9) and RHY2 (synth ch 8) must be DRUM-type
                // channels. On a melodic channel the bank-128 kit select fails
                // and FluidSynth silently falls back to a bank-0 melodic voice —
                // sub-rhythm hits played an English Horn instead of a kit.
                forceDrumChannelTypes();
            }
            juce::Logger::writeToLog("[Cadenza] FluidSynth drum channels active: synth channels 9 (RHY1) + 8 (RHY2); reverb+chorus on; interp=highest; poly=512");
        }
    }

    ~FluidSynthEngine() override {
        if (m_synth) delete_fluid_synth(m_synth);
        if (m_settings) delete_fluid_settings(m_settings);
    }

    void prepare(double sampleRate, int /*blockSize*/) override {
        SynthEngine::prepare(sampleRate, 0);
        if (m_settings) {
            fluid_settings_setnum(m_settings, "synth.sample-rate", sampleRate);
        }
    }

    void renderBlock(juce::AudioBuffer<float>& buffer) override {
        buffer.clear();
        if (!m_synth) return;

        const int n = buffer.getNumSamples();
        float* left  = buffer.getWritePointer(0);
        float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : left;

        // fluid_synth_write_float writes interleaved into two separate float arrays.
        const std::lock_guard<std::mutex> lock(m_mutex);
        fluid_synth_write_float(m_synth, n, left, 0, 1, right, 0, 1);
    }

    void noteOn(int channel, int note, int velocity) override {
        if (!m_synth) return;
        const std::lock_guard<std::mutex> lock(m_mutex);
        fluid_synth_noteon(m_synth, channel, note, velocity);
    }

    void noteOff(int channel, int note) override {
        if (!m_synth) return;
        const std::lock_guard<std::mutex> lock(m_mutex);
        fluid_synth_noteoff(m_synth, channel, note);
    }

    void programChange(int channel, int program) override {
        if (!m_synth) return;
        const std::lock_guard<std::mutex> lock(m_mutex);

        if (isDrumSynthChannel(channel)) {
            forceDrumChannelType(channel);
            // Drum kits live on the GM percussion bank (128) in GM/GS/XG
            // SoundFonts. Yamaha/Genos styles move this channel to the XG drum
            // bank (CC0=127), which most SoundFonts don't have, so the kit lookup
            // fails and drums garble. Force bank 128 where kits actually are, then
            // select the kit, falling back to the standard kit if the style's kit
            // number isn't present. Two channels are drums: synth ch 9 = RHY1 main
            // kit, synth ch 8 = RHY2 sub-rhythm.
            fluid_synth_bank_select(m_synth, channel, kDrumBank);
            fluid_synth_program_change(m_synth, channel, program);
            bool kitFellBack = false;
            if (fluid_synth_get_channel_preset(m_synth, channel) == nullptr) {
                fluid_synth_program_change(m_synth, channel, 0);   // standard kit
                kitFellBack = true;
            }
            if constexpr (kLogResolvedPresets)
                logResolvedPreset(channel, program, kitFellBack);
            return;
        }

        // Non-rhythm channels must be explicitly restored to melodic mode. In
        // XG bank-select mode a previous CC0 can turn a channel into a drum
        // channel, and a later program change alone does not turn it back. This
        // is especially visible when manual Right voices reuse a style channel.
        fluid_synth_set_channel_type(m_synth, channel, CHANNEL_TYPE_MELODIC);

        // Melodic: select the requested voice (bank set by prior CC0/CC32).
        fluid_synth_program_change(m_synth, channel, program);

        // Cross-SoundFont fallback: if the requested bank/program isn't in the
        // loaded SoundFont, FluidSynth leaves the channel with no preset. So a
        // Yamaha/XG style voice plays correctly on an XG SoundFont (e.g. Timbres
        // of Heaven) but still gets a sensible GM voice on a plain GM SoundFont.
        bool fellBack = false;
        if (fluid_synth_get_channel_preset(m_synth, channel) == nullptr) {
            fluid_synth_bank_select(m_synth, channel, 0);
            fluid_synth_program_change(m_synth, channel, program);
            fellBack = true;
        }

        if constexpr (kLogResolvedPresets)
            logResolvedPreset(channel, program, fellBack);
    }

    void controlChange(int channel, int controller, int value) override {
        if (!m_synth) return;
        const std::lock_guard<std::mutex> lock(m_mutex);
        fluid_synth_cc(m_synth, channel, controller, value);
    }

    void pitchBend(int channel, int value14) override {
        if (!m_synth) return;
        const std::lock_guard<std::mutex> lock(m_mutex);
        // FluidSynth expects a 14-bit value (0..16383, 8192 = centre).
        fluid_synth_pitch_bend(m_synth, channel, value14);
    }

    void allNotesOff() override {
        if (!m_synth) return;
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (int ch = 0; ch < 16; ++ch) fluid_synth_all_notes_off(m_synth, ch);
    }

    void setReverbLevel(double level) override {
        if (!m_synth) return;
        const std::lock_guard<std::mutex> lock(m_mutex);
        fluid_synth_set_reverb_group_level(m_synth, -1, level);   // -1 = all fx groups
    }

    bool loadSoundFont(const std::string& path) override {
        if (!m_synth) return false;
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_soundFontId >= 0) {
            fluid_synth_sfunload(m_synth, m_soundFontId, 1);
            m_soundFontId = -1;
        }
        m_soundFontId = fluid_synth_sfload(m_synth, path.c_str(), 1);
        if (m_soundFontId >= 0) {
            // sfload(reset=1) reprograms every channel; keep both playback drum
            // channels explicitly percussive before callers re-issue programs.
            forceDrumChannelTypes();
        }
        return m_soundFontId >= 0;
    }

    const char* engineName() const noexcept override { return "FluidSynthEngine"; }
    bool supportsSoundFonts() const noexcept override { return true; }

private:
    // GM percussion channel (0-based). Voice fallback must not move it off the
    // percussion bank.
    static constexpr int kDrumChannel = 9;
    // Second drum channel for Yamaha RHY2 (sub-rhythm), mapped from cadenza ch 9.
    static constexpr int kDrumChannel2 = 8;
    // GM/GS/XG SoundFonts keep every drum kit on bank 128 (the percussion bank).
    static constexpr int kDrumBank = 128;

    static constexpr bool isDrumSynthChannel(int ch) noexcept {
        return ch == kDrumChannel || ch == kDrumChannel2;
    }

    void forceDrumChannelType(int channel) {
        if (m_synth)
            fluid_synth_set_channel_type(m_synth, channel, CHANNEL_TYPE_DRUM);
    }

    void forceDrumChannelTypes() {
        // FluidSynth resets can rebuild channel state, so assert both channels
        // through the same explicit API path.
        forceDrumChannelType(kDrumChannel);
        forceDrumChannelType(kDrumChannel2);
    }

    // Log which SoundFont preset a channel actually resolved to after a program
    // change. Caller holds m_mutex. Lets us confirm XG variation banks resolve to
    // real voices (vs. collapsing to plain GM) without having to listen.
    void logResolvedPreset(int channel, int requestedProgram, bool fellBack) {
        fluid_preset_t* preset = fluid_synth_get_channel_preset(m_synth, channel);
        const char* name = preset ? fluid_preset_get_name(preset) : "(none)";
        const int bank = preset ? fluid_preset_get_banknum(preset) : -1;
        const int prog = preset ? fluid_preset_get_num(preset) : -1;
        juce::Logger::writeToLog(
            "[Cadenza] Voice resolve ch=" + juce::String(channel)
            + " reqProgram=" + juce::String(requestedProgram)
            + " -> '" + juce::String(name) + "' bank=" + juce::String(bank)
            + " program=" + juce::String(prog)
            + (fellBack ? " (GM fallback)" : ""));
    }

    fluid_settings_t* m_settings = nullptr;
    fluid_synth_t*    m_synth = nullptr;
    int               m_soundFontId = -1;
    std::mutex        m_mutex;
};

#endif

// ============================================================
// Factory
// ============================================================
std::unique_ptr<SynthEngine> createSynthEngine()
{
#if defined(CADENZA_HAVE_FLUIDSYNTH)
    return std::make_unique<FluidSynthEngine>();
#else
    return std::make_unique<NullSynthEngine>();
#endif
}
}

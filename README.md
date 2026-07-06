# Cadenza

[![Windows build](https://github.com/Piskocis/Cadenza/actions/workflows/windows.yml/badge.svg)](https://github.com/Piskocis/Cadenza/actions/workflows/windows.yml)

**A free, open-source software arranger workstation for Windows.**

Play chords with your left hand and Cadenza plays a full backing band — drums, bass,
chords, and more — in real time, following your chords. Play a melody on top. It's the
software equivalent of a hardware "arranger keyboard," built with JUCE and C++.

> Status: actively developed, builds and runs on Windows. Contributors welcome —
> **especially musicians who know music theory** (see [Contributing](#contributing)).

![Cadenza arranger — main view](screenshots/cadenza-main.png)

---

## What it does

- 🎹 **Live auto-accompaniment** — play chords, hear a styled backing band that
  transposes to follow you in real time.
- 🎛️ **Arranger sections** — Intro / Main A–D / Fills / Ending, with transition fills,
  One Touch Settings, registrations, and a mixer.
- 🎼 **Style editor** — a full piano-roll editor (zoom, undo/redo, velocity lane,
  drag-to-duplicate, live tooltips) for authoring/tweaking native `.cstyle` styles.
- 🎵 **MIDI → Style converter** — import any General-MIDI `.mid` file and turn it into a
  playable, chord-transposable style:
  - **Auto-split** detects sections (Main A/B, Intro, Ending) from the song,
  - **preview** lets you audition sections by ear before converting,
  - **"Play in C"** normalizes a style so you can play it on easy white keys and use
    Transpose to reach the real key,
  - one-chord-per-section detection keeps imported styles in key.
- 🤖 **AI assist (optional, bring-your-own Anthropic API key)** — generate fills /
  intros / endings and "polish" a style's notes, using the Claude API. Fully opt-in.
- 🔊 **General-MIDI playback** via FluidSynth + a SoundFont; loads Yamaha `.sty/.prs`
  styles and the native JSON-based `.cstyle` format.

---

## Screenshots

**Import any MIDI → a playable, chord-following style** — Cadenza auto-detects the
chord, auto-splits the song into sections, normalizes to C for easy white-key play,
and can generate fills with AI:

![Import MIDI as Style](screenshots/cadenza-import.png)

**Style page** — sections (Main A–D, fills, ending), the per-part mixer, and the
AI assist actions (Add Fills / Polish):

![Style page](screenshots/cadenza-style.png)

---

## Build (Windows)

Requirements: **Visual Studio 2022/2026 (MSVC)**, **CMake**, **Ninja**, and **JUCE**
(cloned into `lib/JUCE`). FluidSynth is provided via vcpkg.

```powershell
# Clone JUCE into lib/ (not committed to this repo)
git clone https://github.com/juce-framework/JUCE.git lib/JUCE

# Configure + build (from a Visual Studio Developer shell, or after vcvars64)
cmake -S . -B build-msvc -G Ninja
cmake --build build-msvc
ctest --test-dir build-msvc --output-on-failure
```

Output: `build-msvc/Cadenza_artefacts/Debug/Cadenza Workstation.exe`.

A **SoundFont** is required for sound — drop a GM `.sf2` into `resources/sf2/`
(e.g. the freely-redistributable *GeneralUser GS*). Cadenza loads the one you
select, or the largest it finds there.

## Build (macOS / Linux core)

The portable arranger core, command-line tools, and unit tests can be built
without JUCE or FluidSynth. This is the fastest way to validate macOS/Linux
compiler and test coverage while the native GUI host remains Windows-first.

```sh
cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Debug -DCADENZA_BUILD_TESTS=ON
cmake --build build-core --parallel
ctest --test-dir build-core --output-on-failure
```

When `lib/JUCE` is not present, CMake skips the native GUI app target and still
builds the portable core/tests/tools. When FluidSynth is not installed, the
audio path uses the `NullSynthEngine` fallback.

---

## Contributing

Contributions are very welcome — code, bug reports, and especially **music-theory
expertise**. The hardest and most valuable open problems are musical:

- chord voicing / voice-leading when transposing a style to the played chord,
- Yamaha NTR/NTT transposition fidelity and color/scale-tone handling,
- drum-fill and section-flow musicality.

See [CONTRIBUTING.md](CONTRIBUTING.md) for build/PR details and the contributor
agreement. Good first issues will be tagged in the issue tracker.

---

## License

Cadenza is licensed under the **GNU General Public License v3.0** — see
[LICENSE](LICENSE). You're free to use, study, modify, and share it; derivative
works must also be GPLv3 and retain attribution.

**Commercial licensing:** the project is © its author (Cadenza). If you want to use
Cadenza in a **closed-source or commercial** product, that's not permitted under the
GPL — please contact **suko5rp2@gmail.com** to discuss a commercial license.

Built on **JUCE** (used under its GPLv3 option) and **FluidSynth** (LGPL). See
[NOTICE](NOTICE) for third-party acknowledgements.

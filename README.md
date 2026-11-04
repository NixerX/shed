# Shed — a tiny Reaper-ish DAW (JUCE 8)

Audio + MIDI tracks, VST3/LV2/AU hosting, a piano roll, recording, and WAV/MP3 mix + stem export.
~2,500 lines of C++. No undo, no automation, no mercy.

## Build

Needs CMake ≥ 3.22, a C++17 compiler, and JUCE 8 dropped into this folder:

```sh
git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git
```

### Linux (Debian/Ubuntu)
```sh
sudo apt install build-essential cmake ninja-build git lame \
  libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev \
  libxrandr-dev libxrender-dev libgl1-mesa-dev
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release          # add -DSHED_JACK=ON for JACK/PipeWire-JACK
cmake --build build
./build/Shed_artefacts/Release/Shed
```
(Release uses LTO — the link takes a few minutes. Use `-DCMAKE_BUILD_TYPE=RelWithDebInfo` or Debug if you're iterating.)

### macOS
`brew install cmake lame`, then the same cmake lines (use `-G Xcode` if you like). App ends up in `build/Shed_artefacts/Release/Shed.app`.

### Windows
VS 2022 + CMake: `cmake -B build` then open `build/Shed.sln` or `cmake --build build --config Release`.
For MP3, drop `lame.exe` next to `Shed.exe` (or point to it under Options).

## Using it

1. **Options → Audio/MIDI settings** – pick your interface. All MIDI inputs are enabled by default.
2. **Options → Plugin Manager** – `Options... → Scan for new or updated VST3 plug-ins` (and LV2/AU). The list is saved.
3. **Track menu** (Ctrl+T / Ctrl+M) adds audio or MIDI tracks.
4. Track header: **M**ute **S**olo **R**ecord-arm **I**nput-monitor (through the track FX), volume, pan, FX chain, input select.
   On a MIDI track the first plugin is the instrument; anything after it is FX.
5. **Recording**: arm tracks, hit **R** (or Rec). Audio goes to `~/Music/Shed/Recordings` as 24-bit WAV and is shifted by
   the device's reported latency. MIDI tracks record note on/off from every enabled MIDI input into a new clip.
6. **MIDI editing**: double-click an empty MIDI lane to make a 4-bar clip, double-click a clip to open the piano roll.
   Click to add, drag to move/resize, right-click or double-click to delete, Alt-drag for velocity, arrows to nudge
   (Shift = octave). Grid + quantize + clip length on the toolbar. Clicking notes/keys previews through the instrument.
7. **Arranging**: drag clips (also between tracks of the same type), drag the right edge to trim, S splits at the playhead,
   Del deletes, drop audio files onto lanes to import. Ctrl+wheel zooms, Shift+wheel scrolls. Shift while dragging = no snap.
8. **File → Export** (Ctrl+E): master mix as WAV and/or MP3, plus stems (one file per track, post-FX/post-fader, pan applied).
   Rendering is offline (faster than realtime, plugins put in non-realtime mode) from 0 to the last clip + 2 s tail.
   MP3 uses the `lame` binary; 320 kbps CBR by default.

Projects save as `.shed` (XML) with plugin state embedded. Audio is referenced by absolute path, not copied.

## Code map

| File | What |
|---|---|
| `Engine.*` | Data model, audio callback, mixer, plugin hosting, recording, offline render, save/load |
| `ArrangeView.*` | Track headers, ruler, clip lanes, drag/drop |
| `PianoRoll.*` | MIDI editor window |
| `MainComponent.*` | Transport bar, menus, dialogs, export job, editor windows |

Threading model is deliberately dumb: one `CriticalSection`. The audio thread *try*-locks it and outputs silence for that
block if the GUI is mid-edit, so it never blocks.

## Known limits (aka the to-do list)
- No VST2 (Steinberg pulled the SDK) — VST3, LV2 (Linux), AU (macOS) only.
- No undo, automation, sends/buses, master FX, plugin delay compensation, or tempo/time-signature changes (4/4 fixed).
- Plugins are scanned in-process; a crashing plugin will take Shed with it (the scanner blacklists it next time).
- Audio clips are loaded fully into RAM.
- Notes already sounding when you start playback mid-note don't retrigger.

## Prebuilt binaries (GitHub Actions)

`.github/workflows/build.yml` builds macOS (universal arm64+x86_64), Windows x64 and Linux on every push to `main`;
grab them from the run's **Artifacts**. Push a tag like `v0.1.0` and it also attaches them to a GitHub Release.

- **macOS**: the app is ad-hoc signed, not notarized. First launch: right-click → Open, or
  `xattr -dr com.apple.quarantine Shed.app`. For MP3 export: `brew install lame`.
- **Windows**: static runtime, no installer — just run `Shed.exe`. SmartScreen will complain ("More info → Run anyway").
  For MP3 export put `lame.exe` next to it.

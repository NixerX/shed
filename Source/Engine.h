#pragma once
#include <JuceHeader.h>
#include <bitset>

//==============================================================================
// Data model
//==============================================================================
struct MidiNote
{
    int note = 60;
    int velocity = 100;
    double start = 0.0;   // beats, relative to clip start
    double length = 1.0;  // beats
};

struct MidiClip
{
    juce::String name { "MIDI" };
    double start = 0.0;   // beats on timeline
    double length = 16.0; // beats
    std::vector<MidiNote> notes;
};

struct AudioClip
{
    juce::File file;
    double start = 0.0;        // seconds on timeline
    double offset = 0.0;       // seconds into the source
    double length = 0.0;       // seconds of source played
    double sourceLength = 0.0; // seconds
    std::shared_ptr<juce::AudioBuffer<float>> data; // resampled to engine rate
    std::vector<float> peaks;                       // abs peak per peakBlock samples
    static constexpr int peakBlock = 256;
};

enum class TrackType { Audio, Midi };

struct PluginSlot
{
    std::unique_ptr<juce::AudioPluginInstance> instance;
    bool bypassed = false;
    juce::PluginDescription desc; // as scanned (the instance's own description can point inside the bundle)
};

struct RecordedMidiEvent
{
    juce::int64 pos;
    juce::uint8 b0, b1, b2;
};

struct Track
{
    juce::String name;
    TrackType type = TrackType::Audio;
    juce::Colour colour;
    float volumeDb = 0.0f;
    float pan = 0.0f;          // -1..1
    bool mute = false, solo = false, armed = false, monitor = false;
    int inputChannel = -1;     // -1 = stereo 1/2, otherwise mono channel index

    juce::OwnedArray<AudioClip> audioClips;
    juce::OwnedArray<MidiClip> midiClips;
    std::vector<PluginSlot> plugins;

    // --- runtime (engine-owned) ---
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
    std::atomic<bool> needNotesOff { false };
    std::bitset<128> activeNotes;
    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> recWriter;
    juce::File recFile;
    std::vector<RecordedMidiEvent> recEvents;
};

//==============================================================================
class Engine : public juce::AudioIODeviceCallback,
               public juce::AudioPlayHead,
               public juce::ChangeBroadcaster
{
public:
    Engine();
    ~Engine() override;

    void initialise (juce::PropertiesFile* props);
    void shutdown();

    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }
    juce::AudioPluginFormatManager& getFormatManager() { return formatManager; }
    juce::KnownPluginList& getKnownPlugins() { return knownPlugins; }
    juce::AudioFormatManager& getAudioFormats() { return audioFormats; }
    juce::CriticalSection& getLock() { return lock; }

    // ---- tracks ----
    juce::OwnedArray<Track>& getTracks() { return tracks; }
    Track* addTrack (TrackType type, const juce::String& name = {});
    void removeTrack (int index);
    void clearProject();
    int indexOf (const Track* t) const;
    Track* findTrackForMidiClip (const MidiClip* c, int* indexOut = nullptr) const;

    // ---- clips ----
    AudioClip* addAudioClip (Track& t, const juce::File& file, double startSeconds, juce::String& error);
    MidiClip* addMidiClip (Track& t, double startBeat, double lengthBeats);
    void deleteAudioClip (Track& t, AudioClip* c);
    void deleteMidiClip (Track& t, MidiClip* c);
    void splitClipsAt (Track& t, double seconds);

    // ---- plugins ----
    juce::AudioPluginInstance* addPlugin (Track& t, const juce::PluginDescription& desc, juce::String& error);
    std::unique_ptr<juce::AudioPluginInstance> takePlugin (Track& t, int slot); // caller destroys (after closing UI)

    // ---- transport ----
    void play();
    void stop();
    void toggleRecord();
    bool isPlaying() const { return playing.load(); }
    bool isRecording() const { return recording.load(); }
    juce::int64 getPlayPosition() const { return position.load(); }
    void setPosition (juce::int64 samples);
    juce::int64 getRecordStart() const { return recordStart; }

    double getSampleRate() const { return sampleRate; }
    double getBpm() const { return bpm.load(); }
    void setBpm (double b) { bpm = juce::jlimit (20.0, 400.0, b); }
    double samplesPerBeat() const { return sampleRate * 60.0 / bpm.load(); }
    double secondsToBeats (double s) const { return s * bpm.load() / 60.0; }
    double beatsToSeconds (double b) const { return b * 60.0 / bpm.load(); }

    std::atomic<bool> metronomeOn { false };
    std::atomic<float> masterGainDb { 0.0f };
    std::atomic<float> masterPeakL { 0.0f }, masterPeakR { 0.0f };

    double getProjectEndSeconds() const;
    juce::File getRecordingsFolder() const;

    // Live note preview from the piano roll
    void previewNote (int trackIndex, int note, int velocity, bool on);

    // ---- offline render ----
    struct RenderTarget
    {
        juce::File file;
        bool mp3 = false;
    };
    // Renders the whole project. onlyTrack = -1 for full mix, otherwise a single track (stem).
    bool render (const std::vector<RenderTarget>& targets, int onlyTrack, int mp3QualityIndex,
                 const juce::File& lameExe, std::function<bool (double)> progress, juce::String& error);

    // ---- persistence ----
    std::unique_ptr<juce::XmlElement> saveProject() const;
    bool loadProject (const juce::XmlElement& xml, juce::StringArray& problems);

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numIn,
                                           float* const* outputs, int numOut, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    // AudioPlayHead
    juce::Optional<PositionInfo> getPosition() const override;

private:
    void renderTracks (int numSamples, juce::int64 pos, bool play,
                       const float* const* inputs, int numIn,
                       const juce::MidiBuffer* live, const juce::MidiBuffer* preview,
                       int onlyTrack, bool realtime);
    void prepareTrack (Track& t);
    void prepareAll();
    void loadClipData (AudioClip& c, juce::String& error);
    void finishRecording();
    void flagAllNotesOff();

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::AudioFormatManager audioFormats;
    juce::TimeSliceThread writerThread { "Shed recorder" };

    juce::CriticalSection lock;
    juce::OwnedArray<Track> tracks;

    double sampleRate = 44100.0;
    int blockSize = 512;
    std::atomic<double> bpm { 120.0 };
    std::atomic<bool> playing { false }, recording { false }, offline { false };
    std::atomic<juce::int64> position { 0 };
    juce::int64 recordStart = 0;
    int recordLatency = 0;

    // playhead info for plugins during a render pass
    juce::int64 renderPos = 0;
    bool renderPlaying = false;

    juce::MidiMessageCollector midiInput, previewInput;
    std::atomic<int> previewTrack { -1 };
    juce::MidiBuffer liveMidi, previewMidi;

    juce::AudioBuffer<float> master;
    std::vector<float> zeros;
    double clickPhase = 0.0;
    int clickRemaining = 0;
    float clickFreq = 1000.0f;

    int colourIndex = 0;
    bool isShutdown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};

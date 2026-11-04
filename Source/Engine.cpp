#include "Engine.h"

using namespace juce;

namespace
{
    const Colour trackPalette[] = {
        Colour (0xff5b9bd5), Colour (0xffed7d31), Colour (0xff70ad47), Colour (0xffffc000),
        Colour (0xff9e72c3), Colour (0xff4bc4c4), Colour (0xffe06c75), Colour (0xffa9a9a9)
    };

    constexpr int maxBlock = 8192;
}

Engine::Engine()
{
    midiInput.reset (sampleRate);
    previewInput.reset (sampleRate);
    master.setSize (2, maxBlock);
    zeros.assign ((size_t) maxBlock, 0.0f);
    liveMidi.ensureSize (4096);
    previewMidi.ensureSize (4096);
}

Engine::~Engine()
{
    shutdown();
}

void Engine::initialise (PropertiesFile* props)
{
    formatManager.addDefaultFormats();
    audioFormats.registerBasicFormats();

    if (props != nullptr)
        if (auto xml = props->getXmlValue ("pluginList"))
            knownPlugins.recreateFromXml (*xml);

    std::unique_ptr<XmlElement> savedDevice;
    if (props != nullptr)
        savedDevice = props->getXmlValue ("audioDevice");

    auto err = deviceManager.initialise (8, 2, savedDevice.get(), true);
    if (err.isNotEmpty())
    {
        DBG ("Audio device error: " << err);
    }

    for (auto& d : MidiInput::getAvailableDevices())
        deviceManager.setMidiInputDeviceEnabled (d.identifier, true);

    deviceManager.addMidiInputDeviceCallback ({}, &midiInput);
    writerThread.startThread();
    deviceManager.addAudioCallback (this);
}

void Engine::shutdown()
{
    if (isShutdown) return;
    isShutdown = true;

    if (recording)
        finishRecording();

    deviceManager.removeAudioCallback (this);
    deviceManager.removeMidiInputDeviceCallback ({}, &midiInput);
    deviceManager.closeAudioDevice();

    OwnedArray<Track> old;
    {
        const ScopedLock sl (lock);
        old.swapWith (tracks);
    }
    old.clear();
    writerThread.stopThread (2000);
}

//==============================================================================
void Engine::prepareTrack (Track& t)
{
    int maxCh = 2;
    for (auto& slot : t.plugins)
    {
        if (slot.instance == nullptr) continue;
        slot.instance->setPlayHead (this);
        slot.instance->prepareToPlay (sampleRate, blockSize);
        maxCh = jmax (maxCh, slot.instance->getTotalNumInputChannels(), slot.instance->getTotalNumOutputChannels());
    }
    t.scratch.setSize (jmin (maxCh, 32), maxBlock, false, true, true);
    t.midi.ensureSize (8192);
    if (t.recEvents.capacity() < 65536)
        t.recEvents.reserve (65536);
}

void Engine::prepareAll()
{
    for (auto* t : tracks)
        prepareTrack (*t);
}

void Engine::audioDeviceAboutToStart (AudioIODevice* device)
{
    const ScopedLock sl (lock);
    const double newRate = device->getCurrentSampleRate();
    const bool rateChanged = std::abs (newRate - sampleRate) > 0.5;
    sampleRate = newRate;
    blockSize = jlimit (16, maxBlock, device->getCurrentBufferSizeSamples());
    recordLatency = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();
    midiInput.reset (sampleRate);
    previewInput.reset (sampleRate);

    if (rateChanged)
    {
        for (auto* t : tracks)
            for (auto* c : t->audioClips)
            {
                String err;
                loadClipData (*c, err);
            }
    }

    prepareAll();
}

void Engine::audioDeviceStopped()
{
    const ScopedLock sl (lock);
    for (auto* t : tracks)
        for (auto& slot : t->plugins)
            if (slot.instance != nullptr)
                slot.instance->releaseResources();
}

//==============================================================================
Track* Engine::addTrack (TrackType type, const String& name)
{
    auto t = std::make_unique<Track>();
    t->type = type;
    int count = 1;
    for (auto* other : tracks)
        if (other->type == type) ++count;
    t->name = name.isNotEmpty() ? name : (type == TrackType::Audio ? "Audio " : "MIDI ") + String (count);
    t->colour = trackPalette[colourIndex++ % numElementsInArray (trackPalette)];
    prepareTrack (*t);

    auto* raw = t.get();
    {
        const ScopedLock sl (lock);
        tracks.add (t.release());
    }
    sendChangeMessage();
    return raw;
}

void Engine::removeTrack (int index)
{
    std::unique_ptr<Track> doomed;
    {
        const ScopedLock sl (lock);
        if (! isPositiveAndBelow (index, tracks.size())) return;
        doomed.reset (tracks.removeAndReturn (index));
    }
    doomed.reset();
    sendChangeMessage();
}

void Engine::clearProject()
{
    stop();
    OwnedArray<Track> old;
    {
        const ScopedLock sl (lock);
        old.swapWith (tracks);
    }
    old.clear();
    position = 0;
    bpm = 120.0;
    masterGainDb = 0.0f;
    colourIndex = 0;
    sendChangeMessage();
}

int Engine::indexOf (const Track* t) const
{
    for (int i = 0; i < tracks.size(); ++i)
        if (tracks[i] == t) return i;
    return -1;
}

Track* Engine::findTrackForMidiClip (const MidiClip* c, int* indexOut) const
{
    for (int i = 0; i < tracks.size(); ++i)
        if (tracks[i]->midiClips.contains (c))
        {
            if (indexOut != nullptr) *indexOut = i;
            return tracks[i];
        }
    return nullptr;
}

//==============================================================================
void Engine::loadClipData (AudioClip& c, String& error)
{
    std::unique_ptr<AudioFormatReader> reader (audioFormats.createReaderFor (c.file));
    if (reader == nullptr)
    {
        error = "Couldn't read " + c.file.getFileName();
        return;
    }

    if (reader->lengthInSamples > (int64) std::numeric_limits<int>::max() / 2)
    {
        error = c.file.getFileName() + " is too long";
        return;
    }

    const int len = (int) reader->lengthInSamples;
    const int numCh = (int) jmin (2u, reader->numChannels);
    AudioBuffer<float> src (numCh, len + 16);
    src.clear();
    reader->read (&src, 0, len, 0, true, numCh > 1);

    std::shared_ptr<AudioBuffer<float>> data;
    const double ratio = reader->sampleRate / sampleRate;

    if (std::abs (ratio - 1.0) > 1.0e-6)
    {
        const int outLen = (int) std::floor (len / ratio);
        data = std::make_shared<AudioBuffer<float>> (numCh, outLen);
        for (int ch = 0; ch < numCh; ++ch)
        {
            LagrangeInterpolator interp;
            interp.process (ratio, src.getReadPointer (ch), data->getWritePointer (ch), outLen);
        }
    }
    else
    {
        data = std::make_shared<AudioBuffer<float>> (numCh, len);
        for (int ch = 0; ch < numCh; ++ch)
            data->copyFrom (ch, 0, src, ch, 0, len);
    }

    std::vector<float> peaks ((size_t) (data->getNumSamples() / AudioClip::peakBlock + 1), 0.0f);
    for (size_t b = 0; b < peaks.size(); ++b)
    {
        const int s0 = (int) b * AudioClip::peakBlock;
        const int count = jmin (AudioClip::peakBlock, data->getNumSamples() - s0);
        if (count <= 0) break;
        float m = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            m = jmax (m, data->getMagnitude (ch, s0, count));
        peaks[b] = m;
    }

    const ScopedLock sl (lock);
    c.data = std::move (data);
    c.peaks = std::move (peaks);
    c.sourceLength = c.data->getNumSamples() / sampleRate;
}

AudioClip* Engine::addAudioClip (Track& t, const File& file, double startSeconds, String& error)
{
    auto c = std::make_unique<AudioClip>();
    c->file = file;
    c->start = jmax (0.0, startSeconds);
    loadClipData (*c, error);
    if (c->data == nullptr)
        return nullptr;

    c->length = c->sourceLength;
    auto* raw = c.get();
    {
        const ScopedLock sl (lock);
        t.audioClips.add (c.release());
    }
    sendChangeMessage();
    return raw;
}

MidiClip* Engine::addMidiClip (Track& t, double startBeat, double lengthBeats)
{
    auto c = std::make_unique<MidiClip>();
    c->start = jmax (0.0, startBeat);
    c->length = jmax (0.25, lengthBeats);
    c->name = t.name;
    auto* raw = c.get();
    {
        const ScopedLock sl (lock);
        t.midiClips.add (c.release());
    }
    sendChangeMessage();
    return raw;
}

void Engine::deleteAudioClip (Track& t, AudioClip* c)
{
    {
        const ScopedLock sl (lock);
        t.audioClips.removeObject (c);
    }
    sendChangeMessage();
}

void Engine::deleteMidiClip (Track& t, MidiClip* c)
{
    {
        const ScopedLock sl (lock);
        t.midiClips.removeObject (c);
        t.needNotesOff = true;
    }
    sendChangeMessage();
}

void Engine::splitClipsAt (Track& t, double seconds)
{
    const ScopedLock sl (lock);

    for (int i = t.audioClips.size(); --i >= 0;)
    {
        auto* c = t.audioClips[i];
        const double rel = seconds - c->start;
        if (rel <= 0.001 || rel >= c->length - 0.001) continue;

        auto right = std::make_unique<AudioClip> (*c);
        right->start = seconds;
        right->offset = c->offset + rel;
        right->length = c->length - rel;
        c->length = rel;
        t.audioClips.add (right.release());
    }

    const double beat = secondsToBeats (seconds);
    for (int i = t.midiClips.size(); --i >= 0;)
    {
        auto* c = t.midiClips[i];
        const double rel = beat - c->start;
        if (rel <= 0.001 || rel >= c->length - 0.001) continue;

        auto right = std::make_unique<MidiClip>();
        right->name = c->name;
        right->start = beat;
        right->length = c->length - rel;

        std::vector<MidiNote> left;
        for (auto n : c->notes)
        {
            if (n.start >= rel) { n.start -= rel; right->notes.push_back (n); }
            else left.push_back (n);
        }
        c->notes = std::move (left);
        c->length = rel;
        t.midiClips.add (right.release());
    }

    t.needNotesOff = true;
    sendChangeMessage();
}

//==============================================================================
AudioPluginInstance* Engine::addPlugin (Track& t, const PluginDescription& desc, String& error)
{
    auto inst = formatManager.createPluginInstance (desc, sampleRate, blockSize, error);
    if (inst == nullptr)
    {
        if (error.isEmpty()) error = "Couldn't load " + desc.name;
        return nullptr;
    }

    auto layout = inst->getBusesLayout();
    if (! layout.inputBuses.isEmpty())  layout.inputBuses.getReference (0)  = AudioChannelSet::stereo();
    if (! layout.outputBuses.isEmpty()) layout.outputBuses.getReference (0) = AudioChannelSet::stereo();
    inst->setBusesLayout (layout); // fine if the plugin refuses; we adapt to whatever it has

    inst->setPlayHead (this);
    inst->prepareToPlay (sampleRate, blockSize);
    auto* raw = inst.get();

    const int need = jmin (32, jmax (2, inst->getTotalNumInputChannels(), inst->getTotalNumOutputChannels()));
    {
        const ScopedLock sl (lock);
        if (t.scratch.getNumChannels() < need)
            t.scratch.setSize (need, maxBlock, false, true, true);
        t.plugins.push_back ({ std::move (inst), false, desc });
    }
    sendChangeMessage();
    return raw;
}

std::unique_ptr<AudioPluginInstance> Engine::takePlugin (Track& t, int slot)
{
    std::unique_ptr<AudioPluginInstance> out;
    {
        const ScopedLock sl (lock);
        if (! isPositiveAndBelow (slot, (int) t.plugins.size())) return {};
        out = std::move (t.plugins[(size_t) slot].instance);
        t.plugins.erase (t.plugins.begin() + slot);
        t.needNotesOff = true;
    }
    if (out != nullptr)
        out->releaseResources();
    sendChangeMessage();
    return out;
}

//==============================================================================
void Engine::play()
{
    playing = true;
    sendChangeMessage();
}

void Engine::stop()
{
    if (recording)
        finishRecording();
    playing = false;
    flagAllNotesOff();
    sendChangeMessage();
}

void Engine::flagAllNotesOff()
{
    for (auto* t : tracks)
        t->needNotesOff = true;
}

void Engine::setPosition (int64 samples)
{
    if (recording) return;
    position = jmax ((int64) 0, samples);
    flagAllNotesOff();
    sendChangeMessage();
}

File Engine::getRecordingsFolder() const
{
    auto f = File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Shed").getChildFile ("Recordings");
    f.createDirectory();
    return f;
}

void Engine::toggleRecord()
{
    if (recording)
    {
        stop();
        return;
    }

    const auto stamp = Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S");
    const ScopedLock sl (lock);
    recordStart = position.load();

    for (auto* t : tracks)
    {
        if (! t->armed) continue;

        if (t->type == TrackType::Midi)
        {
            t->recEvents.clear();
            continue;
        }

        const int numCh = t->inputChannel < 0 ? 2 : 1;
        t->recFile = getRecordingsFolder().getNonexistentChildFile (File::createLegalFileName (t->name) + "_" + stamp, ".wav");
        auto stream = std::make_unique<FileOutputStream> (t->recFile);
        if (! stream->openedOk()) continue;

        WavAudioFormat wav;
        if (auto* w = wav.createWriterFor (stream.get(), sampleRate, (unsigned int) numCh, 24, {}, 0))
        {
            stream.release();
            t->recWriter = std::make_unique<AudioFormatWriter::ThreadedWriter> (w, writerThread, 65536);
        }
    }

    recording = true;
    playing = true;
    sendChangeMessage();
}

void Engine::finishRecording()
{
    struct Pending
    {
        Track* track;
        std::unique_ptr<AudioFormatWriter::ThreadedWriter> writer;
        File file;
        std::vector<RecordedMidiEvent> events;
        bool isMidi;
    };
    std::vector<Pending> pending;
    int64 endPos = 0;

    {
        const ScopedLock sl (lock);
        recording = false;
        endPos = position.load();
        for (auto* t : tracks)
        {
            if (t->recWriter != nullptr)
                pending.push_back ({ t, std::move (t->recWriter), t->recFile, {}, false });
            else if (t->armed && t->type == TrackType::Midi && ! t->recEvents.empty())
            {
                Pending p { t, nullptr, {}, {}, true };
                p.events = t->recEvents;
                t->recEvents.clear();
                pending.push_back (std::move (p));
            }
        }
    }

    const double startSec = recordStart / sampleRate;
    const double latencySec = recordLatency / sampleRate;

    for (auto& p : pending)
    {
        if (indexOf (p.track) < 0) continue;

        if (! p.isMidi)
        {
            p.writer.reset(); // flushes the file
            String err;
            if (auto* c = addAudioClip (*p.track, p.file, startSec, err))
            {
                const ScopedLock sl (lock);
                c->offset = jmin (latencySec, c->sourceLength);
                c->length = jmax (0.0, c->sourceLength - c->offset);
            }
            continue;
        }

        // Turn recorded note on/offs into a clip
        const double spb = samplesPerBeat();
        const double clipStart = std::floor ((recordStart / spb) / 4.0) * 4.0;
        const double endBeat = endPos / spb - clipStart;
        double onAt[128];
        int onVel[128];
        for (int i = 0; i < 128; ++i) { onAt[i] = -1.0; onVel[i] = 0; }

        std::vector<MidiNote> notes;
        auto close = [&] (int n, double at)
        {
            if (onAt[n] < 0) return;
            notes.push_back ({ n, onVel[n], onAt[n], jmax (1.0 / 64.0, at - onAt[n]) });
            onAt[n] = -1.0;
        };

        for (auto& e : p.events)
        {
            const double beat = e.pos / spb - clipStart;
            const int status = e.b0 & 0xf0;
            const int n = e.b1 & 0x7f;
            if (status == 0x90 && e.b2 > 0)
            {
                close (n, beat);
                onAt[n] = beat;
                onVel[n] = e.b2;
            }
            else if (status == 0x80 || status == 0x90)
                close (n, beat);
        }
        for (int n = 0; n < 128; ++n)
            close (n, endBeat);

        if (notes.empty()) continue;

        auto* clip = addMidiClip (*p.track, clipStart, jmax (4.0, std::ceil (endBeat / 4.0) * 4.0));
        const ScopedLock sl (lock);
        clip->notes = std::move (notes);
    }

    sendChangeMessage();
}

void Engine::previewNote (int trackIndex, int note, int velocity, bool on)
{
    previewTrack = trackIndex;
    auto m = on ? MidiMessage::noteOn (1, note, (uint8) jlimit (1, 127, velocity))
                : MidiMessage::noteOff (1, note);
    m.setTimeStamp (Time::getMillisecondCounterHiRes() * 0.001);
    previewInput.addMessageToQueue (m);
}

double Engine::getProjectEndSeconds() const
{
    double end = 0.0;
    for (auto* t : tracks)
    {
        for (auto* c : t->audioClips) end = jmax (end, c->start + c->length);
        for (auto* c : t->midiClips)  end = jmax (end, beatsToSeconds (c->start + c->length));
    }
    return end;
}

//==============================================================================
Optional<AudioPlayHead::PositionInfo> Engine::getPosition() const
{
    PositionInfo info;
    const double b = bpm.load();
    const double secs = renderPos / sampleRate;
    const double ppq = secs * b / 60.0;
    info.setBpm (b);
    info.setTimeSignature (TimeSignature { 4, 4 });
    info.setTimeInSamples (renderPos);
    info.setTimeInSeconds (secs);
    info.setPpqPosition (ppq);
    info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
    info.setBarCount ((int64) std::floor (ppq / 4.0));
    info.setIsPlaying (renderPlaying);
    info.setIsRecording (recording.load());
    return info;
}

//==============================================================================
void Engine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numIn,
                                               float* const* outputs, int numOut, int numSamples,
                                               const AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOut; ++ch)
        if (outputs[ch] != nullptr)
            FloatVectorOperations::clear (outputs[ch], numSamples);

    liveMidi.clear();
    previewMidi.clear();
    midiInput.removeNextBlockOfMessages (liveMidi, numSamples);
    previewInput.removeNextBlockOfMessages (previewMidi, numSamples);

    if (offline) return;
    const ScopedTryLock sl (lock);
    if (! sl.isLocked()) return;

    const bool play = playing.load();
    int64 pos = position.load();
    int done = 0;

    while (done < numSamples)
    {
        const int n = jmin (blockSize, numSamples - done);

        // shift MIDI for this chunk (normally there's only one chunk)
        MidiBuffer* live = &liveMidi;
        MidiBuffer* prev = &previewMidi;
        MidiBuffer liveChunk, prevChunk;
        if (n != numSamples)
        {
            for (auto m : liveMidi)
                if (m.samplePosition >= done && m.samplePosition < done + n)
                    liveChunk.addEvent (m.data, m.numBytes, m.samplePosition - done);
            for (auto m : previewMidi)
                if (m.samplePosition >= done && m.samplePosition < done + n)
                    prevChunk.addEvent (m.data, m.numBytes, m.samplePosition - done);
            live = &liveChunk;
            prev = &prevChunk;
        }

        const float* in[64];
        const int inCount = jmin (numIn, 64);
        for (int ch = 0; ch < inCount; ++ch)
            in[ch] = inputs[ch] != nullptr ? inputs[ch] + done : zeros.data();

        auto chan = [&] (int c) -> const float* { return c < inCount ? in[c] : zeros.data(); };

        if (play && recording)
        {
            for (auto* t : tracks)
            {
                if (! t->armed) continue;
                if (t->recWriter != nullptr)
                {
                    const float* ptrs[2];
                    if (t->inputChannel < 0) { ptrs[0] = chan (0); ptrs[1] = chan (1); }
                    else                       { ptrs[0] = chan (t->inputChannel); ptrs[1] = ptrs[0]; }
                    t->recWriter->write (ptrs, n);
                }
                else if (t->type == TrackType::Midi)
                {
                    for (auto m : *live)
                    {
                        if (m.numBytes < 2 || t->recEvents.size() >= t->recEvents.capacity()) continue;
                        t->recEvents.push_back ({ pos + m.samplePosition, m.data[0], m.data[1],
                                                  (uint8) (m.numBytes > 2 ? m.data[2] : 0) });
                    }
                }
            }
        }

        renderTracks (n, pos, play, in, inCount, live, prev, -1, true);

        if (numOut >= 2)
        {
            if (outputs[0] != nullptr) FloatVectorOperations::copy (outputs[0] + done, master.getReadPointer (0), n);
            if (outputs[1] != nullptr) FloatVectorOperations::copy (outputs[1] + done, master.getReadPointer (1), n);
        }
        else if (numOut == 1 && outputs[0] != nullptr)
        {
            FloatVectorOperations::copyWithMultiply (outputs[0] + done, master.getReadPointer (0), 0.5f, n);
            FloatVectorOperations::addWithMultiply (outputs[0] + done, master.getReadPointer (1), 0.5f, n);
        }

        if (play) pos += n;
        done += n;
    }

    if (play)
        position = pos;
}

void Engine::renderTracks (int n, int64 pos, bool play, const float* const* inputs, int numIn,
                           const MidiBuffer* live, const MidiBuffer* preview, int onlyTrack, bool realtime)
{
    renderPos = pos;
    renderPlaying = play;
    master.clear (0, n);

    bool anySolo = false;
    for (auto* t : tracks) anySolo = anySolo || t->solo;

    const double spb = samplesPerBeat();
    const int prevTrack = previewTrack.load();
    const int64 end = pos + n;

    for (int i = 0; i < tracks.size(); ++i)
    {
        if (onlyTrack >= 0 && i != onlyTrack) continue;
        auto& t = *tracks[i];
        auto& buf = t.scratch;
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            buf.clear (ch, 0, n);

        auto& midi = t.midi;
        midi.clear();

        if (t.needNotesOff.exchange (false))
        {
            for (int nn = 0; nn < 128; ++nn)
                if (t.activeNotes[(size_t) nn])
                    midi.addEvent (MidiMessage::noteOff (1, nn), 0);
            midi.addEvent (MidiMessage::controllerEvent (1, 64, 0), 0);
            midi.addEvent (MidiMessage::allNotesOff (1), 0);
            t.activeNotes.reset();
        }

        if (play)
        {
            if (t.type == TrackType::Audio)
            {
                for (auto* c : t.audioClips)
                {
                    if (c->data == nullptr) continue;
                    const int64 cs = (int64) std::llround (c->start * sampleRate);
                    const int64 cl = (int64) std::llround (c->length * sampleRate);
                    const int64 co = (int64) std::llround (c->offset * sampleRate);
                    const int64 s0 = jmax (pos, cs), s1 = jmin (end, cs + cl);
                    if (s1 <= s0) continue;

                    int64 src = co + (s0 - cs);
                    int count = (int) (s1 - s0);
                    const int avail = c->data->getNumSamples();
                    if (src >= avail) continue;
                    count = (int) jmin ((int64) count, avail - src);
                    const int dst = (int) (s0 - pos);
                    const int srcCh = c->data->getNumChannels();
                    for (int ch = 0; ch < 2; ++ch)
                        buf.addFrom (ch, dst, *c->data, jmin (ch, srcCh - 1), (int) src, count);
                }
            }
            else
            {
                // offs first so a re-triggered note at the same sample works
                for (int pass = 0; pass < 2; ++pass)
                {
                    for (auto* c : t.midiClips)
                    {
                        const double clipStartS = c->start * spb;
                        const double clipEndS = (c->start + c->length) * spb;
                        if (clipEndS < pos || clipStartS >= end + 1) continue;

                        for (auto& nt : c->notes)
                        {
                            if (nt.start >= c->length) continue;
                            if (pass == 1)
                            {
                                const int64 on = (int64) std::llround (clipStartS + nt.start * spb);
                                if (on >= pos && on < end)
                                    midi.addEvent (MidiMessage::noteOn (1, nt.note, (uint8) jlimit (1, 127, nt.velocity)), (int) (on - pos));
                            }
                            else
                            {
                                const double offBeat = jmin (nt.start + nt.length, c->length);
                                int64 off = (int64) std::llround (clipStartS + offBeat * spb);
                                if (off >= pos && off < end)
                                    midi.addEvent (MidiMessage::noteOff (1, nt.note), (int) (off - pos));
                            }
                        }
                    }
                }
            }
        }

        if (realtime && t.armed)
        {
            if (t.type == TrackType::Audio && t.monitor && inputs != nullptr && numIn > 0)
            {
                if (t.inputChannel < 0)
                {
                    buf.addFrom (0, 0, inputs[0], n);
                    buf.addFrom (1, 0, numIn > 1 ? inputs[1] : inputs[0], n);
                }
                else if (t.inputChannel < numIn)
                {
                    buf.addFrom (0, 0, inputs[t.inputChannel], n);
                    buf.addFrom (1, 0, inputs[t.inputChannel], n);
                }
            }
            else if (t.type == TrackType::Midi && live != nullptr)
                midi.addEvents (*live, 0, n, 0);
        }

        if (realtime && preview != nullptr && i == prevTrack)
            midi.addEvents (*preview, 0, n, 0);

        for (auto m : midi)
        {
            auto msg = m.getMessage();
            if (msg.isNoteOn())       t.activeNotes.set ((size_t) msg.getNoteNumber());
            else if (msg.isNoteOff()) t.activeNotes.reset ((size_t) msg.getNoteNumber());
        }

        for (auto& slot : t.plugins)
        {
            if (slot.bypassed || slot.instance == nullptr) continue;
            auto* p = slot.instance.get();
            const int need = jlimit (1, buf.getNumChannels(),
                                     jmax (p->getTotalNumInputChannels(), p->getTotalNumOutputChannels()));
            for (int ch = 2; ch < need; ++ch)
                buf.clear (ch, 0, n);

            AudioBuffer<float> view (buf.getArrayOfWritePointers(), need, n);
            p->processBlock (view, midi);

            if (p->getTotalNumOutputChannels() == 1)
                buf.copyFrom (1, 0, buf, 0, 0, n);
        }

        const bool audible = onlyTrack >= 0 || (! t.mute && (! anySolo || t.solo));
        const float g = Decibels::decibelsToGain (t.volumeDb, -96.0f);
        const float gl = g * jmin (1.0f, 1.0f - t.pan);
        const float gr = g * jmin (1.0f, 1.0f + t.pan);

        if (realtime)
        {
            const float pl = buf.getMagnitude (0, 0, n) * gl;
            const float pr = buf.getMagnitude (1, 0, n) * gr;
            t.peakL = jmax (t.peakL.load(), audible ? pl : 0.0f);
            t.peakR = jmax (t.peakR.load(), audible ? pr : 0.0f);
        }

        if (audible)
        {
            master.addFrom (0, 0, buf, 0, 0, n, gl);
            master.addFrom (1, 0, buf, 1, 0, n, gr);
        }
    }

    // metronome
    if (realtime && play && metronomeOn)
    {
        const int clickLen = (int) (sampleRate * 0.03);
        auto writeClick = [&] (int from)
        {
            for (int s = from; s < n && clickRemaining > 0; ++s, --clickRemaining)
            {
                const float env = (float) clickRemaining / (float) clickLen;
                const float v = 0.35f * env * (float) std::sin (clickPhase);
                clickPhase += MathConstants<double>::twoPi * clickFreq / sampleRate;
                master.addSample (0, s, v);
                master.addSample (1, s, v);
            }
        };
        writeClick (0);
        for (int64 b = (int64) std::ceil (pos / spb); (double) b * spb < (double) end; ++b)
        {
            const int off = (int) (std::llround ((double) b * spb) - pos);
            if (off < 0 || off >= n) continue;
            clickRemaining = clickLen;
            clickPhase = 0.0;
            clickFreq = (b % 4 == 0) ? 1600.0f : 1000.0f;
            writeClick (off);
        }
    }
    else
    {
        clickRemaining = 0;
    }

    master.applyGain (0, n, Decibels::decibelsToGain (masterGainDb.load(), -96.0f));
    if (realtime)
    {
        masterPeakL = jmax (masterPeakL.load(), master.getMagnitude (0, 0, n));
        masterPeakR = jmax (masterPeakR.load(), master.getMagnitude (1, 0, n));
    }
}

//==============================================================================
bool Engine::render (const std::vector<RenderTarget>& targets, int onlyTrack, int mp3QualityIndex,
                     const File& lameExe, std::function<bool (double)> progress, String& error)
{
    std::vector<std::unique_ptr<AudioFormatWriter>> writers;

    for (auto& target : targets)
    {
        target.file.deleteFile();
        auto stream = target.file.createOutputStream();
        if (stream == nullptr)
        {
            error = "Can't write to " + target.file.getFullPathName();
            return false;
        }

        AudioFormatWriter* w = nullptr;
        if (target.mp3)
        {
            if (! lameExe.existsAsFile())
            {
                error = "MP3 export needs the 'lame' encoder. Install it (e.g. sudo apt install lame / brew install lame) "
                        "or set its location under Options > Set LAME encoder path.";
                return false;
            }
            LAMEEncoderAudioFormat fmt (lameExe);
            w = fmt.createWriterFor (stream.get(), sampleRate, 2, 16, {}, mp3QualityIndex);
        }
        else
        {
            WavAudioFormat fmt;
            w = fmt.createWriterFor (stream.get(), sampleRate, 2, 24, {}, 0);
        }

        if (w == nullptr)
        {
            error = "Couldn't create writer for " + target.file.getFileName();
            return false;
        }
        stream.release();
        writers.emplace_back (w);
    }

    bool cancelled = false;
    {
        offline = true;
        const ScopedLock sl (lock);

        for (auto* t : tracks)
        {
            for (auto& slot : t->plugins)
                if (slot.instance != nullptr)
                {
                    slot.instance->setNonRealtime (true);
                    slot.instance->reset();
                }
            t->needNotesOff = true;
        }

        const int64 total = (int64) ((getProjectEndSeconds() + 2.0) * sampleRate);
        int64 pos = 0;
        while (pos < total)
        {
            const int n = (int) jmin ((int64) blockSize, total - pos);
            renderTracks (n, pos, true, nullptr, 0, nullptr, nullptr, onlyTrack, false);
            for (auto& w : writers)
                w->writeFromAudioSampleBuffer (master, 0, n);
            pos += n;

            if (progress && ! progress ((double) pos / (double) total))
            {
                cancelled = true;
                break;
            }
        }

        for (auto* t : tracks)
        {
            for (auto& slot : t->plugins)
                if (slot.instance != nullptr)
                {
                    slot.instance->setNonRealtime (false);
                    slot.instance->reset();
                }
            t->needNotesOff = true;
        }
        offline = false;
    }

    writers.clear(); // closes files (and runs lame for MP3)

    if (cancelled)
    {
        for (auto& t : targets) t.file.deleteFile();
        error = "Export cancelled";
        return false;
    }
    return true;
}

//==============================================================================
std::unique_ptr<XmlElement> Engine::saveProject() const
{
    auto xml = std::make_unique<XmlElement> ("ShedProject");
    xml->setAttribute ("version", 1);
    xml->setAttribute ("bpm", bpm.load());
    xml->setAttribute ("master", (double) masterGainDb.load());
    xml->setAttribute ("metronome", metronomeOn.load());

    for (auto* t : tracks)
    {
        auto* te = xml->createNewChildElement ("Track");
        te->setAttribute ("name", t->name);
        te->setAttribute ("type", t->type == TrackType::Audio ? "audio" : "midi");
        te->setAttribute ("colour", t->colour.toString());
        te->setAttribute ("volume", (double) t->volumeDb);
        te->setAttribute ("pan", (double) t->pan);
        te->setAttribute ("mute", t->mute);
        te->setAttribute ("solo", t->solo);
        te->setAttribute ("monitor", t->monitor);
        te->setAttribute ("input", t->inputChannel);

        for (auto& slot : t->plugins)
        {
            if (slot.instance == nullptr) continue;
            auto* pe = te->createNewChildElement ("Plugin");
            pe->setAttribute ("bypass", slot.bypassed);
            pe->addChildElement (slot.desc.createXml().release());
            MemoryBlock mb;
            slot.instance->getStateInformation (mb);
            pe->createNewChildElement ("State")->addTextElement (mb.toBase64Encoding());
        }

        for (auto* c : t->audioClips)
        {
            auto* ce = te->createNewChildElement ("AudioClip");
            ce->setAttribute ("file", c->file.getFullPathName());
            ce->setAttribute ("start", c->start);
            ce->setAttribute ("offset", c->offset);
            ce->setAttribute ("length", c->length);
        }

        for (auto* c : t->midiClips)
        {
            auto* ce = te->createNewChildElement ("MidiClip");
            ce->setAttribute ("name", c->name);
            ce->setAttribute ("start", c->start);
            ce->setAttribute ("length", c->length);
            for (auto& n : c->notes)
            {
                auto* ne = ce->createNewChildElement ("N");
                ne->setAttribute ("n", n.note);
                ne->setAttribute ("v", n.velocity);
                ne->setAttribute ("s", n.start);
                ne->setAttribute ("l", n.length);
            }
        }
    }
    return xml;
}

bool Engine::loadProject (const XmlElement& xml, StringArray& problems)
{
    if (! xml.hasTagName ("ShedProject"))
    {
        problems.add ("Not a Shed project");
        return false;
    }

    clearProject();
    setBpm (xml.getDoubleAttribute ("bpm", 120.0));
    masterGainDb = (float) xml.getDoubleAttribute ("master", 0.0);
    metronomeOn = xml.getBoolAttribute ("metronome", false);

    for (auto* te : xml.getChildWithTagNameIterator ("Track"))
    {
        const auto type = te->getStringAttribute ("type") == "midi" ? TrackType::Midi : TrackType::Audio;
        auto* t = addTrack (type, te->getStringAttribute ("name"));
        {
            const ScopedLock sl (lock);
            t->colour = Colour::fromString (te->getStringAttribute ("colour", t->colour.toString()));
            t->volumeDb = (float) te->getDoubleAttribute ("volume", 0.0);
            t->pan = (float) te->getDoubleAttribute ("pan", 0.0);
            t->mute = te->getBoolAttribute ("mute");
            t->solo = te->getBoolAttribute ("solo");
            t->monitor = te->getBoolAttribute ("monitor");
            t->inputChannel = te->getIntAttribute ("input", -1);
        }

        for (auto* pe : te->getChildWithTagNameIterator ("Plugin"))
        {
            PluginDescription desc;
            auto* de = pe->getChildByName ("PLUGIN");
            if (de == nullptr || ! desc.loadFromXml (*de)) continue;

            String err;
            auto* inst = addPlugin (*t, desc, err);
            if (inst == nullptr)
            {
                problems.add (t->name + ": " + desc.name + " (" + err + ")");
                continue;
            }

            const ScopedLock sl (lock);
            if (auto* se = pe->getChildByName ("State"))
            {
                MemoryBlock mb;
                if (mb.fromBase64Encoding (se->getAllSubText()))
                    inst->setStateInformation (mb.getData(), (int) mb.getSize());
            }
            t->plugins.back().bypassed = pe->getBoolAttribute ("bypass");
        }

        for (auto* ce : te->getChildWithTagNameIterator ("AudioClip"))
        {
            String err;
            File f (ce->getStringAttribute ("file"));
            if (auto* c = addAudioClip (*t, f, ce->getDoubleAttribute ("start"), err))
            {
                const ScopedLock sl (lock);
                c->offset = jlimit (0.0, c->sourceLength, ce->getDoubleAttribute ("offset"));
                c->length = jlimit (0.0, c->sourceLength - c->offset, ce->getDoubleAttribute ("length", c->sourceLength));
            }
            else
                problems.add (err);
        }

        for (auto* ce : te->getChildWithTagNameIterator ("MidiClip"))
        {
            auto* c = addMidiClip (*t, ce->getDoubleAttribute ("start"), ce->getDoubleAttribute ("length", 16.0));
            const ScopedLock sl (lock);
            c->name = ce->getStringAttribute ("name", "MIDI");
            for (auto* ne : ce->getChildWithTagNameIterator ("N"))
                c->notes.push_back ({ ne->getIntAttribute ("n", 60), ne->getIntAttribute ("v", 100),
                                      ne->getDoubleAttribute ("s"), ne->getDoubleAttribute ("l", 1.0) });
        }
    }

    sendChangeMessage();
    return true;
}

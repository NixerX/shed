#include "ArrangeView.h"

using namespace juce;

static void noFocus (Component& c)
{
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

//==============================================================================
class TrackHeader : public Component
{
public:
    TrackHeader (ArrangeView& av, Engine& e, Track& tr) : view (av), engine (e), t (tr)
    {
        name.setText (t.name, dontSendNotification);
        name.setEditable (false, true, false);
        name.setFont (FontOptions (14.0f, Font::bold));
        name.onTextChange = [this] { t.name = name.getText(); };
        addAndMakeVisible (name);

        auto setupToggle = [this] (TextButton& b, const String& text, Colour on, bool& flag, const String& tip)
        {
            b.setButtonText (text);
            b.setClickingTogglesState (true);
            b.setToggleState (flag, dontSendNotification);
            b.setColour (TextButton::buttonOnColourId, on);
            b.setTooltip (tip);
            b.onClick = [this, &b, &flag] { flag = b.getToggleState(); engine.sendChangeMessage(); };
            noFocus (b);
            addAndMakeVisible (b);
        };
        setupToggle (muteB, "M", Colour (0xffd9534f), t.mute, "Mute");
        setupToggle (soloB, "S", Colour (0xffe0b020), t.solo, "Solo");
        setupToggle (armB, "R", Colour (0xffc0392b), t.armed, "Arm for recording");
        if (t.type == TrackType::Audio)
            setupToggle (monB, "I", Colour (0xff3a8fd9), t.monitor, "Input monitoring (through FX)");

        vol.setSliderStyle (Slider::LinearHorizontal);
        vol.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        vol.setRange (-60.0, 12.0, 0.1);
        vol.setSkewFactorFromMidPoint (-12.0);
        vol.setValue (t.volumeDb, dontSendNotification);
        vol.setDoubleClickReturnValue (true, 0.0);
        vol.setPopupDisplayEnabled (true, true, nullptr);
        vol.setTextValueSuffix (" dB");
        vol.onValueChange = [this] { t.volumeDb = (float) vol.getValue(); };
        noFocus (vol);
        addAndMakeVisible (vol);

        pan.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
        pan.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        pan.setRange (-1.0, 1.0, 0.01);
        pan.setValue (t.pan, dontSendNotification);
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.setPopupDisplayEnabled (true, true, nullptr);
        pan.onValueChange = [this] { t.pan = (float) pan.getValue(); };
        noFocus (pan);
        addAndMakeVisible (pan);

        fx.onClick = [this] { showFxMenu(); };
        noFocus (fx);
        addAndMakeVisible (fx);

        if (t.type == TrackType::Audio)
        {
            input.setTooltip ("Recording input");
            input.addItem ("In 1+2", 1);
            int numIns = 2;
            if (auto* dev = engine.getDeviceManager().getCurrentAudioDevice())
                numIns = jmax (2, dev->getActiveInputChannels().countNumberOfSetBits());
            for (int i = 0; i < numIns; ++i)
                input.addItem ("In " + String (i + 1), i + 2);
            input.setSelectedId (t.inputChannel < 0 ? 1 : t.inputChannel + 2, dontSendNotification);
            input.onChange = [this] { t.inputChannel = input.getSelectedId() == 1 ? -1 : input.getSelectedId() - 2; };
            noFocus (input);
            addAndMakeVisible (input);
        }

        refresh();
    }

    void refresh()
    {
        int n = (int) t.plugins.size();
        if (t.type == TrackType::Midi && n > 0 && t.plugins[0].instance != nullptr)
            fx.setButtonText (t.plugins[0].instance->getName() + (n > 1 ? " +" + String (n - 1) : String()));
        else
            fx.setButtonText (n == 0 ? (t.type == TrackType::Midi ? "+ Instrument" : "FX") : "FX (" + String (n) + ")");
        muteB.setToggleState (t.mute, dontSendNotification);
        soloB.setToggleState (t.solo, dontSendNotification);
        armB.setToggleState (t.armed, dontSendNotification);
        monB.setToggleState (t.monitor, dontSendNotification);
        name.setText (t.name, dontSendNotification);
        repaint();
    }

    void updateMeter()
    {
        const float l = t.peakL.exchange (0.0f), r = t.peakR.exchange (0.0f);
        meterL = jmax (l, meterL * 0.85f);
        meterR = jmax (r, meterR * 0.85f);
        repaint (getWidth() - 14, 0, 14, getHeight());
    }

    void paint (Graphics& g) override
    {
        const bool sel = view.getSelectedTrack() == engine.indexOf (&t);
        g.fillAll (sel ? Colour (0xff45454d) : Colour (0xff36363c));
        g.setColour (t.colour);
        g.fillRect (0, 0, 5, getHeight());
        g.setColour (Colours::black.withAlpha (0.5f));
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

        g.setColour (Colours::grey.withAlpha (0.7f));
        g.setFont (11.0f);
        g.drawText (t.type == TrackType::Audio ? "AUDIO" : "MIDI", 10, getHeight() - 22, 40, 14, Justification::left);

        // meters
        auto drawMeter = [&] (int x, float v)
        {
            const int h = getHeight() - 10;
            const float db = Decibels::gainToDecibels (v, -60.0f);
            const int fill = (int) jmap (db, -60.0f, 6.0f, 0.0f, (float) h);
            g.setColour (Colour (0xff1c1c20));
            g.fillRect (x, 5, 5, h);
            g.setColour (v > 1.0f ? Colours::red : (db > -6.0f ? Colours::yellow : Colour (0xff4cd964)));
            g.fillRect (x, 5 + h - fill, 5, fill);
        };
        drawMeter (getWidth() - 13, meterL);
        drawMeter (getWidth() - 7, meterR);
    }

    void resized() override
    {
        const int w = getWidth() - 18;
        name.setBounds (8, 4, w - 8 - 4 * 23, 20);
        int x = w - 4 * 23;
        for (auto* b : { &muteB, &soloB, &armB, &monB })
        {
            b->setBounds (x, 4, 21, 20);
            x += 23;
        }
        vol.setBounds (8, 32, w - 50, 22);
        pan.setBounds (w - 40, 23, 40, 40); // V4 rotary draws reduced by 10px
        fx.setBounds (52, 61, t.type == TrackType::Audio ? w - 52 - 84 : w - 52, 21);
        input.setBounds (w - 80, 61, 80, 21);
    }

    void mouseDown (const MouseEvent& e) override
    {
        view.selectTrack (engine.indexOf (&t));
        if (e.mods.isPopupMenu())
        {
            PopupMenu m;
            m.addItem (1, "Rename");
            m.addItem (2, "Delete track");
            m.showMenuAsync (PopupMenu::Options(), [safe = SafePointer<TrackHeader> (this)] (int r)
            {
                if (safe == nullptr) return;
                if (r == 1) safe->name.showEditor();
                if (r == 2 && safe->view.onDeleteTrack) safe->view.onDeleteTrack (safe->engine.indexOf (&safe->t));
            });
        }
    }

    void showFxMenu()
    {
        PopupMenu m;
        for (int i = 0; i < (int) t.plugins.size(); ++i)
        {
            auto& slot = t.plugins[(size_t) i];
            if (slot.instance == nullptr) continue;
            PopupMenu sub;
            sub.addItem (1000 + i * 10 + 1, "Show editor");
            sub.addItem (1000 + i * 10 + 2, "Bypass", true, slot.bypassed);
            sub.addItem (1000 + i * 10 + 3, "Remove");
            m.addSubMenu (String (i + 1) + ". " + slot.instance->getName(), sub);
        }
        if (! t.plugins.empty()) m.addSeparator();

        auto types = engine.getKnownPlugins().getTypes();
        if (t.type == TrackType::Audio)
        {
            Array<PluginDescription> effects;
            for (auto& d : types) if (! d.isInstrument) effects.add (d);
            types = effects;
        }

        PopupMenu add;
        KnownPluginList::addToMenu (add, types, KnownPluginList::sortByManufacturer);
        if (types.isEmpty())
            m.addItem (1, "No plugins found - use Options > Plugin Manager to scan", false, false);
        else
            m.addSubMenu (t.type == TrackType::Midi && t.plugins.empty() ? "Add instrument" : "Add plugin", add);

        m.showMenuAsync (PopupMenu::Options().withTargetComponent (fx),
                         [safe = SafePointer<TrackHeader> (this), types] (int r)
        {
            if (safe == nullptr || r == 0) return;
            safe->handleFxResult (r, types);
        });
    }

    void handleFxResult (int r, const Array<PluginDescription>& types)
    {
        if (r >= 1000 && r < 1000 + 10 * (int) t.plugins.size())
        {
            const int slot = (r - 1000) / 10, action = (r - 1000) % 10;
            if (action == 1 && view.onOpenPlugin) view.onOpenPlugin (t, slot);
            if (action == 2) { t.plugins[(size_t) slot].bypassed = ! t.plugins[(size_t) slot].bypassed; t.needNotesOff = true; }
            if (action == 3 && view.onRemovePlugin) view.onRemovePlugin (t, slot);
            refresh();
            return;
        }

        const int idx = KnownPluginList::getIndexChosenByMenu (types, r);
        if (idx < 0) return;

        String err;
        MouseCursor::showWaitCursor();
        auto* inst = engine.addPlugin (t, types.getReference (idx), err);
        MouseCursor::hideWaitCursor();
        if (inst == nullptr)
        {
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Plugin failed to load", err);
            return;
        }
        refresh();
        if (view.onOpenPlugin) view.onOpenPlugin (t, (int) t.plugins.size() - 1);
    }

    ArrangeView& view;
    Engine& engine;
    Track& t;
    Label name;
    TextButton muteB, soloB, armB, monB, fx;
    Slider vol, pan;
    ComboBox input;
    float meterL = 0.0f, meterR = 0.0f;
};

//==============================================================================
class RulerBar : public Component
{
public:
    RulerBar (ArrangeView& av, Engine& e) : view (av), engine (e) {}

    void paint (Graphics& g) override
    {
        g.fillAll (Colour (0xff232327));
        const double ppb = view.pixelsPerBeat;
        int barStep = 1;
        while (barStep * 4 * ppb < 60.0) barStep *= 2;

        const int firstBar = (int) std::floor (view.scrollBeats / 4.0);
        g.setFont (11.0f);
        for (int bar = firstBar; ; ++bar)
        {
            const double x = view.beatToX (bar * 4.0);
            if (x > getWidth()) break;
            if (bar < 0) continue;
            if (bar % barStep == 0)
            {
                g.setColour (Colours::grey);
                g.drawVerticalLine ((int) x, 8.0f, (float) getHeight());
                g.setColour (Colours::lightgrey);
                g.drawText (String (bar + 1), (int) x + 3, 2, 40, 14, Justification::left);
            }
            else if (ppb * 4 > 8)
            {
                g.setColour (Colours::grey.withAlpha (0.4f));
                g.drawVerticalLine ((int) x, 16.0f, (float) getHeight());
            }
        }

        const double px = view.beatToX (engine.secondsToBeats (engine.getPlayPosition() / engine.getSampleRate()));
        Path tri;
        tri.addTriangle ((float) px - 6, 10.0f, (float) px + 6, 10.0f, (float) px, (float) getHeight());
        g.setColour (Colour (0xffff5050));
        g.fillPath (tri);
    }

    void mouseDown (const MouseEvent& e) override { mouseDrag (e); }
    void mouseDrag (const MouseEvent& e) override
    {
        if (engine.isRecording()) return;
        double beat = jmax (0.0, view.xToBeat (e.x));
        if (! e.mods.isShiftDown()) beat = view.snapBeat (beat);
        engine.setPosition ((int64) (engine.beatsToSeconds (beat) * engine.getSampleRate()));
        view.repaint();
    }

    ArrangeView& view;
    Engine& engine;
};

//==============================================================================
class LaneCanvas : public Component
{
public:
    LaneCanvas (ArrangeView& av, Engine& e) : view (av), engine (e) {}

    struct Hit
    {
        int track = -1;
        AudioClip* audio = nullptr;
        MidiClip* midi = nullptr;
        bool rightEdge = false;
        bool clip() const { return audio != nullptr || midi != nullptr; }
    };

    int rowTop (int i) const { return i * ArrangeView::trackHeight - view.scrollY; }
    int trackAtY (int y) const
    {
        const int i = (y + view.scrollY) / ArrangeView::trackHeight;
        return (y + view.scrollY) >= 0 && i < engine.getTracks().size() ? i : -1;
    }

    Rectangle<float> clipRect (int track, double startBeat, double lenBeats) const
    {
        const float x0 = (float) view.beatToX (startBeat);
        const float x1 = (float) view.beatToX (startBeat + lenBeats);
        return { x0, (float) rowTop (track) + 3.0f, jmax (2.0f, x1 - x0), (float) ArrangeView::trackHeight - 7.0f };
    }

    Hit hitClip (Point<int> p) const
    {
        Hit h;
        h.track = trackAtY (p.y);
        if (h.track < 0) return h;
        auto& t = *engine.getTracks()[h.track];
        const auto pf = p.toFloat();

        for (int i = t.audioClips.size(); --i >= 0;)
        {
            auto* c = t.audioClips[i];
            auto r = clipRect (h.track, engine.secondsToBeats (c->start), engine.secondsToBeats (c->length));
            if (r.contains (pf)) { h.audio = c; h.rightEdge = pf.x > r.getRight() - 6; return h; }
        }
        for (int i = t.midiClips.size(); --i >= 0;)
        {
            auto* c = t.midiClips[i];
            auto r = clipRect (h.track, c->start, c->length);
            if (r.contains (pf)) { h.midi = c; h.rightEdge = pf.x > r.getRight() - 6; return h; }
        }
        return h;
    }

    void paint (Graphics& g) override
    {
        auto& tracks = engine.getTracks();
        g.fillAll (Colour (0xff26262a));

        for (int i = 0; i < tracks.size(); ++i)
        {
            const int y = rowTop (i);
            if (y > getHeight() || y + ArrangeView::trackHeight < 0) continue;
            g.setColour (i == view.getSelectedTrack() ? Colour (0xff34343b) : (i % 2 ? Colour (0xff2c2c31) : Colour (0xff2f2f34)));
            g.fillRect (0, y, getWidth(), ArrangeView::trackHeight);
            g.setColour (Colours::black.withAlpha (0.4f));
            g.drawHorizontalLine (y + ArrangeView::trackHeight - 1, 0.0f, (float) getWidth());
        }

        // grid
        const double ppb = view.pixelsPerBeat;
        int barStep = 1;
        while (barStep * 4 * ppb < 24.0) barStep *= 2;
        const int contentBottom = jmin (getHeight(), rowTop (tracks.size()));
        for (int beat = (int) std::floor (view.scrollBeats); ; ++beat)
        {
            const double x = view.beatToX (beat);
            if (x > getWidth()) break;
            if (beat < 0) continue;
            const bool isBar = beat % 4 == 0;
            if (isBar && (beat / 4) % barStep == 0)
                g.setColour (Colours::white.withAlpha (0.13f));
            else if (! isBar && ppb >= 12)
                g.setColour (Colours::white.withAlpha (0.04f));
            else
                continue;
            g.drawVerticalLine ((int) x, 0.0f, (float) contentBottom);
        }

        // clips
        for (int i = 0; i < tracks.size(); ++i)
        {
            const int y = rowTop (i);
            if (y > getHeight() || y + ArrangeView::trackHeight < 0) continue;
            auto& t = *tracks[i];
            for (auto* c : t.audioClips) drawAudioClip (g, i, t, *c);
            for (auto* c : t.midiClips)  drawMidiClip (g, i, t, *c);
        }

        // live recording region
        if (engine.isRecording())
        {
            const double sr = engine.getSampleRate();
            const double b0 = engine.secondsToBeats (engine.getRecordStart() / sr);
            const double b1 = engine.secondsToBeats (engine.getPlayPosition() / sr);
            for (int i = 0; i < tracks.size(); ++i)
                if (tracks[i]->armed)
                {
                    auto r = clipRect (i, b0, b1 - b0);
                    g.setColour (Colour (0x66ff3030));
                    g.fillRoundedRectangle (r, 3.0f);
                    g.setColour (Colours::red);
                    g.drawRoundedRectangle (r, 3.0f, 1.0f);
                }
        }

        // playhead
        const double px = view.beatToX (engine.secondsToBeats (engine.getPlayPosition() / engine.getSampleRate()));
        g.setColour (Colour (0xffff5050));
        g.drawVerticalLine ((int) px, 0.0f, (float) getHeight());

        if (tracks.isEmpty())
        {
            g.setColour (Colours::grey);
            g.setFont (16.0f);
            g.drawText ("Add a track from the Track menu (Ctrl+T audio, Ctrl+M MIDI), or drop audio files here.",
                        getLocalBounds(), Justification::centred);
        }
    }

    void drawClipFrame (Graphics& g, Rectangle<float> r, Colour col, bool selected, const String& label)
    {
        g.setColour (col.withAlpha (0.35f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (col.withAlpha (0.85f));
        g.fillRoundedRectangle (r.withHeight (15.0f), 4.0f);
        g.setColour (selected ? Colours::white : col.brighter (0.3f));
        g.drawRoundedRectangle (r, 4.0f, selected ? 2.0f : 1.0f);
        g.setColour (Colours::black);
        g.setFont (11.0f);
        g.drawText (label, r.withHeight (15.0f).reduced (4.0f, 0.0f).toNearestInt(), Justification::centredLeft, true);
    }

    void drawAudioClip (Graphics& g, int row, Track& t, AudioClip& c)
    {
        auto r = clipRect (row, engine.secondsToBeats (c.start), engine.secondsToBeats (c.length));
        if (r.getRight() < 0 || r.getX() > getWidth()) return;
        drawClipFrame (g, r, t.colour, view.selection.audio == &c, c.file.getFileNameWithoutExtension());

        if (c.peaks.empty()) return;
        auto wave = r.withTrimmedTop (16.0f).reduced (1.0f, 2.0f);
        const float mid = wave.getCentreY(), halfH = wave.getHeight() * 0.5f;
        const double sr = engine.getSampleRate();
        const double samplesPerPixel = sr * 60.0 / (engine.getBpm() * view.pixelsPerBeat);
        const int xStart = (int) jmax (0.0f, wave.getX()), xEnd = (int) jmin ((float) getWidth(), wave.getRight());
        g.setColour (t.colour.brighter (0.6f).withAlpha (0.9f));
        for (int x = xStart; x < xEnd; ++x)
        {
            const double srcStart = (x - r.getX()) * samplesPerPixel + c.offset * sr;
            const int b0 = (int) (srcStart / AudioClip::peakBlock);
            const int b1 = jmax (b0 + 1, (int) ((srcStart + samplesPerPixel) / AudioClip::peakBlock));
            float m = 0.0f;
            for (int b = b0; b < b1 && b < (int) c.peaks.size(); ++b)
                if (b >= 0) m = jmax (m, c.peaks[(size_t) b]);
            const float h = jmin (1.0f, m) * halfH;
            g.drawVerticalLine (x, mid - h, mid + h + 1.0f);
        }
    }

    void drawMidiClip (Graphics& g, int row, Track& t, MidiClip& c)
    {
        auto r = clipRect (row, c.start, c.length);
        if (r.getRight() < 0 || r.getX() > getWidth()) return;
        drawClipFrame (g, r, t.colour, view.selection.midi == &c, c.name);

        if (c.notes.empty()) return;
        int lo = 127, hi = 0;
        for (auto& n : c.notes) { lo = jmin (lo, n.note); hi = jmax (hi, n.note); }
        lo -= 2; hi += 2;
        auto area = r.withTrimmedTop (17.0f).reduced (1.0f, 2.0f);
        const float rowH = area.getHeight() / (float) (hi - lo + 1);
        g.setColour (t.colour.brighter (0.7f));
        for (auto& n : c.notes)
        {
            if (n.start >= c.length) continue;
            const float x0 = (float) view.beatToX (c.start + n.start);
            const float x1 = (float) view.beatToX (c.start + jmin (n.start + n.length, c.length));
            const float y = area.getBottom() - (n.note - lo + 1) * rowH;
            g.fillRect (x0, y, jmax (1.0f, x1 - x0 - 1.0f), jmax (1.5f, rowH - 0.5f));
        }
    }

    //--------------------------------------------------------------------------
    void mouseMove (const MouseEvent& e) override
    {
        auto h = hitClip (e.getPosition());
        setMouseCursor (h.clip() && h.rightEdge ? MouseCursor::LeftRightResizeCursor
                                                : (h.clip() ? MouseCursor::DraggingHandCursor : MouseCursor::NormalCursor));
    }

    void mouseDown (const MouseEvent& e) override
    {
        auto h = hitClip (e.getPosition());
        if (h.track >= 0) view.selectTrack (h.track);
        auto& tracks = engine.getTracks();

        view.selection = {};
        if (h.track >= 0 && h.clip())
            view.selection = { tracks[h.track], h.audio, h.midi };

        if (e.mods.isPopupMenu())
        {
            showContextMenu (h, e.x);
            view.repaint();
            return;
        }

        drag = h;
        mode = None;
        if (h.clip())
        {
            mode = h.rightEdge ? Resize : Move;
            if (h.audio) { origStart = engine.secondsToBeats (h.audio->start); origLength = engine.secondsToBeats (h.audio->length); }
            else         { origStart = h.midi->start; origLength = h.midi->length; }
        }
        else if (! engine.isRecording())
        {
            double beat = jmax (0.0, view.xToBeat (e.x));
            if (! e.mods.isShiftDown()) beat = view.snapBeat (beat);
            engine.setPosition ((int64) (engine.beatsToSeconds (beat) * engine.getSampleRate()));
        }
        view.repaint();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (mode == None || drag.track < 0) return;
        const double delta = e.getDistanceFromDragStartX() / view.pixelsPerBeat;
        const bool free = e.mods.isShiftDown() || ! view.snapEnabled;
        auto& tracks = engine.getTracks();
        const ScopedLock sl (engine.getLock());

        if (mode == Move)
        {
            double s = jmax (0.0, origStart + delta);
            if (! free) s = view.snapBeat (s, true);
            if (drag.audio) drag.audio->start = engine.beatsToSeconds (s);
            else            drag.midi->start = s;

            // move to another track of the same type
            const int target = trackAtY (e.y);
            if (target >= 0 && target != drag.track && tracks[target]->type == tracks[drag.track]->type)
            {
                auto* from = tracks[drag.track];
                auto* to = tracks[target];
                if (drag.audio) { from->audioClips.removeObject (drag.audio, false); to->audioClips.add (drag.audio); }
                else            { from->midiClips.removeObject (drag.midi, false); to->midiClips.add (drag.midi); from->needNotesOff = true; }
                drag.track = target;
                view.selection.track = to;
                view.selectTrack (target);
            }
        }
        else if (mode == Resize)
        {
            double endB = origStart + origLength + delta;
            if (! free) endB = view.snapBeat (endB, true);
            double len = jmax (0.125, endB - origStart);
            if (drag.audio)
            {
                const double maxLen = drag.audio->sourceLength - drag.audio->offset;
                drag.audio->length = jlimit (0.01, maxLen, engine.beatsToSeconds (len));
            }
            else
                drag.midi->length = len;
        }
        repaint();
    }

    void mouseUp (const MouseEvent&) override
    {
        if (mode != None) engine.sendChangeMessage();
        mode = None;
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        auto h = hitClip (e.getPosition());
        if (h.track < 0) return;
        auto& t = *engine.getTracks()[h.track];
        if (h.midi != nullptr)
        {
            if (view.onOpenPianoRoll) view.onOpenPianoRoll (h.midi);
        }
        else if (! h.clip() && t.type == TrackType::Midi)
        {
            const double bar = std::floor (jmax (0.0, view.xToBeat (e.x)) / 4.0) * 4.0;
            auto* c = engine.addMidiClip (t, bar, 16.0);
            view.selection = { &t, nullptr, c };
            if (view.onOpenPianoRoll) view.onOpenPianoRoll (c);
        }
    }

    void showContextMenu (Hit h, int x)
    {
        PopupMenu m;
        if (h.clip())
        {
            if (h.midi) m.addItem (1, "Edit in piano roll");
            m.addItem (2, "Split at playhead  (S)");
            m.addItem (3, "Delete clip  (Del)");
            if (h.midi) m.addItem (4, "Duplicate");
        }
        else if (h.track >= 0)
        {
            auto& t = *engine.getTracks()[h.track];
            if (t.type == TrackType::Midi) m.addItem (10, "Insert empty MIDI clip (4 bars)");
            else m.addItem (11, "Import audio here...");
        }
        if (m.getNumItems() == 0) return;

        const double beat = view.snapBeat (jmax (0.0, view.xToBeat (x)));
        m.showMenuAsync (PopupMenu::Options(), [safe = SafePointer<LaneCanvas> (this), h, beat] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& v = safe->view;
            auto& eng = safe->engine;
            if (! isPositiveAndBelow (h.track, eng.getTracks().size())) return;
            auto& t = *eng.getTracks()[h.track];

            if (r == 1 && v.onOpenPianoRoll) v.onOpenPianoRoll (h.midi);
            if (r == 2) v.splitAtPlayhead();
            if (r == 3) v.deleteSelectedClip();
            if (r == 4 && h.midi)
            {
                auto* c = eng.addMidiClip (t, h.midi->start + h.midi->length, h.midi->length);
                const ScopedLock sl (eng.getLock());
                c->notes = h.midi->notes;
                c->name = h.midi->name;
            }
            if (r == 10) eng.addMidiClip (t, std::floor (beat / 4.0) * 4.0, 16.0);
            if (r == 11)
            {
                auto chooser = std::make_shared<FileChooser> ("Import audio", File(), eng.getAudioFormats().getWildcardForAllFormats());
                chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectMultipleItems,
                                      [safe, chooser, track = h.track, beat] (const FileChooser& fc)
                {
                    if (safe != nullptr && ! fc.getResults().isEmpty())
                        safe->view.importAudio (fc.getResults(), track, beat);
                });
            }
        });
    }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        view.mouseWheelMove (e.getEventRelativeTo (&view), w);
    }

    ArrangeView& view;
    Engine& engine;
    enum Mode { None, Move, Resize } mode = None;
    Hit drag;
    double origStart = 0.0, origLength = 0.0;
};

//==============================================================================
ArrangeView::ArrangeView (Engine& e) : engine (e)
{
    ruler = std::make_unique<RulerBar> (*this, engine);
    canvas = std::make_unique<LaneCanvas> (*this, engine);
    addAndMakeVisible (*ruler);
    addAndMakeVisible (*canvas);
    addAndMakeVisible (headerHolder);
    addAndMakeVisible (vbar);
    addAndMakeVisible (hbar);
    noFocus (*canvas);
    noFocus (*ruler);
    noFocus (vbar);
    noFocus (hbar);
    vbar.addListener (this);
    hbar.addListener (this);
    vbar.setAutoHide (false);
    hbar.setAutoHide (false);
    engine.addChangeListener (this);
    rebuild();
}

ArrangeView::~ArrangeView()
{
    engine.removeChangeListener (this);
}

void ArrangeView::changeListenerCallback (ChangeBroadcaster*)
{
    rebuild();
}

void ArrangeView::rebuild()
{
    auto& tracks = engine.getTracks();
    bool same = headerTracks.size() == tracks.size();
    for (int i = 0; same && i < tracks.size(); ++i)
        same = headerTracks[i] == tracks[i];

    if (! same)
    {
        headers.clear();
        headerTracks.clear();
        for (auto* t : tracks)
        {
            auto* h = new TrackHeader (*this, engine, *t);
            headers.add (h);
            headerTracks.add (t);
            headerHolder.addAndMakeVisible (h);
        }
        if (selectedTrack >= tracks.size()) selectedTrack = tracks.size() - 1;
        if (selection.track != nullptr && ! tracks.contains (selection.track)) selection = {};
    }
    else
    {
        for (auto* h : headers)
            static_cast<TrackHeader*> (h)->refresh();
    }

    // drop a stale clip selection
    if (selection.track != nullptr)
        if ((selection.audio && ! selection.track->audioClips.contains (selection.audio))
            || (selection.midi && ! selection.track->midiClips.contains (selection.midi)))
            selection = {};

    layoutHeaders();
    updateScrollBars();
    repaint();
}

void ArrangeView::selectTrack (int i)
{
    if (selectedTrack == i) return;
    selectedTrack = i;
    for (auto* h : headers) h->repaint();
    canvas->repaint();
}

int ArrangeView::totalTracksHeight() const
{
    return engine.getTracks().size() * trackHeight;
}

void ArrangeView::layoutHeaders()
{
    for (int i = 0; i < headers.size(); ++i)
        headers[i]->setBounds (0, i * trackHeight - scrollY, headerWidth, trackHeight);
}

void ArrangeView::resized()
{
    auto r = getLocalBounds();
    hbar.setBounds (r.removeFromBottom (14).withTrimmedLeft (headerWidth).withTrimmedRight (14));
    vbar.setBounds (r.removeFromRight (14).withTrimmedTop (rulerHeight));
    auto top = r.removeFromTop (rulerHeight);
    top.removeFromLeft (headerWidth);
    ruler->setBounds (top);
    headerHolder.setBounds (r.removeFromLeft (headerWidth));
    canvas->setBounds (r);
    layoutHeaders();
    updateScrollBars();
}

void ArrangeView::paint (Graphics& g)
{
    g.fillAll (Colour (0xff2a2a2f));
    g.setColour (Colour (0xff1f1f23));
    g.fillRect (0, 0, headerWidth, rulerHeight);
    g.setColour (Colours::grey);
    g.setFont (11.0f);
    g.drawText ("TRACKS", 10, 0, 100, rulerHeight, Justification::centredLeft);
}

void ArrangeView::updateScrollBars()
{
    const int visH = canvas->getHeight();
    const int totalH = totalTracksHeight() + trackHeight;
    scrollY = jlimit (0, jmax (0, totalH - visH), scrollY);
    vbar.setRangeLimits (0.0, (double) jmax (totalH, visH));
    vbar.setCurrentRange ((double) scrollY, (double) visH, dontSendNotification);

    const double visBeats = canvas->getWidth() / pixelsPerBeat;
    const double endBeats = engine.secondsToBeats (engine.getProjectEndSeconds()) + 64.0;
    hbar.setRangeLimits (0.0, jmax (endBeats, scrollBeats + visBeats));
    hbar.setCurrentRange (scrollBeats, visBeats, dontSendNotification);
    layoutHeaders();
}

void ArrangeView::scrollBarMoved (ScrollBar* bar, double start)
{
    if (bar == &vbar) scrollY = (int) start;
    else scrollBeats = jmax (0.0, start);
    layoutHeaders();
    repaint();
}

void ArrangeView::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        zoomBy (w.deltaY > 0 ? 1.2 : 1.0 / 1.2, e.x - headerWidth);
        return;
    }
    const bool horiz = e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY);
    if (horiz)
    {
        const float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        scrollBeats = jmax (0.0, scrollBeats - d * 300.0 / pixelsPerBeat);
    }
    else
        scrollY -= (int) (w.deltaY * 250.0f);
    updateScrollBars();
    repaint();
}

void ArrangeView::zoomBy (double factor, int anchorX)
{
    if (anchorX < 0) anchorX = canvas->getWidth() / 2;
    const double anchorBeat = xToBeat (anchorX);
    pixelsPerBeat = jlimit (1.0, 400.0, pixelsPerBeat * factor);
    scrollBeats = jmax (0.0, anchorBeat - anchorX / pixelsPerBeat);
    updateScrollBars();
    repaint();
}

double ArrangeView::snapBeat (double b, bool force) const
{
    if (! snapEnabled && ! force) return b;
    double grid = 1.0;
    if (pixelsPerBeat > 80) grid = 0.25;
    else if (pixelsPerBeat > 40) grid = 0.5;
    else if (pixelsPerBeat < 6) grid = 4.0;
    return std::round (b / grid) * grid;
}

void ArrangeView::tick()
{
    for (auto* h : headers)
        static_cast<TrackHeader*> (h)->updateMeter();

    if (engine.isPlaying())
    {
        const double beat = engine.secondsToBeats (engine.getPlayPosition() / engine.getSampleRate());
        const double x = beatToX (beat);
        if (x > canvas->getWidth() - 30 || x < 0)
        {
            scrollBeats = jmax (0.0, beat - 2.0);
            updateScrollBars();
        }
        canvas->repaint();
        ruler->repaint();
        if (engine.isRecording()) updateScrollBars();
    }
}

void ArrangeView::deleteSelectedClip()
{
    if (! selection.any() || selection.track == nullptr) return;
    auto sel = selection;
    selection = {};
    if (sel.midi)
    {
        if (onMidiClipAboutToBeDeleted) onMidiClipAboutToBeDeleted (sel.midi);
        engine.deleteMidiClip (*sel.track, sel.midi);
    }
    else
        engine.deleteAudioClip (*sel.track, sel.audio);
}

void ArrangeView::splitAtPlayhead()
{
    const double secs = engine.getPlayPosition() / engine.getSampleRate();
    if (selection.track != nullptr)
        engine.splitClipsAt (*selection.track, secs);
    else if (isPositiveAndBelow (selectedTrack, engine.getTracks().size()))
        engine.splitClipsAt (*engine.getTracks()[selectedTrack], secs);
    selection = {};
}

void ArrangeView::importAudio (const Array<File>& files, int trackIndex, double beat)
{
    StringArray errors;
    bool first = true;
    for (auto& f : files)
    {
        Track* t = nullptr;
        if (first && isPositiveAndBelow (trackIndex, engine.getTracks().size())
            && engine.getTracks()[trackIndex]->type == TrackType::Audio)
            t = engine.getTracks()[trackIndex];
        if (t == nullptr)
            t = engine.addTrack (TrackType::Audio, f.getFileNameWithoutExtension());
        first = false;

        String err;
        MouseCursor::showWaitCursor();
        if (engine.addAudioClip (*t, f, engine.beatsToSeconds (beat), err) == nullptr)
            errors.add (err);
        MouseCursor::hideWaitCursor();
    }
    if (! errors.isEmpty())
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Import", errors.joinIntoString ("\n"));
}

bool ArrangeView::isInterestedInFileDrag (const StringArray& files)
{
    for (auto& f : files)
        if (engine.getAudioFormats().findFormatForFileExtension (File (f).getFileExtension()) != nullptr)
            return true;
    return false;
}

void ArrangeView::filesDropped (const StringArray& files, int x, int y)
{
    Array<File> audio;
    for (auto& f : files)
        if (engine.getAudioFormats().findFormatForFileExtension (File (f).getFileExtension()) != nullptr)
            audio.add (File (f));

    auto* lc = static_cast<LaneCanvas*> (canvas.get());
    const auto p = canvas->getLocalPoint (this, Point<int> (x, y));
    const int track = lc->trackAtY (p.y);
    double beat = jmax (0.0, xToBeat (p.x));
    beat = snapBeat (beat);
    importAudio (audio, track, beat);
}

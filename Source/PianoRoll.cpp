#include "PianoRoll.h"

using namespace juce;

namespace
{
    constexpr int rowH = 14;
    constexpr int keysW = 58;

    bool isBlack (int note)
    {
        const int n = note % 12;
        return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    }
}

class SyncViewport : public Viewport
{
public:
    std::function<void()> onScroll;
    void visibleAreaChanged (const Rectangle<int>&) override { if (onScroll) onScroll(); }
};

//==============================================================================
class NoteGrid : public Component
{
public:
    NoteGrid (Engine& e, MidiClip& c) : engine (e), clip (c)
    {
        setWantsKeyboardFocus (true);
        updateSize();
    }

    std::function<void()> onSelectionChanged;
    double ppb = 80.0;
    double grid = 0.25;
    int selected = -1;
    double lastLength = 0.25;

    void updateSize()
    {
        setSize ((int) ((clip.length + 16.0) * ppb), 128 * rowH);
    }

    int trackIndex() const
    {
        int idx = -1;
        engine.findTrackForMidiClip (&clip, &idx);
        return idx;
    }

    double xToBeat (int x) const { return x / ppb; }
    int noteAtY (int y) const { return jlimit (0, 127, 127 - y / rowH); }
    Rectangle<float> noteRect (const MidiNote& n) const
    {
        return { (float) (n.start * ppb), (float) ((127 - n.note) * rowH),
                 (float) jmax (3.0, n.length * ppb), (float) rowH - 1.0f };
    }

    int hitNote (Point<int> p) const
    {
        for (int i = (int) clip.notes.size(); --i >= 0;)
            if (noteRect (clip.notes[(size_t) i]).contains (p.toFloat()))
                return i;
        return -1;
    }

    double snapDown (double b) const { return std::floor (b / grid + 1.0e-6) * grid; }
    double snapNear (double b) const { return std::round (b / grid) * grid; }

    void paint (Graphics& g) override
    {
        auto clipR = g.getClipBounds();
        for (int n = 0; n < 128; ++n)
        {
            const int y = (127 - n) * rowH;
            if (y > clipR.getBottom() || y + rowH < clipR.getY()) continue;
            g.setColour (isBlack (n) ? Colour (0xff26262b) : Colour (0xff2e2e34));
            g.fillRect (clipR.getX(), y, clipR.getWidth(), rowH);
            if (n % 12 == 0)
            {
                g.setColour (Colours::black.withAlpha (0.6f));
                g.drawHorizontalLine (y + rowH - 1, (float) clipR.getX(), (float) clipR.getRight());
            }
        }

        // vertical grid
        const double totalBeats = getWidth() / ppb;
        const double step = (grid * ppb < 6.0) ? 1.0 : grid;
        const int firstLine = (int) std::floor (xToBeat (clipR.getX()) / step);
        for (int i = firstLine; i * step <= totalBeats; ++i)
        {
            const double b = i * step;
            const float x = (float) (b * ppb);
            if (x > clipR.getRight()) break;
            const bool bar = std::fmod (b, 4.0) < 1.0e-6;
            const bool beat = std::fmod (b, 1.0) < 1.0e-6;
            g.setColour (Colours::white.withAlpha (bar ? 0.22f : beat ? 0.1f : 0.04f));
            g.drawVerticalLine ((int) x, (float) clipR.getY(), (float) clipR.getBottom());
            if (bar)
            {
                g.setColour (Colours::grey);
                g.setFont (10.0f);
            }
        }

        // past clip end
        const float endX = (float) (clip.length * ppb);
        g.setColour (Colours::black.withAlpha (0.45f));
        g.fillRect (endX, (float) clipR.getY(), (float) getWidth() - endX, (float) clipR.getHeight());
        g.setColour (Colour (0xffff9040));
        g.drawVerticalLine ((int) endX, (float) clipR.getY(), (float) clipR.getBottom());

        // notes
        for (size_t i = 0; i < clip.notes.size(); ++i)
        {
            auto& n = clip.notes[i];
            auto r = noteRect (n);
            if (! r.toNearestInt().intersects (clipR)) continue;
            const float v = n.velocity / 127.0f;
            auto col = Colour (0xff3a7bd5).interpolatedWith (Colour (0xffff5a36), v);
            if (n.start >= clip.length) col = col.withAlpha (0.35f);
            g.setColour (col);
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour ((int) i == selected ? Colours::white : Colours::black.withAlpha (0.6f));
            g.drawRoundedRectangle (r, 2.0f, (int) i == selected ? 2.0f : 1.0f);
        }

        // playhead
        if (engine.isPlaying())
        {
            const double beat = engine.secondsToBeats (engine.getPlayPosition() / engine.getSampleRate()) - clip.start;
            if (beat >= 0 && beat <= clip.length + 16)
            {
                g.setColour (Colour (0xffff5050));
                g.drawVerticalLine ((int) (beat * ppb), (float) clipR.getY(), (float) clipR.getBottom());
            }
        }
    }

    void preview (int note, int vel, bool on)
    {
        const int t = trackIndex();
        if (t >= 0) engine.previewNote (t, note, vel, on);
    }

    void stopPreview()
    {
        if (previewing >= 0) preview (previewing, 0, false);
        previewing = -1;
    }

    void mouseDown (const MouseEvent& e) override
    {
        grabKeyboardFocus();
        const int hit = hitNote (e.getPosition());
        const ScopedLock sl (engine.getLock());

        if (e.mods.isPopupMenu())
        {
            if (hit >= 0)
            {
                clip.notes.erase (clip.notes.begin() + hit);
                selected = -1;
            }
            changed();
            return;
        }

        mode = None;
        if (hit >= 0)
        {
            selected = hit;
            auto& n = clip.notes[(size_t) hit];
            orig = n;
            const auto r = noteRect (n);
            if (e.mods.isAltDown())         mode = Velocity;
            else if (e.x > r.getRight() - 6) mode = Resize;
            else                             mode = Move;
        }
        else
        {
            MidiNote n;
            n.note = noteAtY (e.y);
            n.start = jmax (0.0, snapDown (xToBeat (e.x)));
            n.length = lastLength;
            n.velocity = 100;
            clip.notes.push_back (n);
            selected = (int) clip.notes.size() - 1;
            orig = n;
            mode = Create;
        }

        stopPreview();
        previewing = clip.notes[(size_t) selected].note;
        preview (previewing, clip.notes[(size_t) selected].velocity, true);
        changed();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (mode == None || ! isPositiveAndBelow (selected, (int) clip.notes.size())) return;
        const ScopedLock sl (engine.getLock());
        auto& n = clip.notes[(size_t) selected];
        const double dBeats = e.getDistanceFromDragStartX() / ppb;
        const bool free = e.mods.isShiftDown();

        if (mode == Move)
        {
            double s = orig.start + dBeats;
            n.start = jmax (0.0, free ? s : snapNear (s));
            const int newNote = jlimit (0, 127, orig.note - (int) std::round (e.getDistanceFromDragStartY() / (double) rowH));
            if (newNote != n.note)
            {
                n.note = newNote;
                stopPreview();
                previewing = newNote;
                preview (newNote, n.velocity, true);
            }
        }
        else if (mode == Resize || mode == Create)
        {
            double endB = orig.start + orig.length + dBeats;
            if (mode == Create) endB = xToBeat (e.x);
            endB = free ? endB : std::ceil (endB / grid - 1.0e-6) * grid;
            n.length = jmax (free ? 1.0 / 64.0 : grid, endB - n.start);
        }
        else if (mode == Velocity)
        {
            n.velocity = jlimit (1, 127, orig.velocity - e.getDistanceFromDragStartY());
        }
        changed();
    }

    void mouseUp (const MouseEvent&) override
    {
        stopPreview();
        if (isPositiveAndBelow (selected, (int) clip.notes.size()) && (mode == Resize || mode == Create))
            lastLength = clip.notes[(size_t) selected].length;
        mode = None;
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        const int hit = hitNote (e.getPosition());
        if (hit < 0) return;
        const ScopedLock sl (engine.getLock());
        clip.notes.erase (clip.notes.begin() + hit);
        selected = -1;
        changed();
    }

    void mouseMove (const MouseEvent& e) override
    {
        const int hit = hitNote (e.getPosition());
        if (hit >= 0 && e.x > noteRect (clip.notes[(size_t) hit]).getRight() - 6)
            setMouseCursor (MouseCursor::LeftRightResizeCursor);
        else
            setMouseCursor (hit >= 0 ? MouseCursor::DraggingHandCursor : MouseCursor::NormalCursor);
    }

    bool keyPressed (const KeyPress& k) override
    {
        if (! isPositiveAndBelow (selected, (int) clip.notes.size()))
            return false;

        const ScopedLock sl (engine.getLock());
        auto& n = clip.notes[(size_t) selected];
        const int code = k.getKeyCode();
        const bool shift = k.getModifiers().isShiftDown();

        if (code == KeyPress::deleteKey || code == KeyPress::backspaceKey)
        {
            clip.notes.erase (clip.notes.begin() + selected);
            selected = -1;
        }
        else if (code == KeyPress::upKey)    n.note = jmin (127, n.note + (shift ? 12 : 1));
        else if (code == KeyPress::downKey)  n.note = jmax (0, n.note - (shift ? 12 : 1));
        else if (code == KeyPress::leftKey)  n.start = jmax (0.0, n.start - grid);
        else if (code == KeyPress::rightKey) n.start += grid;
        else return false;

        changed();
        return true;
    }

    void changed()
    {
        if (auto* tr = engine.findTrackForMidiClip (&clip)) tr->needNotesOff = true;
        repaint();
        if (onSelectionChanged) onSelectionChanged();
    }

    Engine& engine;
    MidiClip& clip;
    enum Mode { None, Move, Resize, Create, Velocity } mode = None;
    MidiNote orig;
    int previewing = -1;
};

//==============================================================================
class KeyboardStrip : public Component
{
public:
    KeyboardStrip (NoteGrid& g) : grid (g) {}
    int offsetY = 0;

    void paint (Graphics& g) override
    {
        g.fillAll (Colour (0xff1e1e22));
        for (int n = 0; n < 128; ++n)
        {
            const int y = (127 - n) * rowH - offsetY;
            if (y > getHeight() || y + rowH < 0) continue;
            const bool black = isBlack (n);
            g.setColour (n == held ? Colour (0xff5aa0ff) : (black ? Colour (0xff222222) : Colour (0xffe8e8e8)));
            g.fillRect (black ? 0 : 0, y, black ? getWidth() - 18 : getWidth(), rowH - 1);
            if (n % 12 == 0)
            {
                g.setColour (Colours::black);
                g.setFont (10.0f);
                g.drawText ("C" + String (n / 12 - 1), getWidth() - 30, y, 28, rowH, Justification::centredRight);
            }
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        held = jlimit (0, 127, 127 - (e.y + offsetY) / rowH);
        grid.preview (held, 100, true);
        repaint();
    }
    void mouseUp (const MouseEvent&) override
    {
        if (held >= 0) grid.preview (held, 0, false);
        held = -1;
        repaint();
    }

    NoteGrid& grid;
    int held = -1;
};

//==============================================================================
class PianoRollEditor : public Component, private Timer
{
public:
    PianoRollEditor (Engine& e, MidiClip& c) : engine (e), clip (c), grid (e, c), keys (grid)
    {
        gridBox.addItemList ({ "Grid 1/4", "Grid 1/8", "Grid 1/16", "Grid 1/32", "Grid 1/8T", "Grid 1/16T" }, 1);
        gridBox.setSelectedId (3, dontSendNotification);
        gridBox.onChange = [this]
        {
            const double vals[] = { 1.0, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0 };
            grid.grid = vals[jlimit (0, 5, gridBox.getSelectedId() - 1)];
            grid.lastLength = grid.grid;
            grid.repaint();
        };
        addAndMakeVisible (gridBox);

        quantize.setButtonText ("Quantize all");
        quantize.onClick = [this]
        {
            const ScopedLock sl (engine.getLock());
            for (auto& n : clip.notes)
            {
                n.start = grid.snapNear (n.start);
                n.length = jmax (grid.grid, grid.snapNear (n.length));
            }
            grid.changed();
        };
        addAndMakeVisible (quantize);

        lengthLabel.setText ("Clip bars:", dontSendNotification);
        lengthLabel.setJustificationType (Justification::centredRight);
        addAndMakeVisible (lengthLabel);
        bars.setSliderStyle (Slider::IncDecButtons);
        bars.setTextBoxStyle (Slider::TextBoxLeft, false, 44, 22);
        bars.setRange (1, 512, 1);
        bars.setValue (std::ceil (clip.length / 4.0), dontSendNotification);
        bars.onValueChange = [this]
        {
            {
                const ScopedLock sl (engine.getLock());
                clip.length = bars.getValue() * 4.0;
            }
            grid.updateSize();
            engine.sendChangeMessage();
            grid.changed();
        };
        addAndMakeVisible (bars);

        zoomOut.setButtonText ("-");
        zoomIn.setButtonText ("+");
        zoomOut.onClick = [this] { setZoom (grid.ppb / 1.4); };
        zoomIn.onClick = [this] { setZoom (grid.ppb * 1.4); };
        addAndMakeVisible (zoomOut);
        addAndMakeVisible (zoomIn);

        info.setColour (Label::textColourId, Colours::lightgrey);
        addAndMakeVisible (info);

        help.setText ("Click: add  |  Drag: move/resize  |  Right/double-click: delete  |  Alt-drag: velocity  |  Arrows: nudge",
                      dontSendNotification);
        help.setFont (FontOptions (11.0f));
        help.setColour (Label::textColourId, Colours::grey);
        addAndMakeVisible (help);

        grid.onSelectionChanged = [this] { updateInfo(); };
        viewport.setViewedComponent (&grid, false);
        viewport.setScrollBarsShown (true, true);
        viewport.onScroll = [this] { keys.offsetY = viewport.getViewPositionY(); keys.repaint(); };
        addAndMakeVisible (viewport);
        addAndMakeVisible (keys);

        // centre view on existing notes (or around C4)
        int centre = 60;
        if (! clip.notes.empty())
        {
            int sum = 0;
            for (auto& n : clip.notes) sum += n.note;
            centre = sum / (int) clip.notes.size();
        }
        setSize (1000, 560);
        viewport.setViewPosition (0, jmax (0, (127 - centre) * rowH - 220));
        updateInfo();
        startTimerHz (30);
    }

    void setZoom (double ppb)
    {
        grid.ppb = jlimit (10.0, 600.0, ppb);
        grid.updateSize();
        grid.repaint();
    }

    void updateInfo()
    {
        if (isPositiveAndBelow (grid.selected, (int) clip.notes.size()))
        {
            auto& n = clip.notes[(size_t) grid.selected];
            info.setText (MidiMessage::getMidiNoteName (n.note, true, true, 4) + "  vel " + String (n.velocity),
                          dontSendNotification);
        }
        else
            info.setText (String ((int) clip.notes.size()) + " notes", dontSendNotification);
    }

    void timerCallback() override
    {
        if (engine.isPlaying()) grid.repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop (34).reduced (6, 5);
        gridBox.setBounds (bar.removeFromLeft (110));
        bar.removeFromLeft (6);
        quantize.setBounds (bar.removeFromLeft (100));
        bar.removeFromLeft (6);
        lengthLabel.setBounds (bar.removeFromLeft (70));
        bars.setBounds (bar.removeFromLeft (110));
        bar.removeFromLeft (10);
        zoomOut.setBounds (bar.removeFromLeft (26));
        zoomIn.setBounds (bar.removeFromLeft (26));
        bar.removeFromLeft (10);
        info.setBounds (bar.removeFromLeft (120));
        help.setBounds (bar);
        keys.setBounds (r.removeFromLeft (keysW).withTrimmedBottom (viewport.getScrollBarThickness()));
        viewport.setBounds (r);
    }

    void paint (Graphics& g) override { g.fillAll (Colour (0xff202024)); }

    Engine& engine;
    MidiClip& clip;
    NoteGrid grid;
    KeyboardStrip keys;
    SyncViewport viewport;
    ComboBox gridBox;
    TextButton quantize, zoomIn, zoomOut;
    Label lengthLabel, info, help;
    Slider bars;
};

//==============================================================================
PianoRollWindow::PianoRollWindow (Engine& e, MidiClip& c, std::function<void (PianoRollWindow*)> onClose)
    : DocumentWindow ("Piano Roll - " + c.name, Colour (0xff202024), DocumentWindow::closeButton),
      clip (&c), closeCallback (std::move (onClose))
{
    setUsingNativeTitleBar (true);
    setContentOwned (new PianoRollEditor (e, c), true);
    setResizable (true, true);
    setResizeLimits (500, 300, 4000, 3000);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
}

PianoRollWindow::~PianoRollWindow() = default;

void PianoRollWindow::closeButtonPressed()
{
    if (closeCallback) closeCallback (this);
}

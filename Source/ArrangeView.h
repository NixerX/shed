#pragma once
#include "Engine.h"

//==============================================================================
// The multitrack arrange view: track headers on the left, ruler + lanes/clips on the right.
//==============================================================================
class ArrangeView : public juce::Component,
                    public juce::FileDragAndDropTarget,
                    private juce::ChangeListener,
                    private juce::ScrollBar::Listener
{
public:
    explicit ArrangeView (Engine&);
    ~ArrangeView() override;

    // Hooks the main window fills in (it owns editor windows)
    std::function<void (MidiClip*)> onOpenPianoRoll;
    std::function<void (MidiClip*)> onMidiClipAboutToBeDeleted;
    std::function<void (int)> onDeleteTrack;
    std::function<void (Track&, int)> onOpenPlugin;
    std::function<void (Track&, int)> onRemovePlugin;

    void rebuild();
    void tick();  // ~30 Hz from the main timer

    // editing commands
    void deleteSelectedClip();
    void splitAtPlayhead();
    void zoomBy (double factor, int anchorX = -1);
    void importAudio (const juce::Array<juce::File>& files, int trackIndex, double beat);

    int getSelectedTrack() const { return selectedTrack; }
    void selectTrack (int i);
    bool snapEnabled = true;

    // geometry helpers (x relative to lane area)
    static constexpr int headerWidth = 250;
    static constexpr int rulerHeight = 24;
    static constexpr int trackHeight = 86;
    double xToBeat (double x) const { return scrollBeats + x / pixelsPerBeat; }
    double beatToX (double b) const { return (b - scrollBeats) * pixelsPerBeat; }
    double snapBeat (double b, bool force = false) const;

    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    struct Selection
    {
        Track* track = nullptr;
        AudioClip* audio = nullptr;
        MidiClip* midi = nullptr;
        bool any() const { return audio != nullptr || midi != nullptr; }
    } selection;

private:
    friend class LaneCanvas;
    friend class RulerBar;
    friend class TrackHeader;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scrollBarMoved (juce::ScrollBar*, double) override;
    void updateScrollBars();
    void layoutHeaders();
    int totalTracksHeight() const;

    Engine& engine;
    double pixelsPerBeat = 36.0;
    double scrollBeats = 0.0;
    int scrollY = 0;
    int selectedTrack = -1;

    std::unique_ptr<juce::Component> ruler, canvas;
    juce::Component headerHolder;
    juce::OwnedArray<juce::Component> headers;
    juce::Array<Track*> headerTracks;
    juce::ScrollBar vbar { true }, hbar { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrangeView)
};

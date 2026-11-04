#pragma once
#include "Engine.h"

class PianoRollEditor;

class PianoRollWindow : public juce::DocumentWindow
{
public:
    PianoRollWindow (Engine&, MidiClip&, std::function<void (PianoRollWindow*)> onClose);
    ~PianoRollWindow() override;
    void closeButtonPressed() override;
    MidiClip* getClip() const { return clip; }

private:
    MidiClip* clip;
    std::function<void (PianoRollWindow*)> closeCallback;
};

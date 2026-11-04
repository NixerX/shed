#pragma once
#include "Engine.h"
#include "ArrangeView.h"
#include "PianoRoll.h"

class PluginWindow;

class MainComponent : public juce::Component,
                      public juce::MenuBarModel,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    explicit MainComponent (juce::ApplicationProperties&);
    ~MainComponent() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int, const juce::String&) override;
    void menuItemSelected (int id, int) override;

    void requestQuit();
    void loadFrom (const juce::File&);
    std::function<void (const juce::String&)> onTitleChange;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void newProject();
    void openProject();
    void saveProject (bool saveAs, std::function<void()> then = {});
    void importAudio();
    void showExportDialog();
    void showAudioSettings();
    void showPluginManager();
    void chooseLamePath();
    void showShortcuts();
    juce::File findLame() const;

    void openPianoRoll (MidiClip*);
    void closePianoRollFor (MidiClip*);
    void openPluginWindow (Track&, int slot);
    void closePluginWindowFor (juce::AudioPluginInstance*);
    void removePlugin (Track&, int slot);
    void deleteTrack (int index);
    void closeAllWindows();
    void updateTitle();

    juce::ApplicationProperties& appProps;
    Engine engine;
    juce::MenuBarComponent menuBar;
    ArrangeView arrange;

    juce::TextButton rewindB { "|<" }, playB { "Play" }, stopB { "Stop" }, recB { "Rec" }, clickB { "Click" }, snapB { "Snap" };
    juce::Label timeLabel, bpmLabel, masterLabel, cpuLabel;
    juce::Slider bpmSlider, masterSlider;
    float masterMeterL = 0, masterMeterR = 0;

    juce::OwnedArray<PianoRollWindow> pianoRolls;
    juce::OwnedArray<PluginWindow> pluginWindows;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::AlertWindow> dialog;
    void confirmDiscard (std::function<void()> then);
    juce::File projectFile;
    juce::TooltipWindow tooltips { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

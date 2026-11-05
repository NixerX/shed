#include "MainComponent.h"

using namespace juce;

enum MenuIds
{
    idNew = 1, idOpen, idSave, idSaveAs, idImport, idExport, idQuit,
    idAddAudio = 20, idAddMidi, idDeleteTrack,
    idSplit = 30, idDeleteClip, idSnap, idZoomIn, idZoomOut,
    idAudioSettings = 40, idPluginManager, idLamePath,
    idShortcuts = 50
};

//==============================================================================
class PluginWindow : public DocumentWindow
{
public:
    PluginWindow (AudioPluginInstance& p, const String& trackName, std::function<void (PluginWindow*)> onClose)
        : DocumentWindow (trackName + " - " + p.getName(), Colour (0xff2a2a2f), DocumentWindow::closeButton),
          plugin (p), closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        AudioProcessorEditor* ed = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
        if (ed == nullptr)
            ed = new GenericAudioProcessorEditor (p);
        setContentOwned (ed, true);
        setResizable (ed->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    ~PluginWindow() override { clearContentComponent(); }
    void closeButtonPressed() override { if (closeCallback) closeCallback (this); }

    AudioPluginInstance& plugin;
    std::function<void (PluginWindow*)> closeCallback;
};

//==============================================================================
class ExportJob : public ThreadWithProgressWindow
{
public:
    struct Job
    {
        std::vector<Engine::RenderTarget> targets;
        int track = -1;
        String label;
    };

    ExportJob (Engine& e, std::vector<Job> j, int quality, File lameExe, File folder)
        : ThreadWithProgressWindow ("Exporting", true, true),
          engine (e), jobs (std::move (j)), mp3Quality (quality), lame (std::move (lameExe)), outFolder (std::move (folder)) {}

    void run() override
    {
        for (size_t k = 0; k < jobs.size(); ++k)
        {
            if (threadShouldExit()) { cancelled = true; return; }
            setStatusMessage ("Rendering " + jobs[k].label + " (" + String ((int) k + 1) + "/" + String ((int) jobs.size()) + ")");
            String err;
            const bool ok = engine.render (jobs[k].targets, jobs[k].track, mp3Quality, lame,
                                           [this, k] (double p)
                                           {
                                               setProgress ((k + p) / (double) jobs.size());
                                               return ! threadShouldExit();
                                           }, err);
            if (! ok)
            {
                error = err;
                return;
            }
            written += (int) jobs[k].targets.size();
        }
    }

    void threadComplete (bool userPressedCancel) override
    {
        if (userPressedCancel || cancelled)
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, "Export", "Export cancelled.");
        else if (error.isNotEmpty())
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Export failed", error);
        else
        {
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, "Export done",
                                              String (written) + " file(s) written to\n" + outFolder.getFullPathName());
            outFolder.revealToUser();
        }
        delete this;
    }

    Engine& engine;
    std::vector<Job> jobs;
    int mp3Quality;
    File lame, outFolder;
    String error;
    bool cancelled = false;
    int written = 0;
};

//==============================================================================
static void noFocus (Component& c)
{
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

MainComponent::MainComponent (ApplicationProperties& props) : appProps (props), arrange (engine)
{
    engine.initialise (appProps.getUserSettings());
    engine.getDeviceManager().addChangeListener (this);
    engine.getKnownPlugins().addChangeListener (this);
    engine.addChangeListener (this);

    menuBar.setModel (this);
   #if ! JUCE_MAC
    addAndMakeVisible (menuBar);
   #endif

    auto setupButton = [this] (TextButton& b, const String& tip, std::function<void()> fn)
    {
        b.setTooltip (tip);
        b.onClick = std::move (fn);
        noFocus (b);
        addAndMakeVisible (b);
    };
    setupButton (rewindB, "Go to start (Home)", [this] { engine.setPosition (0); });
    setupButton (playB, "Play / pause (Space)", [this] { if (engine.isPlaying()) engine.stop(); else engine.play(); });
    setupButton (stopB, "Stop and return to start", [this] { const bool was = engine.isPlaying(); engine.stop(); if (! was) engine.setPosition (0); });
    setupButton (recB, "Record armed tracks (R)", [this] { engine.toggleRecord(); });
    setupButton (clickB, "Metronome (C)", [this] { engine.metronomeOn = clickB.getToggleState(); });
    setupButton (snapB, "Snap to grid (hold Shift while dragging to bypass)", [this] { arrange.snapEnabled = snapB.getToggleState(); });
    clickB.setClickingTogglesState (true);
    snapB.setClickingTogglesState (true);
    snapB.setToggleState (true, dontSendNotification);
    recB.setColour (TextButton::buttonOnColourId, Colours::red);
    playB.setColour (TextButton::buttonOnColourId, Colour (0xff2e8b57));
    clickB.setColour (TextButton::buttonOnColourId, Colour (0xff3a6fd9));
    snapB.setColour (TextButton::buttonOnColourId, Colour (0xff3a6fd9));

    timeLabel.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 18.0f, Font::bold));
    timeLabel.setColour (Label::textColourId, Colour (0xff8fe388));
    timeLabel.setColour (Label::backgroundColourId, Colour (0xff111114));
    timeLabel.setJustificationType (Justification::centred);
    addAndMakeVisible (timeLabel);

    bpmLabel.setText ("BPM", dontSendNotification);
    bpmLabel.setJustificationType (Justification::centredRight);
    addAndMakeVisible (bpmLabel);
    bpmSlider.setSliderStyle (Slider::LinearBar);
    bpmSlider.setRange (20.0, 400.0, 0.1);
    bpmSlider.setValue (engine.getBpm(), dontSendNotification);
    bpmSlider.setTooltip ("Tempo - drag or double-click to type");
    bpmSlider.onValueChange = [this] { engine.setBpm (bpmSlider.getValue()); arrange.repaint(); };
    bpmSlider.setMouseClickGrabsKeyboardFocus (false);
    addAndMakeVisible (bpmSlider);

    masterLabel.setText ("Master", dontSendNotification);
    masterLabel.setJustificationType (Justification::centredRight);
    addAndMakeVisible (masterLabel);
    masterSlider.setSliderStyle (Slider::LinearHorizontal);
    masterSlider.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
    masterSlider.setRange (-60.0, 12.0, 0.1);
    masterSlider.setSkewFactorFromMidPoint (-12.0);
    masterSlider.setDoubleClickReturnValue (true, 0.0);
    masterSlider.setPopupDisplayEnabled (true, true, nullptr);
    masterSlider.setTextValueSuffix (" dB");
    masterSlider.onValueChange = [this] { engine.masterGainDb = (float) masterSlider.getValue(); };
    noFocus (masterSlider);
    addAndMakeVisible (masterSlider);

    cpuLabel.setFont (FontOptions (11.0f));
    cpuLabel.setColour (Label::textColourId, Colours::grey);
    addAndMakeVisible (cpuLabel);

    arrange.onOpenPianoRoll = [this] (MidiClip* c) { openPianoRoll (c); };
    arrange.onMidiClipAboutToBeDeleted = [this] (MidiClip* c) { closePianoRollFor (c); };
    arrange.onDeleteTrack = [this] (int i) { deleteTrack (i); };
    arrange.onOpenPlugin = [this] (Track& t, int s) { openPluginWindow (t, s); };
    arrange.onRemovePlugin = [this] (Track& t, int s) { removePlugin (t, s); };
    addAndMakeVisible (arrange);

    setWantsKeyboardFocus (true);

    // a friendly starting point
    engine.addTrack (TrackType::Audio);
    engine.addTrack (TrackType::Midi);
    arrange.selectTrack (0);

    setSize (1280, 780);
    updateTitle();
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    menuBar.setModel (nullptr);
    closeAllWindows();
    engine.getDeviceManager().removeChangeListener (this);
    engine.getKnownPlugins().removeChangeListener (this);
    engine.removeChangeListener (this);
    engine.shutdown();
}

//==============================================================================
void MainComponent::resized()
{
    auto r = getLocalBounds();
   #if ! JUCE_MAC
    menuBar.setBounds (r.removeFromTop (24));
   #endif
    auto bar = r.removeFromTop (46).reduced (8, 7);
    rewindB.setBounds (bar.removeFromLeft (38));
    bar.removeFromLeft (4);
    playB.setBounds (bar.removeFromLeft (60));
    bar.removeFromLeft (4);
    stopB.setBounds (bar.removeFromLeft (56));
    bar.removeFromLeft (4);
    recB.setBounds (bar.removeFromLeft (56));
    bar.removeFromLeft (12);
    timeLabel.setBounds (bar.removeFromLeft (250));
    bar.removeFromLeft (12);
    bpmLabel.setBounds (bar.removeFromLeft (40));
    bpmSlider.setBounds (bar.removeFromLeft (80));
    bar.removeFromLeft (8);
    clickB.setBounds (bar.removeFromLeft (56));
    bar.removeFromLeft (4);
    snapB.setBounds (bar.removeFromLeft (56));

    bar.removeFromRight (30); // master meter, painted
    masterSlider.setBounds (bar.removeFromRight (150));
    masterLabel.setBounds (bar.removeFromRight (56));
    cpuLabel.setBounds (bar.removeFromRight (90));

    arrange.setBounds (r);
}

void MainComponent::paint (Graphics& g)
{
    g.fillAll (Colour (0xff1c1c20));
    auto meter = Rectangle<int> (getWidth() - 34, (getHeight() - arrange.getHeight()) - 40, 22, 32);
    auto drawBar = [&] (Rectangle<int> b, float v)
    {
        const float db = Decibels::gainToDecibels (v, -60.0f);
        g.setColour (Colour (0xff0e0e10));
        g.fillRect (b);
        const int fill = (int) jmap (db, -60.0f, 6.0f, 0.0f, (float) b.getHeight());
        g.setColour (v > 1.0f ? Colours::red : (db > -6.0f ? Colours::yellow : Colour (0xff4cd964)));
        g.fillRect (b.withTop (b.getBottom() - fill));
    };
    drawBar (meter.removeFromLeft (10), masterMeterL);
    meter.removeFromLeft (2);
    drawBar (meter.removeFromLeft (10), masterMeterR);
}

void MainComponent::timerCallback()
{
    arrange.tick();

    const double sr = engine.getSampleRate();
    const double secs = engine.getPlayPosition() / sr;
    const double beats = engine.secondsToBeats (secs);
    const int bar = (int) (beats / 4.0) + 1;
    const int beat = (int) std::fmod (beats, 4.0) + 1;
    const int tick = (int) (std::fmod (beats, 1.0) * 100.0);
    const int mins = (int) (secs / 60.0);
    timeLabel.setText (String (bar) + "." + String (beat) + "." + String (tick).paddedLeft ('0', 2) + "   "
                           + String (mins) + ":" + String (std::fmod (secs, 60.0), 3).paddedLeft ('0', 6),
                       dontSendNotification);

    playB.setToggleState (engine.isPlaying(), dontSendNotification);
    recB.setToggleState (engine.isRecording(), dontSendNotification);
    playB.setButtonText (engine.isPlaying() ? "Pause" : "Play");

    if (std::abs (bpmSlider.getValue() - engine.getBpm()) > 0.01)
        bpmSlider.setValue (engine.getBpm(), dontSendNotification);

    static int cpuTick = 0;
    if (++cpuTick % 15 == 0)
    {
        auto* dev = engine.getDeviceManager().getCurrentAudioDevice();
        cpuLabel.setText (dev == nullptr ? "No audio device"
                                         : "DSP " + String (engine.getDeviceManager().getCpuUsage() * 100.0, 1) + "%",
                          dontSendNotification);
    }

    masterMeterL = jmax (engine.masterPeakL.exchange (0.0f), masterMeterL * 0.85f);
    masterMeterR = jmax (engine.masterPeakR.exchange (0.0f), masterMeterR * 0.85f);
    repaint (getWidth() - 40, 0, 40, getHeight() - arrange.getHeight());

    if (getPeer() != nullptr && getPeer()->isFocused() && Component::getCurrentlyFocusedComponent() == nullptr)
        grabKeyboardFocus();
}

void MainComponent::changeListenerCallback (ChangeBroadcaster* source)
{
    if (source == &engine.getDeviceManager())
    {
        if (auto xml = engine.getDeviceManager().createStateXml())
            appProps.getUserSettings()->setValue ("audioDevice", xml.get());
        appProps.saveIfNeeded();
    }
    else if (source == &engine.getKnownPlugins())
    {
        if (auto xml = engine.getKnownPlugins().createXml())
            appProps.getUserSettings()->setValue ("pluginList", xml.get());
        appProps.saveIfNeeded();
    }
    else if (source == &engine)
    {
        // close piano rolls whose clips have vanished (split, track deleted, etc.)
        for (int i = pianoRolls.size(); --i >= 0;)
            if (engine.findTrackForMidiClip (pianoRolls[i]->getClip()) == nullptr)
                pianoRolls.remove (i);
    }
}

bool MainComponent::keyPressed (const KeyPress& k)
{
    const auto mods = k.getModifiers();
    const int code = k.getKeyCode();
    const auto ch = CharacterFunctions::toLowerCase (k.getTextCharacter());

    if (mods.isCommandDown())
    {
        if (code == 'S' || ch == 's') { saveProject (mods.isShiftDown()); return true; }
        if (code == 'O' || ch == 'o') { openProject(); return true; }
        if (code == 'N' || ch == 'n') { newProject(); return true; }
        if (code == 'T' || ch == 't') { engine.addTrack (TrackType::Audio); return true; }
        if (code == 'M' || ch == 'm') { engine.addTrack (TrackType::Midi); return true; }
        if (code == 'E' || ch == 'e') { showExportDialog(); return true; }
        if (code == 'I' || ch == 'i') { importAudio(); return true; }
        return false;
    }

    if (code == KeyPress::spaceKey)  { if (engine.isPlaying()) engine.stop(); else engine.play(); return true; }
    if (code == KeyPress::homeKey)   { engine.setPosition (0); return true; }
    if (code == KeyPress::deleteKey || code == KeyPress::backspaceKey) { arrange.deleteSelectedClip(); return true; }
    if (ch == 'r') { engine.toggleRecord(); return true; }
    if (ch == 's') { arrange.splitAtPlayhead(); return true; }
    if (ch == 'c') { clickB.setToggleState (! clickB.getToggleState(), sendNotificationSync); return true; }
    if (ch == '=' || ch == '+') { arrange.zoomBy (1.3); return true; }
    if (ch == '-') { arrange.zoomBy (1.0 / 1.3); return true; }
    return false;
}

//==============================================================================
StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Track", "Options", "Help" };
}

PopupMenu MainComponent::getMenuForIndex (int index, const String&)
{
    PopupMenu m;
    if (index == 0)
    {
        m.addItem (idNew, "New project\tCtrl+N");
        m.addItem (idOpen, "Open project...\tCtrl+O");
        m.addItem (idSave, "Save\tCtrl+S");
        m.addItem (idSaveAs, "Save as...\tCtrl+Shift+S");
        m.addSeparator();
        m.addItem (idImport, "Import audio file...\tCtrl+I");
        m.addItem (idExport, "Export mix / stems...\tCtrl+E");
        m.addSeparator();
        m.addItem (idQuit, "Quit");
    }
    else if (index == 1)
    {
        m.addItem (idSplit, "Split at playhead\tS");
        m.addItem (idDeleteClip, "Delete selected clip\tDel");
        m.addItem (idSnap, "Snap to grid", true, arrange.snapEnabled);
        m.addSeparator();
        m.addItem (idZoomIn, "Zoom in\t+");
        m.addItem (idZoomOut, "Zoom out\t-");
    }
    else if (index == 2)
    {
        m.addItem (idAddAudio, "Add audio track\tCtrl+T");
        m.addItem (idAddMidi, "Add MIDI track\tCtrl+M");
        m.addSeparator();
        m.addItem (idDeleteTrack, "Delete selected track", arrange.getSelectedTrack() >= 0);
    }
    else if (index == 3)
    {
        m.addItem (idAudioSettings, "Audio / MIDI settings...");
        m.addItem (idPluginManager, "Plugin manager (scan VST3/LV2/AU)...");
        m.addItem (idLamePath, "Set LAME encoder path (for MP3)...");
    }
    else if (index == 4)
    {
        m.addItem (idShortcuts, "Keyboard shortcuts");
    }
    return m;
}

void MainComponent::menuItemSelected (int id, int)
{
    switch (id)
    {
        case idNew:           newProject(); break;
        case idOpen:          openProject(); break;
        case idSave:          saveProject (false); break;
        case idSaveAs:        saveProject (true); break;
        case idImport:        importAudio(); break;
        case idExport:        showExportDialog(); break;
        case idQuit:          requestQuit(); break;
        case idSplit:         arrange.splitAtPlayhead(); break;
        case idDeleteClip:    arrange.deleteSelectedClip(); break;
        case idSnap:          snapB.setToggleState (! snapB.getToggleState(), sendNotificationSync); break;
        case idZoomIn:        arrange.zoomBy (1.3); break;
        case idZoomOut:       arrange.zoomBy (1.0 / 1.3); break;
        case idAddAudio:      engine.addTrack (TrackType::Audio); break;
        case idAddMidi:       engine.addTrack (TrackType::Midi); break;
        case idDeleteTrack:   deleteTrack (arrange.getSelectedTrack()); break;
        case idAudioSettings: showAudioSettings(); break;
        case idPluginManager: showPluginManager(); break;
        case idLamePath:      chooseLamePath(); break;
        case idShortcuts:     showShortcuts(); break;
        default: break;
    }
}

//==============================================================================
void MainComponent::updateTitle()
{
    if (onTitleChange)
        onTitleChange ("Shed - " + (projectFile == File() ? String ("untitled") : projectFile.getFileNameWithoutExtension()));
}

void MainComponent::confirmDiscard (std::function<void()> then)
{
    if (engine.getTracks().isEmpty())
    {
        then();
        return;
    }

    AlertWindow::showAsync (MessageBoxOptions()
                                .withIconType (MessageBoxIconType::QuestionIcon)
                                .withTitle ("Shed")
                                .withMessage ("Save the current project first?")
                                .withButton ("Save")
                                .withButton ("Don't save")
                                .withButton ("Cancel")
                                .withAssociatedComponent (this),
                            [this, then] (int r)
                            {
                                if (r == 1) saveProject (false, then);
                                else if (r == 2) then();
                            });
}

void MainComponent::requestQuit()
{
    confirmDiscard ([] { JUCEApplication::getInstance()->quit(); });
}

void MainComponent::newProject()
{
    confirmDiscard ([this]
    {
        closeAllWindows();
        engine.clearProject();
        projectFile = File();
        bpmSlider.setValue (engine.getBpm(), dontSendNotification);
        masterSlider.setValue (0.0, dontSendNotification);
        updateTitle();
    });
}

void MainComponent::openProject()
{
    confirmDiscard ([this]
    {
        chooser = std::make_unique<FileChooser> ("Open project", projectFile.getParentDirectory(), "*.shed");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this] (const FileChooser& fc)
                              {
                                  auto f = fc.getResult();
                                  if (f.existsAsFile()) loadFrom (f);
                              });
    });
}

void MainComponent::loadFrom (const File& f)
{
    auto xml = XmlDocument::parse (f);
    if (xml == nullptr)
    {
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Open", "Couldn't parse " + f.getFileName());
        return;
    }

    closeAllWindows();
    StringArray problems;
    MouseCursor::showWaitCursor();
    engine.loadProject (*xml, problems);
    MouseCursor::hideWaitCursor();
    projectFile = f;
    bpmSlider.setValue (engine.getBpm(), dontSendNotification);
    masterSlider.setValue (engine.masterGainDb.load(), dontSendNotification);
    clickB.setToggleState (engine.metronomeOn.load(), dontSendNotification);
    updateTitle();

    if (! problems.isEmpty())
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Some things didn't load",
                                          problems.joinIntoString ("\n"));
}

void MainComponent::saveProject (bool saveAs, std::function<void()> then)
{
    auto write = [this, then] (const File& f)
    {
        auto xml = engine.saveProject();
        if (! xml->writeTo (f))
        {
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Save", "Couldn't write " + f.getFullPathName());
            return;
        }
        projectFile = f;
        updateTitle();
        if (then) then();
    };

    if (projectFile != File() && ! saveAs)
    {
        write (projectFile);
        return;
    }

    auto start = projectFile != File() ? projectFile
                                       : File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Shed").getChildFile ("Untitled.shed");
    start.getParentDirectory().createDirectory();
    chooser = std::make_unique<FileChooser> ("Save project", start, "*.shed");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting,
                          [write] (const FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f == File()) return;
                              write (f.withFileExtension (".shed"));
                          });
}

void MainComponent::importAudio()
{
    chooser = std::make_unique<FileChooser> ("Import audio", File(), engine.getAudioFormats().getWildcardForAllFormats());
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::canSelectMultipleItems,
                          [this] (const FileChooser& fc)
                          {
                              if (fc.getResults().isEmpty()) return;
                              const double beat = engine.secondsToBeats (engine.getPlayPosition() / engine.getSampleRate());
                              arrange.importAudio (fc.getResults(), arrange.getSelectedTrack(), beat);
                          });
}

//==============================================================================
File MainComponent::findLame() const
{
    File saved (appProps.getUserSettings()->getValue ("lamePath"));
    if (saved.existsAsFile()) return saved;

    StringArray candidates { "/usr/bin/lame", "/usr/local/bin/lame", "/opt/homebrew/bin/lame",
                             "C:\\Program Files\\LAME\\lame.exe", "C:\\Program Files (x86)\\LAME\\lame.exe" };
    candidates.add (File::getSpecialLocation (File::currentExecutableFile).getSiblingFile ("lame.exe").getFullPathName());
    candidates.add (File::getSpecialLocation (File::currentExecutableFile).getSiblingFile ("lame").getFullPathName());

    for (auto dir : StringArray::fromTokens (SystemStats::getEnvironmentVariable ("PATH", {}), File::getSeparatorChar() == '\\' ? ";" : ":", {}))
    {
        candidates.add (File (dir).getChildFile ("lame").getFullPathName());
        candidates.add (File (dir).getChildFile ("lame.exe").getFullPathName());
    }

    for (auto& c : candidates)
        if (File::isAbsolutePath (c) && File (c).existsAsFile())
            return File (c);
    return {};
}

void MainComponent::chooseLamePath()
{
    chooser = std::make_unique<FileChooser> ("Locate the lame executable", findLame());
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                          [this] (const FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (! f.existsAsFile()) return;
                              appProps.getUserSettings()->setValue ("lamePath", f.getFullPathName());
                              appProps.saveIfNeeded();
                          });
}

void MainComponent::showExportDialog()
{
    if (engine.getProjectEndSeconds() <= 0.0)
    {
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, "Export", "Nothing to export yet - the project is empty.");
        return;
    }
    if (engine.isPlaying()) engine.stop();

    struct Opts
    {
        ToggleButton mixWav { "Master mix - WAV (24-bit)" }, mixMp3 { "Master mix - MP3" },
                     stemsWav { "Stems - one WAV per track" }, stemsMp3 { "Stems - MP3 too" },
                     skipMuted { "Skip muted tracks when exporting stems" };
    };
    auto opts = std::make_shared<Opts>();
    for (auto* b : { &opts->mixWav, &opts->mixMp3, &opts->stemsWav, &opts->stemsMp3, &opts->skipMuted })
        b->setSize (320, 24);
    opts->mixWav.setToggleState (true, dontSendNotification);
    opts->mixMp3.setToggleState (true, dontSendNotification);
    opts->skipMuted.setToggleState (true, dontSendNotification);

    dialog = std::make_unique<AlertWindow> ("Export", "Render the project (from the start to the last clip, plus a 2 s tail).",
                                            MessageBoxIconType::NoIcon, this);
    dialog->addTextEditor ("name", projectFile != File() ? projectFile.getFileNameWithoutExtension() : String ("Mix"), "File name:");
    dialog->addCustomComponent (&opts->mixWav);
    dialog->addCustomComponent (&opts->mixMp3);
    dialog->addCustomComponent (&opts->stemsWav);
    dialog->addCustomComponent (&opts->stemsMp3);
    dialog->addCustomComponent (&opts->skipMuted);
    dialog->addComboBox ("quality", LAMEEncoderAudioFormat (File()).getQualityOptions(), "MP3 quality:");
    dialog->getComboBoxComponent ("quality")->setSelectedItemIndex (23);
    dialog->addButton ("Choose folder & export", 1, KeyPress (KeyPress::returnKey));
    dialog->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));

    dialog->enterModalState (true, ModalCallbackFunction::create ([this, opts] (int r)
    {
        const auto name = File::createLegalFileName (dialog->getTextEditorContents ("name").trim());
        const int quality = dialog->getComboBoxComponent ("quality")->getSelectedItemIndex();
        const bool mixWav = opts->mixWav.getToggleState(), mixMp3 = opts->mixMp3.getToggleState();
        const bool stemsWav = opts->stemsWav.getToggleState(), stemsMp3 = opts->stemsMp3.getToggleState();
        const bool skipMuted = opts->skipMuted.getToggleState();
        MessageManager::callAsync ([this, opts] { dialog.reset(); });

        if (r != 1 || ! (mixWav || mixMp3 || stemsWav || stemsMp3)) return;

        const bool needLame = mixMp3 || stemsMp3;
        const File lame = findLame();
        if (needLame && ! lame.existsAsFile())
        {
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "MP3 export",
                "MP3 export uses the free 'lame' encoder, which I couldn't find.\n\n"
                "Linux: sudo apt install lame\nmacOS: brew install lame\n"
                "Windows: put lame.exe next to Shed.exe\n\nOr point to it with Options > Set LAME encoder path.");
            return;
        }

        auto start = projectFile != File() ? projectFile.getParentDirectory()
                                           : File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Shed");
        start.createDirectory();
        chooser = std::make_unique<FileChooser> ("Export to folder", start);
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                              [this, name, quality, mixWav, mixMp3, stemsWav, stemsMp3, skipMuted, lame] (const FileChooser& fc)
        {
            auto folder = fc.getResult();
            if (! folder.isDirectory()) return;
            const auto base = name.isEmpty() ? String ("Mix") : name;

            std::vector<ExportJob::Job> jobs;
            if (mixWav || mixMp3)
            {
                ExportJob::Job j;
                j.label = "master mix";
                if (mixWav) j.targets.push_back ({ folder.getChildFile (base + ".wav"), false });
                if (mixMp3) j.targets.push_back ({ folder.getChildFile (base + ".mp3"), true });
                jobs.push_back (j);
            }
            if (stemsWav || stemsMp3)
            {
                auto& tracks = engine.getTracks();
                for (int i = 0; i < tracks.size(); ++i)
                {
                    auto& t = *tracks[i];
                    if (t.audioClips.isEmpty() && t.midiClips.isEmpty()) continue;
                    if (skipMuted && t.mute) continue;
                    const auto stem = base + " - " + String (i + 1).paddedLeft ('0', 2) + " " + File::createLegalFileName (t.name);
                    ExportJob::Job j;
                    j.track = i;
                    j.label = t.name;
                    if (stemsWav) j.targets.push_back ({ folder.getChildFile (stem + ".wav"), false });
                    if (stemsMp3) j.targets.push_back ({ folder.getChildFile (stem + ".mp3"), true });
                    jobs.push_back (j);
                }
            }
            if (jobs.empty()) return;
            (new ExportJob (engine, std::move (jobs), quality, lame, folder))->launchThread();
        });
    }), false);
}

void MainComponent::showAudioSettings()
{
    auto* sel = new AudioDeviceSelectorComponent (engine.getDeviceManager(), 0, 64, 0, 2, true, false, true, false);
    sel->setSize (560, 560);
    DialogWindow::LaunchOptions o;
    o.content.setOwned (sel);
    o.dialogTitle = "Audio / MIDI Settings";
    o.dialogBackgroundColour = Colour (0xff2a2a2f);
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.launchAsync();
}

void MainComponent::showPluginManager()
{
    auto deadMansPedal = appProps.getUserSettings()->getFile().getSiblingFile ("RecentlyCrashedPluginsList");
    auto* list = new PluginListComponent (engine.getFormatManager(), engine.getKnownPlugins(), deadMansPedal,
                                          appProps.getUserSettings(), true);
    list->setSize (720, 520);
    DialogWindow::LaunchOptions o;
    o.content.setOwned (list);
    o.dialogTitle = "Plugin Manager - use 'Options...' > 'Scan for new or updated ... plug-ins'";
    o.dialogBackgroundColour = Colour (0xff2a2a2f);
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.launchAsync();
}

void MainComponent::showShortcuts()
{
    AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, "Shortcuts",
        "Space - play / pause\n"
        "R - record armed tracks\n"
        "Home - go to start\n"
        "S - split clip(s) at playhead\n"
        "Del - delete selected clip\n"
        "C - metronome on/off\n"
        "+ / - (or Ctrl+wheel) - zoom\n"
        "Shift+wheel - scroll horizontally\n"
        "Shift while dragging - ignore snap\n\n"
        "Double-click an empty MIDI lane - new clip\n"
        "Double-click a MIDI clip - piano roll\n"
        "Drag a clip's right edge - trim / extend\n"
        "Drop audio files onto the lanes to import\n\n"
        "Ctrl+T / Ctrl+M - add audio / MIDI track\n"
        "Ctrl+S save, Ctrl+O open, Ctrl+E export, Ctrl+I import");
}

//==============================================================================
void MainComponent::openPianoRoll (MidiClip* c)
{
    if (c == nullptr) return;
    for (auto* w : pianoRolls)
        if (w->getClip() == c) { w->toFront (true); return; }

    pianoRolls.add (new PianoRollWindow (engine, *c, [safe = SafePointer<MainComponent> (this)] (PianoRollWindow* w)
    {
        MessageManager::callAsync ([safe, w]
        {
            if (safe != nullptr) safe->pianoRolls.removeObject (w);
        });
    }));
}

void MainComponent::closePianoRollFor (MidiClip* c)
{
    for (int i = pianoRolls.size(); --i >= 0;)
        if (pianoRolls[i]->getClip() == c)
            pianoRolls.remove (i);
}

void MainComponent::openPluginWindow (Track& t, int slot)
{
    if (! isPositiveAndBelow (slot, (int) t.plugins.size()) || t.plugins[(size_t) slot].instance == nullptr) return;
    auto* inst = t.plugins[(size_t) slot].instance.get();

    for (auto* w : pluginWindows)
        if (&w->plugin == inst) { w->toFront (true); return; }

    pluginWindows.add (new PluginWindow (*inst, t.name, [safe = SafePointer<MainComponent> (this)] (PluginWindow* w)
    {
        MessageManager::callAsync ([safe, w]
        {
            if (safe != nullptr) safe->pluginWindows.removeObject (w);
        });
    }));
}

void MainComponent::closePluginWindowFor (AudioPluginInstance* inst)
{
    for (int i = pluginWindows.size(); --i >= 0;)
        if (&pluginWindows[i]->plugin == inst)
            pluginWindows.remove (i);
}

void MainComponent::removePlugin (Track& t, int slot)
{
    if (! isPositiveAndBelow (slot, (int) t.plugins.size())) return;
    closePluginWindowFor (t.plugins[(size_t) slot].instance.get());
    auto doomed = engine.takePlugin (t, slot);
    doomed.reset();
}

void MainComponent::deleteTrack (int index)
{
    auto& tracks = engine.getTracks();
    if (! isPositiveAndBelow (index, tracks.size())) return;
    auto& t = *tracks[index];
    for (auto* c : t.midiClips) closePianoRollFor (c);
    for (auto& s : t.plugins) closePluginWindowFor (s.instance.get());
    arrange.selection = {};
    engine.removeTrack (index);
}

void MainComponent::closeAllWindows()
{
    pianoRolls.clear();
    pluginWindows.clear();
}

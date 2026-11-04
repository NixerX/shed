#include "MainComponent.h"

using namespace juce;

class ShedLookAndFeel : public LookAndFeel_V4
{
public:
    ShedLookAndFeel() : LookAndFeel_V4 (makeScheme())
    {
        setColour (TextButton::buttonColourId, Colour (0xff3b3b42));
        setColour (ComboBox::backgroundColourId, Colour (0xff3b3b42));
        setColour (Slider::thumbColourId, Colour (0xffd0d0d8));
        setColour (Slider::trackColourId, Colour (0xff5b9bd5));
        setColour (Slider::rotarySliderFillColourId, Colour (0xff5b9bd5));
        setColour (Slider::rotarySliderOutlineColourId, Colour (0xff5a5a64));
        setColour (Slider::backgroundColourId, Colour (0xff4a4a52));
        setColour (PopupMenu::backgroundColourId, Colour (0xff2c2c31));
    }

    static ColourScheme makeScheme()
    {
        auto s = LookAndFeel_V4::getDarkColourScheme();
        s.setUIColour (ColourScheme::windowBackground, Colour (0xff2a2a2f));
        s.setUIColour (ColourScheme::widgetBackground, Colour (0xff3b3b42));
        s.setUIColour (ColourScheme::menuBackground, Colour (0xff2c2c31));
        s.setUIColour (ColourScheme::outline, Colour (0xff4a4a52));
        s.setUIColour (ColourScheme::defaultFill, Colour (0xff5b9bd5));
        s.setUIColour (ColourScheme::highlightedFill, Colour (0xff5b9bd5));
        return s;
    }
};

class ShedApplication : public JUCEApplication
{
public:
    const String getApplicationName() override { return "Shed"; }
    const String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const String& commandLine) override
    {
        LookAndFeel::setDefaultLookAndFeel (&lnf);

        PropertiesFile::Options o;
        o.applicationName = "Shed";
        o.filenameSuffix = ".settings";
        o.folderName = "Shed";
        o.osxLibrarySubFolder = "Application Support";
        props.setStorageParameters (o);

        window = std::make_unique<MainWindow> (props);

        auto arg = commandLine.unquoted().trim();
        if (arg.endsWithIgnoreCase (".shed") && File::isAbsolutePath (arg) && File (arg).existsAsFile())
            window->openOnStart (File (arg));
    }

    void shutdown() override
    {
        window.reset();
        props.closeFiles();
        LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        if (window != nullptr) window->content->requestQuit();
        else quit();
    }

    class MainWindow : public DocumentWindow
    {
    public:
        explicit MainWindow (ApplicationProperties& props)
            : DocumentWindow ("Shed", Colour (0xff1c1c20), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            content = new MainComponent (props);
            content->onTitleChange = [this] (const String& t) { setName (t); };
            setContentOwned (content, true);
           #if JUCE_MAC
            MenuBarModel::setMacMainMenu (content);
           #endif
            setResizable (true, true);
            setResizeLimits (900, 500, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
            content->grabKeyboardFocus();
            setName ("Shed - untitled");
        }

        ~MainWindow() override
        {
           #if JUCE_MAC
            MenuBarModel::setMacMainMenu (nullptr);
           #endif
            clearContentComponent();
        }

        void openOnStart (const File& f) { content->loadFrom (f); }

        void closeButtonPressed() override { content->requestQuit(); }

        MainComponent* content = nullptr;
    };

private:
    ShedLookAndFeel lnf;
    ApplicationProperties props;
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (ShedApplication)

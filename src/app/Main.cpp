#include <juce_gui_extra/juce_gui_extra.h>

#include "EngineSetup.h"
#include "MainComponent.h"

namespace orionish::app
{

class Application : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override     { return "Orionish TE"; }
    const juce::String getApplicationVersion() override  { return "0.1.0"; }

    void initialise (const juce::String&) override
    {
        engine = orionish::createEngine ("Orionish TE");
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine);
    }

    void shutdown() override
    {
        mainWindow.reset();   // destroys the Edit before the Engine
        engine.reset();
    }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, te::Engine& engineToUse)
            : DocumentWindow (name,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (backgroundColourId),
                              allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (engineToUse), true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace orionish::app

START_JUCE_APPLICATION (orionish::app::Application)

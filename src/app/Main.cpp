#include <juce_gui_extra/juce_gui_extra.h>

#include "EngineSetup.h"
#include "MainComponent.h"

namespace carve::app
{

class Application : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override     { return "Carve DAW"; }
    const juce::String getApplicationVersion() override  { return "0.1.0"; }

    void initialise (const juce::String& commandLine) override
    {
        // Plugin scans run in a child process, which is this same executable
        // re-launched with a command line the engine recognises. This has to
        // come first: a scanner process must not start an engine or open a
        // window. Paired with canScanPluginsOutOfProcess() in EngineSetup.h.
        if (te::PluginManager::startChildProcessPluginScan (commandLine))
        {
            juce::Process::setDockIconVisible (false);
            return;
        }

        engine = carve::createEngine (nullptr, true);
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine);
    }

    void shutdown() override
    {
        mainWindow.reset();   // destroys the Edit before the Engine
        engine.reset();
    }

    // Cmd-Q, the Quit menu item and the window's close button all arrive here.
    // Unsaved work is worth a question before any of them takes it away, and
    // the answer is asynchronous -- so quitting waits for it rather than
    // happening now.
    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
        {
            if (auto* main = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
            {
                main->confirmDiscardChanges ([this] (bool goAhead)
                {
                    if (goAhead)
                        quit();
                });

                return;
            }
        }

        quit();
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

} // namespace carve::app

START_JUCE_APPLICATION (carve::app::Application)

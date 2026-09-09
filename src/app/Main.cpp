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

       #if JUCE_MAC
        menu = std::make_unique<Menu> (*this);
        juce::MenuBarModel::setMacMainMenu (menu.get());
       #endif
    }

    void shutdown() override
    {
       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (nullptr);
        menu.reset();
       #endif

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
    MainComponent* getMainComponent() const
    {
        return mainWindow != nullptr ? dynamic_cast<MainComponent*> (mainWindow->getContentComponent())
                                     : nullptr;
    }

    // The menu bar, which on macOS is the one at the top of the screen rather
    // than in the window. Every item is one of MainComponent's actions, the
    // same ones the logo menu and the shortcuts reach; the shortcuts shown
    // beside the items are handled by the component's keyPressed, so the
    // menu only names them. Undo and Redo live here as well as on the bar
    // because macOS users look for them in Edit.
    class Menu : public juce::MenuBarModel
    {
    public:
        explicit Menu (Application& app) : owner (app) {}

        juce::StringArray getMenuBarNames() override  { return { "File", "Edit", "View" }; }

        juce::PopupMenu getMenuForIndex (int index, const juce::String&) override
        {
            using Action = MainComponent::Action;
            juce::PopupMenu m;

            auto add = [&m] (Action action, const juce::String& name, const juce::String& shortcut = {},
                             bool ticked = false)
            {
                juce::PopupMenu::Item item (name);
                item.itemID = id (action);
                item.shortcutKeyDescription = shortcut;
                item.isTicked = ticked;
                m.addItem (std::move (item));
            };

            if (index == 0)
            {
                add (Action::newSong, "New");
                add (Action::openSong, "Open...");
                add (Action::openDemoSong, "Open Demo Song");
                m.addSeparator();
                add (Action::save, "Save", "Cmd+S");
                add (Action::saveAs, "Save As...", "Shift+Cmd+S");
                m.addSeparator();
                add (Action::exportWav, "Export WAV...", "Cmd+E");
            }
            else if (index == 1)
            {
                add (Action::undo, "Undo", "Cmd+Z");
                add (Action::redo, "Redo", "Shift+Cmd+Z");
            }
            else if (index == 2)
            {
                const auto main = owner.getMainComponent();
                add (Action::toggleBrowser, "Sample Browser", "Cmd+B", main != nullptr && main->isBrowserShown());
                add (Action::openMixer, "Mixer");
                add (Action::openPatternEditor, "Pattern Editor");
                m.addSeparator();
                add (Action::pluginManager, "Plugins...");
            }

            return m;
        }

        void menuItemSelected (int itemID, int) override
        {
            if (auto main = owner.getMainComponent())
                main->perform (static_cast<MainComponent::Action> (itemID - 1));
        }

    private:
        // Menu ids must be non-zero, hence the offset.
        static int id (MainComponent::Action action)  { return static_cast<int> (action) + 1; }

        Application& owner;
    };

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
    std::unique_ptr<Menu> menu;
};

} // namespace carve::app

START_JUCE_APPLICATION (carve::app::Application)

#include <juce_gui_extra/juce_gui_extra.h>

#include "EngineSetup.h"
#include "Fonts.h"
#include "LegalNotices.h"
#include "MainComponent.h"
#include "SandboxAccess.h"
#include "TouchUi.h"
#include "model/SampleSongs.h"

namespace carve::app
{

class Application : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override     { return "Carve DAW"; }
    // From the build rather than typed here, where it went stale: this said
    // 0.1.0 while the project was at 0.11.0.
    const juce::String getApplicationVersion() override  { return JUCE_APPLICATION_VERSION_STRING; }

    void initialise (const juce::String& commandLine) override
    {
        // Plugin scans run in a child process, which is this same executable
        // re-launched with a command line the engine recognises. This has to
        // come first: a scanner process must not start an engine or open a
        // window. Paired with canScanPluginsOutOfProcess() in EngineSetup.h.
        //
        // A build that hosts no plugins never scans, so it never relaunches
        // itself -- which the App Sandbox would have something to say about.
       #if CARVE_PLUGIN_HOSTING
        if (te::PluginManager::startChildProcessPluginScan (commandLine))
        {
            // The scanner is this same executable launched again, so on macOS
            // it would take a Dock icon of its own. The call exists only there.
           #if JUCE_MAC
            juce::Process::setDockIconVisible (false);
           #endif
            return;
        }
       #else
        juce::ignoreUnused (commandLine);
       #endif

        // Before any window exists: it decides how every label, menu and
        // button in them is drawn (see Fonts.h).
        juce::Desktop::getInstance().setDefaultLookAndFeel (&lookAndFeel);

        // Before the engine and before any window: both read paths the user
        // gave the app on an earlier run (recent songs, the browser's
        // folders), and inside the App Sandbox those are out of reach until
        // this claims them back. Nothing happens outside the sandbox.
        carve::sandbox::restoreAccess (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                           .getChildFile (carve::applicationName)
                                           .getChildFile ("FileAccess.xml"));
        carve::model::FileRef::onFileReferenced() = [] (const juce::File& file) { carve::sandbox::rememberAccess (file); };

        engine = carve::createEngine (nullptr, CARVE_PLUGIN_HOSTING != 0);
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine);

        // For the logo menu, which is where About lives when there is no
        // application menu (see TransportBar::showFileMenu).
        if (auto* main = getMainComponent())
        {
            main->onShowAbout = [this] { showAbout(); };
            main->onShowNotices = [this] { showNotices(); };
        }

       #if JUCE_MAC
        menu = std::make_unique<Menu> (*this);

        // About, in the Apple menu where macOS puts it. Not decoration: the
        // AGPL build has to say who holds the copyright, that there is no
        // warranty, that it may be redistributed under that licence, and where
        // to read it. The same window carries the "Powered by Tracktion Engine"
        // line the Personal licence asks for. Notices are the third-party list.
        juce::PopupMenu appleMenu;
        appleMenu.addItem (Menu::aboutItemID, "About " + getApplicationName());
        appleMenu.addItem (Menu::noticesItemID, "Third-Party Notices...");
        appleMenu.addSeparator();

        // Where macOS users look for it, shortcut and all. It is in the View
        // menu too, because that is where everything else the app opens is.
        juce::PopupMenu::Item settingsItem ("Settings...");
        settingsItem.itemID = Menu::settingsItemID;
        settingsItem.shortcutKeyDescription = "Cmd+,";
        appleMenu.addItem (std::move (settingsItem));

        juce::MenuBarModel::setMacMainMenu (menu.get(), &appleMenu);
       #endif
    }

    void shutdown() override
    {
       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (nullptr);
        menu.reset();
       #endif

        // Before the windows that hold them. A deferred close posted by
        // either window must not touch this object once shutdown has begun.
        *appAlive = false;
        aboutWindow.reset();
        noticesWindow.reset();

        mainWindow.reset();   // destroys the Edit before the Engine
        engine.reset();

        juce::Desktop::getInstance().setDefaultLookAndFeel (nullptr);
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
    // The About window and the third-party notices. Deferred close, same as
    // the other windows: the button runs while the window is still on screen,
    // so destroying it is the next turn's job. appAlive is cleared in shutdown
    // so a turn that arrives after that does not touch this object.
    void showAbout()
    {
        if (aboutWindow != nullptr)
        {
            aboutWindow->toFront (true);
            return;
        }

        auto alive = appAlive;

        aboutWindow = std::make_unique<AboutWindow> (getApplicationName(),
                                                     getApplicationVersion(),
                                                     [this] { showNotices(); },
                                                     [this, alive]
                                                     {
                                                         juce::MessageManager::callAsync ([this, alive]
                                                         {
                                                             if (*alive)
                                                                 aboutWindow.reset();
                                                         });
                                                     });
    }

    void showNotices()
    {
        if (noticesWindow != nullptr)
        {
            noticesWindow->toFront (true);
            return;
        }

        auto alive = appAlive;

        noticesWindow = std::make_unique<NoticesWindow> ([this, alive]
        {
            juce::MessageManager::callAsync ([this, alive]
            {
                if (*alive)
                    noticesWindow.reset();
            });
        });
    }

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

                // Same list the logo menu shows, asked for as the menu opens.
                juce::PopupMenu recentMenu;

                if (const auto main = owner.getMainComponent())
                {
                    const auto recent = main->recentSongNames();

                    for (int i = 0; i < recent.size(); ++i)
                        recentMenu.addItem (firstRecentItemID + i, recent[i]);

                    m.addSubMenu ("Open Recent", recentMenu, ! recent.isEmpty());
                }

                add (Action::openDemoSong, "Open Demo Song");

                // The songs that ship with the app, which the model names.
                juce::PopupMenu sampleMenu;
                const auto samples = model::sampleSongNames();

                for (int i = 0; i < samples.size(); ++i)
                    sampleMenu.addItem (firstSampleSongItemID + i, samples[i]);

                m.addSubMenu ("Open Sample Song", sampleMenu, ! samples.isEmpty());
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
                add (Action::settings, "Settings...", "Cmd+,");
            }

            return m;
        }

        // The Apple menu's own items, which arrive here like any other.
        static constexpr int aboutItemID = 9000;
        static constexpr int settingsItemID = 9001;
        static constexpr int noticesItemID = 9002;

        void menuItemSelected (int itemID, int) override
        {
            if (itemID == aboutItemID)
            {
                owner.showAbout();
                return;
            }

            if (itemID == noticesItemID)
            {
                owner.showNotices();
                return;
            }

            if (itemID == settingsItemID)
            {
                if (auto main = owner.getMainComponent())
                    main->perform (MainComponent::Action::settings);

                return;
            }

            if (auto main = owner.getMainComponent())
            {
                if (itemID >= firstSampleSongItemID)
                    main->openSampleSong (itemID - firstSampleSongItemID);
                else if (itemID >= firstRecentItemID)
                    main->openRecentSong (itemID - firstRecentItemID);
                else
                    main->perform (static_cast<MainComponent::Action> (itemID - 1));
            }
        }

    private:
        // Menu ids must be non-zero, hence the offset.
        static int id (MainComponent::Action action)  { return static_cast<int> (action) + 1; }

        // Above every id() an Action can take, so Open Recent's items and the
        // fixed ones share the one callback. The recents are capped at ten,
        // and the sample songs start far enough above them that neither list
        // has to know the other's length.
        static constexpr int firstRecentItemID = 100;
        static constexpr int firstSampleSongItemID = 200;

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

           #if JUCE_IOS
            // The window is the screen; there is nothing to drag or resize.
            setFullScreen (true);
           #else
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
           #endif

            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

       #if JUCE_IOS
        // The window is the whole screen, and the screen's edges are not all
        // the app's: the status bar, the home indicator, and corners that are
        // round. The content stays inside those, and what is left over is the
        // window's own background.
        juce::BorderSize<int> getContentComponentBorder() const override
        {
            auto border = juce::DocumentWindow::getContentComponentBorder();

            if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            {
                const auto safe = display->safeAreaInsets;

                // At least the corner's worth at the sides even where the
                // system reports nothing to avoid.
                border = { border.getTop() + juce::jmax (safe.getTop(), 4),
                           border.getLeft() + juce::jmax (safe.getLeft(), 8),
                           border.getBottom() + juce::jmax (safe.getBottom(), 4),
                           border.getRight() + juce::jmax (safe.getRight(), 8) };
            }

            return border;
        }
       #endif
    };

    // First, so it outlives the windows that are drawn with it.
    CarveLookAndFeel lookAndFeel;

    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<Menu> menu;

    // See showAbout. Shared so the deferred close still has the flag after
    // this object has started to go.
    std::shared_ptr<bool> appAlive = std::make_shared<bool> (true);
    std::unique_ptr<AboutWindow> aboutWindow;
    std::unique_ptr<NoticesWindow> noticesWindow;
};

} // namespace carve::app

START_JUCE_APPLICATION (carve::app::Application)

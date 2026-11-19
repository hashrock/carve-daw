#pragma once

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace carve::app
{

// The sounds Carve offers to download: UTAU voicebanks and SoundFonts whose
// authors allow them to be passed on, served from the carve-library Worker
// (library-server/) out of R2.
//
// The list of them is library.json, which names each pack's version, size,
// URL and SHA-256. A pack is fetched to a .part file (resumed with a Range
// request if a previous attempt was cut short), checked against its size and
// hash, unpacked into a staging folder, and only then moved into place as
//
//     <root>/<id>/pack.json          what was installed (its library.json entry)
//     <root>/<id>/README-Carve.txt   credits and the author's terms, UTF-8
//     <root>/<id>/<entry>            the voicebank folder or the .sf2
//
// so an installed pack is always a whole one. The folder does not carry the
// version: a song that used a pack still finds it after an update.
//
// Everything public is for the message thread. Downloads run on threads of
// their own and report back through the ChangeBroadcaster.
class SoundLibrary : public juce::ChangeBroadcaster
{
public:
    enum class Kind { voicebank, soundFont };

    struct Pack
    {
        juce::String id;
        Kind kind = Kind::voicebank;
        juce::String name, nameEn, version, author, homepage, description, licenseSummary, credit;
        juce::String entry;      // the voicebank folder or .sf2, relative to the pack's folder
        juce::URL url, readmeUrl;
        juce::int64 size = 0, installedSize = 0;
        juce::String sha256;     // lower-case hex

        // "足立レイ (Adachi Rei)", or the one name when the two are the same.
        juce::String displayName() const;

        static std::optional<Pack> fromVar (const juce::var&);
        juce::var toVar() const;
    };

    enum class State
    {
        notInstalled,
        downloading,
        verifying,
        installing,
        installed,
        updateAvailable,   // installed, and the list has another version
        failed
    };

    struct Status
    {
        State state = State::notInstalled;
        double progress = 0.0;          // 0..1 while downloading
        juce::int64 bytesDone = 0;
        juce::String error;             // why, when failed
        juce::String installedVersion;  // when installed or updateAvailable
    };

    enum class ListState { loading, online, offline };

    // The folder everything is installed under, created if need be.
    static juce::File defaultRoot();

    // Where library.json is read from. CARVE_LIBRARY_URL at build time
    // overrides it.
    static juce::URL defaultListUrl();

    explicit SoundLibrary (juce::File root = defaultRoot(), juce::URL listUrl = defaultListUrl());
    ~SoundLibrary() override;

    // Fetches library.json again. Until it arrives (or if it cannot), the
    // last copy that did arrive is used, so the installed packs are listed
    // and usable offline.
    void refreshList();
    ListState getListState() const        { return listState; }
    juce::String getListError() const     { return listError; }

    // Every pack the list names, then every installed pack it no longer
    // does, in that order.
    std::vector<Pack> getPacks() const;
    std::optional<Pack> findPack (const juce::String& id) const;

    Status getStatus (const juce::String& id) const;

    // The installed copy: what it was installed as, and where.
    std::optional<Pack> getInstalledPack (const juce::String& id) const;
    juce::File getPackFolder (const juce::String& id) const   { return root.getChildFile (id); }
    juce::File getInstalledEntry (const juce::String& id) const;
    juce::File getInstalledReadme (const juce::String& id) const;

    // Starts (or restarts, after a failure) fetching the list's version of
    // the pack. Installing over an installed copy replaces it only once the
    // new one is whole. Re-download is the same call.
    void download (const juce::String& id);
    void cancel (const juce::String& id);

    // Deletes the installed copy. False if it could not be.
    bool remove (const juce::String& id);

    // A pack's credits and terms: the installed README if there is one,
    // otherwise fetched from the list's readmeUrl. `done` is called on the
    // message thread, with an empty string if neither could be had.
    void fetchReadme (const juce::String& id, std::function<void (const juce::String&)> done);

    // The voicebank folders, or .sf2 files, that are installed now.
    std::vector<std::pair<Pack, juce::File>> getInstalledEntries (Kind) const;

    juce::File getRoot() const  { return root; }

    // How a byte count is written in the window: "60.3 MB".
    static juce::String describeSize (juce::int64 bytes);

private:
    class Job;
    friend class Job;

    void loadCachedList();
    void parseList (const juce::String& json, bool fromNetwork);
    void scanInstalled();
    void jobProgress (const juce::String& id, State, double progress, juce::int64 bytes);
    void jobFinished (const juce::String& id, juce::String error);

    juce::File root;
    juce::URL listUrl;

    std::vector<Pack> listed;
    std::map<juce::String, Pack> installed;

    ListState listState = ListState::loading;
    juce::String listError;

    std::map<juce::String, std::unique_ptr<Job>> jobs;
    std::map<juce::String, Status> transient;   // downloading..installing, or failed

    std::unique_ptr<juce::Thread> listThread;

    // Cleared on destruction, so a callback posted by a thread that has
    // not yet stopped finds nothing to call.
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundLibrary)
};

// Keeps a folder of downloads out of iCloud and device backups, which Apple
// asks of anything the app can fetch again. A no-op elsewhere.
void excludeFromBackup (const juce::File&);

} // namespace carve::app

#include "SoundLibrary.h"
#include "PerfProbe.h"

#include <juce_cryptography/juce_cryptography.h>

namespace carve::app
{

namespace
{
    constexpr int maxAttempts = 3;
    constexpr int connectTimeoutMs = 15000;

    juce::String kindName (SoundLibrary::Kind kind)
    {
        return kind == SoundLibrary::Kind::soundFont ? "soundfont" : "voicebank";
    }

    // A pack's id names its folder, so it is held to what a folder name can
    // safely be everywhere.
    bool isSafeId (const juce::String& id)
    {
        return id.isNotEmpty() && id.length() <= 64
               && id.containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789-");
    }

    bool isSafeRelativePath (const juce::String& path)
    {
        return path.isNotEmpty() && ! path.startsWithChar ('/') && ! path.contains ("..")
               && ! path.containsChar ('\\') && ! path.containsChar (':');
    }

    juce::File downloadsFolder (const juce::File& root)  { return root.getChildFile (".downloads"); }
    juce::File stagingFolder (const juce::File& root)    { return root.getChildFile (".staging"); }
} // namespace

//==============================================================================
juce::String SoundLibrary::Pack::displayName() const
{
    if (nameEn.isEmpty() || nameEn == name)
        return name;

    return name + " (" + nameEn + ")";
}

std::optional<SoundLibrary::Pack> SoundLibrary::Pack::fromVar (const juce::var& v)
{
    if (! v.isObject())
        return std::nullopt;

    Pack p;
    p.id = v["id"].toString();
    p.kind = v["kind"].toString() == "soundfont" ? Kind::soundFont : Kind::voicebank;
    p.name = v["name"].toString();
    p.nameEn = v["nameEn"].toString();
    p.version = v["version"].toString();
    p.author = v["author"].toString();
    p.homepage = v["homepage"].toString();
    p.description = v["description"].toString();
    p.licenseSummary = v["licenseSummary"].toString();
    p.credit = v["credit"].toString();
    p.entry = v["entry"].toString();
    p.url = juce::URL (v["url"].toString());
    p.readmeUrl = juce::URL (v["readmeUrl"].toString());
    p.size = (juce::int64) v["size"];
    p.installedSize = (juce::int64) v["installedSize"];
    p.sha256 = v["sha256"].toString().toLowerCase();

    if (! isSafeId (p.id) || ! isSafeRelativePath (p.entry) || p.version.isEmpty())
        return std::nullopt;

    if (p.name.isEmpty())
        p.name = p.nameEn.isNotEmpty() ? p.nameEn : p.id;

    return p;
}

juce::var SoundLibrary::Pack::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("id", id);
    o->setProperty ("kind", kindName (kind));
    o->setProperty ("name", name);
    o->setProperty ("nameEn", nameEn);
    o->setProperty ("version", version);
    o->setProperty ("author", author);
    o->setProperty ("homepage", homepage);
    o->setProperty ("description", description);
    o->setProperty ("licenseSummary", licenseSummary);
    o->setProperty ("credit", credit);
    o->setProperty ("entry", entry);
    o->setProperty ("url", url.toString (false));
    o->setProperty ("readmeUrl", readmeUrl.toString (false));
    o->setProperty ("size", size);
    o->setProperty ("installedSize", installedSize);
    o->setProperty ("sha256", sha256);
    return juce::var (o);
}

//==============================================================================
// One pack's download, check and install, on a thread of its own.
class SoundLibrary::Job : public juce::Thread
{
public:
    Job (SoundLibrary& ownerIn, Pack packIn)
        : juce::Thread ("Sound library: " + packIn.id),
          owner (ownerIn), pack (std::move (packIn)), root (ownerIn.root), alive (ownerIn.alive)
    {
    }

    ~Job() override  { stopThread (5000); }

    void run() override
    {
        const auto error = fetchCheckAndInstall();

        post ([id = pack.id, error] (SoundLibrary& library) { library.jobFinished (id, error); });
    }

private:
    juce::String fetchCheckAndInstall()
    {
        downloadsFolder (root).createDirectory();
        const auto part = downloadsFolder (root).getChildFile (pack.id + "-" + pack.version + ".part");

        // Room for the zip and what comes out of it, side by side, for the
        // moment both exist.
        if (const auto free = root.getBytesFreeOnVolume(); free > 0 && free < pack.size + pack.installedSize)
            return "Not enough free space: it needs " + describeSize (pack.size + pack.installedSize) + ".";

        juce::String error;

        for (int attempt = 1; attempt <= maxAttempts; ++attempt)
        {
            error = fetch (part);

            if (error.isEmpty())
            {
                report (State::verifying, 1.0);
                error = verify (part);

                if (error.isEmpty())
                    break;

                part.deleteFile();   // a bad copy; the next attempt starts over
            }

            if (threadShouldExit())
                return "Cancelled";

            if (attempt < maxAttempts)
                wait (1000 * attempt * attempt);

            if (threadShouldExit())
                return "Cancelled";
        }

        if (error.isNotEmpty())
            return error;

        report (State::installing, 0.0);
        error = install (part);

        if (error.isEmpty())
            part.deleteFile();

        return error;
    }

    // Into `part`, carrying on from whatever an earlier attempt left there.
    juce::String fetch (const juce::File& part)
    {
        if (pack.url.isEmpty())
            return "The library list gives no address for this pack.";

        auto have = part.existsAsFile() ? part.getSize() : (juce::int64) 0;

        if (have > pack.size)
        {
            part.deleteFile();
            have = 0;
        }

        if (have == pack.size && have > 0)
            return {};   // a whole copy from last time; the hash decides

        int status = 0;
        juce::StringPairArray responseHeaders;
        const auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                 .withConnectionTimeoutMs (connectTimeoutMs)
                                 .withStatusCode (&status)
                                 .withResponseHeaders (&responseHeaders)
                                 .withExtraHeaders (have > 0 ? "Range: bytes=" + juce::String (have) + "-" : juce::String())
                                 .withProgressCallback ([this] (int, int) { return ! threadShouldExit(); });

        auto in = pack.url.createInputStream (options);

        if (in == nullptr)
            return "Could not reach the server. Check the internet connection.";

        // A local file (the tests) has no status; it is read from the start.
        const bool resumed = status == 206;

        if (! resumed && status != 200 && ! pack.url.isLocalFile())
            return "The server answered " + juce::String (status) + ".";

        if (! resumed)
        {
            part.deleteFile();
            have = 0;
        }

        juce::FileOutputStream out (part);   // appends

        if (out.failedToOpen())
            return "Could not write to " + part.getParentDirectory().getFullPathName() + ".";

        juce::HeapBlock<char> buffer (1 << 16);
        auto lastReport = juce::Time::getMillisecondCounter();

        while (! threadShouldExit())
        {
            const auto n = in->read (buffer.get(), 1 << 16);

            if (n <= 0)
                break;

            if (! out.write (buffer.get(), (size_t) n))
                return "Could not write the download. Is the disk full?";

            have += n;

            if (const auto now = juce::Time::getMillisecondCounter(); now - lastReport > 100)
            {
                lastReport = now;
                report (State::downloading, pack.size > 0 ? (double) have / (double) pack.size : 0.0, have);
            }
        }

        out.flush();

        if (threadShouldExit())
            return "Cancelled";

        if (have != pack.size)
            return "The download stopped at " + describeSize (have) + " of " + describeSize (pack.size) + ".";

        report (State::downloading, 1.0, have);
        return {};
    }

    juce::String verify (const juce::File& part)
    {
        if (part.getSize() != pack.size)
            return "The download is the wrong size.";

        if (pack.sha256.isNotEmpty() && juce::SHA256 (part).toHexString() != pack.sha256)
            return "The download did not match its checksum.";

        return {};
    }

    juce::String install (const juce::File& part)
    {
        const auto staging = stagingFolder (root).getChildFile (pack.id);
        staging.deleteRecursively();

        if (! staging.createDirectory())
            return "Could not create " + staging.getFullPathName() + ".";

        {
            juce::ZipFile zip (part);

            if (zip.getNumEntries() == 0)
                return "The download is not a readable archive.";

            for (int i = 0; i < zip.getNumEntries(); ++i)
            {
                if (threadShouldExit())
                {
                    staging.deleteRecursively();
                    return "Cancelled";
                }

                if (const auto result = zip.uncompressEntry (i, staging); result.failed())
                {
                    staging.deleteRecursively();
                    return "Could not unpack: " + result.getErrorMessage();
                }

                report (State::installing, (double) (i + 1) / (double) zip.getNumEntries());
            }
        }

        if (! staging.getChildFile (pack.entry).exists())
        {
            staging.deleteRecursively();
            return "The archive does not contain " + pack.entry + ".";
        }

        if (! staging.getChildFile ("pack.json").replaceWithText (juce::JSON::toString (pack.toVar())))
            return "Could not write pack.json.";

        // The old copy goes only once the new one is whole, and comes back
        // if the new one cannot be put in its place.
        const auto target = root.getChildFile (pack.id);
        const auto old = stagingFolder (root).getChildFile (pack.id + ".old");
        old.deleteRecursively();

        if (target.exists() && ! target.moveFileTo (old))
            return "Could not replace the installed copy. Is something using it?";

        if (! staging.moveFileTo (target))
        {
            old.moveFileTo (target);
            return "Could not move the pack into place.";
        }

        old.deleteRecursively();
        return {};
    }

    void report (State state, double progress, juce::int64 bytes = 0)
    {
        post ([id = pack.id, state, progress, bytes] (SoundLibrary& library)
        {
            library.jobProgress (id, state, progress, bytes);
        });
    }

    void post (std::function<void (SoundLibrary&)> fn)
    {
        juce::MessageManager::callAsync ([weakAlive = std::weak_ptr<bool> (alive), &library = owner, call = std::move (fn)]
        {
            if (auto a = weakAlive.lock(); a != nullptr && *a)
                call (library);
        });
    }

    SoundLibrary& owner;
    const Pack pack;
    const juce::File root;
    std::shared_ptr<bool> alive;
};

//==============================================================================
juce::File SoundLibrary::defaultRoot()
{
    const auto data = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

   #if JUCE_MAC || JUCE_IOS
    const auto root = data.getChildFile ("Application Support/Carve DAW/Sound Library");
   #else
    const auto root = data.getChildFile ("Carve DAW/Sound Library");
   #endif

    if (! root.isDirectory())
    {
        root.createDirectory();
        excludeFromBackup (root);
    }

    return root;
}

juce::URL SoundLibrary::defaultListUrl()
{
   #ifdef CARVE_LIBRARY_URL
    return juce::URL (CARVE_LIBRARY_URL);
   #else
    return juce::URL ("https://carve-library.hashrock.workers.dev/v1/library.json");
   #endif
}

juce::String SoundLibrary::describeSize (juce::int64 bytes)
{
    if (bytes >= 1024 * 1024)
        return juce::String ((double) bytes / (1024.0 * 1024.0), 1) + " MB";

    if (bytes >= 1024)
        return juce::String ((double) bytes / 1024.0, 0) + " KB";

    return juce::String (bytes) + " bytes";
}

SoundLibrary::SoundLibrary (juce::File rootIn, juce::URL listUrlIn)
    : root (std::move (rootIn)), listUrl (std::move (listUrlIn))
{
    root.createDirectory();
    stagingFolder (root).deleteRecursively();   // whatever an interrupted install left
    scanInstalled();
    loadCachedList();
}

SoundLibrary::~SoundLibrary()
{
    *alive = false;

    for (auto& [id, job] : jobs)
        job->signalThreadShouldExit();

    jobs.clear();   // each waits for its thread

    if (listThread != nullptr)
        listThread->stopThread (5000);
}

//==============================================================================
void SoundLibrary::loadCachedList()
{
    if (const auto cached = root.getChildFile ("library.json"); cached.existsAsFile())
        parseList (cached.loadFileAsString(), false);
}

void SoundLibrary::parseList (const juce::String& json, bool fromNetwork)
{
    carve::perf::Scope perfProbe ("lib.parseList");
    const auto parsed = juce::JSON::parse (json);

    if ((int) parsed["schema"] != 1 || ! parsed["packs"].isArray())
    {
        if (fromNetwork)
        {
            listState = ListState::offline;
            listError = "The library list could not be read.";
        }

        return;
    }

    std::vector<Pack> packs;

    for (const auto& v : *parsed["packs"].getArray())
        if (auto pack = Pack::fromVar (v))
            packs.push_back (std::move (*pack));

    listed = std::move (packs);

    if (fromNetwork)
    {
        root.getChildFile ("library.json").replaceWithText (json);
        listState = ListState::online;
        listError = {};
    }
}

void SoundLibrary::refreshList()
{
    carve::perf::Scope perfProbe ("lib.refreshList");
    if (listThread != nullptr && listThread->isThreadRunning())
        return;

    listState = ListState::loading;
    sendChangeMessage();

    struct ListThread : juce::Thread
    {
        ListThread (juce::URL u, std::weak_ptr<bool> a, SoundLibrary& l)
            : juce::Thread ("Sound library list"), url (std::move (u)), alive (std::move (a)), library (l) {}

        void run() override
        {
            int status = 0;
            juce::String text;

            if (auto in = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                     .withConnectionTimeoutMs (connectTimeoutMs)
                                                     .withStatusCode (&status)))
                if (status == 200 || url.isLocalFile())
                    text = in->readEntireStreamAsString();

            juce::MessageManager::callAsync ([a = alive, &lib = library, text, status]
            {
                if (auto p = a.lock(); p == nullptr || ! *p)
                    return;

                if (text.isNotEmpty())
                {
                    lib.parseList (text, true);
                }
                else
                {
                    lib.listState = ListState::offline;
                    lib.listError = status != 0 ? "The library server answered " + juce::String (status) + "."
                                                : juce::String ("Could not reach the library server.");
                }

                lib.sendChangeMessage();
            });
        }

        juce::URL url;
        std::weak_ptr<bool> alive;
        SoundLibrary& library;
    };

    listThread = std::make_unique<ListThread> (listUrl, alive, *this);
    listThread->startThread();
}

void SoundLibrary::scanInstalled()
{
    carve::perf::Scope perfProbe ("lib.scanInstalled");
    installed.clear();

    for (const auto& folder : root.findChildFiles (juce::File::findDirectories, false))
    {
        if (folder.getFileName().startsWithChar ('.'))
            continue;

        const auto meta = folder.getChildFile ("pack.json");

        if (! meta.existsAsFile())
            continue;

        if (auto pack = Pack::fromVar (juce::JSON::parse (meta.loadFileAsString()));
            pack && pack->id == folder.getFileName() && folder.getChildFile (pack->entry).exists())
            installed[pack->id] = *pack;
    }
}

//==============================================================================
std::vector<SoundLibrary::Pack> SoundLibrary::getPacks() const
{
    auto packs = listed;

    for (const auto& [id, pack] : installed)
        if (std::none_of (listed.begin(), listed.end(), [&] (const Pack& p) { return p.id == id; }))
            packs.push_back (pack);

    return packs;
}

std::optional<SoundLibrary::Pack> SoundLibrary::findPack (const juce::String& id) const
{
    for (const auto& pack : listed)
        if (pack.id == id)
            return pack;

    return getInstalledPack (id);
}

std::optional<SoundLibrary::Pack> SoundLibrary::getInstalledPack (const juce::String& id) const
{
    if (const auto it = installed.find (id); it != installed.end())
        return it->second;

    return std::nullopt;
}

juce::File SoundLibrary::getInstalledEntry (const juce::String& id) const
{
    if (const auto pack = getInstalledPack (id))
        return getPackFolder (id).getChildFile (pack->entry);

    return {};
}

juce::File SoundLibrary::getInstalledReadme (const juce::String& id) const
{
    const auto readme = getPackFolder (id).getChildFile ("README-Carve.txt");
    return installed.count (id) != 0 && readme.existsAsFile() ? readme : juce::File();
}

std::vector<std::pair<SoundLibrary::Pack, juce::File>> SoundLibrary::getInstalledEntries (Kind kind) const
{
    std::vector<std::pair<Pack, juce::File>> entries;

    for (const auto& [id, pack] : installed)
        if (pack.kind == kind)
            entries.emplace_back (pack, getPackFolder (id).getChildFile (pack.entry));

    return entries;
}

SoundLibrary::Status SoundLibrary::getStatus (const juce::String& id) const
{
    carve::perf::Scope perfProbe ("lib.getStatus");
    Status status;

    if (const auto it = installed.find (id); it != installed.end())
    {
        status.installedVersion = it->second.version;
        status.state = State::installed;

        for (const auto& pack : listed)
            if (pack.id == id && pack.version != it->second.version)
                status.state = State::updateAvailable;
    }

    if (const auto it = transient.find (id); it != transient.end())
    {
        const auto version = status.installedVersion;
        status = it->second;
        status.installedVersion = version;
    }

    return status;
}

//==============================================================================
void SoundLibrary::download (const juce::String& id)
{
    if (jobs.count (id) != 0)
        return;

    std::optional<Pack> pack;

    for (const auto& p : listed)
        if (p.id == id)
            pack = p;

    if (! pack)
    {
        transient[id] = { State::failed, 0.0, 0, "This pack is not in the library list. Refresh and try again.", {} };
        sendChangeMessage();
        return;
    }

    transient[id] = { State::downloading, 0.0, 0, {}, {} };
    auto job = std::make_unique<Job> (*this, *pack);
    job->startThread();
    jobs[id] = std::move (job);
    sendChangeMessage();
}

void SoundLibrary::cancel (const juce::String& id)
{
    if (const auto it = jobs.find (id); it != jobs.end())
        it->second->signalThreadShouldExit();
}

bool SoundLibrary::remove (const juce::String& id)
{
    if (jobs.count (id) != 0 || ! isSafeId (id))
        return false;

    const auto folder = getPackFolder (id);
    const bool ok = ! folder.exists() || folder.deleteRecursively();

    for (const auto& part : downloadsFolder (root).findChildFiles (juce::File::findFiles, false, id + "-*.part"))
        part.deleteFile();

    transient.erase (id);
    scanInstalled();
    sendChangeMessage();
    return ok;
}

void SoundLibrary::jobProgress (const juce::String& id, State state, double progress, juce::int64 bytes)
{
    if (jobs.count (id) == 0)
        return;

    auto& status = transient[id];
    status.state = state;
    status.progress = progress;

    if (bytes > 0)
        status.bytesDone = bytes;

    sendChangeMessage();
}

void SoundLibrary::jobFinished (const juce::String& id, juce::String error)
{
    jobs.erase (id);   // its thread has posted this as its last act

    if (error.isEmpty() || error == "Cancelled")
        transient.erase (id);
    else
        transient[id] = { State::failed, 0.0, 0, error, {} };

    scanInstalled();
    sendChangeMessage();
}

void SoundLibrary::fetchReadme (const juce::String& id, std::function<void (const juce::String&)> done)
{
    if (const auto readme = getInstalledReadme (id); readme != juce::File())
    {
        done (readme.loadFileAsString());
        return;
    }

    const auto pack = findPack (id);

    if (! pack || pack->readmeUrl.isEmpty())
    {
        done ({});
        return;
    }

    // Small enough to fetch on a thread of its own, which ends by itself.
    juce::Thread::launch ([url = pack->readmeUrl, weakAlive = std::weak_ptr<bool> (alive), callback = std::move (done)]
    {
        juce::String text;
        int status = 0;

        if (auto in = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                 .withConnectionTimeoutMs (connectTimeoutMs)
                                                 .withStatusCode (&status)))
            if (status == 200 || url.isLocalFile())
                text = in->readEntireStreamAsString();

        juce::MessageManager::callAsync ([weakAlive, callback, text]
        {
            if (auto a = weakAlive.lock(); a != nullptr && *a)
                callback (text);
        });
    });
}

#if ! (JUCE_MAC || JUCE_IOS)
void excludeFromBackup (const juce::File&) {}
#endif

} // namespace carve::app

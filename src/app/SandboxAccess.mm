#include "SandboxAccess.h"

#include <cstdlib>
#include <vector>

#import <Foundation/Foundation.h>
#include <TargetConditionals.h>

// The Mac App Store build and the iPad build both come through here. The two
// systems keep the same idea -- a bookmark that buys access back -- with
// different spellings: the Mac's is "security-scoped" and asked for by name,
// the iPad's is any bookmark made from a URL the user picked, which carries
// the access implicitly.

namespace carve::sandbox
{

namespace
{
    // How many bookmarks are kept. Each one that is claimed back holds a
    // kernel resource for as long as the app runs, and the system's supply of
    // those is finite, so the least recently used go first. A song's samples
    // usually sit under a browser folder and need no entry of their own;
    // this is room for a great many songs' worth of the ones that do not.
    constexpr size_t maxBookmarks = 512;

    struct Entry
    {
        juce::String path;
        juce::MemoryBlock bookmark;
        bool active = false;   // claimed back, or granted, in this run
    };

    struct State
    {
        juce::File store;
        std::vector<Entry> entries;   // most recently used first
    };

    State& getState()
    {
        static State state;
        return state;
    }

    NSURL* urlFor (const juce::String& path)
    {
        NSString* string = [NSString stringWithUTF8String: path.toRawUTF8()];
        return [NSURL fileURLWithPath: string != nil ? string : @""];
    }

    juce::MemoryBlock createBookmark (const juce::String& path)
    {
        @autoreleasepool
        {
            NSError* error = nil;
           #if TARGET_OS_IPHONE
            const NSURLBookmarkCreationOptions options = 0;
           #else
            const NSURLBookmarkCreationOptions options = NSURLBookmarkCreationWithSecurityScope;
           #endif

            NSData* data = [urlFor (path) bookmarkDataWithOptions: options
                                   includingResourceValuesForKeys: nil
                                                    relativeToURL: nil
                                                            error: &error];

            if (data == nil)
                return {};

            return juce::MemoryBlock (data.bytes, (size_t) data.length);
        }
    }

    // Whether `path` is `parent` or somewhere inside it.
    bool isCoveredBy (const juce::String& path, const juce::String& parent)
    {
        return path == parent || path.startsWith (parent.endsWithChar ('/') ? parent : parent + "/");
    }

    NSURL* resolve (NSData* data, BOOL& stale)
    {
        NSError* error = nil;

       #if TARGET_OS_IPHONE
        const NSURLBookmarkResolutionOptions options = NSURLBookmarkResolutionWithoutUI;
       #else
        const NSURLBookmarkResolutionOptions options = NSURLBookmarkResolutionWithSecurityScope
                                                     | NSURLBookmarkResolutionWithoutUI
                                                     | NSURLBookmarkResolutionWithoutMounting;
       #endif

        return [NSURL URLByResolvingBookmarkData: data
                                         options: options
                                   relativeToURL: nil
                             bookmarkDataIsStale: &stale
                                           error: &error];
    }

   #if TARGET_OS_IPHONE
    // The app's own container and bundle are always open to it -- and their
    // paths are not even stable, since the system moves both on an update --
    // so nothing under them is worth a bookmark.
    bool isInsideApp (const juce::String& path)
    {
        return isCoveredBy (path, juce::String::fromUTF8 (NSHomeDirectory().UTF8String))
            || isCoveredBy (path, juce::String::fromUTF8 (NSBundle.mainBundle.bundlePath.UTF8String));
    }
   #endif

    void save()
    {
        auto& state = getState();

        if (state.store == juce::File())
            return;

        juce::XmlElement xml ("FILEACCESS");

        for (const auto& entry : state.entries)
        {
            auto* e = xml.createNewChildElement ("BOOKMARK");
            e->setAttribute ("path", entry.path);
            e->setAttribute ("data", entry.bookmark.toBase64Encoding());
        }

        state.store.getParentDirectory().createDirectory();
        xml.writeTo (state.store);
    }
} // namespace

bool isSandboxed()
{
   #if TARGET_OS_IPHONE
    return true;   // every iOS app is
   #endif

    // Set by the system for every sandboxed process, and by nothing else.
    static const bool sandboxed = std::getenv ("APP_SANDBOX_CONTAINER_ID") != nullptr;
    return sandboxed;
}

void restoreAccess (const juce::File& store)
{
    if (! isSandboxed())
        return;

    auto& state = getState();
    state.store = store;
    state.entries.clear();

    const auto xml = juce::parseXML (store);

    if (xml == nullptr)
        return;

    bool changed = false;

    for (auto* e : xml->getChildWithTagNameIterator ("BOOKMARK"))
    {
        Entry entry;
        entry.path = e->getStringAttribute ("path");

        if (entry.path.isEmpty() || ! entry.bookmark.fromBase64Encoding (e->getStringAttribute ("data")))
            continue;

        @autoreleasepool
        {
            NSData* data = [NSData dataWithBytes: entry.bookmark.getData() length: entry.bookmark.getSize()];
            BOOL stale = NO;
            NSURL* url = resolve (data, stale);

            // Never stopped: the access is wanted for as long as the app is
            // open, and the system takes it back when the process ends.
            if (url != nil && [url startAccessingSecurityScopedResource])
            {
                entry.active = true;

                // The file was moved or renamed and the bookmark followed
                // it. The path every song knows it by is the old one, so the
                // entry keeps that; only the token is renewed.
                if (stale)
                {
                    if (auto renewed = createBookmark (juce::String::fromUTF8 (url.path.UTF8String)); ! renewed.isEmpty())
                    {
                        entry.bookmark = std::move (renewed);
                        changed = true;
                    }
                }
            }
        }

        // One that would not resolve is kept all the same: a drive that is
        // not plugged in today is the ordinary reason, and it will be back.
        state.entries.push_back (std::move (entry));
    }

    if (changed)
        save();
}

void rememberAccess (const juce::File& file)
{
    if (! isSandboxed() || file == juce::File())
        return;

    auto& state = getState();
    const auto path = file.getFullPathName();

   #if TARGET_OS_IPHONE
    if (isInsideApp (path))
        return;
   #endif

    // Already covered by something claimed back or granted this run -- the
    // file itself, or a folder above it. Moved to the front, so that what is
    // in use is what survives the cap.
    for (size_t i = 0; i < state.entries.size(); ++i)
    {
        if (state.entries[i].active && isCoveredBy (path, state.entries[i].path))
        {
            if (i != 0)
            {
                auto entry = std::move (state.entries[i]);
                state.entries.erase (state.entries.begin() + (std::ptrdiff_t) i);
                state.entries.insert (state.entries.begin(), std::move (entry));
                save();
            }

            return;
        }
    }

    // Fails for anything the app cannot reach, which is the right answer:
    // there is no access to remember.
    auto bookmark = createBookmark (path);

    if (bookmark.isEmpty())
        return;

    // An entry for this path that did not resolve at startup is replaced.
    std::erase_if (state.entries, [&path] (const Entry& e) { return e.path == path; });

    state.entries.insert (state.entries.begin(), Entry { path, std::move (bookmark), true });

    if (state.entries.size() > maxBookmarks)
        state.entries.resize (maxBookmarks);

    save();
}

juce::File claimChosen (juce::URL chosen)
{
    if (! chosen.isLocalFile())
        return {};

   #if TARGET_OS_IPHONE
    // The token the picker handed over, which JUCE keeps beside the URL
    // (getURLBookmark is the URL's friend, found by argument lookup).
    if (auto* token = (__bridge NSData*) getURLBookmark (chosen))
    {
        @autoreleasepool
        {
            BOOL stale = NO;
            NSURL* url = resolve (token, stale);

            // Never stopped, as at startup: the file is wanted for as long as
            // the app is open.
            if (url != nil && [url startAccessingSecurityScopedResource])
            {
                const auto path = juce::String::fromUTF8 (url.path.UTF8String);

                if (! isInsideApp (path))
                {
                    auto& state = getState();
                    std::erase_if (state.entries, [&path] (const Entry& e) { return e.path == path; });
                    state.entries.insert (state.entries.begin(),
                                          Entry { path, juce::MemoryBlock (token.bytes, (size_t) token.length), true });

                    if (state.entries.size() > maxBookmarks)
                        state.entries.resize (maxBookmarks);

                    save();
                }

                return juce::File (path);
            }
        }
    }
   #endif

    return chosen.getLocalFile();
}

} // namespace carve::sandbox

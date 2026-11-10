#include "SandboxAccess.h"

#include <cstdlib>
#include <vector>

#import <Foundation/Foundation.h>

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
            NSData* data = [urlFor (path) bookmarkDataWithOptions: NSURLBookmarkCreationWithSecurityScope
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
            NSError* error = nil;
            NSURL* url = [NSURL URLByResolvingBookmarkData: data
                                                   options: NSURLBookmarkResolutionWithSecurityScope
                                                             | NSURLBookmarkResolutionWithoutUI
                                                             | NSURLBookmarkResolutionWithoutMounting
                                             relativeToURL: nil
                                       bookmarkDataIsStale: &stale
                                                     error: &error];

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

} // namespace carve::sandbox

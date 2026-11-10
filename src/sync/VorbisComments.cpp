#include "VorbisComments.h"

#include <array>
#include <cstring>
#include <optional>
#include <vector>

namespace carve::sync
{

namespace
{
    using Bytes = std::vector<juce::uint8>;

    constexpr size_t pageHeaderSize = 27;
    constexpr size_t maxSegmentsPerPage = 255;
    constexpr juce::uint8 continuedPacketFlag = 0x01;

    juce::uint32 readLE32 (const juce::uint8* p) noexcept
    {
        return (juce::uint32) p[0] | ((juce::uint32) p[1] << 8)
             | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[3] << 24);
    }

    void writeLE32 (juce::uint8* p, juce::uint32 v) noexcept
    {
        p[0] = (juce::uint8) v;
        p[1] = (juce::uint8) (v >> 8);
        p[2] = (juce::uint8) (v >> 16);
        p[3] = (juce::uint8) (v >> 24);
    }

    void appendLE32 (Bytes& out, juce::uint32 v)
    {
        std::array<juce::uint8, 4> b {};
        writeLE32 (b.data(), v);
        out.insert (out.end(), b.begin(), b.end());
    }

    // Ogg's checksum: CRC-32 with the polynomial 0x04c11db7, most significant
    // bit first, starting from zero and with no final inversion -- not the
    // zlib one. It covers the whole page with the checksum field as zeros.
    juce::uint32 oggCrc (const juce::uint8* data, size_t size) noexcept
    {
        static const auto table = []
        {
            std::array<juce::uint32, 256> t {};

            for (juce::uint32 i = 0; i < 256; ++i)
            {
                auto r = i << 24;

                for (int bit = 0; bit < 8; ++bit)
                    r = (r & 0x80000000u) != 0 ? (r << 1) ^ 0x04c11db7u : r << 1;

                t[i] = r;
            }

            return t;
        }();

        juce::uint32 crc = 0;

        for (size_t i = 0; i < size; ++i)
            crc = (crc << 8) ^ table[((crc >> 24) ^ data[i]) & 0xff];

        return crc;
    }

    // Stamps the checksum into a complete page.
    void sealPage (juce::uint8* page, size_t size) noexcept
    {
        writeLE32 (page + 22, 0);
        writeLE32 (page + 22, oggCrc (page, size));
    }

    struct Page
    {
        size_t offset = 0;      // of the page in the file
        size_t size = 0;        // header, segment table and body
        size_t bodyOffset = 0;  // of the body in the file
        juce::uint32 serial = 0;
        const juce::uint8* segments = nullptr;
        size_t numSegments = 0;
    };

    std::optional<Page> parsePage (const juce::uint8* data, size_t size, size_t offset)
    {
        if (offset + pageHeaderSize > size || std::memcmp (data + offset, "OggS", 4) != 0
             || data[offset + 4] != 0)
            return std::nullopt;

        Page page;
        page.offset = offset;
        page.serial = readLE32 (data + offset + 14);
        page.numSegments = data[offset + 26];
        page.segments = data + offset + pageHeaderSize;
        page.bodyOffset = offset + pageHeaderSize + page.numSegments;

        if (page.bodyOffset > size)
            return std::nullopt;

        size_t bodySize = 0;

        for (size_t i = 0; i < page.numSegments; ++i)
            bodySize += page.segments[i];

        page.size = pageHeaderSize + page.numSegments + bodySize;

        if (offset + page.size > size)
            return std::nullopt;

        return page;
    }

    // The three Vorbis headers and where the audio pages start.
    struct Headers
    {
        Page firstPage;             // the identification header, alone on its page
        Bytes comment, setup;
        size_t audioOffset = 0;     // first byte after the header pages
        juce::uint32 numHeaderPages = 0;   // those carrying comment + setup
    };

    // The identification header is on a page of its own; the comment and
    // setup headers follow, over as many pages as they need, and the audio
    // starts on a fresh page after them. That last rule is what makes it
    // possible to swap the headers without re-laying the audio.
    std::optional<Headers> readHeaders (const juce::uint8* data, size_t size)
    {
        Headers headers;

        const auto first = parsePage (data, size, 0);

        if (! first || first->numSegments != 1 || first->size - first->bodyOffset < 7
             || std::memcmp (data + first->bodyOffset, "\x01vorbis", 7) != 0)
            return std::nullopt;

        headers.firstPage = *first;

        std::array<Bytes*, 2> packets { &headers.comment, &headers.setup };
        size_t packetIndex = 0;
        auto offset = first->size;

        while (packetIndex < packets.size())
        {
            const auto page = parsePage (data, size, offset);

            if (! page || page->serial != first->serial)
                return std::nullopt;

            auto body = page->bodyOffset;

            for (size_t i = 0; i < page->numSegments; ++i)
            {
                // A third packet sharing a header page would be audio that
                // this cannot move.
                if (packetIndex >= packets.size())
                    return std::nullopt;

                const auto length = (size_t) page->segments[i];
                packets[packetIndex]->insert (packets[packetIndex]->end(), data + body, data + body + length);
                body += length;

                if (length < 255)
                    ++packetIndex;
            }

            offset += page->size;
            ++headers.numHeaderPages;
        }

        headers.audioOffset = offset;

        if (headers.comment.size() < 7 || std::memcmp (headers.comment.data(), "\x03vorbis", 7) != 0)
            return std::nullopt;

        return headers;
    }

    struct CommentList
    {
        juce::String vendor;
        juce::StringArray entries;   // "NAME=value", as stored
    };

    std::optional<CommentList> parseComments (const Bytes& packet)
    {
        CommentList list;
        size_t position = 7;   // past "\x03vorbis"

        const auto readString = [&] () -> std::optional<juce::String>
        {
            if (position + 4 > packet.size())
                return std::nullopt;

            const auto length = (size_t) readLE32 (packet.data() + position);
            position += 4;

            if (length > packet.size() - position)
                return std::nullopt;

            auto text = juce::String::fromUTF8 ((const char*) packet.data() + position, (int) length);
            position += length;
            return text;
        };

        const auto vendor = readString();

        if (! vendor || position + 4 > packet.size())
            return std::nullopt;

        list.vendor = *vendor;

        const auto count = readLE32 (packet.data() + position);
        position += 4;

        for (juce::uint32 i = 0; i < count; ++i)
        {
            const auto entry = readString();

            if (! entry)
                return std::nullopt;

            list.entries.add (*entry);
        }

        return list;
    }

    Bytes buildCommentPacket (const CommentList& list)
    {
        Bytes packet;

        const auto appendString = [&packet] (const juce::String& text)
        {
            const auto bytes = (size_t) text.getNumBytesAsUTF8();
            appendLE32 (packet, (juce::uint32) bytes);
            const auto* utf8 = (const juce::uint8*) text.toRawUTF8();
            packet.insert (packet.end(), utf8, utf8 + bytes);
        };

        const char signature[] = "\x03vorbis";
        packet.insert (packet.end(), signature, signature + 7);
        appendString (list.vendor);
        appendLE32 (packet, (juce::uint32) list.entries.size());

        for (const auto& entry : list.entries)
            appendString (entry);

        packet.push_back (1);   // the framing bit
        return packet;
    }

    // Lays packets out over pages numbered from firstSequence. Returns how
    // many pages that took.
    juce::uint32 writePackets (Bytes& out, const std::vector<const Bytes*>& packets,
                               juce::uint32 serial, juce::uint32 firstSequence)
    {
        // Each packet as lacing values: 255 for every full segment, then one
        // below 255 that ends it (zero, if the length was a multiple of 255).
        struct Segment { const juce::uint8* data; juce::uint8 length; bool endsPacket; };
        std::vector<Segment> segments;

        for (const auto* packet : packets)
        {
            auto remaining = packet->size();
            const auto* data = packet->data();

            for (;;)
            {
                const auto length = juce::jmin (remaining, (size_t) 255);
                segments.push_back ({ data, (juce::uint8) length, length < 255 });
                data += length;
                remaining -= length;

                if (length < 255)
                    break;
            }
        }

        juce::uint32 numPages = 0;

        for (size_t start = 0; start < segments.size(); start += maxSegmentsPerPage)
        {
            const auto count = juce::jmin (maxSegmentsPerPage, segments.size() - start);
            const auto pageStart = out.size();
            bool finishesPacket = false;

            for (size_t i = 0; i < count; ++i)
                finishesPacket = finishesPacket || segments[start + i].endsPacket;

            out.insert (out.end(), { 'O', 'g', 'g', 'S', 0 });
            out.push_back (start > 0 && ! segments[start - 1].endsPacket ? continuedPacketFlag : (juce::uint8) 0);

            // Header pages sit at position zero; a page no packet ends on has
            // no position at all, which is spelled as all ones.
            out.insert (out.end(), 8, finishesPacket ? (juce::uint8) 0 : (juce::uint8) 0xff);

            appendLE32 (out, serial);
            appendLE32 (out, firstSequence + numPages);
            appendLE32 (out, 0);   // checksum, sealed below
            out.push_back ((juce::uint8) count);

            for (size_t i = 0; i < count; ++i)
                out.push_back (segments[start + i].length);

            for (size_t i = 0; i < count; ++i)
                out.insert (out.end(), segments[start + i].data,
                            segments[start + i].data + segments[start + i].length);

            sealPage (out.data() + pageStart, out.size() - pageStart);
            ++numPages;
        }

        return numPages;
    }

    juce::String nameOf (const juce::String& entry)
    {
        return entry.upToFirstOccurrenceOf ("=", false, false).toUpperCase();
    }
} // namespace

//==============================================================================
juce::String setVorbisComments (const juce::File& oggFile, const juce::StringPairArray& comments)
{
    juce::MemoryBlock file;

    if (! oggFile.loadFileAsData (file))
        return "could not read " + oggFile.getFileName();

    const auto* data = (const juce::uint8*) file.getData();
    const auto size = file.getSize();

    const auto headers = readHeaders (data, size);
    auto list = headers ? parseComments (headers->comment) : std::nullopt;

    if (! list)
        return oggFile.getFileName() + " is not an Ogg Vorbis file this can tag";

    for (const auto& name : comments.getAllKeys())
    {
        for (int i = list->entries.size(); --i >= 0;)
            if (nameOf (list->entries[i]) == name.toUpperCase())
                list->entries.remove (i);

        list->entries.add (name.toUpperCase() + "=" + comments[name]);
    }

    const auto commentPacket = buildCommentPacket (*list);

    Bytes out;
    out.reserve (size + commentPacket.size());
    out.insert (out.end(), data, data + headers->firstPage.size);

    const auto serial = headers->firstPage.serial;
    const auto numHeaderPages = writePackets (out, { &commentPacket, &headers->setup }, serial, 1);

    // The audio, as it was. Its pages are renumbered only if the headers now
    // take a different number of pages, and a renumbered page needs its
    // checksum again.
    const auto audioStart = out.size();
    out.insert (out.end(), data + headers->audioOffset, data + size);

    if (numHeaderPages != headers->numHeaderPages)
    {
        const auto shift = numHeaderPages - headers->numHeaderPages;   // wraps, as the addition below does

        for (auto offset = audioStart; offset < out.size();)
        {
            const auto page = parsePage (out.data(), out.size(), offset);

            if (! page || page->serial != serial)
                return oggFile.getFileName() + " is not an Ogg Vorbis file this can tag";

            auto* bytes = out.data() + offset;
            writeLE32 (bytes + 18, readLE32 (bytes + 18) + shift);
            sealPage (bytes, page->size);
            offset += page->size;
        }
    }

    // Written beside the file and moved over it, so a failure halfway leaves
    // the original rather than half of each.
    juce::TemporaryFile temp (oggFile);

    if (! temp.getFile().replaceWithData (out.data(), out.size()) || ! temp.overwriteTargetFileWithTemporary())
        return "could not write " + oggFile.getFileName();

    return {};
}

juce::StringPairArray readVorbisComments (const juce::File& oggFile)
{
    juce::StringPairArray result;
    juce::MemoryBlock file;

    if (! oggFile.loadFileAsData (file))
        return result;

    const auto headers = readHeaders ((const juce::uint8*) file.getData(), file.getSize());
    const auto list = headers ? parseComments (headers->comment) : std::nullopt;

    if (list)
        for (const auto& entry : list->entries)
            result.set (nameOf (entry), entry.fromFirstOccurrenceOf ("=", false, false));

    return result;
}

} // namespace carve::sync

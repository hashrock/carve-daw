#include "NoteMonitorPlugin.h"

namespace carve::plugins
{

const char* NoteMonitorPlugin::xmlTypeName = "carveNoteMonitor";

NoteMonitorPlugin::NoteMonitorPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    for (auto& count : hitCounts)
        count.store (0, std::memory_order_relaxed);

    releaseAll();
}

NoteMonitorPlugin::~NoteMonitorPlugin()
{
    notifyListenersOfDeletion();
}

void NoteMonitorPlugin::initialise (const te::PluginInitialisationInfo&)
{
    // A note held across a graph rebuild gets its note-off from the engine
    // (see the CombiningNode note in EditSync's clip sync) but not through
    // here, so the held flags start clean rather than stick.
    releaseAll();
}

void NoteMonitorPlugin::deinitialise()
{
    releaseAll();
}

void NoteMonitorPlugin::releaseAll()
{
    for (auto& held : heldNotes)
        held.store (false, std::memory_order_relaxed);
}

NoteMonitorPlugin::NoteActivity NoteMonitorPlugin::getActivity (int midiNote) const
{
    if (midiNote < 0 || midiNote >= 128)
        return {};

    return { hitCounts[(size_t) midiNote].load (std::memory_order_relaxed),
             heldNotes[(size_t) midiNote].load (std::memory_order_relaxed) };
}

void NoteMonitorPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    // The audio is left exactly as it arrived: PluginNode has already copied
    // the input into the output buffer before calling this.
    if (fc.bufferForMidiMessages == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    // The flag is what a playhead jump or a mute turns into -- the sampler
    // treats it as an all-notes-off, so the pads should too.
    if (fc.bufferForMidiMessages->isAllNotesOff)
        releaseAll();

    for (const auto& m : *fc.bufferForMidiMessages)
    {
        if (m.isNoteOn())
        {
            const auto note = (size_t) m.getNoteNumber();
            hitCounts[note].fetch_add (1, std::memory_order_relaxed);
            heldNotes[note].store (true, std::memory_order_relaxed);
        }
        else if (m.isNoteOff())
        {
            heldNotes[(size_t) m.getNoteNumber()].store (false, std::memory_order_relaxed);
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            releaseAll();
        }
    }
}

} // namespace carve::plugins

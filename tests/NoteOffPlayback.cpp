// Deleting a note, a clip or a pattern while it plays must turn its notes
// off. This drives the real EditSync against the engine's hosted audio device
// block by block, so the same code the app runs is what is checked here, with
// no audio hardware and no timing luck: the message loop is pumped by hand
// between blocks, the way the app's audio callbacks interleave with its UI.
//
// A "MidiSpy" plugin sits in front of the generator's instrument and records
// every MIDI message the track delivers, which is what the instrument hears.
//
// Why this exists: see newClipAdded in EngineSetup.h. These scenarios are
// the ones that stuck before it, and before EditSync matched clips to
// placements by id.

#include "NoteOffPlayback.h"

#include <cstdio>
#include <mutex>
#include <vector>

#include "model/SongModel.h"
#include "sync/EditSync.h"

namespace te = tracktion;

namespace carve::test
{
namespace
{

struct Event
{
    int block;
    juce::MidiMessage message;
};

struct MidiSpyPlugin : public te::Plugin
{
    explicit MidiSpyPlugin (te::PluginCreationInfo info) : te::Plugin (info) {}
    ~MidiSpyPlugin() override { notifyListenersOfDeletion(); }

    static const char* getPluginName()  { return "MidiSpy"; }
    static const char* xmlTypeName;
    static juce::ValueTree create()     { return te::createValueTree (te::IDs::PLUGIN, te::IDs::type, xmlTypeName); }

    juce::String getName() const override             { return "MidiSpy"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getSelectableDescription() override  { return "MidiSpy"; }
    bool takesMidiInput() override                    { return true; }
    bool takesAudioInput() override                   { return true; }
    int getNumOutputChannelsGivenInputs (int n) override { return n; }
    void initialise (const te::PluginInitialisationInfo&) override {}
    void deinitialise() override {}

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (fc.bufferForMidiMessages == nullptr)
            return;

        const std::lock_guard<std::mutex> lock (mutex);

        for (const auto& m : *fc.bufferForMidiMessages)
            events.push_back ({ block, m });
    }

    int block = 0;                 // stepped by the harness, once per processBlock
    std::mutex mutex;
    std::vector<Event> events;
};

const char* MidiSpyPlugin::xmlTypeName = "carveMidiSpy";

void pump (int ms)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
}

constexpr double sampleRate = 44100.0;
constexpr int blockSize = 256;

struct Scenario
{
    const char* title;
    void (*mutate) (model::Song&);
};

// Three placements of one pattern holding a single note that lasts most of
// the placement. Playback is run into the first placement's note, the
// scenario edits the song, and playback continues.
bool runScenario (te::Engine& engine, te::HostedAudioDeviceInterface& hosted, const Scenario& scenario)
{
    auto song = model::Song::create ("note-offs");
    auto generator = song.addGenerator ("Bass", "internal-synth", nullptr);
    auto pattern = generator.addPattern ("P", 4.0, nullptr);
    pattern.addNote (0.0, 3.0, 60, 100, nullptr);
    auto playlist = song.getPlaylist();
    playlist.addClip (generator, pattern, 0.0, nullptr);
    playlist.addClip (generator, pattern, 4.0, nullptr);
    playlist.addClip (generator, pattern, 8.0, nullptr);

    auto edit = te::Edit::createSingleTrackEdit (engine);
    sync::EditSync editSync (song, *edit);
    pump (20);

    auto spy = te::insertNewPlugin<MidiSpyPlugin> (*te::getAudioTracks (*edit)[0], 0);
    pump (20);

    juce::AudioBuffer<float> audio (2, blockSize);
    juce::MidiBuffer midi;

    // The message loop runs between blocks the way the app's does between
    // audio callbacks, at a fraction of the block rate: every pump sleeps a
    // millisecond when there is nothing queued.
    const auto processBlocks = [&] (int count)
    {
        for (int i = 0; i < count; ++i)
        {
            ++spy->block;
            audio.clear();
            hosted.processBlock (audio, midi);

            if ((i & 15) == 0)
                pump (1);
        }
    };

    auto& transport = edit->getTransport();
    transport.setPosition (te::TimePosition());
    transport.play (false);
    pump (50);

    processBlocks (90);                               // ~0.52 s at 120 bpm: a beat into the note
    const int mutationBlock = spy->block;
    scenario.mutate (song);
    pump (80);                                        // EditSync's resync, then the Edit's graph rebuild
    processBlocks (300);                              // ~1.7 s more: past the note's own end and the next clip's start
    transport.stop (false, false);
    pump (20);

    bool sawNoteOn = false, sawOffAfter = false;
    std::printf ("\n== %s (edit after block %d)\n", scenario.title, mutationBlock);

    const std::lock_guard<std::mutex> lock (spy->mutex);

    for (const auto& e : spy->events)
    {
        const auto& m = e.message;

        if (m.isNoteOn())
        {
            sawNoteOn = true;
            std::printf ("  block %4d  note on  %d\n", e.block, m.getNoteNumber());
        }
        else if (m.isNoteOff())
        {
            std::printf ("  block %4d  note off %d\n", e.block, m.getNoteNumber());

            if (e.block >= mutationBlock && m.getNoteNumber() == 60)
                sawOffAfter = true;
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            std::printf ("  block %4d  CC%d\n", e.block, m.getControllerNumber());

            if (e.block >= mutationBlock)
                sawOffAfter = true;
        }
    }

    std::printf ("  -> %s\n", ! sawNoteOn ? "no note-on at all: the harness is broken"
                            : sawOffAfter ? "released"
                                          : "STUCK: no note-off after the edit");
    return sawNoteOn && sawOffAfter;
}

} // namespace

int runNoteOffPlaybackChecks (te::Engine& engine)
{
    engine.getPluginManager().createBuiltInType<MidiSpyPlugin>();

    auto& hosted = engine.getDeviceManager().getHostedAudioDeviceInterface();
    te::HostedAudioDeviceInterface::Parameters parameters;
    parameters.sampleRate = sampleRate;
    parameters.blockSize = blockSize;
    parameters.inputChannels = 0;
    parameters.outputChannels = 2;
    hosted.initialise (parameters);

    const Scenario scenarios[] =
    {
        { "control: nothing deleted, the note ends on its own", [] (model::Song&) {} },

        { "delete the sounding note", [] (model::Song& song)
          {
              auto pattern = song.getGenerators()[0].getPatterns()[0];
              pattern.removeNote (pattern.getNotes()[0], nullptr);
          } },

        { "replace the sounding note with another pitch", [] (model::Song& song)
          {
              auto pattern = song.getGenerators()[0].getPatterns()[0];
              pattern.removeNote (pattern.getNotes()[0], nullptr);
              pattern.addNote (0.0, 3.0, 62, 100, nullptr);
          } },

        { "delete the sounding clip, the first of three", [] (model::Song& song)
          {
              auto playlist = song.getPlaylist();
              playlist.removeClip (playlist.getClips()[0], nullptr);
          } },

        { "delete the last clip while the first sounds", [] (model::Song& song)
          {
              auto playlist = song.getPlaylist();
              playlist.removeClip (playlist.getClips()[2], nullptr);
          } },

        { "delete the pattern and every clip of it", [] (model::Song& song)
          {
              auto generator = song.getGenerators()[0];
              generator.addPattern ("Other", 4.0, nullptr);   // a generator keeps at least one
              auto pattern = generator.getPatterns()[0];
              auto playlist = song.getPlaylist();

              for (const auto& clip : playlist.getClips())
                  if (clip.getPatternId() == pattern.getId())
                      playlist.removeClip (clip, nullptr);

              generator.removePattern (pattern, nullptr);
          } },
    };

    int stuck = 0;

    for (const auto& scenario : scenarios)
        if (! runScenario (engine, hosted, scenario))
            ++stuck;

    std::printf ("\n%d of %d scenarios stuck\n", stuck, (int) std::size (scenarios));
    return stuck;
}

} // namespace carve::test

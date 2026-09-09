#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace carve::test
{

// Plays a small song through EditSync on the hosted audio device, deletes
// the note / clip / pattern that is sounding, and checks that the instrument
// receives a note-off for it. Prints a report; returns the number of
// scenarios whose note stayed on. Run by carve-render --check-note-offs.
int runNoteOffPlaybackChecks (tracktion::Engine&);

}

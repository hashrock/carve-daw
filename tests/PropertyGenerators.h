#pragma once

#include <rapidcheck.h>

#include "model/SongModel.h"

// The generators every property file here wants.
//
// Each of the three test files had grown its own genBeat, its own 0..1 value,
// and its own copy of the tempo range the model clamps to. Free `inline`
// functions merge across translation units, so one header serves all three
// without any of them having to be a template or a macro.
//
// What is *not* here: anything whose shape is particular to one file. The
// piano roll's grid units, its ordered pairs of beats and the song driver's
// command language stay where they are used, because a generator nobody else
// calls is not shared code.
//
// It pulls in the model for the one range it needs rather than restating the
// numbers. That does mean the piano roll's properties -- which want nothing
// from the model -- compile against it, but they are already in a binary that
// compiles SongModel.cpp, so the include costs them nothing that matters and a
// restated constant would cost a way for the two to drift.

namespace carve::test
{

// Beats are generated on a grid rather than as arbitrary doubles, which is
// what every gesture in the app snaps to; arbitrary doubles would only buy
// floating-point noise no song can hold. The unit differs per file -- the
// piano roll draws finer than the playlist -- so it is an argument.
inline rc::Gen<double> genGridBeat (double unit, int maxSteps)
{
    return rc::gen::map (rc::gen::inRange (0, maxSteps + 1),
                         [unit] (int steps) { return steps * unit; });
}

// 0..1 in hundredths, which is what the knobs that take one are worth. Scaled
// to whatever range a caller needs.
inline rc::Gen<double> genUnitInterval()
{
    return rc::gen::map (rc::gen::inRange (0, 101), [] (int n) { return n / 100.0; });
}

// Tempos inside what TempoChange::setBpm clamps to, since generating outside
// it would only be testing the clamp. Tenths, so a fractional tempo is
// reachable without generating a double no tempo field could hold.
inline rc::Gen<double> genBpm()
{
    return rc::gen::map (rc::gen::inRange ((int) (model::TempoChange::minBpm * 10.0),
                                           (int) (model::TempoChange::maxBpm * 10.0) + 1),
                         [] (int tenths) { return tenths / 10.0; });
}

} // namespace carve::test

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// The engines of Modelled Drums (DrumModelPlugin.h): a physically modelled kit,
// solved as it plays. Ports of the PhysKit project's real-time engines
// (instruments/drums/kit/player/rt/drum.zig and snare.zig, written for its
// player/live.html), which run models that the kit's offline engines wrote:
// everything that does not depend on the hit -- the heads' modes, the air in
// the shell, the shell, the mics -- so that a hit is only the stick or beater
// meeting the head and the time step that follows.
//
// Each drum runs at its model's own rate (48kHz toms and snare, 16kHz kick);
// the Kit resamples to the host's. Nothing here allocates once loaded.
namespace carve::plugins::drummodel
{

//==============================================================================
// A modal drum (model format PKD2): the toms and the kick. Two heads in modal
// coordinates, the air cavity, the shell, a port plug, up to five mics with
// their filters and a stereo mix. The drum is one object: a second hit lands
// on the moving head.
class ModalDrum
{
public:
    ModalDrum();
    ~ModalDrum();

    // A model, as written by the kit's tools/export_rt.py (uncompressed).
    bool load (const void* data, std::size_t size, double gain);
    bool isLoaded() const;
    double getModelRate() const;

    // x, y: the point on the batter (m; x to the drummer's right, y away from
    // the drummer); v: stick (or beater) speed in m/s; stick < 0 alternates
    // hands. Starts with the next process().
    void hit (double x, double y, double v, int stick = -1);

    // Relative to the model (1 = as rendered): 0 tuning (frequency factor),
    // 1 loss (factor on every decay rate), 2 pitch glide (tension modulation),
    // 3 stick hardness (contact stiffness), 4 close mic, 5 overheads.
    void setParam (int what, double value);

    // Adds n samples at the model rate.
    void process (float* left, float* right, int n);

    int getActiveModes() const;
    bool isAsleep() const;
    void reset();   // silence, at once

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};

//==============================================================================
// The snare (model format PKS1): the batter head in modal coordinates, the
// resonant head on a grid, twelve wires and their collisions with it, the air
// in the shell, the shell and hoop, the stick (and its rim for a rimshot),
// three mics. Mono.
class Snare
{
public:
    Snare();
    ~Snare();

    // Loads the model and lets the wires settle on the head (a fraction of a
    // second of computing).
    bool load (const void* data, std::size_t size);
    bool isLoaded() const;
    double getModelRate() const;

    // A hit at (x, y) on the batter at v m/s; a rimshot catches the hoop too.
    void hit (double x, double y, double v, bool rim, int stick = -1);

    // 0 tuning (batter frequency factor), 1 muffling (batter loss factor),
    // 2 strainer (wire preload factor; < 0 lifts the wires), 3 stick hardness,
    // 4 close mics, 5 overhead, 6 master.
    void setParam (int what, double value);

    void process (float* mono, int n);   // adds n samples at the model rate

    int getActiveModes() const;
    bool isAsleep() const;
    void reset();   // silence, at once: back to the drum as loaded, settings kept

    struct Impl;

private:
    std::unique_ptr<Impl> impl, settled;
};

//==============================================================================
// A streaming windowed-sinc resampler for stereo: from a source rate to the
// host's, the source pulled as needed. Equal rates pass straight through.
class Resampler
{
public:
    void prepare (double sourceRate, double targetRate, int maxOutputBlock);
    void reset();

    // How many new source samples producing n output samples takes.
    int sourceNeeded (int n) const;

    // Takes sourceNeeded (n) source samples (added to history) and writes n.
    void process (const float* inL, const float* inR, int numIn, float* outL, float* outR, int n);

private:
    double ratio = 1.0;          // source samples per output sample
    bool bypass = true;
    int halfWidth = 0;           // taps either side, in source samples
    double step = 0.0;           // table steps per source sample
    std::vector<float> table;    // the kernel, h(u) for u >= 0
    std::vector<float> histL, histR;
    std::int64_t pushed = 0;     // source samples pushed so far
    std::int64_t base = 0;       // next output's time: base + frac (source samples)
    double frac = 0.0;
    int histSize = 0;
    int maxOut = 1;
};

} // namespace carve::plugins::drummodel

#include "FactoryPresets.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "AcousticDrumsPlugin.h"
#include "BassModelPlugin.h"
#include "DrumModelPlugin.h"
#include "DrumSynthPlugin.h"
#include "ElectricBassPlugin.h"
#include "GabberKickPlugin.h"
#include "PianoPlugin.h"
#include "VocalModelPlugin.h"
#include "sync/EngineIds.h"

namespace carve::plugins::presets
{

namespace
{
    // One knob, by the name of its property in the plugin's tree, at the
    // value the tree holds (the parameter's own units, not 0..1).
    struct Setting
    {
        const char* id;
        double value;
    };

    struct Preset
    {
        const char* name;
        std::vector<Setting> settings;
    };

    struct Bank
    {
        const char* pluginType;
        std::vector<const char*> keep;   // see replaceSettings
        std::vector<Preset> presets;
    };

    //==============================================================================
    // Slate (4OSC). Every sound names its oscillators, filter and envelopes
    // outright: what a preset leaves out goes back to the *engine's* default
    // (a sine through no filter with a tenth of a second of attack), not to
    // the brighter start a new Slate is given (EditSync's applyNewSynthDefaults).
    //
    // Units as the tree holds them: waveShape 0 off 1 sine 2 square 3 saw
    // 4 triangle 5 noise; level and masterLevel dB; tune semitones, fineTune
    // cents; filterType 0 off 1 low pass 2 high pass 3 band pass; filterSlope
    // dB/oct; filterFreq a note number (69 = 440 Hz); resonance, sustain,
    // velocity and key tracking 0..100; times seconds; voiceMode 0 mono
    // 1 legato 2 poly; legato (glide) ms.
    //
    // Unison is what makes a 4OSC sound wide, and each unison voice is an
    // oscillator more per note -- the CPU column in the PR measures it.
    Bank slate()
    {
        return { te::FourOscPlugin::xmlTypeName, {}, {
            { "Fat Saw Bass", {
                { "waveShape1", 3 }, { "voices1", 2 }, { "detune1", 0.08 }, { "spread1", 30 },
                { "waveShape2", 2 }, { "tune2", -12 }, { "level2", -4 },
                { "filterType", 1 }, { "filterSlope", 24 }, { "filterFreq", 62 }, { "filterResonance", 22 },
                { "filterAmount", 0.35 }, { "filterKey", 40 },
                { "filterAttack", 0 }, { "filterDecay", 0.28 }, { "filterSustain", 25 }, { "filterRelease", 0.15 },
                { "ampAttack", 0.001 }, { "ampDecay", 0.3 }, { "ampSustain", 100 }, { "ampRelease", 0.06 },
                { "voiceMode", 1 }, { "masterLevel", -4 } } },
            { "Sub Bass", {
                { "waveShape1", 1 },
                { "waveShape2", 4 }, { "tune2", 12 }, { "level2", -14 },
                { "filterType", 1 }, { "filterFreq", 76 }, { "filterResonance", 0 },
                { "ampAttack", 0.003 }, { "ampDecay", 0.2 }, { "ampSustain", 100 }, { "ampRelease", 0.08 },
                { "voiceMode", 1 }, { "masterLevel", -2 } } },
            { "Reese Bass", {
                { "waveShape1", 3 }, { "voices1", 3 }, { "detune1", 0.22 }, { "spread1", 60 },
                { "waveShape2", 3 }, { "tune2", -12 }, { "voices2", 2 }, { "detune2", 0.12 }, { "level2", 0 },
                { "filterType", 1 }, { "filterSlope", 24 }, { "filterFreq", 72 }, { "filterResonance", 15 },
                { "filterKey", 50 },
                { "ampAttack", 0.004 }, { "ampDecay", 0.3 }, { "ampSustain", 100 }, { "ampRelease", 0.12 },
                { "voiceMode", 1 }, { "legato", 30 }, { "masterLevel", 0 } } },
            { "Acid Squelch", {
                { "waveShape1", 3 },
                { "filterType", 1 }, { "filterSlope", 24 }, { "filterFreq", 56 }, { "filterResonance", 78 },
                { "filterAmount", 0.7 }, { "filterVelocity", 40 },
                { "filterAttack", 0 }, { "filterDecay", 0.22 }, { "filterSustain", 10 }, { "filterRelease", 0.1 },
                { "ampAttack", 0.001 }, { "ampDecay", 0.25 }, { "ampSustain", 85 }, { "ampRelease", 0.05 },
                { "voiceMode", 1 }, { "legato", 60 }, { "masterLevel", 0 } } },
            { "Supersaw Lead", {
                { "waveShape1", 3 }, { "voices1", 7 }, { "detune1", 0.28 }, { "spread1", 90 },
                { "waveShape2", 3 }, { "tune2", 12 }, { "voices2", 3 }, { "detune2", 0.18 }, { "spread2", 70 }, { "level2", -3 },
                { "filterType", 1 }, { "filterFreq", 118 }, { "filterResonance", 8 },
                { "ampAttack", 0.005 }, { "ampDecay", 0.4 }, { "ampSustain", 90 }, { "ampRelease", 0.25 },
                { "masterLevel", 0 } } },
            { "Square Lead", {
                { "waveShape1", 2 }, { "pulseWidth1", 0.38 },
                { "waveShape2", 2 }, { "tune2", 12 }, { "fineTune2", 6 }, { "level2", -10 },
                { "filterType", 1 }, { "filterFreq", 100 }, { "filterResonance", 12 }, { "filterKey", 60 },
                { "ampAttack", 0.003 }, { "ampDecay", 0.3 }, { "ampSustain", 90 }, { "ampRelease", 0.12 },
                { "voiceMode", 1 }, { "legato", 45 }, { "masterLevel", -2 } } },
            { "Chip Lead", {
                { "waveShape1", 2 }, { "pulseWidth1", 0.25 },
                { "ampAttack", 0.001 }, { "ampDecay", 0.1 }, { "ampSustain", 100 }, { "ampRelease", 0.02 },
                { "voiceMode", 0 }, { "masterLevel", -1 } } },
            { "Soft Pluck", {
                { "waveShape1", 3 }, { "voices1", 2 }, { "detune1", 0.06 }, { "spread1", 40 },
                { "waveShape2", 2 }, { "tune2", 12 }, { "level2", -12 },
                { "filterType", 1 }, { "filterSlope", 24 }, { "filterFreq", 58 }, { "filterResonance", 18 },
                { "filterAmount", 0.55 }, { "filterKey", 50 },
                { "filterAttack", 0 }, { "filterDecay", 0.25 }, { "filterSustain", 0 }, { "filterRelease", 0.3 },
                { "ampAttack", 0.001 }, { "ampDecay", 0.7 }, { "ampSustain", 15 }, { "ampRelease", 0.35 },
                { "masterLevel", 0 } } },
            { "Brass Stab", {
                { "waveShape1", 3 }, { "voices1", 2 }, { "detune1", 0.1 }, { "spread1", 50 },
                { "waveShape2", 3 }, { "tune2", -12 }, { "level2", -8 },
                { "filterType", 1 }, { "filterSlope", 12 }, { "filterFreq", 66 }, { "filterResonance", 10 },
                { "filterAmount", 0.45 }, { "filterKey", 50 }, { "filterVelocity", 40 },
                { "filterAttack", 0.04 }, { "filterDecay", 0.3 }, { "filterSustain", 35 }, { "filterRelease", 0.2 },
                { "ampAttack", 0.02 }, { "ampDecay", 0.3 }, { "ampSustain", 80 }, { "ampRelease", 0.15 },
                { "masterLevel", 0 } } },
            { "Warm Pad", {
                { "waveShape1", 3 }, { "voices1", 4 }, { "detune1", 0.14 }, { "spread1", 80 },
                { "waveShape2", 4 }, { "tune2", -12 }, { "voices2", 2 }, { "detune2", 0.08 }, { "spread2", 60 }, { "level2", -6 },
                { "filterType", 1 }, { "filterSlope", 24 }, { "filterFreq", 76 }, { "filterResonance", 10 },
                { "filterAmount", 0.2 }, { "filterKey", 30 },
                { "filterAttack", 0.9 }, { "filterDecay", 1.5 }, { "filterSustain", 60 }, { "filterRelease", 1.2 },
                { "ampAttack", 0.5 }, { "ampDecay", 1.0 }, { "ampSustain", 100 }, { "ampRelease", 1.2 },
                { "masterLevel", 0 } } },
            { "Organ", {
                { "waveShape1", 1 },
                { "waveShape2", 1 }, { "tune2", 12 }, { "level2", -5 },
                { "waveShape3", 1 }, { "tune3", 19 }, { "level3", -12 },
                { "waveShape4", 1 }, { "tune4", -12 }, { "level4", -6 },
                { "ampAttack", 0.004 }, { "ampDecay", 0.1 }, { "ampSustain", 100 }, { "ampRelease", 0.05 },
                { "masterLevel", -6 } } },
            { "Glass Keys", {
                { "waveShape1", 4 },
                { "waveShape2", 1 }, { "tune2", 24 }, { "level2", -14 },
                { "filterType", 1 }, { "filterFreq", 100 },
                { "ampAttack", 0.002 }, { "ampDecay", 1.2 }, { "ampSustain", 20 }, { "ampRelease", 0.6 },
                { "masterLevel", 0 } } },
        } };
    }

    //==============================================================================
    // Drum Synth. Tunes in Hz, decays in seconds, levels in dB (-24..+6);
    // drive, sweep, click and snappy 0..1.
    Bank drumSynth()
    {
        return { DrumSynthPlugin::xmlTypeName, {}, {
            { "808 Classic", {
                { "kickDecay", 0.8 } } },
            { "Trap Boom", {
                { "kickTune", 44 }, { "kickDecay", 1.6 }, { "kickSweep", 0.35 }, { "kickDrive", 0.35 },
                { "kickClick", 0.15 }, { "kickLevel", 2 },
                { "snareSnappy", 0.75 }, { "snareDecay", 0.18 }, { "clapDecay", 0.25 },
                { "closedHatDecay", 0.035 }, { "closedHatLevel", -3 }, { "openHatDecay", 0.35 } } },
            { "Punchy House", {
                { "kickTune", 54 }, { "kickDecay", 0.42 }, { "kickSweep", 0.6 }, { "kickDrive", 0.35 },
                { "kickClick", 0.5 }, { "kickLevel", 1 },
                { "clapDecay", 0.3 }, { "clapLevel", 1 },
                { "closedHatDecay", 0.05 }, { "openHatDecay", 0.32 } } },
            { "Techno Rumble", {
                { "kickTune", 47 }, { "kickDecay", 1.0 }, { "kickSweep", 0.55 }, { "kickDrive", 0.7 },
                { "kickClick", 0.4 }, { "kickLevel", 1 },
                { "closedHatDecay", 0.04 }, { "openHatDecay", 0.25 }, { "clapDecay", 0.35 } } },
            { "Hard Distorted", {
                { "kickTune", 52 }, { "kickDecay", 0.7 }, { "kickSweep", 0.7 }, { "kickDrive", 1.0 },
                { "kickClick", 0.8 },
                { "snareSnappy", 0.9 }, { "snareDecay", 0.22 }, { "snareLevel", 2 } } },
            { "Lo-Fi Dusty", {
                { "kickTune", 52 }, { "kickDecay", 0.5 }, { "kickDrive", 0.5 }, { "kickClick", 0.08 },
                { "snareSnappy", 0.35 }, { "snareDecay", 0.15 },
                { "closedHatDecay", 0.05 }, { "closedHatLevel", -8 }, { "openHatLevel", -8 }, { "rimLevel", -3 } } },
            { "Tight Funk", {
                { "kickTune", 58 }, { "kickDecay", 0.3 }, { "kickClick", 0.6 },
                { "snareSnappy", 0.85 }, { "snareDecay", 0.12 }, { "snareLevel", 2 },
                { "closedHatDecay", 0.03 }, { "openHatDecay", 0.2 }, { "tomDecay", 0.25 } } },
            { "Big Room Clap", {
                { "kickTune", 50 }, { "kickDecay", 0.6 }, { "kickDrive", 0.45 }, { "kickClick", 0.45 },
                { "clapDecay", 0.55 }, { "clapLevel", 3 },
                { "snareDecay", 0.3 }, { "openHatDecay", 0.6 }, { "tomDecay", 0.6 } } },
        } };
    }

    //==============================================================================
    // Modelled Piano. The pedal is how the part is played: a preset leaves it.
    Bank piano()
    {
        return { PianoPlugin::xmlTypeName, { "pedal" }, {
            { "Concert Grand", {
                { "hardness", 0.2 }, { "brightness", 0.8 }, { "board", 1.2 }, { "reverb", 0.25 } } },
            { "Warm Felt", {
                { "hardness", -1.2 }, { "brightness", 0.25 }, { "knock", 1.6 }, { "board", 1.1 }, { "reverb", 0.2 } } },
            { "Bright Pop", {
                { "hardness", 1.0 }, { "brightness", 1.4 }, { "board", 1.1 }, { "reverb", 0.12 }, { "volume", 0.9 } } },
            { "Honky-Tonk", {
                { "hardness", 0.6 }, { "detune", 4.0 }, { "brightness", 1.0 }, { "reverb", 0.1 } } },
            { "Dry Close Mic", {
                { "brightness", 0.9 }, { "knock", 1.5 }, { "board", 0.85 }, { "reverb", 0.0 }, { "volume", 0.9 } } },
            { "Ballad Hall", {
                { "hardness", -0.3 }, { "sympathetic", 2.0 }, { "brightness", 0.5 }, { "reverb", 0.5 } } },
        } };
    }

    //==============================================================================
    // Electric Bass. The legato technique and mono switch are how the part is
    // written: a preset leaves them.
    Bank electricBass()
    {
        return { ElectricBassPlugin::xmlTypeName, { "technique", "mono" }, {
            { "Round Fingers", {
                { "attack", -3 }, { "tone", -1 }, { "body", 3 }, { "level", 5 } } },
            { "Thick Amp", {
                { "body", 4 }, { "tone", -0.5 },
                { "ampOn", 1 }, { "ampDrive", 0.4 }, { "ampBlend", 0.6 }, { "ampBass", 6 }, { "ampMid", 2 },
                { "ampMidFreq", 500 }, { "ampTreble", -2 }, { "ampLevel", 4 } } },
            { "Punchy Pick", {
                { "pick", 0.08 }, { "attack", 4 }, { "tone", 1.5 }, { "level", 1 },
                { "ampOn", 1 }, { "ampDrive", 0.3 }, { "ampMid", 4 }, { "ampMidFreq", 900 }, { "ampBass", 4 } } },
            { "Muted Motown", {
                { "pick", 0.25 }, { "mute", 0.6 }, { "tone", -3 }, { "body", 4 }, { "sustain", 0.6 }, { "level", 5 } } },
            { "Growl Drive", {
                { "buzz", 4 }, { "attack", 2 },
                { "ampOn", 1 }, { "ampDrive", 0.85 }, { "ampBlend", 0.8 }, { "ampMid", 6 }, { "ampMidFreq", 1000 },
                { "ampTreble", 3 }, { "ampLevel", 0 } } },
            { "Fretless Smooth", {
                { "fretless", 1 }, { "tone", -2 }, { "attack", -6 }, { "sustain", 1.6 }, { "slideTime", 0.2 }, { "level", 5 } } },
            { "Dub Deep", {
                { "attack", -10 }, { "tone", -5 }, { "body", 6 }, { "level", 1.5 },
                { "ampOn", 1 }, { "ampDrive", 0.15 }, { "ampBass", 8 }, { "ampTreble", -8 } } },
            { "Clanky Rock", {
                { "buzz", 6 }, { "attack", 6 }, { "tone", 1.5 },
                { "ampOn", 1 }, { "ampDrive", 0.5 }, { "ampTreble", 4 }, { "ampBass", 3 }, { "ampMid", 3 }, { "ampMidFreq", 1500 } } },
        } };
    }

    //==============================================================================
    // Acoustic Drums. Per drum, `<drum>_<knob>`: level, attack, body and
    // noise in dB, tune in semitones, decay as a factor, tone in dB/oct. The
    // first preset is the recording as it was.
    Bank acousticDrums()
    {
        return { AcousticDrumsPlugin::xmlTypeName, {}, {
            { "Natural Studio", {} },
            { "Tight Rock", {
                { "kick_attack", 3 }, { "kick_body", 3 }, { "kick_decay", 0.7 },
                { "snare_attack", 3 }, { "snare_body", 2 },
                { "htom_decay", 0.8 }, { "mtom_decay", 0.8 }, { "ltom_decay", 0.8 }, { "ftom_decay", 0.8 },
                { "compOn", 1 }, { "compThreshold", -20 }, { "compRatio", 4 }, { "compAttack", 10 },
                { "compRelease", 120 }, { "compMakeup", 6 }, { "compMix", 0.6 } } },
            { "Fat & Squashed", {
                { "kick_body", 4 }, { "kick_tune", -1 },
                { "snare_body", 3 }, { "snare_decay", 1.3 },
                { "compOn", 1 }, { "compThreshold", -28 }, { "compRatio", 8 }, { "compAttack", 3 },
                { "compRelease", 80 }, { "compMakeup", 12 }, { "compMix", 0.5 } } },
            { "Dry Funk", {
                { "kick_decay", 0.6 }, { "kick_attack", 2 },
                { "snare_decay", 0.7 }, { "snare_tone", 1 }, { "snare_tune", 1 },
                { "hhc_tone", 1 }, { "hho_decay", 0.7 },
                { "compOn", 1 }, { "compThreshold", -16 }, { "compRatio", 3 }, { "compAttack", 15 },
                { "compRelease", 100 }, { "compMakeup", 2 }, { "compMix", 0.8 } } },
            { "Big Toms Ballad", {
                { "kick_tune", -2 }, { "kick_body", 3 }, { "kick_decay", 1.3 },
                { "snare_decay", 1.5 }, { "snare_tune", -1 },
                { "htom_decay", 1.5 }, { "mtom_decay", 1.5 }, { "ltom_decay", 1.6 }, { "ftom_decay", 1.7 },
                { "crash_decay", 1.4 }, { "ride_decay", 1.3 } } },
            { "Lo-Fi Pitched Down", {
                { "kick_tune", -3 }, { "snare_tune", -3 }, { "rim_tune", -3 }, { "xstick_tune", -3 },
                { "hhc_tune", -3 }, { "hhp_tune", -3 }, { "hhh_tune", -3 }, { "hho_tune", -3 },
                { "htom_tune", -3 }, { "mtom_tune", -3 }, { "ltom_tune", -3 }, { "ftom_tune", -3 },
                { "crash_tune", -3 }, { "ride_tune", -3 },
                { "hhc_tone", -3 }, { "hho_tone", -3 }, { "crash_tone", -3 }, { "ride_tone", -3 },
                { "snare_tone", -2 }, { "snare_noise", -4 },
                { "compOn", 1 }, { "compThreshold", -26 }, { "compRatio", 6 }, { "compAttack", 5 },
                { "compRelease", 150 }, { "compMakeup", 9 }, { "compMix", 0.7 } } },
            { "Metal Click", {
                { "kick_attack", 8 }, { "kick_tone", 3 }, { "kick_decay", 0.5 },
                { "snare_attack", 4 }, { "snare_body", 2 },
                { "compOn", 1 }, { "compThreshold", -18 }, { "compRatio", 4 }, { "compAttack", 8 },
                { "compRelease", 90 }, { "compMakeup", 3 }, { "compMix", 0.7 } } },
        } };
    }

    //==============================================================================
    // Modelled Drums. Tunings in semitones; mute, beater and stick as log2 of
    // a factor; glide, wires, levels and mics as factors; room 0..1. The
    // first preset is the kit as modelled.
    Bank drumModel()
    {
        return { DrumModelPlugin::xmlTypeName, {}, {
            { "Studio Kit", {} },
            { "Fat Rock", {
                { "kickTune", -2 }, { "kickMute", 2.0 }, { "kickBeater", 1.0 }, { "kickLevel", 1.2 },
                { "snareTune", -2 }, { "snareMute", 0.5 }, { "snareStick", 0.8 }, { "snareRim", 1 },
                { "close", 1.3 }, { "room", 0.35 } } },
            { "Dead 70s", {
                { "kickMute", 3.2 }, { "snareMute", 1.5 }, { "snareWires", 0.7 },
                { "tom1Mute", 1.2 }, { "tom2Mute", 1.2 }, { "tom3Mute", 1.2 },
                { "overhead", 0.6 }, { "room", 0.05 }, { "volume", 1.2 } } },
            { "Big Room", {
                { "kickTune", -1 }, { "snareTune", -1 },
                { "tom1Glide", 1.5 }, { "tom2Glide", 1.5 }, { "tom3Glide", 1.5 },
                { "overhead", 1.4 }, { "room", 0.8 } } },
            { "Tight Funk", {
                { "kickTune", 2 }, { "kickMute", 2.8 },
                { "snareTune", 3 }, { "snareMute", 0.8 }, { "snareStick", 0.5 },
                { "room", 0.1 } } },
            { "Jazz Open", {
                { "kickTune", 5 }, { "kickMute", 0.8 },
                { "snareTune", 4 }, { "snareWires", 0.6 },
                { "tom1Tune", 4 }, { "tom2Tune", 4 }, { "tom3Tune", 4 },
                { "rideLevel", 1.0 }, { "close", 0.6 }, { "overhead", 1.4 }, { "room", 0.3 } } },
        } };
    }

    //==============================================================================
    // Modelled Voice. What a preset leaves out reads as the Default
    // character (VocalModelPlugin's defaults). The language and the live
    // lyric are the part's, not the voice's: a preset leaves them.
    Bank vocalModel()
    {
        return { VocalModelPlugin::xmlTypeName, { "english", "liveLyric" }, {
            { "Pop Lead", {
                // Woman, with the ring and air a lead needs to sit on top of a mix.
                { "tractLength", 13.5 }, { "pharynx", 0.75 }, { "jitter", 0.8 }, { "shimmer", 2.5 },
                { "fold", 0.75 }, { "throat", 0.64 }, { "nasal", 0.81 }, { "clarity", 0.84 },
                { "vibrato", 1 }, { "register", 0.67 }, { "twang", 0.38 }, { "resonance", 1.22 },
                { "ring", 3 }, { "air", 3 }, { "body", 1 }, { "reverb", 0.12 } } },
            { "Bright Idol", {
                // Genki, brighter and more forward.
                { "tractLength", 11.6 }, { "pharynx", 0.75 }, { "jitter", 0.8 }, { "shimmer", 2.5 },
                { "breath", 1 }, { "brightness", 0.8 }, { "throat", 0.9 }, { "nasal", 0.1 }, { "press", 0.5 },
                { "clarity", 1.15 }, { "body", -1 }, { "ring", 5 }, { "vibrato", 0.8 }, { "register", 0 },
                { "twang", 0.3 }, { "resonance", 1.2 }, { "air", 4 }, { "reverb", 0.14 } } },
            { "Breathy Ballad", {
                // Sweet, with more breath and a longer room.
                { "tractLength", 12 }, { "pharynx", 0.75 }, { "jitter", 0.8 }, { "shimmer", 2.5 },
                { "fold", 0.74 }, { "breath", 4.5 }, { "brightness", 0.35 }, { "throat", 1 }, { "nasal", 0.5 },
                { "press", -0.15 }, { "clarity", 0.9 }, { "body", 2 }, { "vibrato", 1.4 }, { "register", 0.3 },
                { "twang", 0 }, { "resonance", 1 }, { "air", 5 }, { "reverb", 0.3 }, { "volume", 1.1 } } },
            { "Power Belt", {
                // Standard, pressed and twangy: the chest voice of a rock chorus.
                { "tractLength", 12.5 }, { "pharynx", 0.8 }, { "jitter", 0.8 }, { "shimmer", 2.5 },
                { "fold", 0.76 }, { "breath", 1.2 }, { "brightness", 0.6 }, { "throat", 1 }, { "nasal", 0 },
                { "press", 0.6 }, { "clarity", 1.05 }, { "body", 3 }, { "ring", 6 }, { "vibrato", 1.2 },
                { "register", 0 }, { "twang", 0.6 }, { "resonance", 1.1 }, { "strength", 1.4 },
                { "reverb", 0.15 } } },
            { "Deep Male", {
                { "tractLength", 15 }, { "pharynx", 1.0 }, { "jitter", 0.6 }, { "shimmer", 2 },
                { "fold", 0.85 }, { "breath", 1.5 }, { "brightness", 0.4 }, { "throat", 1.1 }, { "nasal", 0 },
                { "press", 0.1 }, { "clarity", 1 }, { "body", 4 }, { "ring", 2 }, { "vibrato", 1 },
                { "register", 0 }, { "twang", 0.1 }, { "resonance", 1 }, { "reverb", 0.14 }, { "volume", 1.0 } } },
            { "Whisper", {
                { "breath", 6 }, { "press", -0.8 }, { "jitter", 1.5 }, { "shimmer", 5 },
                { "vibrato", 0.3 }, { "air", 6 }, { "body", 1 }, { "reverb", 0.2 }, { "volume", 1.1 } } },
            { "Synth Vox", {
                // Steady as a machine: no vibrato, drift or roughness, and a
                // ringing, clear vowel.
                { "tractLength", 11.8 }, { "pharynx", 0.75 }, { "fold", 0.7 }, { "breath", 0.6 },
                { "brightness", 0.7 }, { "nasal", 0.2 }, { "press", 0.3 }, { "clarity", 1.2 },
                { "vibrato", 0 }, { "drift", 0 }, { "jitter", 0 }, { "shimmer", 0 },
                { "register", 0.4 }, { "twang", 0.5 }, { "resonance", 1.6 }, { "ring", 4 }, { "air", 2 },
                { "reverb", 0.1 } } },
        } };
    }

    const std::vector<Bank>& getBanks()
    {
        static const std::vector<Bank> banks { slate(), drumSynth(), piano(), electricBass(),
                                               acousticDrums(), drumModel(), vocalModel() };
        return banks;
    }

    const Bank* findBank (te::Plugin& plugin)
    {
        if (dynamic_cast<te::ExternalPlugin*> (&plugin) != nullptr)
            return nullptr;

        const auto type = plugin.getPluginType();

        for (const auto& bank : getBanks())
            if (type == bank.pluginType)
                return &bank;

        return nullptr;
    }

    juce::Array<juce::Identifier> keepList (const Bank& bank)
    {
        juce::Array<juce::Identifier> keep;

        for (auto* id : bank.keep)
            keep.add (id);

        return keep;
    }

    // Close enough to be the same knob position: a value that has been through
    // a float parameter and back is not bit-identical to the table's double,
    // and one that has been through a file is text.
    bool sameValue (const juce::var& a, double b)
    {
        // A tree read back from a saved song holds its numbers as text.
        if (a.isString() && ! a.toString().trim().containsOnly ("0123456789.-+eE"))
            return false;

        const auto v = a.isString() ? a.toString().getDoubleValue() : (double) a;
        return std::abs (v - b) <= 1.0e-4 * std::max (1.0, std::abs (b));
    }

    // Whether a property holds what its parameter reads while the property is
    // absent. Only automatable parameters know their default; a plain
    // property (a 4OSC wave shape, a switch) is never written back once
    // removed, so one that is present has been set.
    bool isAtDefault (te::Plugin& plugin, const juce::Identifier& property)
    {
        for (auto* parameter : plugin.getAutomatableParameters())
            if (parameter != nullptr && parameter->paramID == property.toString())
                if (const auto value = parameter->getDefaultValue())
                    return sameValue (plugin.state[property], (double) *value);

        return false;
    }
} // namespace

//==============================================================================
// None of this may travel in a preset: `id` is the EditItemID, the window
// properties are where the editor was last opened, `enabled` is the bypass
// switch the user set on this slot, and carveEffectId is what EditSync
// reconciles the chain by -- copying that onto another plugin would leave two
// slots claiming the same effect.
bool isStructuralProperty (const juce::Identifier& property)
{
    return property == te::IDs::type
            || property == te::IDs::id
            || property == te::IDs::enabled
            || property == te::IDs::process
            || property == te::IDs::windowX
            || property == te::IDs::windowY
            || property == te::IDs::windowLocked
            || property == te::IDs::quickParamName
            || property == sync::effectIdProperty;
}

// Automation belongs to the song, not to the sound: a preset load should
// leave a curve the user drew exactly where it was.
bool isStructuralChild (const juce::Identifier& type)
{
    return type == te::IDs::AUTOMATIONCURVE
            || type == te::IDs::MACROPARAMETERS
            || type == te::IDs::MODIFIERASSIGNMENTS;
}

void stripStructuralParts (juce::ValueTree& state)
{
    for (int i = state.getNumProperties(); --i >= 0;)
        if (const auto name = state.getPropertyName (i); isStructuralProperty (name))
            state.removeProperty (name, nullptr);

    for (int i = state.getNumChildren(); --i >= 0;)
        if (isStructuralChild (state.getChild (i).getType()))
            state.removeChild (i, nullptr);
}

void replaceSettings (te::Plugin& plugin, const juce::ValueTree& stored,
                      const juce::Array<juce::Identifier>& keep)
{
    // Deliberately not Plugin::restorePluginStateFromValueTree(): the base
    // implementation is a jassertfalse, and several of the internal effects the
    // app offers -- lowpass among them -- never override it. Writing the
    // properties is what the ones that do override it do anyway, because the
    // tree is the store.
    auto state = plugin.state;

    auto leftAlone = [&keep] (const juce::Identifier& name)
    {
        return isStructuralProperty (name) || keep.contains (name);
    };

    // A CachedValue returns its default while its property is absent, so a
    // plugin's tree only ever holds what has been moved away from the defaults
    // -- and a preset is exactly that much. Clearing the settings the preset
    // does not mention is therefore what puts them back to default; without
    // it, loading a second preset would leave the first one's stray values
    // behind.
    for (int i = state.getNumProperties(); --i >= 0;)
        if (const auto name = state.getPropertyName (i); ! leftAlone (name) && ! stored.hasProperty (name))
            state.removeProperty (name, nullptr);

    for (int i = 0; i < stored.getNumProperties(); ++i)
        if (const auto name = stored.getPropertyName (i); ! leftAlone (name))
            state.setProperty (name, stored[name], nullptr);

    // 4OSC keeps its modulation matrix in a child tree, and the same rule
    // applies to it: the preset's children replace the plugin's, and one the
    // preset does not carry goes away. Automation curves and the rest are
    // structural, so they stay where they were.
    for (int i = state.getNumChildren(); --i >= 0;)
        if (const auto type = state.getChild (i).getType();
            ! isStructuralChild (type) && ! stored.getChildWithName (type).isValid())
            state.removeChild (i, nullptr);

    for (const auto& child : stored)
    {
        if (isStructuralChild (child.getType()))
            continue;

        state.removeChild (state.getChildWithName (child.getType()), nullptr);
        state.appendChild (child.createCopy(), nullptr);
    }

    // The parameters cache their value alongside the tree, and a property write
    // is not enough to move the cached one.
    for (auto* parameter : plugin.getAutomatableParameters())
        if (parameter != nullptr)
            parameter->updateFromAttachedValue();
}

//==============================================================================
juce::StringArray getFactoryPresetNames (te::Plugin& plugin)
{
    juce::StringArray names;

    if (dynamic_cast<BassModelPlugin*> (&plugin) != nullptr)
    {
        for (int i = 0; i < bassmodel::numFactoryPresets; ++i)
            names.add (bassmodel::getFactoryPresetName (i));
    }
    else if (dynamic_cast<GabberKickPlugin*> (&plugin) != nullptr)
    {
        for (int i = 0; i < gabber::numFactoryPresets; ++i)
            names.add (gabber::getFactoryPresetName (i));
    }
    else if (auto* bank = findBank (plugin))
    {
        for (const auto& preset : bank->presets)
            names.add (preset.name);
    }

    return names;
}

bool hasFactoryPresets (te::Plugin& plugin)
{
    return ! getFactoryPresetNames (plugin).isEmpty();
}

bool applyFactoryPreset (te::Plugin& plugin, int index)
{
    if (auto* bass = dynamic_cast<BassModelPlugin*> (&plugin))
    {
        if (! juce::isPositiveAndBelow (index, bassmodel::numFactoryPresets))
            return false;

        bass->setFactoryPreset (index);
        return true;
    }

    if (auto* kick = dynamic_cast<GabberKickPlugin*> (&plugin))
    {
        if (! juce::isPositiveAndBelow (index, gabber::numFactoryPresets))
            return false;

        kick->setPatch (gabber::getFactoryPreset (index));
        return true;
    }

    auto* bank = findBank (plugin);

    if (bank == nullptr || ! juce::isPositiveAndBelow (index, (int) bank->presets.size()))
        return false;

    juce::ValueTree stored (te::IDs::PLUGIN);

    for (const auto& setting : bank->presets[(size_t) index].settings)
        stored.setProperty (setting.id, setting.value, nullptr);

    replaceSettings (plugin, stored, keepList (*bank));
    return true;
}

int findMatchingFactoryPreset (te::Plugin& plugin)
{
    if (auto* kick = dynamic_cast<GabberKickPlugin*> (&plugin))
        return kick->getMatchingFactoryPreset();

    if (auto* bass = dynamic_cast<BassModelPlugin*> (&plugin))
        return bass->getMatchingFactoryPreset();

    auto* bank = findBank (plugin);

    if (bank == nullptr)
        return -1;

    const auto keep = keepList (*bank);
    const auto& state = plugin.state;

    for (int i = 0; i < (int) bank->presets.size(); ++i)
    {
        const auto& settings = bank->presets[(size_t) i].settings;
        bool matches = true;

        // Every setting the preset makes is in the tree, at its value...
        for (const auto& setting : settings)
            if (! state.hasProperty (setting.id) || ! sameValue (state[setting.id], setting.value))
                matches = false;

        // ...and nothing else has been moved. A setting the preset leaves out
        // can still be in the tree, at its default: clearing it makes its
        // parameter write the default straight back.
        for (int p = 0; matches && p < state.getNumProperties(); ++p)
        {
            const auto name = state.getPropertyName (p);

            if (isStructuralProperty (name) || keep.contains (name)
                 || std::any_of (settings.begin(), settings.end(),
                                 [&name] (const Setting& s) { return name == juce::StringRef (s.id); }))
                continue;

            if (! isAtDefault (plugin, name))
                matches = false;
        }

        if (matches)
            return i;
    }

    return -1;
}

} // namespace carve::plugins::presets

#pragma once

#include <cstring>

// Small fixed-width vectors for the Modelled Drums engines' inner loops (the
// PhysKit originals use Zig's @Vector the same way): clang's and gcc's vector
// extensions, so the loops are vectorised whatever the auto-vectoriser makes
// of them, and plain arrays elsewhere. Loads and stores are unaligned.
namespace carve::plugins::drummodel::simd
{

#if defined(__clang__) || defined(__GNUC__)

typedef float F4 __attribute__ ((vector_size (16)));
typedef double D2 __attribute__ ((vector_size (16)));

inline F4 load (const float* p)            { F4 v; std::memcpy (&v, p, sizeof (v)); return v; }
inline void store (float* p, F4 v)         { std::memcpy (p, &v, sizeof (v)); }
inline F4 splat (float x)                  { return F4 { x, x, x, x }; }
inline float sum (F4 v)                    { return (v[0] + v[1]) + (v[2] + v[3]); }

inline D2 load (const double* p)           { D2 v; std::memcpy (&v, p, sizeof (v)); return v; }
inline void store (double* p, D2 v)        { std::memcpy (p, &v, sizeof (v)); }
inline D2 splat (double x)                 { return D2 { x, x }; }
inline double sum (D2 v)                   { return v[0] + v[1]; }

#else

template <typename T, int N>
struct Vec
{
    T v[N];
    T& operator[] (int i)        { return v[i]; }
    T operator[] (int i) const   { return v[i]; }
    friend Vec operator+ (Vec a, Vec b)  { for (int i = 0; i < N; ++i) a.v[i] += b.v[i]; return a; }
    friend Vec operator- (Vec a, Vec b)  { for (int i = 0; i < N; ++i) a.v[i] -= b.v[i]; return a; }
    friend Vec operator* (Vec a, Vec b)  { for (int i = 0; i < N; ++i) a.v[i] *= b.v[i]; return a; }
    Vec& operator+= (Vec b)              { for (int i = 0; i < N; ++i) v[i] += b.v[i]; return *this; }
};

using F4 = Vec<float, 4>;
using D2 = Vec<double, 2>;

inline F4 load (const float* p)            { F4 v; std::memcpy (v.v, p, sizeof (v.v)); return v; }
inline void store (float* p, F4 v)         { std::memcpy (p, v.v, sizeof (v.v)); }
inline F4 splat (float x)                  { return F4 { { x, x, x, x } }; }
inline float sum (F4 v)                    { return (v[0] + v[1]) + (v[2] + v[3]); }

inline D2 load (const double* p)           { D2 v; std::memcpy (v.v, p, sizeof (v.v)); return v; }
inline void store (double* p, D2 v)        { std::memcpy (p, v.v, sizeof (v.v)); }
inline D2 splat (double x)                 { return D2 { { x, x } }; }
inline double sum (D2 v)                   { return v[0] + v[1]; }

#endif

} // namespace carve::plugins::drummodel::simd

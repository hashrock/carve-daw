// The snare engine: a port of the PhysKit project's player/rt/snare.zig
// (itself the time step of the kit's snarelite.c). See DrumModel.h.
//
// Per sample: the batter head in modal coordinates, the resonant head on its
// grid, the wires (stiff strings) and their collisions with it, the modal air
// cavity, the shell / hoop modes, the stick (and for a rimshot the stick on
// the hoop), three mics (top, bottom, overhead). As the original:
//  - batter mic taps: one delay per mic, the rest of each mode's delay as a
//    phase at its frequency;
//  - batter modes sorted by frequency in chunks of 8; a chunk 90dB below the
//    peak since the last hit is frozen, a hit wakes the chunks the stick
//    reaches and those coupled to the air and the shell;
//  - the resonant head and the wires in single precision;
//  - when everything has died away the drum sleeps.

#include "DrumModel.h"
#include "DrumModelBessel.h"
#include "DrumModelSimd.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <numeric>

// The comparisons with == and != on doubles here are exact on purpose, as in
// the PhysKit engines: flags and counts read from the model, a setting that
// changed, a target reached.
#if defined(__clang__)
 #pragma clang diagnostic ignored "-Wfloat-equal"
#elif defined(__GNUC__)
 #pragma GCC diagnostic ignored "-Wfloat-equal"
#endif

namespace carve::plugins::drummodel
{

namespace
{
    using G = float;   // resonant head and wires
    constexpr int RING = 1024;
    constexpr int MASK = RING - 1;
    constexpr std::size_t CH = 8;
    constexpr double RHO = 1.2;
    constexpr double CAIR = 343.0;
    constexpr double PI = 3.14159265358979323846;
    constexpr std::size_t NBODY = 10;

    template <typename T>
    using Vec = std::vector<T>;

    struct Reader
    {
        const unsigned char* p;
        std::size_t count, i = 0;
        bool bad = false;

        double f()
        {
            if (i >= count) { bad = true; return 0; }
            double v;
            std::memcpy (&v, p + 8 * i++, 8);
            return v;
        }
        std::size_t u()   { const auto v = f(); return v > 0 && v < 4e9 ? (std::size_t) v : 0; }

        template <typename T>
        Vec<T> arr (std::size_t n, std::size_t pad = 0)
        {
            Vec<T> a (n + pad);
            for (std::size_t j = 0; j < n; ++j)
                a[j] = (T) f();
            return a;
        }
    };

    // the contact: Hertz law, energy-conserving (drum_common.h contact_hertz)
    inline void hzEval (double K, double Dd, double e1, double p1, double r, double& F, double& dF)
    {
        const double e = e1 + r;
        double ph = 0, dp = 0;
        if (e > 0)
        {
            const double s = std::sqrt (e);
            dp = K * e * s;
            ph = 0.4 * e * dp;
        }
        double Q, dQ;
        if (std::abs (r) < 1e-13)
        {
            const double em = e1 + 0.5 * r;
            const double sm = em > 0 ? std::sqrt (em) : 0;
            Q = K * em * sm;
            dQ = 0.75 * K * sm;
        }
        else
        {
            Q = (ph - p1) / r;
            dQ = (dp - Q) / r;
        }
        const double f = Q + Dd * r;
        F = f > 0 ? f : 0;
        dF = dQ + Dd;
    }

    // Newton started from the root that force Fg would give (the node's force at the previous step):
    // same equations and tolerance, fewer iterations while a wire node stays in contact
    inline double contactHertzG (double k, double K, double lam, double M, double ef, double e1, double e0, double Fg)
    {
        if (ef <= 0 && e1 <= 0 && e0 <= 0) return 0;
        const double Dd = e0 > 0 ? K * lam * e0 * std::sqrt (e0) / (2 * k) : 0;
        const double p1 = e1 > 0 ? 0.4 * K * e1 * e1 * std::sqrt (e1) : 0;
        const double tol = 1e-15;
        const double b = ef - e1;
        double F = 0, dF = 0;
        hzEval (K, Dd, e1, p1, b, F, dF);
        if (F <= 0) return 0;
        double hi = b, lo = b - M * F, r = b;
        if (Fg > 0)
        {
            const double rg = b - M * Fg;
            if (rg > lo && rg < hi)
            {
                r = rg;
                hzEval (K, Dd, e1, p1, r, F, dF);
            }
        }
        for (int it = 0; it < 40; ++it)
        {
            const double g = r - b + M * F;
            if (g > 0) hi = r; else lo = r;
            if (std::abs (g) < tol || hi - lo < tol) break;
            double rn = r - g / (1 + M * dF);
            if (! (rn > lo && rn < hi)) rn = 0.5 * (lo + hi);
            r = rn;
            hzEval (K, Dd, e1, p1, r, F, dF);
        }
        return F;
    }

    //==============================================================================
    // kernels, four lanes wide as the original's f32 x 4 vectors (the arrays never overlap)
    using simd::F4;
    using simd::D2;

    inline G rowStep (G* __restrict o2, const G* __restrict ua, const G* __restrict ub1, const G* __restrict f,
                      std::size_t a, std::size_t b, std::size_t N, G sk, G c2k2, G s1k, G fk, G isk, G pr)
    {
        std::size_t i = a;
        F4 acc = simd::splat (0.0f);
        const F4 vsk = simd::splat (1 - sk), vc = simd::splat (c2k2), vs = simd::splat (s1k), vf = simd::splat (fk),
                 vi = simd::splat (isk), vp = simd::splat (pr), four = simd::splat (4.0f), two = simd::splat (2.0f);
        for (; i + 4 <= b; i += 4)
        {
            const F4 c = simd::load (ua + i), c1 = simd::load (ub1 + i);
            const F4 lap = simd::load (ua + i + 1) + simd::load (ua + i - 1) + simd::load (ua + i + N) + simd::load (ua + i - N) - four * c;
            const F4 lap1 = simd::load (ub1 + i + 1) + simd::load (ub1 + i - 1) + simd::load (ub1 + i + N) + simd::load (ub1 + i - N) - four * c1;
            const F4 v = (two * c - vsk * c1 + vc * lap + vs * (lap - lap1) + vf * (simd::load (f + i) - vp)) * vi;
            simd::store (o2 + i, v);
            acc += v;
        }
        G sum = simd::sum (acc);
        for (; i < b; ++i)
        {
            const G lap = ua[i + 1] + ua[i - 1] + ua[i + N] + ua[i - N] - 4 * ua[i];
            const G lap1 = ub1[i + 1] + ub1[i - 1] + ub1[i + N] + ub1[i - N] - 4 * ub1[i];
            const G v = (2 * ua[i] - (1 - sk) * ub1[i] + c2k2 * lap + s1k * (lap - lap1) + fk * (f[i] - pr)) * isk;
            o2[i] = v;
            sum += v;
        }
        return sum;
    }

    // stiff string interior nodes 2..N
    inline void stringStep (G* __restrict v2, const G* __restrict v, const G* __restrict v1, std::size_t N, G sk, G A, G B, G S1, G isk)
    {
        std::size_t i = 2;
        const F4 vsk = simd::splat (1 - sk), vA = simd::splat (A), vB = simd::splat (B), vS = simd::splat (S1), vi = simd::splat (isk),
                 two = simd::splat (2.0f), four = simd::splat (4.0f), six = simd::splat (6.0f);
        for (; i + 4 <= N + 1; i += 4)
        {
            const F4 c = simd::load (v + i);
            const F4 xx = simd::load (v + i + 1) - two * c + simd::load (v + i - 1);
            const F4 xx1 = simd::load (v1 + i + 1) - two * simd::load (v1 + i) + simd::load (v1 + i - 1);
            const F4 x4 = simd::load (v + i + 2) - four * simd::load (v + i + 1) + six * c - four * simd::load (v + i - 1) + simd::load (v + i - 2);
            simd::store (v2 + i, (two * c - vsk * simd::load (v1 + i) + vA * xx - vB * x4 + vS * (xx - xx1)) * vi);
        }
        for (; i <= N; ++i)
        {
            const G xx = v[i + 1] - 2 * v[i] + v[i - 1];
            const G xx1 = v1[i + 1] - 2 * v1[i] + v1[i - 1];
            const G x4 = v[i + 2] - 4 * v[i + 1] + 6 * v[i] - 4 * v[i - 1] + v[i - 2];
            v2[i] = (2 * v[i] - (1 - sk) * v1[i] + A * xx - B * x4 + S1 * (xx - xx1)) * isk;
        }
    }

    // sum over a row of (u[i+1] - u[i])^2 + (u[i+N] - u[i])^2 (tension modulation)
    inline G gradSq (const G* c, std::size_t a, std::size_t b, std::size_t N)
    {
        F4 acc = simd::splat (0.0f);
        std::size_t i = a;
        for (; i + 4 <= b; i += 4)
        {
            const F4 v = simd::load (c + i);
            const F4 dx = simd::load (c + i + 1) - v;
            const F4 dy = simd::load (c + i + N) - v;
            acc += dx * dx + dy * dy;
        }
        G s = simd::sum (acc);
        for (; i < b; ++i)
        {
            const G dx = c[i + 1] - c[i];
            const G dy = c[i + N] - c[i];
            s += dx * dx + dy * dy;
        }
        return s;
    }

    inline G accSum (const G* a2, const G* a1, const G* a0, std::size_t from, std::size_t to)
    {
        F4 acc = simd::splat (0.0f);
        std::size_t i = from;
        for (; i + 4 <= to; i += 4)
            acc += simd::load (a2 + i) - simd::load (a1 + i) - simd::load (a1 + i) + simd::load (a0 + i);
        G s = simd::sum (acc);
        for (; i < to; ++i)
            s += a2[i] - 2 * a1[i] + a0[i];
        return s;
    }

    inline float dot32 (const float* __restrict a, const float* __restrict b, std::size_t n)
    {
        F4 acc = simd::splat (0.0f);
        std::size_t i = 0;
        for (; i + 4 <= n; i += 4)
            acc += simd::load (a + i) * simd::load (b + i);
        float s = simd::sum (acc);
        for (; i < n; ++i)
            s += a[i] * b[i];
        return s;
    }

    inline void axpy32 (float* __restrict y, const float* __restrict x, float a, std::size_t n)
    {
        const F4 va = simd::splat (a);
        std::size_t i = 0;
        for (; i + 4 <= n; i += 4)
            simd::store (y + i, simd::load (y + i) - va * simd::load (x + i));
        for (; i < n; ++i)
            y[i] -= a * x[i];
    }
}

struct Snare::Impl
{
    double fs = 48000, k = 0, k2 = 0, k2i = 0;
    std::size_t nset = 0, maxd = 0;
    double ra = 0;   // head radius
    // stick and rim
    double Ks = 0, baseKs = 0, lams = 0, ms = 0, alift = 0, rstick = 0, hb = 0;
    double rimMass = 0, rimOff = 0, rimK = 0, stickRad = 0, stickLen = 0;
    bool stickModes = false;
    double stickQ = 0, stickMm = 0;
    double stickF[3] {};
    double hoopArim = 1, hoopA = 1;

    // batter (modes sorted by frequency, padded to whole chunks)
    std::size_t nB = 0, nBp = 0, nch = 0;
    Vec<std::uint32_t> bM, bO;
    Vec<double> bKap, bW2, bSig, bImu, bKN, bVol;
    Vec<double> cA, cB, cC, cD, sA, sB, sC, sD, cS;   // s*: during the settling (extra loss); cS: stick force -> q2
    double bT0 = 0, bNlc = 0, bTeff = 0;
    bool bNonlin = false;
    Vec<double> bq, bq1, bq2, bf, bx;
    Vec<double> tapA[3], tapV[3];
    std::size_t Dc[3] {};
    std::size_t nsp = 0;   // batter modes heard by the bottom mic
    Vec<std::uint32_t> spI;
    std::size_t nrc = 0;
    Vec<std::uint32_t> rci, rcm;
    Vec<double> rcg;
    double Kn[8] {};
    Vec<std::uint32_t> volI;
    std::size_t nvol = 0;
    Vec<char> chOn, chCpl;
    Vec<std::uint32_t> act;
    std::size_t nact = 0;
    double Emax = 0;
    bool cullOn = true;
    double tuneF = 1, lossF = 1;
    // stick footprint: J_m(kappa r) on r = 0..a (Catmull-Rom), cos / sin (m theta) by recurrence
    std::size_t NR = 0, mmax = 0;
    Vec<double> Jt, cmv, smv;

    // resonant head (grid)
    std::size_t RN = 0;
    double Rh = 0, RrhoH = 0, RT0 = 0, Rs0 = 0, Rs1 = 0, Rnlc = 0, RTeff = 0;
    std::size_t Rnp = 0;
    Vec<std::uint32_t> Rpts;
    std::size_t nrow = 0;
    Vec<std::uint32_t> rj, ri0, ri1;
    std::size_t ne = 0;
    Vec<std::uint32_t> edge;
    Vec<double> edgec, ecosN;   // ecosN [order n][edge]: cos(n theta_e)
    bool hasN[8] {};
    std::size_t nlb = 0;
    Vec<std::uint32_t> lb;
    std::size_t nc = 0, nrun = 0;
    Vec<std::uint32_t> runc, runp, runl, cellOf, blk;   // blk: cells of 4 x 4 nodes aligned on the grid
    std::size_t NC4 = 0;
    Vec<G> u, u_1, u_2, fR;
    Vec<float> uc, fc;
    Vec<double> xi, Fe;
    double us = 0, us1 = 0, us2 = 0, Rprad = 0;

    // cavity
    bool cavOn = true;
    std::size_t cavDiv = 2, nq = 0;
    Vec<double> cOm, cZeta, cLam, cZ, cZ1, cZ2, cSp;
    Vec<std::uint32_t> cOff, cI;
    Vec<double> cCb;
    Vec<float> Pr;

    // shell + hoop
    double bodyF[NBODY] {}, bodyQ[NBODY] {}, bodyM[NBODY] {}, bodyA[NBODY] {};
    std::uint32_t bodyN[NBODY] {};
    bool shellBack = true;
    double qb[NBODY] {}, qb1[NBODY] {}, qb2[NBODY] {};
    double QMb = 0;

    struct Wire
    {
        std::size_t Nw = 0;
        double h = 0, c2 = 0, eps = 0, epsT = 0;
        Vec<G> v, v1, v2, rough;
        std::size_t nc = 0, i0 = 0, row = 0, x0 = 0, nx = 0;
        G fy = 0;
        Vec<std::uint32_t> cp, cx;
        Vec<G> cfx, cwi, cw2, d1, g1, g0, gm, ub, ub2;
    };
    std::size_t nw = 0;
    Vec<std::uint32_t> hotList;
    std::size_t wst = 1;
    Vec<Wire> wires;
    double wireS0 = 0, wireS1 = 0, wireRhoA = 0, wireK2 = 0, Kc = 0, lamc = 0, wireR = 0, wprev = 0;
    double epsF = 1, epsTarget = 1;   // strainer: wire preload x epsF (approached gradually)

    struct Mic
    {
        double x = 0, y = 0, z = 0, R = 0;
        std::size_t D = 0;
        double Rw = 0;
        std::size_t Dw = 0;
        bool facing = false;
        double g = 0;
        std::size_t d = 0;
        double fr = 0;
        std::size_t b0 = 0, b1 = 0;
        Vec<std::uint32_t> bs, bi, gi;
        Vec<G> w1, w2;
        Vec<double> ys;   // per delay bucket: sums of w1 u, w2 u at steps n+1, n, n-1 (6 per bucket)
    };
    Mic mics[3];
    Vec<double> ring[3];
    std::size_t pos = 0;
    double mixT = 0, mixB = 0, mixO = 0, gClose = 1, gOver = 1, gMaster = 1;

    struct Stick
    {
        double z = 0, z1 = 0, z2 = 0, zr = 0, zr1 = 0, zr2 = 0, M = 0, Mr = 0, rimref = 0, F = 0, Fr = 0;
        double sx = 0, sy = 0, aprev = 0;
        double sm[3] {}, sm1[3] {};
        bool active = false, gone = false, rim = false;
        Vec<double> w;
        Vec<std::uint32_t> idx;
        std::size_t ns = 0, idle = 0;
    };
    Stick sticks[2];
    std::size_t nextStick = 1;
    struct Hit { double x, y, v; bool rim; std::size_t stick; };
    Hit pending[16] {};
    std::size_t npend = 0;
    std::uint64_t stepN = 0;   // steps since load, settling included
    bool loaded = false, asleep = false, measuring = false;
    std::size_t quiet = 0;
    double Rpeak = 0, kinRel = 0;
    double kinFloor = 0, eFloor = 0;   // the resting drum's noise floor (single precision), measured after the settling
    Vec<double> scratch;

    //==============================================================================
    void coefs()
    {
        for (std::size_t i = 0; i < nBp; ++i)
        {
            if (i >= nB)
            {
                cA[i] = 2; cB[i] = 1; sA[i] = 2; sB[i] = 1;
                continue;
            }
            const double sk = bSig[i] * lossF * k;
            cA[i] = 2 / (1 + sk);
            cB[i] = (1 - sk) / (1 + sk);
            cC[i] = k2 * bW2[i] * tuneF * tuneF / (1 + sk);
            cD[i] = k2 * bImu[i] / (1 + sk);
            const double ss = sk + 250 * k;
            sA[i] = 2 / (1 + ss);
            sB[i] = (1 - ss) / (1 + ss);
            sC[i] = k2 * bW2[i] / (1 + ss);
            sD[i] = k2 * bImu[i] / (1 + ss);
            cS[i] = k2 * bImu[i] / (1 + bSig[i] * k);
        }
    }

    bool load (const void* data, std::size_t len)
    {
        const auto* bytes = static_cast<const unsigned char*> (data);
        if (len < 4 || std::memcmp (bytes, "PKS1", 4) != 0)
            return false;

        Reader r { bytes + 4, (len - 4) / 8 };
        fs = r.f();
        if (! (fs > 1000)) return false;
        k = 1 / fs; k2 = k * k; k2i = 1 / k2;
        nset = r.u(); maxd = r.u();
        if (maxd + 4 >= (std::size_t) RING) return false;
        ra = r.f();
        (void) r.f();   // Hc
        Ks = r.f(); lams = r.f(); ms = r.f(); alift = r.f(); rstick = r.f(); hb = r.f();
        rimMass = r.f(); rimOff = r.f(); rimK = r.f(); stickRad = r.f(); stickLen = r.f();
        stickModes = r.f() != 0; stickQ = r.f(); stickMm = r.f();
        for (auto& v : stickF) v = r.f();
        hoopArim = r.f();

        // batter, read in the file's order, then sorted by frequency
        const auto n = r.u();
        nB = n;
        nch = (n + CH - 1) / CH;
        nBp = nch * CH;
        bT0 = r.f(); bTeff = bT0; bNlc = r.f(); bNonlin = r.f() != 0;
        const auto m0 = r.arr<std::uint32_t> (n);
        const auto o0 = r.arr<std::uint32_t> (n);
        const auto kap0 = r.arr<double> (n);
        const auto w20 = r.arr<double> (n);
        const auto sig0 = r.arr<double> (n);
        const auto imu0 = r.arr<double> (n);
        const auto kN0 = r.arr<double> (n);
        const auto vol0 = r.arr<double> (n);
        nrc = r.u();
        rci.assign (nrc, 0); rcm.assign (nrc, 0); rcg.assign (nrc, 0.0);
        Vec<std::uint32_t> rci0 (nrc);
        for (std::size_t j = 0; j < nrc; ++j)
        {
            rci0[j] = (std::uint32_t) r.u();
            rcm[j] = (std::uint32_t) r.u();
            rcg[j] = r.f();
            if (rci0[j] >= n || rcm[j] >= 8) return false;
        }
        for (auto& v : Kn) v = r.f();
        Vec<double> ta0[3], tv0[3];
        for (int mi = 0; mi < 3; ++mi)
        {
            Dc[mi] = r.u();
            if (Dc[mi] >= (std::size_t) RING) return false;
            ta0[mi] = r.arr<double> (n);
            tv0[mi] = r.arr<double> (n);
        }
        if (r.bad) return false;

        Vec<std::uint32_t> perm (n);
        std::iota (perm.begin(), perm.end(), 0u);
        std::stable_sort (perm.begin(), perm.end(), [&] (std::uint32_t x, std::uint32_t y) { return w20[x] < w20[y]; });
        Vec<std::uint32_t> inv (n);
        for (std::size_t i = 0; i < n; ++i) inv[perm[i]] = (std::uint32_t) i;

        bM.assign (nBp, 0); bO.assign (nBp, 0);
        bKap.assign (nBp, 0.0); bW2.assign (nBp, 0.0); bSig.assign (nBp, 0.0);
        bImu.assign (nBp, 0.0); bKN.assign (nBp, 0.0); bVol.assign (nBp, 0.0);
        for (int mi = 0; mi < 3; ++mi) { tapA[mi].assign (nBp, 0.0); tapV[mi].assign (nBp, 0.0); }
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto s = perm[i];
            bM[i] = m0[s]; bO[i] = o0[s]; bKap[i] = kap0[s]; bW2[i] = w20[s]; bSig[i] = sig0[s];
            bImu[i] = imu0[s]; bKN[i] = kN0[s]; bVol[i] = vol0[s];
            for (int mi = 0; mi < 3; ++mi) { tapA[mi][i] = ta0[mi][s]; tapV[mi][i] = tv0[mi][s]; }
        }
        for (std::size_t j = 0; j < nrc; ++j) rci[j] = inv[rci0[j]];
        spI.assign (n + 1, 0);
        nsp = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (tapA[1][i] != 0 || tapV[1][i] != 0)
                spI[nsp++] = (std::uint32_t) i;
        for (auto* c : { &cA, &cB, &cC, &cD, &sA, &sB, &sC, &sD, &cS }) c->assign (nBp, 0.0);
        coefs();
        for (auto* c : { &bq, &bq1, &bq2, &bf, &bx }) c->assign (nBp, 0.0);
        volI.assign (n + 1, 0);
        nvol = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (bVol[i] != 0)
                volI[nvol++] = (std::uint32_t) i;
        chOn.assign (nch, 0); chCpl.assign (nch, 0); act.assign (nch, 0);
        for (std::size_t j = 0; j < nvol; ++j) chCpl[volI[j] / CH] = 1;
        for (std::size_t j = 0; j < nrc; ++j) chCpl[rci[j] / CH] = 1;

        // footprint table
        double km = 0;
        mmax = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            km = std::max (km, bKap[i]);
            mmax = std::max<std::size_t> (mmax, bM[i]);
        }
        NR = std::max<std::size_t> (64, (std::size_t) std::ceil (km * ra / (2 * PI) * 24));
        Jt.assign (n * (NR + 3), 0.0);
        const double hr = ra / (double) (NR - 1);
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < NR + 3; ++j)
                Jt[i * (NR + 3) + j] = bessel::jn ((int) bM[i], bKap[i] * ((double) j - 1) * hr);
        cmv.assign (mmax + 1, 0.0);
        smv.assign (mmax + 1, 0.0);
        for (auto& s : sticks) { s.w.assign (nBp, 0.0); s.idx.assign (nBp, 0); }

        // resonant head
        RN = r.u(); Rh = r.f(); RrhoH = r.f(); RT0 = r.f(); RTeff = RT0; Rs0 = r.f(); Rs1 = r.f(); Rnlc = r.f();
        Rnp = r.u();
        Rpts = r.arr<std::uint32_t> (Rnp);
        nrow = r.u();
        rj.assign (nrow, 0); ri0.assign (nrow, 0); ri1.assign (nrow, 0);
        for (std::size_t i = 0; i < nrow; ++i) { rj[i] = (std::uint32_t) r.u(); ri0[i] = (std::uint32_t) r.u(); ri1[i] = (std::uint32_t) r.u(); }
        ne = r.u();
        edge.assign (ne, 0); edgec.assign (ne, 0.0);
        Vec<double> eth (ne);
        for (std::size_t e = 0; e < ne; ++e) { edge[e] = (std::uint32_t) r.u(); edgec[e] = r.f(); eth[e] = r.f(); }
        nlb = r.u();
        lb = r.arr<std::uint32_t> (nlb);
        nc = r.u(); nrun = r.u();
        runc.assign (nrun, 0); runp.assign (nrun, 0); runl.assign (nrun, 0);
        for (std::size_t i = 0; i < nrun; ++i) { runc[i] = (std::uint32_t) r.u(); runp[i] = (std::uint32_t) r.u(); runl[i] = (std::uint32_t) r.u(); }
        if (r.bad) return false;

        const auto NN = RN * RN;
        for (auto p : Rpts) if (p >= NN) return false;
        for (auto e : edge) if (e >= NN) return false;
        for (auto e : lb) if (e >= NN) return false;
        for (std::size_t q = 0; q < nrun; ++q) if (runp[q] + runl[q] > NN || runc[q] >= nc) return false;
        for (std::size_t q = 0; q < nrow; ++q) if (rj[q] < 1 || rj[q] + 1 >= RN || ri0[q] < 1 || ri1[q] + 1 >= RN) return false;

        cellOf.assign (NN, 0);
        for (std::size_t q = 0; q < nrun; ++q)
            for (std::size_t i = 0; i < runl[q]; ++i)
                cellOf[runp[q] + i] = runc[q];
        {
            NC4 = (RN + 3) / 4 + 1;
            Vec<std::uint32_t> tb (NC4 * NC4, 0xffffffffu);
            bool ok = true;
            for (std::size_t q = 0; q < Rnp; ++q)
            {
                const auto pp = Rpts[q];
                const auto bi = (pp / RN / 4) * NC4 + (pp % RN) / 4;
                if (tb[bi] == 0xffffffffu) tb[bi] = cellOf[pp];
                else if (tb[bi] != cellOf[pp]) ok = false;
            }
            for (auto& t : tb) if (t == 0xffffffffu) t = 0;
            if (ok) blk = std::move (tb);
        }
        u.assign (NN + 8, 0); u_1.assign (NN + 8, 0); u_2.assign (NN + 8, 0); fR.assign (NN + 8, 0);
        uc.assign (nc, 0); fc.assign (nc, 0);
        xi.assign (ne, 0.0); Fe.assign (ne, 0.0);

        // cavity
        cavOn = r.f() != 0;
        cavDiv = std::max<std::size_t> (1, r.u());
        nq = r.u();
        cOm = r.arr<double> (nq); cZeta = r.arr<double> (nq); cLam = r.arr<double> (nq);
        cZ.assign (nq, 0.0); cZ1.assign (nq, 0.0); cZ2.assign (nq, 0.0); cSp.assign (nq, 0.0);
        cOff.assign (nq + 1, 0);
        {
            const auto save = r.i;
            std::size_t tot = 0;
            for (std::size_t q = 0; q < nq; ++q) { const auto nb = r.u(); r.i += 2 * nb; tot += nb; }
            r.i = save;
            cI.assign (tot + 1, 0); cCb.assign (tot + 1, 0.0);
            std::size_t c = 0;
            for (std::size_t q = 0; q < nq; ++q)
            {
                cOff[q] = (std::uint32_t) c;
                const auto nb = r.u();
                for (std::size_t t = 0; t < nb; ++t)
                {
                    const auto o = r.u();
                    if (o >= n) return false;
                    cI[c] = inv[o];
                    chCpl[cI[c] / CH] = 1;
                    cCb[c] = r.f();
                    ++c;
                }
            }
            cOff[nq] = (std::uint32_t) c;
        }
        Pr = r.arr<float> (nq * nc);

        // shell + hoop
        if (r.u() != NBODY) return false;
        for (auto& v : bodyF) v = r.f();
        for (auto& v : bodyN) v = (std::uint32_t) r.u();
        for (auto& v : bodyQ) v = r.f();
        for (auto& v : bodyM) v = r.f();
        for (auto& v : bodyA) v = r.f();
        shellBack = r.f() != 0;
        if (r.u() != 2) return false;   // radLoad 2 only
        ecosN.assign (8 * ne, 0.0);
        for (auto bn : bodyN)
        {
            if (bn < 2 || bn > 7) return false;
            hasN[bn] = true;
        }
        for (std::size_t nn = 2; nn < 8; ++nn)
            for (std::size_t e = 0; e < ne; ++e)
                ecosN[nn * ne + e] = std::cos ((double) nn * eth[e]);

        // wires
        nw = r.u();
        wireS0 = r.f(); wireS1 = r.f(); wireRhoA = r.f();
        const double kap = r.f();
        wireK2 = kap * kap;
        Kc = r.f(); lamc = r.f(); wireR = r.f();
        if (r.f() == 0) return false;   // wireRad 1 only
        wst = std::max<std::size_t> (1, r.u());
        if (nw > 64) return false;
        wires.assign (nw, {});
        for (auto& w : wires)
        {
            w.Nw = r.u();
            w.h = r.f();
            const double T = r.f();
            w.c2 = T / wireRhoA;
            w.epsT = r.f();
            w.eps = 0;
            w.rough = r.arr<G> (w.Nw + 3);
            w.v.assign (w.Nw + 3, 0); w.v1.assign (w.Nw + 3, 0); w.v2.assign (w.Nw + 3, 0);
            w.nc = r.u(); w.i0 = r.u(); w.row = r.u(); w.x0 = r.u(); w.nx = r.u();
            w.fy = (G) r.f();
            if (w.i0 + wst * (w.nc ? w.nc - 1 : 0) >= w.Nw + 3 || (w.row + 1) * RN + w.x0 + w.nx + 1 >= NN + 8)
                return false;
            w.cp.assign (w.nc, 0); w.cx.assign (w.nc, 0); w.cfx.assign (w.nc, 0); w.cwi.assign (4 * w.nc, 0); w.cw2.assign (w.nc, 0);
            for (std::size_t c = 0; c < w.nc; ++c)
            {
                w.cp[c] = (std::uint32_t) r.u();
                w.cx[c] = (std::uint32_t) r.u();
                w.cfx[c] = (G) r.f();
                for (int t = 0; t < 4; ++t) w.cwi[4 * c + (std::size_t) t] = (G) r.f();
                w.cw2[c] = (G) r.f();
                if (w.cp[c] + RN + 1 >= NN || w.cx[c] + 1 >= w.nx + 2) return false;
            }
            w.d1.assign (w.nc, 0); w.g1.assign (w.nc, 0); w.g0.assign (w.nc, 0); w.gm.assign (w.nc, 0);
            w.ub.assign (w.nx + 2, 0); w.ub2.assign (w.nx + 2, 0);
            if (hotList.size() < w.nc) hotList.assign (w.nc + 64, 0);
        }

        // mics
        for (auto& M : mics)
        {
            M = {};
            M.x = r.f(); M.y = r.f(); M.z = r.f(); M.R = r.f(); M.D = r.u(); M.Rw = r.f(); M.Dw = r.u(); M.facing = r.f() != 0;
            if (M.D >= (std::size_t) RING || M.Dw >= (std::size_t) RING) return false;
            if (! M.facing)
            {
                M.g = r.f(); M.d = r.u(); M.fr = r.f();
            }
            else
            {
                M.b0 = r.u(); M.b1 = r.u();
                M.bs = r.arr<std::uint32_t> (maxd + 2);
                M.bi = r.arr<std::uint32_t> (Rnp);
                M.w1 = r.arr<G> (Rnp);
                M.w2 = r.arr<G> (Rnp);
                if (M.b1 > maxd + 1) return false;
                M.gi.assign (Rnp, 0);
                for (std::size_t e = 0; e < Rnp; ++e)
                {
                    if (M.bi[e] >= Rnp) return false;
                    M.gi[e] = Rpts[M.bi[e]];
                }
                M.ys.assign (6 * (maxd + 2), 0.0);
            }
        }
        mixT = r.f(); mixB = r.f(); mixO = r.f();
        if (r.bad || r.i * 8 + 4 != len) return false;

        for (auto& rg : ring) rg.assign (RING, 0.0);
        scratch.assign (nch + 1, 0.0);
        loaded = true;
        baseKs = Ks;
        for (std::size_t c = 0; c < nch; ++c) if (chCpl[c]) wake (c);
        // settling: the wires come down onto the head (snarelite.c's first `settle` seconds, extra loss everywhere)
        for (std::size_t i = 0; i < nset; ++i) step (true);
        measuring = true;
        float discard[128];
        for (int i = 0; i < 8; ++i) render (discard, 128, false);
        measuring = false;
        // at rest: what is left is single precision's floor (-100dB or so), not worth computing
        asleep = true;
        return true;
    }

    //==============================================================================
    // the time step (snarelite.c main loop)

    // cavity cells: sums of the head displacement over runs of nodes (consecutive nodes of one grid row in one cell)
    void cellSums()
    {
        std::fill (uc.begin(), uc.end(), 0.0f);
        if (! blk.empty())
        {
            // 4 x 4 cells aligned on the grid: one 4-node sum per cell and row
            for (std::size_t rr = 0; rr < nrow; ++rr)
            {
                const auto j = rj[rr];
                const auto base = (j / 4) * NC4;
                for (auto ic = ri0[rr] / 4; ic < ri1[rr] / 4 + 1; ++ic)
                {
                    const G* p = u.data() + j * RN + 4 * ic;
                    uc[blk[base + ic]] += (p[0] + p[2]) + (p[1] + p[3]);
                }
            }
            return;
        }
        for (std::size_t q = 0; q < nrun; ++q)
        {
            const auto p = runp[q];
            G v = 0;
            for (std::size_t i = 0; i < runl[q]; ++i) v += u[p + i];
            uc[runc[q]] += v;
        }
    }

    void cellFill()
    {
        if (! blk.empty())
        {
            for (std::size_t rr = 0; rr < nrow; ++rr)
            {
                const auto j = rj[rr];
                const auto base = (j / 4) * NC4;
                for (auto ic = ri0[rr] / 4; ic < ri1[rr] / 4 + 1; ++ic)
                {
                    const G v = fc[blk[base + ic]];
                    G* p = fR.data() + j * RN + 4 * ic;
                    p[0] = p[1] = p[2] = p[3] = v;
                }
            }
            return;
        }
        for (std::size_t q = 0; q < nrun; ++q)
        {
            const auto p = runp[q];
            const G v = fc[runc[q]];
            for (std::size_t i = 0; i < runl[q]; ++i) fR[p + i] = v;
        }
    }

    void cavStep (double kk, double sx)
    {
        cellSums();
        const double dAr = Rh * Rh;
        const double K = RHO * CAIR * CAIR;
        std::fill (bf.begin(), bf.end(), 0.0);
        std::fill (fc.begin(), fc.end(), 0.0f);
        for (std::size_t q = 0; q < nq; ++q)
        {
            const float* pr = Pr.data() + q * nc;
            double sb = 0;
            for (auto j = cOff[q]; j < cOff[q + 1]; ++j) sb += cCb[j] * bq[cI[j]];
            const float sr = dot32 (pr, uc.data(), nc);
            const double S = sb - (double) sr * dAr;
            const double w = cOm[q];
            const double zk = (cZeta[q] * w + sx) * kk;
            cZ2[q] = (2 * cZ[q] - (1 - zk) * cZ1[q] - kk * kk * w * w * (cZ[q] - S) + 2 * zk * (S - cSp[q])) / (1 + zk);
            cSp[q] = S;
            const double P = -K / cLam[q] * (S - cZ[q]);
            const auto Pf = (float) P;
            for (auto j = cOff[q]; j < cOff[q + 1]; ++j) bf[cI[j]] += P * cCb[j];
            axpy32 (fc.data(), pr, Pf, nc);
        }
        cellFill();
        std::swap (cZ1, cZ);
        std::swap (cZ, cZ2);
    }

    void memStep (double sx)
    {
        const auto N = RN;
        if (bNonlin && stepN % 4 == 0)
        {
            // tension modulation of the resonant head: every 4th step (it changes slowly)
            double g = 0;
            for (std::size_t rr = 0; rr < nrow; ++rr)
            {
                const auto o = rj[rr] * N;
                g += gradSq (u.data(), o + ri0[rr], o + ri1[rr] + 1, N);
            }
            for (std::size_t e = 0; e < nlb; ++e)
            {
                const double ue = u[lb[e]];
                g += ue * ue;
            }
            RTeff = RT0 + Rnlc * g;
        }
        const auto c2k2 = (G) (RTeff / RrhoH * k2 / (Rh * Rh));
        const auto s1k = (G) (2 * Rs1 * k / (Rh * Rh));
        const auto fk = (G) (k2 / RrhoH);
        const double skd = (Rs0 + sx) * k;
        const auto sk = (G) skd;
        const auto isk = (G) (1 / (1 + skd));
        const auto pr = (G) -Rprad;
        double sum = 0;
        for (std::size_t rr = 0; rr < nrow; ++rr)
        {
            const auto o = rj[rr] * N;
            sum += rowStep (u_2.data(), u.data(), u_1.data(), fR.data(), o + ri0[rr], o + ri1[rr] + 1, N, sk, c2k2, s1k, fk, isk, pr);
        }
        const double ck = (double) c2k2 * (double) isk;
        for (std::size_t e = 0; e < ne; ++e)
        {
            const double v = ck * edgec[e] * xi[e];
            u_2[edge[e]] += (G) v;
            sum += v;
        }
        us2 = sum;
    }

    void wireStep (Wire& w, double sx)
    {
        const auto N = w.Nw;
        auto& v = w.v;
        auto& v1 = w.v1;
        v[0] = -v[2];
        v[N + 2] = -v[N];
        v1[0] = -v1[2];
        v1[N + 2] = -v1[N];
        const double h2 = w.h * w.h;
        const double skd = (wireS0 + sx) * k;
        stringStep (w.v2.data(), v.data(), v1.data(), N, (G) skd, (G) (w.c2 * k2 / h2), (G) (wireK2 * k2 / (h2 * h2)),
                    (G) (2 * wireS1 * k / h2), (G) (1 / (1 + skd)));
        w.v2[1] = 0;
        w.v2[N + 1] = 0;
    }

    void wireContacts (double sx)
    {
        const auto N = RN;
        const double ihm = k2 / (RrhoH * Rh * Rh * (1 + (Rs0 + sx) * k));
        for (auto& w : wires)
        {
            const double mw = wireRhoA * w.h * (1 + (wireS0 + sx) * k);
            const double imw = k2 / mw;
            const G fy = w.fy;
            const G gy = 1 - fy;
            const auto o = w.row * N + w.x0;
            G* ub = w.ub.data();
            G* ub2 = w.ub2.data();
            {
                const G* r0 = u.data() + o;
                const G* r1 = u.data() + o + N;
                const G* q0 = u_2.data() + o;
                const G* q1 = u_2.data() + o + N;
                for (std::size_t x = 0; x < w.nx; ++x)
                {
                    ub[x] = gy * r0[x] + fy * r1[x];
                    ub2[x] = gy * q0[x] + fy * q1[x];
                }
            }
            const auto wi0 = w.i0;
            const auto we = (G) w.eps;

            // pass 1: gaps at n-1, n and the predicted one; the nodes that may touch
            std::size_t nh = 0;
            {
                const G* v = w.v.data() + wi0;
                const G* v2 = w.v2.data() + wi0;
                const G* rough = w.rough.data() + wi0;
                for (std::size_t c = 0; c < w.nc; ++c)
                {
                    const auto x = w.cx[c];
                    const G fx = w.cfx[c];
                    const G off = we + rough[wst * c];
                    const G d0 = v[wst * c] - (ub[x] + fx * (ub[x + 1] - ub[x]));
                    const G e2 = v2[wst * c] + off - (ub2[x] + fx * (ub2[x + 1] - ub2[x]));
                    const G e1 = w.d1[c] + off;
                    const G e0 = d0 + off;
                    w.d1[c] = d0;
                    w.g1[c] = e1;
                    w.g0[c] = e0;
                    hotList[nh] = (std::uint32_t) c;
                    nh += (e2 > 0 || e1 > 0 || e0 > 0) ? 1 : 0;
                }
            }

            // pass 2: Gauss-Seidel over the nodes that may touch
            for (std::size_t hh = 0; hh < nh; ++hh)
            {
                const auto c = hotList[hh];
                const auto i = wi0 + wst * c;
                const auto x = w.cx[c];
                const double fx = w.cfx[c];
                const double hd = (double) ub2[x] + fx * ((double) ub2[x + 1] - (double) ub2[x]);
                const double ef = (double) w.v2[i] + w.eps + (double) w.rough[i] - hd;
                const double e1 = w.g1[c];
                const double e0 = w.g0[c];
                if (ef <= 0 && e1 <= 0 && e0 <= 0) continue;
                const double F = contactHertzG (k, Kc, lamc, imw + ihm * (double) w.cw2[c], ef, e1, e0, w.gm[c]);
                w.gm[c] = (G) F;
                if (F > 0)
                {
                    const G* ci = w.cwi.data() + 4 * c;
                    const auto s = (G) (ihm * F);
                    const auto p = w.cp[c];
                    w.v2[i] -= (G) (imw * F);
                    u_2[p] += s * ci[0];
                    u_2[p + 1] += s * ci[1];
                    u_2[p + N] += s * ci[2];
                    u_2[p + N + 1] += s * ci[3];
                    ub2[x] += s * (gy * ci[0] + fy * ci[2]);
                    ub2[x + 1] += s * (gy * ci[1] + fy * ci[3]);
                    us2 += (double) s * (double) (ci[0] + ci[1] + ci[2] + ci[3]);
                }
            }
        }
    }

    double eta (const Stick& st, const Vec<double>& q, double z) const
    {
        double e = 0;
        for (std::size_t t = 0; t < st.ns; ++t)
        {
            const auto i = st.idx[t];
            e += st.w[i] * q[i];
        }
        return e - z;
    }

    void wake (std::size_t c)
    {
        if (chOn[c]) return;
        chOn[c] = 1;
        act[nact++] = (std::uint32_t) c;
    }

    void startStick (const Hit& hh)
    {
        auto& st = sticks[hh.stick];
        auto& w = st.w;
        std::fill (w.begin(), w.end(), 0.0);
        double sum = 0;
        const int ic = (int) std::floor (hh.x / hb);
        const int jc = (int) std::floor (hh.y / hb);
        const int nn = (int) std::ceil (2.2 * rstick / hb) + 1;
        for (int j = jc - nn; j <= jc + nn; ++j)
            for (int i = ic - nn; i <= ic + nn; ++i)
            {
                const double x = i * hb, y = j * hb;
                const double d = std::hypot (x - hh.x, y - hh.y);
                const double rr = std::hypot (x, y);
                if (d >= 2.2 * rstick || rr >= ra) continue;
                const double wt = std::exp (-(d / rstick) * (d / rstick));
                const double th = std::atan2 (y, x);
                sum += wt;
                const double c1 = std::cos (th), s1 = std::sin (th);
                cmv[0] = 1;
                smv[0] = 0;
                for (std::size_t m = 1; m <= mmax; ++m)
                {
                    cmv[m] = cmv[m - 1] * c1 - smv[m - 1] * s1;
                    smv[m] = smv[m - 1] * c1 + cmv[m - 1] * s1;
                }
                const double uu = rr / ra * (double) (NR - 1) + 1;
                const auto iu = (std::size_t) std::floor (uu);
                const double fu = uu - std::floor (uu), f2 = fu * fu, f3 = f2 * fu;
                const double k0 = -0.5 * f3 + f2 - 0.5 * fu;
                const double k1 = 1.5 * f3 - 2.5 * f2 + 1;
                const double kc2 = -1.5 * f3 + 2 * f2 + 0.5 * fu;
                const double k3 = 0.5 * f3 - 0.5 * f2;
                for (std::size_t s = 0; s < nB; ++s)
                {
                    const auto base = s * (NR + 3) + iu - 1;
                    const double J = k0 * Jt[base] + k1 * Jt[base + 1] + kc2 * Jt[base + 2] + k3 * Jt[base + 3];
                    w[s] += wt * J * (bO[s] != 0 ? smv[bM[s]] : cmv[bM[s]]);
                }
            }
        if (sum <= 0) return;

        double wmax = 0;
        for (std::size_t s = 0; s < nB; ++s)
        {
            w[s] /= sum;
            wmax = std::max (wmax, w[s] * w[s] * bImu[s]);
        }
        std::size_t nsel = 0;
        double M = 0;
        for (std::size_t s = 0; s < nB; ++s)
        {
            if (w[s] * w[s] * bImu[s] > 1e-12 * wmax)
            {
                st.idx[nsel++] = (std::uint32_t) s;
                M += w[s] * w[s] * bImu[s] / (1 + bSig[s] * k);
                wake (s / CH);
            }
            else
            {
                w[s] = 0;
            }
        }
        st.ns = nsel;
        for (std::size_t c = 0; c < nch; ++c) if (chCpl[c]) wake (c);
        st.M = k2 * (M + 1 / ms);
        const double head = eta (st, bq, 0);
        st.z = head + 2.5 * hh.v * k;
        st.z1 = st.z + hh.v * k;
        st.active = true;
        st.gone = false;
        st.rim = hh.rim;
        st.sx = hh.x;
        st.sy = hh.y;
        st.aprev = 0;
        for (int m = 0; m < 3; ++m) { st.sm[m] = 0; st.sm1[m] = 0; }
        st.zr = st.z;
        st.zr1 = st.z1;
        st.F = 0;
        st.Fr = 0;
        st.idle = 0;
        if (hh.rim)
        {
            double Mr = 0;
            for (std::size_t m = 0; m < NBODY; ++m) Mr += 1.0 / (bodyM[m] * (1 + PI * bodyF[m] / bodyQ[m] * k));
            st.Mr = k2 * (Mr + 1 / (rimMass > 0 ? rimMass : ms));
            st.rimref = head - rimOff;
        }
        hoopA = hh.rim ? hoopArim : 1;
        asleep = false;
        quiet = 0;
        Emax = 0;
        Rpeak = 0;
    }

    void step (bool settling)
    {
        const double sx = settling ? 250 : 0;
        if (settling)
        {
            double ramp = (double) stepN / (0.6 * (double) nset);
            if (ramp > 1) ramp = 1;
            for (auto& w : wires) w.eps = w.epsT * ramp;
        }
        else if (epsF != epsTarget)
        {
            // strainer: move the preload by at most 2 % of its nominal value per ms
            const double dmax = 0.02 * 1000 * k;
            epsF = std::abs (epsTarget - epsF) <= dmax ? epsTarget : epsF + (epsTarget > epsF ? dmax : -dmax);
            for (auto& w : wires) w.eps = w.epsT * epsF;
        }

        // 1. cavity
        if (cavOn && stepN % cavDiv == 0) cavStep ((double) cavDiv * k, sx);

        // 2. shell drive from the head edges
        double Xi[8] {}, Hs[8] {}, bfrc[NBODY] {};
        if (shellBack)
            for (std::size_t b = 0; b < NBODY; ++b) Xi[bodyN[b]] += qb[b];
        // (the shell modes have only 6 distinct orders n: one cos(n theta) row per order)
        std::fill (xi.begin(), xi.end(), 0.0);
        if (shellBack)
            for (std::size_t nn = 2; nn < 8; ++nn)
                if (Xi[nn] != 0)
                {
                    const double* row = ecosN.data() + nn * ne;
                    for (std::size_t e = 0; e < ne; ++e) xi[e] += Xi[nn] * row[e];
                }
        for (std::size_t e = 0; e < ne; ++e) Fe[e] = RTeff * ((double) u[edge[e]] - xi[e]) * edgec[e];
        double Dn[8] {};
        for (std::size_t nn = 2; nn < 8; ++nn)
            if (hasN[nn])
            {
                const double* row = ecosN.data() + nn * ne;
                double s = 0;
                for (std::size_t e = 0; e < ne; ++e) s += Fe[e] * row[e];
                Dn[nn] = s;
            }
        for (std::size_t m = 0; m < NBODY; ++m) bfrc[m] = Dn[bodyN[m]];
        for (std::size_t j = 0; j < nrc; ++j) Hs[rcm[j]] += rcg[j] * bq[rci[j]];
        for (std::size_t b = 0; b < NBODY; ++b) bfrc[b] += bTeff * (Hs[bodyN[b]] - Kn[bodyN[b]] * Xi[bodyN[b]]);
        for (std::size_t m = 0; m < NBODY; ++m)
        {
            const double w = 2 * PI * bodyF[m];
            const double sk = (w / (2 * bodyQ[m]) + sx) * k;
            qb2[m] = (2 * qb[m] - (1 - sk) * qb1[m] - k2 * w * w * qb[m] + k2 * bfrc[m] / bodyM[m]) / (1 + sk);
        }

        // 3. outer radiation load (radLoad 2: one piston for both heads)
        double pradB = 0;
        {
            double Qb = 0;
            for (std::size_t j = 0; j < nvol; ++j)
            {
                const auto i = volI[j];
                Qb += bVol[i] * (bq[i] - bq1[i]);
            }
            Qb *= fs;
            const double Qr = -(us - us1) * Rh * Rh * fs;
            const double A2 = 2 * PI * ra * ra;
            const double Ra = RHO * CAIR / A2;
            const double ma = RHO / std::sqrt (8 * PI * PI * ra * ra);
            pradB = Ra * (Qb + Qr - QMb);
            QMb += k * pradB / ma;
            Rprad = pradB;
        }

        // 4. free updates: batter modes, resonant grid, wires, sticks
        {
            if (bNonlin)
            {
                D2 gv = simd::splat (0.0);
                for (std::size_t t = 0; t < nact; ++t)
                {
                    const auto b = act[t] * CH;
                    for (std::size_t i = b; i < b + CH; i += 2)
                    {
                        const D2 qv = simd::load (bq.data() + i);
                        gv += simd::load (bKN.data() + i) * qv * qv;
                    }
                }
                const double g = simd::sum (gv);
                bTeff = bT0 + bNlc * g;
            }
            const double tr = bTeff / bT0;
            for (std::size_t j = 0; j < nrc; ++j) bx[rci[j]] += bTeff * rcg[j] * Xi[rcm[j]];
            const double* A = settling ? sA.data() : cA.data();
            const double* B = settling ? sB.data() : cB.data();
            const double* C = settling ? sC.data() : cC.data();
            const double* D = settling ? sD.data() : cD.data();
            const D2 trv = simd::splat (tr), pv = simd::splat (pradB);
            for (std::size_t t = 0; t < nact; ++t)
            {
                const auto b = act[t] * CH;
                for (std::size_t i = b; i < b + CH; i += 2)
                {
                    const D2 qv = simd::load (bq.data() + i);
                    const D2 f = simd::load (bf.data() + i) - pv * simd::load (bVol.data() + i) + simd::load (bx.data() + i);
                    simd::store (bq2.data() + i, simd::load (A + i) * qv - simd::load (B + i) * simd::load (bq1.data() + i)
                                                   - simd::load (C + i) * trv * qv + simd::load (D + i) * f);
                }
            }
            for (std::size_t j = 0; j < nrc; ++j) bx[rci[j]] = 0;
        }
        memStep (sx);
        for (auto& w : wires) wireStep (w, sx);
        for (auto& st : sticks)
            if (st.active)
            {
                st.z2 = 2 * st.z - st.z1 + k2 * alift;
                st.zr2 = 2 * st.zr - st.zr1 + k2 * alift;
                st.F = 0;
                st.Fr = 0;
            }

        // 5. contacts: sticks (2 Gauss-Seidel sweeps), then the wires against the resonant head
        for (auto& st : sticks)
        {
            if (! st.active) continue;
            double e1 = 0, e0 = 0;
            if (! st.gone)
            {
                e1 = eta (st, bq1, st.z1);
                e0 = eta (st, bq, st.z);
            }
            for (int sweep = 0; sweep < 2; ++sweep)
            {
                if (! st.gone)
                {
                    const double ef = eta (st, bq2, st.z2);
                    const double F = contactHertzG (k, Ks, lams, st.M, ef, e1, e0, 0);
                    if (F > 0)
                    {
                        for (std::size_t t = 0; t < st.ns; ++t)
                        {
                            const auto i = st.idx[t];
                            bq2[i] -= F * st.w[i] * cS[i];
                        }
                        st.z2 += k2 * F / ms;
                        st.F += F;
                    }
                    if (sweep == 1 && F <= 0 && st.z2 > st.z && -ef > 0.02) st.gone = true;
                }
                if (st.rim)
                {
                    double qr2 = 0, qr1 = 0, qr0 = 0;
                    for (std::size_t m = 0; m < NBODY; ++m)
                    {
                        const double ph = bodyN[m] % 2 == 0 ? 1 : -1;
                        qr2 += ph * qb2[m];
                        qr1 += ph * qb1[m];
                        qr0 += ph * qb[m];
                    }
                    const bool sep = rimMass > 0;
                    double& pz2 = sep ? st.zr2 : st.z2;
                    const double pz1 = sep ? st.zr1 : st.z1;
                    const double pz0 = sep ? st.zr : st.z;
                    const double ef2 = st.rimref + qr2 - pz2;
                    const double e12 = st.rimref + qr1 - pz1;
                    const double e02 = st.rimref + qr0 - pz0;
                    const double Fr = contactHertzG (k, Ks * rimK, lams, st.Mr, ef2, e12, e02, 0);
                    if (Fr > 0)
                    {
                        for (std::size_t m = 0; m < NBODY; ++m)
                        {
                            const double ph = bodyN[m] % 2 == 0 ? 1 : -1;
                            const double w = 2 * PI * bodyF[m];
                            qb2[m] -= k2 * Fr * ph / (bodyM[m] * (1 + w / (2 * bodyQ[m]) * k));
                        }
                        pz2 += k2 * Fr / (sep ? rimMass : ms);
                        st.Fr += Fr;
                    }
                }
            }
        }
        wireContacts (sx);

        // 6. outputs
        raySums();
        if (! settling) outputs();

        // 7. swap
        std::swap (bq1, bq);
        std::swap (bq, bq2);
        std::swap (u_1, u);
        std::swap (u, u_2);
        us1 = us;
        us = us2;
        for (auto& w : wires)
        {
            std::swap (w.v1, w.v);
            std::swap (w.v, w.v2);
        }
        std::copy (std::begin (qb), std::end (qb), std::begin (qb1));
        std::copy (std::begin (qb2), std::end (qb2), std::begin (qb));
        for (auto& st : sticks)
            if (st.active)
            {
                st.z1 = st.z;
                st.z = st.z2;
                st.zr1 = st.zr;
                st.zr = st.zr2;
                if (st.gone && st.Fr == 0) ++st.idle; else st.idle = 0;
                // a rimshot: until the stick's own modes have rung out
                if (st.idle > (st.rim ? (std::size_t) (0.3 * fs) : 0)) st.active = false;
            }
        ++stepN;
    }

    // facing mics: Rayleigh sums of the new displacement per delay bucket (every step, settling included, so the history is valid)
    void raySums()
    {
        for (auto& M : mics)
        {
            if (! M.facing) continue;
            for (auto b = M.b0; b < M.b1; ++b)
            {
                double* y = M.ys.data() + 6 * b;
                G v1 = 0, v2 = 0;
                for (auto e = M.bs[b]; e < M.bs[b + 1]; ++e)
                {
                    const G uv = u_2[M.gi[e]];
                    v1 += M.w1[e] * uv;
                    v2 += M.w2[e] * uv;
                }
                y[2] = y[1]; y[1] = y[0]; y[0] = v1;
                y[5] = y[4]; y[4] = y[3]; y[3] = v2;
            }
        }
    }

    void outputs()
    {
        const double racs = us2 - 2 * us + us1;
        // batter: sum taps over the active chunks
        double sa[3] = { 0, 0, 0 };
        {
            const double hf = 0.5 * fs;
            const D2 ki = simd::splat (k2i), hv = simd::splat (hf);
            D2 a0v = simd::splat (0.0), a2v = simd::splat (0.0);
            for (std::size_t t = 0; t < nact; ++t)
            {
                const auto b = act[t] * CH;
                for (std::size_t i = b; i < b + CH; i += 2)
                {
                    const D2 q2v = simd::load (bq2.data() + i), qv = simd::load (bq.data() + i), q1v = simd::load (bq1.data() + i);
                    const D2 acc = (q2v - qv - qv + q1v) * ki;
                    const D2 vel = (q2v - q1v) * hv;
                    a0v += simd::load (tapA[0].data() + i) * acc + simd::load (tapV[0].data() + i) * vel;
                    a2v += simd::load (tapA[2].data() + i) * acc + simd::load (tapV[2].data() + i) * vel;
                }
            }
            const double a0 = simd::sum (a0v), a2 = simd::sum (a2v);
            double s1 = 0;
            for (std::size_t j = 0; j < nsp; ++j)
            {
                const auto i = spI[j];
                s1 += tapA[1][i] * (bq2[i] - 2 * bq[i] + bq1[i]) * k2i + tapV[1][i] * (bq2[i] - bq1[i]) * (0.5 * fs);
            }
            sa[0] = a0; sa[1] = s1; sa[2] = a2;
        }
        // wires (radiation of the wire bundle as a moving cylinder)
        double wl = 0;
        for (auto& w : wires)
            wl += (double) accSum (w.v2.data(), w.v.data(), w.v1.data(), 2, w.Nw + 1) * w.h;
        wl *= k2i;
        const double wjerk = (wl - wprev) * fs;
        wprev = wl;
        double sacc[2] = { 0, 0 }, sjerk[2] = { 0, 0 };
        for (int s = 0; s < 2; ++s)
        {
            auto& st = sticks[s];
            if (! st.active) continue;
            const double at = (st.z2 - 2 * st.z + st.z1) * k2i - alift;
            const double ar = (st.zr2 - 2 * st.zr + st.zr1) * k2i - alift;
            double av = st.rim && rimMass > 0 ? 0.5 * (at + ar) : at;
            if (stickModes && st.rim)
                for (int m = 0; m < 3; ++m)
                {
                    const double w = 2 * PI * stickF[m];
                    const double sk = w / (2 * stickQ) * k;
                    const double x2 = (2 * st.sm[m] - (1 - sk) * st.sm1[m] - k2 * w * w * st.sm[m] + k2 * st.Fr / stickMm) / (1 + sk);
                    av += (x2 - 2 * st.sm[m] + st.sm1[m]) * k2i;
                    st.sm1[m] = st.sm[m];
                    st.sm[m] = x2;
                }
            sacc[s] = av;
            sjerk[s] = (av - st.aprev) * fs;
            st.aprev = av;
        }
        double bacc = 0;
        for (std::size_t m = 0; m < NBODY; ++m)
            bacc += bodyA[m] * (m >= 5 ? hoopA : 1) * (qb2[m] - 2 * qb[m] + qb1[m]) * k2i;
        for (int mi = 0; mi < 3; ++mi)
        {
            auto& M = mics[mi];
            auto& o = ring[mi];
            o[(pos + Dc[mi]) & MASK] += sa[mi];
            if (! M.facing)
            {
                const double v = M.g * racs * k2i;
                o[(pos + M.d) & MASK] += v * (1 - M.fr);
                o[(pos + M.d + 1) & MASK] += v * M.fr;
            }
            else
            {
                for (auto b = M.b0; b < M.b1; ++b)
                {
                    const double* y = M.ys.data() + 6 * b;
                    o[(pos + b) & MASK] += (y[0] - 2 * y[1] + y[2]) * k2i;
                    o[(pos + b + 1) & MASK] += (y[3] - 2 * y[4] + y[5]) * k2i;
                }
            }
            o[(pos + M.D) & MASK] += RHO * bacc / (4 * PI * M.R);
            if (stickRad > 0)
                for (int s = 0; s < 2; ++s)
                {
                    const auto& st = sticks[s];
                    if (! (st.active && st.rim)) continue;
                    const double dx = M.x - st.sx, dy = M.y - st.sy, dzm = M.z;
                    const double rr = std::sqrt (dx * dx + dy * dy + dzm * dzm);
                    const double Veff = 2 * PI * stickRad * stickRad * stickLen;
                    const double cth = dzm / rr;
                    const double ps = RHO * Veff * cth / (4 * PI) * (sjerk[s] / (rr * CAIR) + sacc[s] / (rr * rr));
                    const auto is = (std::size_t) (rr / CAIR * fs);
                    if (is < maxd) o[(pos + is) & MASK] += ps;
                }
            const double wacc = 2 * PI * wireR * wireR * wjerk / CAIR * (mi == 1 ? 1.0 : 0.3);
            o[(pos + M.Dw) & MASK] += RHO * wacc / (4 * PI * M.Rw);
        }
    }

    // culling and sleep (once per render call)
    void cull()
    {
        double Etot = 0;
        double* Ec = scratch.data();
        for (std::size_t j = 0; j < nact; ++j)
        {
            const auto b = act[j] * CH;
            double e = 0;
            for (std::size_t i = b; i < b + CH && i < nB; ++i)
            {
                const double dv = (bq[i] - bq1[i]) * fs;
                e += (bW2[i] * bq[i] * bq[i] + dv * dv) / bImu[i];
            }
            Ec[j] = e;
            Etot += e;
        }
        Emax = std::max (Emax, Etot);
        if (! cullOn) return;
        const double lim = Emax * 1e-9;
        const bool busy = sticks[0].active || sticks[1].active;
        std::size_t t = 0;
        for (std::size_t j = 0; j < nact; ++j)
        {
            const auto c = act[j];
            if (Ec[j] < lim && ! busy)
            {
                chOn[c] = 0;
                const auto b = c * CH;
                for (std::size_t i = b; i < b + CH; ++i) { bq[i] = 0; bq1[i] = 0; bq2[i] = 0; }
            }
            else
            {
                act[t++] = c;
            }
        }
        nact = t;
        // sleep: batter energy and the resonant head's kinetic energy 90 dB below their peaks since the last hit, sticks gone
        double kin = 0;
        for (std::size_t rr = 0; rr < nrow; ++rr)
        {
            const auto o = rj[rr] * RN;
            G s = 0;
            for (auto i = ri0[rr]; i < ri1[rr] + 1; ++i)
            {
                const G d = u[o + i] - u_1[o + i];
                s += d * d;
            }
            kin += s;
        }
        Rpeak = std::max (Rpeak, kin);
        kinRel = Rpeak > 0 ? kin / Rpeak : 0;
        // (single precision leaves a floor near -100 dB on the head held by the wires: sleep below -90 dB)
        if (measuring)
        {
            kinFloor = std::max (kinFloor, kin);
            eFloor = std::max (eFloor, Etot);
            return;
        }
        if (Etot < std::max (Emax * 1e-9, 10 * eFloor) && ! busy && kin < std::max (Rpeak * 1e-9, 10 * kinFloor) && epsF == epsTarget)
        {
            if (++quiet > 40) asleep = true;
        }
        else
        {
            quiet = 0;
        }
    }

    void hit (double x, double y, double v, bool rim, int stick)
    {
        if (npend >= std::size (pending)) return;
        std::size_t s;
        if (stick < 0)
        {
            nextStick = 1 - nextStick;
            s = nextStick;
        }
        else
        {
            s = (std::size_t) (stick & 1);
        }
        pending[npend++] = { x, y, v, rim, s };
    }

    void render (float* out, int n, bool add)
    {
        for (std::size_t i = 0; i < npend; ++i) startStick (pending[i]);
        npend = 0;
        if (asleep && epsF == epsTarget)
            return;
        asleep = false;
        const double gT = gMaster * gClose * mixT;
        const double gB = gMaster * gClose * mixB;
        const double gO = gMaster * gOver * mixO;
        for (int i = 0; i < n; ++i)
        {
            step (false);
            const double t = ring[0][pos], b = ring[1][pos], o = ring[2][pos];
            ring[0][pos] = 0;
            ring[1][pos] = 0;
            ring[2][pos] = 0;
            pos = (pos + 1) & MASK;
            if (add)
                out[i] += (float) (gT * t + gB * b + gO * o);
        }
        cull();
    }

    void setParam (int what, double v)
    {
        switch (what)
        {
            case 0: tuneF = v; coefs(); break;
            case 1: lossF = v; coefs(); break;
            case 2: epsTarget = v; asleep = false; break;
            case 3: Ks = baseKs * v; break;
            case 4: gClose = v; break;
            case 5: gOver = v; break;
            case 6: gMaster = v; break;
            case 9: cullOn = v != 0; break;
            default: break;
        }
    }

    // back to the drum as it was loaded (the wires settled, at rest), the
    // settings kept. The arrays are the same sizes, so copying allocates nothing.
    void restore (const Impl& atRest)
    {
        const auto t = tuneF, l = lossF, e = epsTarget, ks = Ks, gc = gClose, go = gOver, gm = gMaster;
        const auto c = cullOn;
        const auto next = nextStick;
        *this = atRest;
        tuneF = t; lossF = l; epsTarget = e; Ks = ks; gClose = gc; gOver = go; gMaster = gm; cullOn = c;
        nextStick = next;
        coefs();
        asleep = epsF == epsTarget;
    }
};

//==============================================================================
Snare::Snare() : impl (std::make_unique<Impl>()) {}
Snare::~Snare() = default;

bool Snare::load (const void* data, std::size_t size)
{
    impl = std::make_unique<Impl>();
    if (impl->load (data, size))
    {
        settled = std::make_unique<Impl> (*impl);
        return true;
    }
    impl = std::make_unique<Impl>();
    settled.reset();
    return false;
}

bool Snare::isLoaded() const                     { return impl->loaded; }
double Snare::getModelRate() const               { return impl->fs; }
void Snare::hit (double x, double y, double v, bool rim, int stick)  { if (impl->loaded) impl->hit (x, y, v, rim, stick); }
void Snare::setParam (int what, double value)    { if (impl->loaded) impl->setParam (what, value); }
void Snare::process (float* mono, int n)         { if (impl->loaded) impl->render (mono, n, true); }
int Snare::getActiveModes() const                { return (int) (impl->nact * CH); }
bool Snare::isAsleep() const                     { return ! impl->loaded || (impl->asleep && impl->npend == 0 && impl->epsF == impl->epsTarget); }
void Snare::reset()                              { if (settled != nullptr) impl->restore (*settled); }

} // namespace carve::plugins::drummodel

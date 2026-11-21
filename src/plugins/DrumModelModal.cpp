// The modal drum engine (toms, kick): a port of the PhysKit project's
// player/rt/drum.zig, line for line where it can be. See DrumModel.h.
//
// Approximations it inherits (the kit's notes/realtime.md):
//  - mic taps: one delay per head and mic, the rest of each mode's delay as a
//    phase at its frequency (two taps);
//  - modes 90dB below the drum's peak energy are frozen; at a hit only the
//    modes the stick reaches and those coupled to the air or the shell wake.
//    Active modes are kept contiguous (a frozen mode swaps places with the
//    last active one), so the per-mode loops run over dense arrays.

#include "DrumModel.h"
#include "DrumModelBessel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
    constexpr int RING = 2048;
    constexpr int MASK = RING - 1;
    constexpr int MAXMIC = 5;
    constexpr int MAXBQ = 4;
    constexpr double PI = 3.14159265358979323846;

    // per-mode f64 fields that move with the mode (slot order)
    enum
    {
        F_CA, F_CB, F_CC, F_CD, F_KN, F_VOL, F_W2, F_IMU,
        F_SA = 8,
        F_SB = 8 + MAXMIC,
        F_SK0 = 8 + 2 * MAXMIC,   // the model's values, for setParam: loss rate x k, k^2 w^2, k^2 / m, w^2
        F_CC0, F_CD0, F_W20,
        NF
    };

    // per-mode i32 fields that move with the mode
    enum { I_M, I_O, I_COUPLED, NI };

    using VecD = std::vector<double>;
    using VecI = std::vector<std::int32_t>;

    struct Head
    {
        std::size_t n = 0;
        double T0 = 0, Ecoef = 0, Ecoef0 = 0, Teff = 0, g = 0, Vs = 0;
        std::array<int, MAXMIC> Dc {};
        std::array<VecD, NF> f;
        std::array<VecI, NI> i;
        VecD kap;   // footprint type 0: radial wavenumber, by orig index (does not move)
        VecD q, q1, q2, frc, E;
        VecI orig, slot;
        std::size_t nact = 0;
        // shell coupling, sparse: (orig mode, shell mode, coefficient); active part rebuilt by lists()
        VecI gI, gS; VecD gC;
        VecI aI, aS; VecD aC;
        std::size_t na = 0;
        VecI volS;
        std::size_t nvol = 0;
        std::array<double, MAXMIC> SB {};   // sum sb a of the previous step, per mic

        void swap (std::size_t a, std::size_t b)
        {
            if (a == b)
                return;

            for (auto& fk : f) std::swap (fk[a], fk[b]);
            for (auto& ik : i) std::swap (ik[a], ik[b]);
            std::swap (q[a], q[b]);
            std::swap (q1[a], q1[b]);
            std::swap (q2[a], q2[b]);
            const auto oa = orig[a], ob = orig[b];
            orig[a] = ob;
            orig[b] = oa;
            slot[(std::size_t) ob] = (std::int32_t) a;
            slot[(std::size_t) oa] = (std::int32_t) b;
        }

        void activate (std::size_t o)
        {
            const auto s = (std::size_t) slot[o];
            if (s < nact)
                return;
            swap (s, nact);
            ++nact;
        }
    };

    struct Stick
    {
        bool active = false;
        VecD w;
        VecI idx, sl;
        std::size_t ns = 0;
        double z = 0, z1 = 0, z2 = 0, M = 0, e0 = 0, e1 = 0;
    };

    struct Hit { double x, y, v; int stick; };

    struct Mic
    {
        double gL = 0, gR = 0, gL0 = 0, gR0 = 0;
        int nbq = 0;
        double bq[MAXBQ][5] {};   // b0 b1 b2 a1 a2
        double st[MAXBQ][4] {};   // x1 x2 y1 y2
        VecD ring;
    };

    // PKD2: "PKD2", then little-endian f64 values only (counts and indices too).
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
        std::size_t u()   { const auto v = f(); return v > 0 && v < 1e9 ? (std::size_t) v : 0; }
        VecD arr (std::size_t n)   { VecD a (n); for (auto& v : a) v = f(); return a; }
        VecI arrI (std::size_t n)  { VecI a (n); for (auto& v : a) v = (std::int32_t) f(); return a; }
    };
}

struct ModalDrum::Impl
{
    bool loaded = false;
    double fs = 48000, k = 0;
    std::size_t nmic = 0;
    std::array<Mic, MAXMIC> mic;
    double a = 0, th0 = 0;
    // contact
    double K = 0, K0 = 0, pw = 1.5, lam = 0, ms = 0, alift = 0;
    std::size_t nsweep = 2;
    double release = 0.02;
    std::size_t fpType = 0;
    double fpR = 0, fpH = 0;
    bool nonlin = true;
    double tmax = 0;
    // outer radiation load
    bool radOn = false;
    double rtau = 0, rA = 0, prad = 0, V1 = 0, V2 = 0;
    // plug in a port
    bool plug = false;
    double mp = 0, Rp = 0, Ap = 0;
    VecD Ph;
    double xp = 0, xp1 = 0, xp2 = 0;
    std::array<std::size_t, MAXMIC> plD {};
    std::array<double, MAXMIC> plG {};
    // shell
    std::size_t ns = 0;
    VecD shCA, shCB, shCD, shA;
    std::array<std::size_t, MAXMIC> shD {};
    std::array<double, MAXMIC> shG {};
    bool shBack = false;
    std::array<VecD, 2> shK;
    VecD shq, shq1, shF;
    std::array<Head, 2> h;
    // cavity
    std::size_t nq = 0;
    VecD om, zeta, Lam, Pm;
    int cavMic = -1;
    std::array<VecI, 2> cOff, cIdx, cS, cN;
    std::array<VecD, 2> cCoef, cC;
    VecD Z, Z1, Z2, Sp, P;
    // footprint
    VecD Jt;
    std::size_t NR = 0, mmax = 0;
    VecD cm, sm;
    std::size_t nfp = 0;   // type 1: nodes (x, y) and mode shapes [node][orig mode]
    VecD fpX, fpY, fpPhi;
    std::array<Stick, 2> sticks;
    int nextStick = 0;
    std::array<Hit, 16> pending {};
    std::size_t npend = 0;
    double gain = 1, tune = 1, loss = 1, tT = 1, cullDb = 90;
    std::size_t pos = 0;
    bool asleep = true;
    double Epeak = 0;

    //==============================================================================
    bool load (const void* data, std::size_t len, double g)
    {
        const auto* bytes = static_cast<const unsigned char*> (data);
        if (len < 4 || std::memcmp (bytes, "PKD2", 4) != 0)
            return false;

        Reader r { bytes + 4, (len - 4) / 8 };
        gain = g;
        fs = r.f();
        if (! (fs > 1000))
            return false;
        k = 1 / fs;
        nmic = r.u();
        if (nmic < 1 || nmic > MAXMIC)
            return false;

        for (std::size_t m = 0; m < nmic; ++m)
        {
            auto& M = mic[m];
            M.gL = M.gL0 = r.f();
            M.gR = M.gR0 = r.f();
            M.nbq = (int) r.u();
            if (M.nbq > MAXBQ)
                return false;
            for (int b = 0; b < M.nbq; ++b)
                for (int c = 0; c < 5; ++c)
                    M.bq[b][c] = r.f();
            M.ring.assign (RING, 0.0);
        }

        a = r.f(); th0 = r.f();
        K = K0 = r.f(); pw = r.f(); lam = r.f(); ms = r.f(); alift = r.f();
        nsweep = r.u(); release = r.f(); fpType = r.u(); fpR = r.f(); fpH = r.f();
        nonlin = r.f() != 0; tmax = r.f();
        radOn = r.f() != 0; rtau = r.f(); rA = r.f();
        plug = r.f() != 0;
        if (plug) { mp = r.f(); Rp = r.f(); Ap = r.f(); }
        for (std::size_t m = 0; m < nmic; ++m) { plD[m] = r.u(); plG[m] = r.f(); }

        ns = r.u();
        shCA = r.arr (ns); shCB = r.arr (ns); shCD = r.arr (ns); shA = r.arr (ns);
        for (std::size_t m = 0; m < nmic; ++m) { shD[m] = r.u(); shG[m] = r.f(); }
        shBack = r.f() != 0;
        if (shBack) { shK[0] = r.arr (ns * ns); shK[1] = r.arr (ns * ns); }
        shq.assign (ns, 0.0); shq1.assign (ns, 0.0); shF.assign (ns, 0.0);

        if (r.bad)
            return false;

        for (std::size_t hd = 0; hd < 2; ++hd)
        {
            auto& H = h[hd];
            const auto n = r.u();
            H.n = n;
            H.T0 = r.f();
            H.Ecoef = H.Ecoef0 = r.f();
            H.Teff = H.T0;
            for (auto& fk : H.f) fk.assign (n + 2, 0.0);
            for (int fi = 0; fi < 8; ++fi)
                for (std::size_t i = 0; i < n; ++i)
                    H.f[(std::size_t) fi][i] = r.f();

            // base values for setParam (models whose loss is all in the rate sk: cA = 2 / (1 + sk))
            for (std::size_t i = 0; i < n; ++i)
            {
                const double sk = 2 / H.f[F_CA][i] - 1;
                H.f[F_SK0][i] = sk;
                H.f[F_CC0][i] = H.f[F_CC][i] * (1 + sk);
                H.f[F_CD0][i] = H.f[F_CD][i] * (1 + sk);
                H.f[F_W20][i] = H.f[F_W2][i];
            }

            for (auto& ik : H.i) { ik = r.arrI (n); ik.resize (n + 2, 0); }

            for (std::size_t m = 0; m < nmic; ++m)
            {
                H.Dc[m] = (int) r.u();
                for (std::size_t i = 0; i < n; ++i) H.f[F_SA + m][i] = r.f();
                for (std::size_t i = 0; i < n; ++i) H.f[F_SB + m][i] = r.f();
            }

            const auto nG = r.u();
            H.gI.assign (nG, 0); H.gS.assign (nG, 0); H.gC.assign (nG, 0.0);
            for (std::size_t t = 0; t < nG; ++t)
            {
                H.gI[t] = (std::int32_t) r.u();
                H.gS[t] = (std::int32_t) r.u();
                H.gC[t] = r.f();
                if ((std::size_t) H.gI[t] >= n || (std::size_t) H.gS[t] >= ns)
                    return false;
            }
            H.aI.assign (nG, 0); H.aS.assign (nG, 0); H.aC.assign (nG, 0.0);

            if (fpType == 0 && hd == 0)
                H.kap = r.arr (n);

            H.q.assign (n + 2, 0.0); H.q1.assign (n + 2, 0.0); H.q2.assign (n + 2, 0.0);
            H.frc.assign (n + 2, 0.0); H.E.assign (n + 2, 0.0);
            H.orig.resize (n); H.slot.resize (n);
            for (std::size_t i = 0; i < n; ++i) { H.orig[i] = (std::int32_t) i; H.slot[i] = (std::int32_t) i; }
            H.volS.assign (n + 1, 0);
        }

        // cavity
        nq = r.u();
        om = r.arr (nq); zeta = r.arr (nq); Lam = r.arr (nq); Pm = r.arr (nq);
        cavMic = (int) r.f();
        if (cavMic >= (int) nmic)
            return false;

        for (std::size_t hd = 0; hd < 2; ++hd)
        {
            const auto start = r.i;
            std::size_t tot = 0;
            for (std::size_t q = 0; q < nq; ++q)
            {
                const auto nb = r.u();
                tot += nb;
                r.i += 2 * nb;
            }
            r.i = start;
            cOff[hd].assign (nq + 1, 0);
            cIdx[hd].assign (tot + 1, 0);
            cCoef[hd].assign (tot + 1, 0.0);
            std::size_t c = 0;
            for (std::size_t q = 0; q < nq; ++q)
            {
                cOff[hd][q] = (std::int32_t) c;
                const auto nb = r.u();
                for (std::size_t t = 0; t < nb; ++t)
                {
                    cIdx[hd][c] = (std::int32_t) r.u();
                    cCoef[hd][c] = r.f();
                    if ((std::size_t) cIdx[hd][c] >= h[hd].n)
                        return false;
                    ++c;
                }
            }
            cOff[hd][nq] = (std::int32_t) c;
            cS[hd].assign (tot + 1, 0);
            cC[hd].assign (tot + 1, 0.0);
            cN[hd].assign (nq + 1, 0);
        }

        if (plug)
            Ph = r.arr (nq);
        Z.assign (nq, 0.0); Z1.assign (nq, 0.0); Z2.assign (nq, 0.0); Sp.assign (nq, 0.0); P.assign (nq, 0.0);

        // footprint
        auto& B = h[0];
        if (fpType == 0)
        {
            double km = 0;
            mmax = 0;
            for (std::size_t i = 0; i < B.n; ++i)
            {
                km = std::max (km, B.kap[i]);
                mmax = std::max (mmax, (std::size_t) B.i[I_M][i]);
            }
            NR = std::max<std::size_t> (64, (std::size_t) std::ceil (km * a / (2 * PI) * 24));
            cm.assign (mmax + 1, 0.0);
            sm.assign (mmax + 1, 0.0);
            Jt.assign (B.n * (NR + 3), 0.0);
            const double hr = a / (double) (NR - 1);
            for (std::size_t i = 0; i < B.n; ++i)
            {
                const auto mi = (int) B.i[I_M][i];
                for (std::size_t j = 0; j < NR + 3; ++j)
                    Jt[i * (NR + 3) + j] = bessel::jn (mi, B.kap[i] * ((double) j - 1) * hr);
            }
        }
        else
        {
            nfp = r.u();
            fpX = r.arr (nfp);
            fpY = r.arr (nfp);
            fpPhi = r.arr (nfp * B.n);
        }

        if (r.bad)
            return false;

        for (auto& s : sticks)
        {
            s.w.assign (B.n + 1, 0.0);
            s.idx.assign (B.n + 1, 0);
            s.sl.assign (B.n + 1, 0);
        }

        for (auto& H : h)
            H.nact = 0;

        lists();
        loaded = true;
        return true;
    }

    void setParam (int what, double v)
    {
        switch (what)
        {
            case 0:
            case 1:
            {
                if (what == 0) tune = v; else loss = v;
                const double t2 = tune * tune;
                for (auto& H : h)
                {
                    for (std::size_t s = 0; s < H.n; ++s)
                    {
                        const double sk = loss * H.f[F_SK0][s];
                        H.f[F_CA][s] = 2 / (1 + sk);
                        H.f[F_CB][s] = (1 - sk) / (1 + sk);
                        H.f[F_CC][s] = t2 * H.f[F_CC0][s] / (1 + sk);
                        H.f[F_CD][s] = H.f[F_CD0][s] / (1 + sk);
                        H.f[F_W2][s] = t2 * H.f[F_W20][s];
                    }
                    H.T0 = H.T0 / tT * t2;   // the tension modulation is relative to the (tuned) tension
                }
                tT = t2;
                break;
            }
            case 2:
                for (auto& H : h) H.Ecoef = H.Ecoef0 * v;
                break;
            case 3:
                K = K0 * v;
                break;
            case 4:
            case 5:
                for (std::size_t m = 0; m < nmic; ++m)
                    if ((m == 0) == (what == 4))
                    {
                        mic[m].gL = mic[m].gL0 * v;
                        mic[m].gR = mic[m].gR0 * v;
                    }
                break;
            default:
                break;
        }
    }

    void hit (double x, double y, double v, int stick)
    {
        if (npend >= pending.size())
            return;

        int st;
        if (stick < 0)
        {
            nextStick = 1 - nextStick;
            st = nextStick;
        }
        else
        {
            st = stick & 1;
        }

        pending[npend++] = { x, y, v, st };
    }

    //==============================================================================
    // coupling lists restricted to the active slots
    void lists()
    {
        for (std::size_t hd = 0; hd < 2; ++hd)
        {
            auto& H = h[hd];
            for (std::size_t q = 0; q < nq; ++q)
            {
                const auto o0 = (std::size_t) cOff[hd][q];
                const auto o1 = (std::size_t) cOff[hd][q + 1];
                auto c = o0;
                for (auto t = o0; t < o1; ++t)
                {
                    const auto s = (std::size_t) H.slot[(std::size_t) cIdx[hd][t]];
                    if (s < H.nact)
                    {
                        cS[hd][c] = (std::int32_t) s;
                        cC[hd][c] = cCoef[hd][t];
                        ++c;
                    }
                }
                cN[hd][q] = (std::int32_t) (c - o0);
            }

            std::size_t c = 0;
            for (std::size_t t = 0; t < H.gI.size(); ++t)
            {
                const auto s = (std::size_t) H.slot[(std::size_t) H.gI[t]];
                if (s < H.nact)
                {
                    H.aI[c] = (std::int32_t) s;
                    H.aS[c] = H.gS[t];
                    H.aC[c] = H.gC[t];
                    ++c;
                }
            }
            H.na = c;

            std::size_t nv = 0;
            for (std::size_t s = 0; s < H.nact; ++s)
                if (H.f[F_VOL][s] != 0)
                    H.volS[nv++] = (std::int32_t) s;
            H.nvol = nv;
        }
    }

    //==============================================================================
    // contact: power law K/(p+1) [eta]^(p+1), Hunt-Crossley loss (drum_common.h)
    static double ctPhi (double K_, double p, double e)
    {
        if (! (e > 0)) return 0;
        return p == 1.5 ? 0.4 * K_ * e * e * std::sqrt (e) : K_ / (p + 1) * std::pow (e, p + 1);
    }
    static double ctDphi (double K_, double p, double e)
    {
        if (! (e > 0)) return 0;
        return p == 1.5 ? K_ * e * std::sqrt (e) : K_ * std::pow (e, p);
    }
    static double ctD2phi (double K_, double p, double e)
    {
        if (! (e > 0)) return 0;
        return p == 1.5 ? 1.5 * K_ * std::sqrt (e) : p * K_ * std::pow (e, p - 1);
    }
    static double ctForce (double K_, double p, double Dd, double e1, double r)
    {
        const double Q = std::abs (r) < 1e-13 ? ctDphi (K_, p, e1 + 0.5 * r) : (ctPhi (K_, p, e1 + r) - ctPhi (K_, p, e1)) / r;
        const double F = Q + Dd * r;
        return F > 0 ? F : 0;
    }
    static double ctDforce (double K_, double p, double Dd, double e1, double r)
    {
        const double dQ = std::abs (r) < 1e-13 ? 0.5 * ctD2phi (K_, p, e1 + 0.5 * r)
                                               : (ctDphi (K_, p, e1 + r) * r - (ctPhi (K_, p, e1 + r) - ctPhi (K_, p, e1))) / (r * r);
        return dQ + Dd;
    }
    double contact (double M, double ef, double e1, double e0) const
    {
        if (ef <= 0 && e1 <= 0 && e0 <= 0) return 0;
        const double p = pw;
        const double Dd = e0 > 0 ? K * lam * ctDphi (1, p, e0) / (2 * k) : 0;
        const double b = ef - e1;
        const double Fb = ctForce (K, p, Dd, e1, b);
        if (Fb <= 0) return 0;
        double hi = b, lo = b - M * Fb, r = b;
        const int iters = p == 1.5 ? 40 : 60;
        const double tol = p == 1.5 ? 1e-15 : 1e-16;
        for (int it = 0; it < iters; ++it)
        {
            const double F = ctForce (K, p, Dd, e1, r);
            const double g = r - b + M * F;
            if (g > 0) hi = r; else lo = r;
            if (std::abs (g) < tol || hi - lo < tol) break;
            double rn = r - g / (1 + M * ctDforce (K, p, Dd, e1, r));
            if (! (rn > lo && rn < hi)) rn = 0.5 * (lo + hi);
            r = rn;
        }
        return ctForce (K, p, Dd, e1, r);
    }

    //==============================================================================
    static double eta (const Stick& st, const VecD& q, double z)
    {
        double e = 0;
        for (std::size_t t = 0; t < st.ns; ++t)
            e += st.w[(std::size_t) st.idx[t]] * q[(std::size_t) st.sl[t]];
        return e - z;
    }

    void startStick (const Hit& hh)
    {
        auto& B = h[0];
        auto& st = sticks[(std::size_t) hh.stick];
        auto& w = st.w;
        std::fill (w.begin(), w.end(), 0.0);
        double sum = 0;

        if (fpType == 0)
        {
            // gaussian footprint of radius fpR sampled on the lattice of spacing fpH through the head centre
            // (as tomlite.c), turned by -th0 into the mode frame; J_m from the table (Catmull-Rom),
            // cos / sin (m theta) by recurrence
            const double hg = fpH, rs = fpR;
            const double c = std::cos (-th0), sn = std::sin (-th0);
            const int ic = (int) std::floor (hh.x / hg);
            const int jc = (int) std::floor (hh.y / hg);
            const int nn = (int) std::ceil (2.2 * rs / hg) + 1;
            for (int j = jc - nn; j <= jc + nn; ++j)
                for (int i = ic - nn; i <= ic + nn; ++i)
                {
                    const double x0 = i * hg, y0 = j * hg;
                    const double d = std::hypot (x0 - hh.x, y0 - hh.y);
                    const double r = std::hypot (x0, y0);
                    if (d >= 2.2 * rs || r >= a) continue;
                    const double x = x0 * c - y0 * sn;
                    const double y = x0 * sn + y0 * c;
                    const double wt = std::exp (-(d / rs) * (d / rs));
                    const double th = std::atan2 (y, x);
                    sum += wt;
                    const double c1 = std::cos (th), s1 = std::sin (th);
                    cm[0] = 1;
                    sm[0] = 0;
                    for (std::size_t m = 1; m <= mmax; ++m)
                    {
                        cm[m] = cm[m - 1] * c1 - sm[m - 1] * s1;
                        sm[m] = sm[m - 1] * c1 + cm[m - 1] * s1;
                    }
                    const double u = r / a * (double) (NR - 1) + 1;
                    const auto iu = (std::size_t) std::floor (u);
                    const double fu = u - std::floor (u), f2 = fu * fu, f3 = f2 * fu;
                    const double k0 = -0.5 * f3 + f2 - 0.5 * fu;
                    const double k1 = 1.5 * f3 - 2.5 * f2 + 1;
                    const double k2c = -1.5 * f3 + 2 * f2 + 0.5 * fu;
                    const double k3 = 0.5 * f3 - 0.5 * f2;
                    for (std::size_t s = 0; s < B.n; ++s)
                    {
                        const auto o = (std::size_t) B.orig[s];
                        const auto mm = (std::size_t) B.i[I_M][s];
                        const auto base = o * (NR + 3) + iu - 1;
                        const double J = k0 * Jt[base] + k1 * Jt[base + 1] + k2c * Jt[base + 2] + k3 * Jt[base + 3];
                        w[o] += wt * J * (B.i[I_O][s] != 0 ? sm[mm] : cm[mm]);
                    }
                }
        }
        else
        {
            // flat felt face: super-gaussian exp(-(d / fpR)^4) over the grid nodes stored with the model
            for (std::size_t p = 0; p < nfp; ++p)
            {
                const double d = std::hypot (fpX[p] - hh.x, fpY[p] - hh.y);
                if (d >= 1.6 * fpR) continue;
                const double rr = d / fpR;
                const double wt = std::exp (-rr * rr * rr * rr);
                sum += wt;
                const double* row = fpPhi.data() + p * B.n;
                for (std::size_t o = 0; o < B.n; ++o)
                    w[o] += wt * row[o];
            }
        }

        if (sum <= 0) return;

        double wmax = 0;
        for (std::size_t s = 0; s < B.n; ++s)
        {
            const auto o = (std::size_t) B.orig[s];
            w[o] /= sum;
            wmax = std::max (wmax, w[o] * w[o] * B.f[F_IMU][s]);
        }

        std::size_t nsel = 0;
        for (std::size_t o = 0; o < B.n; ++o)
        {
            const auto s = (std::size_t) B.slot[o];
            if (w[o] * w[o] * B.f[F_IMU][s] > 1e-10 * wmax)
                st.idx[nsel++] = (std::int32_t) o;
            else
                w[o] = 0;
        }
        st.ns = nsel;

        for (std::size_t t = 0; t < nsel; ++t)
            B.activate ((std::size_t) st.idx[t]);

        for (auto& H : h)
            for (std::size_t o = 0; o < H.n; ++o)
                if (H.i[I_COUPLED][(std::size_t) H.slot[o]] != 0)
                    H.activate (o);

        lists();

        for (auto& S : sticks)
            for (std::size_t t = 0; t < S.ns; ++t)
                S.sl[t] = B.slot[(std::size_t) S.idx[t]];

        double Mi = 0;
        for (std::size_t t = 0; t < nsel; ++t)
        {
            const auto o = (std::size_t) st.idx[t];
            Mi += w[o] * w[o] * B.f[F_CD][(std::size_t) st.sl[t]];
        }
        st.M = Mi + k * k / ms;
        const double head = eta (st, B.q, 0);
        st.z = head + 2.5 * hh.v * k;
        st.z1 = st.z + hh.v * k;
        st.active = true;
        st.e0 = eta (st, B.q, st.z);
        st.e1 = eta (st, B.q1, st.z1);
        asleep = false;
    }

    //==============================================================================
    void tapSums (Head& H, double k2i, double* a3, double* b3, double& g) const
    {
        const auto na = H.nact;
        const auto* q2 = H.q2.data();
        const auto* q = H.q.data();
        const auto* q1 = H.q1.data();
        double gs = 0;

        for (std::size_t m = 0; m < nmic; ++m) { a3[m] = 0; b3[m] = 0; }

        // the acceleration once, then one pass per mic (vectorises)
        auto* acc = H.E.data();   // scratch: E is rewritten by cull before it is read
        for (std::size_t i = 0; i < na; ++i)
        {
            acc[i] = (q2[i] - 2 * q[i] + q1[i]) * k2i;
            gs += H.f[F_KN][i] * q2[i] * q2[i];
        }

        for (std::size_t m = 0; m < nmic; ++m)
        {
            const auto* sa = H.f[F_SA + m].data();
            const auto* sb = H.f[F_SB + m].data();
            double va[4] {}, vb[4] {};
            std::size_t i = 0;
            for (; i + 4 <= na; i += 4)
                for (std::size_t t = 0; t < 4; ++t)
                {
                    va[t] += sa[i + t] * acc[i + t];
                    vb[t] += sb[i + t] * acc[i + t];
                }
            double A = (va[0] + va[1]) + (va[2] + va[3]), Bs = (vb[0] + vb[1]) + (vb[2] + vb[3]);
            for (; i < na; ++i)
            {
                A += sa[i] * acc[i];
                Bs += sb[i] * acc[i];
            }
            a3[m] = A;
            b3[m] = Bs;
        }

        g = gs;
    }

    void process (float* outL, float* outR, int n)
    {
        if (! loaded)
            return;

        if (npend > 0)
        {
            for (std::size_t p = 0; p < npend; ++p)
                startStick (pending[p]);
            npend = 0;
        }

        if (asleep)
            return;

        constexpr double RHO = 1.2, CAIR = 343.0;
        const double k2 = k * k, k2i = 1 / k2;
        const double Kair = RHO * CAIR * CAIR;
        const double rtk = rtau / k;
        const double radC = radOn ? RHO * CAIR * rtau / rA * k2i : 0;
        const double ia2 = 1 / (PI * a * a);
        auto& B = h[0];
        auto& R = h[1];

        for (int j = 0; j < n; ++j)
        {
            const auto p0 = pos;
            for (auto& H : h)
                std::fill (H.frc.begin(), H.frc.begin() + (std::ptrdiff_t) H.nact, 0.0);

            // cavity (+ plug)
            double pcav = 0, Fp = 0;
            if (nq > 0)
            {
                for (std::size_t q = 0; q < nq; ++q)
                {
                    const auto ob = (std::size_t) cOff[0][q];
                    const auto orr = (std::size_t) cOff[1][q];
                    const auto nb = (std::size_t) cN[0][q];
                    const auto nr = (std::size_t) cN[1][q];
                    double sb = 0, sr = 0;
                    for (auto t = ob; t < ob + nb; ++t) sb += cC[0][t] * B.q[(std::size_t) cS[0][t]];
                    for (auto t = orr; t < orr + nr; ++t) sr += cC[1][t] * R.q[(std::size_t) cS[1][t]];
                    double S = sb - sr;
                    if (plug) S -= Ph[q] * Ap * xp;
                    const double w = om[q];
                    const double zk = zeta[q] * w * k;
                    Z2[q] = (2 * Z[q] - (1 - zk) * Z1[q] - k2 * w * w * (Z[q] - S) + 2 * zk * (S - Sp[q])) / (1 + zk);
                    Sp[q] = S;
                    const double Pq = -Kair / Lam[q] * (S - Z[q]);
                    P[q] = Pq;
                    pcav += Pq * Pm[q];
                    if (plug) Fp -= Pq * Ph[q];
                    for (auto t = ob; t < ob + nb; ++t) B.frc[(std::size_t) cS[0][t]] += Pq * cC[0][t];
                    for (auto t = orr; t < orr + nr; ++t) R.frc[(std::size_t) cS[1][t]] -= Pq * cC[1][t];
                }
                std::swap (Z1, Z);    // Z1 <- Z
                std::swap (Z, Z2);    // Z <- Z2, Z2 <- old Z1
            }

            if (plug)
            {
                const double rk = Rp * k / (2 * mp);
                xp2 = (2 * xp - (1 - rk) * xp1 + k2 * Fp * Ap / mp) / (1 + rk);
            }

            if (radOn)
            {
                const double Vv = B.Vs - R.Vs;
                prad = (prad * rtk + radC * (Vv - 2 * V1 + V2)) / (1 + rtk);
                V2 = V1;
                V1 = Vv;
                for (std::size_t t = 0; t < B.nvol; ++t)
                {
                    const auto s = (std::size_t) B.volS[t];
                    B.frc[s] -= prad * B.f[F_VOL][s];
                }
                for (std::size_t t = 0; t < R.nvol; ++t)
                {
                    const auto s = (std::size_t) R.volS[t];
                    R.frc[s] += prad * R.f[F_VOL][s];
                }
            }

            // shell: driven by the rim forces of the heads (and pushing back on them, if shBack)
            double bacc = 0;
            if (ns > 0)
            {
                std::fill (shF.begin(), shF.end(), 0.0);
                for (std::size_t hd = 0; hd < 2; ++hd)
                {
                    auto& H = h[hd];
                    for (std::size_t t = 0; t < H.na; ++t)
                        shF[(std::size_t) H.aS[t]] += H.Teff * H.aC[t] * H.q[(std::size_t) H.aI[t]];
                    if (shBack)
                    {
                        const auto& Km = shK[hd];
                        for (std::size_t s = 0; s < ns; ++s)
                        {
                            double x = 0;
                            for (std::size_t s2 = 0; s2 < ns; ++s2) x += Km[s * ns + s2] * shq[s2];
                            shF[s] -= H.Teff * x;
                        }
                        for (std::size_t t = 0; t < H.na; ++t)
                            H.frc[(std::size_t) H.aI[t]] += H.Teff * H.aC[t] * shq[(std::size_t) H.aS[t]];
                    }
                }
                for (std::size_t s = 0; s < ns; ++s)
                {
                    const double q2 = shCA[s] * shq[s] - shCB[s] * shq1[s] + shCD[s] * shF[s];
                    bacc += shA[s] * (q2 - 2 * shq[s] + shq1[s]) * k2i;
                    shq1[s] = shq[s];
                    shq[s] = q2;
                }
            }

            // heads: free update with tension modulation
            for (auto& H : h)
            {
                double tr = 1;
                if (nonlin)
                {
                    H.Teff = H.T0 + H.Ecoef * H.g * ia2;
                    if (tmax > 0 && H.Teff > tmax * H.T0) H.Teff = tmax * H.T0;
                    tr = H.Teff / H.T0;
                }
                const auto na = H.nact;
                const auto* cA = H.f[F_CA].data();
                const auto* cB = H.f[F_CB].data();
                const auto* cC_ = H.f[F_CC].data();
                const auto* cD = H.f[F_CD].data();
                const auto* q = H.q.data();
                const auto* q1 = H.q1.data();
                const auto* fr = H.frc.data();
                auto* q2 = H.q2.data();
                for (std::size_t i = 0; i < na; ++i)
                    q2[i] = cA[i] * q[i] - cB[i] * q1[i] - cC_[i] * tr * q[i] + cD[i] * fr[i];
            }

            // sticks: nsweep Gauss-Seidel sweeps; with one stick down the gap needs one modal sum per step
            auto& S0 = sticks[0];
            auto& S1 = sticks[1];
            if (S0.active || S1.active)
            {
                const bool both = S0.active && S1.active;
                for (auto& st : sticks)
                    if (st.active) st.z2 = 2 * st.z - st.z1 + k2 * alift;

                if (! both)
                {
                    auto& st = S0.active ? S0 : S1;
                    double ef = eta (st, B.q2, st.z2);
                    double Ft = 0;
                    for (std::size_t sw = 0; sw < nsweep; ++sw)
                    {
                        const double F = contact (st.M, ef, st.e1, st.e0);
                        ef -= st.M * F;
                        Ft += F;
                    }
                    if (Ft > 0)
                    {
                        for (std::size_t t = 0; t < st.ns; ++t)
                        {
                            const auto s = (std::size_t) st.sl[t];
                            B.q2[s] -= Ft * st.w[(std::size_t) st.idx[t]] * B.f[F_CD][s];
                        }
                        st.z2 += k2 * Ft / ms;
                    }
                    st.e1 = st.e0;
                    st.e0 = ef;
                    if (st.z2 - st.z > 0 && -ef > release) st.active = false;
                    st.z1 = st.z;
                    st.z = st.z2;
                }
                else
                {
                    for (std::size_t sw = 0; sw < nsweep; ++sw)
                        for (auto& st : sticks)
                        {
                            const double F = contact (st.M, eta (st, B.q2, st.z2), eta (st, B.q1, st.z1), eta (st, B.q, st.z));
                            if (F > 0)
                            {
                                for (std::size_t t = 0; t < st.ns; ++t)
                                {
                                    const auto sl = (std::size_t) st.sl[t];
                                    B.q2[sl] -= F * st.w[(std::size_t) st.idx[t]] * B.f[F_CD][sl];
                                }
                                st.z2 += k2 * F / ms;
                            }
                        }
                    for (auto& st : sticks)
                    {
                        const double e = eta (st, B.q2, st.z2);
                        st.e1 = st.e0;
                        st.e0 = e;
                        if (st.z2 - st.z > 0 && -e > release) st.active = false;
                        st.z1 = st.z;
                        st.z = st.z2;
                    }
                }
            }

            // mic taps (sums over the active slots), sums for the next step, swap
            for (auto& H : h)
            {
                double a3[MAXMIC], b3[MAXMIC], g = 0;
                tapSums (H, k2i, a3, b3, g);
                double Vv = 0;
                for (std::size_t t = 0; t < H.nvol; ++t)
                {
                    const auto s = (std::size_t) H.volS[t];
                    Vv += H.f[F_VOL][s] * H.q2[s];
                }
                H.g = g;
                H.Vs = Vv;
                std::swap (H.q1, H.q);   // q1 <- q
                std::swap (H.q, H.q2);   // q <- q2, q2 <- old q1
                for (std::size_t mi = 0; mi < nmic; ++mi)
                {
                    // mic = sum sa a_n + sum sb a_n-1 (the sb sum of the previous step)
                    mic[mi].ring[(p0 + (std::size_t) H.Dc[mi]) & MASK] += a3[mi] + H.SB[mi];
                    H.SB[mi] = b3[mi];
                }
            }

            // shell, plug and cavity pressure into the mics
            const double pacc = plug ? (xp2 - 2 * xp + xp1) * k2i : 0;
            for (std::size_t mi = 0; mi < nmic; ++mi)
            {
                auto& M = mic[mi];
                if (ns > 0) M.ring[(p0 + shD[mi]) & MASK] += shG[mi] * bacc;
                if (plug) M.ring[(p0 + plD[mi]) & MASK] += plG[mi] * pacc;
            }
            if (cavMic >= 0)
                mic[(std::size_t) cavMic].ring[p0] += pcav;
            if (plug)
            {
                xp1 = xp;
                xp = xp2;
            }

            // mic filters and the stereo mix
            double l = 0, rr = 0;
            for (std::size_t mi = 0; mi < nmic; ++mi)
            {
                auto& M = mic[mi];
                double x = M.ring[p0];
                M.ring[p0] = 0;
                for (int b = 0; b < M.nbq; ++b)
                {
                    const auto* c = M.bq[b];
                    auto* s = M.st[b];
                    const double y = c[0] * x + c[1] * s[0] + c[2] * s[1] - c[3] * s[2] - c[4] * s[3];
                    s[1] = s[0];
                    s[0] = x;
                    s[3] = s[2];
                    s[2] = y;
                    x = y;
                }
                l += M.gL * x;
                rr += M.gR * x;
            }
            outL[j] += (float) (l * gain);
            outR[j] += (float) (rr * gain);
            pos = (p0 + 1) & MASK;
        }

        cull();
    }

    // freeze the modes whose energy is cullDb below the drum's peak energy since it last fell asleep
    void cull()
    {
        if (sticks[0].active || sticks[1].active) return;
        double Et = 0;
        for (auto& H : h)
            for (std::size_t s = 0; s < H.nact; ++s)
            {
                const double v = (H.q[s] - H.q1[s]) * fs;
                const double e = (v * v + H.f[F_W2][s] * H.q[s] * H.q[s]) / H.f[F_IMU][s];
                H.E[s] = e;
                Et += e;
            }
        if (Et > Epeak) Epeak = Et;
        const double thr = Epeak * std::pow (10.0, -cullDb / 10);
        std::size_t alive = 0;
        bool changed = false;
        for (auto& H : h)
        {
            std::size_t s = 0;
            while (s < H.nact)
            {
                if (H.E[s] > thr) { ++s; continue; }
                H.q[s] = 0;
                H.q1[s] = 0;
                H.q2[s] = 0;
                H.E[s] = H.E[H.nact - 1];
                H.swap (s, H.nact - 1);
                --H.nact;
                changed = true;
            }
            alive += H.nact;
        }
        if (alive == 0)
        {
            // asleep once the mic filters and delay lines have run out too (a few ms later: the rings are cleared as read)
            asleep = true;
            Epeak = 0;
            resetState();
        }
        if (changed) lists();
    }

    void resetState()
    {
        for (auto& H : h)
        {
            std::fill (H.q.begin(), H.q.end(), 0.0);
            std::fill (H.q1.begin(), H.q1.end(), 0.0);
            std::fill (H.q2.begin(), H.q2.end(), 0.0);
            H.g = 0;
            H.Vs = 0;
            H.Teff = H.T0;
            H.nact = 0;
            H.SB = {};
        }
        std::fill (Z.begin(), Z.end(), 0.0);
        std::fill (Z1.begin(), Z1.end(), 0.0);
        std::fill (Z2.begin(), Z2.end(), 0.0);
        std::fill (Sp.begin(), Sp.end(), 0.0);
        std::fill (shq.begin(), shq.end(), 0.0);
        std::fill (shq1.begin(), shq1.end(), 0.0);
        prad = V1 = V2 = 0;
        xp = xp1 = xp2 = 0;
        for (std::size_t m = 0; m < nmic; ++m)
        {
            std::fill (mic[m].ring.begin(), mic[m].ring.end(), 0.0);
            std::memset (mic[m].st, 0, sizeof (mic[m].st));
        }
    }

    void silence()
    {
        if (! loaded) return;
        npend = 0;
        for (auto& s : sticks) s.active = false;
        asleep = true;
        Epeak = 0;
        resetState();
        lists();
    }
};

//==============================================================================
ModalDrum::ModalDrum() : impl (std::make_unique<Impl>()) {}
ModalDrum::~ModalDrum() = default;

bool ModalDrum::load (const void* data, std::size_t size, double gain)
{
    impl = std::make_unique<Impl>();
    if (impl->load (data, size, gain))
        return true;
    impl = std::make_unique<Impl>();
    return false;
}

bool ModalDrum::isLoaded() const                    { return impl->loaded; }
double ModalDrum::getModelRate() const              { return impl->fs; }
void ModalDrum::hit (double x, double y, double v, int stick)  { if (impl->loaded) impl->hit (x, y, v, stick); }
void ModalDrum::setParam (int what, double value)   { if (impl->loaded) impl->setParam (what, value); }
void ModalDrum::process (float* l, float* r, int n) { impl->process (l, r, n); }
int ModalDrum::getActiveModes() const               { return (int) (impl->h[0].nact + impl->h[1].nact); }
bool ModalDrum::isAsleep() const                    { return impl->asleep && impl->npend == 0; }
void ModalDrum::reset()                             { impl->silence(); }

} // namespace carve::plugins::drummodel

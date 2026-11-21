#pragma once

#include <cmath>

// Bessel J_n for the stick footprints of Modelled Drums (DrumModel.h): the
// Numerical Recipes approximations and Miller's recurrence, as the PhysKit
// engines have them (so the tables come out the same).
namespace carve::plugins::drummodel::bessel
{

inline double j0 (double x)
{
    const double ax = std::abs (x);
    if (ax < 8)
    {
        const double y = x * x;
        return (57568490574.0 + y * (-13362590354.0 + y * (651619640.7 + y * (-11214424.18 + y * (77392.33017 + y * (-184.9052456))))))
             / (57568490411.0 + y * (1029532985.0 + y * (9494680.718 + y * (59272.64853 + y * (267.8532712 + y)))));
    }
    const double z = 8 / ax, y = z * z, xx = ax - 0.785398164;
    const double a1 = 1 + y * (-0.1098628627e-2 + y * (0.2734510407e-4 + y * (-0.2073370639e-5 + y * 0.2093887211e-6)));
    const double a2 = -0.1562499995e-1 + y * (0.1430488765e-3 + y * (-0.6911147651e-5 + y * (0.7621095161e-6 - y * 0.934935152e-7)));
    return std::sqrt (0.636619772 / ax) * (std::cos (xx) * a1 - z * std::sin (xx) * a2);
}

inline double j1 (double x)
{
    const double ax = std::abs (x);
    if (ax < 8)
    {
        const double y = x * x;
        return x * (72362614232.0 + y * (-7895059235.0 + y * (242396853.1 + y * (-2972611.439 + y * (15704.48260 + y * (-30.16036606))))))
                 / (144725228442.0 + y * (2300535178.0 + y * (18583304.74 + y * (99447.43394 + y * (376.9991397 + y)))));
    }
    const double z = 8 / ax, y = z * z, xx = ax - 2.356194491;
    const double a1 = 1 + y * (0.183105e-2 + y * (-0.3516396496e-4 + y * (0.2457520174e-5 + y * (-0.240337019e-6))));
    const double a2 = 0.04687499995 + y * (-0.2002690873e-3 + y * (0.8449199096e-5 + y * (-0.88228987e-6 + y * 0.105787412e-6)));
    const double r = std::sqrt (0.636619772 / ax) * (std::cos (xx) * a1 - z * std::sin (xx) * a2);
    return x < 0 ? -r : r;
}

inline double jn (int n, double x)
{
    if (n == 0) return j0 (x);
    if (n == 1) return j1 (x);
    const double ax = std::abs (x);
    if (! (ax > 0)) return 0;
    double ans = 0;
    const double nf = n;

    if (ax > nf)
    {
        const double tox = 2 / ax;
        double bjm = j0 (ax), bj = j1 (ax);
        for (int j = 1; j < n; ++j)
        {
            const double bjp = j * tox * bj - bjm;
            bjm = bj;
            bj = bjp;
        }
        ans = bj;
    }
    else
    {
        const double tox = 2 / ax;
        const int m = 2 * ((n + (int) std::floor (std::sqrt (160 * nf))) / 2);
        bool jsum = false;
        double bjp = 0, sum = 0, bj = 1;
        for (int j = m; j > 0; --j)
        {
            const double bjm = j * tox * bj - bjp;
            bjp = bj;
            bj = bjm;
            if (std::abs (bj) > 1e10)
            {
                bj *= 1e-10;
                bjp *= 1e-10;
                ans *= 1e-10;
                sum *= 1e-10;
            }
            if (jsum) sum += bj;
            jsum = ! jsum;
            if (j == n) ans = bjp;
        }
        ans /= 2 * sum - bj;
    }
    return x < 0 && (n & 1) == 1 ? -ans : ans;
}

} // namespace carve::plugins::drummodel::bessel

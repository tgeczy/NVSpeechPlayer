// Linear Predictive Coding (LPC) for formant extraction from short
// voiced-speech segments. Designed to replace naive FFT+smoothing peak
// detection in tests where harmonic interference at typical F0 frequencies
// (~140 Hz) makes direct spectral peak-finding unreliable.
//
// Method: autocorrelation → Levinson-Durbin → LPC coefficients a_1..a_p.
// The all-pole spectrum H(z) = 1 / A(z), where A(z) = 1 - sum(a_k z^-k),
// approximates the vocal-tract transfer function. Peaks in |H(e^jw)| are
// the formants. Evaluated via FFT of the padded coefficient vector.
//
// Standard parameters for 22050 Hz voiced speech:
//   Pre-emphasis coefficient: 0.97
//   Window: Hamming
//   LPC order: sampleRate/1000 + 2 ≈ 24  (we use 14-16 for shorter windows)
//
// References: Makhoul 1975 "Linear Prediction: A Tutorial Review";
// Rabiner & Schafer "Theory and Applications of Digital Speech Processing";
// O'Shaughnessy "Speech Communications: Human and Machine".

#ifndef TGSB_TEST_LPC_H
#define TGSB_TEST_LPC_H

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <vector>

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "pocketfft.h"
#include "spectrum_helpers.h"

namespace tgsb_test {

// Copy `length` samples from `pcm` starting at `start`, convert to
// double in [-1,1], and return as a vector.
inline std::vector<double> pcmSlice(
    const std::vector<std::int16_t>& pcm,
    std::size_t start, std::size_t length)
{
    std::vector<double> out(length, 0.0);
    for (std::size_t i = 0; i < length; ++i) {
        const std::size_t idx = start + i;
        if (idx < pcm.size()) out[i] = static_cast<double>(pcm[idx]) / 32768.0;
    }
    return out;
}

// Pre-emphasis filter y[n] = x[n] - alpha * x[n-1] (typically alpha=0.97).
// Flattens the glottal-source spectral tilt so LPC can better model the
// vocal-tract resonances. Operates in place.
inline void preEmphasize(std::vector<double>& x, double alpha = 0.97) {
    if (x.empty()) return;
    for (std::size_t i = x.size() - 1; i > 0; --i) {
        x[i] -= alpha * x[i - 1];
    }
    x[0] *= (1.0 - alpha);
}

// Apply a Hamming window in place.
inline void hammingWindow(std::vector<double>& x) {
    const std::size_t N = x.size();
    if (N < 2) return;
    for (std::size_t i = 0; i < N; ++i) {
        const double w = 0.54 - 0.46 * std::cos(2.0 * M_PI *
            static_cast<double>(i) / static_cast<double>(N - 1));
        x[i] *= w;
    }
}

// Compute biased autocorrelation r[k] = sum_n x[n]*x[n+k] for k=0..maxLag.
inline std::vector<double> autocorrelation(const std::vector<double>& x,
                                            std::size_t maxLag)
{
    std::vector<double> r(maxLag + 1, 0.0);
    for (std::size_t k = 0; k <= maxLag; ++k) {
        double sum = 0.0;
        for (std::size_t n = 0; n + k < x.size(); ++n) {
            sum += x[n] * x[n + k];
        }
        r[k] = sum;
    }
    return r;
}

// Levinson-Durbin recursion for the all-pole autocorrelation problem.
// Given r[0..p], returns (a[0..p], gain), where a[0]=1 and A(z) = 1 -
// sum_{k=1..p} a[k] z^-k. gain = sqrt(E) where E is the final residual
// energy.
struct LpcCoeffs {
    std::vector<double> a;
    double gain = 0.0;
};

inline LpcCoeffs levinsonDurbin(const std::vector<double>& r, std::size_t order) {
    LpcCoeffs out;
    out.a.assign(order + 1, 0.0);
    out.a[0] = 1.0;
    if (r.empty() || r[0] <= 0.0) {
        out.gain = 0.0;
        return out;
    }
    double E = r[0];
    std::vector<double> a_prev(order + 1, 0.0);
    a_prev[0] = 1.0;
    for (std::size_t i = 1; i <= order; ++i) {
        double k = -r[i];
        for (std::size_t j = 1; j < i; ++j) k -= a_prev[j] * r[i - j];
        if (std::abs(E) < 1e-20) break;
        k /= E;

        std::vector<double> a_new = a_prev;
        a_new[i] = k;
        for (std::size_t j = 1; j < i; ++j) {
            a_new[j] = a_prev[j] + k * a_prev[i - j];
        }
        a_prev = a_new;
        E *= (1.0 - k * k);
        if (E <= 0.0) break;
    }
    // Convert convention: we want A(z) = 1 - sum(a_k z^-k), and
    // Levinson produced coefficients such that r[i] + sum(a[j]*r[i-j]) = 0,
    // i.e. the polynomial A'(z) = 1 + sum(a[j] z^-j). Flip signs to match
    // the standard "minus" convention used for the LPC spectrum formula.
    for (std::size_t i = 1; i <= order; ++i) a_prev[i] = -a_prev[i];
    out.a = std::move(a_prev);
    out.gain = (E > 0.0) ? std::sqrt(E) : 0.0;
    return out;
}

// Compute the LPC magnitude spectrum |H(e^jw)| = gain / |1 - sum(a_k e^-jwk)|
// at fftLen/2 + 1 frequency bins using pocketfft.
inline std::vector<double> lpcSpectrum(const LpcCoeffs& c, std::size_t fftLen) {
    std::vector<double> padded(fftLen, 0.0);
    padded[0] = 1.0;
    for (std::size_t i = 1; i < c.a.size() && i < fftLen; ++i) {
        padded[i] = -c.a[i];  // A(z) = 1 - sum(a_k z^-k), so feed -a_k into FFT
    }
    std::vector<std::complex<double>> ft(fftLen / 2 + 1);
    pocketfft::r2c(pocketfft::shape_t{fftLen},
                   pocketfft::stride_t{sizeof(double)},
                   pocketfft::stride_t{sizeof(std::complex<double>)},
                   pocketfft::shape_t{0}, pocketfft::FORWARD,
                   padded.data(), ft.data(), 1.0);
    std::vector<double> env(ft.size(), 0.0);
    for (std::size_t i = 0; i < ft.size(); ++i) {
        const double m = std::abs(ft[i]);
        env[i] = (m > 1e-12) ? (c.gain / m) : 0.0;
    }
    return env;
}

// ---- Laguerre's method: polynomial root-finding via complex iteration
// on LPC coefficients, with forward deflation and root polishing on the
// original polynomial. Gives exact formant frequency AND bandwidth, unlike
// FFT-magnitude peak detection which gives only frequency.
//
// The LPC polynomial as stored is A(z) = 1 - sum_{k=1..p} a_k z^-k. Substituting
// x = z^-1 makes it a standard ascending polynomial P(x) = 1 - a_1 x - a_2 x^2
// - ... - a_p x^p with coefficients [1, -a_1, -a_2, ..., -a_p]. Laguerre finds
// roots x_i; we then INVERT to get true z-plane roots z_i = 1 / x_i before
// converting to Hz. Missing this inversion is a classic LPC bug.

using Cplx = std::complex<double>;

// Combined Horner evaluation of P, P', P''/2 at complex x.
// Input: p[k] = coefficient of x^k, so p has degree = p.size() - 1.
// Output fills P, P1 (first deriv), P2half (second deriv / 2).
inline void evalPolyWithDerivatives(const std::vector<Cplx>& p, Cplx x,
                                    Cplx& P, Cplx& P1, Cplx& P2half)
{
    const std::size_t n = p.size();
    if (n == 0) { P = P1 = P2half = 0.0; return; }
    P = p[n - 1];
    P1 = 0.0;
    P2half = 0.0;
    for (std::size_t k = n - 1; k > 0; --k) {
        P2half = P2half * x + P1;
        P1     = P1     * x + P;
        P      = P      * x + p[k - 1];
    }
}

// One Laguerre iteration step. Returns the new x.
inline Cplx laguerreStep(const std::vector<Cplx>& p, Cplx x)
{
    const double n = static_cast<double>(p.size() - 1);
    if (n <= 0.0) return x;
    Cplx P, P1, P2half;
    evalPolyWithDerivatives(p, x, P, P1, P2half);
    if (std::abs(P) < 1e-14) return x;  // root already nailed
    const Cplx P2 = 2.0 * P2half;
    const Cplx G  = P1 / P;
    const Cplx H  = G * G - P2 / P;
    const Cplx disc = std::sqrt((n - 1.0) * (n * H - G * G));
    const Cplx denomA = G + disc;
    const Cplx denomB = G - disc;
    const Cplx denom  = (std::abs(denomA) > std::abs(denomB)) ? denomA : denomB;
    if (std::abs(denom) < 1e-14) return x;
    return x - n / denom;
}

// Iterate Laguerre until convergence (|delta| small or |P(x)| small).
// Returns best root found within maxIters.
inline Cplx laguerreFindRoot(const std::vector<Cplx>& p, Cplx start = {0.0, 0.0},
                              int maxIters = 200)
{
    Cplx x = start;
    for (int iter = 0; iter < maxIters; ++iter) {
        Cplx next = laguerreStep(p, x);
        const Cplx d = next - x;
        x = next;
        if (std::abs(d) < 1e-14 * (1.0 + std::abs(x))) break;
        Cplx P, P1, P2half;
        evalPolyWithDerivatives(p, x, P, P1, P2half);
        if (std::abs(P) < 1e-14) break;
    }
    return x;
}

// Synthetic division: divide P(x) by (x - root), returning the quotient
// polynomial of degree deg(P) - 1. Coefficients indexed ascending (p[0]
// is x^0 term).
inline std::vector<Cplx> deflateRoot(const std::vector<Cplx>& p, Cplx root)
{
    if (p.size() < 2) return {};
    // P(x) = (x - root) Q(x) + remainder.
    // Quotient coeffs computed via Horner-style reverse sweep:
    //   q_{n-1} = p_n
    //   q_{k-1} = p_k + root * q_k    (for k from n-1 down to 1)
    const std::size_t n = p.size() - 1;  // degree
    std::vector<Cplx> q(n);
    q[n - 1] = p[n];
    for (std::size_t k = n - 1; k > 0; --k) {
        q[k - 1] = p[k] + root * q[k];
    }
    return q;
}

// Find all roots of an LPC polynomial via deflation, then polish each
// against the ORIGINAL (non-deflated) polynomial to erase accumulated
// forward-deflation error. Returns exactly deg(original) complex roots.
//
// The `coeffs` argument follows our LpcCoeffs sign convention: A(z) = 1
// - sum_{k=1..p} a_k z^-k, so the polynomial-in-x = z^-1 has coefficients
// [1, -a_1, -a_2, ..., -a_p].
inline std::vector<Cplx> findLpcRoots(const LpcCoeffs& coeffs)
{
    const std::size_t p = coeffs.a.size() > 0 ? coeffs.a.size() - 1 : 0;
    if (p == 0) return {};

    // Build the polynomial in x = z^-1 with ASCENDING coefficients:
    //   poly[0] = 1, poly[k] = -coeffs.a[k] for k = 1..p
    std::vector<Cplx> poly(p + 1);
    poly[0] = 1.0;
    for (std::size_t k = 1; k <= p; ++k) poly[k] = -coeffs.a[k];

    const std::vector<Cplx> original = poly;

    std::vector<Cplx> roots;
    roots.reserve(p);
    std::vector<Cplx> working = poly;
    for (std::size_t i = 0; i < p; ++i) {
        Cplx start = {0.0, 0.0};  // Laguerre is globally convergent; start at origin
        Cplx root = laguerreFindRoot(working, start, 200);
        roots.push_back(root);
        working = deflateRoot(working, root);
        if (working.size() < 2) break;
    }

    // Polish each root against the original polynomial. One or two Laguerre
    // iterations on the un-deflated polynomial snaps accumulated deflation
    // error out of every root.
    for (Cplx& r : roots) {
        for (int k = 0; k < 3; ++k) {
            r = laguerreStep(original, r);
        }
    }

    return roots;
}

// Convert LPC polynomial roots in x = z^-1 space to formant (freq, bw)
// pairs in z space. Applies the standard filters:
//   - invert x_i -> z_i = 1 / x_i (the critical z^-1 unpacking step)
//   - keep only roots with positive imaginary part (drop conjugate pairs)
//   - drop roots with bandwidth > maxBandwidthHz (phantom poles from
//     spectral tilt or from LPC's approximation of zeros)
//   - drop roots with frequency outside [minHz, maxHz]
// Returns sorted by frequency ascending.
struct FormantRoot {
    double freqHz;
    double bandwidthHz;
};

inline std::vector<FormantRoot> rootsToFormants(
    const std::vector<Cplx>& xRoots, int sampleRate,
    double minHz = 200.0, double maxHz = 5000.0,
    double maxBandwidthHz = 400.0)
{
    std::vector<FormantRoot> out;
    out.reserve(xRoots.size() / 2);
    const double fsOver2Pi = static_cast<double>(sampleRate) / (2.0 * M_PI);
    const double fsOverPi  = static_cast<double>(sampleRate) / M_PI;

    for (const Cplx& x : xRoots) {
        if (std::abs(x) < 1e-12) continue;
        const Cplx z = 1.0 / x;
        if (z.imag() <= 0.0) continue;  // drop conjugate-pair negatives
        const double f = fsOver2Pi * std::atan2(z.imag(), z.real());
        if (f < minHz || f > maxHz) continue;
        const double mag = std::abs(z);
        if (mag >= 1.0 || mag <= 0.0) continue;  // pole outside unit circle
        const double bw = -fsOverPi * std::log(mag);
        if (bw > maxBandwidthHz) continue;  // phantom / too-damped
        out.push_back({f, bw});
    }
    std::sort(out.begin(), out.end(),
              [](const FormantRoot& a, const FormantRoot& b) {
                  return a.freqHz < b.freqHz;
              });
    return out;
}

// High-level: extract formants via LPC + Laguerre root-finding + filtering.
// This is the "sense without ears" version with bandwidths — use in place
// of extractFormantsLPC() when you need to trust the numbers.
struct LpcRootFormants {
    std::vector<FormantRoot> formants;
    bool valid = false;
};

inline LpcRootFormants extractFormantsViaRoots(
    const std::vector<std::int16_t>& pcm,
    std::size_t centerSample,
    int sampleRate,
    std::size_t windowLen = 512,
    int lpcOrder = 14,
    double maxBandwidthHz = 400.0)
{
    LpcRootFormants out;
    if (centerSample < windowLen / 2) centerSample = windowLen / 2;
    const std::size_t start = centerSample - windowLen / 2;

    auto slice = pcmSlice(pcm, start, windowLen);
    preEmphasize(slice, 0.97);
    hammingWindow(slice);

    auto r = autocorrelation(slice, static_cast<std::size_t>(lpcOrder));
    if (r.empty() || r[0] <= 0.0) return out;
    auto c = levinsonDurbin(r, static_cast<std::size_t>(lpcOrder));
    auto roots = findLpcRoots(c);
    out.formants = rootsToFormants(roots, sampleRate,
                                   /*min*/ 150.0, /*max*/ static_cast<double>(sampleRate) / 2.0 - 100.0,
                                   maxBandwidthHz);
    out.valid = !out.formants.empty();
    return out;
}

// High-level: extract the first `nFormants` formant frequencies from a
// PCM slice using LPC. Returns them sorted ascending.
struct LpcFormants {
    std::vector<double> freqsHz;  // F1, F2, F3, ...
    bool valid = false;
};

inline LpcFormants extractFormantsLPC(
    const std::vector<std::int16_t>& pcm,
    std::size_t centerSample,
    int sampleRate,
    std::size_t windowLen = 512,
    int lpcOrder = 14,
    std::size_t envFftLen = 2048,
    std::size_t nFormants = 4,
    double minHz = 200.0,
    double maxHz = 4000.0)
{
    LpcFormants out;
    if (centerSample < windowLen / 2) centerSample = windowLen / 2;
    const std::size_t start = centerSample - windowLen / 2;

    auto slice = pcmSlice(pcm, start, windowLen);
    preEmphasize(slice, 0.97);
    hammingWindow(slice);

    auto r = autocorrelation(slice, static_cast<std::size_t>(lpcOrder));
    if (r.empty() || r[0] <= 0.0) return out;

    auto c = levinsonDurbin(r, static_cast<std::size_t>(lpcOrder));
    auto env = lpcSpectrum(c, envFftLen);

    auto peaks = findFormantPeaks(env, sampleRate, envFftLen,
                                  minHz, maxHz, nFormants);
    out.freqsHz.reserve(peaks.size());
    for (const auto& p : peaks) out.freqsHz.push_back(p.freqHz);
    out.valid = !peaks.empty();
    return out;
}

}  // namespace tgsb_test

#endif  // TGSB_TEST_LPC_H

// Spectral-analysis helpers built on pocketfft (header-only BSD-3, vendored
// as tests/cpp/pocketfft.h). Turns captured PCM into magnitude spectra and
// then into formant peaks — the "sense without ears" primitive layer that
// per-test assertions build on top of.

#ifndef TGSB_TEST_SPECTRUM_HELPERS_H
#define TGSB_TEST_SPECTRUM_HELPERS_H

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "pocketfft.h"

namespace tgsb_test {

// Compute the magnitude spectrum of a Hann-windowed slice of PCM.
//
// pcm:          full int16 mono buffer (as produced by synthesizeToPcm)
// startSample:  position in the buffer where the analysis window begins
// length:       analysis-window length in samples (power of two preferred
//               for fastest FFT; otherwise any positive integer works)
//
// Returns length/2 + 1 magnitude bins. Bin k corresponds to frequency
// k * sampleRate / length. DC is bin 0; Nyquist is the last bin.
// If startSample/length go outside the buffer, those samples are treated
// as zero — standard zero-padding behavior.
inline std::vector<double> computeMagnitudeSpectrum(
    const std::vector<std::int16_t>& pcm,
    std::size_t startSample,
    std::size_t length,
    bool applyHann = true)
{
    std::vector<double> input(length, 0.0);
    for (std::size_t i = 0; i < length; ++i) {
        const std::size_t idx = startSample + i;
        if (idx < pcm.size()) {
            input[i] = static_cast<double>(pcm[idx]) / 32768.0;
        }
    }
    if (applyHann && length > 1) {
        for (std::size_t i = 0; i < length; ++i) {
            const double w = 0.5 * (1.0 - std::cos(2.0 * M_PI *
                static_cast<double>(i) / static_cast<double>(length - 1)));
            input[i] *= w;
        }
    }

    std::vector<std::complex<double>> output(length / 2 + 1);
    const pocketfft::shape_t shape{length};
    const pocketfft::stride_t strideIn{sizeof(double)};
    const pocketfft::stride_t strideOut{sizeof(std::complex<double>)};
    const pocketfft::shape_t axes{0};
    pocketfft::r2c(shape, strideIn, strideOut, axes, pocketfft::FORWARD,
                   input.data(), output.data(), /*fct*/ 1.0);

    std::vector<double> mag(output.size());
    for (std::size_t i = 0; i < output.size(); ++i) {
        mag[i] = std::abs(output[i]);
    }
    return mag;
}

// Smooth a magnitude spectrum with a box-average kernel `kernelBins` wide
// (centered; edges handled with zero-padding). Used to suppress harmonic
// ripples from voiced speech (which otherwise dominate raw FFT peaks) and
// expose the formant-envelope peaks. Choose kernelBins to cover about one
// fundamental period — e.g. if F0≈140 Hz and binHz≈5.4 Hz, a kernel of
// 26 bins averages ~140 Hz which wipes out single-harmonic peaks.
inline std::vector<double> smoothSpectrum(
    const std::vector<double>& spectrum, std::size_t kernelBins)
{
    if (kernelBins < 2 || spectrum.size() <= kernelBins) return spectrum;
    std::vector<double> out(spectrum.size(), 0.0);
    const std::ptrdiff_t half = static_cast<std::ptrdiff_t>(kernelBins / 2);
    for (std::size_t i = 0; i < spectrum.size(); ++i) {
        double sum = 0.0;
        std::size_t cnt = 0;
        for (std::ptrdiff_t k = -half; k <= half; ++k) {
            const std::ptrdiff_t idx = static_cast<std::ptrdiff_t>(i) + k;
            if (idx >= 0 && idx < static_cast<std::ptrdiff_t>(spectrum.size())) {
                sum += spectrum[static_cast<std::size_t>(idx)];
                ++cnt;
            }
        }
        out[i] = (cnt > 0) ? sum / static_cast<double>(cnt) : 0.0;
    }
    return out;
}

struct FormantPeak {
    double freqHz;
    double magnitude;
};

// Find local-maxima peaks in a magnitude spectrum within a frequency band
// and return the top `maxPeaks` by magnitude, sorted by frequency ascending.
// Uses parabolic interpolation around each peak bin for sub-bin frequency
// resolution.
//
// For formant extraction, typical args: minFreq=150, maxFreq=5000.
// Set maxPeaks=5 to capture F1–F5.
inline std::vector<FormantPeak> findFormantPeaks(
    const std::vector<double>& spectrum,
    int sampleRate, std::size_t fftLength,
    double minFreq = 150.0,
    double maxFreq = 5000.0,
    std::size_t maxPeaks = 5)
{
    const std::size_t N = spectrum.size();
    if (N < 3) return {};
    const double binHz = static_cast<double>(sampleRate) /
                         static_cast<double>(fftLength);

    const std::size_t minBin = std::max<std::size_t>(
        1, static_cast<std::size_t>(minFreq / binHz));
    const std::size_t maxBin = std::min<std::size_t>(
        N - 2, static_cast<std::size_t>(maxFreq / binHz));

    std::vector<FormantPeak> peaks;
    for (std::size_t i = minBin; i <= maxBin; ++i) {
        if (spectrum[i] > spectrum[i - 1] && spectrum[i] > spectrum[i + 1]) {
            // Parabolic interpolation in log-magnitude for sub-bin precision.
            const double a = spectrum[i - 1];
            const double b = spectrum[i];
            const double c = spectrum[i + 1];
            const double denom = (a - 2.0 * b + c);
            const double p = (denom != 0.0) ? 0.5 * (a - c) / denom : 0.0;
            const double freq = (static_cast<double>(i) + p) * binHz;
            peaks.push_back({freq, b});
        }
    }

    if (peaks.size() > maxPeaks) {
        std::partial_sort(peaks.begin(),
                          peaks.begin() + static_cast<std::ptrdiff_t>(maxPeaks),
                          peaks.end(),
                          [](const FormantPeak& a, const FormantPeak& b) {
                              return a.magnitude > b.magnitude;
                          });
        peaks.resize(maxPeaks);
    }
    std::sort(peaks.begin(), peaks.end(),
              [](const FormantPeak& a, const FormantPeak& b) {
                  return a.freqHz < b.freqHz;
              });
    return peaks;
}

// Convenience: compute a spectrum centered on `centerSample` with a
// power-of-two window chosen for FFT efficiency. Returns the FFT length
// alongside the spectrum so callers can turn bins into Hz.
struct WindowedSpectrum {
    std::vector<double> magnitude;
    std::size_t fftLength = 0;
};

inline WindowedSpectrum spectrumAt(
    const std::vector<std::int16_t>& pcm,
    std::size_t centerSample,
    std::size_t fftLength = 1024)
{
    WindowedSpectrum out;
    if (centerSample < fftLength / 2) centerSample = fftLength / 2;
    const std::size_t start = centerSample - fftLength / 2;
    out.magnitude = computeMagnitudeSpectrum(pcm, start, fftLength, /*hann*/ true);
    out.fftLength = fftLength;
    return out;
}

// Produce a formant-envelope-friendly spectrum at the given center by using
// a long FFT window (high frequency resolution) AND applying a smoothing
// kernel sized to wipe out individual harmonic peaks at the expected pitch.
//
// Use this — not spectrumAt() alone — when hunting for formants in voiced
// speech. Typical args: fftLength=4096, smoothHz=150 (covers one F0 period
// at 140 Hz).
inline WindowedSpectrum smoothedEnvelopeAt(
    const std::vector<std::int16_t>& pcm,
    std::size_t centerSample,
    int sampleRate,
    std::size_t fftLength = 4096,
    double smoothHz = 150.0)
{
    WindowedSpectrum out = spectrumAt(pcm, centerSample, fftLength);
    const double binHz = static_cast<double>(sampleRate) /
                         static_cast<double>(fftLength);
    const std::size_t kernelBins = std::max<std::size_t>(
        3, static_cast<std::size_t>(smoothHz / binHz));
    out.magnitude = smoothSpectrum(out.magnitude, kernelBins);
    return out;
}

}  // namespace tgsb_test

#endif  // TGSB_TEST_SPECTRUM_HELPERS_H

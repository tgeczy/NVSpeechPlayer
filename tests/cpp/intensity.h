// Consonant-intensity measurements for PCM audio windows.
//
// LPC tests tell us WHERE formants sit. Intensity tests tell us HOW LOUD
// a consonant is relative to flanking vowels. That's the perceptual axis
// testers are reporting when they say "/ɣ/ is almost imperceptible" — a
// frequency measurement alone can't distinguish an audible /ɣ/ from a
// silent one at the same cf2.
//
// Target references from acoustic phonetics literature:
//   Jongman 2000 ("Acoustic Characteristics of English Fricatives"):
//     nonsibilant fricatives sit at -17 to -18 dB relative to flanking
//     vowel amplitude in connected speech.
//   Kingston 2008 ("Lenition"): Spanish [ɣ̞] approximants use intensity
//     difference (IntDiff) as the primary lenition cue — quieter than
//     flanking vowels, but audibly continuant (not silent).
//
// So for Spanish intervocalic /ɣ/ we expect something like -15 to -25 dB
// relative to flanking vowels. If we measure -30+ dB below vowel, the
// phoneme is effectively inaudible.

#ifndef TGSB_TEST_INTENSITY_H
#define TGSB_TEST_INTENSITY_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tgsb_test {

// RMS amplitude of a PCM slice, returned as a fraction in [0, 1].
// Window is [start, start+length) in sample indices; clamped to buffer.
inline double rmsAmplitude(const std::vector<std::int16_t>& pcm,
                           std::size_t start, std::size_t length) {
    if (length == 0 || start >= pcm.size()) return 0.0;
    const std::size_t end = std::min(start + length, pcm.size());
    const std::size_t actualLength = end - start;
    if (actualLength == 0) return 0.0;
    double sumSquares = 0.0;
    for (std::size_t i = start; i < end; ++i) {
        const double x = static_cast<double>(pcm[i]) / 32768.0;
        sumSquares += x * x;
    }
    return std::sqrt(sumSquares / static_cast<double>(actualLength));
}

// Express a linear RMS ratio as dB: 20*log10(rmsA / rmsB).
// Handles zero/near-zero inputs by clamping to -120 dB floor.
inline double dbRatio(double rmsA, double rmsB) {
    if (rmsB < 1e-12) return 0.0;    // reference is silent; ratio undefined
    if (rmsA < 1e-12) return -120.0; // target is silent; floor
    return 20.0 * std::log10(rmsA / rmsB);
}

// Convenience: measure RMS in a window and return dB relative to a
// reference RMS (e.g., flanking-vowel average).
inline double dbRelativeTo(const std::vector<std::int16_t>& pcm,
                           std::size_t windowStart, std::size_t windowLen,
                           double referenceRms) {
    const double rms = rmsAmplitude(pcm, windowStart, windowLen);
    return dbRatio(rms, referenceRms);
}

}  // namespace tgsb_test

#endif  // TGSB_TEST_INTENSITY_H

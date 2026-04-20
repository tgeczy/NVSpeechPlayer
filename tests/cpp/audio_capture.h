// In-process audio-capture harness for acoustic-level tests.
//
// Synthesizes an IPA input into a PCM buffer by wiring nvspFrontend's
// frame callback directly to speechPlayer (no platform audio sinks).
// Output is a flat std::vector<int16_t> of mono PCM at the chosen
// sample rate.
//
// This is the foundation for "sensing without ears": once we have PCM,
// we can FFT it, extract formant peaks, measure spectral centroids,
// check for residual-energy ghosts, etc. — the real DSP, not a Python
// port, exercising the actual code users hear.

#ifndef TGSB_TEST_AUDIO_CAPTURE_H
#define TGSB_TEST_AUDIO_CAPTURE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "pack.h"
#include "ipa_engine.h"
#include "nvspFrontend.h"
#include "speechPlayer.h"
#include "sample.h"

namespace tgsb_test {

// Per-utterance state the queueing callback forwards into speechPlayer.
// Stored on-heap and passed as userData; the callback converts each
// FrameExCallback invocation into a speechPlayer_queueFrameEx call.
struct QueueContext {
    speechPlayer_handle_t player = nullptr;
    int sampleRate = 22050;
    double totalExpectedMs = 0.0;  // running sum of all frame durations
};

// Converts a ms value into an integer sample count at the given rate.
inline unsigned int msToSamples(double ms, int sampleRate) {
    if (ms <= 0.0) return 0;
    const double s = std::ceil(ms * static_cast<double>(sampleRate) / 1000.0);
    return static_cast<unsigned int>(std::max(1.0, s));
}

// FrameExCallback that queues each emitted frame into the speechPlayer.
// Signature matches nvspFrontend_FrameExCallback.
inline void queuingCallback(void* userData,
                            const nvspFrontend_Frame* f,
                            const nvspFrontend_FrameEx* fEx,
                            double durationMs, double fadeMs, int userIndex) {
    auto* ctx = static_cast<QueueContext*>(userData);
    const unsigned int minSamples = std::max(1u, msToSamples(durationMs, ctx->sampleRate));
    const unsigned int fadeSamples = std::max(1u, msToSamples(fadeMs, ctx->sampleRate));

    if (f) {
        // speechPlayer takes non-const pointers for historic reasons;
        // safe to const_cast because queueFrameEx only reads the frame.
        speechPlayer_queueFrameEx(
            ctx->player,
            const_cast<speechPlayer_frame_t*>(reinterpret_cast<const speechPlayer_frame_t*>(f)),
            reinterpret_cast<const speechPlayer_frameEx_t*>(fEx),
            fEx ? static_cast<unsigned int>(sizeof(speechPlayer_frameEx_t)) : 0u,
            minSamples, fadeSamples, userIndex, /*purgeQueue*/ false);
    } else {
        // Silence frame: queue the gap duration with a null frame pointer.
        speechPlayer_queueFrame(ctx->player, nullptr, minSamples, fadeSamples,
                                userIndex, false);
    }
    ctx->totalExpectedMs += durationMs;
}

// Synthesize `ipa` through the full TGSpeechBox pipeline (frontend +
// passes + DSP) into a mono int16 PCM buffer at `sampleRate`. Returns an
// empty vector on error (caller should check .empty()).
//
// The pack must already be loaded on `frontendHandle` (via
// nvspFrontend_setLanguage). `frontendHandle` can be from a live pack
// fixture — this function neither creates nor destroys it.
inline std::vector<std::int16_t> synthesizeToPcm(
    nvspFrontend_handle_t frontendHandle,
    const std::string& ipa,
    double speed = 1.0,
    double basePitch = 140.0,
    double inflection = 0.5,
    int sampleRate = 22050)
{
    if (!frontendHandle) return {};

    QueueContext ctx;
    ctx.sampleRate = sampleRate;
    ctx.player = speechPlayer_initialize(sampleRate);
    if (!ctx.player) return {};

    const int rc = nvspFrontend_queueIPA_Ex(
        frontendHandle, ipa.c_str(),
        speed, basePitch, inflection,
        ".", /*userIndexBase*/ 0,
        &queuingCallback, &ctx);

    std::vector<std::int16_t> result;
    if (rc == 0) {
        speechPlayer_terminate(ctx.player);
        return result;  // frontend error
    }

    // Drain samples. Request chunks until we either hit the expected
    // duration plus a tail margin, or speechPlayer returns zero.
    const int expectedSamples = static_cast<int>(
        std::ceil(ctx.totalExpectedMs * sampleRate / 1000.0));
    const int maxSamples = expectedSamples + sampleRate / 5;  // +200 ms tail
    result.reserve(static_cast<std::size_t>(maxSamples));

    std::vector<sample> chunk(1024);
    int remaining = maxSamples;
    while (remaining > 0) {
        const unsigned int want = static_cast<unsigned int>(
            std::min(remaining, static_cast<int>(chunk.size())));
        const int got = speechPlayer_synthesize(ctx.player, want, chunk.data());
        if (got <= 0) break;
        for (int i = 0; i < got; ++i) {
            result.push_back(chunk[static_cast<std::size_t>(i)].value);
        }
        remaining -= got;
        if (static_cast<unsigned int>(got) < want) break;  // fully drained
    }

    speechPlayer_terminate(ctx.player);
    return result;
}

// Convenience: same as synthesizeToPcm but uses a loaded PackSet rather
// than going through a frontend handle. Useful when a test already has
// a PackSet (e.g. from PackFixture) and wants to avoid the handle round-trip.
// NOTE: this uses the convertIpaToTokens + manual emission path; if tests
// need identical behavior to production they should use synthesizeToPcm
// against a real handle instead.

}  // namespace tgsb_test

#endif  // TGSB_TEST_AUDIO_CAPTURE_H

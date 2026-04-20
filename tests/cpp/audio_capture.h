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
    // Sample index at which each emitted frame begins. Index i of the
    // frame callback invocations corresponds to samplePositions[i].
    // Used to map phoneme frame_index from NVSP_DATA_FRAMETRACE into the
    // right sample offset in the captured PCM buffer.
    std::vector<std::size_t> samplePositions;
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

    // Record the sample-index position of this frame's start so tests can
    // later map frame_trace indices to PCM sample offsets.
    const double msSoFar = ctx->totalExpectedMs;
    const std::size_t samplePos = static_cast<std::size_t>(
        msSoFar * static_cast<double>(ctx->sampleRate) / 1000.0);
    ctx->samplePositions.push_back(samplePos);

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
// Capture that also exposes the per-phoneme start-sample positions.
// samplePositions[i] is the sample index at which the i-th frame
// callback began. To find where phoneme K begins in the PCM buffer,
// read the frame_trace via nvspFrontend_queryData(NVSP_DATA_FRAMETRACE)
// and look up samplePositions[trace_entry.frameIndex].
struct SynthesisResult {
    std::vector<std::int16_t> pcm;
    std::vector<std::size_t> samplePositions;  // one entry per emitted frame
    int sampleRate = 22050;
};

inline SynthesisResult synthesizeToPcmWithTrace(
    nvspFrontend_handle_t frontendHandle,
    const std::string& ipa,
    double speed = 1.0,
    double basePitch = 140.0,
    double inflection = 0.5,
    int sampleRate = 22050);

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

inline SynthesisResult synthesizeToPcmWithTrace(
    nvspFrontend_handle_t frontendHandle,
    const std::string& ipa,
    double speed,
    double basePitch,
    double inflection,
    int sampleRate)
{
    SynthesisResult res;
    res.sampleRate = sampleRate;
    if (!frontendHandle) return res;

    QueueContext ctx;
    ctx.sampleRate = sampleRate;
    ctx.player = speechPlayer_initialize(sampleRate);
    if (!ctx.player) return res;

    const int rc = nvspFrontend_queueIPA_Ex(
        frontendHandle, ipa.c_str(),
        speed, basePitch, inflection,
        ".", 0, &queuingCallback, &ctx);
    if (rc == 0) {
        speechPlayer_terminate(ctx.player);
        return res;
    }

    const int expectedSamples = static_cast<int>(
        std::ceil(ctx.totalExpectedMs * sampleRate / 1000.0));
    const int maxSamples = expectedSamples + sampleRate / 5;
    res.pcm.reserve(static_cast<std::size_t>(maxSamples));

    std::vector<sample> chunk(1024);
    int remaining = maxSamples;
    while (remaining > 0) {
        const unsigned int want = static_cast<unsigned int>(
            std::min(remaining, static_cast<int>(chunk.size())));
        const int got = speechPlayer_synthesize(ctx.player, want, chunk.data());
        if (got <= 0) break;
        for (int i = 0; i < got; ++i) {
            res.pcm.push_back(chunk[static_cast<std::size_t>(i)].value);
        }
        remaining -= got;
        if (static_cast<unsigned int>(got) < want) break;
    }

    res.samplePositions = std::move(ctx.samplePositions);
    speechPlayer_terminate(ctx.player);
    return res;
}

// Read NVSP_DATA_FRAMETRACE JSON and return a list of (frameIndex, phonemeKey).
// Pure string parsing — no external JSON library. The JSON we emit is
// flat and contains no escape sequences, so naive parsing is safe.
struct TraceEntry { int frameIndex; std::string phonemeKey; };
inline std::vector<TraceEntry> readFrameTrace(nvspFrontend_handle_t h) {
    std::vector<TraceEntry> out;
    char* raw = nvspFrontend_queryData(h, NVSP_DATA_FRAMETRACE, "", 0, 0);
    if (!raw) return out;
    std::string s(raw);
    nvspFrontend_freeString(raw);
    // Extremely minimal parse: look for "frameIndex":N,"phonemeKey":"K"
    std::size_t p = 0;
    while (true) {
        const auto fi = s.find("\"frameIndex\":", p);
        if (fi == std::string::npos) break;
        const auto numStart = fi + 13;
        const auto numEnd = s.find(',', numStart);
        if (numEnd == std::string::npos) break;
        const int idx = std::atoi(s.substr(numStart, numEnd - numStart).c_str());
        const auto kStart = s.find("\"phonemeKey\":\"", numEnd);
        if (kStart == std::string::npos) break;
        const auto kBegin = kStart + 14;
        const auto kEnd = s.find('"', kBegin);
        if (kEnd == std::string::npos) break;
        out.push_back({idx, s.substr(kBegin, kEnd - kBegin)});
        p = kEnd + 1;
    }
    return out;
}

}  // namespace tgsb_test

#endif  // TGSB_TEST_AUDIO_CAPTURE_H

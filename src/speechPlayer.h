/*
TGSpeechBox — Public C API header for the DSP engine.
Copyright 2014 NV Access Limited.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSPEECHBOX_SPEECHPLAYER_H
#define TGSPEECHBOX_SPEECHPLAYER_H

#include "frame.h"
#include "sample.h"
#include "voicingTone.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* speechPlayer_handle_t;

/* ============================================================================
 * Core API (unchanged for ABI compatibility)
 * ============================================================================ */

speechPlayer_handle_t speechPlayer_initialize(int sampleRate);
void speechPlayer_queueFrame(speechPlayer_handle_t playerHandle, speechPlayer_frame_t* framePtr, unsigned int minFrameDuration, unsigned int fadeDuration, int userIndex, bool purgeQueue);
void speechPlayer_queueFrameEx(speechPlayer_handle_t playerHandle, speechPlayer_frame_t* framePtr, const speechPlayer_frameEx_t* frameExPtr, unsigned int frameExSize, unsigned int minFrameDuration, unsigned int fadeDuration, int userIndex, bool purgeQueue);
int speechPlayer_synthesize(speechPlayer_handle_t playerHandle, unsigned int sampleCount, sample* sampleBuf); 
int speechPlayer_getLastIndex(speechPlayer_handle_t playerHandle);
void speechPlayer_terminate(speechPlayer_handle_t playerHandle);

/* ============================================================================
 * Extended API (safe ABI extension - old drivers won't call these)
 * ============================================================================ */

/**
 * Set voicing tone parameters for DSP-level voice quality adjustments.
 * 
 * This is an optional API extension. Old drivers that never call this function
 * will get identical behavior to before (defaults are used).
 * 
 * New frontends/tools can call this to adjust:
 *   - Glottal pulse shape (crispness)
 *   - Voiced pre-emphasis (clarity)
 *   - High-shelf EQ (brightness)
 * 
 * @param playerHandle  Handle returned by speechPlayer_initialize()
 * @param tone          Pointer to voicing tone parameters, or NULL to reset to defaults
 */
void speechPlayer_setVoicingTone(speechPlayer_handle_t playerHandle, const speechPlayer_voicingTone_t* tone);

/**
 * Get current voicing tone parameters.
 * 
 * @param playerHandle  Handle returned by speechPlayer_initialize()
 * @param tone          Output pointer to receive current parameters
 */
void speechPlayer_getVoicingTone(speechPlayer_handle_t playerHandle, speechPlayer_voicingTone_t* tone);

/**
 * Set output gain applied before the limiter.
 *
 * Each platform's audio output chain has different amplification levels.
 * By applying gain inside the DSP (before the limiter), all platforms
 * get identical clipping and limiting behavior for the same phoneme data.
 * Default is 1.0 (no gain).  Typical values: NVDA=1.0, iOS=1.7, Android=3.0.
 *
 * @param playerHandle  Handle returned by speechPlayer_initialize()
 * @param gain          Output gain multiplier (clamped to 0.0–10.0)
 */
void speechPlayer_setOutputGain(speechPlayer_handle_t playerHandle, double gain);

/**
 * Get the DSP version implemented by this DLL.
 *
 * This is intended for frontends/drivers that want to detect whether a newer
 * DSP feature-set is available (or avoid calling APIs that would misbehave on
 * an older build).
 */
unsigned int speechPlayer_getDspVersion(void);

/**
 * Like speechPlayer_synthesize, but stops at user-index boundaries.
 *
 * When a frame queued with userIndex != -1 starts generating, synthesis
 * stops early: the samples returned are exactly the audio that precedes
 * the marker, and *indexReached receives the marker's index. If no marker
 * was crossed, *indexReached is set to -1. A return of 0 with
 * *indexReached >= 0 means a marker sits at the head of the queue (no
 * audio precedes it); a return of 0 with *indexReached == -1 means no
 * audio is available (same as speechPlayer_synthesize returning 0).
 *
 * This lets a driver bind index-reached notifications to the exact audio
 * chunk that precedes each marker (NVDA needs this for punctual
 * IndexCommand/BeepCommand callbacks). Platforms that don't use indexes
 * can keep calling speechPlayer_synthesize — behavior there is unchanged.
 *
 * @param playerHandle  Handle returned by speechPlayer_initialize()
 * @param sampleCount   Maximum samples to generate
 * @param sampleBuf     Output buffer
 * @param indexReached  Receives the reached index or -1; must not be NULL
 * @return Number of samples generated
 */
int speechPlayer_synthesize2(speechPlayer_handle_t playerHandle, unsigned int sampleCount, sample* sampleBuf, int* indexReached);

/**
 * Set time-stretch factor for DSP-level rate boost.
 *
 * 1.0 = normal (no stretching). 2.0 = skip every other glottal cycle
 * for 2x speedup without formant compression.  Uses pitch-synchronous
 * cycle skipping with linear crossfade at boundaries.  Inspired by
 * Sonic (Bill Cox) but implemented natively — the DSP knows exact
 * glottal cycle timing.
 *
 * @param playerHandle  Handle returned by speechPlayer_initialize()
 * @param factor        Time-stretch factor (clamped to 1.0–8.0)
 */
void speechPlayer_setTimeStretch(speechPlayer_handle_t playerHandle, double factor);

#ifdef __cplusplus
}
#endif

#endif

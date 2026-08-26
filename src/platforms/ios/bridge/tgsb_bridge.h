/*
 * tgsb_bridge.h — Plain C API for TGSpeechBox pipeline.
 *
 * Wraps eSpeak + nvspFrontend + speechPlayer into a simple interface
 * callable from Swift via a bridging header.
 *
 * License: GPL-3.0 (links eSpeak-ng GPL with TGSpeechBox MIT)
 */

#ifndef TGSB_BRIDGE_H
#define TGSB_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TgsbEngine TgsbEngine;

/* --- Lifecycle --- */
TgsbEngine *tgsb_create(const char *espeakDataPath,
                         const char *packDir,
                         int sampleRate);
void tgsb_destroy(TgsbEngine *engine);

/* --- Configuration --- */

/* Set an override directory checked before the bundle for lang YAML files.
   Call after tgsb_create, before tgsb_set_language. Pass NULL to clear. */
void tgsb_set_override_directory(TgsbEngine *engine, const char *overrideDir);

/* Export merged YAML (base + overrides) for a domain.
   Returns malloc'd string — caller must free(). Returns NULL on error. */
char *tgsb_export_data(TgsbEngine *engine, int domain,
                       const char *langTag, const char *overridesJson);

int tgsb_set_language(TgsbEngine *engine,
                      const char *espeakLang,
                      const char *tgsbLang);
int tgsb_set_voice(TgsbEngine *engine, const char *voiceName);

/* --- Synthesis --- */
void tgsb_queue_text(TgsbEngine *engine,
                     const char *text,
                     double speed,
                     double pitch);

/*
 * Segmented synthesis for SSML <break> support (#pause-mode):
 *   tgsb_begin_utterance  resets the stop flag and purges stale frames —
 *                         call ONCE per speech request;
 *   tgsb_queue_text_ex    queues one text segment (beginUtterance=0 keeps
 *                         previously queued segments; a stop between
 *                         segments sticks);
 *   tgsb_queue_silence    queues real silence (ms, capped at 2000) so a
 *                         VoiceOver break renders at its requested length.
 * tgsb_queue_text() remains equivalent to begin+queue in one call.
 */
void tgsb_begin_utterance(TgsbEngine *engine);
void tgsb_queue_text_ex(TgsbEngine *engine,
                        const char *text,
                        double speed,
                        double pitch,
                        int beginUtterance);
void tgsb_queue_silence(TgsbEngine *engine, double ms);

/*
 * Pull synthesized PCM (s16le, mono) into outBuffer.
 * Returns number of samples written (0 = done).
 * Call in a loop until it returns 0.
 */
int tgsb_pull_audio(TgsbEngine *engine,
                    int16_t *outBuffer,
                    int maxSamples);

void tgsb_stop(TgsbEngine *engine);

/* --- Voice quality --- */
void tgsb_set_voicing_tone(TgsbEngine *engine,
    double voicedTiltDbPerOct,
    double noiseGlottalModDepth,
    double pitchSyncF1DeltaHz,
    double pitchSyncB1DeltaHz,
    double speedQuotient,
    double aspirationTiltDbPerOct,
    double cascadeBwScale,
    double tremorDepth,
    double nasalBwScale,
    double f4FreqScale,
    double nasalGainScale,
    double chorusDepth,
    double chorusDetuneHz);

void tgsb_set_frame_ex_defaults(TgsbEngine *engine,
    double creakiness,
    double breathiness,
    double jitter,
    double shimmer,
    double sharpness);

/* --- Pitch mode --- */
int tgsb_set_pitch_mode(TgsbEngine *engine, const char *mode);
void tgsb_set_legacy_pitch_inflection_scale(TgsbEngine *engine, double scale);

/* --- Inflection (pitch range scaling, 0..1) --- */
void tgsb_set_inflection(TgsbEngine *engine, double inflection);

/* --- Pause mode (0=off, 1=short, 2=long) --- */
void tgsb_set_pause_mode(TgsbEngine *engine, int mode);

/* --- Sample rate (reinitializes DSP only, keeps eSpeak/frontend) --- */
void tgsb_set_sample_rate(TgsbEngine *engine, int sampleRate);

/* --- Voice preset info --- */
int tgsb_get_num_voices(void);
const char *tgsb_get_voice_name(int index);

/* --- Voice profiles (YAML-defined in phonemes.yaml) --- */

/*
 * Set a voice profile by name (e.g. "Beth", "Bobby").
 * Profiles apply formant overrides and voicingTone from the pack.
 * Pass NULL or "" to clear the active profile.
 * Returns 1 on success, 0 if profile not found.
 */
int tgsb_set_voice_profile(TgsbEngine *engine, const char *profileName);

/*
 * Get available voice profile names (newline-separated).
 * Caller must free() the returned string.
 * Returns NULL if no profiles available.
 */
char *tgsb_get_voice_profile_names(TgsbEngine *engine);

/* --- Pack settings editor --- */

/*
 * DEPRECATED: Use tgsb_set_data(engine, TGSB_DATA_SETTINGS, langTag, key, value)
 * for per-key overrides instead. This YAML batch path is retained for
 * backwards compatibility but all callers have been migrated.
 */
int tgsb_apply_setting_overrides(TgsbEngine *engine, const char *yamlSnippet);

/*
 * Get available language tags (newline-separated).
 * Caller must free() the returned string.
 */
char *tgsb_get_available_languages(TgsbEngine *engine);

/* Free a string returned by tgsb_query_data or tgsb_get_available_languages. */
void tgsb_free_string(char *str);

/* --- Phoneme preview --- */

/*
 * Preview a single phoneme in isolation (bypasses eSpeak + pipeline).
 * Queues DSP frames directly — call tgsb_pull_audio() afterwards.
 * Returns 1 on success, 0 if phoneme not found.
 */
int tgsb_preview_phoneme(TgsbEngine *engine, const char *phonemeKey,
                         double pitchHz, double durationMs);

/* --- Generic Data Query API (ABI v5+) --- */

/* Domain constants (match NVSP_DATA_* in nvspFrontend.h). */
#define TGSB_DATA_SETTINGS   0
#define TGSB_DATA_PHONEMES   1
#define TGSB_DATA_DICTIONARY 2

/* Count records in a domain for a language. Returns -1 on error. */
int tgsb_get_data_count(TgsbEngine *engine, int domain, const char *langTag);

/* Query a page of typed records as a JSON array string.
   Caller must free with tgsb_free_string(). Returns NULL on error. */
char *tgsb_query_data(TgsbEngine *engine, int domain, const char *langTag,
                      int offset, int limit);

/* Set a single value in a domain. Returns 1 on success, 0 on failure. */
int tgsb_set_data(TgsbEngine *engine, int domain, const char *langTag,
                  const char *key, const char *value);

/* Convert text to IPA via eSpeak.  Caller must free() the result. */
char *tgsb_text_to_ipa(TgsbEngine *engine, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* TGSB_BRIDGE_H */

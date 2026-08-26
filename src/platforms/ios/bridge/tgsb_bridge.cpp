/*
 * tgsb_bridge.cpp — C bridge implementation for macOS/iOS.
 *
 * Translates tgsb_jni.cpp (Android JNI bridge) to plain C functions.
 * Same pipeline: text -> eSpeak IPA -> nvspFrontend -> speechPlayer -> PCM
 *
 * License: GPL-3.0
 */

#include "tgsb_bridge.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <mutex>
#include <string>

#include <espeak-ng/speak_lib.h>
#include "speechPlayer.h"
#include "nvspFrontend.h"

/* ------------------------------------------------------------------ */
/* eSpeak lifecycle — refcounted process-global                        */
/*                                                                     */
/* eSpeak state is process-global while TgsbEngine is per-instance.    */
/* iOS 27 instantiates extra audio units in the same extension         */
/* process; an unconditional espeak_Initialize/espeak_Terminate per    */
/* engine reset the global voice to en-us (or tore eSpeak down) under  */
/* whichever engine was still speaking — en-us phonemization through a */
/* non-English pack.  First engine in initializes, last one out        */
/* terminates.  g_espeakCurrentLang mirrors the global voice so each   */
/* engine can re-assert its own language per utterance with a string   */
/* compare instead of a voice reload.                                  */
/* ------------------------------------------------------------------ */

static int g_espeakRefCount = 0;
static std::mutex g_espeakLock;
static std::string g_espeakCurrentLang;

static bool espeakAcquire(const char *dataPath) {
    std::lock_guard<std::mutex> lock(g_espeakLock);
    if (g_espeakRefCount == 0) {
        /* espeakINITIALIZE_DONT_EXIT (0x8000): return an error instead
         * of calling exit(1) if data files are not found. */
        int sr = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0,
                                   dataPath, 0x8000);
        if (sr <= 0) return false;

        /* With DONT_EXIT, espeak_Initialize can return a positive sample
         * rate even if data loading failed — verify with a voice set. */
        espeak_VOICE spec;
        memset(&spec, 0, sizeof(spec));
        spec.languages = "en-us";
        if (espeak_SetVoiceByProperties(&spec) != EE_OK) {
            espeak_Terminate();
            return false;
        }
        g_espeakCurrentLang = "en-us";
    }
    g_espeakRefCount++;
    return true;
}

static void espeakRelease() {
    std::lock_guard<std::mutex> lock(g_espeakLock);
    if (g_espeakRefCount > 0 && --g_espeakRefCount == 0) {
        espeak_Terminate();
        g_espeakCurrentLang.clear();
    }
}

/* Set the global eSpeak voice if it isn't already lang.
 * Caller must hold g_espeakLock. */
static int espeakSetLanguageLocked(const char *lang) {
    if (g_espeakCurrentLang == lang) return 1;
    espeak_VOICE spec;
    memset(&spec, 0, sizeof(spec));
    spec.languages = lang;
    if (espeak_SetVoiceByProperties(&spec) != EE_OK) return 0;
    g_espeakCurrentLang = lang;
    return 1;
}

static int espeakSetLanguage(const char *lang) {
    std::lock_guard<std::mutex> lock(g_espeakLock);
    return espeakSetLanguageLocked(lang);
}

/* ------------------------------------------------------------------ */
/* Emoji spacing — pad emoji codepoints with spaces so eSpeak treats  */
/* them as separate words for $textmode dictionary lookup.             */
/* ------------------------------------------------------------------ */

static std::string padEmojiWithSpaces(const char *text) {
    std::string out;
    out.reserve(strlen(text) * 2);
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        // 4-byte UTF-8: emoji in U+1F000..U+1FFFF (F0 9F xx xx)
        if (p[0] == 0xF0 && p[1] >= 0x9F && p[1] <= 0x9F &&
            (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            if (!out.empty() && out.back() != ' ') out += ' ';
            out += (char)p[0]; out += (char)p[1];
            out += (char)p[2]; out += (char)p[3];
            p += 4;
            while (p[0] == 0xEF && p[1] == 0xB8 && (p[2] == 0x8E || p[2] == 0x8F)) {
                out += (char)p[0]; out += (char)p[1]; out += (char)p[2];
                p += 3;
            }
            if (*p && *p != ' ') out += ' ';
            continue;
        }
        // 3-byte UTF-8: U+2600..U+27BF (E2 98 80..E2 9E BF)
        if (p[0] == 0xE2 && p[1] >= 0x98 && p[1] <= 0x9E &&
            (p[2] & 0xC0) == 0x80) {
            if (!out.empty() && out.back() != ' ') out += ' ';
            out += (char)p[0]; out += (char)p[1]; out += (char)p[2];
            p += 3;
            while (p[0] == 0xEF && p[1] == 0xB8 && (p[2] == 0x8E || p[2] == 0x8F)) {
                out += (char)p[0]; out += (char)p[1]; out += (char)p[2];
                p += 3;
            }
            if (*p && *p != ' ') out += ' ';
            continue;
        }
        out += (char)*p++;
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Voice presets (from tgsb_jni.cpp)                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t offset;
    double value;
    int isMultiplier; /* 1 = multiply, 0 = override */
} FrameOverride;

typedef struct {
    const char *name;
    const FrameOverride *overrides;
    int numOverrides;
    double voicedTiltDbPerOct;
    int hasVoicedTilt;
    double f4FreqScale;  /* 0 = use default 1.0 */
} VoicePreset;

#define OFF(field) offsetof(speechPlayer_frame_t, field)

static const FrameOverride kAdamOverrides[] = {
    { OFF(cb1), 1.3, 1 },
    { OFF(pa6), 1.3, 1 },
    { OFF(fricationAmplitude), 0.85, 1 },
};

static const FrameOverride kBenjaminOverrides[] = {
    { OFF(cf1), 1.01, 1 },
    { OFF(cf2), 1.02, 1 },
    { OFF(cf4), 3770.0, 0 },
    { OFF(cf5), 4100.0, 0 },
    { OFF(cf6), 5000.0, 0 },
    { OFF(cfNP), 0.9, 1 },
    { OFF(cb1), 1.3, 1 },
    { OFF(fricationAmplitude), 0.7, 1 },
    { OFF(pa6), 1.3, 1 },
};

static const FrameOverride kCalebOverrides[] = {
    { OFF(aspirationAmplitude), 1.0, 0 },
    { OFF(voiceAmplitude), 0.0, 0 },
};

static const FrameOverride kDavidOverrides[] = {
    { OFF(voicePitch), 0.75, 1 },
    { OFF(endVoicePitch), 0.75, 1 },
    { OFF(cf1), 0.90, 1 },
    { OFF(cf2), 0.93, 1 },
    { OFF(cf3), 0.95, 1 },
};

static const FrameOverride kRobertOverrides[] = {
    { OFF(voicePitch), 1.10, 1 },
    { OFF(endVoicePitch), 1.10, 1 },
    { OFF(cf1), 1.02, 1 }, { OFF(cf2), 1.06, 1 }, { OFF(cf3), 1.08, 1 },
    { OFF(cf4), 1.08, 1 }, { OFF(cf5), 1.10, 1 }, { OFF(cf6), 1.05, 1 },
    { OFF(cb1), 0.65, 1 }, { OFF(cb2), 0.68, 1 }, { OFF(cb3), 0.72, 1 },
    { OFF(cb4), 0.75, 1 }, { OFF(cb5), 0.78, 1 }, { OFF(cb6), 0.80, 1 },
    { OFF(glottalOpenQuotient), 0.30, 0 },
    { OFF(voiceTurbulenceAmplitude), 0.20, 1 },
    { OFF(fricationAmplitude), 0.75, 1 },
    { OFF(parallelBypass), 0.70, 1 },
    { OFF(pa3), 1.08, 1 }, { OFF(pa4), 1.15, 1 },
    { OFF(pa5), 1.20, 1 }, { OFF(pa6), 1.25, 1 },
    { OFF(pb1), 0.72, 1 }, { OFF(pb2), 0.75, 1 }, { OFF(pb3), 0.78, 1 },
    { OFF(pb4), 0.80, 1 }, { OFF(pb5), 0.82, 1 }, { OFF(pb6), 0.85, 1 },
    { OFF(pf3), 1.06, 1 }, { OFF(pf4), 1.08, 1 },
    { OFF(pf5), 1.10, 1 }, { OFF(pf6), 1.00, 1 },
};

#define PRESET(name, arr, tilt, hasTilt, f4s) \
    { name, arr, sizeof(arr)/sizeof(arr[0]), tilt, hasTilt, f4s }

static const VoicePreset kPresets[] = {
    PRESET("adam",     kAdamOverrides,     0.0,  0, 0),
    PRESET("benjamin", kBenjaminOverrides, 0.0,  0, 0),
    PRESET("caleb",    kCalebOverrides,    0.0,  0, 0),
    PRESET("david",    kDavidOverrides,    0.0,  0, 0),
    PRESET("robert",   kRobertOverrides,  -6.0,  1, 0),
};
static const int kNumPresets = sizeof(kPresets) / sizeof(kPresets[0]);

static void applyOverrides(speechPlayer_frame_t *f,
                           const FrameOverride *ov, int count) {
    for (int i = 0; i < count; i++) {
        double *field = (double *)((char *)f + ov[i].offset);
        if (ov[i].isMultiplier)
            *field *= ov[i].value;
        else
            *field = ov[i].value;
    }
}

/* ------------------------------------------------------------------ */
/* Engine struct                                                       */
/* ------------------------------------------------------------------ */

struct TgsbEngine {
    speechPlayer_handle_t player;
    nvspFrontend_handle_t frontend;
    int sampleRate;
    volatile int stopRequested;
    int voiceIndex;

    /* User VoicingTone overrides (applied on top of voice preset defaults) */
    int hasUserTone;
    double userVoicedTiltDbPerOct;
    double userNoiseGlottalModDepth;
    double userPitchSyncF1DeltaHz;
    double userPitchSyncB1DeltaHz;
    double userSpeedQuotient;
    double userAspirationTiltDbPerOct;
    double userCascadeBwScale;
    double userTremorDepth;
    double userChorusDepth;
    double userChorusDetuneHz;

    /* Pitch inflection (0..1), default 0.5 */
    double inflection;

    /* Pause mode: 0=off, 1=short, 2=long */
    int pauseMode;

    /* Desired eSpeak language for THIS engine.  eSpeak voice state is
     * process-global and a sibling engine may move it, so every
     * phonemization re-asserts this first (see espeakSetLanguageLocked). */
    char espeakLang[32];
};

/* Frame callback context */
typedef struct {
    TgsbEngine *engine;
    int frameCount;
} FrameCtx;

static void onFrame(
    void *userData,
    const nvspFrontend_Frame *frameOrNull,
    const nvspFrontend_FrameEx *frameExOrNull,
    double durationMs,
    double fadeMs,
    int userIndex
) {
    FrameCtx *ctx = (FrameCtx *)userData;
    if (!ctx || !ctx->engine || !ctx->engine->player) return;

    int sr = ctx->engine->sampleRate;
    unsigned int minSamples = durationMs > 0.0
        ? (unsigned int)(durationMs * sr / 1000.0 + 0.5) : 0;
    unsigned int fadeSamples = fadeMs > 0.0
        ? (unsigned int)(fadeMs * sr / 1000.0 + 0.5) : 0;

    if (frameOrNull) {
        speechPlayer_frame_t f;
        memcpy(&f, frameOrNull, sizeof(f));

        /* Apply voice preset overrides */
        const VoicePreset *vp = &kPresets[ctx->engine->voiceIndex];
        applyOverrides(&f, vp->overrides, vp->numOverrides);

        if (frameExOrNull) {
            speechPlayer_queueFrameEx(ctx->engine->player, &f,
                (const speechPlayer_frameEx_t *)frameExOrNull,
                (unsigned int)sizeof(nvspFrontend_FrameEx),
                minSamples, fadeSamples, userIndex, 0);
        } else {
            speechPlayer_queueFrame(ctx->engine->player, &f,
                minSamples, fadeSamples, userIndex, 0);
        }
    } else {
        speechPlayer_queueFrame(ctx->engine->player, NULL,
            minSamples, fadeSamples, userIndex, 0);
    }
    ctx->frameCount++;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

extern "C" {

TgsbEngine *tgsb_create(const char *espeakDataPath,
                         const char *packDir,
                         int sampleRate)
{
    /* Initialize eSpeak (refcounted — see espeakAcquire). */
    if (!espeakAcquire(espeakDataPath)) return NULL;

    /* Create TGSpeechBox components */
    speechPlayer_handle_t player = speechPlayer_initialize(sampleRate);
    nvspFrontend_handle_t fe = nvspFrontend_create(packDir);
    if (!fe) {
        speechPlayer_terminate(player);
        espeakRelease();
        return NULL;
    }
    nvspFrontend_setLanguage(fe, "en-us");

    /* Set default voicing tone */
    speechPlayer_voicingTone_t tone = speechPlayer_getDefaultVoicingTone();
    speechPlayer_setVoicingTone(player, &tone);

    /* Platform output gain: iOS/macOS AU path needs modest boost. */
    speechPlayer_setOutputGain(player, 1.7);

    TgsbEngine *engine = (TgsbEngine *)calloc(1, sizeof(TgsbEngine));
    engine->player = player;
    engine->frontend = fe;
    engine->sampleRate = sampleRate;
    engine->stopRequested = 0;
    engine->voiceIndex = 0; /* Adam */
    engine->inflection = 0.5;
    snprintf(engine->espeakLang, sizeof(engine->espeakLang), "en-us");

    return engine;
}

void tgsb_destroy(TgsbEngine *engine)
{
    if (!engine) return;
    if (engine->frontend) nvspFrontend_destroy(engine->frontend);
    if (engine->player) speechPlayer_terminate(engine->player);
    espeakRelease();
    free(engine);
}

void tgsb_set_override_directory(TgsbEngine *engine, const char *overrideDir)
{
    if (!engine || !engine->frontend) return;
    nvspFrontend_setOverrideDirectory(engine->frontend, overrideDir);
}

char *tgsb_export_data(TgsbEngine *engine, int domain,
                       const char *langTag, const char *overridesJson)
{
    if (!engine || !engine->frontend) return nullptr;
    return nvspFrontend_exportData(engine->frontend, domain,
                                   langTag, overridesJson);
}

int tgsb_set_language(TgsbEngine *engine,
                      const char *espeakLang,
                      const char *tgsbLang)
{
    if (!engine || !espeakLang || !tgsbLang) return 0;

    /* Record the desired language even if the set fails — queue_text
     * re-asserts it before every phonemization, so a transient failure
     * (or a sibling engine moving the global voice) self-heals. */
    snprintf(engine->espeakLang, sizeof(engine->espeakLang),
             "%s", espeakLang);
    int espeakOk = espeakSetLanguage(espeakLang);

    int packOk = nvspFrontend_setLanguage(engine->frontend, tgsbLang);
    return espeakOk && packOk;
}

int tgsb_set_voice(TgsbEngine *engine, const char *voiceName)
{
    if (!engine) return 0;

    for (int i = 0; i < kNumPresets; i++) {
        if (strcmp(kPresets[i].name, voiceName) == 0) {
            engine->voiceIndex = i;

            /* Clear any active voice profile when switching to a DSP preset */
            if (engine->frontend)
                nvspFrontend_setVoiceProfile(engine->frontend, "");

            speechPlayer_voicingTone_t tone =
                speechPlayer_getDefaultVoicingTone();
            if (kPresets[i].hasVoicedTilt)
                tone.voicedTiltDbPerOct = kPresets[i].voicedTiltDbPerOct;
            if (kPresets[i].f4FreqScale > 0.0)
                tone.f4FreqScale = kPresets[i].f4FreqScale;
            speechPlayer_setVoicingTone(engine->player, &tone);
            return 1;
        }
    }
    return 0;
}

int tgsb_set_voice_profile(TgsbEngine *engine, const char *profileName)
{
    if (!engine || !engine->frontend) return 0;
    return nvspFrontend_setVoiceProfile(engine->frontend, profileName);
}

char *tgsb_get_voice_profile_names(TgsbEngine *engine)
{
    if (!engine || !engine->frontend) return NULL;
    const char *names = nvspFrontend_getVoiceProfileNames(engine->frontend);
    if (!names || names[0] == '\0') return NULL;
    return strdup(names);
}

/*
 * Pre-split text at clause boundaries (. ? ! , ; :) then feed each
 * clause to eSpeak individually, tagging it with the correct clause
 * type for prosody.  This mirrors the NVDA driver and Android JNI,
 * which pre-split rather than relying on eSpeak's opaque clause chunking.
 *
 * Previous approach let eSpeak decide clause boundaries, which caused
 * issues with colon-separated text like "Unread: 1" — eSpeak could
 * split at the colon, and the trailing number leaked into the next
 * VoiceOver utterance (GitHub issue #40).
 */
void tgsb_begin_utterance(TgsbEngine *engine)
{
    if (!engine || !engine->player) return;
    engine->stopRequested = 0;
    /* Purge stale frames from previous utterance */
    speechPlayer_queueFrame(engine->player, NULL, 0, 0, -1, true);
}

void tgsb_queue_silence(TgsbEngine *engine, double ms)
{
    if (!engine || !engine->player || engine->stopRequested) return;
    if (ms <= 0.0) return;
    if (ms > 2000.0) ms = 2000.0;  /* matches the SSML break cap in the AU */
    unsigned int samples = (unsigned int)(
        ms * engine->sampleRate / 1000.0 + 0.5);
    unsigned int fadeSamp = (unsigned int)(
        3.0 * engine->sampleRate / 1000.0 + 0.5);
    speechPlayer_queueFrame(engine->player, NULL, samples, fadeSamp, -1, 0);
}

void tgsb_queue_text(TgsbEngine *engine,
                     const char *text,
                     double speed,
                     double pitch)
{
    tgsb_queue_text_ex(engine, text, speed, pitch, 1);
}

void tgsb_queue_text_ex(TgsbEngine *engine,
                        const char *text,
                        double speed,
                        double pitch,
                        int beginUtterance)
{
    if (!engine || !engine->player || !engine->frontend) return;
    if (!text || !*text) return;

    if (beginUtterance) {
        tgsb_begin_utterance(engine);
    } else if (engine->stopRequested) {
        /* A stop that lands between segments of one request must stick. */
        return;
    }

    /* Clamp parameters */
    if (speed < 0.1) speed = 0.1;

    /* Automatic speed split: cap synthesis at 2.0x, frame-advance the rest */
    const double kSynthCap = 2.0;
    double timeStretch = 1.0;
    if (speed > kSynthCap) {
        timeStretch = speed / kSynthCap;
        speed = kSynthCap;
    }
    speechPlayer_setTimeStretch(engine->player, timeStretch);

    if (pitch < 40.0) pitch = 40.0;
    if (pitch > 500.0) pitch = 500.0;

    FrameCtx ctx;
    ctx.engine = engine;
    ctx.frameCount = 0;

    const char *p = text;
    while (*p && !engine->stopRequested) {
        /* skip leading whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        /* scan forward to find next clause boundary */
        const char *clauseStart = p;
        char clauseType = '.';   /* default if no punctuation found */
        while (*p) {
            char c = *p;
            if (c == '?' || c == '!') {
                clauseType = c;
                p++;
                break;
            }
            /* comma/period between digits is a thousands separator or decimal
             * (e.g. "26,655" or "3.14"), not a clause boundary */
            if (c == ',' || c == '.') {
                bool prevDigit = (p > clauseStart) &&
                    (unsigned char)(*(p - 1) - '0') <= 9;
                bool nextDigit = *(p + 1) &&
                    (unsigned char)(*(p + 1) - '0') <= 9;
                if (prevDigit && nextDigit) {
                    p++;
                    continue;
                }
                /* Ordinal dot: "3. Mai" — digit + dot + space + letter
                 * (German, Swedish, Czech, Finnish ordinals) */
                if (c == '.' && prevDigit && *(p+1) == ' ' && *(p+2) &&
                    ((unsigned char)(*(p+2) - 'A') <= 25 ||
                     (unsigned char)(*(p+2) - 'a') <= 25)) {
                    p++;
                    continue;
                }
                clauseType = c;
                p++;
                break;
            }
            /* U+2026 ellipsis (UTF-8: E2 80 A6) — treat as period */
            if ((unsigned char)c == 0xE2 &&
                (unsigned char)*(p+1) == 0x80 &&
                (unsigned char)*(p+2) == 0xA6) {
                clauseType = '.';
                p += 3;
                break;
            }
            /* colon/semicolon only split when followed by whitespace
             * (avoids splitting times like "5:44" or ratios like "3:1") */
            if (c == ';' || c == ':') {
                char next = *(p + 1);
                if (next == ' ' || next == '\t' || next == '\r' ||
                    next == '\n' || next == '\0') {
                    clauseType = ',';
                    p++;
                    break;
                }
            }
            p++;
        }
        /* if we hit end-of-string without punctuation, p is at '\0' */

        /* copy clause into a NUL-terminated buffer */
        size_t len = (size_t)(p - clauseStart);
        if (len == 0) continue;

        char *clause = (char *)malloc(len + 1);
        if (!clause) continue;
        memcpy(clause, clauseStart, len);
        clause[len] = '\0';

        /* Pre-eSpeak text normalization: compound splitting, date ordinals, etc. */
        char *splitClause = nvspFrontend_prepareText(engine->frontend, clause);
        if (splitClause) {
            free(clause);
            clause = splitClause;
        }

        /* Pad emoji with spaces so eSpeak treats them as separate words */
        std::string padded = padEmojiWithSpaces(clause);
        if (padded.size() != strlen(clause)) {
            free(clause);
            clause = strdup(padded.c_str());
        }

        /* eSpeak → IPA for this clause.
         * Accumulate all IPA chunks into one string so the text parser
         * can align the full clause text against the full IPA output
         * (matches NVDA's one-call-per-clause pattern).
         * Re-assert this engine's language and hold the lock across the
         * whole clause so a sibling engine instance can't move the
         * global eSpeak voice mid-phonemization. */
        const void *ePtr = clause;
        std::string combinedIpa;
        {
            std::lock_guard<std::mutex> lock(g_espeakLock);
            espeakSetLanguageLocked(engine->espeakLang);
            while (ePtr && *(const char *)ePtr && !engine->stopRequested) {
                const char *ipa = espeak_TextToPhonemes(
                    &ePtr, espeakCHARS_UTF8, 0x02 /* IPA */);
                if (!ipa || !*ipa) continue;
                if (!combinedIpa.empty()) combinedIpa += ' ';
                combinedIpa += ipa;
            }
        }
        if (!combinedIpa.empty() && !engine->stopRequested) {
            char clauseStr[2] = { clauseType, 0 };
            nvspFrontend_queueIPA_ExWithText(
                engine->frontend, clause, combinedIpa.c_str(),
                speed, pitch, engine->inflection, clauseStr, 0,
                onFrame, &ctx
            );
        }

        /* Punctuation pause — matches NVDA driver durations.
         * Short: 35 ms sentence-final, 25 ms comma
         * Long:  60 ms sentence-final, 50 ms comma */
        if (engine->pauseMode > 0 && ctx.frameCount > 0) {
            double pauseMs = 0.0;
            if (clauseType == '.' || clauseType == '!' ||
                clauseType == '?') {
                pauseMs = engine->pauseMode == 2 ? 60.0 : 35.0;
            } else if (clauseType == ',') {
                pauseMs = engine->pauseMode == 2 ? 50.0 : 25.0;
            }
            if (pauseMs > 0.0) {
                unsigned int samples = (unsigned int)(
                    pauseMs * engine->sampleRate / 1000.0 + 0.5);
                unsigned int fadeSamp = (unsigned int)(
                    3.0 * engine->sampleRate / 1000.0 + 0.5);
                speechPlayer_queueFrame(engine->player, NULL,
                    samples, fadeSamp, -1, 0);
            }
        }

        free(clause);
    }
}

int tgsb_pull_audio(TgsbEngine *engine,
                    int16_t *outBuffer,
                    int maxSamples)
{
    if (!engine || !engine->player || engine->stopRequested) return 0;
    if (maxSamples <= 0) return 0;
    if (maxSamples > 4096) maxSamples = 4096;

    sample buf[4096];
    int n = speechPlayer_synthesize(engine->player,
                                    (unsigned int)maxSamples, buf);
    if (n <= 0) return 0;

    /* Base gain is inside the DSP (speechPlayer_setOutputGain).
     * Just copy samples — hard clip is already handled by the DSP limiter. */
    for (int i = 0; i < n; i++) {
        double s = buf[i].value;
        if (s > 32767.0) s = 32767.0;
        if (s < -32767.0) s = -32767.0;
        outBuffer[i] = (int16_t)s;
    }

    return n;
}

void tgsb_stop(TgsbEngine *engine)
{
    if (engine) engine->stopRequested = 1;
}

int tgsb_get_num_voices(void)
{
    return kNumPresets;
}

const char *tgsb_get_voice_name(int index)
{
    if (index < 0 || index >= kNumPresets) return NULL;
    return kPresets[index].name;
}

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
    double chorusDetuneHz)
{
    if (!engine || !engine->player) return;

    engine->hasUserTone = 1;
    engine->userVoicedTiltDbPerOct = voicedTiltDbPerOct;
    engine->userNoiseGlottalModDepth = noiseGlottalModDepth;
    engine->userPitchSyncF1DeltaHz = pitchSyncF1DeltaHz;
    engine->userPitchSyncB1DeltaHz = pitchSyncB1DeltaHz;
    engine->userSpeedQuotient = speedQuotient;
    engine->userAspirationTiltDbPerOct = aspirationTiltDbPerOct;
    engine->userCascadeBwScale = cascadeBwScale;
    engine->userTremorDepth = tremorDepth;
    engine->userChorusDepth = chorusDepth;
    engine->userChorusDetuneHz = chorusDetuneHz;

    speechPlayer_voicingTone_t tone = speechPlayer_getDefaultVoicingTone();
    const VoicePreset *vp = &kPresets[engine->voiceIndex];
    if (vp->hasVoicedTilt)
        tone.voicedTiltDbPerOct = vp->voicedTiltDbPerOct;

    tone.voicedTiltDbPerOct += voicedTiltDbPerOct;
    tone.noiseGlottalModDepth = noiseGlottalModDepth;
    tone.pitchSyncF1DeltaHz = pitchSyncF1DeltaHz;
    tone.pitchSyncB1DeltaHz = pitchSyncB1DeltaHz;
    tone.speedQuotient = speedQuotient;
    tone.aspirationTiltDbPerOct = aspirationTiltDbPerOct;
    tone.cascadeBwScale = cascadeBwScale;
    tone.tremorDepth = tremorDepth;
    tone.nasalBwScale = nasalBwScale;
    tone.f4FreqScale = f4FreqScale;
    tone.nasalGainScale = nasalGainScale;
    tone.chorusDepth = chorusDepth;
    tone.chorusDetuneHz = chorusDetuneHz;

    speechPlayer_setVoicingTone(engine->player, &tone);
}

void tgsb_set_frame_ex_defaults(TgsbEngine *engine,
    double creakiness, double breathiness,
    double jitter, double shimmer, double sharpness)
{
    if (!engine || !engine->frontend) return;
    nvspFrontend_setFrameExDefaults(
        engine->frontend,
        creakiness, breathiness, jitter, shimmer, sharpness);
}

int tgsb_set_pitch_mode(TgsbEngine *engine, const char *mode)
{
    if (!engine || !engine->frontend || !mode) return 0;
    return nvspFrontend_setPitchMode(engine->frontend, mode);
}

void tgsb_set_legacy_pitch_inflection_scale(TgsbEngine *engine, double scale)
{
    if (!engine || !engine->frontend) return;
    nvspFrontend_setLegacyPitchInflectionScale(engine->frontend, scale);
}

void tgsb_set_inflection(TgsbEngine *engine, double inflection)
{
    if (!engine) return;
    if (inflection < 0.0) inflection = 0.0;
    if (inflection > 1.0) inflection = 1.0;
    engine->inflection = inflection;
}

void tgsb_set_pause_mode(TgsbEngine *engine, int mode)
{
    if (!engine) return;
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    engine->pauseMode = mode;
}

void tgsb_set_sample_rate(TgsbEngine *engine, int sampleRate)
{
    if (!engine || sampleRate <= 0) return;
    if (sampleRate == engine->sampleRate) return;

    /* Reinitialize speechPlayer with new sample rate */
    if (engine->player) {
        speechPlayer_terminate(engine->player);
    }
    engine->player = speechPlayer_initialize(sampleRate);
    engine->sampleRate = sampleRate;
    speechPlayer_setOutputGain(engine->player, 1.7);

    /* Re-apply voicing tone settings */
    if (engine->hasUserTone) {
        speechPlayer_voicingTone_t tone = speechPlayer_getDefaultVoicingTone();
        const VoicePreset *vp = &kPresets[engine->voiceIndex];
        if (vp->hasVoicedTilt)
            tone.voicedTiltDbPerOct = vp->voicedTiltDbPerOct;

        tone.voicedTiltDbPerOct += engine->userVoicedTiltDbPerOct;
        tone.noiseGlottalModDepth = engine->userNoiseGlottalModDepth;
        tone.pitchSyncF1DeltaHz = engine->userPitchSyncF1DeltaHz;
        tone.pitchSyncB1DeltaHz = engine->userPitchSyncB1DeltaHz;
        tone.speedQuotient = engine->userSpeedQuotient;
        tone.aspirationTiltDbPerOct = engine->userAspirationTiltDbPerOct;
        tone.cascadeBwScale = engine->userCascadeBwScale;
        tone.tremorDepth = engine->userTremorDepth;
        tone.chorusDepth = engine->userChorusDepth;
        tone.chorusDetuneHz = engine->userChorusDetuneHz;

        speechPlayer_setVoicingTone(engine->player, &tone);
    } else {
        speechPlayer_voicingTone_t tone = speechPlayer_getDefaultVoicingTone();
        const VoicePreset *vp = &kPresets[engine->voiceIndex];
        if (vp->hasVoicedTilt)
            tone.voicedTiltDbPerOct = vp->voicedTiltDbPerOct;
        speechPlayer_setVoicingTone(engine->player, &tone);
    }
}

/* ------------------------------------------------------------------ */
/* Pack settings editor API                                           */
/* ------------------------------------------------------------------ */

int tgsb_apply_setting_overrides(TgsbEngine *engine, const char *yamlSnippet)
{
    if (!engine || !engine->frontend || !yamlSnippet) return 0;
    return nvspFrontend_applySettingOverrides(engine->frontend, yamlSnippet);
}

char *tgsb_get_available_languages(TgsbEngine *engine)
{
    if (!engine || !engine->frontend) return NULL;
    return nvspFrontend_getAvailableLanguages(engine->frontend);
}

void tgsb_free_string(char *str)
{
    nvspFrontend_freeString(str);
}

/* ------------------------------------------------------------------ */
/* Phoneme preview                                                     */
/* ------------------------------------------------------------------ */

int tgsb_preview_phoneme(TgsbEngine *engine, const char *phonemeKey,
                         double pitchHz, double durationMs)
{
    if (!engine || !engine->player || !engine->frontend) return 0;
    if (!phonemeKey || !*phonemeKey) return 0;

    engine->stopRequested = 0;

    /* Purge stale frames */
    speechPlayer_queueFrame(engine->player, NULL, 0, 0, -1, true);

    if (pitchHz < 40.0) pitchHz = 40.0;
    if (pitchHz > 500.0) pitchHz = 500.0;

    FrameCtx ctx;
    ctx.engine = engine;
    ctx.frameCount = 0;

    int ok = nvspFrontend_previewPhoneme(
        engine->frontend, phonemeKey,
        pitchHz, durationMs,
        onFrame, &ctx
    );
    return ok;
}

/* ------------------------------------------------------------------ */
/* Generic Data Query API (ABI v5+)                                   */
/* ------------------------------------------------------------------ */

int tgsb_get_data_count(TgsbEngine *engine, int domain, const char *langTag)
{
    if (!engine || !engine->frontend || !langTag) return -1;
    return nvspFrontend_getDataCount(engine->frontend, domain, langTag);
}

char *tgsb_query_data(TgsbEngine *engine, int domain, const char *langTag,
                      int offset, int limit)
{
    if (!engine || !engine->frontend || !langTag) return NULL;
    return nvspFrontend_queryData(engine->frontend, domain, langTag, offset, limit);
}

int tgsb_set_data(TgsbEngine *engine, int domain, const char *langTag,
                  const char *key, const char *value)
{
    if (!engine || !engine->frontend || !langTag || !key) return 0;
    return nvspFrontend_setData(engine->frontend, domain, langTag, key, value);
}

char *tgsb_text_to_ipa(TgsbEngine *engine, const char *text)
{
    if (!engine || !text || !*text) return strdup("");

    const void *ePtr = text;
    std::string ipa;
    {
        std::lock_guard<std::mutex> lock(g_espeakLock);
        espeakSetLanguageLocked(engine->espeakLang);
        while (ePtr && *(const char *)ePtr) {
            const char *chunk = espeak_TextToPhonemes(
                &ePtr, espeakCHARS_UTF8, 0x02 /* IPA */);
            if (!chunk || !*chunk) continue;
            if (!ipa.empty()) ipa += ' ';
            ipa += chunk;
        }
    }
    return strdup(ipa.c_str());
}

} /* extern "C" */

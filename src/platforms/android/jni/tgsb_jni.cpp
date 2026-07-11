/*
 * tgsb_jni.cpp — JNI bridge for Android TTS Service.
 *
 * Full pipeline: text → eSpeak IPA → nvspFrontend → speechPlayer → PCM
 *
 * License: GPL-3.0 (links eSpeak-ng GPL code with TGSpeechBox MIT code;
 *          combined binary is GPL-3.0 per copyleft rules)
 */

#include <jni.h>
#include <android/log.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <string>
#include <mutex>

#include <espeak-ng/speak_lib.h>
#include "speechPlayer.h"
#include "nvspFrontend.h"

#define TAG "TgsbJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* eSpeak lifecycle — refcounted process-global                        */
/*                                                                     */
/* eSpeak has process-global state, but multiple engines share it in   */
/* this process (TgsbTtsService, TgsbSpeakEngine, DebugNatives).       */
/* Unconditional espeak_Terminate() in nativeDestroy tore eSpeak down  */
/* under whichever engine was still alive.  Refcount instead: first    */
/* engine initializes, last one out terminates.                        */
/* ------------------------------------------------------------------ */

static int g_espeakRefCount = 0;
static std::mutex g_espeakLock;

static bool espeakAcquire(const char *dataPath) {
    std::lock_guard<std::mutex> lock(g_espeakLock);
    if (g_espeakRefCount == 0) {
        int sr = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0, dataPath, 0);
        if (sr <= 0) {
            LOGE("espeak_Initialize failed: %d", sr);
            return false;
        }
        LOGI("eSpeak initialized (sr=%d)", sr);
    }
    g_espeakRefCount++;
    return true;
}

static void espeakRelease() {
    std::lock_guard<std::mutex> lock(g_espeakLock);
    if (g_espeakRefCount > 0 && --g_espeakRefCount == 0) {
        espeak_Terminate();
        LOGI("eSpeak terminated (last engine destroyed)");
    }
}

/* ------------------------------------------------------------------ */
/* Emoji spacing — pad emoji codepoints with spaces so eSpeak treats  */
/* them as separate words for $textmode dictionary lookup.             */
/* ------------------------------------------------------------------ */

static bool isEmojiLeadByte(unsigned char b) {
    // 4-byte UTF-8 sequences starting with F0 9F cover U+1F000..U+1FFFF
    // which includes all major emoji blocks.
    // We also handle U+2600..U+27BF (Misc Symbols + Dingbats) as 3-byte.
    return (b == 0xF0);
}

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
            // Skip variation selectors (FE0E/FE0F) — 3-byte: EF B8 8E/8F
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
            // Skip variation selectors
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
/* Voice presets                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t offset;    /* offsetof the double field in speechPlayer_frame_t */
    double value;
    int isMultiplier; /* 1 = multiply, 0 = override */
} FrameOverride;

typedef struct {
    const char *name;
    const FrameOverride *overrides;
    int numOverrides;
    /* VoicingTone delta (applied on top of defaults) */
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
/* Engine instance                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    speechPlayer_handle_t player;
    nvspFrontend_handle_t frontend;
    int sampleRate;
    volatile int stopRequested;
    int voiceIndex;

    /* User VoicingTone overrides (deltas from voice preset defaults).
     * Applied on top of the preset in nativeApplyVoicingTone(). */
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

    /* Inflection (pitch range) — 0.0..1.0, default 0.5 */
    double inflection;

    /* Output volume — 0.0..1.0, multiplied into the gain stage */
    double volume;

    /* Pause mode: 0=off, 1=short, 2=long */
    int pauseMode;
} TgsbEngine;

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
/* Clause splitting + IPA synthesis                                    */
/* ------------------------------------------------------------------ */

/*
 * Pre-split text at clause boundaries (. ? ! , ; :) then feed each
 * clause to eSpeak individually, tagging it with the correct clause
 * type for prosody.  This mirrors the NVDA driver, which pre-splits
 * rather than relying on eSpeak's opaque clause chunking.
 */
static void synthesizeClauses(TgsbEngine *engine,
                              const char *text,
                              double speed, double basePitch,
                              nvspFrontend_FrameExCallback cb,
                              FrameCtx *ctx)
{
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
                /* Ordinal dot: "3. Mai" (German/Swedish/Czech/Finnish) */
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
         * (matches NVDA's one-call-per-clause pattern). */
        const void *ePtr = clause;
        std::string combinedIpa;
        while (ePtr && *(const char *)ePtr && !engine->stopRequested) {
            const char *ipa = espeak_TextToPhonemes(
                &ePtr, espeakCHARS_UTF8, 0x02 /* IPA */);
            if (!ipa || !*ipa) continue;
            if (!combinedIpa.empty()) combinedIpa += ' ';
            combinedIpa += ipa;
        }
        if (!combinedIpa.empty() && !engine->stopRequested) {
            char clauseStr[2] = { clauseType, 0 };
            nvspFrontend_queueIPA_ExWithText(
                engine->frontend, clause, combinedIpa.c_str(),
                speed, basePitch, engine->inflection, clauseStr, 0,
                cb, ctx
            );
        }

        /* Punctuation pause — matches NVDA driver durations.
         * Short: 35 ms sentence-final, 25 ms comma
         * Long:  60 ms sentence-final, 50 ms comma */
        if (engine->pauseMode > 0 && ctx->frameCount > 0) {
            double pauseMs = 0.0;
            if (clauseType == '.' || clauseType == '!' ||
                clauseType == '?' || clauseType == ':' || clauseType == ';') {
                pauseMs = engine->pauseMode == 2 ? 60.0 : 35.0;
            } else if (clauseType == ',') {
                pauseMs = engine->pauseMode == 2 ? 50.0 : 25.0;
            }
            if (pauseMs > 0.0) {
                unsigned int samples = (unsigned int)(
                    pauseMs * engine->sampleRate / 1000.0 + 0.5);
                unsigned int fadeSamp = (unsigned int)(
                    2.0 * engine->sampleRate / 1000.0 + 0.5);
                speechPlayer_queueFrame(engine->player, NULL,
                    samples, fadeSamp, 0, 0);
            }
        }

        free(clause);
    }
}

/* ------------------------------------------------------------------ */
/* JNI functions                                                       */
/* ------------------------------------------------------------------ */

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeCreate(
    JNIEnv *env, jobject thiz,
    jstring espeakDataPath, jstring packDirPath, jint sampleRate
) {
    const char *dataPath = env->GetStringUTFChars(espeakDataPath, NULL);
    const char *packDir  = env->GetStringUTFChars(packDirPath, NULL);

    LOGI("nativeCreate: espeakData=%s packDir=%s sr=%d", dataPath, packDir, sampleRate);

    /* Initialize eSpeak (refcounted — see espeakAcquire) */
    if (!espeakAcquire(dataPath)) {
        env->ReleaseStringUTFChars(espeakDataPath, dataPath);
        env->ReleaseStringUTFChars(packDirPath, packDir);
        return 0;
    }
    {
        espeak_VOICE voice_spec;
        memset(&voice_spec, 0, sizeof(voice_spec));
        voice_spec.languages = "en-us";
        espeak_SetVoiceByProperties(&voice_spec);
    }

    /* Create TGSpeechBox components */
    speechPlayer_handle_t player = speechPlayer_initialize(sampleRate);
    nvspFrontend_handle_t fe = nvspFrontend_create(packDir);
    if (!fe) {
        LOGE("nvspFrontend_create failed");
        speechPlayer_terminate(player);
        espeakRelease();
        env->ReleaseStringUTFChars(espeakDataPath, dataPath);
        env->ReleaseStringUTFChars(packDirPath, packDir);
        return 0;
    }
    int langOk = nvspFrontend_setLanguage(fe, "en-us");
    LOGI("nvspFrontend created, setLanguage('en-us')=%d", langOk);

    /* Set default voicing tone */
    speechPlayer_voicingTone_t tone = speechPlayer_getDefaultVoicingTone();
    speechPlayer_setVoicingTone(player, &tone);

    /* Platform output gain: Android TTS stream is attenuated by the system.
     * 1.7x keeps speech loud enough while leaving headroom so the limiter
     * doesn't engage on hot vowels like /æ/ (3.0x caused constant limiting →
     * buzzy/saturated quality, 2.0x still clipped /æ/ — issue #50). */
    speechPlayer_setOutputGain(player, 1.6);

    TgsbEngine *engine = (TgsbEngine *)calloc(1, sizeof(TgsbEngine));
    engine->player = player;
    engine->frontend = fe;
    engine->sampleRate = sampleRate;
    engine->stopRequested = 0;
    engine->voiceIndex = 0; /* Adam */
    engine->inflection = 0.5;
    engine->volume = 1.0;

    env->ReleaseStringUTFChars(espeakDataPath, dataPath);
    env->ReleaseStringUTFChars(packDirPath, packDir);

    LOGI("Engine ready (handle=%p)", (void *)engine);
    return (jlong)(intptr_t)engine;
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeDestroy(
    JNIEnv *env, jobject thiz, jlong handle
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine) return;

    if (engine->frontend) nvspFrontend_destroy(engine->frontend);
    if (engine->player) speechPlayer_terminate(engine->player);
    espeakRelease();
    free(engine);
    LOGI("Engine destroyed");
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetVoice(
    JNIEnv *env, jobject thiz, jlong handle, jstring voiceName
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine) return;

    const char *name = env->GetStringUTFChars(voiceName, NULL);
    for (int i = 0; i < kNumPresets; i++) {
        if (strcmp(kPresets[i].name, name) == 0) {
            engine->voiceIndex = i;

            /* Clear any active voice profile when switching to a DSP preset */
            if (engine->frontend)
                nvspFrontend_setVoiceProfile(engine->frontend, "");

            /* Apply VoicingTone changes for this preset */
            speechPlayer_voicingTone_t tone = speechPlayer_getDefaultVoicingTone();
            if (kPresets[i].hasVoicedTilt)
                tone.voicedTiltDbPerOct = kPresets[i].voicedTiltDbPerOct;
            if (kPresets[i].f4FreqScale > 0.0)
                tone.f4FreqScale = kPresets[i].f4FreqScale;
            speechPlayer_setVoicingTone(engine->player, &tone);

            LOGI("Voice set to: %s (index=%d)", name, i);
            break;
        }
    }
    env->ReleaseStringUTFChars(voiceName, name);
}

extern "C" JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetVoiceProfile(
    JNIEnv *env, jobject thiz, jlong handle, jstring profileName
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->frontend) return;

    const char *name = env->GetStringUTFChars(profileName, NULL);
    nvspFrontend_setVoiceProfile(engine->frontend, name);
    LOGI("Voice profile set to: %s", name);
    env->ReleaseStringUTFChars(profileName, name);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeGetVoiceProfileNames(
    JNIEnv *env, jobject thiz, jlong handle
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->frontend) return env->NewStringUTF("");

    const char *names = nvspFrontend_getVoiceProfileNames(engine->frontend);
    return env->NewStringUTF(names ? names : "");
}

/* Standalone engine wrappers for voice profiles */
extern "C" JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeSetVoiceProfile(
    JNIEnv *env, jobject thiz, jlong handle, jstring profileName
) {
    Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetVoiceProfile(
        env, thiz, handle, profileName);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeGetVoiceProfileNames(
    JNIEnv *env, jobject thiz, jlong handle
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativeGetVoiceProfileNames(
        env, thiz, handle);
}

/*
 * nativeQueueText — Phase 1: text → IPA → frames (fast, <10ms).
 * Queues frames into speechPlayer; audio can be pulled immediately after.
 */
JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeQueueText(
    JNIEnv *env, jobject thiz,
    jlong handle, jstring text, jint speechRate, jint pitch
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->player || !engine->frontend) return;

    engine->stopRequested = 0;

    /* Purge any stale frames from previous utterance so they don't
     * leak into the start of the new one on interruption. */
    speechPlayer_queueFrame(engine->player, NULL, 0, 0, -1, true);

    const char *textChars = env->GetStringUTFChars(text, NULL);
    if (!textChars || !*textChars) {
        if (textChars) env->ReleaseStringUTFChars(text, textChars);
        return;
    }

    /* Map Android rate/pitch to TGSpeechBox params */
    double speed = (double)speechRate / 100.0;
    if (speed < 0.1) speed = 0.1;

    /* Automatic speed split: cap synthesis at 2.0x, frame-advance the rest */
    const double kSynthCap = 2.0;
    double timeStretch = 1.0;
    if (speed > kSynthCap) {
        timeStretch = speed / kSynthCap;
        speed = kSynthCap;
    }
    speechPlayer_setTimeStretch(engine->player, timeStretch);

    double basePitch = 110.0 * ((double)pitch / 100.0);
    if (basePitch < 40.0) basePitch = 40.0;
    if (basePitch > 500.0) basePitch = 500.0;

    FrameCtx ctx;
    ctx.engine = engine;
    ctx.frameCount = 0;

    synthesizeClauses(engine, textChars, speed, basePitch, onFrame, &ctx);
    env->ReleaseStringUTFChars(text, textChars);
}

/*
 * nativePullAudio — Phase 2: pull PCM in small chunks (streaming).
 * Call in a loop until it returns 0. Each call fills outBuffer with
 * s16le PCM and returns bytes written.
 */
JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativePullAudio(
    JNIEnv *env, jobject thiz,
    jlong handle, jbyteArray outBuffer, jint maxBytes
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->player || engine->stopRequested) return 0;

    /* maxBytes → max samples (2 bytes per sample) */
    int maxSamples = maxBytes / 2;
    if (maxSamples <= 0) return 0;
    if (maxSamples > 4096) maxSamples = 4096;

    sample buf[4096];
    int n = speechPlayer_synthesize(engine->player,
                                    (unsigned int)maxSamples, buf);
    if (n <= 0) return 0;

    /* Convert sample structs to raw s16le bytes.
     * Base gain is now inside the DSP (speechPlayer_setOutputGain).
     * Only volume scaling (0.0–1.0) is applied here. */
    double vol = engine->volume;
    int16_t pcm[4096];
    for (int i = 0; i < n; i++) {
        double s = buf[i].value * vol;
        if (s > 32767.0) s = 32767.0;
        if (s < -32767.0) s = -32767.0;
        pcm[i] = (int16_t)s;
    }

    int byteLen = n * 2;
    env->SetByteArrayRegion(outBuffer, 0, byteLen, (jbyte *)pcm);
    return byteLen;
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeStop(
    JNIEnv *env, jobject thiz, jlong handle
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (engine) engine->stopRequested = 1;
}

JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetLanguage(
    JNIEnv *env, jobject thiz, jlong handle,
    jstring espeakLang, jstring tgsbLang
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine) return -1;

    const char *eLang = env->GetStringUTFChars(espeakLang, NULL);
    const char *tLang = env->GetStringUTFChars(tgsbLang, NULL);

    int result = 0;
    {
        espeak_VOICE voice_spec;
        memset(&voice_spec, 0, sizeof(voice_spec));
        voice_spec.languages = eLang;
        espeak_ERROR err = espeak_SetVoiceByProperties(&voice_spec);
        if (err != EE_OK) {
            LOGE("espeak_SetVoiceByProperties failed for '%s': error=%d",
                 eLang, (int)err);
            result = -1;
        }
    }
    int langOk = nvspFrontend_setLanguage(engine->frontend, tLang);
    if (!langOk) {
        LOGE("nvspFrontend_setLanguage failed for '%s'", tLang);
        result = -1;
    }

    LOGI("Language set: espeak=%s tgsb=%s (frontendOk=%d result=%d)",
         eLang, tLang, langOk, result);

    env->ReleaseStringUTFChars(espeakLang, eLang);
    env->ReleaseStringUTFChars(tgsbLang, tLang);
    return result;
}

/* ------------------------------------------------------------------ */
/* Voice quality settings (shared by Service + Standalone)              */
/* ------------------------------------------------------------------ */

static void applyVoicingTone(TgsbEngine *engine,
    double voicedTiltDbPerOct, double noiseGlottalModDepth,
    double pitchSyncF1DeltaHz, double pitchSyncB1DeltaHz,
    double speedQuotient, double aspirationTiltDbPerOct,
    double cascadeBwScale, double tremorDepth,
    double nasalBwScale = 1.0, double f4FreqScale = 1.0,
    double nasalGainScale = 1.0,
    double chorusDepth = 0.0, double chorusDetuneHz = 2.0)
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

static void applyFrameExDefaults(TgsbEngine *engine,
    double creakiness, double breathiness,
    double jitter, double shimmer, double sharpness)
{
    if (!engine || !engine->frontend) return;
    nvspFrontend_setFrameExDefaults(
        engine->frontend,
        creakiness, breathiness, jitter, shimmer, sharpness);
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetVoicingTone(
    JNIEnv *env, jobject thiz, jlong handle,
    jdouble a, jdouble b, jdouble c, jdouble d,
    jdouble e, jdouble f, jdouble g, jdouble h,
    jdouble nasalBw, jdouble f4Freq, jdouble nasalGain,
    jdouble chorusDepth, jdouble chorusDetuneHz
) {
    applyVoicingTone((TgsbEngine *)(intptr_t)handle, a,b,c,d,e,f,g,h, nasalBw, f4Freq, nasalGain, chorusDepth, chorusDetuneHz);
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetFrameExDefaults(
    JNIEnv *env, jobject thiz, jlong handle,
    jdouble a, jdouble b, jdouble c, jdouble d, jdouble e
) {
    applyFrameExDefaults((TgsbEngine *)(intptr_t)handle, a,b,c,d,e);
}

JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetPitchMode(
    JNIEnv *env, jobject thiz, jlong handle, jstring mode
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->frontend) return -1;
    const char *modeStr = env->GetStringUTFChars(mode, NULL);
    if (!modeStr) return -1;
    int ret = nvspFrontend_setPitchMode(engine->frontend, modeStr);
    env->ReleaseStringUTFChars(mode, modeStr);
    return ret;
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetInflectionScale(
    JNIEnv *env, jobject thiz, jlong handle, jdouble scale
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->frontend) return;
    nvspFrontend_setLegacyPitchInflectionScale(engine->frontend, scale);
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetInflection(
    JNIEnv *env, jobject thiz, jlong handle, jdouble value
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine) return;
    double v = value;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    engine->inflection = v;
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetSampleRate(
    JNIEnv *env, jobject thiz, jlong handle, jint sampleRate
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || sampleRate <= 0) return;
    if (sampleRate == engine->sampleRate) return;

    /* Reinitialize speechPlayer with new sample rate */
    if (engine->player) {
        speechPlayer_terminate(engine->player);
    }
    engine->player = speechPlayer_initialize(sampleRate);
    engine->sampleRate = sampleRate;
    speechPlayer_setOutputGain(engine->player, 1.6);

    /* Re-apply voicing tone settings */
    if (engine->hasUserTone) {
        applyVoicingTone(engine,
            engine->userVoicedTiltDbPerOct,
            engine->userNoiseGlottalModDepth,
            engine->userPitchSyncF1DeltaHz,
            engine->userPitchSyncB1DeltaHz,
            engine->userSpeedQuotient,
            engine->userAspirationTiltDbPerOct,
            engine->userCascadeBwScale,
            engine->userTremorDepth,
            1.0, 1.0, 1.0,
            engine->userChorusDepth,
            engine->userChorusDetuneHz);
    } else {
        speechPlayer_voicingTone_t tone = speechPlayer_getDefaultVoicingTone();
        const VoicePreset *vp = &kPresets[engine->voiceIndex];
        if (vp->hasVoicedTilt)
            tone.voicedTiltDbPerOct = vp->voicedTiltDbPerOct;
        speechPlayer_setVoicingTone(engine->player, &tone);
    }

    LOGI("Sample rate changed to %d", sampleRate);
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetVolume(
    JNIEnv *env, jobject thiz, jlong handle, jdouble value
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine) return;
    double v = value;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    engine->volume = v;
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetPauseMode(
    JNIEnv *env, jobject thiz, jlong handle, jint mode
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine) return;
    int m = (int)mode;
    if (m < 0) m = 0;
    if (m > 2) m = 2;
    engine->pauseMode = m;
}

/* ------------------------------------------------------------------ */
/* Standalone engine for MainActivity                                  */
/*                                                                     */
/* Separate engine instance so the consumer "Speak" UI doesn't fight   */
/* TalkBack for the TextToSpeechService audio path.  Same pipeline,    */
/* different JNI class prefix.                                         */
/* ------------------------------------------------------------------ */

JNIEXPORT jlong JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeCreate(
    JNIEnv *env, jobject thiz,
    jstring espeakDataPath, jstring packDirPath, jint sampleRate
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativeCreate(
        env, thiz, espeakDataPath, packDirPath, sampleRate);
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeDestroy(
    JNIEnv *env, jobject thiz, jlong handle
) {
    Java_com_tgspeechbox_tts_TgsbTtsService_nativeDestroy(env, thiz, handle);
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeSetVoice(
    JNIEnv *env, jobject thiz, jlong handle, jstring voiceName
) {
    Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetVoice(
        env, thiz, handle, voiceName);
}

JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeSetLanguage(
    JNIEnv *env, jobject thiz, jlong handle,
    jstring espeakLang, jstring tgsbLang
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetLanguage(
        env, thiz, handle, espeakLang, tgsbLang);
}

/*
 * nativeQueueTextDirect — same as nativeQueueText but takes speed as
 * a float multiplier (1.0 = normal) and pitch as Hz, matching the iOS
 * tgsb_queue_text() API.  No Android TTS 100-based int conversion.
 */
JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeQueueText(
    JNIEnv *env, jobject thiz,
    jlong handle, jstring text, jdouble speed, jdouble pitchHz
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->player || !engine->frontend) return;

    engine->stopRequested = 0;

    speechPlayer_queueFrame(engine->player, NULL, 0, 0, -1, true);

    const char *textChars = env->GetStringUTFChars(text, NULL);
    if (!textChars || !*textChars) {
        if (textChars) env->ReleaseStringUTFChars(text, textChars);
        return;
    }

    double sp = speed;
    if (sp < 0.1) sp = 0.1;

    // Automatic speed split: cap synthesis at 2.0x and put the excess
    // into DSP time-stretch (frame-advance).  Prevents formant mush
    // at extreme rates while maintaining clean output.
    const double kSynthCap = 2.0;
    double timeStretch = 1.0;
    if (sp > kSynthCap) {
        timeStretch = sp / kSynthCap;
        sp = kSynthCap;
    }
    speechPlayer_setTimeStretch(engine->player, timeStretch);

    double bp = pitchHz;
    if (bp < 40.0) bp = 40.0;
    if (bp > 500.0) bp = 500.0;

    FrameCtx ctx;
    ctx.engine = engine;
    ctx.frameCount = 0;

    synthesizeClauses(engine, textChars, sp, bp, onFrame, &ctx);
    env->ReleaseStringUTFChars(text, textChars);
    LOGI("SpeakEngine: queued %d frames (speed=%.2f ts=%.2f pitch=%.0f)",
         ctx.frameCount, sp, timeStretch, bp);
}

/*
 * nativeQueueIpa — Queue raw IPA for synthesis, skipping eSpeak.
 * Used for phoneme preview: play a single phoneme in isolation.
 * Uses nvspFrontend_previewPhoneme to bypass the full pipeline
 * (no allophone rules, no pitch contour — just raw DSP frame).
 */
JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeQueueIpa(
    JNIEnv *env, jobject thiz,
    jlong handle, jstring ipa, jdouble speed, jdouble pitchHz
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->player || !engine->frontend) return;

    engine->stopRequested = 0;

    speechPlayer_queueFrame(engine->player, NULL, 0, 0, -1, true);

    const char *ipaChars = env->GetStringUTFChars(ipa, NULL);
    if (!ipaChars || !*ipaChars) {
        if (ipaChars) env->ReleaseStringUTFChars(ipa, ipaChars);
        return;
    }

    double bp = pitchHz;
    if (bp < 40.0) bp = 40.0;
    if (bp > 500.0) bp = 500.0;

    FrameCtx ctx;
    ctx.engine = engine;
    ctx.frameCount = 0;

    int ok = nvspFrontend_previewPhoneme(
        engine->frontend, ipaChars,
        bp, 300.0,
        onFrame, &ctx
    );

    LOGI("SpeakEngine: previewPhoneme ok=%d frames=%d", ok, ctx.frameCount);
    env->ReleaseStringUTFChars(ipa, ipaChars);
}

JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativePullAudio(
    JNIEnv *env, jobject thiz,
    jlong handle, jshortArray outBuffer, jint maxSamples
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->player || engine->stopRequested) return 0;

    int count = maxSamples;
    if (count <= 0) return 0;
    if (count > 4096) count = 4096;

    sample buf[4096];
    int n = speechPlayer_synthesize(engine->player,
                                     (unsigned int)count, buf);
    if (n <= 0) return 0;

    /* Base gain is inside the DSP (speechPlayer_setOutputGain).
     * Only volume scaling here. */
    double vol = engine->volume;
    int16_t pcm[4096];
    for (int i = 0; i < n; i++) {
        double s = buf[i].value * vol;
        if (s > 32767.0) s = 32767.0;
        if (s < -32767.0) s = -32767.0;
        pcm[i] = (int16_t)s;
    }

    env->SetShortArrayRegion(outBuffer, 0, n, pcm);
    return n;
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeStop(
    JNIEnv *env, jobject thiz, jlong handle
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (engine) engine->stopRequested = 1;
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeSetVoicingTone(
    JNIEnv *env, jobject thiz, jlong handle,
    jdouble a, jdouble b, jdouble c, jdouble d,
    jdouble e, jdouble f, jdouble g, jdouble h,
    jdouble nasalBw, jdouble f4Freq, jdouble nasalGain,
    jdouble chorusDepth, jdouble chorusDetuneHz
) {
    applyVoicingTone((TgsbEngine *)(intptr_t)handle, a,b,c,d,e,f,g,h, nasalBw, f4Freq, nasalGain, chorusDepth, chorusDetuneHz);
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeSetFrameExDefaults(
    JNIEnv *env, jobject thiz, jlong handle,
    jdouble a, jdouble b, jdouble c, jdouble d, jdouble e
) {
    applyFrameExDefaults((TgsbEngine *)(intptr_t)handle, a,b,c,d,e);
}

JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeSetPitchMode(
    JNIEnv *env, jobject thiz, jlong handle, jstring mode
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetPitchMode(
        env, thiz, handle, mode);
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeSetInflectionScale(
    JNIEnv *env, jobject thiz, jlong handle, jdouble scale
) {
    Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetInflectionScale(
        env, thiz, handle, scale);
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeSetInflection(
    JNIEnv *env, jobject thiz, jlong handle, jdouble value
) {
    Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetInflection(
        env, thiz, handle, value);
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeSetVolume(
    JNIEnv *env, jobject thiz, jlong handle, jdouble value
) {
    Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetVolume(
        env, thiz, handle, value);
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeSetSampleRate(
    JNIEnv *env, jobject thiz, jlong handle, jint sampleRate
) {
    Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetSampleRate(
        env, thiz, handle, sampleRate);
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeSetPauseMode(
    JNIEnv *env, jobject thiz, jlong handle, jint mode
) {
    Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetPauseMode(
        env, thiz, handle, mode);
}

/* ------------------------------------------------------------------ */
/* Pack settings editor API                                           */
/* DEPRECATED: Use nativeSetData(DATA_SETTINGS, ...) per-key instead. */
/* Retained for backwards compatibility — all callers have migrated.  */
/* ------------------------------------------------------------------ */

JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeApplySettingOverrides(
    JNIEnv *env, jobject thiz, jlong handle, jstring yamlSnippet
) {
    (void)thiz;
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->frontend || !yamlSnippet) return 0;

    const char *snippet = env->GetStringUTFChars(yamlSnippet, NULL);
    if (!snippet) return 0;

    int ok = nvspFrontend_applySettingOverrides(engine->frontend, snippet);
    env->ReleaseStringUTFChars(yamlSnippet, snippet);
    return (jint)ok;
}

JNIEXPORT jstring JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeGetAvailableLanguages(
    JNIEnv *env, jobject thiz, jlong handle
) {
    (void)thiz;
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->frontend) return NULL;

    char *langs = nvspFrontend_getAvailableLanguages(engine->frontend);
    if (!langs) return NULL;

    jstring result = env->NewStringUTF(langs);
    nvspFrontend_freeString(langs);
    return result;
}

/* SpeakEngine delegates */


JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeApplySettingOverrides(
    JNIEnv *env, jobject thiz, jlong handle, jstring yamlSnippet
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativeApplySettingOverrides(
        env, thiz, handle, yamlSnippet);
}

JNIEXPORT jstring JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeGetAvailableLanguages(
    JNIEnv *env, jobject thiz, jlong handle
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativeGetAvailableLanguages(
        env, thiz, handle);
}

/* ------------------------------------------------------------------ */
/* Generic Data Query API (ABI v5+)                                   */
/* ------------------------------------------------------------------ */

JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeGetDataCount(
    JNIEnv *env, jobject thiz, jlong handle, jint domain, jstring langTag
) {
    (void)thiz;
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->frontend || !langTag) return -1;

    const char *tag = env->GetStringUTFChars(langTag, NULL);
    if (!tag) return -1;

    int count = nvspFrontend_getDataCount(engine->frontend, (int)domain, tag);
    env->ReleaseStringUTFChars(langTag, tag);
    return (jint)count;
}

JNIEXPORT jstring JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeQueryData(
    JNIEnv *env, jobject thiz, jlong handle,
    jint domain, jstring langTag, jint offset, jint limit
) {
    (void)thiz;
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->frontend || !langTag) return NULL;

    const char *tag = env->GetStringUTFChars(langTag, NULL);
    if (!tag) return NULL;

    char *json = nvspFrontend_queryData(engine->frontend, (int)domain, tag,
                                        (int)offset, (int)limit);
    env->ReleaseStringUTFChars(langTag, tag);
    if (!json) return NULL;

    jstring result = env->NewStringUTF(json);
    nvspFrontend_freeString(json);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetData(
    JNIEnv *env, jobject thiz, jlong handle,
    jint domain, jstring langTag, jstring key, jstring value
) {
    (void)thiz;
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->frontend || !langTag || !key) return 0;

    const char *tag = env->GetStringUTFChars(langTag, NULL);
    const char *k = env->GetStringUTFChars(key, NULL);
    const char *v = value ? env->GetStringUTFChars(value, NULL) : "";
    if (!tag || !k) {
        if (tag) env->ReleaseStringUTFChars(langTag, tag);
        if (k) env->ReleaseStringUTFChars(key, k);
        if (value && v) env->ReleaseStringUTFChars(value, v);
        return 0;
    }

    int ok = nvspFrontend_setData(engine->frontend, (int)domain, tag, k, v);

    env->ReleaseStringUTFChars(langTag, tag);
    env->ReleaseStringUTFChars(key, k);
    if (value && v) env->ReleaseStringUTFChars(value, v);
    return (jint)ok;
}

/* SpeakEngine delegates for data query API */

JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeGetDataCount(
    JNIEnv *env, jobject thiz, jlong handle, jint domain, jstring langTag
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativeGetDataCount(
        env, thiz, handle, domain, langTag);
}

JNIEXPORT jstring JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeQueryData(
    JNIEnv *env, jobject thiz, jlong handle,
    jint domain, jstring langTag, jint offset, jint limit
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativeQueryData(
        env, thiz, handle, domain, langTag, offset, limit);
}

JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeSetData(
    JNIEnv *env, jobject thiz, jlong handle,
    jint domain, jstring langTag, jstring key, jstring value
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetData(
        env, thiz, handle, domain, langTag, key, value);
}

JNIEXPORT jstring JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeExportData(
    JNIEnv *env, jobject thiz, jlong handle,
    jint domain, jstring langTag, jstring overridesJson
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->frontend) return nullptr;
    const char *lang = langTag ? env->GetStringUTFChars(langTag, nullptr) : nullptr;
    const char *json = overridesJson ? env->GetStringUTFChars(overridesJson, nullptr) : nullptr;
    char *result = nvspFrontend_exportData(engine->frontend, domain, lang, json);
    if (lang) env->ReleaseStringUTFChars(langTag, lang);
    if (json) env->ReleaseStringUTFChars(overridesJson, json);
    if (!result) return nullptr;
    jstring jResult = env->NewStringUTF(result);
    nvspFrontend_freeString(result);
    return jResult;
}

JNIEXPORT jstring JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeExportData(
    JNIEnv *env, jobject thiz, jlong handle,
    jint domain, jstring langTag, jstring overridesJson
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativeExportData(
        env, thiz, handle, domain, langTag, overridesJson);
}

/* ── Text-to-IPA via eSpeak ───────────────────────────────────────── */

JNIEXPORT jstring JNICALL
Java_com_tgspeechbox_tts_TgsbTtsService_nativeTextToIpa(
    JNIEnv *env, jobject thiz, jlong handle, jstring text
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !text) return env->NewStringUTF("");

    const char *textUtf8 = env->GetStringUTFChars(text, nullptr);
    if (!textUtf8 || !*textUtf8) {
        if (textUtf8) env->ReleaseStringUTFChars(text, textUtf8);
        return env->NewStringUTF("");
    }

    /* espeak_TextToPhonemes walks the text pointer forward. */
    const void *ePtr = textUtf8;
    std::string ipa;
    while (ePtr && *(const char *)ePtr) {
        const char *chunk = espeak_TextToPhonemes(
            &ePtr, espeakCHARS_UTF8, 0x02 /* IPA */);
        if (!chunk || !*chunk) continue;
        if (!ipa.empty()) ipa += ' ';
        ipa += chunk;
    }

    env->ReleaseStringUTFChars(text, textUtf8);
    return env->NewStringUTF(ipa.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_tgspeechbox_tts_TgsbSpeakEngine_nativeTextToIpa(
    JNIEnv *env, jobject thiz, jlong handle, jstring text
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativeTextToIpa(
        env, thiz, handle, text);
}

/* ------------------------------------------------------------------ */
/* DebugNatives — instrumentation-only surface (issue #100 forensics)  */
/* ------------------------------------------------------------------ */

/*
 * Full-pipeline render from raw IPA, selecting the production path
 * (queueIPA_ExWithText, withText=true) or the desktop-harness path
 * (queueIPA_Ex, withText=false) so the two can be A/B'd on-device
 * with identical inputs. Unlike nativeQueueIpa this runs the whole
 * pass pipeline (allophones, dialect replacements, prosody).
 */
JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_DebugNatives_nativeDebugQueueIpaEx(
    JNIEnv *env, jobject thiz,
    jlong handle, jstring text, jstring ipa,
    jdouble speed, jdouble basePitch, jboolean withText
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->player || !engine->frontend) return -1;

    engine->stopRequested = 0;
    speechPlayer_queueFrame(engine->player, NULL, 0, 0, -1, true);

    const char *ipaChars = env->GetStringUTFChars(ipa, NULL);
    if (!ipaChars || !*ipaChars) {
        if (ipaChars) env->ReleaseStringUTFChars(ipa, ipaChars);
        return -1;
    }
    const char *textChars = text ? env->GetStringUTFChars(text, NULL) : NULL;

    FrameCtx ctx;
    ctx.engine = engine;
    ctx.frameCount = 0;

    int ok;
    if (withText) {
        ok = nvspFrontend_queueIPA_ExWithText(
            engine->frontend, textChars ? textChars : "", ipaChars,
            speed, basePitch, engine->inflection, ".", 0, onFrame, &ctx);
    } else {
        ok = nvspFrontend_queueIPA_Ex(
            engine->frontend, ipaChars,
            speed, basePitch, engine->inflection, ".", 0, onFrame, &ctx);
    }

    env->ReleaseStringUTFChars(ipa, ipaChars);
    if (textChars) env->ReleaseStringUTFChars(text, textChars);
    return ok ? ctx.frameCount : -1;
}

JNIEXPORT jlong JNICALL
Java_com_tgspeechbox_tts_DebugNatives_nativeCreate(
    JNIEnv *env, jobject thiz,
    jstring espeakDataPath, jstring packDirPath, jint sampleRate
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativeCreate(
        env, thiz, espeakDataPath, packDirPath, sampleRate);
}

JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_DebugNatives_nativeSetLanguage(
    JNIEnv *env, jobject thiz, jlong handle,
    jstring espeakLang, jstring tgsbLang
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativeSetLanguage(
        env, thiz, handle, espeakLang, tgsbLang);
}

JNIEXPORT jstring JNICALL
Java_com_tgspeechbox_tts_DebugNatives_nativeTextToIpa(
    JNIEnv *env, jobject thiz, jlong handle, jstring text
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativeTextToIpa(
        env, thiz, handle, text);
}

JNIEXPORT jint JNICALL
Java_com_tgspeechbox_tts_DebugNatives_nativePullAudio(
    JNIEnv *env, jobject thiz,
    jlong handle, jbyteArray outBuffer, jint maxBytes
) {
    return Java_com_tgspeechbox_tts_TgsbTtsService_nativePullAudio(
        env, thiz, handle, outBuffer, maxBytes);
}

JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_DebugNatives_nativeDestroy(
    JNIEnv *env, jobject thiz, jlong handle
) {
    Java_com_tgspeechbox_tts_TgsbTtsService_nativeDestroy(env, thiz, handle);
}

/*
 * nativeDebugQueueText — nativeQueueText replica with per-step skip flags,
 * for bisecting the cross-utterance fricative latch (issue #100 forensics).
 *   bit0 (1): skip nvspFrontend_prepareText
 *   bit1 (2): skip padEmojiWithSpaces
 *   bit2 (4): skip speechPlayer_setTimeStretch
 *   bit3 (8): skip the leading purge frame
 *   bit4 (16): skip the internal espeak call; use fixedIpa from the caller
 * Logs the prepared clause and assembled IPA per call so drift is visible
 * in logcat (tag TgsbJNI, "dbgQT").
 */
JNIEXPORT void JNICALL
Java_com_tgspeechbox_tts_DebugNatives_nativeDebugQueueText(
    JNIEnv *env, jobject thiz,
    jlong handle, jstring text, jdouble speed, jdouble pitchHz, jint flags,
    jstring fixedIpa
) {
    TgsbEngine *engine = (TgsbEngine *)(intptr_t)handle;
    if (!engine || !engine->player || !engine->frontend) return;

    engine->stopRequested = 0;
    if (!(flags & 8))
        speechPlayer_queueFrame(engine->player, NULL, 0, 0, -1, true);

    const char *textChars = env->GetStringUTFChars(text, NULL);
    if (!textChars || !*textChars) {
        if (textChars) env->ReleaseStringUTFChars(text, textChars);
        return;
    }

    double sp = speed;
    if (sp < 0.1) sp = 0.1;
    if (!(flags & 4))
        speechPlayer_setTimeStretch(engine->player, 1.0);

    double bp = pitchHz;
    if (bp < 40.0) bp = 40.0;
    if (bp > 500.0) bp = 500.0;

    char *clause = strdup(textChars);
    if (!(flags & 1)) {
        char *prepped = nvspFrontend_prepareText(engine->frontend, clause);
        if (prepped) {
            free(clause);
            clause = prepped;
        }
    }
    if (!(flags & 2)) {
        std::string padded = padEmojiWithSpaces(clause);
        if (padded.size() != strlen(clause)) {
            free(clause);
            clause = strdup(padded.c_str());
        }
    }

    std::string combinedIpa;
    if ((flags & 16) && fixedIpa) {
        const char *fx = env->GetStringUTFChars(fixedIpa, NULL);
        if (fx) {
            combinedIpa = fx;
            env->ReleaseStringUTFChars(fixedIpa, fx);
        }
    } else {
        const void *ePtr = clause;
        while (ePtr && *(const char *)ePtr && !engine->stopRequested) {
            const char *ipa = espeak_TextToPhonemes(
                &ePtr, espeakCHARS_UTF8, 0x02 /* IPA */);
            if (!ipa || !*ipa) continue;
            if (!combinedIpa.empty()) combinedIpa += ' ';
            combinedIpa += ipa;
        }
    }

    LOGI("dbgQT flags=%d clause=[%s] ipa=[%s]", (int)flags, clause,
         combinedIpa.c_str());

    if (!combinedIpa.empty()) {
        FrameCtx ctx;
        ctx.engine = engine;
        ctx.frameCount = 0;
        nvspFrontend_queueIPA_ExWithText(
            engine->frontend, clause, combinedIpa.c_str(),
            sp, bp, engine->inflection, ".", 0, onFrame, &ctx);
        LOGI("dbgQT queued %d frames", ctx.frameCount);
    }

    free(clause);
    env->ReleaseStringUTFChars(text, textChars);
}

} /* extern "C" */

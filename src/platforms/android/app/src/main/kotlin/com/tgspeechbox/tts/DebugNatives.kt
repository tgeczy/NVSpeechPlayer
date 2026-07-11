package com.tgspeechbox.tts

/**
 * Instrumentation-only native surface (issue #100 stress-initial /s/
 * forensics). Lets a test drive a fresh engine through the exact same
 * JNI layer as production, but with control over the entry point:
 * direct raw-IPA (queueIPA_Ex, the desktop-harness path) vs
 * text+IPA (queueIPA_ExWithText, the production path).
 *
 * Not referenced by any production code path.
 */
object DebugNatives {
    init {
        System.loadLibrary("tgspeechbox_jni")
    }

    external fun nativeCreate(
        espeakDataPath: String, packDirPath: String, sampleRate: Int
    ): Long

    external fun nativeSetLanguage(
        handle: Long, espeakLang: String, tgsbLang: String
    ): Int

    external fun nativeTextToIpa(handle: Long, text: String): String

    /** Returns frame count, or -1 on failure. */
    external fun nativeDebugQueueIpaEx(
        handle: Long, text: String?, ipa: String,
        speed: Double, basePitch: Double, withText: Boolean
    ): Int

    external fun nativePullAudio(
        handle: Long, outBuffer: ByteArray, maxBytes: Int
    ): Int

    /**
     * nativeQueueText replica with per-step skip flags (latch bisection):
     * bit0 skip prepareText, bit1 skip padEmoji, bit2 skip setTimeStretch,
     * bit3 skip purge frame.
     */
    external fun nativeDebugQueueText(
        handle: Long, text: String, speed: Double, pitchHz: Double, flags: Int,
        fixedIpa: String?
    )

    external fun nativeDestroy(handle: Long)
}

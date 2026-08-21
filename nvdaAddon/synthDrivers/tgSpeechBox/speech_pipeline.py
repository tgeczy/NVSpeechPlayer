# -*- coding: utf-8 -*-
"""Speech synthesis pipeline — mixin for SynthDriver.

Provides:
- _buildBlocks(speechSequence, coalesceSayAll)
- speak(speechSequence)
- _speakBg(speakList, generation) — including the nested _onFrame callback
- _notifyIndexesAndDone(indexes, generation)
"""

from __future__ import annotations

import ctypes
import os

from logHandler import log
from synthDriverHandler import synthDoneSpeaking, synthIndexReached
from speech.commands import IndexCommand, PitchCommand

from . import speechPlayer, espeak_direct
from .constants import COALESCE_MAX_CHARS, COALESCE_MAX_INDEXES
from .text_utils import re_textPause, normalizeTextForEspeak, looksLikeSentenceEnd
from .espeak_bridge import espeakTextToIPA, espeakTextToIPA_scriptAware
from .profile_utils import buildVoiceOps, applyVoiceToFrame
from .constants import voices

# Pre-calculate per-voice operations for fast application.
_frameFieldNames = {x[0] for x in speechPlayer.Frame._fields_}
_voiceOps = buildVoiceOps(voices, _frameFieldNames)
del _frameFieldNames


# ---------------------------------------------------------------------------
# Letter-name dictionaries — per-language TSV files mapping single characters
# to their spoken names (e.g. Spanish ó → "o acentuada", y → "i griega").
# Used only for single-character chunks (character-by-character navigation).
# ---------------------------------------------------------------------------
_letterNameCache: dict[str, dict[str, str]] = {}  # langTag -> {char -> spokenName}


def _loadLetterNames(packsDir: str, langTag: str) -> dict[str, str]:
    """Load letter names TSV for a language.  Returns empty dict if not found."""
    if langTag in _letterNameCache:
        return _letterNameCache[langTag]

    result: dict[str, str] = {}
    # Try exact tag first (es-mx), then base (es)
    candidates = [langTag]
    if "-" in langTag:
        candidates.append(langTag.split("-")[0])

    for tag in candidates:
        path = os.path.join(packsDir, "dict", f"{tag}-letters.tsv")
        if os.path.isfile(path):
            try:
                with open(path, "r", encoding="utf-8") as f:
                    for line in f:
                        line = line.strip()
                        if not line or line.startswith("#"):
                            continue
                        parts = line.split("\t", 1)
                        if len(parts) == 2 and parts[0] and parts[1]:
                            result[parts[0]] = parts[1]
                log.debug("TGSpeechBox: loaded %d letter names from %s", len(result), path)
            except Exception:
                log.debug("TGSpeechBox: failed to load letter names from %s", path, exc_info=True)
            break  # found a file, stop looking

    _letterNameCache[langTag] = result
    return result


def _applyVoiceToFrame(frame: speechPlayer.Frame, voiceName: str) -> None:
    applyVoiceToFrame(frame, voiceName, _voiceOps)


class SpeechPipelineMixin:
    """Speech synthesis pipeline: text -> IPA -> frames -> audio."""

    def _notifyIndexesAndDone(self, indexes, generation):
        # cancel() may have invalidated this generation while it was
        # queued in BgThread — don't fire a spurious synthDoneSpeaking.
        if generation != self._speakGen:
            return
        for i in indexes:
            synthIndexReached.notify(synth=self, index=i)
        synthDoneSpeaking.notify(synth=self)

    # ---- eSpeak thin wrappers (use instance state) ----

    def _espeakTextToIPA(self, text: str) -> str:
        if not espeak_direct.is_ready():
            log.debug("TGSpeechBox: espeak not ready, skipping IPA conversion")
            return ""
        dll = espeak_direct.get_dll()
        if not dll:
            return ""
        try:
            return espeakTextToIPA(text, dll, self._ESPEAK_PHONEME_MODE)
        except OSError as e:
            # Access violation or other OS error — disable further attempts.
            log.error(f"TGSpeechBox: espeak_TextToPhonemes failed: {e}")
            return ""

    def _espeakTextToIPA_scriptAware(self, text: str) -> str:
        if not text:
            return ""
        if not espeak_direct.is_ready():
            return ""
        dll = espeak_direct.get_dll()
        if not dll:
            return ""
        latinFallback = getattr(self, "_latinFallbackLang", "en-gb")
        espeakLang = getattr(self, "_espeakLang", "en")
        try:
            return espeakTextToIPA_scriptAware(
                text, dll, self._ESPEAK_PHONEME_MODE,
                espeakLang, latinFallback,
            )
        except OSError as e:
            log.error(f"TGSpeechBox: espeak_TextToPhonemes failed: {e}")
            return ""

    # ---- Block building ----

    def _buildBlocks(self, speechSequence, coalesceSayAll: bool = False):
        """Convert an NVDA speechSequence into blocks: (text, [indexesAfterText], pitchOffset).

        When coalesceSayAll is True, we *delay* IndexCommands that occur mid-sentence.
        This prevents audible gaps when NVDA inserts index markers at visual line wraps
        during Say All.
        """
        blocks = []  # list[tuple[str, list[int], int]]
        textBuf = []
        pendingIndexes = []
        seenNonEmptyText = False

        pitchOffset = 0
        bufPitchOffset = pitchOffset

        def flush():
            nonlocal seenNonEmptyText, bufPitchOffset
            raw = normalizeTextForEspeak(" ".join(textBuf))
            textBuf.clear()
            blocks.append((raw, pendingIndexes.copy(), bufPitchOffset))
            pendingIndexes.clear()
            seenNonEmptyText = False
            bufPitchOffset = pitchOffset

        for item in speechSequence:
            # Treat pitch changes as hard boundaries.
            if PitchCommand and isinstance(item, PitchCommand):
                if textBuf or pendingIndexes:
                    flush()
                pitchOffset = getattr(item, "offset", 0) or 0
                bufPitchOffset = pitchOffset
                continue

            if isinstance(item, str):
                if item:
                    if not textBuf and not pendingIndexes:
                        bufPitchOffset = pitchOffset
                    textBuf.append(item)
                    if item.strip():
                        seenNonEmptyText = True
                continue

            if IndexCommand and isinstance(item, IndexCommand):
                # Leading indexes (no text yet) should fire immediately.
                if not seenNonEmptyText and not textBuf:
                    blocks.append(("", [item.index], pitchOffset))
                    continue

                pendingIndexes.append(item.index)

                if not coalesceSayAll:
                    flush()
                    continue

                # Coalesce across wrapped lines: flush only at a "real" boundary
                # (sentence end) or if the buffer becomes too large.
                safeSoFar = normalizeTextForEspeak(" ".join(textBuf))
                if (
                    looksLikeSentenceEnd(safeSoFar)
                    or len(safeSoFar) >= COALESCE_MAX_CHARS
                    or len(pendingIndexes) >= COALESCE_MAX_INDEXES
                ):
                    flush()
                continue

            # Ignore other command types.

        # Trailing text (and/or delayed indexes)
        if textBuf or pendingIndexes:
            flush()

        # Remove trailing empty blocks with no indexes
        while blocks and (not blocks[-1][0]) and (not blocks[-1][1]):
            blocks.pop()

        return blocks

    # ---- speak() entry point ----

    def speak(self, speechSequence):
        indexes = []
        anyText = False
        for item in speechSequence:
            if IndexCommand and isinstance(item, IndexCommand):
                indexes.append(item.index)
            elif isinstance(item, str) and item.strip():
                anyText = True

        if (not anyText):
            self._enqueue(self._notifyIndexesAndDone, indexes, self._speakGen)
            return

        # Stamp this speak with the current generation so cancel() can invalidate it.
        # IMPORTANT: Do NOT increment here — only cancel() bumps the counter.
        # If speak() bumped it, a benign second speak() (e.g. from synthDoneSpeaking)
        # would kill an in-flight _speakBg that was never cancelled.
        self._enqueue(self._speakBg, list(speechSequence), self._speakGen)

    # ---- Background speak implementation ----

    def _speakBg(self, speakList, generation):
        # Bail immediately if a cancel() already invalidated this generation
        if generation != self._speakGen:
            return
        hadRealSpeech = False
        hadKickedAudio = False  # streaming: kick AudioThread after first chunk
        hasIndex = bool(IndexCommand) and any(isinstance(i, IndexCommand) for i in speakList)
        blocks = self._buildBlocks(speakList, coalesceSayAll=hasIndex)

        endPause = 0.0
        leadingSilenceMs = 1.0
        minFadeInMs = 3.0
        minFadeOutMs = 3.0
        lastStreamWasVoiced = False
        pauseMode = str(getattr(self, "_pauseMode", "short") or "short").strip().lower()

        def _punctuationPauseMs(punctToken: str | None) -> float:
            """Return pause duration in ms for punctuation.

            Short mode: subtle pauses for natural flow
            Long mode: deliberate pauses for clarity
            """
            if not punctToken or pauseMode == "off":
                return 0.0
            if punctToken in (".", "!", "?", "...", ":", ";"):
                return 60.0 if pauseMode == "long" else 35.0
            if punctToken == ",":
                return 50.0 if pauseMode == "long" else 25.0
            return 0.0

        for (text, indexesAfter, blockPitchOffset) in blocks:
            # Bail if cancel() invalidated this generation
            if generation != self._speakGen:
                return

            # Speak text for this block.
            if text:
                for chunk in re_textPause.split(text):
                    # Check again between chunks for fast cancellation
                    if generation != self._speakGen:
                        return

                    if not chunk:
                        continue

                    chunk = normalizeTextForEspeak(chunk)
                    if not chunk:
                        continue

                    # Determine punctuation at the *end* of the chunk.
                    # This influences two things:
                    # - clauseType passed to the frontend (intonation hints)
                    # - optional micro-pause insertion after the chunk
                    punctToken = None
                    s = chunk.rstrip()
                    # Strip trailing closing quotes/brackets so ." and ?"
                    # expose the actual punctuation mark for clause detection.
                    s_stripped = s.rstrip(')\]"\u2019\u201D\'')
                    if s_stripped.endswith("..."):
                        punctToken = "..."
                        # Frontend only reads 1 byte; treat ellipsis as '.' for prosody.
                        clauseType = "."
                    elif s_stripped and (s_stripped[-1] in ".?!,:;"):
                        punctToken = s_stripped[-1]
                        clauseType = s_stripped[-1]
                    else:
                        clauseType = None

                    punctPauseMs = _punctuationPauseMs(punctToken)

                    # Single-character letter name lookup: if the chunk is
                    # a lone character and we have a letter-name override for
                    # this language, replace it with the spoken name before
                    # eSpeak phonemization.  e.g. Spanish ó → "o acentuada".
                    stripped = chunk.strip()
                    if len(stripped) <= 3:
                        log.debug("TGSpeechBox: short chunk=%r len=%d lang=%r packs=%r",
                                  stripped, len(stripped),
                                  getattr(self, "_frontendLangTag", "?"),
                                  getattr(self, "_packsDir", "?"))
                    if len(stripped) == 1:
                        langTag = getattr(self, "_frontendLangTag", "") or ""
                        packsDir = getattr(self, "_packsDir", "") or ""
                        if packsDir and langTag:
                            letterNames = _loadLetterNames(packsDir, langTag)
                            replacement = letterNames.get(stripped) or letterNames.get(stripped.lower())
                            log.debug("TGSpeechBox: letter lookup char=%r found=%r (dict has %d entries)",
                                      stripped, replacement, len(letterNames))
                            if replacement:
                                chunk = replacement

                    # Pre-eSpeak text normalization: compound splitting, date ordinals, etc.
                    chunk = self._frontend.prepareText(chunk)

                    ipaText = self._espeakTextToIPA_scriptAware(chunk)
                    if not ipaText:
                        # Nothing speakable, but don't drop indexes (they are queued after the block).
                        continue

                    pitch = float(self._curPitch) + float(blockPitchOffset)
                    basePitch = 25.0 + (21.25 * (pitch / 12.5))

                    # nvspFrontend.dll: IPA -> frames.
                    queuedCount = 0

                    # Some generators (including nvspFrontend) may emit an initial silence
                    # frame for each queued utterance. When NVDA feeds us multiple chunks
                    # back-to-back (e.g. Say All reading "visual" lines), that redundant
                    # leading silence can become a perceptible pause. Once we've already
                    # queued real speech for this speak operation (or we're appending to
                    # existing output), suppress those leading silence frames.
                    suppressLeadingSilence = hadRealSpeech or bool(getattr(self._audio, "isSpeaking", False))
                    sawRealFrameInThisUtterance = False
                    sawSilenceAfterVoice = False

                    # Pre-calculate extra parameter multipliers once per utterance.
                    # This avoids repeated getattr(self, f"speechPlayer_{p}") lookups on every frame.
                    extraParamMultipliers = ()
                    if self.exposeExtraParams:
                        try:
                            names = getattr(self, "_extraParamNames", ()) or ()
                            attrNames = getattr(self, "_extraParamAttrNames", None)
                            if not attrNames or len(attrNames) != len(names):
                                attrNames = [f"speechPlayer_{x}" for x in names]

                            pairs = []
                            for paramName, attrName in zip(names, attrNames):
                                try:
                                    ratio = float(getattr(self, attrName, 50)) / 50.0
                                except Exception:
                                    continue
                                # Skip default (ratio=1.0) to keep the per-frame hot path tiny.
                                if ratio != 1.0:
                                    pairs.append((paramName, ratio))

                            extraParamMultipliers = tuple(pairs)
                        except Exception:
                            extraParamMultipliers = ()

                    def _onFrame(framePtr, frameExPtr, frameDuration, fadeDuration, idxToSet):
                        nonlocal queuedCount, hadRealSpeech, sawRealFrameInThisUtterance, sawSilenceAfterVoice, lastStreamWasVoiced

                        # Bail immediately if cancel() invalidated this generation mid-word
                        if generation != self._speakGen:
                            return

                        # Reduce leading silence frames to minimize gaps
                        if (not framePtr) and suppressLeadingSilence and (not sawRealFrameInThisUtterance):
                            dur = min(float(frameDuration), float(leadingSilenceMs))
                            if dur > 0:
                                self._player.queueFrame(None, dur, min(float(fadeDuration), dur), userIndex=idxToSet)
                                queuedCount += 1
                                lastStreamWasVoiced = False
                            return

                        # Silence frame: queue as pause
                        if not framePtr:
                            if sawRealFrameInThisUtterance and (not sawSilenceAfterVoice):
                                fd = max(float(fadeDuration), float(minFadeOutMs))
                                if float(frameDuration) > 0:
                                    fd = min(fd, float(frameDuration))
                                else:
                                    fd = 0.0
                                fadeDuration = fd
                                sawSilenceAfterVoice = True
                            self._player.queueFrame(None, frameDuration, fadeDuration, userIndex=idxToSet)
                            queuedCount += 1
                            lastStreamWasVoiced = False
                            return

                        # Ensure fade-in on first voiced frame
                        if not sawRealFrameInThisUtterance:
                            fadeDuration = max(float(fadeDuration), float(minFadeInMs))

                        sawRealFrameInThisUtterance = True
                        hadRealSpeech = True

                        # Copy C frame to Python-owned Frame
                        frame = speechPlayer.Frame()
                        ctypes.memmove(ctypes.byref(frame), framePtr, ctypes.sizeof(speechPlayer.Frame))

                        # Only apply Python voice preset if NOT using a C++ voice profile.
                        # When using a profile, the formant transforms are already applied by the frontend.
                        if not getattr(self, "_usingVoiceProfile", False):
                            _applyVoiceToFrame(frame, self._curVoice)

                        if extraParamMultipliers:
                            for paramName, ratio in extraParamMultipliers:
                                setattr(frame, paramName, getattr(frame, paramName) * ratio)

                        frame.preFormantGain *= self._curVolume

                        # Use FrameEx from frontend callback if available (ABI v2+)
                        # Frontend has already mixed per-phoneme values with user defaults
                        frameEx = None
                        if frameExPtr and getattr(self._player, "hasFrameExSupport", lambda: False)():
                            # Copy C FrameEx to Python-owned struct
                            frameEx = speechPlayer.FrameEx()
                            ctypes.memmove(ctypes.byref(frameEx), frameExPtr, ctypes.sizeof(speechPlayer.FrameEx))

                        # Use queueFrameEx if we have FrameEx data, otherwise fall back
                        if frameEx is not None:
                            self._player.queueFrameEx(frame, frameEx, frameDuration, fadeDuration, userIndex=idxToSet)
                        else:
                            self._player.queueFrame(frame, frameDuration, fadeDuration, userIndex=idxToSet)
                        queuedCount += 1
                        lastStreamWasVoiced = True

                    ok = False
                    try:
                        # Automatic speed split: cap synthesis at 3.0x and put
                        # the excess into DSP time-stretch (frame-advance).
                        # Prevents formant mush at extreme rates.
                        _SYNTH_CAP = 2.0
                        _effectiveSpeed = self._curRate
                        if getattr(self, '_rateBoost', False):
                            _effectiveSpeed *= 2.0
                        if _effectiveSpeed > _SYNTH_CAP:
                            _synthSpeed = _SYNTH_CAP
                            _timeStretch = _effectiveSpeed / _SYNTH_CAP
                        else:
                            _synthSpeed = _effectiveSpeed
                            _timeStretch = 1.0
                        self._player.setTimeStretch(_timeStretch)

                        # Use queueIPA_ExWithText for stress correction (ABI v4+).
                        # Falls back to queueIPA_Ex internally if not available.
                        ok = self._frontend.queueIPA_ExWithText(
                            ipaText,
                            originalText=chunk,
                            speed=_synthSpeed,
                            basePitch=basePitch,
                            inflection=self._curInflection,
                            clauseType=clauseType,
                            userIndex=None,
                            onFrame=_onFrame,
                        )
                    except Exception:
                        log.error("TGSpeechBox: frontend queueIPA_ExWithText failed", exc_info=True)
                        ok = False

                    if not ok:
                        err = self._frontend.getLastError()
                        if err:
                            log.error(f"TGSpeechBox: frontend error: {err}")

                    # If the frontend fails or outputs nothing, keep going (indexes are still queued).
                    if (not ok) or queuedCount <= 0:
                        continue

                    # Streaming: kick the AudioThread as soon as we have
                    # frames so audio starts while we generate the rest.
                    if not hadKickedAudio:
                        if generation != self._speakGen:
                            return
                        self._audio.allFramesQueued = False
                        self._audio.beginUtterance()
                        if generation != self._speakGen:
                            self._audio.isSpeaking = False
                            return
                        self._audio.kick()
                        hadKickedAudio = True

                    # Optional punctuation pause (micro-silence) after the clause.
                    # Insert only when we actually queued a voiced frame; otherwise we'd
                    # be adding silence after silence.
                    if punctPauseMs and sawRealFrameInThisUtterance:
                        try:
                            dur = float(punctPauseMs)
                            fd = float(min(float(minFadeOutMs), dur))
                            self._player.queueFrame(None, dur, fd)
                            lastStreamWasVoiced = False
                        except Exception:
                            log.debug("TGSpeechBox: failed inserting punctuation pause", exc_info=True)

                    # Signal AudioThread that new frames are available (streaming mode).
                    # This replaces the 1ms polling sleep with an event-driven wakeup.
                    if hadKickedAudio:
                        self._audio._framesReady.set()

            # Emit IndexCommands after this block
            if indexesAfter:
                for idx in indexesAfter:
                    try:
                        if lastStreamWasVoiced:
                            dur = float(minFadeOutMs)
                            self._player.queueFrame(None, dur, dur, userIndex=int(idx))
                            lastStreamWasVoiced = False
                        else:
                            self._player.queueFrame(None, 0.0, 0.0, userIndex=int(idx))
                    except Exception:
                        log.debug("TGSpeechBox: failed to queue index marker %r", idx, exc_info=True)

        if endPause and endPause > 0:
            self._player.queueFrame(None, float(endPause), min(float(endPause), 5.0))

        # Utterance-end drain (#107).  This frame is the only time the DSP
        # gets to ring out after the last real frame: the AudioThread exits
        # as soon as synthesize() returns empty, so whatever decay hasn't
        # been rendered by then is simply cut.  1 ms silently truncated
        # word-final release bursts (final /t d p/, ~30-50 ms of decay).
        # NOTE: this longer tail widens the AudioThread's end-of-pass
        # wavePlayer.idle() window ~50x, which exposed a latent
        # isSpeaking-clobber race on b8 ship night (first utterance spoke,
        # everything after was silent).  The race is fixed by the
        # utteranceSeq guard in audio.py -- if you touch either side,
        # re-test live with two consecutive utterances and arrow-key spam.
        if hadRealSpeech:
            self._player.queueFrame(None, 50.0, 50.0)

        # Signal that all frames have been queued so the AudioThread
        # knows it can exit when synthesize() returns empty.
        self._audio.allFramesQueued = True
        self._audio._framesReady.set()  # wake AudioThread if waiting

        # Final generation check — if cancel() was called while we were
        # generating IPA/frames above, don't start the audio thread.
        if generation != self._speakGen:
            return

        if not hadKickedAudio:
            # No streaming kick happened (e.g. very short utterance).
            # Start the AudioThread now.
            self._audio.beginUtterance()
            if generation != self._speakGen:
                self._audio.isSpeaking = False
                return
            self._audio.kick()

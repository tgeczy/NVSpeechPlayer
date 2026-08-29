# -*- coding: utf-8 -*-
"""VoicingTone and FrameEx slider methods — mixin for SynthDriver.

Provides get/set pairs for all voicing-tone sliders (tilt, noise mod,
pitch-sync, speed quotient, aspiration tilt, cascade BW, tremor) and
FrameEx voice quality sliders (creakiness, breathiness, jitter, shimmer,
sharpness), plus:
- _pushFrameExDefaultsToFrontend()
- _applyVoicingTone(profileName)
"""

from __future__ import annotations

from logHandler import log

from . import speechPlayer
from .constants import VOICE_PROFILE_PREFIX


class VoicingToneMixin:
    """VoicingTone and FrameEx slider methods for SynthDriver."""

    # ---- Voice tilt slider ----

    def _get_voiceTilt(self):
        return int(getattr(self, "_curVoiceTilt", 50))

    def _set_voiceTilt(self, val):
        try:
            newVal = int(val)
            # Only re-apply if value actually changed
            if newVal == getattr(self, "_curVoiceTilt", 50):
                return

            self._curVoiceTilt = newVal

            # During init, just store the value — step 10 applies once.
            if not getattr(self, "_initComplete", False):
                return

            # Derive profile name from _curVoice (the source of truth)
            curVoice = getattr(self, "_curVoice", "Adam") or "Adam"
            if curVoice.startswith(VOICE_PROFILE_PREFIX):
                profileName = curVoice[len(VOICE_PROFILE_PREFIX):]
            else:
                profileName = ""

            # Re-apply voicing tone (and frontend profile) with the new tilt offset
            self._applyVoicingTone(profileName)
        except Exception:
            # Never crash during settings application
            pass

    # ---- Noise Glottal Modulation slider (0-100, maps to 0.0-1.0) ----

    def _get_noiseGlottalMod(self):
        return int(getattr(self, "_curNoiseGlottalMod", 0))

    def _set_noiseGlottalMod(self, val):
        try:
            newVal = int(val)
            if newVal == getattr(self, "_curNoiseGlottalMod", 0):
                return
            self._curNoiseGlottalMod = newVal
            if not getattr(self, "_initComplete", False):
                return
            curVoice = getattr(self, "_curVoice", "Adam") or "Adam"
            if curVoice.startswith(VOICE_PROFILE_PREFIX):
                profileName = curVoice[len(VOICE_PROFILE_PREFIX):]
            else:
                profileName = ""
            self._applyVoicingTone(profileName)
        except Exception:
            pass

    # ---- Pitch-sync F1 slider (0-100, maps to 0-120 Hz delta) ----

    def _get_pitchSyncF1(self):
        return int(getattr(self, "_curPitchSyncF1", 50))

    def _set_pitchSyncF1(self, val):
        try:
            newVal = int(val)
            if newVal == getattr(self, "_curPitchSyncF1", 50):
                return
            self._curPitchSyncF1 = newVal
            if not getattr(self, "_initComplete", False):
                return
            curVoice = getattr(self, "_curVoice", "Adam") or "Adam"
            if curVoice.startswith(VOICE_PROFILE_PREFIX):
                profileName = curVoice[len(VOICE_PROFILE_PREFIX):]
            else:
                profileName = ""
            self._applyVoicingTone(profileName)
        except Exception:
            pass

    # ---- Pitch-sync B1 slider (0-100, maps to 0-100 Hz delta) ----

    def _get_pitchSyncB1(self):
        return int(getattr(self, "_curPitchSyncB1", 50))

    def _set_pitchSyncB1(self, val):
        try:
            newVal = int(val)
            if newVal == getattr(self, "_curPitchSyncB1", 50):
                return
            self._curPitchSyncB1 = newVal
            if not getattr(self, "_initComplete", False):
                return
            curVoice = getattr(self, "_curVoice", "Adam") or "Adam"
            if curVoice.startswith(VOICE_PROFILE_PREFIX):
                profileName = curVoice[len(VOICE_PROFILE_PREFIX):]
            else:
                profileName = ""
            self._applyVoicingTone(profileName)
        except Exception:
            pass

    # ---- Speed Quotient slider (0-100, maps to 0.5-4.0) ----
    # Controls glottal pulse asymmetry for male/female voice distinction.
    # 0 = 0.5 (very breathy/soft), 50 = 2.0 (neutral), 100 = 4.0 (pressed/tense)

    def _get_speedQuotient(self):
        return int(getattr(self, "_curSpeedQuotient", 50))

    def _set_speedQuotient(self, val):
        try:
            newVal = int(val)
            if newVal == getattr(self, "_curSpeedQuotient", 50):
                return
            self._curSpeedQuotient = newVal
            if not getattr(self, "_initComplete", False):
                return
            curVoice = getattr(self, "_curVoice", "Adam") or "Adam"
            if curVoice.startswith(VOICE_PROFILE_PREFIX):
                profileName = curVoice[len(VOICE_PROFILE_PREFIX):]
            else:
                profileName = ""
            self._applyVoicingTone(profileName)
        except Exception:
            pass

    # ---- Aspiration Tilt slider (0-100 maps to -12 to +12 dB/oct) ----
    # Controls brightness/darkness of breath noise.
    # 0 = -12 dB/oct (dark/soft), 50 = 0 (neutral), 100 = +12 dB/oct (bright/harsh)

    def _get_aspirationTilt(self):
        return int(getattr(self, "_curAspirationTilt", 50))

    def _set_aspirationTilt(self, val):
        try:
            newVal = int(val)
            if newVal == getattr(self, "_curAspirationTilt", 50):
                return
            self._curAspirationTilt = newVal
            if not getattr(self, "_initComplete", False):
                return
            curVoice = getattr(self, "_curVoice", "Adam") or "Adam"
            if curVoice.startswith(VOICE_PROFILE_PREFIX):
                profileName = curVoice[len(VOICE_PROFILE_PREFIX):]
            else:
                profileName = ""
            self._applyVoicingTone(profileName)
        except Exception:
            pass

    # ---- Cascade Bandwidth Scale slider ----

    def _get_cascadeBwScale(self):
        return int(getattr(self, "_curCascadeBwScale", 50))

    def _set_cascadeBwScale(self, val):
        try:
            newVal = int(val)
            if newVal == getattr(self, "_curCascadeBwScale", 50):
                return
            self._curCascadeBwScale = newVal
            if not getattr(self, "_initComplete", False):
                return
            curVoice = getattr(self, "_curVoice", "Adam") or "Adam"
            if curVoice.startswith(VOICE_PROFILE_PREFIX):
                profileName = curVoice[len(VOICE_PROFILE_PREFIX):]
            else:
                profileName = ""
            self._applyVoicingTone(profileName)
        except Exception:
            pass

    # ---- Head Size (F4 Frequency Scale) slider ----
    # 0 = 0.7 (large pharynx, deep male), 50 = 1.0 (neutral),
    # 100 = 1.5 (small pharynx, child)

    def _get_headSize(self):
        return int(getattr(self, "_curHeadSize", 50))

    def _set_headSize(self, val):
        try:
            newVal = int(val)
            if newVal == getattr(self, "_curHeadSize", 50):
                return
            self._curHeadSize = newVal
            if not getattr(self, "_initComplete", False):
                return
            curVoice = getattr(self, "_curVoice", "Adam") or "Adam"
            if curVoice.startswith(VOICE_PROFILE_PREFIX):
                profileName = curVoice[len(VOICE_PROFILE_PREFIX):]
            else:
                profileName = ""
            self._applyVoicingTone(profileName)
        except Exception:
            pass

    # ---- Chorus Depth slider ----
    # 0 = off (default), 100 = full 50/50 blend

    def _get_chorusDepth(self):
        return int(getattr(self, "_curChorusDepth", 0))

    def _set_chorusDepth(self, val):
        try:
            newVal = int(val)
            if newVal == getattr(self, "_curChorusDepth", 0):
                return
            self._curChorusDepth = newVal
            if not getattr(self, "_initComplete", False):
                return
            curVoice = getattr(self, "_curVoice", "Adam") or "Adam"
            if curVoice.startswith(VOICE_PROFILE_PREFIX):
                profileName = curVoice[len(VOICE_PROFILE_PREFIX):]
            else:
                profileName = ""
            self._applyVoicingTone(profileName)
        except Exception:
            pass

    # ---- Chorus Detune slider ----
    # 0 = 0.5 Hz (slow beating), 50 = 2.0 Hz (default), 100 = 5.0 Hz (fast)

    def _get_chorusDetune(self):
        return int(getattr(self, "_curChorusDetune", 33))

    def _set_chorusDetune(self, val):
        try:
            newVal = int(val)
            if newVal == getattr(self, "_curChorusDetune", 33):
                return
            self._curChorusDetune = newVal
            if not getattr(self, "_initComplete", False):
                return
            curVoice = getattr(self, "_curVoice", "Adam") or "Adam"
            if curVoice.startswith(VOICE_PROFILE_PREFIX):
                profileName = curVoice[len(VOICE_PROFILE_PREFIX):]
            else:
                profileName = ""
            self._applyVoicingTone(profileName)
        except Exception:
            pass

    # ---- Voice Tremor slider ----

    def _get_voiceTremor(self):
        return int(getattr(self, "_curVoiceTremor", 0))

    def _set_voiceTremor(self, val):
        try:
            newVal = int(val)
            if newVal == getattr(self, "_curVoiceTremor", 0):
                return
            self._curVoiceTremor = newVal
            if not getattr(self, "_initComplete", False):
                return
            curVoice = getattr(self, "_curVoice", "Adam") or "Adam"
            if curVoice.startswith(VOICE_PROFILE_PREFIX):
                profileName = curVoice[len(VOICE_PROFILE_PREFIX):]
            else:
                profileName = ""
            self._applyVoicingTone(profileName)
        except Exception:
            pass

    # =========================================================================
    # FrameEx voice quality sliders (DSP v5+)
    # These are applied per-frame via queueFrameEx, not via voicingTone.
    # Slider range 0-100 maps to 0.0-1.0 for the DSP.
    # =========================================================================

    def _get_frameExCreakiness(self):
        return int(getattr(self, "_curFrameExCreakiness", 0))

    def _set_frameExCreakiness(self, val):
        try:
            self._curFrameExCreakiness = int(val)
            if getattr(self, "_initComplete", False):
                self._pushFrameExDefaultsToFrontend()
        except Exception:
            pass

    def _get_frameExBreathiness(self):
        return int(getattr(self, "_curFrameExBreathiness", 0))

    def _set_frameExBreathiness(self, val):
        try:
            self._curFrameExBreathiness = int(val)
            if getattr(self, "_initComplete", False):
                self._pushFrameExDefaultsToFrontend()
        except Exception:
            pass

    def _get_frameExJitter(self):
        return int(getattr(self, "_curFrameExJitter", 0))

    def _set_frameExJitter(self, val):
        try:
            self._curFrameExJitter = int(val)
            if getattr(self, "_initComplete", False):
                self._pushFrameExDefaultsToFrontend()
        except Exception:
            pass

    def _get_frameExShimmer(self):
        return int(getattr(self, "_curFrameExShimmer", 0))

    def _set_frameExShimmer(self, val):
        try:
            self._curFrameExShimmer = int(val)
            if getattr(self, "_initComplete", False):
                self._pushFrameExDefaultsToFrontend()
        except Exception:
            pass

    def _get_frameExSharpness(self):
        return int(getattr(self, "_curFrameExSharpness", 50))

    def _set_frameExSharpness(self, val):
        try:
            self._curFrameExSharpness = int(val)
            if getattr(self, "_initComplete", False):
                self._pushFrameExDefaultsToFrontend()
        except Exception:
            pass

    # ---- FrameEx defaults push ----

    def _pushFrameExDefaultsToFrontend(self) -> None:
        """Push current FrameEx slider values to the frontend (ABI v2+).

        The frontend mixes these user-level defaults with per-phoneme values
        when emitting frames via queueIPA_Ex(). This must be called:
        - After frontend initialization
        - Whenever a FrameEx slider changes
        - When switching voices (to restore per-voice settings)
        """
        if not hasattr(self, "_frontend") or not self._frontend:
            return
        if not self._frontend.hasFrameExSupport():
            return

        try:
            creakVal = getattr(self, "_curFrameExCreakiness", 0)
            breathVal = getattr(self, "_curFrameExBreathiness", 0)
            jitterVal = getattr(self, "_curFrameExJitter", 0)
            shimmerVal = getattr(self, "_curFrameExShimmer", 0)
            sharpnessVal = getattr(self, "_curFrameExSharpness", 50)

            # Convert slider values (0-100) to FrameEx ranges
            # Sharpness: exponential mapping centered at 50 = 1.0x.
            # Two octaves each way: 0 → 0.25x, 50 → 1.0x, 100 → 4.0x.
            # Formula: 4^((slider - 50) / 50) = 2^((slider - 50) / 25)
            sharpnessMul = 2.0 ** ((sharpnessVal - 50.0) / 25.0)

            self._frontend.setFrameExDefaults(
                creakiness=creakVal / 100.0,
                breathiness=breathVal / 100.0,
                jitter=jitterVal / 100.0,
                shimmer=shimmerVal / 100.0,
                sharpness=sharpnessMul,
            )
        except Exception:
            log.debug("TGSpeechBox: _pushFrameExDefaultsToFrontend failed", exc_info=True)

    # ---- Main voicing tone orchestrator ----

    def _applyVoicingTone(self, profileName: str) -> None:
        """Apply DSP-level voicing tone parameters safely.

        Gets base voicing tone from frontend (parses voicingTone: from YAML),
        then applies slider offsets on top.

        ALSO ensures the frontend voice profile (formant transforms) is applied.

        CRITICAL: This entire function is wrapped in try/except to prevent crashes
        from killing NVDA's settings application loop.
        """
        # Guard: need player
        if not hasattr(self, "_player") or not self._player:
            return

        # CRITICAL: Catch ALL exceptions here.
        try:
            # ALWAYS ensure the frontend has the correct voice profile set
            if hasattr(self, "_frontend") and self._frontend:
                self._frontend.setVoiceProfile(profileName or "")
                self._pushFrameExDefaultsToFrontend()

            playerHasSupport = getattr(self._player, "hasVoicingToneSupport", lambda: False)()
            if not playerHasSupport:
                return

            # Build the tone struct with safe defaults
            tone = speechPlayer.VoicingTone.defaults()

            # Get base tone from frontend (ABI v2+) - parses voicingTone: from YAML
            if profileName and hasattr(self, "_frontend") and self._frontend:
                if self._frontend.hasExplicitVoicingTone():
                    frontendTone = self._frontend.getVoicingTone()
                    if frontendTone:
                        tone.voicingPeakPos = frontendTone.voicingPeakPos
                        tone.voicedPreEmphA = frontendTone.voicedPreEmphA
                        tone.voicedPreEmphMix = frontendTone.voicedPreEmphMix
                        tone.highShelfGainDb = frontendTone.highShelfGainDb
                        tone.highShelfFcHz = frontendTone.highShelfFcHz
                        tone.highShelfQ = frontendTone.highShelfQ
                        tone.voicedTiltDbPerOct = frontendTone.voicedTiltDbPerOct
                        tone.noiseGlottalModDepth = frontendTone.noiseGlottalModDepth
                        tone.pitchSyncF1DeltaHz = frontendTone.pitchSyncF1DeltaHz
                        tone.pitchSyncB1DeltaHz = frontendTone.pitchSyncB1DeltaHz
                        tone.speedQuotient = frontendTone.speedQuotient
                        tone.aspirationTiltDbPerOct = frontendTone.aspirationTiltDbPerOct
                        tone.cascadeBwScale = frontendTone.cascadeBwScale
                        tone.nasalBwScale = frontendTone.nasalBwScale
                        tone.f4FreqScale = frontendTone.f4FreqScale
                        tone.nasalGainScale = frontendTone.nasalGainScale
                        if hasattr(frontendTone, 'chorusDepth'):
                            tone.chorusDepth = frontendTone.chorusDepth
                            tone.chorusDetuneHz = frontendTone.chorusDetuneHz

            # Helper for slider values
            def safe_float(val, default=0.0):
                try:
                    return float(val)
                except (ValueError, TypeError):
                    return default

            # Apply voice tilt OFFSET from the slider.
            # Direction is INTENTIONAL — think of a valve: at 100 it's wide
            # open and the rush muffles/darkens the voice; closing it toward
            # 0 opens the voice up, brighter. So slider up = positive dB/oct
            # = darker; slider down = negative = brighter (the DSP's
            # voicedTiltDbPerOct treats negative as brighter — opposite sign
            # convention from aspirationTiltDbPerOct and fricationTiltDb).
            # Do not "fix" this by flipping the map.
            tiltSlider = safe_float(getattr(self, "_curVoiceTilt", 50), 50.0)
            tiltOffset = (tiltSlider - 50.0) * (24.0 / 50.0)
            tone.voicedTiltDbPerOct += tiltOffset
            tone.voicedTiltDbPerOct = max(-24.0, min(24.0, tone.voicedTiltDbPerOct))

            # Apply noise glottal modulation from slider (0-100 maps to 0.0-1.0)
            noiseModSlider = safe_float(getattr(self, "_curNoiseGlottalMod", 0), 0.0)
            tone.noiseGlottalModDepth = noiseModSlider / 100.0

            # Apply pitch-sync F1 from slider (0-100 maps to -60 to +60 Hz, centered at 50 = 0)
            f1Slider = safe_float(getattr(self, "_curPitchSyncF1", 50), 50.0)
            tone.pitchSyncF1DeltaHz = (f1Slider - 50.0) * 1.2

            # Apply pitch-sync B1 from slider (0-100 maps to -50 to +50 Hz, centered at 50 = 0)
            b1Slider = safe_float(getattr(self, "_curPitchSyncB1", 50), 50.0)
            tone.pitchSyncB1DeltaHz = (b1Slider - 50.0) * 1.0

            # Apply speed quotient from slider (0-100 maps to 0.5-4.0, centered at 50 = 2.0)
            sqSlider = safe_float(getattr(self, "_curSpeedQuotient", 50), 50.0)
            if sqSlider <= 50.0:
                tone.speedQuotient = 0.5 + (sqSlider / 50.0) * 1.5
            else:
                tone.speedQuotient = 2.0 + ((sqSlider - 50.0) / 50.0) * 2.0

            # Apply aspiration tilt from slider (0-100 maps to -12 to +12 dB/oct, centered at 50 = 0)
            aspTiltSlider = safe_float(getattr(self, "_curAspirationTilt", 50), 50.0)
            tone.aspirationTiltDbPerOct = (aspTiltSlider - 50.0) * 0.24

            # Apply cascade bandwidth scale from slider (0-100 maps to 0.4-1.4, centered at 50 = 1.0)
            # Below 50: sharper formants (Eloquence-like clarity)
            # Above 50: wider formants (softer, more blended)
            bwSlider = safe_float(getattr(self, "_curCascadeBwScale", 50), 50.0)
            if bwSlider <= 50.0:
                # 0 -> 2.0 (wide/muffled), 50 -> 1.0 (neutral)
                tone.cascadeBwScale = 2.0 - (bwSlider / 50.0) * 1.0
            else:
                # 50 -> 1.0 (neutral), 100 -> 0.3 (sharp/ringy)
                tone.cascadeBwScale = 1.0 - ((bwSlider - 50.0) / 50.0) * 0.7
            tone.cascadeBwScale = max(0.3, min(2.0, tone.cascadeBwScale))

            # Apply voice tremor from slider (0-100 maps to 0.0-0.4 depth)
            tremorSlider = safe_float(getattr(self, "_curVoiceTremor", 0), 0.0)
            tone.tremorDepth = (tremorSlider / 100.0) * 0.4
            tone.tremorDepth = max(0.0, min(0.5, tone.tremorDepth))

            # Apply head size (F4 frequency scale) from slider
            # 0 = small head (F4 high, 1.25), 50 = neutral (1.0), 100 = large head (F4 low, 0.85)
            # Tighter range keeps the effect perceptually useful throughout.
            headSizeSlider = safe_float(getattr(self, "_curHeadSize", 50), 50.0)
            if headSizeSlider <= 50.0:
                # 0 -> 1.25 (small), 50 -> 1.0 (neutral)
                tone.f4FreqScale = 1.25 - (headSizeSlider / 50.0) * 0.25
            else:
                # 50 -> 1.0 (neutral), 100 -> 0.85 (large)
                tone.f4FreqScale = 1.0 - ((headSizeSlider - 50.0) / 50.0) * 0.15
            tone.f4FreqScale = max(0.7, min(1.5, tone.f4FreqScale))

            # Apply chorus depth from slider (0-100 maps to 0.0-1.0)
            chorusSlider = safe_float(getattr(self, "_curChorusDepth", 0), 0.0)
            tone.chorusDepth = chorusSlider / 100.0
            tone.chorusDepth = max(0.0, min(1.0, tone.chorusDepth))

            # Apply chorus detune from slider (0-100 maps to 0.5-5.0 Hz)
            # Default slider 33 maps to 2.0 Hz
            detuneSlider = safe_float(getattr(self, "_curChorusDetune", 33), 33.0)
            tone.chorusDetuneHz = 0.5 + (detuneSlider / 100.0) * 4.5
            tone.chorusDetuneHz = max(0.5, min(5.0, tone.chorusDetuneHz))

            # Apply to player
            self._player.setVoicingTone(tone)
            self._lastAppliedVoicingTone = tone

        except Exception as e:
            log.error(f"TGSpeechBox: _applyVoicingTone failed: {e}", exc_info=True)

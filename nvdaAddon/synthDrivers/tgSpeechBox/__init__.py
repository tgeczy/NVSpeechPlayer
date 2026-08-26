# -*- coding: utf-8 -*-
"""TGSpeechBox - NVDA synth driver (modernized)

Pipeline:
- eSpeak (NVDA built-in) for text -> IPA/phonemes
- nvspFrontend.dll for IPA -> timed SpeechPlayer frames
- speechPlayer.dll for frame -> PCM synthesis
"""

from __future__ import annotations

import ctypes
import math
import os
import queue
import threading

from logHandler import log
from synthDriverHandler import SynthDriver as _SynthDriverBase, VoiceInfo

from autoSettingsUtils.driverSetting import (
    BooleanDriverSetting, DriverSetting, NumericDriverSetting,
)

# Local module imports
from . import speechPlayer, espeak_direct
from ._dll_utils import findDllDir
from ._frontend import NvspFrontend

# Import from modularized components
from .constants import (
    languages, pauseModes, sampleRates,
    VOICE_PROFILE_PREFIX,
)
from .profile_utils import discoverVoiceProfiles
from .audio import BgThread, AudioThread
from .migrate_config import run as _migrate_config
from .migrate_config import rescale_volume as _rescale_volume

# Mixin modules
from .espeak_bridge import espeakSetVoiceDirect as _espeakSetVoiceDirect
from .lang_pack_settings import LangPackSettingsMixin
from .voicing_tone import VoicingToneMixin
from .voice_management import VoiceManagementMixin
from .speech_pipeline import SpeechPipelineMixin


try:
    import addonHandler
    addonHandler.initTranslation()
except Exception:
    def _(s): return s


class SynthDriver(
    LangPackSettingsMixin,
    VoicingToneMixin,
    VoiceManagementMixin,
    SpeechPipelineMixin,
    _SynthDriverBase,
):
    name = "tgSpeechBox"
    description = "TGSpeechBox"

    _supportedSettings = [
        _SynthDriverBase.VoiceSetting(),
        _SynthDriverBase.LanguageSetting(),
        _SynthDriverBase.RateSetting(),
        _SynthDriverBase.RateBoostSetting(),
        _SynthDriverBase.PitchSetting(),
        DriverSetting("legacyPitchMode", _("Pitch mode"), availableInSettingsRing=True),
        _SynthDriverBase.InflectionSetting(),
        _SynthDriverBase.VolumeSetting(),
        BooleanDriverSetting("yearSplitting", _("Year splitting (4-digit numbers as digit pairs)"), defaultVal=False),
        BooleanDriverSetting("thousandsSeparatorCommaToSpace", _("Thousands separator comma to space"), defaultVal=False),
        NumericDriverSetting("voiceTilt", _("Voice tilt (brightness)"), defaultVal=50),
        NumericDriverSetting("noiseGlottalMod", _("Noise glottal modulation"), defaultVal=0),
        NumericDriverSetting("pitchSyncF1", _("Pitch-sync F1 delta"), defaultVal=50),
        NumericDriverSetting("pitchSyncB1", _("Pitch-sync B1 delta"), defaultVal=50),
        NumericDriverSetting("speedQuotient", _("Speed quotient (voice tension)"), defaultVal=50),
        NumericDriverSetting("aspirationTilt", _("Aspiration tilt (breath color)"), defaultVal=50),
        NumericDriverSetting("cascadeBwScale", _("Formant sharpness (cascade bandwidth)"), defaultVal=50),
        NumericDriverSetting("voiceTremor", _("Voice tremor (shakiness)"), defaultVal=0),
        NumericDriverSetting("chorusDepth", _("Chorus depth (vocal fold asymmetry)"), defaultVal=0),
        NumericDriverSetting("chorusDetune", _("Chorus variation (requires depth)"), defaultVal=33),
        NumericDriverSetting("headSize", _("Head size (pharynx length)"), defaultVal=50),
        # FrameEx voice quality params (DSP v5+) - for creaky voice, breathiness, etc.
        NumericDriverSetting("frameExCreakiness", _("Creakiness (laryngealization)"), defaultVal=0),
        NumericDriverSetting("frameExBreathiness", _("Breathiness"), defaultVal=0),
        NumericDriverSetting("frameExJitter", _("Jitter (pitch variation)"), defaultVal=0),
        NumericDriverSetting("frameExShimmer", _("Shimmer (amplitude variation)"), defaultVal=0),
        NumericDriverSetting("frameExSharpness", _("Glottal sharpness"), defaultVal=50),
        NumericDriverSetting("legacyPitchInflectionScale",
            _("Legacy pitch inflection scale (only active when pitch mode is classic)"),
            defaultVal=29),
        DriverSetting("pauseMode", _("Pause mode")),
        DriverSetting("sampleRate", _("Sample rate")),
        # Runtime language adjustments (other settings are YAML-only now)
        DriverSetting("stopClosureMode", _("Stop closure mode")),
        DriverSetting("spellingDiphthongMode", _("Spelling diphthong mode")),
    ]

    supportedSettings = tuple(_supportedSettings)

    from synthDriverHandler import synthDoneSpeaking, synthIndexReached
    from speech.commands import IndexCommand, PitchCommand
    supportedCommands = {c for c in (IndexCommand, PitchCommand) if c}
    supportedNotifications = {synthIndexReached, synthDoneSpeaking}
    del IndexCommand, PitchCommand, synthDoneSpeaking, synthIndexReached

    exposeExtraParams = False
    _ESPEAK_PHONEME_MODE = 0x36100 + 0x82

    def __init__(self):
        # Step 0: One-time config migration (nvSpeechPlayer -> tgSpeechBox)
        _migrate_config()
        # Step 0b: one-time volume rescale to the 100%-scale (#114) — must
        # run after the rename migration (so migrated data is rescaled too)
        # and before super().__init__() restores saved settings.
        _rescale_volume()

        # =======================================================================
        # CRITICAL: Initialize ALL internal state BEFORE calling super().__init__()
        # because super().__init__() triggers NVDA to restore saved settings,
        # which calls our setters (_set_voice, _set_language, etc.)
        # =======================================================================

        # 1. Initialize default values for all instance variables
        #    so property getters/setters don't crash if called early
        self._curPitch = 50
        self._curVoice = "Adam"
        self._curInflection = 0.5
        self._curVolume = 1.0
        self._curRate = 1.0
        self._rateBoost = False
        self._curVoiceTilt = 50
        self._curNoiseGlottalMod = 0
        self._curPitchSyncF1 = 50
        self._curPitchSyncB1 = 50
        self._curSpeedQuotient = 50  # Maps to 2.0 (neutral)
        self._curAspirationTilt = 50  # Maps to 0.0 dB/oct (no tilt)
        self._curCascadeBwScale = 50  # Maps to 1.0 (no scaling)
        self._curVoiceTremor = 0     # Maps to 0.0 (no tremor)
        # FrameEx voice quality params (DSP v5+)
        self._curFrameExCreakiness = 0
        self._curFrameExBreathiness = 0
        self._curFrameExJitter = 0
        self._curFrameExShimmer = 0
        self._curFrameExSharpness = 50  # 50 = neutral (1.0x multiplier)
        self._curLegacyPitchInflectionScale = 29  # 29 = 0.58 (pack.h default)
        self._initPerVoiceStorage()
        self._usingVoiceProfile = False
        self._activeProfileName = ""
        self._pauseMode = "short"
        self._language = "auto"
        self._resolvedLang = "en-us"
        self._langPackSettingsCache: dict[str, object] = {}
        self._sampleRate = 22050

        # Initialize containers immediately to avoid NoneType errors
        self._voiceProfiles = []

        # Suppress YAML writes during NVDA config replay (YAML is source of truth)
        self._suppressLangPackWrites = True
        # Guard: skip cancel() and cache refresh in _set_language during init
        self._initComplete = False

        # 2. Check architecture compatibility
        if ctypes.sizeof(ctypes.c_void_p) not in (4, 8):
            raise RuntimeError('TGSpeechBox: unsupported Python architecture')

        # 3. Handle extra params if enabled
        if self.exposeExtraParams:
            self._extraParamNames = [x[0] for x in speechPlayer.Frame._fields_]
            self._extraParamAttrNames = [f"speechPlayer_{x}" for x in self._extraParamNames]

            extraSettings = tuple(
                NumericDriverSetting(attrName, f"Frame: {paramName}")
                for paramName, attrName in zip(self._extraParamNames, self._extraParamAttrNames)
            )
            self.supportedSettings = self.supportedSettings + extraSettings
            for attrName in self._extraParamAttrNames:
                setattr(self, attrName, 50)

        # 4. Setup paths and validate packs directory
        here = os.path.dirname(__file__)
        packsDir = os.path.join(here, "packs")
        self._packsDir = packsDir

        if not os.path.isdir(packsDir):
            raise RuntimeError(f"TGSpeechBox: missing packs directory at {packsDir}")

        # Validate required pack files
        requiredRel = [
            "phonemes.yaml",
            os.path.join("lang", "default.yaml"),
        ]
        missingRel = []
        for rel in requiredRel:
            if not os.path.isfile(os.path.join(packsDir, rel)):
                missingRel.append(rel)

        if missingRel:
            raise RuntimeError(f"TGSpeechBox: missing required packs: {', '.join(missingRel)}")


        # 5. Initialize core components (Player and Frontend)
        #    These MUST be ready before super().__init__() calls our setters
        self._player = speechPlayer.SpeechPlayer(self._sampleRate)

        dllDir = findDllDir(here)
        if not dllDir:
            raise RuntimeError('TGSpeechBox: could not find DLLs for this architecture')

        feDllPath = os.path.join(dllDir, 'nvspFrontend.dll')
        self._frontend = NvspFrontend(feDllPath, packsDir)
        # NOTE: We skip setLanguage("default") here — _set_language (triggered
        # by super().__init__) will load the correct pack via _applyFrontendLangTag.
        # That saves one redundant YAML parse cycle (~50-100ms).


        # 6. Discover voice profiles
        #    Try frontend API first (ABI v2+), fall back to Python parsing
        #    This MUST be done before super().__init__() so availableVoices is populated
        try:
            # Try frontend API first
            self._voiceProfiles = self._frontend.getVoiceProfileNames() if self._frontend.hasFrameExSupport() else []
            if not self._voiceProfiles:
                # Fall back to Python parsing
                self._voiceProfiles = discoverVoiceProfiles(packsDir) or []
            if self._voiceProfiles:
                log.info(f"TGSpeechBox: discovered voice profiles: {self._voiceProfiles}")
        except Exception as e:
            log.error(f"TGSpeechBox: error discovering voice profiles: {e}")
            self._voiceProfiles = []

        # Defer pack warnings to avoid blocking init.
        def _logPackWarnings():
            try:
                if self._frontend and self._frontend.hasVoiceProfileSupport():
                    warnings = self._frontend.getPackWarnings()
                    if warnings:
                        log.warning(f"TGSpeechBox: pack warnings: {warnings}")
            except Exception:
                pass
        try:
            import core
            core.callLater(100, _logPackWarnings)
        except Exception:
            pass


        # 7. Initialize audio system
        self._audio = AudioThread(self, self._player, self._sampleRate)
        self._bgQueue: "queue.Queue" = queue.Queue()
        self._bgStop = threading.Event()
        self._speakGen = 0  # Generation counter: cancel/speak race guard
        self._bgThread = BgThread(self._bgQueue, self._bgStop, onError=self._onBgThreadError)
        self._bgThread.start()


        # 8. Initialize eSpeak (direct DLL access — no NVDA _espeak module)
        #    Loads espeak.dll and calls espeak_Initialize directly.
        #    No WavePlayer, no background thread, no synth callback.
        #    Completely decoupled from NVDA's eSpeak NG synth lifecycle.
        #    Voice selection is deferred to _set_language (called via
        #    initSettings after __init__ returns).
        if not espeak_direct.init():
            log.warning("TGSpeechBox: eSpeak direct init failed")


        # =======================================================================
        # 9. NOW call super().__init__()
        #    This triggers NVDA to load config and call our setters
        #    Since everything above is ready, it will succeed
        # =======================================================================
        super().__init__()

        # Fresh config: loadSettings() has no saved [speech.tgSpeechBox]
        # section, so our language setter never fires.  Force it now.
        if not getattr(self, "_espeakLang", ""):
            self._set_language(self._language or "auto")

        # =======================================================================
        # 10. Post-init tasks (after NVDA has restored settings)
        # =======================================================================
        # All slider setters above deferred their DLL calls (_applyVoicingTone,
        # _pushFrameExDefaultsToFrontend) because _initComplete was False.
        # Do one consolidated apply now with the final slider values.

        self._scheduleEnableLangPackWrites()
        self._refreshLangPackSettingsCache()
        self._pushFrameExDefaultsToFrontend()

        # Apply voicing tone once with the final voice/slider state.
        curVoice = getattr(self, "_curVoice", "Adam") or "Adam"
        if curVoice.startswith(VOICE_PROFILE_PREFIX):
            self._applyVoicingTone(curVoice[len(VOICE_PROFILE_PREFIX):])
        else:
            self._applyVoicingTone("")

        self._initComplete = True

    @classmethod
    def check(cls):
        # Ensure DLLs exist for this NVDA / Python architecture (x86 vs x64).
        if ctypes.sizeof(ctypes.c_void_p) not in (4, 8):
            return False

        here = os.path.dirname(__file__)
        dllDir = findDllDir(here)
        if not dllDir:
            return False

        # Packs are expected in a local ./packs folder.
        packsDir = os.path.join(here, 'packs')
        if not os.path.isdir(packsDir):
            return False

        # Accept either casing on Windows, but check for both to be explicit.
        phonemesLower = os.path.join(packsDir, 'phonemes.yaml')
        phonemesUpper = os.path.join(packsDir, 'Phonemes.yaml')
        if not (os.path.isfile(phonemesLower) or os.path.isfile(phonemesUpper)):
            return False

        # default.yaml is required (others are optional at runtime).
        defaultYaml = os.path.join(packsDir, 'lang', 'default.yaml')
        if not os.path.isfile(defaultYaml):
            return False

        return True

    # ---- Available languages ----

    def _get_availableLanguages(self):
        return languages

    # ---- Punctuation pause mode (driver setting) ----

    def _get_availablePauseModes(self):
        return pauseModes

    # NVDA's settingsDialogs uses .capitalize() on setting IDs, which
    # lowercases everything after the first char.  "pauseMode".capitalize()
    # → "Pausemode" → looks for "availablePausemodes" (lowercase 'm').
    _get_availablePausemodes = _get_availablePauseModes

    def _get_pauseMode(self):
        return getattr(self, "_pauseMode", "short")

    def _set_pauseMode(self, mode):
        try:
            m = str(mode or "").strip().lower()
            if m not in pauseModes:
                m = "short"
            self._pauseMode = m
        except Exception:
            pass

    # ---- Sample rate (driver setting) ----

    def _get_availableSampleRates(self):
        return sampleRates

    _get_availableSamplerates = _get_availableSampleRates

    def _get_sampleRate(self):
        return str(getattr(self, "_sampleRate", 22050))

    def _set_sampleRate(self, rate):
        try:
            try:
                r = int(str(rate).strip())
            except (ValueError, TypeError):
                r = 22050
            if str(r) not in sampleRates:
                r = 22050

            # Only reinitialize if rate actually changed
            if r == getattr(self, "_sampleRate", None):
                return

            self._sampleRate = r

            # Only reinitialize if audio system already exists (not during initial construction)
            if hasattr(self, "_audio") and self._audio:
                self._reinitializeAudio()
        except Exception:
            log.debug("TGSpeechBox: _set_sampleRate failed", exc_info=True)

    def _reinitializeAudio(self):
        """Reinitialize audio subsystem after sample rate change."""
        try:
            self.cancel()
        except Exception:
            log.debug("TGSpeechBox: cancel failed during audio reinit", exc_info=True)

        try:
            # Terminate old audio thread
            if hasattr(self, "_audio") and self._audio:
                self._audio.terminate()

            # Terminate old player
            if hasattr(self, "_player") and self._player:
                self._player.terminate()

            # Create new player with new sample rate
            self._player = speechPlayer.SpeechPlayer(self._sampleRate)

            # Create new audio thread
            self._audio = AudioThread(self, self._player, self._sampleRate)

            # Reapply voicing tone to the new player
            # This must be done for ALL voices (profiles and Python presets)
            # to restore the user's tilt slider setting
            curVoice = getattr(self, "_curVoice", "Adam") or "Adam"
            if curVoice.startswith(VOICE_PROFILE_PREFIX):
                profileName = curVoice[len(VOICE_PROFILE_PREFIX):]
            else:
                profileName = ""
            self._applyVoicingTone(profileName)

        except Exception:
            log.error("TGSpeechBox: failed to reinitialize audio", exc_info=True)

    # ---- Language management ----

    def _get_language(self):
        return getattr(self, "_language", "auto")

    @staticmethod
    def _resolveAutoLang() -> str:
        """Map NVDA's UI language to the closest available TGSpeechBox language tag.

        Primary source: getLanguage() — the NVDA UI language the user chose.
        Secondary source: getWindowsLanguage() — used only for regional variant
        disambiguation when the NVDA language is a bare tag (e.g. "en" → use
        OS locale to pick "en-us" vs "en-gb").
        """
        try:
            import languageHandler
            # Primary: NVDA UI language (e.g. "hu", "en", "es").
            nvdaLang = (languageHandler.getLanguage() or "en").strip().lower().replace("_", "-")

            # Secondary: OS locale for regional variant (e.g. "en-us", "pt-br").
            winLang = (languageHandler.getWindowsLanguage() or "").strip().lower().replace("_", "-")
        except Exception:
            return "en-us"

        # Helper: find best match for a language tag in our pack list.
        def _findMatch(tag: str) -> str:
            if tag in languages and tag != "auto":
                return tag
            base = tag.split("-", 1)[0] if "-" in tag else tag
            if base in languages and base != "auto":
                return base
            for t in languages:
                if t != "auto" and t.startswith(base + "-"):
                    return t
            return ""

        # 1. Try NVDA UI language directly (exact or base match).
        match = _findMatch(nvdaLang)
        if match:
            # If the match is a bare base tag (e.g. "en") and the OS locale
            # gives a more specific regional variant, prefer that.
            if match == nvdaLang.split("-", 1)[0] and winLang.startswith(match + "-"):
                regional = _findMatch(winLang)
                if regional:
                    return regional
            return match

        # 2. Fall back to OS locale (covers cases where NVDA returns an
        #    unusual tag that doesn't match but the OS locale does).
        if winLang:
            match = _findMatch(winLang)
            if match:
                return match

        return "en-us"

    def _set_language(self, langCode):
        # Normalize to pack-style language tag: lowercase with hyphens.
        requested = str(langCode or "").strip().lower().replace("_", "-")

        # Resolve "auto" to the NVDA UI language.
        if requested == "auto":
            self._language = "auto"
            resolved = self._resolveAutoLang()
        else:
            # Validate against the exposed language list.
            if requested not in languages or requested == "auto":
                requested = "en-us"
            self._language = requested
            resolved = requested

        self._resolvedLang = resolved

        # Skip cancel() during __init__ — audio system may not exist yet
        if getattr(self, "_initComplete", False):
            try:
                self.cancel()
            except Exception:
                log.debug("TGSpeechBox: cancel failed while changing language", exc_info=True)

        # Configure eSpeak for text->phonemes.
        # Primary: use direct ListVoices+SetVoiceByName (same approach as SAPI)
        # to ensure the correct voice is selected synchronously.
        espeakApplied = None
        candidates = []

        if _espeakSetVoiceDirect(resolved):
            espeakApplied = resolved
        else:
            # Fallback: try setVoiceByLanguage with candidates.
            for c in (resolved, resolved.replace("-", "_")):
                if c and c not in candidates:
                    candidates.append(c)

            if "-" in resolved:
                base = resolved.split("-", 1)[0]
                for c in (base, base.replace("-", "_")):
                    if c and c not in candidates:
                        candidates.append(c)

            if "en" not in candidates:
                candidates.append("en")

            for tryCode in candidates:
                if espeak_direct.setVoiceByLanguage(tryCode):
                    espeakApplied = tryCode
                    break

        self._espeakLang = (espeakApplied or "").strip().lower().replace("_", "-")

        if espeakApplied is None:
            log.error(
                "TGSpeechBox: could not set eSpeak language for %r (tried %s)",
                resolved,
                candidates,
                exc_info=True,
            )

        # Keep frontend pack selection in sync with the resolved language tag.
        try:
            if getattr(self, "_frontend", None):
                self._applyFrontendLangTag(resolved)
                # Re-apply voice profile after language change — setLanguage
                # replaces the entire PackSet, so the profile's phonetic
                # transforms need to be re-applied on the new pack data.
                if getattr(self, "_usingVoiceProfile", False) and getattr(self, "_activeProfileName", ""):
                    self._frontend.setVoiceProfile(self._activeProfileName)
        except Exception:
            log.error("TGSpeechBox: error setting frontend language", exc_info=True)

        log.debug("TGSpeechBox: language setting=%r resolved=%r; eSpeak=%r; packs=%r", self._language, resolved, self._espeakLang or None, getattr(self, "_frontendLangTag", None))

        # Skip cache refresh and GUI update during __init__ — done explicitly in step 10
        if getattr(self, "_initComplete", False):
            try:
                self._refreshLangPackSettingsCache()
            except Exception:
                log.debug("TGSpeechBox: could not refresh language-pack cache", exc_info=True)
            self._scheduleSettingsPanelRefresh()

    # ---- Settings panel refresh ----

    def _scheduleSettingsPanelRefresh(self) -> None:
        """Schedule a refresh of the VoiceSettingsPanel to update checkboxes.

        When the user changes language, the language pack's settings may differ.
        This method finds the open settings panel (if any) and triggers an update
        so checkboxes reflect the new language pack's values.
        """
        try:
            import wx
            wx.CallAfter(self._doSettingsPanelRefresh)
        except Exception:
            pass

    def _doSettingsPanelRefresh(self) -> None:
        """Actually perform the settings panel refresh (called via wx.CallAfter)."""
        try:
            import wx
            from gui import settingsDialogs

            # Look for an open NVDASettingsDialog.
            for win in wx.GetTopLevelWindows():
                if not isinstance(win, settingsDialogs.NVDASettingsDialog):
                    continue

                # Find the VoiceSettingsPanel via catIdToInstanceMap.
                for panel in win.catIdToInstanceMap.values():
                    if panel is None:
                        continue
                    if isinstance(panel, settingsDialogs.VoiceSettingsPanel):
                        try:
                            panel.updateDriverSettings(changedSetting="language")
                        except Exception:
                            log.debug(
                                "TGSpeechBox: updateDriverSettings failed",
                                exc_info=True,
                            )
                        break
                break
        except Exception:
            # GUI may not be available (e.g., running in secure mode or during shutdown).
            log.debug("TGSpeechBox: could not refresh settings panel", exc_info=True)

    # ---- Frontend pack loading ----

    def _applyFrontendLangTag(self, tag: str) -> bool:
        """Ask the frontend to (re)load packs for *tag*, trying sensible fallbacks.

        Returns True if the frontend reported a successful load.
        """
        if not getattr(self, "_frontend", None):
            return False

        tag = str(tag or "default").strip().lower().replace("_", "-")
        candidates = [tag]
        if "-" in tag:
            candidates.append(tag.split("-", 1)[0])
        candidates.append("default")

        for cand in candidates:
            try:
                if self._frontend.setLanguage(cand):
                    self._frontendLangTag = cand
                    return True
            except Exception:
                log.debug("TGSpeechBox: frontend.setLanguage failed for %r", cand, exc_info=True)
                continue

        try:
            lastErr = self._frontend.getLastError()
        except Exception:
            log.debug("TGSpeechBox: frontend.getLastError failed", exc_info=True)
            lastErr = None
        log.error("TGSpeechBox: frontend could not load pack for %r (tried %s). %s", tag, candidates, lastErr)
        return False

    def reloadLanguagePack(self, tag: str | None = None) -> bool:
        """Public helper (used by the settings panel) to reload frontend packs.

        If *tag* is omitted, reloads the currently selected driver language.
        """
        ok = self._applyFrontendLangTag(tag or self._getCurrentLangTag())
        if ok:
            try:
                self._refreshLangPackSettingsCache()
            except Exception:
                log.debug("TGSpeechBox: could not refresh language-pack cache after reload", exc_info=True)
        return ok

    # ---- Background thread helpers ----

    def _onBgThreadError(self):
        """Called by BgThread when an unhandled exception occurs.

        Ensures AudioThread doesn't hang waiting for frames that will never
        come (e.g. if _speakBg crashed after setting allFramesQueued=False).
        """
        if hasattr(self, "_audio") and self._audio:
            self._audio.allFramesQueued = True
            self._audio._framesReady.set()

    def _enqueue(self, func, *args, **kwargs):
        if self._bgStop.is_set():
            return
        self._bgQueue.put((func, args, kwargs))

    # ---- Lifecycle methods ----

    def cancel(self):
        """Cancel current speech immediately."""
        try:
            # Guard against early calls before __init__ completes
            if not hasattr(self, "_player") or not self._player:
                return
            if not hasattr(self, "_audio") or not self._audio:
                return

            # === PHASE 1: Stop audio output IMMEDIATELY ===
            # isSpeaking must be cleared BEFORE anything else so the
            # AudioThread's inner loop exits at the next iteration and
            # the post-synthesize isSpeaking re-check prevents it from
            # feeding any more audio to the WavePlayer.
            self._audio.allFramesQueued = True  # stop AudioThread polling
            self._audio._framesReady.set()      # wake if waiting on Event
            self._audio.isSpeaking = False
            self._audio.stopPlayback()
            self._audio._applyFadeIn = True

            # === PHASE 2: Invalidate in-flight work ===
            # Bump generation to invalidate any in-flight or pending _speakBg jobs.
            # BgThread checks this counter between chunks and inside _onFrame callbacks.
            self._speakGen += 1

            # Drain pending jobs as an optimisation (they'd bail on generation
            # mismatch anyway, but this avoids the dequeue-and-bail overhead).
            try:
                while True:
                    self._bgQueue.get_nowait()
                    self._bgQueue.task_done()
            except queue.Empty:
                pass

            # === PHASE 3: Purge frame queue and flush resonators ===
            # We do NOT call player.synthesize() here because the
            # AudioThread may still be inside its own synthesize() call
            # (it exits after checking isSpeaking, which we just cleared).
            # Concurrent synthesize() calls on the unsynchronised C DLL
            # cause data races.  Instead, just purge the frame queue.
            # The fade-in envelope (_applyFadeIn) will mask any stale
            # resonator energy when the next utterance starts.
            self._player.queueFrame(None, 3.0, 3.0, purgeQueue=True)

            self._audio.kick()
        except Exception:
            log.debug("TGSpeechBox: cancel failed", exc_info=True)

    def pause(self, switch):
        try:
            if self._audio:
                self._audio.pausePlayback(switch)
        except Exception:
            log.debug("TGSpeechBox: pause failed", exc_info=True)

    def terminate(self):
        try:
            # Cancel any ongoing speech first
            self.cancel()

            # Signal the background thread to stop
            if hasattr(self, "_bgStop"):
                self._bgStop.set()
            if hasattr(self, "_bgQueue"):
                # Put None to wake up the queue.get() if it's blocking
                try:
                    self._bgQueue.put_nowait(None)
                except queue.Full:
                    # Queue is full - thread should wake up anyway when processing items
                    pass
                except (AttributeError, TypeError):
                    # Queue not properly initialized
                    pass

            # Terminate audio thread FIRST (it uses the player)
            # Do this before waiting on bgThread since audio thread is higher priority
            if hasattr(self, "_audio") and self._audio:
                try:
                    self._audio.terminate()
                except Exception:
                    log.debug("TGSpeechBox: audio terminate failed", exc_info=True)
                self._audio = None

            # Now join the background thread with a SHORT timeout
            # It should exit quickly since _bgStop is set
            if hasattr(self, "_bgThread") and self._bgThread is not None:
                try:
                    self._bgThread.join(timeout=0.3)
                except (TypeError, AttributeError):
                    # Very old Python without timeout parameter
                    pass
                except Exception:
                    log.debug("TGSpeechBox: bgThread join failed", exc_info=True)
                self._bgThread = None

            # Terminate frontend (unloads nvspFrontend.dll)
            # Do this BEFORE terminating player since frontend may reference player resources
            if getattr(self, "_frontend", None):
                try:
                    self._frontend.terminate()
                except Exception:
                    log.debug("TGSpeechBox: frontend terminate failed", exc_info=True)
                self._frontend = None

            # Terminate player last (unloads speechPlayer.dll)
            if hasattr(self, "_player") and self._player:
                try:
                    self._player.terminate()
                except Exception:
                    log.debug("TGSpeechBox: player terminate failed", exc_info=True)
                self._player = None

            # Finally terminate espeak (direct — no _espeak module involvement)
            espeak_direct.terminate()
        except Exception:
            log.debug("TGSpeechBox: terminate failed", exc_info=True)

    # ---- loadSettings override (Escape key fix) ----

    def loadSettings(self, onlyChanged=False):
        """Override loadSettings to ensure frontend profile is always properly restored.

        This fixes the Escape key bug where:
        1. User selects a voice profile (e.g., profile:Beth)
        2. User opens settings, modifies tilt/rate, then hits Escape
        3. NVDA calls loadSettings() to restore saved settings
        4. If anything goes wrong (exception in changeVoice, etc.), the frontend
           profile could be left in an inconsistent state

        Our fix: After the parent loadSettings() completes (successfully or not),
        we always re-sync the frontend with the current voice profile state.
        """
        # Capture the voice BEFORE loadSettings runs, in case something corrupts it
        voiceBeforeLoad = getattr(self, "_curVoice", None)

        try:
            # Call parent implementation
            super().loadSettings(onlyChanged)
        except Exception:
            log.debug("TGSpeechBox: parent loadSettings failed", exc_info=True)
            # Don't re-raise - we'll try to recover below

        # CRITICAL: Always re-sync frontend profile after loadSettings completes.
        # This ensures the frontend has the correct phonetic transformations applied,
        # even if NVDA's loadSettings hit an exception and tried to "recover".
        try:
            curVoice = getattr(self, "_curVoice", None)

            # If voice got corrupted (changed unexpectedly), restore it
            if voiceBeforeLoad and curVoice != voiceBeforeLoad:
                # Check if the "new" voice is just a fallback to Adam
                # when we actually had a valid profile before
                if voiceBeforeLoad.startswith(VOICE_PROFILE_PREFIX) and curVoice == "Adam":
                    log.debug(f"TGSpeechBox: loadSettings corrupted voice from {voiceBeforeLoad} to {curVoice}, restoring")
                    self._curVoice = voiceBeforeLoad
                    curVoice = voiceBeforeLoad

            # Re-apply frontend profile to ensure phonetic transformations are active
            if curVoice and curVoice.startswith(VOICE_PROFILE_PREFIX):
                profileName = curVoice[len(VOICE_PROFILE_PREFIX):]
                self._activeProfileName = profileName
                # Re-apply voicing tone as well
                self._applyVoicingTone(profileName)
                log.debug(f"TGSpeechBox: loadSettings re-synced frontend profile '{profileName}'")
            else:
                # Python preset or no voice - ensure frontend profile is cleared
                if hasattr(self, "_frontend") and self._frontend:
                    self._frontend.setVoiceProfile("")
                    self._usingVoiceProfile = False
                    self._activeProfileName = ""
                    self._applyVoicingTone("")
        except Exception:
            log.debug("TGSpeechBox: loadSettings frontend re-sync failed", exc_info=True)

    # ---- Core NVDA settings (rate, pitch, volume, inflection) ----

    def _get_rate(self):
        try:
            return int(math.log(getattr(self, "_curRate", 1.0) / 0.25, 2) * 25.0)
        except Exception:
            return 50

    def _set_rate(self, val):
        try:
            self._curRate = 0.25 * (2 ** (float(val) / 25.0))
        except Exception:
            pass

    def _get_rateBoost(self):
        return self._rateBoost

    def _set_rateBoost(self, val):
        self._rateBoost = val
        # Apply DSP-level time-stretch instead of doubling frontend speed.
        # Cycle-skipping preserves formant quality at extreme rates.
        if hasattr(self, '_player') and self._player:
            self._player.setTimeStretch(2.0 if val else 1.0)

    def _get_pitch(self):
        return int(getattr(self, "_curPitch", 50))

    def _set_pitch(self, val):
        try:
            self._curPitch = int(val)
        except Exception:
            pass

    def _get_volume(self):
        # 100% <-> engine gain 1.0, matching every other platform (#114).
        # The historical /75 mapping made fresh installs report ~75% while
        # Android/Linux/SAPI default to 100; saved configs from that era
        # are rescaled once by migrate_config.rescale_volume().
        return int(getattr(self, "_curVolume", 1.0) * 100)

    def _set_volume(self, val):
        try:
            self._curVolume = float(val) / 100.0
        except Exception:
            pass

    def _get_inflection(self):
        return int(getattr(self, "_curInflection", 0.5) / 0.01)

    def _set_inflection(self, val):
        try:
            self._curInflection = float(val) * 0.01
        except Exception:
            pass

    # ── Promote mixin methods into class dict ────────────────────────────
    #
    # NVDA's AutoPropertyObject metaclass builds property descriptors from
    # _get_*/_set_* methods found in the class namespace during type.__new__.
    # Methods inherited via MRO from mixins aren't in the namespace, so the
    # base class's NotImplementedError stubs win.  Explicit assignments here
    # place them in the namespace at class creation time.

    # VoiceManagementMixin
    _get_voice = VoiceManagementMixin._get_voice
    _set_voice = VoiceManagementMixin._set_voice
    _getAvailableVoices = VoiceManagementMixin._getAvailableVoices

    # SpeechPipelineMixin
    speak = SpeechPipelineMixin.speak

    # VoicingToneMixin
    _get_voiceTilt = VoicingToneMixin._get_voiceTilt
    _set_voiceTilt = VoicingToneMixin._set_voiceTilt
    _get_noiseGlottalMod = VoicingToneMixin._get_noiseGlottalMod
    _set_noiseGlottalMod = VoicingToneMixin._set_noiseGlottalMod
    _get_pitchSyncF1 = VoicingToneMixin._get_pitchSyncF1
    _set_pitchSyncF1 = VoicingToneMixin._set_pitchSyncF1
    _get_pitchSyncB1 = VoicingToneMixin._get_pitchSyncB1
    _set_pitchSyncB1 = VoicingToneMixin._set_pitchSyncB1
    _get_speedQuotient = VoicingToneMixin._get_speedQuotient
    _set_speedQuotient = VoicingToneMixin._set_speedQuotient
    _get_aspirationTilt = VoicingToneMixin._get_aspirationTilt
    _set_aspirationTilt = VoicingToneMixin._set_aspirationTilt
    _get_cascadeBwScale = VoicingToneMixin._get_cascadeBwScale
    _set_cascadeBwScale = VoicingToneMixin._set_cascadeBwScale
    _get_voiceTremor = VoicingToneMixin._get_voiceTremor
    _set_voiceTremor = VoicingToneMixin._set_voiceTremor
    _get_chorusDepth = VoicingToneMixin._get_chorusDepth
    _set_chorusDepth = VoicingToneMixin._set_chorusDepth
    _get_chorusDetune = VoicingToneMixin._get_chorusDetune
    _set_chorusDetune = VoicingToneMixin._set_chorusDetune
    _get_headSize = VoicingToneMixin._get_headSize
    _set_headSize = VoicingToneMixin._set_headSize
    _get_frameExCreakiness = VoicingToneMixin._get_frameExCreakiness
    _set_frameExCreakiness = VoicingToneMixin._set_frameExCreakiness
    _get_frameExBreathiness = VoicingToneMixin._get_frameExBreathiness
    _set_frameExBreathiness = VoicingToneMixin._set_frameExBreathiness
    _get_frameExJitter = VoicingToneMixin._get_frameExJitter
    _set_frameExJitter = VoicingToneMixin._set_frameExJitter
    _get_frameExShimmer = VoicingToneMixin._get_frameExShimmer
    _set_frameExShimmer = VoicingToneMixin._set_frameExShimmer
    _get_frameExSharpness = VoicingToneMixin._get_frameExSharpness
    _set_frameExSharpness = VoicingToneMixin._set_frameExSharpness

    # LangPackSettingsMixin (explicit accessors)
    _get_legacyPitchMode = LangPackSettingsMixin._get_legacyPitchMode
    _set_legacyPitchMode = LangPackSettingsMixin._set_legacyPitchMode
    _get_availableLegacyPitchModes = LangPackSettingsMixin._get_availableLegacyPitchModes
    _get_availableLegacypitchmodes = LangPackSettingsMixin._get_availableLegacypitchmodes
    _get_legacyPitchInflectionScale = LangPackSettingsMixin._get_legacyPitchInflectionScale
    _set_legacyPitchInflectionScale = LangPackSettingsMixin._set_legacyPitchInflectionScale

    # LangPackSettingsMixin (generated accessors from _LANG_PACK_SPECS)
    # These were injected onto the mixin via setattr; copy them into this
    # class namespace so the metaclass sees them.
    for _name in list(vars(LangPackSettingsMixin)):
        if _name.startswith(("_get_", "_set_")) and _name not in locals():
            locals()[_name] = vars(LangPackSettingsMixin)[_name]
    del _name

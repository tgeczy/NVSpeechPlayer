# -*- coding: utf-8 -*-
"""
NV Speech Player - NVDA synth driver (modernized)

This driver uses:
- eSpeak (NVDA's built-in) for text -> phoneme/IPA conversion
- NV Speech Player (speechPlayer.dll) for formant synthesis rendering
"""

from __future__ import annotations

import ctypes
import math
import os
import queue
import re
import threading
import weakref
from collections import OrderedDict

import config
import nvwave

from logHandler import log
from synthDrivers import _espeak
from synthDriverHandler import SynthDriver, VoiceInfo, synthDoneSpeaking, synthIndexReached

# NVDA command classes moved around across versions; keep imports tolerant.
try:
    from speech.commands import IndexCommand, PitchCommand
except Exception:
    try:
        from speech.commands import IndexCommand  # type: ignore
        PitchCommand = None  # type: ignore
    except Exception:
        import speech  # fallback
        IndexCommand = getattr(speech, "IndexCommand", None)
        PitchCommand = getattr(speech, "PitchCommand", None)

from autoSettingsUtils.driverSetting import DriverSetting, NumericDriverSetting

from . import ipa
from . import speechPlayer


# Split on punctuation+space so we can add end-of-clause pauses.
re_textPause = re.compile(r"(?<=[.?!,:;])\s", re.DOTALL | re.UNICODE)

# Language choices exposed in NVDA settings.
languages = OrderedDict([
    ("en-us", VoiceInfo("en-us", "English (US)")),
    ("en", VoiceInfo("en", "English (UK)")),
    ("es", VoiceInfo("es", "Spanish")),
    ("hu", VoiceInfo("hu", "Hungarian")),
    ("pt-br", VoiceInfo("pt-br", "Brazilian Portuguese")),
    ("pl", VoiceInfo("pl", "Polish")),
])


# Voice presets: simple multipliers/overrides on the generated frames.
voices = {
    "Adam": {
        "cb1_mul": 1.3,
        "pa6_mul": 1.3,
        "fricationAmplitude_mul": 0.85,
    },
    "Benjamin": {
        "cf1_mul": 1.01,
        "cf2_mul": 1.02,
        "cf4": 3770,
        "cf5": 4100,
        "cf6": 5000,
        "cfNP_mul": 0.9,
        "cb1_mul": 1.3,
        "fricationAmplitude_mul": 0.7,
        "pa6_mul": 1.3,
    },
    "Caleb": {
        "aspirationAmplitude": 1,
        "voiceAmplitude": 0,
    },
    "David": {
        "voicePitch_mul": 0.75,
        "endVoicePitch_mul": 0.75,
        "cf1_mul": 0.75,
        "cf2_mul": 0.85,
        "cf3_mul": 0.85,
    },
}


def applyVoiceToFrame(frame: speechPlayer.Frame, voiceName: str) -> None:
    v = voices.get(voiceName) or voices.get("Adam", {})
    for paramName in (x[0] for x in frame._fields_):
        absVal = v.get(paramName)
        if absVal is not None:
            setattr(frame, paramName, absVal)
        mulVal = v.get("%s_mul" % paramName)
        if mulVal is not None:
            setattr(frame, paramName, getattr(frame, paramName) * mulVal)


class _BgThread(threading.Thread):
    """Runs text->IPA->frames generation so speak() doesn't block NVDA."""
    def __init__(self, q: "queue.Queue", stopEvent: threading.Event):
        super().__init__(name=f"{self.__class__.__module__}.{self.__class__.__qualname__}")
        self.daemon = True
        self._q = q
        self._stop = stopEvent

    def run(self):
        while not self._stop.is_set():
            try:
                item = self._q.get(timeout=0.2)
            except queue.Empty:
                continue
            try:
                if item is None:
                    return
                func, args, kwargs = item
                func(*args, **kwargs)
            except Exception:
                log.error("nvSpeechPlayer: error in background thread", exc_info=True)
            finally:
                try:
                    self._q.task_done()
                except Exception:
                    pass


class _AudioThread(threading.Thread):
    """Pulls synthesized audio from the DLL and feeds nvwave.WavePlayer."""
    def __init__(self, synth: "SynthDriver", player: speechPlayer.SpeechPlayer, sampleRate: int):
        super().__init__(name=f"{self.__class__.__module__}.{self.__class__.__qualname__}")
        self.daemon = True
        self._synthRef = weakref.ref(synth)
        self._player = player
        self._sampleRate = int(sampleRate)

        self._keepAlive = True
        self.isSpeaking = False

        self._wake = threading.Event()
        self._init = threading.Event()

        self._wavePlayer = None
        self._outputDevice = None

        self.start()
        self._init.wait()

    def _getOutputDevice(self):
        try:
            return config.conf["speech"]["outputDevice"]
        except Exception:
            try:
                return config.conf["audio"]["outputDevice"]
            except Exception:
                return None

    def _feed(self, data: bytes, onDone=None) -> None:
        if not self._wavePlayer:
            return
        try:
            self._wavePlayer.feed(data, len(data), onDone=onDone)
        except TypeError:
            try:
                if onDone is None:
                    self._wavePlayer.feed(data)
                else:
                    self._wavePlayer.feed(data, onDone=onDone)
            except Exception:
                pass

    def terminate(self):
        self._keepAlive = False
        self.isSpeaking = False
        self._wake.set()
        self.join(timeout=2.0)
        try:
            if self._wavePlayer:
                self._wavePlayer.stop()
        except Exception:
            pass

    def kick(self):
        self._wake.set()

    def run(self):
        try:
            self._outputDevice = self._getOutputDevice()
            self._wavePlayer = nvwave.WavePlayer(
                channels=1,
                samplesPerSec=self._sampleRate,
                bitsPerSample=16,
                outputDevice=self._outputDevice,
            )
        finally:
            self._init.set()

        while self._keepAlive:
            self._wake.wait()
            self._wake.clear()

            lastIndex = None

            while self._keepAlive and self.isSpeaking:
                data = self._player.synthesize(8192)

                if data:
                    n = int(getattr(data, "length", 0) or 0)
                    if n <= 0:
                        continue

                    nbytes = n * ctypes.sizeof(ctypes.c_short)
                    audioBytes = ctypes.string_at(ctypes.addressof(data), nbytes)

                    idx = int(self._player.getLastIndex())
                    s = self._synthRef()

                    if idx >= 0:
                        def cb(index=idx, synth=s):
                            if synth:
                                synthIndexReached.notify(synth=synth, index=index)
                        self._feed(audioBytes, onDone=cb)
                    else:
                        self._feed(audioBytes)

                    lastIndex = idx
                    continue
                
                break

            idx = int(self._player.getLastIndex())
            if idx >= 0 and idx != lastIndex:
                s = self._synthRef()
                if s:
                    synthIndexReached.notify(synth=s, index=idx)

            try:
                if self._wavePlayer:
                    self._wavePlayer.idle()
            except Exception:
                pass

            s = self._synthRef()
            if s:
                synthDoneSpeaking.notify(synth=s)

            self.isSpeaking = False


class SynthDriver(SynthDriver):
    name = "nvSpeechPlayer"
    description = "NV Speech Player"

    supportedSettings = (
        DriverSetting("language", "Language"),
        SynthDriver.VoiceSetting(),
        SynthDriver.RateSetting(),
        SynthDriver.PitchSetting(),
        SynthDriver.VolumeSetting(),
        SynthDriver.InflectionSetting(),
    )

    supportedCommands = {c for c in (IndexCommand, PitchCommand) if c}
    supportedNotifications = {synthIndexReached, synthDoneSpeaking}

    exposeExtraParams = True
    _ESPEAK_PHONEME_MODE = 0x36100 + 0x82

    def __init__(self):
        super().__init__()

        if ctypes.sizeof(ctypes.c_void_p) != 4:
            raise RuntimeError("nvSpeechPlayer: 32-bit only")

        if self.exposeExtraParams:
            self._extraParamNames = [x[0] for x in speechPlayer.Frame._fields_]
            extraSettings = tuple(
                NumericDriverSetting(f"speechPlayer_{x}", f"Frame: {x}")
                for x in self._extraParamNames
            )
            self.supportedSettings = self.supportedSettings + extraSettings
            for x in self._extraParamNames:
                setattr(self, f"speechPlayer_{x}", 50)

        self._sampleRate = 16000
        self._player = speechPlayer.SpeechPlayer(self._sampleRate)

        _espeak.initialize()

        self._language = "en-us"
        self._curPitch = 50
        self._curVoice = "Adam"
        self._curInflection = 0.5
        self._curVolume = 1.0
        self._curRate = 1.0

        self.language = self._language
        self.pitch = 50
        self.rate = 50
        self.volume = 90
        self.inflection = 60

        self._audio = _AudioThread(self, self._player, self._sampleRate)

        self._bgQueue: "queue.Queue" = queue.Queue()
        self._bgStop = threading.Event()
        self._bgThread = _BgThread(self._bgQueue, self._bgStop)
        self._bgThread.start()

    @classmethod
    def check(cls):
        if ctypes.sizeof(ctypes.c_void_p) != 4:
            return False
        dllPath = os.path.join(os.path.dirname(__file__), "speechPlayer.dll")
        return os.path.isfile(dllPath)

    def _get_availableLanguages(self):
        return languages

    def _get_language(self):
        return getattr(self, "_language", "en-us")

    def _set_language(self, langCode):
        code = str(langCode or "").strip().lower()
        if code not in languages:
            code = "en-us"

        try:
            self.cancel()
        except Exception:
            pass

        applied = False
        for tryCode in (code, code.replace("_", "-"), code.replace("-", "_")):
            try:
                ok = _espeak.setVoiceByLanguage(tryCode)
                if ok is None or ok:
                    applied = True
                    code = tryCode.lower()
                    break
            except Exception:
                continue

        if not applied:
            try:
                _espeak.setVoiceByLanguage("en")
                code = "en"
                applied = True
            except Exception:
                log.error("nvSpeechPlayer: could not set language", exc_info=True)

        self._language = code

    def _enqueue(self, func, *args, **kwargs):
        if self._bgStop.is_set():
            return
        self._bgQueue.put((func, args, kwargs))

    def _notifyIndexesAndDone(self, indexes):
        for i in indexes:
            synthIndexReached.notify(synth=self, index=i)
        synthDoneSpeaking.notify(synth=self)

    def _espeakTextToIPA(self, text: str) -> str:
        if not text:
            return ""
        textBuf = ctypes.create_unicode_buffer(text)
        textPtr = ctypes.c_void_p(ctypes.addressof(textBuf))
        chunks = []
        lastPtr = None
        while textPtr and textPtr.value:
            if lastPtr == textPtr.value:
                break
            lastPtr = textPtr.value
            phonemeBuf = _espeak.espeakDLL.espeak_TextToPhonemes(
                ctypes.byref(textPtr),
                _espeak.espeakCHARS_WCHAR,
                self._ESPEAK_PHONEME_MODE,
            )
            if phonemeBuf:
                chunks.append(ctypes.string_at(phonemeBuf))
            else:
                break
        ipaBytes = b"".join(chunks)
        try:
            return ipaBytes.decode("utf8", errors="ignore").strip()
        except Exception:
            return ""

    def speak(self, speechSequence):
        indexes = []
        anyText = False
        for item in speechSequence:
            if IndexCommand and isinstance(item, IndexCommand):
                indexes.append(item.index)
            elif isinstance(item, str) and item.strip():
                anyText = True

        if (not anyText):
            self._enqueue(self._notifyIndexesAndDone, indexes)
            return

        self._enqueue(self._speakBg, list(speechSequence))

    def _speakBg(self, speakList):
        userIndex = None
        pitchOffset = 0
        
        i = 0
        while i < len(speakList):
            item = speakList[i]
            if i > 0:
                prev = speakList[i - 1]
                if isinstance(item, str) and isinstance(prev, str):
                    speakList[i - 1] = " ".join([prev, item])
                    del speakList[i]
                    continue
            i += 1

        endPause = 20.0

        for item in speakList:
            if PitchCommand and isinstance(item, PitchCommand):
                pitchOffset = getattr(item, "offset", 0) or 0
                continue
            if IndexCommand and isinstance(item, IndexCommand):
                userIndex = item.index
                continue
            if not isinstance(item, str):
                continue

            for chunk in re_textPause.split(item):
                if not chunk: continue
                chunk = chunk.strip()
                if not chunk: continue

                clauseType = chunk[-1] if chunk[-1] in ".?!," else None
                d_end = float(150.0 if clauseType in (".", "!", "?") else (120.0 if clauseType == "," else 100.0)) / float(self._curRate)

                ipaText = self._espeakTextToIPA(chunk)
                
                # FORCE INDEX FALLBACK: send tiny silent frame to carry the index if IPA is empty
                if not ipaText:
                    if userIndex is not None:
                        dummy = speechPlayer.Frame()
                        dummy.voiceAmplitude = 0
                        dummy.fricationAmplitude = 0
                        self._player.queueFrame(dummy, 10.0, 5.0, userIndex=userIndex)
                        userIndex = None
                    continue

                pitch = float(self._curPitch) + float(pitchOffset)
                basePitch = 25.0 + (21.25 * (pitch / 12.5))

                gen = list(ipa.generateFramesAndTiming(
                    ipaText,
                    speed=self._curRate,
                    basePitch=basePitch,
                    inflection=self._curInflection,
                    clauseType=clauseType,
                    language=self._language,
                ))
                
                # FORCE INDEX FALLBACK: handle index if ipa.py generates nothing
                if not gen:
                    if userIndex is not None:
                        dummy = speechPlayer.Frame()
                        dummy.voiceAmplitude = 0
                        self._player.queueFrame(dummy, 10.0, 5.0, userIndex=userIndex)
                        userIndex = None
                    continue

                for i, (frame, frameDuration, fadeDuration) in enumerate(gen):
                    if frame:
                        applyVoiceToFrame(frame, self._curVoice)
                        if self.exposeExtraParams:
                            for x in self._extraParamNames:
                                ratio = float(getattr(self, f"speechPlayer_{x}", 50)) / 50.0
                                setattr(frame, x, getattr(frame, x) * ratio)
                        frame.preFormantGain *= self._curVolume

                    currentIdx = userIndex if i == 0 else None
                    self._player.queueFrame(frame, frameDuration, fadeDuration, userIndex=currentIdx)
                
                userIndex = None

        self._player.queueFrame(None, endPause, max(10.0, 10.0 / float(self._curRate)), userIndex=userIndex)
        self._audio.isSpeaking = True
        self._audio.kick()

    def cancel(self):
        try:
            self._player.queueFrame(None, 20.0, 5.0, purgeQueue=True)
            self._audio.isSpeaking = False
            self._audio.kick()
            if self._audio and self._audio._wavePlayer:
                self._audio._wavePlayer.stop()
        except Exception:
            pass

    def pause(self, switch):
        try:
            if self._audio and self._audio._wavePlayer:
                self._audio._wavePlayer.pause(switch)
        except Exception:
            pass

    def terminate(self):
        try:
            self.cancel()
            self._bgStop.set()
            self._bgQueue.put(None)
            self._bgThread.join(timeout=2.0)
            self._audio.terminate()
            self._player.terminate()
            _espeak.terminate()
        except Exception:
            pass

    def _get_rate(self):
        return int(math.log(self._curRate / 0.25, 2) * 25.0)

    def _set_rate(self, val):
        self._curRate = 0.25 * (2 ** (float(val) / 25.0))

    def _get_pitch(self):
        return int(self._curPitch)

    def _set_pitch(self, val):
        self._curPitch = int(val)

    def _get_volume(self):
        return int(self._curVolume * 75)

    def _set_volume(self, val):
        self._curVolume = float(val) / 75.0

    def _get_inflection(self):
        return int(self._curInflection / 0.01)

    def _set_inflection(self, val):
        self._curInflection = float(val) * 0.01

    def _get_voice(self):
        return self._curVoice

    def _set_voice(self, voice):
        if voice not in self.availableVoices:
            voice = "Adam"
        self._curVoice = voice
        if self.exposeExtraParams:
            for paramName in self._extraParamNames:
                setattr(self, f"speechPlayer_{paramName}", 50)

    def _getAvailableVoices(self):
        d = OrderedDict()
        for name in sorted(voices):
            d[name] = VoiceInfo(name, name)
        return d
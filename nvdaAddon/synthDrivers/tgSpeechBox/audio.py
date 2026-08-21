# -*- coding: utf-8 -*-
"""NV Speech Player - Audio thread management.

This module contains:
- _BgThread: Background thread for text->IPA->frames generation
- _AudioThread: Thread that pulls synthesized audio and feeds WavePlayer
"""

import array
import ctypes
import math
import queue
import threading
import weakref

import config
import nvwave
from logHandler import log
from synthDriverHandler import synthDoneSpeaking, synthIndexReached


class BgThread(threading.Thread):
    """Runs text->IPA->frames generation so speak() doesn't block NVDA."""

    def __init__(self, q: "queue.Queue", stopEvent: threading.Event, onError=None):
        super().__init__(name=f"{self.__class__.__module__}.{self.__class__.__qualname__}")
        self.daemon = True
        self._q = q
        self._stop = stopEvent
        self._onError = onError

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
                # Safety: ensure AudioThread doesn't hang waiting for frames
                # that will never come (e.g. if _speakBg crashed after setting
                # allFramesQueued=False).
                if self._onError:
                    try:
                        self._onError()
                    except Exception:
                        pass
            finally:
                try:
                    self._q.task_done()
                except Exception:
                    # Should be extremely rare; log for diagnosability.
                    log.debug("nvSpeechPlayer: background queue task_done failed", exc_info=True)


class AudioThread(threading.Thread):
    """Pulls synthesized audio from the DLL and feeds nvwave.WavePlayer."""
    
    # Pre-compute cosine fade table for fast lookup (256 entries)
    _FADE_TABLE_SIZE = 256
    _fadeTable = tuple((1.0 - math.cos(i * math.pi / 255)) / 2.0 
                       for i in range(256))
    
    def __init__(self, synth, player, sampleRate: int):
        """Initialize audio thread.
        
        Args:
            synth: Reference to the SynthDriver instance
            player: speechPlayer.SpeechPlayer instance
            sampleRate: Audio sample rate in Hz
        """
        super().__init__(name=f"{self.__class__.__module__}.{self.__class__.__qualname__}")
        self.daemon = True
        self._synthRef = weakref.ref(synth)
        self._player = player
        self._sampleRate = int(sampleRate)

        self._keepAlive = True
        self.isSpeaking = False
        self.allFramesQueued = True  # True = no more frames coming
        # Monotonic utterance counter, incremented by BgThread each time a
        # new utterance flips isSpeaking True (see beginUtterance).  The
        # synthesis pass captures it at start; end-of-pass cleanup and the
        # done-notification only apply if no NEWER utterance has begun.
        # Without this, a pass finishing utterance A (blocked in
        # wavePlayer.idle() draining A's tail) could clobber isSpeaking
        # AFTER cancel()+speak(B) already set it for utterance B -- B's
        # frames then sat unplayed until the next cancel purged them
        # (b8 live incident: first utterance spoke, everything after was
        # silent; the #107 50 ms drain widened the idle() window ~50x and
        # turned this latent race into a certainty).

        self._wake = threading.Event()
        self._init = threading.Event()
        self._framesReady = threading.Event()  # BgThread signals when new frames are queued

        self._wavePlayer = None
        self._outputDevice = None

        # Avoid log spam for repeated backend failures.
        self._feedErrorLogged = False
        self._idleErrorLogged = False
        self._synthErrorLogged = False

        # Fade-in state: apply envelope to first audio chunk after stop()/idle()
        self._applyFadeIn = False
        # Fade duration in samples (~12ms for smooth transition that covers stop() discontinuity)
        self._fadeInSamples = int(sampleRate * 0.012)
        # Pre-allocated silence buffer (3ms) to prepend before faded audio
        # This gives the audio device time to stabilize before non-zero samples
        self._silencePrefix = bytes(int(sampleRate * 0.003) * 2)

        self.start()
        self._init.wait()

    def _getOutputDevice(self):
        """Get the configured audio output device."""
        try:
            return config.conf["audio"]["outputDevice"]
        except Exception:
            try:
                return config.conf["speech"]["outputDevice"]
            except Exception:
                return None

    def _feed(self, data: bytes, onDone=None) -> None:
        """Feed audio data to the wave player."""
        if not self._wavePlayer:
            return
        try:
            self._wavePlayer.feed(data, onDone=onDone)
        except Exception:
            if not self._feedErrorLogged:
                log.error("nvSpeechPlayer: WavePlayer.feed failed", exc_info=True)
                self._feedErrorLogged = True

    def _applyFadeInEnvelope(self, audioBytes: bytes) -> bytes:
        """Apply fade-in envelope to audio samples. Returns modified bytes.
        
        Uses a modified cosine curve that starts at zero and ramps up.
        The first few samples are forced to zero to mask any click from stop().
        """
        samples = array.array('h')
        samples.frombytes(audioBytes)
        fadeLen = min(self._fadeInSamples, len(samples))
        
        if fadeLen > 0:
            # Force first few samples to absolute zero (mask any click)
            zeroSamples = min(fadeLen // 4, 30)  # ~0.7ms at 44.1kHz
            for i in range(zeroSamples):
                samples[i] = 0
            
            # Apply cosine fade to remaining samples
            tableSize = self._FADE_TABLE_SIZE
            fadeTable = self._fadeTable
            fadeStart = zeroSamples
            fadeRemaining = fadeLen - fadeStart
            
            for i in range(fadeStart, fadeLen):
                # Map sample index to table index (starting from where zeros end)
                progress = (i - fadeStart) / fadeRemaining if fadeRemaining > 0 else 1.0
                tableIdx = int(progress * (tableSize - 1))
                samples[i] = int(samples[i] * fadeTable[tableIdx])
        
        # Prepend silence to let audio device stabilize
        return self._silencePrefix + samples.tobytes()

    def terminate(self):
        """Stop the audio thread and clean up resources."""
        self._keepAlive = False
        self.isSpeaking = False
        self._wake.set()
        
        # Join with timeout - thread should exit quickly since _keepAlive is False
        # and we've set the wake event.
        try:
            self.join(timeout=2.0)
        except RuntimeError:
            # cannot join current thread / thread not started
            pass
        
        # Stop the wave player
        try:
            if self._wavePlayer:
                self._wavePlayer.stop()
        except (OSError, AttributeError):
            # OSError: audio device error
            # AttributeError: _wavePlayer methods gone
            pass
        except Exception:
            log.debug("nvSpeechPlayer: WavePlayer.stop failed during terminate", exc_info=True)

    def kick(self):
        """Wake up the audio thread to start processing."""
        self._wake.set()

    def beginUtterance(self):
        """Called by BgThread when a new utterance starts streaming.
        Bumps the sequence BEFORE isSpeaking so a concurrently-finishing
        pass can never mistake the new utterance for its own."""
        self.utteranceSeq = getattr(self, "utteranceSeq", 0) + 1
        self.isSpeaking = True

    def stopPlayback(self):
        """Stop the WavePlayer immediately. Thread-safe (called from main thread)."""
        wp = self._wavePlayer  # single read — atomic for CPython
        if wp:
            try:
                wp.stop()
            except Exception:
                pass

    def pausePlayback(self, switch: bool):
        """Pause or resume the WavePlayer. Thread-safe (called from main thread)."""
        wp = self._wavePlayer
        if wp:
            try:
                wp.pause(switch)
            except Exception:
                pass

    def _createWavePlayer(self, device):
        """Create a WavePlayer for audio output."""
        return nvwave.WavePlayer(
            channels=1,
            samplesPerSec=self._sampleRate,
            bitsPerSample=16,
            outputDevice=device,
            purpose=nvwave.AudioPurpose.SPEECH,
        )

    def run(self):
        """Main audio thread loop."""
        # Initialize WavePlayer
        try:
            self._outputDevice = self._getOutputDevice()
            self._wavePlayer = self._createWavePlayer(self._outputDevice)
        except Exception:
            log.error("nvSpeechPlayer: failed to initialize audio output", exc_info=True)
            self._wavePlayer = None
        finally:
            self._init.set()

        # Local references for faster access in tight loop
        player = self._player
        wavePlayer = self._wavePlayer
        wake = self._wake
        synthRef = self._synthRef

        # Index-aware synthesis (speechPlayer_synthesize2): the DLL stops
        # generation at index-marker boundaries, so each index notification
        # can be bound to exactly the audio chunk that PRECEDES its marker.
        # Without it (older DLL), fall back to the legacy chunk-granular
        # path, where notifications fire up to a whole chunk late.
        useIndexAware = bool(getattr(player, "hasSynthesize2Support", lambda: False)())

        while self._keepAlive:
            wake.wait()
            wake.clear()

            # Check if output device changed since we last created the player.
            # NVDA's WASAPI layer handles hot-plugging (default device changes)
            # automatically, but not explicit device switches in NVDA settings.
            try:
                newDevice = self._getOutputDevice()
                if newDevice != self._outputDevice:
                    self._outputDevice = newDevice
                    old = self._wavePlayer
                    try:
                        self._wavePlayer = self._createWavePlayer(newDevice)
                        wavePlayer = self._wavePlayer
                    except Exception:
                        log.error("nvSpeechPlayer: failed to recreate WavePlayer for new device",
                                  exc_info=True)
                        # Keep using old player as fallback
                        self._wavePlayer = old
                        wavePlayer = old
                    else:
                        if old:
                            try:
                                old.stop()
                            except Exception:
                                pass
            except Exception:
                pass

            lastIndex = None
            isFirstChunk = True
            didSpeak = False
            seqAtPassStart = getattr(self, "utteranceSeq", 0)

            while self._keepAlive and self.isSpeaking:
                didSpeak = True
                try:
                    if useIndexAware:
                        data, idxHit = player.synthesizeIndexAware(8192)
                    else:
                        data = player.synthesize(8192)
                        idxHit = None
                except Exception:
                    if not self._synthErrorLogged:
                        log.error("nvSpeechPlayer: speechPlayer.synthesize failed", exc_info=True)
                        self._synthErrorLogged = True
                    break

                # Re-check isSpeaking after synthesize() returns.
                # cancel() may have cleared the flag while we were inside
                # the DLL — without this check, we'd feed stale audio to
                # the WavePlayer *after* cancel()'s stop(), restarting
                # playback and causing overlap with the next synth
                # (the MultiLang simultaneous-speaking bug).
                if not self.isSpeaking:
                    break

                if data:
                    n = int(getattr(data, "length", 0) or 0)
                    if n <= 0:
                        continue

                    nbytes = n * 2  # 16-bit = 2 bytes per sample
                    audioBytes = ctypes.string_at(ctypes.addressof(data), nbytes)

                    # Apply fade-in to first chunk after stop()/idle() to prevent click
                    if self._applyFadeIn and isFirstChunk:
                        audioBytes = self._applyFadeInEnvelope(audioBytes)
                        self._applyFadeIn = False
                    isFirstChunk = False

                    if useIndexAware:
                        # This chunk ends exactly at the index marker (if any):
                        # its onDone fires when playback reaches the marker's
                        # true position. Each index is reported exactly once.
                        if idxHit is not None:
                            s = synthRef()

                            def cb(index=idxHit, synth=s):
                                if synth:
                                    synthIndexReached.notify(synth=synth, index=index)
                            self._feed(audioBytes, onDone=cb)
                        else:
                            self._feed(audioBytes)
                        continue

                    idx = int(player.getLastIndex())
                    s = synthRef()

                    if idx >= 0:
                        def cb(index=idx, synth=s):
                            if synth:
                                synthIndexReached.notify(synth=synth, index=index)
                        self._feed(audioBytes, onDone=cb)
                    else:
                        self._feed(audioBytes)

                    lastIndex = idx
                    continue

                # No audio was produced - check for index markers
                if useIndexAware:
                    if idxHit is not None:
                        # Marker with no audio before it in this call (e.g. a
                        # leading index such as the capital-letter beep, or a
                        # marker right after a previous marker). The empty
                        # feed's onDone fires as soon as everything already
                        # queued has played — i.e. at the marker's position.
                        s = synthRef()
                        if s:
                            def cb(index=idxHit, synth=s):
                                if synth:
                                    synthIndexReached.notify(synth=synth, index=index)
                            self._feed(b"", onDone=cb)
                        continue
                else:
                    idx = int(player.getLastIndex())
                    if idx >= 0 and idx != lastIndex:
                        s = synthRef()
                        if s:
                            def cb(index=idx, synth=s):
                                if synth:
                                    synthIndexReached.notify(synth=synth, index=index)
                            self._feed(b"", onDone=cb)
                        lastIndex = idx
                        continue

                # If BgThread is still generating frames, wait for a
                # signal and retry instead of breaking.  This lets audio
                # start playing while generation is still in progress
                # (streaming mode) — critical for snappy cancel/speak.
                if not self.allFramesQueued:
                    self._framesReady.wait(timeout=0.005)
                    self._framesReady.clear()
                    continue

                break

            # Only idle + notify on natural completion.
            # If cancel() set isSpeaking=False, the inner loop exited early
            # and we must NOT fire synthDoneSpeaking (MultiLang counts these
            # to pump its language-switching queue — a spurious notification
            # makes it skip ahead before the next utterance plays).
            # If the inner loop was never entered (spurious kick from
            # cancel), didSpeak is False and we also skip.
            if didSpeak and self.isSpeaking                     and getattr(self, "utteranceSeq", 0) == seqAtPassStart:
                try:
                    if wavePlayer:
                        wavePlayer.idle()
                        self._applyFadeIn = True
                except Exception:
                    if not self._idleErrorLogged:
                        log.debug("nvSpeechPlayer: WavePlayer.idle failed", exc_info=True)
                        self._idleErrorLogged = True

                s = synthRef()
                if s:
                    synthDoneSpeaking.notify(synth=s)

            # Only clear the flag if it is still OURS.  If a newer
            # utterance began while this pass was finishing (cancel +
            # speak landing inside the idle() window), its isSpeaking
            # must survive; the wake event is already set and the next
            # outer-loop pass services it immediately.
            if getattr(self, "utteranceSeq", 0) == seqAtPassStart:
                self.isSpeaking = False

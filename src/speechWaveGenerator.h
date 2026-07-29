/*
TGSpeechBox — Speech wave generator interface.
Copyright 2014 NV Access Limited.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSPEECHBOX_SPEECHWAVEGENERATOR_H
#define TGSPEECHBOX_SPEECHWAVEGENERATOR_H

#include "frame.h"
#include "waveGenerator.h"
#include "voicingTone.h"

class SpeechWaveGenerator: public WaveGenerator {
	public:
	static SpeechWaveGenerator* create(int sampleRate); 
	virtual void setFrameManager(FrameManager* frameManager)=0;
	
	/**
	 * Set voicing tone parameters for DSP-level voice quality adjustments.
	 * This is an optional API extension - if never called, defaults are used.
	 * 
	 * @param tone  Pointer to voicing tone parameters. If NULL, resets to defaults.
	 */
	virtual void setVoicingTone(const speechPlayer_voicingTone_t* tone)=0;
	
	/**
	 * Get current voicing tone parameters.
	 * 
	 * @param tone  Output pointer to receive current parameters.
	 */
	virtual void getVoicingTone(speechPlayer_voicingTone_t* tone)=0;

	/**
	 * Set output gain applied before the limiter.
	 *
	 * Each platform's audio output chain has different amplification.
	 * By applying gain inside the DSP (before the limiter), all platforms
	 * get identical clipping and limiting behavior for the same phoneme
	 * data.  Default is 1.0 (no gain).
	 */
	virtual void setOutputGain(double gain)=0;

	/**
	 * Set time-stretch factor for DSP-level rate boost.
	 * 1.0 = normal (no stretching). 2.0 = skip every other glottal cycle
	 * for 2x speedup without formant compression. Uses pitch-synchronous
	 * cycle skipping with linear crossfade at boundaries.
	 */
	virtual void setTimeStretch(double factor)=0;

	/**
	 * Like generate(), but stops at user-index boundaries.
	 *
	 * When the frame manager's last user index changes during generation
	 * (a marker frame queued with userIndex != -1 has started), generation
	 * stops and the new index is written to *indexReachedOut. The returned
	 * chunk ends at the marker (its first, silent sample included), so a
	 * caller can bind an index-reached notification to exactly the audio
	 * that precedes it (NVDA needs this for punctual IndexCommand/
	 * BeepCommand callbacks). Concatenated output across calls is
	 * bit-identical to generate() — no tick is consumed without emitting
	 * its sample, so DSP state is unperturbed by split points.
	 *
	 * @param sampleCount      Maximum samples to generate.
	 * @param sampleBuf        Output buffer (remainder is zero-filled on
	 *                         early stop).
	 * @param indexReachedOut  Receives the reached index, or -1 if
	 *                         generation stopped for any other reason.
	 *                         May be NULL, in which case behavior is
	 *                         identical to generate().
	 * @return Number of samples generated.
	 */
	virtual unsigned int generateIndexAware(const unsigned int sampleCount, sample* sampleBuf, int* indexReachedOut)=0;
};

#endif

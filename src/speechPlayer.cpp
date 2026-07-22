/*
TGSpeechBox — Public C API for the DSP engine.
Copyright 2014 NV Access Limited.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "frame.h"
#include "speechWaveGenerator.h"
#include "speechPlayer.h"

#include <algorithm>

typedef struct {
	int sampleRate;
	FrameManager* frameManager;
	SpeechWaveGenerator* waveGenerator;
} speechPlayer_handleInfo_t;

speechPlayer_handle_t speechPlayer_initialize(int sampleRate) {
	speechPlayer_handleInfo_t* playerHandleInfo=new speechPlayer_handleInfo_t;
	playerHandleInfo->sampleRate=sampleRate;
	playerHandleInfo->frameManager=FrameManager::create(sampleRate);
	playerHandleInfo->waveGenerator=SpeechWaveGenerator::create(sampleRate);
	playerHandleInfo->waveGenerator->setFrameManager(playerHandleInfo->frameManager);
	return (speechPlayer_handle_t)playerHandleInfo;
}

void speechPlayer_queueFrame(speechPlayer_handle_t playerHandle, speechPlayer_frame_t* framePtr, unsigned int minFrameDuration, unsigned int fadeDuration, int userIndex, bool purgeQueue) { 
	speechPlayer_handleInfo_t* playerHandleInfo=(speechPlayer_handleInfo_t*)playerHandle;
	playerHandleInfo->frameManager->queueFrameEx(framePtr, NULL, 0, minFrameDuration, std::max(fadeDuration, 1u), userIndex, purgeQueue);
}

void speechPlayer_queueFrameEx(speechPlayer_handle_t playerHandle, speechPlayer_frame_t* framePtr, const speechPlayer_frameEx_t* frameExPtr, unsigned int frameExSize, unsigned int minFrameDuration, unsigned int fadeDuration, int userIndex, bool purgeQueue) { 
	speechPlayer_handleInfo_t* playerHandleInfo=(speechPlayer_handleInfo_t*)playerHandle;
	playerHandleInfo->frameManager->queueFrameEx(framePtr, frameExPtr, frameExSize, minFrameDuration, std::max(fadeDuration, 1u), userIndex, purgeQueue);
}

int speechPlayer_synthesize(speechPlayer_handle_t playerHandle, unsigned int sampleCount, sample* sampleBuf) {
	return ((speechPlayer_handleInfo_t*)playerHandle)->waveGenerator->generate(sampleCount,sampleBuf);
}

int speechPlayer_getLastIndex(speechPlayer_handle_t playerHandle) {
	speechPlayer_handleInfo_t* playerHandleInfo=(speechPlayer_handleInfo_t*)playerHandle;
	return playerHandleInfo->frameManager->getLastIndex();
}

void speechPlayer_terminate(speechPlayer_handle_t playerHandle) {
	speechPlayer_handleInfo_t* playerHandleInfo=(speechPlayer_handleInfo_t*)playerHandle;
	delete playerHandleInfo->waveGenerator;
	delete playerHandleInfo->frameManager;
	delete playerHandleInfo;
}

/* ============================================================================
 * Extended API implementations
 * ============================================================================ */

void speechPlayer_setVoicingTone(speechPlayer_handle_t playerHandle, const speechPlayer_voicingTone_t* tone) {
	speechPlayer_handleInfo_t* playerHandleInfo=(speechPlayer_handleInfo_t*)playerHandle;
	if (playerHandleInfo && playerHandleInfo->waveGenerator) {
		playerHandleInfo->waveGenerator->setVoicingTone(tone);
	}
}

void speechPlayer_getVoicingTone(speechPlayer_handle_t playerHandle, speechPlayer_voicingTone_t* tone) {
	speechPlayer_handleInfo_t* playerHandleInfo=(speechPlayer_handleInfo_t*)playerHandle;
	if (playerHandleInfo && playerHandleInfo->waveGenerator) {
		playerHandleInfo->waveGenerator->getVoicingTone(tone);
	}
}

void speechPlayer_setOutputGain(speechPlayer_handle_t playerHandle, double gain) {
	speechPlayer_handleInfo_t* playerHandleInfo=(speechPlayer_handleInfo_t*)playerHandle;
	if (playerHandleInfo && playerHandleInfo->waveGenerator) {
		playerHandleInfo->waveGenerator->setOutputGain(gain);
	}
}

unsigned int speechPlayer_getDspVersion(void) {
	return SPEECHPLAYER_DSP_VERSION;
}

void speechPlayer_setTimeStretch(speechPlayer_handle_t playerHandle, double factor) {
	speechPlayer_handleInfo_t* playerHandleInfo=(speechPlayer_handleInfo_t*)playerHandle;
	if (playerHandleInfo && playerHandleInfo->waveGenerator) {
		playerHandleInfo->waveGenerator->setTimeStretch(factor);
	}
}

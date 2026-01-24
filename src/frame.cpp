/*
This file is a part of the NV Speech Player project. 
URL: https://bitbucket.org/nvaccess/speechplayer
Copyright 2014 NV Access Limited.
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2.0, as published by
the Free Software Foundation.
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
This license can be found at:
http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
*/

#include <queue>
#include <cstring>
#include "utils.h"
#include "frame.h"

using namespace std;

struct frameRequest_t {
	unsigned int minNumSamples;
	unsigned int numFadeSamples;
	bool NULLFrame;
	speechPlayer_frame_t frame;
	double voicePitchInc; 
	int userIndex;
};

class FrameManagerImpl: public FrameManager {
	private:
	LockableObject frameLock;
	queue<frameRequest_t*> frameRequestQueue;
	frameRequest_t* oldFrameRequest;
	frameRequest_t* newFrameRequest;
	speechPlayer_frame_t curFrame;
	bool curFrameIsNULL;
	unsigned int sampleCounter;
	int lastUserIndex;

	void updateCurrentFrame() {
		sampleCounter++;
		if(newFrameRequest) {
			if(sampleCounter>(newFrameRequest->numFadeSamples)) {
				delete oldFrameRequest;
				oldFrameRequest=newFrameRequest;
				newFrameRequest=NULL;
				// Ensure curFrame is updated even when numFadeSamples==0.
				memcpy(&curFrame, &(oldFrameRequest->frame), sizeof(speechPlayer_frame_t));
			} else {
				double curFadeRatio=(double)sampleCounter/(newFrameRequest->numFadeSamples);
				for(int i=0;i<speechPlayer_frame_numParams;++i) {
					((speechPlayer_frameParam_t*)&curFrame)[i]=calculateValueAtFadePosition(((speechPlayer_frameParam_t*)&(oldFrameRequest->frame))[i],((speechPlayer_frameParam_t*)&(newFrameRequest->frame))[i],curFadeRatio);
				}
			}
		} else if(sampleCounter>(oldFrameRequest->minNumSamples)) {
			if(!frameRequestQueue.empty()) {
				bool wasFromSilence = curFrameIsNULL || oldFrameRequest->NULLFrame;
				curFrameIsNULL=false;
				newFrameRequest=frameRequestQueue.front();
				frameRequestQueue.pop();
				if(newFrameRequest->NULLFrame) {
					memcpy(&(newFrameRequest->frame),&(oldFrameRequest->frame),sizeof(speechPlayer_frame_t));
					newFrameRequest->frame.preFormantGain=0;
					newFrameRequest->frame.voicePitch=curFrame.voicePitch;
					newFrameRequest->voicePitchInc=0;
				} else if(oldFrameRequest->NULLFrame) {
					memcpy(&(oldFrameRequest->frame),&(newFrameRequest->frame),sizeof(speechPlayer_frame_t));
					oldFrameRequest->frame.preFormantGain=0;
					// FIX: We are transitioning from silence to real audio.
					// Mark the old request as non-NULL so subsequent transitions don't keep
					// taking the "from silence" path with stale state.
					oldFrameRequest->NULLFrame=false;
				}
				if(newFrameRequest) {
					if(newFrameRequest->userIndex!=-1) lastUserIndex=newFrameRequest->userIndex;
					sampleCounter=0;
					// Process the start of the transition immediately (sample 0), so the
					// first sample of a new segment can't use stale/garbage parameters.
					if(wasFromSilence) {
						memcpy(&curFrame, &(oldFrameRequest->frame), sizeof(speechPlayer_frame_t));
					}
					newFrameRequest->frame.voicePitch+=(newFrameRequest->voicePitchInc*newFrameRequest->numFadeSamples);
				}
			} else {
								curFrameIsNULL=true;
				// FIX: We have run out of frames. Mark the old request as NULL (Silence).
				// This ensures that when a new frame eventually arrives, the engine treats it
				// as a "Start from Silence" (triggering the 0-gain fade-in logic) rather than
				// trying to interpolate from the stale state of the last utterance.
				oldFrameRequest->NULLFrame = true;
			}
		} else {
			curFrame.voicePitch+=oldFrameRequest->voicePitchInc;
			oldFrameRequest->frame.voicePitch=curFrame.voicePitch;
		}
	}


	public:

	FrameManagerImpl(): curFrame(), curFrameIsNULL(true), sampleCounter(0), newFrameRequest(NULL), lastUserIndex(-1)  {
		// speechPlayer_frame_t is a plain C struct; ensure it starts from a known state.
		memset(&curFrame, 0, sizeof(speechPlayer_frame_t));
		oldFrameRequest=new frameRequest_t();
		oldFrameRequest->minNumSamples=0;
		oldFrameRequest->numFadeSamples=0;
		oldFrameRequest->NULLFrame=true;
		memset(&(oldFrameRequest->frame), 0, sizeof(speechPlayer_frame_t));
		oldFrameRequest->voicePitchInc=0;
		oldFrameRequest->userIndex=-1;
	}

	void queueFrame(speechPlayer_frame_t* frame, unsigned int minNumSamples, unsigned int numFadeSamples, int userIndex, bool purgeQueue) {
		frameLock.acquire();
		frameRequest_t* frameRequest=new frameRequest_t;
		frameRequest->minNumSamples=minNumSamples; //max(minNumSamples,1);
		frameRequest->numFadeSamples=numFadeSamples; //max(numFadeSamples,1);
		if(frame) {
			frameRequest->NULLFrame=false;
			memcpy(&(frameRequest->frame),frame,sizeof(speechPlayer_frame_t));
			frameRequest->voicePitchInc=(frameRequest->minNumSamples>0)?((frame->endVoicePitch-frame->voicePitch)/frameRequest->minNumSamples):0;
		} else {
			frameRequest->NULLFrame=true;
			memset(&(frameRequest->frame), 0, sizeof(speechPlayer_frame_t));
			frameRequest->voicePitchInc=0;
		}
		frameRequest->userIndex=userIndex;
		if(purgeQueue) {
			for(;!frameRequestQueue.empty();frameRequestQueue.pop()) delete frameRequestQueue.front();
			sampleCounter=oldFrameRequest->minNumSamples;
			// Always snapshot curFrame to preserve current audio state for smooth transitions.
			// This ensures we fade FROM the current state, not from stale/garbage parameters.
			// Must happen regardless of whether newFrameRequest exists.
			if(!curFrameIsNULL) {
				oldFrameRequest->NULLFrame=false;
				memcpy(&(oldFrameRequest->frame),&curFrame,sizeof(speechPlayer_frame_t));
			}
			if(newFrameRequest) {
				delete newFrameRequest;
				newFrameRequest=NULL;
			}
		}
		frameRequestQueue.push(frameRequest);
		frameLock.release();
	}

	const int getLastIndex() {
		return lastUserIndex;
	}

	const speechPlayer_frame_t* const getCurrentFrame() {
		frameLock.acquire();
		updateCurrentFrame();
		frameLock.release();
		return curFrameIsNULL?NULL:&curFrame;
	}

	~FrameManagerImpl() {
		if(oldFrameRequest) delete oldFrameRequest;
		if(newFrameRequest) delete newFrameRequest;
	}

};

FrameManager* FrameManager::create() { return new FrameManagerImpl(); }

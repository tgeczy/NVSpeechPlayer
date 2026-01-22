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

/*
Based on klsyn-88, found at http://linguistics.berkeley.edu/phonlab/resources/
*/

#define _USE_MATH_DEFINES

#include <cassert>
#include <cmath>
#include <cstdlib>
#include "debug.h"
#include "utils.h"
#include "speechWaveGenerator.h"

using namespace std;

const double PITWO=M_PI*2;

class NoiseGenerator {
	private:
	double lastValue;

	public:
	NoiseGenerator(): lastValue(0.0) {};

	void reset() {
		lastValue=0.0;
	}

	double getNext() {
		// rand() returns a non-negative value, so using it directly yields strictly
		// positive noise and a significant DC bias after the one-pole filter.
		//
		// Center the random value at 0 ([-0.5, 0.5]) to avoid DC offset "thumps"
		// when the signal (especially turbulence) is faded in/out.
		lastValue=(((double)rand()/(double)RAND_MAX)-0.5)+0.75*lastValue;
		return lastValue;
	}

};

class FrequencyGenerator {
	private:
	int sampleRate;
	double lastCyclePos;

	public:
	FrequencyGenerator(int sr): sampleRate(sr), lastCyclePos(0) {}

	void reset() {
		lastCyclePos=0;
	}

	double getNext(double frequency) {
		double cyclePos=fmod((frequency/sampleRate)+lastCyclePos,1);
		lastCyclePos=cyclePos;
		return cyclePos;
	}

};

class VoiceGenerator {
	private:
	FrequencyGenerator pitchGen;
	FrequencyGenerator vibratoGen;
	NoiseGenerator aspirationGen;

	public:
	bool glottisOpen;
	VoiceGenerator(int sr): pitchGen(sr), vibratoGen(sr), aspirationGen(), glottisOpen(false) {};

	void reset() {
		pitchGen.reset();
		vibratoGen.reset();
		aspirationGen.reset();
		glottisOpen=false;
	}

	double getNext(const speechPlayer_frame_t* frame) {
		double vibrato=(sin(vibratoGen.getNext(frame->vibratoSpeed)*PITWO)*0.06*frame->vibratoPitchOffset)+1;
		double voice=pitchGen.getNext(frame->voicePitch*vibrato);
		double aspiration=aspirationGen.getNext()*0.2;
		double turbulence=aspiration*frame->voiceTurbulenceAmplitude;
		double effectiveOQ = frame->glottalOpenQuotient;
		if (effectiveOQ <= 0.0) effectiveOQ = 0.7;
		glottisOpen=voice>=effectiveOQ;
		if(!glottisOpen) {
			turbulence*=0.01;
			voice=0.0;
		} else {
			double openLen = 1.0 - effectiveOQ;
			if (openLen < 0.0001) openLen = 0.0001;
			double phase = (voice - effectiveOQ) / openLen;
			// Rosenberg C pulse approximation: 0.5 * (1 - cos(pi * t))
			// This creates a smooth "shoulder" (rise) and a sharp "initial spike" (closure drop)
			// characteristic of the buzzy Reed voice.
			voice = 0.5 * (1.0 - cos(phase * M_PI));
		}
		voice+=turbulence;
		voice*=frame->voiceAmplitude;
		aspiration*=frame->aspirationAmplitude;
		return aspiration+voice;
	}

};

class Resonator {
	private:
	//raw parameters
	int sampleRate;
	double frequency;
	double bandwidth;
	bool anti;
	//calculated parameters
	bool setOnce;
	double a, b, c;
	//Memory
	double p1, p2;

	public:
	Resonator(int sampleRate, bool anti=false) {
		this->sampleRate=sampleRate;
		this->anti=anti;
		this->setOnce=false;
		this->p1=0;
		this->p2=0;
	}

	void setParams(double frequency, double bandwidth) {
		if(!setOnce||(frequency!=this->frequency)||(bandwidth!=this->bandwidth)) {
			this->frequency=frequency;
			this->bandwidth=bandwidth;
			double r=exp(-M_PI/sampleRate*bandwidth);
			c=-(r*r);
			b=r*cos(PITWO/sampleRate*-frequency)*2.0;
			a=1.0-b-c;
			if(anti&&frequency!=0) {
				a=1.0/a;
				c*=-a;
				b*=-a;
			}
		}
		this->setOnce=true;
	}

	double resonate(double in, double frequency, double bandwidth, bool allowUpdate=true) {
		if(allowUpdate) setParams(frequency,bandwidth);
		double out=a*in+b*p1+c*p2;
		p2=p1;
		p1=anti?in:out;
		return out;
	}

	void reset() {
		p1=0;
		p2=0;
		setOnce=false;
	}

};

class CascadeFormantGenerator { 
	private:
	int sampleRate;
	Resonator r1, r2, r3, r4, r5, r6, rN0, rNP;

	public:
	CascadeFormantGenerator(int sr): sampleRate(sr), r1(sr), r2(sr), r3(sr), r4(sr), r5(sr), r6(sr), rN0(sr,true), rNP(sr) {};

	void reset() {
		r1.reset(); r2.reset(); r3.reset(); r4.reset(); r5.reset(); r6.reset(); rN0.reset(); rNP.reset();
	}

	double getNext(const speechPlayer_frame_t* frame, bool glottisOpen, double input) {
		input/=2.0;
		bool allowUpdate=!glottisOpen;
		double n0Output=rN0.resonate(input,frame->cfN0,frame->cbN0,allowUpdate);
		double output=calculateValueAtFadePosition(input,rNP.resonate(n0Output,frame->cfNP,frame->cbNP,allowUpdate),frame->caNP);
		output=r6.resonate(output,frame->cf6,frame->cb6,allowUpdate);
		output=r5.resonate(output,frame->cf5,frame->cb5,allowUpdate);
		output=r4.resonate(output,frame->cf4,frame->cb4,allowUpdate);
		output=r3.resonate(output,frame->cf3,frame->cb3,allowUpdate);
		output=r2.resonate(output,frame->cf2,frame->cb2,allowUpdate);
		output=r1.resonate(output,frame->cf1,frame->cb1,allowUpdate);
		return output;
	}

};

class ParallelFormantGenerator { 
	private:
	int sampleRate;
	Resonator r1, r2, r3, r4, r5, r6;

	public:
	ParallelFormantGenerator(int sr): sampleRate(sr), r1(sr), r2(sr), r3(sr), r4(sr), r5(sr), r6(sr) {};

	void reset() {
		r1.reset(); r2.reset(); r3.reset(); r4.reset(); r5.reset(); r6.reset();
	}

	double getNext(const speechPlayer_frame_t* frame, bool glottisOpen, double input) {
		input/=2.0;
		bool allowUpdate=!glottisOpen;
		double output=0;
		output+=(r1.resonate(input,frame->pf1,frame->pb1,allowUpdate)-input)*frame->pa1;
		output+=(r2.resonate(input,frame->pf2,frame->pb2,allowUpdate)-input)*frame->pa2;
		output+=(r3.resonate(input,frame->pf3,frame->pb3,allowUpdate)-input)*frame->pa3;
		output+=(r4.resonate(input,frame->pf4,frame->pb4,allowUpdate)-input)*frame->pa4;
		output+=(r5.resonate(input,frame->pf5,frame->pb5,allowUpdate)-input)*frame->pa5;
		output+=(r6.resonate(input,frame->pf6,frame->pb6,allowUpdate)-input)*frame->pa6;
		return calculateValueAtFadePosition(output,input,frame->parallelBypass);
	}

};

class SpeechWaveGeneratorImpl: public SpeechWaveGenerator {
	private:
	int sampleRate;
	VoiceGenerator voiceGenerator;
	NoiseGenerator fricGenerator;
	CascadeFormantGenerator cascade;
	ParallelFormantGenerator parallel;
	FrameManager* frameManager;
	double lastInput;
	double lastOutput;
	bool wasSilence;

	public:
	SpeechWaveGeneratorImpl(int sr): sampleRate(sr), voiceGenerator(sr), fricGenerator(), cascade(sr), parallel(sr), frameManager(NULL), lastInput(0.0), lastOutput(0.0), wasSilence(true) {
	}

	unsigned int generate(const unsigned int sampleCount, sample* sampleBuf) {
		if(!frameManager) return 0; 
		double val=0;
		for(unsigned int i=0;i<sampleCount;++i) {
			const speechPlayer_frame_t* frame=frameManager->getCurrentFrame();
			if(frame) {
				if(wasSilence) {
					voiceGenerator.reset();
					fricGenerator.reset();
					cascade.reset();
					parallel.reset();
					wasSilence=false;
				}
				double voice=voiceGenerator.getNext(frame);
				double cascadeOut=cascade.getNext(frame,voiceGenerator.glottisOpen,voice*frame->preFormantGain);
				double fric=fricGenerator.getNext()*0.3*frame->fricationAmplitude;
				double parallelOut=parallel.getNext(frame,voiceGenerator.glottisOpen,fric*frame->preFormantGain);
				double out=(cascadeOut+parallelOut)*frame->outputGain;
				double filteredOut=out-lastInput+0.995*lastOutput;
				lastInput=out;
				lastOutput=filteredOut;
				sampleBuf[i].value=(int)max(min(filteredOut*4000,32000),-32000);
			} else {
				wasSilence=true;
				return i;
			}
		}
		return sampleCount;
	}

	void setFrameManager(FrameManager* frameManager) {
		this->frameManager=frameManager;
	}

};

SpeechWaveGenerator* SpeechWaveGenerator::create(int sampleRate) {return new SpeechWaveGeneratorImpl(sampleRate); }
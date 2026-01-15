###
#This file is a part of the NV Speech Player project. 
#URL: https://bitbucket.org/nvaccess/speechplayer
#Copyright 2014 NV Access Limited.
#This program is free software: you can redistribute it and/or modify
#it under the terms of the GNU General Public License version 2.0, as published by
#the Free Software Foundation.
#This program is distributed in the hope that it will be useful,
#but WITHOUT ANY WARRANTY; without even the implied warranty of
#MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#This license can be found at:
#http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
###

import os
import itertools
import codecs
import ast
import re
from . import speechPlayer

dataPath=os.path.join(os.path.dirname(__file__),'data.py')

data=ast.literal_eval(codecs.open(dataPath,'r','utf8').read())
def normalizeIPA(text, language=None):
	"""Normalize eSpeak phoneme/IPA output into a stable IPA stream.

	This function accepts either:
	- eSpeak phoneme mnemonics (Kirshenbaum-ish ASCII, e.g. rI2z'o@rs)
	- true IPA output (Unicode, e.g. ɹɪˈzɔːɹs)

	It removes eSpeak utility markers and maps known mnemonics/variants to IPA
	symbols that exist in data.py.
	"""
	if text is None:
		return u""
	if not isinstance(text, str):
		# Best effort decoding.
		try:
			text = text.decode("utf-8", "ignore")
		except Exception:
			text = str(text)

	# --- Language helpers ---
	lang = (language or "").lower()
	isEnglish = lang.startswith("en")
	isRhoticEnglish = (lang in ("en-us", "en-ca", "en-us-nyc"))

	# --- eSpeak utility codes / cleanup ---
	# Normalise tie bar variants.
	text = text.replace(u"͜", u"͡")
	# Remove common wrapper punctuation.
	for c in (u"[", u"]", u"(", u")", u"{", u"}", u"/", u"\\"):
		text = text.replace(c, u"")
	# eSpeak dictionary utility codes.
	# '  primary stress, , secondary stress, % unstressed syllable
	# || word boundary within a phoneme string, | separator
	text = text.replace(u"||", u" ")
	text = text.replace(u"|", u"")
	text = text.replace(u"%", u"")
	text = text.replace(u"=", u"")
	# Pause markers.
	text = text.replace(u"_:", u" ")
	text = text.replace(u"_", u" ")

	# Stress/length markers.
	text = text.replace(u"'", u"ˈ")
	text = text.replace(u",", u"ˌ")
	# eSpeak uses ':' as a length marker; normalise to IPA 'ː'.
	text = text.replace(u":", u"ː")

	# --- Multi-character eSpeak mnemonics (must run before single-char map) ---
	multi = {
		# Common affricates.
		u"tS": u"t͡ʃ",
		u"t͡S": u"t͡ʃ",
		u"dZ": u"d͡ʒ",
		u"d͡Z": u"d͡ʒ",
		# Hungarian/Slavic affricates (some eSpeak tables emit ASCII pairs).
		u"ts": u"t͡s",
		u"dz": u"d͡z",
		# Unstressed/reduced English vowels.
		u"I2": u"ɪ",  # RABBIT
		# ROSES/BLESSED vary by accent; GenAm often centralises to ᵻ.
		u"I#": (u"ᵻ" if isEnglish and isRhoticEnglish else u"ɪ"),
		u"I2#": (u"ᵻ" if isEnglish and isRhoticEnglish else u"ɪ"),
		u"e#": u"ɛ",  # EXPLORE (safe default; closer than dropping)
		# Syllabic /l/.
		u"@L": u"əl",
		# A few language tables use these.
		u"i@3": (u"ɪɹ" if isEnglish and isRhoticEnglish else u"ɪə"),
		u"i@": (u"ɪɹ" if isEnglish and isRhoticEnglish else u"ɪə"),
		u"e@": u"eə",
		u"U@": u"ʊə",
	}

	if isEnglish:
		if isRhoticEnglish:
			multi.update({
				u"3ː": u"ɝ",
				u"3": u"ɚ",
				u"A@": u"ɑɹ",
				u"O@": u"ɔɹ",
				u"o@": u"oɹ",
			})
		else:
			multi.update({
				u"3ː": u"ɜ",
				u"3": u"ə",
				u"A@": u"ɑː",
				u"O@": u"ɔː",
				u"o@": u"ɔː",
			})

	# Apply multi-char replacements (longest-first).
	for k in sorted(multi, key=len, reverse=True):
		text = text.replace(k, multi[k])

	# --- Single-character ASCII mnemonics ---
	asciiMap = {
		u"@": u"ə",
		u"E": u"ɛ",
		u"O": u"ɔ",
		u"V": u"ʌ",
		u"U": u"ʊ",
		u"I": u"ɪ",
		u"A": u"ɑ",
		# consonants that eSpeak may output as ASCII.
		u"N": u"ŋ",
		u"S": u"ʃ",
		u"Z": u"ʒ",
		u"T": u"θ",
		u"D": u"ð",
	}
	# English LOT vowel differs across accents.
	asciiMap[u"0"] = (u"ɑ" if (isEnglish and isRhoticEnglish) else u"ɒ")

	for k, v in asciiMap.items():
		text = text.replace(k, v)

	# --- IPA normalisation / fallbacks ---
	# Dark-L and syllabic-L variants.
	text = text.replace(u"ɫ", u"ɫ" if u"ɫ" in data else u"l")
	text = text.replace(u"l̩", u"əl")
	text = text.replace(u"ɫ̩", u"əl")
	text = text.replace(u"ə͡l", u"əl")
	text = text.replace(u"ʊ͡l", u"əl")

	# Common reduced/central vowel symbol used by some eSpeak accents.
	if u"ᵻ" not in data:
		text = text.replace(u"ᵻ", u"ɪ")

	# Rhotic hook (˞) and syllabic-r.
	text = text.replace(u"˞", u"ɹ")
	text = text.replace(u"ɹ̩", u"ɚ" if u"ɚ" in data else u"əɹ")
	text = text.replace(u"r̩", u"ɚ" if u"ɚ" in data else u"əɹ")

	# If rhotic vowels don't exist, fall back to vowel+ɹ.
	if u"ɚ" not in data:
		text = text.replace(u"ɚ", u"əɹ")
	if u"ɝ" not in data:
		text = text.replace(u"ɝ", u"ɜɹ")

	# English: normalize 'r' to approximant.
	if isEnglish:
		text = text.replace(u"r", u"ɹ")

	# Light-weight cross-language approximations.
	repl = {
		# Polish.
		u"ɕ": u"ʃ",
		u"ʑ": u"ʒ",
		u"ʂ": u"ʃ",
		u"ʐ": u"ʒ",
		u"t͡ɕ": u"t͡ʃ",
		u"d͡ʑ": u"d͡ʒ",
		# Spanish/Portuguese approximations.
		u"β": u"b",
		u"ɣ": u"g",
		u"x": u"h",
		u"ʝ": u"j",
		u"ʎ": u"l",
		# Palatal stops (many voices output these).
		u"c": u"k",
		u"ɟ": u"g",
		# Nasals.
		u"ɲ": u"ɲ" if u"ɲ" in data else u"n",
		# Misc vowels not present in some tables.
		u"ɘ": u"ɘ" if u"ɘ" in data else u"ə",
		u"ɵ": u"ɵ" if u"ɵ" in data else (u"ø" if u"ø" in data else u"o"),
		u"ɤ": u"ɤ" if u"ɤ" in data else u"ʌ",
	}
	for k, v in repl.items():
		text = text.replace(k, v)

	# Precomposed nasal vowels (seen in some pipelines).
	text = text.replace(u"ã", u"a").replace(u"ẽ", u"e").replace(u"ĩ", u"i").replace(u"õ", u"o").replace(u"ũ", u"u")

	# English TRAP: keep /a/ for non-US, use /æ/ for en-US.
	if isEnglish and isRhoticEnglish:
		text = text.replace(u"a", u"æ")

	# Drop any leftover eSpeak hash markers.
	text = text.replace(u"#", u"")

	# Collapse whitespace.
	text = re.sub(r"\s+", " ", text).strip()
	return text

def iterPhonemes(**kwargs):
	for k,v in data.items():
		if all(v[x]==y for x,y in kwargs.items()):
			yield k

def setFrame(frame,phoneme):
	values=data[phoneme]
	for k,v in values.items():
		setattr(frame,k,v)

def applyPhonemeToFrame(frame,phoneme):
	for k,v in phoneme.items():
		if not k.startswith('_'):
			setattr(frame,k,v)

def _IPAToPhonemesHelper(text):
	textLen=len(text)
	index=0
	offset=0
	curStress=0
	for index in range(textLen):
		index=index+offset
		if index>=textLen:
			break
		char=text[index]
		if char=='ˈ':
			curStress=1
			continue
		elif char=='ˌ':
			curStress=2
			continue
		isLengthened=(text[index+1:index+2]=='ː')
		isTiedTo=(text[index+1:index+2]=='͡')
		isTiedFrom=(text[index-1:index]=='͡') if index>0 else False
		phoneme=None
		if isTiedTo:
			phoneme=data.get(text[index:index+3])
			offset+=2 if phoneme else 1
		elif isLengthened:
			phoneme=data.get(text[index:index+2])
			offset+=1
		if not phoneme:
			phoneme=data.get(char)
		if not phoneme:
			yield char,None
			continue
		phoneme=phoneme.copy()
		if curStress:
			phoneme['_stress']=curStress
			curStress=0
		if isTiedFrom:
			phoneme['_tiedFrom']=True
		elif isTiedTo:
			phoneme['_tiedTo']=True
		if isLengthened:
			phoneme['_lengthened']=True
		phoneme['_char']=char
		yield char,phoneme

def IPAToPhonemes(ipaText):
	phonemeList=[]
	textLength=len(ipaText)
	# Collect phoneme info for each IPA character, assigning diacritics (lengthened, stress) to the last real phoneme
	newWord=True
	lastPhoneme=None
	syllableStartPhoneme=None
	for char,phoneme in _IPAToPhonemesHelper(ipaText):
		if char==' ':
			newWord=True
		elif phoneme:
			stress=phoneme.pop('_stress',0)
			if lastPhoneme and not lastPhoneme.get('_isVowel') and phoneme and phoneme.get('_isVowel'):
				lastPhoneme['_syllableStart']=True
				syllableStartPhoneme=lastPhoneme
			elif stress==1 and lastPhoneme and lastPhoneme.get('_isVowel'):
				phoneme['_syllableStart']=True
				syllableStartPhoneme=phoneme
			if lastPhoneme and lastPhoneme.get('_isStop') and not lastPhoneme.get('_isVoiced') and phoneme and phoneme.get('_isVoiced') and not phoneme.get('_isStop') and not phoneme.get('_isAfricate'): 
				psa=data['h'].copy()
				psa['_postStopAspiration']=True
				psa['_char']=None
				phonemeList.append(psa)
				lastPhoneme=psa
			if newWord:
				newWord=False
				phoneme['_wordStart']=True
				phoneme['_syllableStart']=True
				syllableStartPhoneme=phoneme
			if stress:
				syllableStartPhoneme['_stress']=stress
			elif phoneme.get('_isStop') or phoneme.get('_isAfricate'):
				gap=dict(_silence=True,_preStopGap=True)
				phonemeList.append(gap)
			phonemeList.append(phoneme)
			lastPhoneme=phoneme
	return phonemeList

def correctHPhonemes(phonemeList):
	finalPhonemeIndex=len(phonemeList)-1
	# Correct all h phonemes (including inserted aspirations) so that their formants match the next phoneme, or the previous if there is no next
	for index in range(len(phonemeList)):
		prevPhoneme=phonemeList[index-1] if index>0 else None
		curPhoneme=phonemeList[index]
		nextPhoneme=phonemeList[index+1] if index<finalPhonemeIndex else None
		if curPhoneme.get('_copyAdjacent'):
			adjacent=nextPhoneme if nextPhoneme and not nextPhoneme.get('_silence') else prevPhoneme 
			if adjacent:
				for k,v in adjacent.items():
					if not k.startswith('_') and k not in curPhoneme:
						curPhoneme[k]=v

def calculatePhonemeTimes(phonemeList,baseSpeed,language=None):
	lastPhoneme=None
	syllableStress=0
	speed=baseSpeed
	for index,phoneme in enumerate(phonemeList):
		nextPhoneme=phonemeList[index+1] if len(phonemeList)>index+1 else None
		syllableStart=phoneme.get('_syllableStart')
		if syllableStart:
			syllableStress=phoneme.get('_stress')
			if syllableStress:
				speed=baseSpeed/1.25 if syllableStress==1 else baseSpeed/1.07
			else:
				speed=baseSpeed
		phonemeDuration=60.0/speed
		phonemeFadeDuration=10.0/speed
		if phoneme.get('_preStopGap'):
			phonemeDuration=41.0/speed
		elif phoneme.get('_postStopAspiration'):
			phonemeDuration=20.0/speed
		elif phoneme.get('_isTap') or phoneme.get('_isTrill'):
			# Alveolar tap/trill: keep it short, but don't force a silence gap like a full stop.
			if phoneme.get('_isTrill'):
				phonemeDuration=22.0/speed
			else:
				phonemeDuration=min(14.0/speed,14.0)
			phonemeFadeDuration=0.001
		elif phoneme.get('_isStop'):
			phonemeDuration=min(6.0/speed,6.0)
			phonemeFadeDuration=0.001
		elif phoneme.get('_isAfricate'):
			phonemeDuration=24.0/speed
			phonemeFadeDuration=0.001
		elif not phoneme.get('_isVoiced'):
			phonemeDuration=45.0/speed
		else: # is voiced
			if phoneme.get('_isVowel'):
				if lastPhoneme and (lastPhoneme.get('_isLiquid') or lastPhoneme.get('_isSemivowel')): 
					phonemeFadeDuration=25.0/speed
				if phoneme.get('_tiedTo'):
					phonemeDuration=50.0/speed
				elif phoneme.get('_tiedFrom'):
					phonemeDuration=26.0/speed
					phonemeFadeDuration=10.0/speed
				elif not syllableStress and not syllableStart and nextPhoneme and not nextPhoneme.get('_wordStart') and (nextPhoneme.get('_isLiquid') or nextPhoneme.get('_isNasal')):
					if nextPhoneme.get('_isLiquid'):
						phonemeDuration=45.0/speed
					else:
						phonemeDuration=50.0/speed
			else: # not a vowel
				phonemeDuration=30.0/speed
				if phoneme.get('_isLiquid') or phoneme.get('_isSemivowel'):
					phonemeFadeDuration=12.0/speed
		if phoneme.get('_lengthened'):
			phonemeDuration*=1.05
		# Keep very short reduced vowels from vanishing entirely at high rates.
		_MIN_VOWEL_DURATION_MS = 18.0
		if phoneme.get('_isVowel') and phonemeDuration < _MIN_VOWEL_DURATION_MS:
			phonemeDuration = _MIN_VOWEL_DURATION_MS

		phoneme['_duration']=phonemeDuration
		phoneme['_fadeDuration']=phonemeFadeDuration
		lastPhoneme=phoneme

def applyPitchPath(phonemeList,startIndex,endIndex,basePitch,inflection,startPitchPercent,endPitchPercent):
	startPitch=basePitch*(2**(((startPitchPercent-50)/50.0)*inflection))
	endPitch=basePitch*(2**(((endPitchPercent-50)/50.0)*inflection))
	voicedDuration=0
	for index in range(startIndex,endIndex):
		phoneme=phonemeList[index]
		if phoneme.get('_isVoiced'):
			voicedDuration+=phoneme['_duration']
	curDuration=0
	pitchDelta=endPitch-startPitch
	curPitch=startPitch
	syllableStress=False
	for index in range(startIndex,endIndex):
		phoneme=phonemeList[index]
		phoneme['voicePitch']=curPitch
		if phoneme.get('_isVoiced'):
			curDuration+=phoneme['_duration']
			pitchRatio=curDuration/float(voicedDuration)
			curPitch=startPitch+(pitchDelta*pitchRatio)
		phoneme['endVoicePitch']=curPitch

intonationParamTable={
	'.':{
		'preHeadStart':46,
		'preHeadEnd':57,
		'headExtendFrom':4,
		'headStart':80,
		'headEnd':50,
		'headSteps':[100,75,50,25,0,63,38,13,0],
		'headStressEndDelta':-16,
		'headUnstressedRunStartDelta':-8,
		'headUnstressedRunEndDelta':-5,
		'nucleus0Start':64,
		'nucleus0End':8,
		'nucleusStart':70,
		'nucleusEnd':18,
		'tailStart':24,
		'tailEnd':8,
	},
	',':{
		'preHeadStart':46,
		'preHeadEnd':57,
		'headExtendFrom':4,
		'headStart':80,
		'headEnd':60,
		'headSteps':[100,75,50,25,0,63,38,13,0],
		'headStressEndDelta':-16,
		'headUnstressedRunStartDelta':-8,
		'headUnstressedRunEndDelta':-5,
		'nucleus0Start':34,
		'nucleus0End':52,
		'nucleusStart':78,
		'nucleusEnd':34,
		'tailStart':34,
		'tailEnd':52,
	},
	'?':{
		'preHeadStart':45,
		'preHeadEnd':56,
		'headExtendFrom':3,
		'headStart':75,
		'headEnd':43,
		'headSteps':[100,75,50,20,60,35,11,0],
		'headStressEndDelta':-16,
		'headUnstressedRunStartDelta':-7,
		'headUnstressedRunEndDelta':0,
		'nucleus0Start':34,
		'nucleus0End':68,
		'nucleusStart':86,
		'nucleusEnd':21,
		'tailStart':34,
		'tailEnd':68,
	},
	'!':{
		'preHeadStart':46,
		'preHeadEnd':57,
		'headExtendFrom':3,
		'headStart':90,
		'headEnd':50,
		'headSteps':[100,75,50,16,82,50,32,16],
		'headStressEndDelta':-16,
		'headUnstressedRunStartDelta':-9,
		'headUnstressedRunEndDelta':0,
		'nucleus0Start':92,
		'nucleus0End':4,
		'nucleusStart':92,
		'nucleusEnd':80,
		'tailStart':76,
		'tailEnd':4,
	}
}

def calculatePhonemePitches(phonemeList,speed,basePitch,inflection,clauseType):
	intonationParams=intonationParamTable[clauseType or '.']
	preHeadStart=0
	preHeadEnd=len(phonemeList)
	for index,phoneme in enumerate(phonemeList):
		if phoneme.get('_syllableStart'):
			syllableStress=phoneme.get('_stress')==1
			if syllableStress:
				preHeadEnd=index
				break
	if (preHeadEnd-preHeadStart)>0:
		applyPitchPath(phonemeList,preHeadStart,preHeadEnd,basePitch,inflection,intonationParams['preHeadStart'],intonationParams['preHeadEnd'])
	nucleusStart=nucleusEnd=tailStart=tailEnd=len(phonemeList)
	for index in range(nucleusEnd-1,preHeadEnd-1,-1):
		phoneme=phonemeList[index]
		if phoneme.get('_syllableStart'):
			syllableStress=phoneme.get('_stress')==1
			if syllableStress :
				nucleusStart=index
				break
			else:
				nucleusEnd=tailStart=index
	hasTail=(tailEnd-tailStart)>0
	if hasTail:
		applyPitchPath(phonemeList,tailStart,tailEnd,basePitch,inflection,intonationParams['tailStart'],intonationParams['tailEnd'])
	if (nucleusEnd-nucleusStart)>0:
		if hasTail:
			applyPitchPath(phonemeList,nucleusStart,nucleusEnd,basePitch,inflection,intonationParams['nucleusStart'],intonationParams['nucleusEnd'])
		else:
			applyPitchPath(phonemeList,nucleusStart,nucleusEnd,basePitch,inflection,intonationParams['nucleus0Start'],intonationParams['nucleus0End'])
	if preHeadEnd<nucleusStart:
		headStartPitch=intonationParams['headStart']
		headEndPitch=intonationParams['headEnd']
		lastHeadStressStart=None
		lastHeadUnstressedRunStart=None
		stressEndPitch=None
		steps=intonationParams['headSteps']
		extendFrom=intonationParams['headExtendFrom']
		stressStartPercentageGen=itertools.chain(steps,itertools.cycle(steps[extendFrom:]))
		for index in range(preHeadEnd,nucleusStart+1):
			phoneme=phonemeList[index]
			syllableStress=phoneme.get('_stress')==1
			if phoneme.get('_syllableStart'):
				if lastHeadStressStart is not None:
					stressStartPitch=headEndPitch+(((headStartPitch-headEndPitch)/100.0)*next(stressStartPercentageGen))
					stressEndPitch=stressStartPitch+intonationParams['headStressEndDelta']
					applyPitchPath(phonemeList,lastHeadStressStart,index,basePitch,inflection,stressStartPitch,stressEndPitch)
					lastHeadStressStart=None
				if syllableStress :
					if lastHeadUnstressedRunStart is not None:
						unstressedRunStartPitch=stressEndPitch+intonationParams['headUnstressedRunStartDelta']
						unstressedRunEndPitch=stressEndPitch+intonationParams['headUnstressedRunEndDelta']
						applyPitchPath(phonemeList,lastHeadUnstressedRunStart,index,basePitch,inflection,unstressedRunStartPitch,unstressedRunEndPitch)
						lastHeadUnstressedRunStart=None
					lastHeadStressStart=index
				elif lastHeadUnstressedRunStart is None: 
					lastHeadUnstressedRunStart=index

def generateFramesAndTiming(ipaText,speed=1,basePitch=100,inflection=0.5,clauseType=None,language=None):
	ipaText=normalizeIPA(ipaText,language=language)
	phonemeList=IPAToPhonemes(ipaText)
	if len(phonemeList)==0:
		return
	correctHPhonemes(phonemeList)
	calculatePhonemeTimes(phonemeList,speed,language=language)
	calculatePhonemePitches(phonemeList,speed,basePitch,inflection,clauseType)
	for phoneme in phonemeList:
		frameDuration=phoneme.pop('_duration')
		fadeDuration=phoneme.pop('_fadeDuration')
		if phoneme.get('_silence'):
			yield None,frameDuration,fadeDuration
		else:
			frame=speechPlayer.Frame()
			frame.preFormantGain=1.0
			frame.outputGain=2.0
			applyPhonemeToFrame(frame,phoneme)
			yield frame,frameDuration,fadeDuration

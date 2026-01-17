# NV Speech Player
A Klatt-based speech synthesis engine written in c++
Author: NV Access Limited

## Maintenance Note
NV Access is no longer maintaining this project. If you make use of this project or find it interesting, and you have the time and expertise to maintain it, please feel free to fork it and let us know you are interested in taking it on.

This includes the speechPlayer core itself, plus the nvSpeechPlayer NVDA add-on also in this repository.
Note that the eSpeak-ng/espeak-ng project also includes a copy of the speechPlayer code as an alternative Klatt implementation.
 
## Overview
NV Speech Player is a free and open-source prototype speech synthesizer that can be used by NVDA. It generates speech using Klatt synthesis, making it somewhat similar to speech synthesizers such as Dectalk and Eloquence.

## Licence and copyright
NV Speech Player is Copyright (c) 2014 NV Speech Player contributors
NV Speech Player is covered by the GNU General Public License (Version 2). 
You are free to share or change this software in any way you like 
as long as it is accompanied by the license and you make all 
source code available to anyone who wants it. This applies to 
both original and modified copies of this software, plus any 
derivative works.
For further details, you can view the license online at: 
http://www.gnu.org/licenses/old-licenses/gpl-2.0.html

## Background
The 70s and 80s saw much research in speech synthesis. One of the most prominent synthesis models that appeared was a formant-frequency synthesis known as Klatt synthesis. Some well-known Klatt synthesizers are Dectalk and Eloquence. They are well suited for use by the blind as they are extremely responsive, their pronunciation is smooth and predictable, and they are small in memory footprint. However, research soon moved onto other forms of synthesis such as concatinative speech, as although this was slower, it was much closer to the human voice. This was an advantage for usage in mainstream applications such as GPS units or telephone systems, but not necessarily so much of an advantage to the blind, who tend to care more about responsiveness and predictability over prettiness.

Although synthesizers such as Dectalk and Eloquence continued to be maintained and available for nearly 20 years, now they are becoming harder to get, with multiple companies saying that these, and their variants, have been end-of-lifed and will not be updated anymore. 

Concatinative synthesis is now starting to show promise as a replacement as the responsiveness and smoothness is improving. However, most if not all of the acceptable quality synthesizers are commercial and are rather expensive.

Both Dectalk and Eloquence were closed-source commercial products themselves. However, there is a substantial amount of source code and research material on Klatt synthesis available to the community. NV Speech Player tries to take advantage of this by being a 
modern prototype of a Klatt synthesizer, in the hopes to either be a replacement for synthesizers like Dectalk or Eloquence, or at least restart research and conversation around this synthesis method.

The eSpeak synthesizer, itself a free and open-source product has proved well as a replacement to a certain number of people in the community, but many people who hear it are extremely quick to point out its "metallic" sound and cannot seem to continue to use it. Although the authors of NV Speech Player still prefer eSpeak as their synthesizer of choice, they would still hope to try and understand better this strange resistance to eSpeak which may have something to do with eSpeak's spectral frequency synthesis verses Klatt synthesis. It may also have to do with the fact that consonants are also gathered from recorded speech and can therefore be perceived as being injected into the speech stream.

## Implementation
The synthesis engine itself is written in C++ using modern idioms, but closely following the implementation of klsyn-88, found at http://linguistics.berkeley.edu/phonlab/resources/

eSpeak is used to parse text into phonemes represented in IPA, making use of existing eSpeak dictionary processing. eSpeak can be found at: http://espeak.sourceforge.net/

The Klatt formant data for each individual phoneme was collected mostly from a project called PyKlatt: http://code.google.com/p/pyklatt/ However it has been further tweaked based on testing and matching with eSpeak's own data.

The rules for phoneme lengths, gaps, speed and intonation have been coded by hand in Python, though eSpeak's own intonation data was tried to be copied as much as possible.

## DSP pipeline internals (speechPlayer.cpp + speechWaveGenerator.cpp)
At the highest level, `speechPlayer.cpp` wires together the frame queue and the DSP generator:
- `speechPlayer_initialize()` builds a `FrameManager` plus a `SpeechWaveGenerator` and connects them so the generator can pull the current frame data as it produces audio samples.
- `speechPlayer_queueFrame()` pushes time-aligned frame data into the `FrameManager`, including minimum frame duration, fade time, and a user index for tracking.
- `speechPlayer_synthesize()` asks the wave generator for the next block of samples, which is where all the DSP happens.
- `speechPlayer_getLastIndex()` lets the caller know which queued frame index was last consumed by the renderer.  

The actual DSP pipeline lives in `speechWaveGenerator.cpp` and is executed once per output sample:
1. **Frame selection and interpolation:** `FrameManager::getCurrentFrame()` returns the current frame, or interpolates between the old/new frames using the configured fade time. This is how crossfades, pitch glides, and NULL (silence) frames work.
2. **Source generation (voicing + aspiration):**
   - `VoiceGenerator` turns `voicePitch` into a simple periodic waveform (saw-like cycle position mapped to -1..1), applies vibrato (`vibratoSpeed`, `vibratoPitchOffset`), and mixes in turbulence based on `voiceTurbulenceAmplitude` and `glottalOpenQuotient`.
   - `aspirationAmplitude` adds breath noise to the source.
3. **Cascade formant path:** The voiced source is shaped by a cascade of resonators (`cf1..cf6` with `cb1..cb6`), with optional nasal coupling (`cfN0/cfNP`, `cbN0/cbNP`, `caNP`).
4. **Parallel frication path:** A separate noise source (`fricationAmplitude`) is passed through parallel resonators (`pf1..pf6`, `pb1..pb6`, `pa1..pa6`). The `parallelBypass` control mixes raw noise against the resonated output.
5. **Mix and scale:** Cascade + parallel outputs are mixed, scaled by `preFormantGain` and `outputGain`, and clipped to a 16-bit range before being returned to the caller.

This structure keeps the time-domain synthesis logic entirely in the C++ core: Python code builds frame parameter tracks, while the C++ engine interpolates and renders them into audio.  

## How to add or tune phonemes (data.py)
`data.py` is a dictionary: keys are IPA symbols (like a, ɚ, t͡ʃ, ᴒ, etc.) and values are parameter sets that describe how the formant synthesizer should shape that sound.

### Adding a new phoneme (recommended workflow)
1. **Pick a key**
   - Use a real IPA symbol if possible (ɲ, ʎ, ɨ, …).
   - If you need a language-specific variant, use a private/internal key (we use things like ᴒ, ᴇ, ᴀ, ᴐ).
2. **Clone the closest existing phoneme**
   - Copy an existing entry and adjust it.
   - This is important: the engine expects most fields to exist. A “minimal” entry can crash.
3. **Tune it**
   - Start by adjusting formant center frequencies (`cf1`, `cf2`, `cf3`).
   - Then adjust bandwidths (`cb1`, `cb2`, `cb3`) if it sounds “boxy/ringy”.
   - Only then adjust frication/aspiration settings.
4. **Wire it up in `ipa.py`**
   - Make sure `normalizeIPA()` actually outputs your new key for the right language/case.
   - If you don’t map it, the phoneme will never be used.

### Parameter reference (what the fields mean)
**Phoneme type flags (metadata)**  
These fields are used by timing rules and by a few special cases:
- `_isVowel`: This is a vowel (timed longer, can be lengthened with ː).
- `_isVoiced`: Voiced (uses `voiceAmplitude`).
- `_isStop`: Stop consonant (very short; may get a silence gap).
- `_isNasal`: Nasal consonant or nasal vowel coupling.
- `_isLiquid`: l/r-like sounds (often get longer fades).
- `_isSemivowel`: Glides like j/w.
- `_isTap`, `_isTrill`: Very short rhotic types.
- `_isAfricate`: Affricate (timed like a stop+fricative).

**Core formant synthesizer knobs**  
Think of a vowel as resonances (formants). The important ones are F1–F3.

**Formant center frequencies** (where the resonances are, in Hz-ish units):
- `cf1`, `cf2`, `cf3`, `cf4`, `cf5`, `cf6`: “Cascade” formant frequencies. F1–F3 matter most for vowel identity.
- `pf1`, `pf2`, `pf3`, `pf4`, `pf5`, `pf6`: “Parallel” formant frequencies. Usually matched to the `cf*` values.

Quick intuition:
- Higher `cf1` → more open mouth (e.g. “ah”).
- Higher `cf2` → more front / brighter (e.g. “ee”).
- Lower `cf2` → more back / rounder (“oo”).
- Lower `cf3` → more “r-colored” (rhotic vowels).

**Bandwidths** (how “ringy” vs “flat” it sounds):
- `cb1..cb6` and `pb1..pb6`.

Quick intuition:
- Narrow bandwidth (small numbers) → very “ringy / boxy / hollow”.
- Wider bandwidth → smoother / less resonant / less “plastic box”.
- If something sounds “boxy”, widening `cb2`/`cb3` (and matching `pb2`/`pb3`) is often the first fix.

**Amplitude / mixing controls**
- `voiceAmplitude`: Strength of voicing. Lower it slightly if vowels feel “over-held” or harsh.
- `fricationAmplitude`: Noise level for fricatives (s, ʃ, f, x, etc.). If “s” is too hissy, reduce this.
- `aspirationAmplitude`: Breath noise used for aspirated/“h-like” behavior. Usually 0 for vowels.
- `parallelBypass`: Mix control between cascade and parallel paths. Most phonemes keep this at 0.0 unless you know you need it.
- `pa1..pa6`: Per-formant amplitude in the parallel path. Most entries keep these at 0.0. If a diphthong glide is too weak, a tiny `pa2`/`pa3` boost can help.

**Nasal coupling (optional)**
Some entries include:
- `cfN0`, `cfNP`, `cbN0`, `cbNP`, `caNP`: nasal resonance and coupling parameters. We currently treat nasality conservatively; if you don’t know what to do, clone from an existing nasal vowel/consonant entry.

### Practical tuning tips (fast wins)
**“This vowel sounds too much like another vowel”**
- Adjust `cf1` and `cf2` first.
- Example: Hungarian short *a* vs long *á*: make short *a* lower `cf1` and lower `cf2` compared to *á*.

**“This vowel is boxy / hollow / plastic”**
- Widen `cb2`/`cb3` (and `pb2`/`pb3`) a bit.

**“This sound is too sharp/hissy”**
- Lower `fricationAmplitude`.

**“This rhotic vowel (ɚ/ɝ) is too thick”**
- Raise `cf3` slightly (less r-color) or widen `cb3`.

### Don’t forget: mapping in ipa.py
Adding a phoneme to `data.py` does nothing until `normalizeIPA()` actually outputs it.  
Example: Hungarian short *a* uses `A` in eSpeak mnemonics. We map it to an internal symbol so it can be tuned without touching English:
```
if isHungarian:
    asciiMap[u"A"] = u"ᴒ"
```
 
## Building NV Speech Player
You will need:
- Python 3.7: http://www.python.org
- SCons 3: http://www.scons.org/
- Visual Studio 2019 Community 
 
To build: run scons

After building, there will be a nvSpeechPlayer_xxx.nvda-addon file in the root directory, where xxx is the git revision or hardcoded version number.
Installing this add-on into NVDA will allow you to use the Speech Player synthesizer in NVDA. Note everything you need is in the add-on, no extra dlls or files need to be copied.
 

// speechPlayer_synthesize2: index-aware synthesis for punctual NVDA callbacks.
//
// The NVDA driver binds synthIndexReached notifications to WavePlayer chunk
// completions. With plain synthesize(), a chunk can contain audio on BOTH
// sides of an index marker, so the notification fires up to a whole chunk
// (371 ms at 22050/8192) after the marker's true position — the root cause of
// issue #102 (capital-letter beep late / missing on fast typing). synthesize2
// stops generation at marker boundaries so every returned chunk ends exactly
// at the marker it reports.

#include "doctest.h"

#include "speechPlayer.h"

#include <cstring>
#include <vector>

namespace {

speechPlayer_frame_t makeVoicedFrame() {
	speechPlayer_frame_t f;
	memset(&f, 0, sizeof(f));
	f.voicePitch = 110.0;
	f.endVoicePitch = 110.0;
	f.glottalOpenQuotient = 0.5;
	f.voiceAmplitude = 0.6;
	f.cf1 = 500.0;  f.cb1 = 80.0;
	f.cf2 = 1500.0; f.cb2 = 90.0;
	f.cf3 = 2500.0; f.cb3 = 150.0;
	f.cf4 = 3300.0; f.cb4 = 250.0;
	f.cf5 = 3750.0; f.cb5 = 200.0;
	f.cf6 = 4900.0; f.cb6 = 1000.0;
	f.preFormantGain = 1.0;
	f.outputGain = 1.0;
	return f;
}

long long absEnergy(const sample* buf, int n) {
	long long e = 0;
	for (int i = 0; i < n; ++i) e += (buf[i].value < 0) ? -buf[i].value : buf[i].value;
	return e;
}

// Drive synthesize2 until it stops producing audio; record every (samples,
// index) stop. Returns the list of reported indexes in order.
struct Stop { int samples; int index; };

std::vector<Stop> drain(speechPlayer_handle_t h, std::vector<sample>& allAudio, int maxCalls = 64) {
	std::vector<Stop> stops;
	sample buf[8192];
	for (int call = 0; call < maxCalls; ++call) {
		int idx = -2;
		int n = speechPlayer_synthesize2(h, 8192, buf, &idx);
		CHECK(idx >= -1);  // -2 sentinel must always be overwritten
		if (n > 0) allAudio.insert(allAudio.end(), buf, buf + n);
		stops.push_back({n, idx});
		if (n == 0 && idx == -1) break;  // fully drained
	}
	return stops;
}

}  // namespace

TEST_CASE("synthesize2: chunks end exactly at index markers, each reported once") {
	const int sr = 22050;
	speechPlayer_handle_t h = speechPlayer_initialize(sr);
	REQUIRE(h);

	speechPlayer_frame_t voiced = makeVoicedFrame();
	const unsigned int frameLen = 2205;  // 100 ms
	speechPlayer_queueFrame(h, &voiced, frameLen, 22, -1, false);
	speechPlayer_queueFrame(h, NULL, 0, 1, 5, false);   // marker index 5
	speechPlayer_queueFrame(h, &voiced, frameLen, 22, -1, false);
	speechPlayer_queueFrame(h, NULL, 0, 1, 7, false);   // trailing marker index 7

	std::vector<sample> audio;
	auto stops = drain(h, audio);

	// Collect reported indexes in order.
	std::vector<int> reported;
	for (auto& s : stops) if (s.index >= 0) reported.push_back(s.index);
	REQUIRE(reported.size() == 2);
	CHECK(reported[0] == 5);
	CHECK(reported[1] == 7);

	// The stop that reported index 5 must sit near the first frame's length:
	// all audio before it belongs to frame 1 (plus initial fade-in), none of
	// frame 2's audio may precede the marker.
	int samplesBeforeIdx5 = 0;
	for (auto& s : stops) {
		samplesBeforeIdx5 += s.samples;
		if (s.index == 5) break;
	}
	CHECK(samplesBeforeIdx5 >= (int)frameLen - 32);
	CHECK(samplesBeforeIdx5 <= (int)frameLen + 128);

	// Real audio was produced on both sides.
	CHECK(absEnergy(audio.data(), (int)audio.size()) > 1000);

	speechPlayer_terminate(h);
}

TEST_CASE("synthesize2: leading marker is reported before any audio") {
	speechPlayer_handle_t h = speechPlayer_initialize(22050);
	REQUIRE(h);

	speechPlayer_frame_t voiced = makeVoicedFrame();
	speechPlayer_queueFrame(h, NULL, 0, 1, 3, false);   // leading marker (e.g. beep)
	speechPlayer_queueFrame(h, &voiced, 2205, 22, -1, false);

	sample buf[8192];
	int idx = -2;
	int n = speechPlayer_synthesize2(h, 8192, buf, &idx);
	// The marker sits at the queue head: it must be reported with (nearly) no
	// audio preceding it — this is what makes the capital-letter beep fire at
	// the letter's onset instead of after it.
	CHECK(idx == 3);
	CHECK(n <= 8);

	// The letter audio follows in later calls.
	std::vector<sample> audio;
	drain(h, audio);
	CHECK(absEnergy(audio.data(), (int)audio.size()) > 1000);

	speechPlayer_terminate(h);
}

TEST_CASE("synthesize2: no markers behaves like synthesize") {
	speechPlayer_handle_t h = speechPlayer_initialize(22050);
	REQUIRE(h);

	speechPlayer_frame_t voiced = makeVoicedFrame();
	speechPlayer_queueFrame(h, &voiced, 2205, 22, -1, false);

	std::vector<sample> audio;
	auto stops = drain(h, audio);
	for (auto& s : stops) CHECK(s.index == -1);
	CHECK((int)audio.size() >= 2205 - 32);
	CHECK(absEnergy(audio.data(), (int)audio.size()) > 1000);

	speechPlayer_terminate(h);
}

TEST_CASE("synthesize2: concatenated output is bit-identical to legacy synthesize") {
	// The strong parity property: splitting at markers must not perturb the
	// DSP by even one sample, or every say-all word boundary becomes a tick.
	// Uses two different vowel frames around the markers so resonator state
	// genuinely carries across each split point. Pure voiced (no noise
	// sources) keeps both handles deterministic.
	speechPlayer_frame_t v1 = makeVoicedFrame();
	speechPlayer_frame_t v2 = makeVoicedFrame();
	v2.cf1 = 700.0; v2.cf2 = 1100.0;  // different vowel -> real state motion

	auto queueUtterance = [&](speechPlayer_handle_t h) {
		speechPlayer_queueFrame(h, &v1, 1102, 22, -1, false);
		speechPlayer_queueFrame(h, NULL, 0, 1, 5, false);
		speechPlayer_queueFrame(h, NULL, 0, 1, 6, false);  // back-to-back: second
		    // flip lands on the tick right after the first chunk's teardown --
		    // the shape most likely to get swallowed by a split-fix like this
		speechPlayer_queueFrame(h, &v2, 1102, 22, -1, false);
		speechPlayer_queueFrame(h, NULL, 0, 1, 7, false);
		speechPlayer_queueFrame(h, &v1, 551, 11, -1, false);
	};

	speechPlayer_handle_t hA = speechPlayer_initialize(22050);
	speechPlayer_handle_t hB = speechPlayer_initialize(22050);
	REQUIRE(hA);
	REQUIRE(hB);
	queueUtterance(hA);
	queueUtterance(hB);

	// Fixed call budget on BOTH sides (mid-stream zero returns are legal for
	// both APIs while silence frames drain).
	std::vector<sample> viaSplit, viaLegacy;
	sample buf[8192];
	int markersSeen = 0;
	for (int call = 0; call < 24; ++call) {
		int idx = -1;
		int n = speechPlayer_synthesize2(hA, 8192, buf, &idx);
		if (idx >= 0) ++markersSeen;
		if (n > 0) viaSplit.insert(viaSplit.end(), buf, buf + n);
	}
	for (int call = 0; call < 24; ++call) {
		int n = speechPlayer_synthesize(hB, 8192, buf);
		if (n > 0) viaLegacy.insert(viaLegacy.end(), buf, buf + n);
	}

	CHECK(markersSeen == 3);  // splitting genuinely happened, incl. both
	                          // halves of the adjacent pair
	REQUIRE(viaSplit.size() == viaLegacy.size());
	CHECK(memcmp(viaSplit.data(), viaLegacy.data(),
	             viaSplit.size() * sizeof(sample)) == 0);
	CHECK(absEnergy(viaSplit.data(), (int)viaSplit.size()) > 1000);

	speechPlayer_terminate(hA);
	speechPlayer_terminate(hB);
}

TEST_CASE("synthesize2: consecutive markers each reported, in order") {
	speechPlayer_handle_t h = speechPlayer_initialize(22050);
	REQUIRE(h);

	speechPlayer_frame_t voiced = makeVoicedFrame();
	speechPlayer_queueFrame(h, &voiced, 1102, 22, -1, false);
	speechPlayer_queueFrame(h, NULL, 0, 1, 10, false);
	speechPlayer_queueFrame(h, NULL, 0, 1, 11, false);  // back-to-back markers
	speechPlayer_queueFrame(h, &voiced, 1102, 22, -1, false);
	speechPlayer_queueFrame(h, NULL, 0, 1, 12, false);

	std::vector<sample> audio;
	auto stops = drain(h, audio);
	std::vector<int> reported;
	for (auto& s : stops) if (s.index >= 0) reported.push_back(s.index);
	REQUIRE(reported.size() == 3);
	CHECK(reported[0] == 10);
	CHECK(reported[1] == 11);
	CHECK(reported[2] == 12);

	speechPlayer_terminate(h);
}

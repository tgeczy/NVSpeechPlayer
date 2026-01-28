/*
  nvspRender
  ---------
  Small command-line helper that turns an IPA stream into audio using:
    - nvspFrontend (IPA -> formant frames)
    - speechPlayer (frames -> 16-bit PCM)

  Intended use:
    - Speech Dispatcher via sd_generic (see extras/speech-dispatcher/)

  Notes:
    - This tool reads UTF-8 from stdin and writes raw 16-bit signed
      little-endian PCM to stdout at a configurable sample rate (default 16000 Hz).
    - We deliberately keep the interface small and self-contained.
*/

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <iostream>
#include <string>
#include <vector>

#include "speechPlayer.h"
#include "nvspFrontend.h"

#if defined(_WIN32)
  #include <fcntl.h>
  #include <io.h>
#endif

namespace {

struct Options {
  std::string packDir = ".";   // Directory containing "packs/" or the packs folder itself.
  std::string language = "en";  // e.g. "en", "en-us", "fr".

  // Speech Dispatcher (SSIP) conventions are typically -100..+100 for rate.
  // We accept that and map it to a speed multiplier.
  int rate = 0;

  // We accept 0..100 (like eSpeak pitch after GenericPitch mapping).
  int pitch = 50;

  // We accept a linear gain multiplier. In Speech Dispatcher generic configs,
  // volume is often mapped to 0.0..2.0 (default 1.0).
  double volume = 1.0;

  // Output sample rate in Hz. Must match whatever plays the raw stream.
  // Default is 16000 to match the NVDA driver.
  int sampleRate = 16000;

  // Leave room for future tuning without having to change the speech-dispatcher config.
  double inflection = 0.5;

  bool help = false;
};

static void printHelp(const char* argv0) {
  std::cerr
    << "Usage: " << (argv0 ? argv0 : "nvspRender") << " [options]\n\n"
    << "Reads IPA text from stdin (UTF-8) and writes raw 16-bit PCM to stdout (default 16000 Hz; configurable).\n\n"
    << "Options:\n"
    << "  --packdir <path>   Path to repo root or packs dir (default: .)\n"
    << "  --lang <tag>       Language tag for pack selection (default: en)\n"
    << "  --rate <int>       SSIP-style rate -100..100 (default: 0)\n"
    << "  --pitch <int>      Pitch 0..100 (default: 50)\n"
    << "  --volume <float>   Output gain multiplier (default: 1.0)\n"
    << "  --samplerate <int> Output sample rate in Hz (default: 16000)\n"
    << "  --inflection <f>   Inflection (octaves across +/-50% pitch path) (default: 0.5)\n"
    << "  -h, --help         Show this help\n";
}

static bool parseInt(const char* s, int& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (!end || *end != '\0') return false;
  out = static_cast<int>(v);
  return true;
}

static bool parseDouble(const char* s, double& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  double v = std::strtod(s, &end);
  if (!end || *end != '\0') return false;
  out = v;
  return true;
}

static Options parseArgs(int argc, char** argv) {
  Options opt;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i] ? argv[i] : "";

    if (a == "-h" || a == "--help") {
      opt.help = true;
      continue;
    }

    auto requireValue = [&](const char* name) -> const char* {
      if (i + 1 >= argc || !argv[i + 1]) {
        std::cerr << "Missing value for " << name << "\n";
        opt.help = true;
        return nullptr;
      }
      return argv[++i];
    };

    if (a == "--packdir") {
      if (const char* v = requireValue("--packdir")) opt.packDir = v;
      continue;
    }
    if (a == "--lang") {
      if (const char* v = requireValue("--lang")) opt.language = v;
      continue;
    }
    if (a == "--rate") {
      if (const char* v = requireValue("--rate")) {
        int tmp = 0;
        if (!parseInt(v, tmp)) {
          std::cerr << "Bad --rate value: " << v << "\n";
          opt.help = true;
        } else {
          opt.rate = tmp;
        }
      }
      continue;
    }
    if (a == "--pitch") {
      if (const char* v = requireValue("--pitch")) {
        int tmp = 0;
        if (!parseInt(v, tmp)) {
          std::cerr << "Bad --pitch value: " << v << "\n";
          opt.help = true;
        } else {
          opt.pitch = tmp;
        }
      }
      continue;
    }
    if (a == "--volume") {
      if (const char* v = requireValue("--volume")) {
        double tmp = 1.0;
        if (!parseDouble(v, tmp)) {
          std::cerr << "Bad --volume value: " << v << "\n";
          opt.help = true;
        } else {
          opt.volume = tmp;
        }
      }
      continue;
    }

    if (a == "--samplerate" || a == "--sample-rate") {
      if (const char* v = requireValue(a.c_str())) {
        int tmp = 16000;
        if (!parseInt(v, tmp) || tmp < 8000 || tmp > 192000) {
          std::cerr << "Bad --samplerate value: " << v << " (expected 8000..192000)\n";
          opt.help = true;
        } else {
          opt.sampleRate = tmp;
        }
      }
      continue;
    }

    if (a == "--inflection") {
      if (const char* v = requireValue("--inflection")) {
        double tmp = 0.5;
        if (!parseDouble(v, tmp)) {
          std::cerr << "Bad --inflection value: " << v << "\n";
          opt.help = true;
        } else {
          opt.inflection = tmp;
        }
      }
      continue;
    }

    std::cerr << "Unknown arg: " << a << "\n";
    opt.help = true;
  }

  return opt;
}

static std::string readAllStdin() {
  std::string out;
  char buf[4096];
  while (true) {
    size_t n = std::fread(buf, 1, sizeof(buf), stdin);
    if (n > 0) out.append(buf, n);
    if (n < sizeof(buf)) {
      if (std::feof(stdin)) break;
      if (std::ferror(stdin)) break;
    }
  }
  return out;
}

struct CallbackCtx {
  speechPlayer_handle_t player = nullptr;
  int sampleRate = 16000;
  double volume = 1.0;
};

static void onFrontendFrame(
    void* userData,
    const nvspFrontend_Frame* frameOrNull,
    double durationMs,
    double fadeMs,
    int userIndex
) {
  auto* ctx = static_cast<CallbackCtx*>(userData);
  if (!ctx || !ctx->player) return;

  auto msToSamples = [&](double ms) -> unsigned int {
    if (ms <= 0.0) return 0;
    const double s = (ms * static_cast<double>(ctx->sampleRate)) / 1000.0;
    if (s <= 0.0) return 0;
    return static_cast<unsigned int>(s + 0.5);
  };

  // Convert ms -> samples.
  const unsigned int minSamples = msToSamples(durationMs);
  const unsigned int fadeSamples = msToSamples(fadeMs);

  // speechPlayer_queueFrame copies the frame struct, so a stack copy is fine.
  if (frameOrNull) {
    static_assert(sizeof(nvspFrontend_Frame) == sizeof(speechPlayer_frame_t), "Frame ABI mismatch");
    speechPlayer_frame_t f{};
    std::memcpy(&f, frameOrNull, sizeof(f));
    f.outputGain *= ctx->volume;
    speechPlayer_queueFrame(ctx->player, &f, minSamples, fadeSamples, userIndex, false);
  } else {
    // Silence frame.
    speechPlayer_queueFrame(ctx->player, nullptr, minSamples, fadeSamples, userIndex, false);
  }
}

// Map Speech Dispatcher SSIP rate (-100..+100) to a speed multiplier.
// Exponential mapping feels more natural than linear at the extremes.
static double ssipRateToSpeed(int ssipRate) {
  // Clamp to a reasonable range.
  if (ssipRate < -100) ssipRate = -100;
  if (ssipRate > 100) ssipRate = 100;
  return std::pow(2.0, static_cast<double>(ssipRate) / 100.0);
}

// Map a 0..100 pitch slider to a base pitch in Hz.
// This matches the mapping used in the NVDA add-on for this engine.
static double sliderPitchToBaseHz(int pitch0to100) {
  if (pitch0to100 < 0) pitch0to100 = 0;
  if (pitch0to100 > 100) pitch0to100 = 100;
  // 0 -> ~25 Hz, 50 -> ~110 Hz, 100 -> ~195 Hz.
  return 25.0 + (21.25 * (static_cast<double>(pitch0to100) / 12.5));
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parseArgs(argc, argv);
  if (opt.help) {
    printHelp(argv && argv[0] ? argv[0] : "nvspRender");
    return 2;
  }

#if defined(_WIN32)
  // Ensure stdout is binary so 0x1A etc don't get mangled.
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  const std::string ipa = readAllStdin();
  if (ipa.empty()) {
    return 0;
  }

  // Initialize speechPlayer.
  const int sampleRate = 16000;
  speechPlayer_handle_t player = speechPlayer_initialize(sampleRate);
  if (!player) {
    std::cerr << "speechPlayer_initialize failed\n";
    return 1;
  }

  // Initialize frontend.
  nvspFrontend_handle_t fe = nvspFrontend_create(opt.packDir.c_str());
  if (!fe) {
    std::cerr << "nvspFrontend_create failed (packDir='" << opt.packDir << "')\n";
    speechPlayer_terminate(player);
    return 1;
  }

  if (!nvspFrontend_setLanguage(fe, opt.language.c_str())) {
    std::cerr << "nvspFrontend_setLanguage failed (lang='" << opt.language << "')\n";
    const char* err = nvspFrontend_getLastError(fe);
    if (err && *err) std::cerr << "  " << err << "\n";
    nvspFrontend_destroy(fe);
    speechPlayer_terminate(player);
    return 1;
  }

  CallbackCtx cbCtx;
  cbCtx.player = player;
  cbCtx.sampleRate = sampleRate;
  cbCtx.volume = opt.volume;

  const double speed = ssipRateToSpeed(opt.rate);
  const double basePitchHz = sliderPitchToBaseHz(opt.pitch);
  const double inflection = opt.inflection;

  // clauseTypeUtf8 = NULL means "default"; the frontend will treat it as '.' internally.
  const char* clauseTypeUtf8 = nullptr;

  if (!nvspFrontend_queueIPA(
    fe,
    ipa.c_str(),
    speed,
    basePitchHz,
    inflection,
    clauseTypeUtf8,
    /*userIndexBase=*/0,
    &onFrontendFrame,
    &cbCtx
  )) {
    std::cerr << "nvspFrontend_queueIPA failed\n";
    const char* err = nvspFrontend_getLastError(fe);
    if (err && *err) std::cerr << "  " << err << "\n";
    nvspFrontend_destroy(fe);
    speechPlayer_terminate(player);
    return 1;
  }

  // Synthesize to stdout as raw PCM.
  static_assert(sizeof(sample) == sizeof(sampleVal), "sample struct should be a packed 16-bit value");
  std::vector<sample> pcm;
  pcm.resize(2048);

  while (true) {
    const int n = speechPlayer_synthesize(player, static_cast<unsigned int>(pcm.size()), pcm.data());
    if (n <= 0) break;

    const size_t bytes = static_cast<size_t>(n) * sizeof(sample);
    size_t written = 0;
    while (written < bytes) {
      const size_t w = std::fwrite(reinterpret_cast<const char*>(pcm.data()) + written, 1, bytes - written, stdout);
      if (w == 0) {
        // stdout closed / pipe broken.
        nvspFrontend_destroy(fe);
        speechPlayer_terminate(player);
        return 0;
      }
      written += w;
    }

    if (n < static_cast<int>(pcm.size())) break;
  }

  nvspFrontend_destroy(fe);
  speechPlayer_terminate(player);
  return 0;
}

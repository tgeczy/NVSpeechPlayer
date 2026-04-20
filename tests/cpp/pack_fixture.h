// Shared test fixture that loads a real language pack once, so individual
// C++ tests can ask questions of the loaded pack or run passes against it
// without each test paying the pack-load cost.
//
// Usage:
//   #include "pack_fixture.h"
//   TEST_CASE_FIXTURE(PackFixture, "test name") { ... use pack ... }
//
// The fixture resolves the repo root by walking up from the test binary's
// working directory until it finds a "packs" folder. This makes the tests
// runnable from any working directory (build-*/tests/cpp/MinSizeRel/ or the
// repo root).

#ifndef TGSB_TEST_PACK_FIXTURE_H
#define TGSB_TEST_PACK_FIXTURE_H

#include "doctest.h"

#include <filesystem>
#include <string>

#include "pack.h"

namespace tgsb_test {

inline std::string findPackDir() {
    namespace fs = std::filesystem;
    fs::path p = fs::current_path();
    for (int i = 0; i < 10; ++i) {
        if (fs::is_directory(p / "packs")) {
            return p.string();
        }
        if (!p.has_parent_path() || p == p.parent_path()) break;
        p = p.parent_path();
    }
    return "";  // caller will fail loudly
}

struct PackFixture {
    nvsp_frontend::PackSet pack;
    std::string loadError;

    explicit PackFixture(const std::string& lang = "es-mx") {
        std::string packDir = findPackDir();
        REQUIRE_MESSAGE(!packDir.empty(),
                        "could not locate 'packs' directory by walking up from cwd");
        const bool ok = nvsp_frontend::loadPackSet(packDir, lang, pack, loadError);
        REQUIRE_MESSAGE(ok, "loadPackSet failed for '" << lang << "': " << loadError);
    }

    // Look up a phoneme by UTF-32 IPA key. Returns nullptr if not found —
    // callers use REQUIRE(def) to fail loudly.
    const nvsp_frontend::PhonemeDef* find(const std::u32string& key) const {
        auto it = pack.phonemes.find(key);
        return (it == pack.phonemes.end()) ? nullptr : &it->second;
    }

    // Resolve a field from a PhonemeDef, or 0.0 if not set.
    static double resolve(const nvsp_frontend::PhonemeDef& def,
                          nvsp_frontend::FieldId id) {
        const int idx = static_cast<int>(id);
        const std::uint64_t bit = 1ULL << idx;
        if (def.setMask & bit) return def.field[idx];
        return 0.0;
    }
};

}  // namespace tgsb_test

#endif  // TGSB_TEST_PACK_FIXTURE_H

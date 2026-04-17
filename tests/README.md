# TGSpeechBox tests

Pytest-based regression tests for the frontend (`nvspFrontend.dll`).

## Why this exists

Most TGSpeechBox bugs over the past months — Spanish allophone misfires
(#84, #95), dialect S leaks (#74, #81), thousands-separator clause splits
(#74), year-spelling regressions ("4000" → "forty oh zero"), the chorus
slider that didn't reach the DLL (#93) — are all bugs that **never should
have shipped** because they're deterministic outputs of pure functions
(text → text, IPA → tokens, etc.). Manual ear-hunting via NVDA + debug
logs has cost real time.

This harness pins those behaviors as tests so any regression fails the
build (via the same CMake `check_struct_parity`-style mechanism) before
users see it.

## Layout

- `conftest.py` — pytest fixtures (loads the frontend DLL, creates a
  per-language handle).
- `_test_frontend.py` — minimal standalone ctypes wrapper around
  `nvspFrontend.dll`. Does NOT depend on NVDA's `logHandler`, so it
  works in plain Python (unlike `nvdaAddon/synthDrivers/tgSpeechBox/_frontend.py`,
  which is NVDA-specific).
- `test_text_preparation.py` — `prepareText()` regression tests
  (text → eSpeak-input text, no IPA involved). Covers letter dict
  lookups, thousands separator handling, year splitting, etc.
- `test_es_mx_phonemes.py` — Spanish-Mexican phonemization smoke tests
  for the bugs in #74/#81/#84/#95.

## Running

Requires:
- A built `nvspFrontend.dll` in `build-x86-nvda/MinSizeRel/` (or
  matching bitness of your Python).
- 32-bit Python (matches the x86-nvda build) with pytest installed.

From the repo root:

```
py -m pytest tests/ -v
```

To run a single file:

```
py -m pytest tests/test_text_preparation.py -v
```

To see expected vs actual on failures (no truncation):

```
py -m pytest tests/ -v -s
```

## Adding a test for a real bug

1. Find the bug in the GitHub issues (e.g. #95 "diálogo" → "diálolo").
2. Write the smallest reproducing input — usually a single word or
   short phrase — in the appropriate `test_*.py`.
3. Use plain assertions (`assert "g" in phonemes`, `assert "l" not in phonemes`)
   that read like the bug report. Avoid snapshot tests for known-bug
   regressions; assertions document the intent better.
4. Reference the issue number in the test docstring.
5. Run the test. **If it passes immediately**: maybe the bug is already
   fixed, or maybe the assertion is wrong — investigate before committing.
   **If it fails**: that's a real shipped bug. Fix the bug, then the test
   passes and stays as a regression guard.

## Why standalone wrapper instead of `nvdaAddon/synthDrivers/tgSpeechBox/_frontend.py`?

That file imports `from logHandler import log` at the top — `logHandler` is
an NVDA-internal module that doesn't exist outside NVDA. We could mock it,
but a small standalone wrapper is cleaner, removes a fragile dependency,
and makes the test code easier to read. The two files do diverge slightly
in coverage; that's intentional — `_test_frontend.py` only wraps the
functions tests actually use.

#!/usr/bin/env python3
"""
Generate FrameEx struct mirrors from src/frame.h (the canonical source).

src/frame.h defines speechPlayer_frameEx_t — the canonical layout of the DSP
voice-quality extension struct. Five other places in the repo MUST match it
byte-for-byte (because they're either the same struct in a different namespace
or a ctypes mirror that gets marshaled to the DLL):

  - src/frontend/nvspFrontend.h                          (nvspFrontend_FrameEx)
  - tools/tgsbPhonemeEditorWin32/tgsb_runtime.h          (EditorFrameEx)
  - tools/tgsbRender.cpp                                 (FrameEx)
  - speechPlayer.py                                      (FrameEx ctypes)
  - nvdaAddon/synthDrivers/tgSpeechBox/_frontend.py      (FrameEx ctypes)

Historically these drifted silently — see issue #93 (chorus sliders not working
because the Python ctypes mirror was missing v5 fields) and the "five FrameEx
structs that must stay in sync" line in MEMORY.md. This script eliminates the
duplication maintenance burden by treating src/frame.h as truth and regenerating
the other five from it.

Each target file has BEGIN/END marker comments delimiting the auto-generated
region. Content outside the markers is preserved verbatim; content inside is
overwritten. If markers are missing, the script fails loudly.

Usage:
  python tools/gen_frame_ex.py            # regenerate all five mirrors
  python tools/gen_frame_ex.py --check    # exit 1 if any mirror is out of sync
                                          # (use as pre-commit hook / CI gate)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Callable, Iterable

REPO_ROOT = Path(__file__).resolve().parent.parent
CANONICAL = REPO_ROOT / "src" / "frame.h"
STRUCT_NAME = "speechPlayer_frameEx_t"

# Section titles, keyed by the first field of each group. Kept here (not parsed
# from frame.h) so the canonical doxygen comments can stay rich and structured
# without dictating the one-liner output format. If you add a field that begins
# a new logical group, add an entry here.
SECTION_TITLES = [
    ("creakiness",            "Voice quality parameters (DSP v5)"),
    ("endCf1",                "Formant end targets for within-frame ramping (NAN = no ramp)"),
    ("fujisakiEnabled",       "Fujisaki-Bartman pitch contour model (DSP v6+)"),
    ("transF1Scale",          "Per-parameter transition speed scales (DSP v7). 0.0 = no override."),
    ("transAmplitudeMode",    "Amplitude crossfade mode (DSP v7.1). 0=linear, 1=equal-power."),
    ("cf7",                   "Higher cascade formants F7/F8 (DSP v8, Rabiner 1968 defaults)"),
    ("transSourceHoldRatio",  "Source amplitude timing (DSP v8). 0.0 = legacy, no hold."),
    ("transVoicingHoldRatio", "Voicing onset hold (DSP v8). 0.0 = legacy, no hold."),
    ("fricationTiltDb",       "Frication spectral tilt (DSP v9). 0=flat, negative=darken high-freq parallels."),
]

MARKER_BEGIN_TAG = (
    "AUTO-GENERATED FROM src/frame.h - DO NOT EDIT MANUALLY "
    "(regenerate: python tools/gen_frame_ex.py)"
)
MARKER_END_TAG = "END AUTO-GENERATED"


class Field:
    __slots__ = ("name", "comment")
    def __init__(self, name: str, comment: str = "") -> None:
        self.name = name
        self.comment = comment


def parse_canonical() -> list[Field]:
    """Extract the ordered list of fields from speechPlayer_frameEx_t in frame.h."""
    text = CANONICAL.read_text(encoding="utf-8")
    m = re.search(
        rf"typedef\s+struct\s*\{{(.+?)\}}\s*{re.escape(STRUCT_NAME)}\s*;",
        text,
        re.DOTALL,
    )
    if not m:
        sys.exit(f"ERROR: could not find `typedef struct {{...}} {STRUCT_NAME};` in {CANONICAL}")
    body = m.group(1)

    fields: list[Field] = []
    # A single field line: `double NAME;` optionally followed by an inline
    # `// comment` or `/* comment */`. Multi-line preceding doxygen blocks are
    # ignored — they belong to the canonical file.
    line_re = re.compile(
        r"^[ \t]*double\s+(\w+)\s*;"
        r"[ \t]*(?://[ \t]*(.+?)|/\*[ \t]*(.+?)[ \t]*\*/)?[ \t]*$",
        re.MULTILINE,
    )
    for fm in line_re.finditer(body):
        name = fm.group(1)
        comment = (fm.group(2) or fm.group(3) or "").strip()
        fields.append(Field(name, comment))

    if not fields:
        sys.exit(f"ERROR: parsed zero fields from {STRUCT_NAME}")
    return fields


def grouped(fields: list[Field]) -> Iterable[tuple[str | None, list[Field]]]:
    """Yield (section_title_or_None, [Field]) tuples in declaration order."""
    title_for = dict(SECTION_TITLES)
    title: str | None = None
    bucket: list[Field] = []
    for f in fields:
        if f.name in title_for:
            if bucket:
                yield title, bucket
            title = title_for[f.name]
            bucket = [f]
        else:
            bucket.append(f)
    if bucket:
        yield title, bucket


# ---------- Renderers per target format ----------

def _cpp_lines(fields: list[Field]) -> list[str]:
    out: list[str] = []
    for title, group in grouped(fields):
        if title:
            out.append(f"  // {title}")
        for f in group:
            line = f"  double {f.name};"
            if f.comment:
                line += f"  // {f.comment}"
            out.append(line)
    return out


def render_cpp_typedef(fields: list[Field], typedef_name: str) -> str:
    return "typedef struct {\n" + "\n".join(_cpp_lines(fields)) + f"\n}} {typedef_name};"


def render_cpp_struct(fields: list[Field], struct_name: str) -> str:
    return f"struct {struct_name} {{\n" + "\n".join(_cpp_lines(fields)) + "\n};"


def render_python_fields(fields: list[Field], ctypes_prefix: str) -> str:
    out = ["    _fields_ = ["]
    for title, group in grouped(fields):
        if title:
            out.append(f"        # {title}")
        for f in group:
            line = f'        ("{f.name}", {ctypes_prefix}),'
            if f.comment:
                line += f"  # {f.comment}"
            out.append(line)
    out.append("    ]")
    return "\n".join(out)


# ---------- Target file table ----------

TARGETS: list[dict] = [
    dict(
        path="src/frontend/nvspFrontend.h",
        comment="//",
        render=lambda f: render_cpp_typedef(f, "nvspFrontend_FrameEx"),
    ),
    dict(
        path="tools/tgsbPhonemeEditorWin32/tgsb_runtime.h",
        comment="//",
        render=lambda f: render_cpp_struct(f, "EditorFrameEx"),
    ),
    dict(
        path="tools/tgsbRender.cpp",
        comment="//",
        render=lambda f: render_cpp_struct(f, "FrameEx"),
    ),
    dict(
        path="speechPlayer.py",
        comment="#",
        render=lambda f: render_python_fields(f, "c_double"),
    ),
    dict(
        path="nvdaAddon/synthDrivers/tgSpeechBox/_frontend.py",
        comment="#",
        render=lambda f: render_python_fields(f, "ctypes.c_double"),
    ),
]


def replace_between_markers(text: str, comment_prefix: str, generated: str) -> str:
    cp = re.escape(comment_prefix)
    begin_re = re.compile(rf"{cp}\s*>>>\s*{re.escape(MARKER_BEGIN_TAG)}\s*>>>")
    end_re = re.compile(rf"{cp}\s*<<<\s*{re.escape(MARKER_END_TAG)}\s*<<<")
    bm = begin_re.search(text)
    if not bm:
        raise RuntimeError("BEGIN marker not found (expected `{} >>> {} >>>`)"
                           .format(comment_prefix, MARKER_BEGIN_TAG))
    # Search for the END marker only AFTER the BEGIN position. The END marker
    # text ("END AUTO-GENERATED") is shared with other codegen scripts, so files
    # with multiple auto-generated regions need positional disambiguation.
    em = end_re.search(text, bm.end())
    if not em:
        raise RuntimeError("END marker not found after BEGIN (expected `{} <<< {} <<<`)"
                           .format(comment_prefix, MARKER_END_TAG))
    # Detect indentation of the END marker's line (whitespace between the
    # preceding newline and the marker itself) so we preserve the caller's
    # chosen indent on regeneration. Important for Python where the END
    # marker often sits inside a class body.
    line_start = text.rfind("\n", 0, em.start()) + 1
    end_indent = text[line_start : em.start()]
    # Keep the marker lines themselves verbatim; replace only the content between.
    return text[: bm.end()] + "\n" + generated + "\n" + end_indent + text[em.start() :]


def main() -> None:
    check_only = "--check" in sys.argv
    fields = parse_canonical()
    print(f"Parsed {len(fields)} fields from {CANONICAL.relative_to(REPO_ROOT)}")

    drift: list[str] = []
    for t in TARGETS:
        rel = t["path"]
        path = REPO_ROOT / rel
        try:
            old = path.read_text(encoding="utf-8")
        except FileNotFoundError:
            sys.exit(f"ERROR: target file missing: {rel}")

        try:
            new = replace_between_markers(old, t["comment"], t["render"](fields))
        except RuntimeError as e:
            sys.exit(f"ERROR in {rel}: {e}")

        if new == old:
            print(f"  unchanged: {rel}")
        elif check_only:
            drift.append(rel)
            print(f"  DRIFT:     {rel}", file=sys.stderr)
        else:
            path.write_text(new, encoding="utf-8")
            print(f"  updated:   {rel}")

    if check_only and drift:
        print(
            f"\nFrameEx mirrors out of sync ({len(drift)} file(s)). Regenerate with:",
            file=sys.stderr,
        )
        print("  python tools/gen_frame_ex.py", file=sys.stderr)
        sys.exit(1)
    if check_only:
        print("All FrameEx mirrors in sync with src/frame.h.")


if __name__ == "__main__":
    main()

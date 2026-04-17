#!/usr/bin/env python3
"""
Generate VoicingTone struct mirrors from src/voicingTone.h (the canonical).

src/voicingTone.h defines speechPlayer_voicingTone_t. Four other places in
the repo must keep its 19 double fields in lockstep (struct order, names,
types) — drift here causes the same silent-attribute-write bug class as #93.

Targets (each marked with BEGIN/END markers around the field declarations):
  - src/frontend/nvspFrontend.h                          (nvspFrontend_VoicingTone)
  - tools/tgsbPhonemeEditorWin32/tgsb_runtime.h          (EditorVoicingToneV3)
  - speechPlayer.py                                      (VoicingTone ctypes)
  - nvdaAddon/synthDrivers/tgSpeechBox/_frontend.py      (VoicingTone ctypes)

Unlike FrameEx (where the whole struct is regenerated), VoicingTone targets
that include the ABI magic header keep that header OUTSIDE the markers — only
the 19 doubles are codegen-managed. This avoids teaching the parser about
uint32_t fields while still solving the actual drift risk.

Usage:
  python tools/gen_voicing_tone.py            # regenerate all 4 mirrors
  python tools/gen_voicing_tone.py --check    # exit 1 if any mirror is out of sync
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parent.parent
CANONICAL = REPO_ROOT / "src" / "voicingTone.h"
STRUCT_NAME = "speechPlayer_voicingTone_t"

# Section titles, keyed by the first field of each group. Match the layout
# blocks in voicingTone.h so generated mirrors are readable.
SECTION_TITLES = [
    ("voicingPeakPos",         "V1 parameters"),
    ("noiseGlottalModDepth",   "V2 parameters"),
    ("speedQuotient",          "V3 parameters"),
    ("nasalBwScale",           "V4 parameters - vocal tract shape"),
    ("chorusDepth",            "V5 parameters - dual-oscillator chorus (vocal fold asymmetry)"),
]

MARKER_BEGIN_TAG = (
    "AUTO-GENERATED FROM src/voicingTone.h - DO NOT EDIT MANUALLY "
    "(regenerate: python tools/gen_voicing_tone.py)"
)
MARKER_END_TAG = "END AUTO-GENERATED"


class Field:
    __slots__ = ("name", "comment")
    def __init__(self, name: str, comment: str = "") -> None:
        self.name = name
        self.comment = comment


def parse_canonical() -> list[Field]:
    """Extract the ordered list of double-typed fields from the canonical struct."""
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
    # Match `double NAME;` declarations only (skips the uint32 ABI header).
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
        sys.exit(f"ERROR: parsed zero double fields from {STRUCT_NAME}")
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


# ---------- Renderers (fields-only; struct wrapper stays in target files) ----------

def render_cpp_fields(fields: list[Field], indent: str = "  ") -> str:
    """Render just the field declarations for a C/C++ struct body."""
    lines: list[str] = []
    for title, group in grouped(fields):
        if title:
            lines.append(f"{indent}// {title}")
        for f in group:
            line = f"{indent}double {f.name};"
            if f.comment:
                line += f"  // {f.comment}"
            lines.append(line)
    return "\n".join(lines)


def render_python_fields(fields: list[Field], ctypes_prefix: str, indent: str = "        ") -> str:
    """Render just the entry tuples for a ctypes Structure._fields_ list."""
    lines: list[str] = []
    for title, group in grouped(fields):
        if title:
            lines.append(f"{indent}# {title}")
        for f in group:
            line = f'{indent}("{f.name}", {ctypes_prefix}),'
            if f.comment:
                line += f"  # {f.comment}"
            lines.append(line)
    return "\n".join(lines)


# ---------- Target file table ----------

TARGETS: list[dict] = [
    dict(
        path="src/frontend/nvspFrontend.h",
        comment="//",
        render=lambda f: render_cpp_fields(f, indent="  "),
    ),
    dict(
        path="tools/tgsbPhonemeEditorWin32/tgsb_runtime.h",
        comment="//",
        render=lambda f: render_cpp_fields(f, indent="  "),
    ),
    dict(
        path="speechPlayer.py",
        comment="#",
        render=lambda f: render_python_fields(f, "c_double", indent="        "),
    ),
    dict(
        path="nvdaAddon/synthDrivers/tgSpeechBox/_frontend.py",
        comment="#",
        render=lambda f: render_python_fields(f, "ctypes.c_double", indent="        "),
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
    # Search for the END marker only AFTER the BEGIN position. Both this script
    # and gen_frame_ex.py share the same END marker text ("END AUTO-GENERATED"),
    # so files with multiple auto-generated regions need positional disambiguation.
    em = end_re.search(text, bm.end())
    if not em:
        raise RuntimeError("END marker not found after BEGIN (expected `{} <<< {} <<<`)"
                           .format(comment_prefix, MARKER_END_TAG))
    line_start = text.rfind("\n", 0, em.start()) + 1
    end_indent = text[line_start : em.start()]
    return text[: bm.end()] + "\n" + generated + "\n" + end_indent + text[em.start() :]


def main() -> None:
    check_only = "--check" in sys.argv
    fields = parse_canonical()
    print(f"Parsed {len(fields)} double fields from {CANONICAL.relative_to(REPO_ROOT)}")

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
            f"\nVoicingTone mirrors out of sync ({len(drift)} file(s)). Regenerate with:",
            file=sys.stderr,
        )
        print("  python tools/gen_voicing_tone.py", file=sys.stderr)
        sys.exit(1)
    if check_only:
        print("All VoicingTone mirrors in sync with src/voicingTone.h.")


if __name__ == "__main__":
    main()

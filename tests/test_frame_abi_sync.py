"""Tripwire: the test wrapper's Frame ctypes mirror matches nvspFrontend.h.

tests/_test_frontend.py hand-declares the base Frame struct (47 doubles,
ABI-frozen since v1 -- new fields go into FrameEx, which the wrapper treats
as opaque and which gen_frame_ex.py keeps in sync everywhere it IS declared).
That leaves exactly one hand-maintained ABI mirror in the repo: if anyone
ever touches nvspFrontend_Frame, the wrapper's positional double list would
misalign and every captured field in every test would silently read garbage
(the issue-#93 bug class). This test makes that failure loud instead.
"""
import pathlib
import re

import _test_frontend

HEADER = (pathlib.Path(__file__).resolve().parent.parent
          / "src" / "frontend" / "nvspFrontend.h")


def _canonical_frame_fields():
    text = HEADER.read_text(encoding="utf-8")
    m = re.search(
        r"typedef\s+struct\s+nvspFrontend_Frame\s*\{(.*?)\}\s*nvspFrontend_Frame\s*;",
        text, re.DOTALL)
    assert m, "nvspFrontend_Frame struct not found in nvspFrontend.h"
    names = []
    for line in m.group(1).splitlines():
        line = line.split("//")[0].strip()
        dm = re.match(r"double\s+(.+);$", line)
        if not dm:
            continue
        names.extend(n.strip() for n in dm.group(1).split(","))
    return names


def test_frame_mirror_matches_header():
    canonical = _canonical_frame_fields()
    mirrored = [name for name, _ in _test_frontend.Frame._fields_]
    assert mirrored == canonical, (
        "tests/_test_frontend.py Frame mirror is out of sync with "
        "src/frontend/nvspFrontend.h -- positional doubles WILL misalign. "
        f"header={canonical} wrapper={mirrored}"
    )
    # Frame is ABI-frozen at 47 doubles by design; growth belongs in FrameEx.
    assert len(canonical) == 47, (
        f"nvspFrontend_Frame changed size ({len(canonical)} fields)! "
        "It is ABI-frozen -- new parameters belong in FrameEx "
        "(and run tools/gen_frame_ex.py for its mirrors)."
    )

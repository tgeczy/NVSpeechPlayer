"""Minimal YAML helpers for NV Speech Player language packs.

NV Speech Player language packs are YAML files (packs/lang/*.yaml). The full
YAML grammar is intentionally *not* implemented here; we only need to read and
write the top-level ``settings:`` mapping (scalar values).

Why not PyYAML?
    NVDA add-ons can't assume third-party Python dependencies are available.

So this module sticks to a conservative, line-based approach that preserves
most of the original file formatting.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Tuple


_SETTINGS_HEADER_RE = re.compile(r"^settings\s*:\s*(?:#.*)?$")
_KEY_VALUE_RE = re.compile(
    r"^(?P<indent>\s*)(?P<key>[-A-Za-z0-9_]+)\s*:\s*(?P<value>.*?)(?P<comment>\s+#.*)?$"
)
_NUM_RE = re.compile(r"^-?(?:\d+)(?:\.\d+)?$")


def normalizeLangTag(tag: str) -> str:
    """Normalize a language tag to the form used by pack filenames.

    - NVDA sometimes uses underscores and upper-case region parts (e.g. en_US).
    - Pack files use hyphen-separated tags (e.g. en-us.yaml).
    """
    tag = (tag or "").strip()
    if not tag:
        return "default"
    # Packs use lowercase, hyphen-separated tags.
    tag = tag.replace("_", "-")
    tag = tag.lower()
    return tag


def getLangDir(packsDir: str) -> str:
    return os.path.join(packsDir, "lang")


def iterLangTagChain(langTag: str) -> Iterable[str]:
    """Yield the inheritance chain used by the frontend.

    For example:
        "en-us-nyc" -> ["default", "en", "en-us", "en-us-nyc"]
    """
    langTag = normalizeLangTag(langTag)
    yield "default"
    if langTag == "default":
        return
    parts = langTag.split("-")
    for i in range(1, len(parts) + 1):
        yield "-".join(parts[:i])


def langYamlPath(packsDir: str, langTag: str) -> str:
    langTag = normalizeLangTag(langTag)
    return os.path.join(getLangDir(packsDir), f"{langTag}.yaml")


@dataclass
class SettingsSection:
    settings: Dict[str, str]
    """Mapping of key -> raw scalar string (unquoted/unescaped is preserved as in file)."""

    # Line indices that bound the mapping in the source file.
    startLine: Optional[int] = None
    endLine: Optional[int] = None
    """Slice range [startLine, endLine) within the original file lines."""

    keyLineIndex: Optional[Dict[str, int]] = None
    """If parsed from a file, maps keys to their line index (within the file)."""


def _stripBom(text: str) -> str:
    # Some editors may write UTF-8 BOM.
    return text[1:] if text.startswith("\ufeff") else text


def _readFileText(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return _stripBom(f.read())


def _writeFileText(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def parseSettingsSectionFromText(text: str) -> SettingsSection:
    """Parse the top-level ``settings:`` mapping from YAML text.

    Only simple one-line key/value scalars are recognized.
    """
    lines = text.splitlines()
    settings: Dict[str, str] = {}
    keyLineIndex: Dict[str, int] = {}
    startLine: Optional[int] = None
    endLine: Optional[int] = None

    # Find a top-level "settings:" header.
    for i, line in enumerate(lines):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line.startswith(" ") or line.startswith("\t"):
            continue
        if _SETTINGS_HEADER_RE.match(line.strip()):
            startLine = i
            break

    if startLine is None:
        return SettingsSection(settings=settings, startLine=None, endLine=None, keyLineIndex=None)

    # Parse indented key/value pairs until we hit a new top-level key.
    i = startLine + 1
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if not stripped or line.lstrip().startswith("#"):
            i += 1
            continue
        # End of the settings mapping when indentation returns to column 0.
        if not (line.startswith(" ") or line.startswith("\t")):
            break

        m = _KEY_VALUE_RE.match(line)
        if m:
            key = m.group("key")
            val = (m.group("value") or "").strip()
            settings[key] = val
            keyLineIndex[key] = i
        i += 1

    endLine = i
    return SettingsSection(
        settings=settings,
        startLine=startLine,
        endLine=endLine,
        keyLineIndex=keyLineIndex,
    )


def parseSettingsSectionFromFile(path: str) -> SettingsSection:
    if not os.path.isfile(path):
        return SettingsSection(settings={}, startLine=None, endLine=None, keyLineIndex=None)
    try:
        return parseSettingsSectionFromText(_readFileText(path))
    except Exception:
        # Corrupt/unsupported YAML; treat as empty.
        return SettingsSection(settings={}, startLine=None, endLine=None, keyLineIndex=None)


def getEffectiveSettings(packsDir: str, langTag: str) -> Dict[str, str]:
    """Return effective settings for a language tag (merged by inheritance)."""
    effective: Dict[str, str] = {}
    for tag in iterLangTagChain(langTag):
        sec = parseSettingsSectionFromFile(langYamlPath(packsDir, tag))
        effective.update(sec.settings)
    return effective


def getEffectiveSettingValue(packsDir: str, langTag: str, key: str) -> Optional[str]:
    """Return the effective value for a given setting key.

    Returns None if the key is not present in any pack layer.
    """
    key = (key or "").strip()
    if not key:
        return None
    val: Optional[str] = None
    for tag in iterLangTagChain(langTag):
        sec = parseSettingsSectionFromFile(langYamlPath(packsDir, tag))
        if key in sec.settings:
            val = sec.settings[key]
    return val


def getSettingSource(packsDir: str, langTag: str, key: str) -> Optional[str]:
    """Return which pack layer provides the effective value for ``key``.

    Returns the tag ("default", "en", "en-us", ...) or None if not found.
    """
    key = (key or "").strip()
    if not key:
        return None
    found: Optional[str] = None
    for tag in iterLangTagChain(langTag):
        sec = parseSettingsSectionFromFile(langYamlPath(packsDir, tag))
        if key in sec.settings:
            found = tag
    return found


def listKnownSettingKeys(packsDir: str) -> List[str]:
    """List known setting keys.

    Uses packs/lang/default.yaml as the source of truth.
    """
    defaultPath = langYamlPath(packsDir, "default")
    sec = parseSettingsSectionFromFile(defaultPath)
    keys = sorted(sec.settings.keys())
    return keys


def _formatYamlScalar(value) -> str:
    """Format a value as a safe-ish YAML scalar."""
    # Preserve values already passed as a bool.
    if isinstance(value, bool):
        return "true" if value else "false"

    if value is None:
        return "null"

    s = str(value)
    s = s.strip()

    if not s:
        return '""'

    lower = s.lower()
    if lower in {"true", "false", "null", "~"}:
        return lower

    # YAML 1.1 boolean-like scalars; quote to avoid accidental coercion.
    if lower in {"yes", "no", "on", "off"}:
        return f'"{lower}"'

    if _NUM_RE.match(s):
        return s

    # If the user already quoted it, keep it.
    if len(s) >= 2 and s[0] == s[-1] and s[0] in ("'", '"'):
        return s

    # Quote if it contains YAML-significant characters.
    if any(ch in s for ch in [":", "#", "{", "}", "[", "]", ",", "&", "*", "!", "|", ">", "@", "`"]):
        escaped = s.replace("\\", "\\\\").replace('"', '\\"')
        return f'"{escaped}"'

    return s


def upsertSetting(packsDir: str, langTag: str, key: str, value) -> None:
    """Insert or update ``settings.<key>`` in the most specific language pack file.

    If the target language YAML does not exist, it will be created with only a
    ``settings:`` section.
    """
    key = (key or "").strip()
    if not key:
        raise ValueError("key is required")

    langTag = normalizeLangTag(langTag)
    targetPath = langYamlPath(packsDir, langTag)
    yamlValue = _formatYamlScalar(value)

    if not os.path.isfile(targetPath):
        _writeFileText(targetPath, f"settings:\n  {key}: {yamlValue}\n")
        return

    text = _readFileText(targetPath)
    lines = text.splitlines(True)  # keep line endings
    sec = parseSettingsSectionFromText(text)

    # If no settings section exists, append one at the end.
    if sec.startLine is None or sec.endLine is None:
        if text and not text.endswith("\n"):
            lines.append("\n")
        lines.append("settings:\n")
        lines.append(f"  {key}: {yamlValue}\n")
        _writeFileText(targetPath, "".join(lines))
        return

    # Update existing key if present.
    keyLineIndex = (sec.keyLineIndex or {}).get(key)
    if keyLineIndex is not None:
        m = _KEY_VALUE_RE.match(lines[keyLineIndex].rstrip("\n"))
        indent = "  "
        comment = ""
        if m:
            indent = m.group("indent") or indent
            comment = m.group("comment") or ""
        lines[keyLineIndex] = f"{indent}{key}: {yamlValue}{comment}\n"
        _writeFileText(targetPath, "".join(lines))
        return

    # Otherwise insert before the end of the settings section.
    insertAt = sec.endLine
    # Keep indentation consistent with other keys if possible.
    indent = "  "
    if sec.keyLineIndex:
        # Take the indentation of the first parsed key.
        firstLineIdx = next(iter(sec.keyLineIndex.values()))
        m = _KEY_VALUE_RE.match(lines[firstLineIdx].rstrip("\n"))
        if m:
            indent = m.group("indent") or indent

    lines.insert(insertAt, f"{indent}{key}: {yamlValue}\n")
    _writeFileText(targetPath, "".join(lines))


def removeSettingOverride(packsDir: str, langTag: str, key: str) -> None:
    """Remove ``settings.<key>`` from the specified language file, if present."""
    key = (key or "").strip()
    if not key:
        return

    langTag = normalizeLangTag(langTag)
    targetPath = langYamlPath(packsDir, langTag)
    if not os.path.isfile(targetPath):
        return

    text = _readFileText(targetPath)
    lines = text.splitlines(True)
    sec = parseSettingsSectionFromText(text)
    if sec.startLine is None or sec.keyLineIndex is None:
        return

    idx = sec.keyLineIndex.get(key)
    if idx is None:
        return

    del lines[idx]
    _writeFileText(targetPath, "".join(lines))


def parseBool(value, default: bool = False) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    s = str(value).strip().lower()
    if s in {"1", "true", "t", "yes", "y", "on"}:
        return True
    if s in {"0", "false", "f", "no", "n", "off"}:
        return False
    return default

# -----------------------------------------------------------------------------
# Backwards-compatible aliases
# -----------------------------------------------------------------------------
# Early driver patches used different helper names. Keep aliases so that older
# driver builds don't crash if they call the old API.


def setSettingValue(*, packsDir: str, langTag: str, key: str, value) -> None:
    """Compatibility wrapper for older driver patches.

    Equivalent to ``upsertSetting(packsDir, langTag, key, value)``.
    """
    upsertSetting(packsDir, langTag, key, value)


def coerceToBool(value, default: bool = False) -> bool:
    """Compatibility wrapper for older driver patches.

    Equivalent to ``parseBool(value, default)``.
    """
    return parseBool(value, default)

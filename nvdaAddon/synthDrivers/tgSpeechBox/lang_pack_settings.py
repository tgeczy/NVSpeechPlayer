# -*- coding: utf-8 -*-
"""YAML-backed language pack settings — mixin for SynthDriver.

Provides:
- Mode OrderedDicts (stop closure, spelling diphthong, pitch modes, etc.)
- _getLangPackBool / _getLangPackStr / _setLangPackSetting / _choiceToIdStr
- _refreshLangPackSettingsCache
- _enableLangPackWrites / _scheduleEnableLangPackWrites
- _getCurrentLangTag
- Generated accessors for all _LANG_PACK_SPECS entries
- Custom legacyPitchMode accessor (handles bool→enum migration)
"""

from __future__ import annotations

from collections import OrderedDict

from logHandler import log
from synthDriverHandler import VoiceInfo

try:
    import addonHandler
    addonHandler.initTranslation()
except Exception:
    def _(s): return s


class LangPackSettingsMixin:
    """YAML-backed language pack settings for SynthDriver."""

    _STOP_CLOSURE_MODES = OrderedDict(
        (
            ("always", VoiceInfo("always", _("Always"))),
            ("after-vowel", VoiceInfo("after-vowel", _("After vowel"))),
            ("vowel-and-cluster", VoiceInfo("vowel-and-cluster", _("Vowel and cluster"))),
            ("none", VoiceInfo("none", _("None"))),
        )
    )

    _SPELLING_DIPHTHONG_MODES = OrderedDict(
        (
            ("none", VoiceInfo("none", _("None"))),
            ("monophthong", VoiceInfo("monophthong", _("Monophthong"))),
        )
    )

    _TONE_CONTOURS_MODES = OrderedDict(
        (
            ("absolute", VoiceInfo("absolute", _("Absolute"))),
            ("relative", VoiceInfo("relative", _("Relative"))),
        )
    )

    _COARTICULATION_ADJACENCY_MODES = OrderedDict(
        (
            ("0", VoiceInfo("0", _("Immediate neighbors only"))),
            ("1", VoiceInfo("1", _("Allow C_V (one consonant)"))),
            ("2", VoiceInfo("2", _("Allow CC_V (two consonants)"))),
        )
    )

    _LEGACY_PITCH_MODES = OrderedDict(
        (
            ("espeak_style", VoiceInfo("espeak_style", _("eSpeak style"))),
            ("fujisaki_style", VoiceInfo("fujisaki_style", _("Fujisaki"))),
            ("impulse_style", VoiceInfo("impulse_style", _("Impulse"))),
            ("klatt_style", VoiceInfo("klatt_style", _("Klatt"))),
            ("legacy", VoiceInfo("legacy", _("Classic"))),
        )
    )

    # ---- Startup guard for YAML writes (see __init__) ----

    def _enableLangPackWrites(self) -> None:
        """Re-enable writing language-pack settings back to YAML."""
        self._suppressLangPackWrites = False

    def _scheduleEnableLangPackWrites(self) -> None:
        """Schedule re-enabling YAML writes after NVDA finishes config replay."""
        try:
            import core
            core.callLater(0, self._enableLangPackWrites)
        except Exception:
            self._enableLangPackWrites()

    # ---- Language-pack (YAML) helpers ----

    def _getCurrentLangTag(self) -> str:
        """Return the current resolved language tag in pack file format (lowercase, hyphen)."""
        return str(getattr(self, "_resolvedLang", "en-us") or "en-us").strip().lower().replace("_", "-")

    def _refreshLangPackSettingsCache(self) -> None:
        """Rebuild the cached effective YAML ``settings:`` map for the current language."""
        try:
            from . import langPackYaml

            packsDir = getattr(self, "_packsDir", None)
            if not packsDir:
                self._langPackSettingsCache = {}
                return

            self._langPackSettingsCache = langPackYaml.getEffectiveSettings(
                packsDir=packsDir,
                langTag=self._getCurrentLangTag(),
            )

            # Clear previous error key on success.
            if getattr(self, "_lastLangPackCacheErrorKey", None) is not None:
                self._lastLangPackCacheErrorKey = None
        except Exception as e:
            # Avoid "log spam" if a corrupt YAML causes repeated refresh failures.
            try:
                tag = self._getCurrentLangTag()
            except Exception:
                tag = None
            errKey = (tag, type(e).__name__, str(e))
            if getattr(self, "_lastLangPackCacheErrorKey", None) != errKey:
                log.error("TGSpeechBox: failed to read language-pack settings for %r", tag, exc_info=True)
                self._lastLangPackCacheErrorKey = errKey
            self._langPackSettingsCache = {}

    def _getLangPackBool(self, key: str, default: bool = False) -> bool:
        # Users may edit YAML on disk (either via our settings panel, or via a
        # text editor). Refresh the cache opportunistically so the values shown
        # in NVDA's GUI don't go stale and then get written back to disk.
        self._refreshLangPackSettingsCache()
        try:
            from . import langPackYaml

            raw = getattr(self, "_langPackSettingsCache", {}).get(key)
            return langPackYaml.parseBool(raw, default)
        except Exception:
            return default

    def _getLangPackStr(self, key: str, default: str = "") -> str:
        self._refreshLangPackSettingsCache()
        raw = getattr(self, "_langPackSettingsCache", {}).get(key)
        if raw is None:
            return default
        return str(raw)

    def _setLangPackSetting(self, key: str, value: object) -> None:
        """Write a language-pack ``settings:`` key and reload packs."""
        try:
            from . import langPackYaml

            # During driver initialization NVDA may replay persisted settings
            # from config.conf by calling our property setters. For YAML-backed
            # language-pack settings we treat YAML as authoritative, so we
            # suppress writes during that replay window to avoid overwriting
            # edits made via Notepad or our settings panel.
            if getattr(self, "_suppressLangPackWrites", False):
                log.debug("TGSpeechBox: _setLangPackSetting(%s, %r) — SUPPRESSED (init replay)", key, value)
                return

            # Ensure our effective-value comparison reflects the current files
            # on disk (the YAML may have been edited externally).
            self._refreshLangPackSettingsCache()

            packsDir = getattr(self, "_packsDir", None)
            if not packsDir:
                log.debug("TGSpeechBox: _setLangPackSetting(%s) — no packsDir", key)
                return

            langTag = self._getCurrentLangTag()
            log.debug("TGSpeechBox: _setLangPackSetting(%s, %r) lang=%r resolved=%r tag=%r",
                       key, value, getattr(self, "_language", "?"), getattr(self, "_resolvedLang", "?"), langTag)

            # Avoid churn if no effective change.
            cur = getattr(self, "_langPackSettingsCache", {}).get(key)
            try:
                if isinstance(value, bool):
                    # If cur is None, the key doesn't exist in YAML yet - always write it.
                    if cur is not None and langPackYaml.parseBool(cur, default=value) == value:
                        log.debug("TGSpeechBox: _setLangPackSetting(%s) — no change (bool cur=%r)", key, cur)
                        return
                else:
                    # Packs store scalars as strings; normalize whitespace for comparison.
                    if cur is not None and str(cur).strip() == str(value).strip():
                        log.debug("TGSpeechBox: _setLangPackSetting(%s) — no change (cur=%r, val=%r)", key, cur, value)
                        return
            except Exception:
                log.debug(
                    "TGSpeechBox: error comparing language-pack setting %s", key, exc_info=True
                )

            langPackYaml.upsertSetting(
                packsDir=packsDir,
                langTag=langTag,
                key=key,
                value=value,
            )
            # Reload so the frontend re-reads updated YAML.
            self.reloadLanguagePack(langTag)
        except Exception:
            log.error("TGSpeechBox: failed to update language-pack setting %s", key, exc_info=True)

    def _choiceToIdStr(self, value: object) -> str:
        """Return the underlying id string for a combo-box choice.

        NVDA's settings UI may pass either the raw id string or an object
        (for example a VoiceInfo).
        """
        if value is None:
            return ""
        # Common attribute spellings used across NVDA versions.
        for attr in ("id", "ID"):
            try:
                v = getattr(value, attr)
            except Exception:
                continue
            if v is not None and v != "":
                return str(v)
        return str(value)

    # ---- legacyPitchMode (custom accessor for bool→enum migration) ----

    def _get_legacyPitchMode(self):
        try:
            val = self._getLangPackStr("legacyPitchMode", default="")
            # Handle missing/empty value
            if not val:
                return "espeak_style"
            # Handle old boolean values (YAML bools become "True"/"False" strings)
            if val in ("true", "True", "1"):
                return "legacy"  # Old "true" meant classic/legacy mode
            if val in ("false", "False", "0"):
                return "espeak_style"  # Old "false" meant espeak style
            # Check if it's a valid new-style value
            if val in self._LEGACY_PITCH_MODES:
                return val
            # Unknown value, return default
            return "espeak_style"
        except Exception:
            return "espeak_style"

    def _set_legacyPitchMode(self, val):
        try:
            self._setLangPackSetting("legacyPitchMode", self._choiceToIdStr(val))
        except Exception:
            pass

    def _get_availableLegacyPitchModes(self):
        return self._LEGACY_PITCH_MODES

    def _get_availableLegacypitchmodes(self):
        return self._LEGACY_PITCH_MODES

    # ---- legacyPitchInflectionScale (YAML-backed slider) ----

    def _get_legacyPitchInflectionScale(self):
        try:
            raw = self._getLangPackStr("legacyPitchInflectionScale", default="")
            if raw:
                fval = float(raw)
                # Map 0.0–2.0 → 0–100
                return int(max(0, min(100, fval / 2.0 * 100.0)))
            return 29  # default 0.58
        except Exception:
            return 29

    def _set_legacyPitchInflectionScale(self, val):
        try:
            sliderVal = int(val)
            if sliderVal == getattr(self, "_curLegacyPitchInflectionScale", 29):
                return
            self._curLegacyPitchInflectionScale = sliderVal
            # Map 0–100 → 0.0–2.0
            yamlVal = round(sliderVal / 100.0 * 2.0, 3)
            self._setLangPackSetting("legacyPitchInflectionScale", yamlVal)
        except Exception:
            pass


# ── Generated accessors for _LANG_PACK_SPECS ────────────────────────────

def _makeLangPackAccessors(attrName, yamlKey, kind="str", default=None, choices=None):
    """Generate _get/_set (and available* when needed) methods for YAML-backed settings."""

    def getter(self, _key=yamlKey, _default=default, _kind=kind):
        try:
            if _kind == "bool":
                return self._getLangPackBool(_key, default=_default)
            return self._getLangPackStr(_key, default=_default)
        except Exception:
            return _default

    def setter(self, val, _key=yamlKey, _kind=kind):
        try:
            if _kind == "bool":
                self._setLangPackSetting(_key, bool(val))
            else:
                self._setLangPackSetting(_key, self._choiceToIdStr(val))
        except Exception:
            # Never crash during settings application
            pass

    accessors = {
        f"_get_{attrName}": getter,
        f"_set_{attrName}": setter,
    }

    if choices is not None:
        camelPlural = attrName[0].upper() + attrName[1:] + "s"

        def availGetter(self, _choices=choices):
            return _choices

        camelName = f"_get_available{camelPlural}"
        accessors[camelName] = availGetter
        # NVDA's settingsDialogs uses attrName.capitalize() which
        # uppercases the first char and lowercases the rest.  Generate
        # an alias that matches that transformation.
        nvdaPlural = attrName.capitalize() + "s"
        aliasName = f"_get_available{nvdaPlural}"
        if aliasName != camelName:
            accessors[aliasName] = availGetter

    return accessors


# Settings specs: (attrName, yamlKey, kind, default, choices)
_LANG_PACK_SPECS = (
    ("stopClosureMode", "stopClosureMode", "enum", "vowel-and-cluster", LangPackSettingsMixin._STOP_CLOSURE_MODES),
    ("stopClosureClusterGapsEnabled", "stopClosureClusterGapsEnabled", "bool", False, None),
    ("stopClosureAfterNasalsEnabled", "stopClosureAfterNasalsEnabled", "bool", False, None),
    ("stopClosureNasalToStopGapMs", "stopClosureNasalToStopGapMs", "float", 0.0, None),
    ("stopClosureNasalToStopFadeMs", "stopClosureNasalToStopFadeMs", "float", 2.0, None),
    ("autoTieDiphthongs", "autoTieDiphthongs", "bool", False, None),
    ("autoDiphthongOffglideToSemivowel", "autoDiphthongOffglideToSemivowel", "bool", False, None),
    ("segmentBoundarySkipVowelToVowel", "segmentBoundarySkipVowelToVowel", "bool", False, None),
    ("segmentBoundarySkipVowelToLiquid", "segmentBoundarySkipVowelToLiquid", "bool", False, None),
    ("spellingDiphthongMode", "spellingDiphthongMode", "enum", "none", LangPackSettingsMixin._SPELLING_DIPHTHONG_MODES),
    ("postStopAspirationEnabled", "postStopAspirationEnabled", "bool", False, None),
    # --- Coarticulation settings ---
    ("coarticulationEnabled", "coarticulationEnabled", "bool", False, None),
    ("coarticulationFadeIntoConsonants", "coarticulationFadeIntoConsonants", "bool", False, None),
    ("coarticulationSteadyState", "coarticulationSteadyState", "bool", False, None),
    ("coarticulationTransInMs", "coarticulationTransInMs", "float", 35.0, None),
    ("coarticulationTransOutMs", "coarticulationTransOutMs", "float", 40.0, None),
    # --- Svarabhakti settings (#113: the inserted vocoid echoes its vowel) ---
    ("svarabhaktiInheritEnabled", "svarabhaktiInheritEnabled", "bool", False, None),
    ("svarabhaktiVowelWeight", "svarabhaktiVowelWeight", "float", 0.8, None),
    ("svarabhaktiDurationScale", "svarabhaktiDurationScale", "float", 0.5, None),
    ("svarabhaktiAmpScale", "svarabhaktiAmpScale", "float", 0.85, None),
    ("svarabhaktiTapBlend", "svarabhaktiTapBlend", "float", 0.0, None),
    ("svarabhaktiTapDip", "svarabhaktiTapDip", "float", 1.0, None),
    ("coarticulationVelarPinchEnabled", "coarticulationVelarPinchEnabled", "bool", False, None),
    ("coarticulationGraduated", "coarticulationGraduated", "bool", False, None),
    ("coarticulationAdjacencyMaxConsonants", "coarticulationAdjacencyMaxConsonants", "enum", "2", LangPackSettingsMixin._COARTICULATION_ADJACENCY_MODES),
    # --- Phrase-final lengthening settings ---
    ("phraseFinalLengtheningEnabled", "phraseFinalLengtheningEnabled", "bool", False, None),
    ("phraseFinalLengtheningNucleusOnlyMode", "phraseFinalLengtheningNucleusOnlyMode", "bool", False, None),
    # --- Single-word tuning settings ---
    ("singleWordTuningEnabled", "singleWordTuningEnabled", "bool", True, None),
    ("singleWordClauseTypeOverrideCommaOnly", "singleWordClauseTypeOverrideCommaOnly", "bool", True, None),
    # --- Microprosody settings ---
    ("microprosodyEnabled", "microprosodyEnabled", "bool", False, None),
    ("microprosodyVoicelessF0RaiseEnabled", "microprosodyVoicelessF0RaiseEnabled", "bool", False, None),
    ("microprosodyVoicedF0LowerEnabled", "microprosodyVoicedF0LowerEnabled", "bool", False, None),
    # --- Rate compensation settings ---
    ("rateCompEnabled", "rateCompEnabled", "bool", False, None),
    # --- Nasalization settings ---
    ("nasalizationAnticipatoryEnabled", "nasalizationAnticipatoryEnabled", "bool", False, None),
    # --- Liquid dynamics settings ---
    ("liquidDynamicsEnabled", "liquidDynamics.enabled", "bool", False, None),
    # --- Length contrast settings ---
    ("lengthContrastEnabled", "lengthContrast.enabled", "bool", False, None),
    # --- Positional allophones settings ---
    ("positionalAllophonesEnabled", "positionalAllophones.enabled", "bool", False, None),
    ("positionalAllophonesGlottalReinforcementEnabled", "positionalAllophones.glottalReinforcement.enabled", "bool", False, None),
    # --- Boundary smoothing settings ---
    ("boundarySmoothingEnabled", "boundarySmoothing.enabled", "bool", False, None),
    # --- Trajectory limit settings ---
    ("trajectoryLimitEnabled", "trajectoryLimit.enabled", "bool", False, None),
    ("trajectoryLimitApplyAcrossWordBoundary", "trajectoryLimit.applyAcrossWordBoundary", "bool", False, None),
    # legacyPitchMode has custom accessors in the mixin (for bool→enum migration)
    ("tonal", "tonal", "bool", False, None),
    ("toneDigitsEnabled", "toneDigitsEnabled", "bool", False, None),
    ("toneContoursMode", "toneContoursMode", "enum", "absolute", LangPackSettingsMixin._TONE_CONTOURS_MODES),
    ("stripAllophoneDigits", "stripAllophoneDigits", "bool", False, None),
    ("stripHyphen", "stripHyphen", "bool", False, None),
    # --- Year splitting ---
    ("yearSplitting", "yearSplittingEnabled", "bool", False, None),
    # --- Thousands separator ---
    ("thousandsSeparatorCommaToSpace", "thousandsSeparatorCommaToSpace", "bool", False, None),
)

# Inject generated accessors onto the mixin class.
for _attrName, _yamlKey, _kind, _default, _choices in _LANG_PACK_SPECS:
    for _methName, _meth in _makeLangPackAccessors(
        _attrName,
        _yamlKey,
        kind=_kind,
        default=_default,
        choices=_choices,
    ).items():
        setattr(LangPackSettingsMixin, _methName, _meth)

# Clean up module namespace.
del _attrName, _yamlKey, _kind, _default, _choices, _methName, _meth

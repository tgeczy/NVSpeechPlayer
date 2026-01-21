"""NVDA Settings panel for editing NV Speech Player language-pack YAML settings.

This provides the UX described in readme.md:
  - Choose a language tag
  - Adjust a few common settings via dedicated edit fields
  - For everything else: choose a setting key (combo) and edit its value
  - Apply the pending edits when the user presses OK in NVDA's Settings dialog

The panel intentionally avoids a full YAML UI; it only edits ``settings:`` keys.

Implementation notes:
  - NVDA can import synth drivers before the GUI is fully initialized.
    Importing wx/gui modules at *module import time* can therefore fail.
  - To keep this robust across NVDA 2024.1 .. 2026.1, all GUI imports and the
    SettingsPanel subclass definition are done lazily when registerSettingsPanel
    is called (typically from SynthDriver.__init__).
"""

from __future__ import annotations

import os
from typing import Dict, Optional


def _lazyInitTranslation():
    """Return a callable translation function ``_``.

    NVDA add-ons usually do:
        import addonHandler
        addonHandler.initTranslation()
    which installs the translation function into ``builtins._``.

    Some NVDA versions also expose a *module* named ``gettext`` under addonHandler,
    so we must not return ``addonHandler.gettext`` directly (it may be a module,
    which would trigger: TypeError: 'module' object is not callable).
    """
    try:
        import addonHandler  # type: ignore

        addonHandler.initTranslation()
        import builtins

        t = getattr(builtins, "_", None)
        if callable(t):
            return t
    except Exception:
        pass

    # Fallback: no translation available.
    return lambda s: s


_ = _lazyInitTranslation()


def _getPacksDir() -> str:
    # This module lives next to synthDrivers/nvSpeechPlayer/__init__.py
    return os.path.join(os.path.dirname(__file__), "packs")


from . import langPackYaml

GitHub_URL = "https://github.com/tgeczy/NVSpeechPlayer"


_PANEL_CLS = None


def _getPanelClass():
    """Return the SettingsPanel subclass, defining it lazily when wx is available."""
    global _PANEL_CLS
    if _PANEL_CLS is not None:
        return _PANEL_CLS

    # Resolve SettingsPanel base across NVDA versions.
    try:
        from gui.settingsDialogs import SettingsPanel as SettingsPanelBase
    except Exception:
        try:
            # Some NVDA builds may relocate panels.
            from gui.settingsPanels import SettingsPanel as SettingsPanelBase  # type: ignore
        except Exception:
            # GUI not ready yet.
            return None

    class NVSpeechPlayerLanguagePacksPanel(SettingsPanelBase):
        title = _("NV Speech Player language packs")

        def __init__(self, *args, **kwargs):
            # NOTE:
            # NVDA's SettingsPanel base class builds the GUI *inside its __init__*
            # (it calls makeSettings via _buildGui). That means attributes used by
            # makeSettings MUST exist before we call super().__init__().
            self._packsDir = _getPacksDir()
            self._knownKeys = []
            # Pending edits are stored per-language so switching the language tag
            # doesn't accidentally apply the previous language's edits.
            #
            #   normalizedLangTag -> { key -> rawValueString }
            self._pending: Dict[str, Dict[str, str]] = {}
            self._currentKey: Optional[str] = None

            # Quick edit fields (key -> wx.TextCtrl)
            self._quickCtrls: Dict[str, object] = {}

            # Guard to prevent EVT_TEXT handlers from recording pending edits when
            # we are programmatically populating controls.
            self._isPopulating = False

            super().__init__(*args, **kwargs)

        # ----- NVDA SettingsPanel hooks -----

        def makeSettings(self, settingsSizer):
            # Import GUI pieces lazily.
            try:
                import wx
                from gui import guiHelper
            except Exception:
                return

            sHelper = guiHelper.BoxSizerHelper(self, sizer=settingsSizer)

            sHelper.addItem(
                wx.StaticText(
                    self,
                    label=_(
                        "Edit language-pack settings (packs/lang/*.yaml) without using Notepad. "
                        "Only the YAML 'settings:' section is edited."
                    ),
                )
            )

            # Language tag control.
            self.langTagCtrl = sHelper.addLabeledControl(
                _("Language tag:"),
                wx.ComboBox,
                choices=self._getLanguageChoices(),
                style=wx.CB_DROPDOWN,
            )
            self.langTagCtrl.Bind(wx.EVT_TEXT, self._onLangTagChanged)
            self.langTagCtrl.Bind(wx.EVT_COMBOBOX, self._onLangTagChanged)

            # --- Common quick settings ---
            sHelper.addItem(wx.StaticText(self, label=_("Common settings (applied on OK):")))
            self._addQuickTextField(
                sHelper,
                label=_("Primary stress divisor (primaryStressDiv):"),
                key="primaryStressDiv",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Secondary stress divisor (secondaryStressDiv):"),
                key="secondaryStressDiv",
            )

            sHelper.addItem(wx.StaticText(self, label=_("Stop-closure timing (ms):")))
            self._addQuickTextField(
                sHelper,
                label=_("Vowel gap (stopClosureVowelGapMs):"),
                key="stopClosureVowelGapMs",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Vowel fade (stopClosureVowelFadeMs):"),
                key="stopClosureVowelFadeMs",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Cluster gap (stopClosureClusterGapMs):"),
                key="stopClosureClusterGapMs",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Cluster fade (stopClosureClusterFadeMs):"),
                key="stopClosureClusterFadeMs",
            )

            # Newer language-pack setting: stressed vowel hiatus timing.
            # These are intentionally kept out of the voice panel (to avoid clutter)
            # and edited here instead.
            sHelper.addItem(wx.StaticText(self, label=_("Stressed vowel hiatus timing (ms):")))
            self._addQuickTextField(
                sHelper,
                label=_("Gap (stressedVowelHiatusGapMs):"),
                key="stressedVowelHiatusGapMs",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Fade (stressedVowelHiatusFadeMs):"),
                key="stressedVowelHiatusFadeMs",
            )

            # Newer language-pack setting: semivowel offglide scaling.
            sHelper.addItem(wx.StaticText(self, label=_("Semivowel / offglide:")))
            self._addQuickTextField(
                sHelper,
                label=_("Offglide scale (semivowelOffglideScale):"),
                key="semivowelOffglideScale",
            )

            # --- Generic key/value editor ---
            sHelper.addItem(wx.StaticText(self, label=_("Other settings:")))

            self._knownKeys = langPackYaml.listKnownSettingKeys(self._packsDir) or []

            # Ensure quick-field keys (and a few important newer keys) are always
            # available in the combo box, even if default.yaml hasn't been updated.
            _extraKeys = [
                "primaryStressDiv",
                "secondaryStressDiv",
                "stopClosureMode",
                "stopClosureVowelGapMs",
                "stopClosureVowelFadeMs",
                "stopClosureClusterGapMs",
                "stopClosureClusterFadeMs",
                "stressedVowelHiatusGapMs",
                "stressedVowelHiatusFadeMs",
                "semivowelOffglideScale",
                "spellingDiphthongMode",
                "segmentBoundarySkipVowelToVowel",
                "segmentBoundarySkipVowelToLiquid",
            ]
            for k in _extraKeys:
                if k not in self._knownKeys:
                    self._knownKeys.append(k)
            if not self._knownKeys:
                # Last-resort fallback for broken/missing default.yaml.
                self._knownKeys = [
                    "primaryStressDiv",
                    "secondaryStressDiv",
                    "stopClosureMode",
                    "stopClosureVowelGapMs",
                    "stopClosureVowelFadeMs",
                    "semivowelOffglideScale",
                    "legacyPitchMode",
                    "stripAllophoneDigits",
                ]

            self.settingKeyCtrl = sHelper.addLabeledControl(
                _("Setting:"),
                wx.ComboBox,
                choices=self._knownKeys,
                style=wx.CB_DROPDOWN | wx.CB_READONLY,
            )
            self.settingKeyCtrl.Bind(wx.EVT_COMBOBOX, self._onSettingKeyChanged)

            self.valueCtrl = sHelper.addLabeledControl(_("Value:"), wx.TextCtrl)
            self.valueCtrl.Bind(wx.EVT_TEXT, self._onGenericValueChanged)

            self.sourceLabel = sHelper.addItem(wx.StaticText(self, label=""))

            # Github link
            self.gitHubButton = sHelper.addItem(wx.Button(self, label=_("Open NV Speech Player on GitHub")))
            self.gitHubButton.Bind(wx.EVT_BUTTON, self._onDonateClick)

            # Initialize defaults.
            self._setInitialLanguageTag()
            self._refreshAllDisplays()
            if self._knownKeys:
                self._setCurrentKey(self._knownKeys[0])

        def onSave(self):
            # Apply all pending changes.
            if not self._pending:
                return

            for langTag, keyMap in list(self._pending.items()):
                for key, val in list((keyMap or {}).items()):
                    if val is None:
                        continue
                    langPackYaml.upsertSetting(self._packsDir, langTag, key, val)

            # Best-effort live reload if NV Speech Player is the active synth.
            try:
                import synthDriverHandler

                synth = synthDriverHandler.getSynth()
                if synth and synth.__class__.__module__.endswith("nvSpeechPlayer"):
                    if hasattr(synth, "reloadLanguagePack"):
                        synth.reloadLanguagePack()
            except Exception:
                pass

            self._pending.clear()

        def onDiscard(self):
            # User hit Cancel.
            self._pending.clear()

        # ----- UI helpers -----

        def _onDonateClick(self, evt):
            """Open the donation link in the default browser."""
            try:
                import wx
                wx.LaunchDefaultBrowser(GitHub_URL)
            except Exception:
                try:
                    import webbrowser
                    webbrowser.open(GitHub_URL)
                except Exception:
                    pass
            evt.Skip()

        def _addQuickTextField(self, sHelper, *, label: str, key: str):
            """Add a labeled wx.TextCtrl bound to a settings key."""
            try:
                import wx
            except Exception:
                return

            ctrl = sHelper.addLabeledControl(label, wx.TextCtrl)
            self._quickCtrls[key] = ctrl
            ctrl.Bind(wx.EVT_TEXT, lambda evt, k=key: self._onQuickValueChanged(evt, k))
            return ctrl

        def _getLanguageChoices(self):
            # List files in packs/lang as suggestions.
            langDir = langPackYaml.getLangDir(self._packsDir)
            choices = []
            try:
                for fn in os.listdir(langDir):
                    if not fn.lower().endswith(".yaml"):
                        continue
                    tag = os.path.splitext(fn)[0]
                    choices.append(tag)
            except Exception:
                pass

            # Ensure default is always present.
            if "default" not in choices:
                choices.append("default")
            return sorted(set(choices))

        def _setInitialLanguageTag(self):
            # Intentionally do *not* auto-detect the language in use.
            # Users may open this panel while a different synth is selected.
            self.langTagCtrl.SetValue("default")

        def _onLangTagChanged(self, evt):
            self._refreshAllDisplays()
            evt.Skip()

        def _onSettingKeyChanged(self, evt):
            key = self.settingKeyCtrl.GetValue()
            self._setCurrentKey(key)
            evt.Skip()

        def _onQuickValueChanged(self, evt, key: str):
            if self._isPopulating:
                evt.Skip()
                return
            try:
                ctrl = evt.GetEventObject()
                langTag = langPackYaml.normalizeLangTag(self.langTagCtrl.GetValue())
                self._pending.setdefault(langTag, {})[key] = ctrl.GetValue()

                # If the generic editor is currently showing this key, refresh its
                # display so the "(pending edit)" source label is accurate.
                if self._currentKey == key:
                    self._updateGenericDisplay()
            except Exception:
                pass
            evt.Skip()

        def _onGenericValueChanged(self, evt):
            if self._isPopulating:
                evt.Skip()
                return
            if not self._currentKey:
                evt.Skip()
                return
            langTag = langPackYaml.normalizeLangTag(self.langTagCtrl.GetValue())
            self._pending.setdefault(langTag, {})[self._currentKey] = self.valueCtrl.GetValue()
            evt.Skip()

        def _setCurrentKey(self, key: str):
            key = (key or "").strip()
            if not key:
                return
            self._currentKey = key
            try:
                self.settingKeyCtrl.SetValue(key)
            except Exception:
                pass
            self._updateGenericDisplay()

        def _refreshAllDisplays(self):
            self._updateQuickDisplays()
            self._updateGenericDisplay()

        def _updateQuickDisplays(self):
            # Populate quick fields based on current language tag.
            langTag = langPackYaml.normalizeLangTag(self.langTagCtrl.GetValue())
            pendingForLang = self._pending.get(langTag, {})

            self._isPopulating = True
            try:
                for key, ctrl in self._quickCtrls.items():
                    if key in pendingForLang:
                        value = pendingForLang[key]
                    else:
                        value = langPackYaml.getEffectiveSettingValue(self._packsDir, langTag, key)
                        if value is None:
                            value = ""

                    try:
                        ctrl.ChangeValue(str(value))
                    except Exception:
                        ctrl.SetValue(str(value))
            finally:
                self._isPopulating = False

        def _updateGenericDisplay(self):
            if not self._currentKey:
                return

            langTag = langPackYaml.normalizeLangTag(self.langTagCtrl.GetValue())
            key = self._currentKey
            pendingForLang = self._pending.get(langTag, {})

            # Prefer pending edits.
            if key in pendingForLang:
                value = pendingForLang[key]
                source = _("(pending edit)")
            else:
                value = langPackYaml.getEffectiveSettingValue(self._packsDir, langTag, key)
                sourceTag = langPackYaml.getSettingSource(self._packsDir, langTag, key)
                source = _(f"(from {sourceTag}.yaml)") if sourceTag else ""
                if value is None:
                    value = ""

            self._isPopulating = True
            try:
                try:
                    self.valueCtrl.ChangeValue(str(value))
                except Exception:
                    self.valueCtrl.SetValue(str(value))

                try:
                    self.sourceLabel.SetLabel(source)
                except Exception:
                    pass
            finally:
                self._isPopulating = False

    _PANEL_CLS = NVSpeechPlayerLanguagePacksPanel
    return _PANEL_CLS


def registerSettingsPanel() -> None:
    """Register the panel with NVDA's Settings dialog (best effort)."""
    panelCls = _getPanelClass()
    if panelCls is None:
        return

    # Import settingsDialogs lazily (NVDA can import synth drivers early).
    try:
        from gui import settingsDialogs
    except Exception:
        return

    # Resolve the dialog class across NVDA versions.
    dlgCls = None
    for name in ("NVDASettingsDialog", "SettingsDialog"):
        dlgCls = getattr(settingsDialogs, name, None)
        if dlgCls:
            break
    if dlgCls is None:
        return

    # Avoid duplicate registration.
    try:
        cats = getattr(dlgCls, "categoryClasses", None)
        if isinstance(cats, (list, tuple)) and panelCls in cats:
            return
    except Exception:
        pass

    # Newer NVDA builds: registerCategory().
    try:
        fn = getattr(dlgCls, "registerCategory", None)
        if callable(fn):
            fn(panelCls)
            return
    except Exception:
        pass

    # Older NVDA builds: categoryClasses list.
    try:
        if hasattr(dlgCls, "categoryClasses"):
            dlgCls.categoryClasses.append(panelCls)
            return
    except Exception:
        pass

    # Last resort: some builds expose module-level helpers.
    for name in ("registerCategory", "registerSettingsPanel"):
        try:
            fn = getattr(settingsDialogs, name, None)
            if callable(fn):
                fn(panelCls)
                return
        except Exception:
            continue

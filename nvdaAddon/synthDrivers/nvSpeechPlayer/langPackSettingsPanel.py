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
ADDON_UPDATE_URL = "https://eurpod.com/synths/nvSpeechPlayer-2026.nvda-addon"
ADDON_VERSION_URL = "https://eurpod.com/nvSpeechPlayer-version.txt"


def _getInstalledAddonVersion() -> str:
    """Read the version from this addon's manifest.ini file.
    
    Returns the version string (e.g., "170") or empty string if not found.
    """
    try:
        # manifest.ini is in the addon root, which is parent of synthDrivers/nvSpeechPlayer
        addonDir = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
        manifestPath = os.path.join(addonDir, "manifest.ini")
        
        if not os.path.isfile(manifestPath):
            return ""
        
        with open(manifestPath, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line.lower().startswith("version"):
                    # Parse "version = 170" or "version=170"
                    parts = line.split("=", 1)
                    if len(parts) == 2:
                        return parts[1].strip()
        return ""
    except Exception:
        return ""


def _parseVersionTuple(versionStr: str):
    """Parse a version string into a tuple of integers for comparison.
    
    Handles versions like "170", "2026.1.27", "2026.1.27.2", etc.
    Non-numeric parts are treated as 0.
    """
    if not versionStr:
        return (0,)
    parts = []
    for part in versionStr.replace("-", ".").replace("_", ".").split("."):
        try:
            parts.append(int(part))
        except ValueError:
            # Non-numeric part (e.g., "beta"), treat as 0
            parts.append(0)
    return tuple(parts) if parts else (0,)


def _isNewerVersion(remoteVersion: str, installedVersion: str) -> bool:
    """Return True if remoteVersion is newer than installedVersion.
    
    Compares version tuples element by element. A shorter version is padded
    with zeros for comparison (e.g., "2026.1.27" vs "2026.1.27.2").
    """
    remote = _parseVersionTuple(remoteVersion)
    installed = _parseVersionTuple(installedVersion)
    
    # Pad shorter tuple with zeros
    maxLen = max(len(remote), len(installed))
    remote = remote + (0,) * (maxLen - len(remote))
    installed = installed + (0,) * (maxLen - len(installed))
    
    return remote > installed


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

        def _addLabeledControlCompat(self, sHelper, label, controlClass, **kwargs):
            """Add a labeled control with compatibility for older NVDA versions.
            
            Older NVDA versions (e.g., 2023.2 with Python 3.10) have a more limited
            addLabeledControl that doesn't support all control types like wx.ComboBox.
            This helper creates the label and control manually if needed.
            """
            import wx
            
            # First, try the modern NVDA approach
            try:
                return sHelper.addLabeledControl(label, controlClass, **kwargs)
            except (NotImplementedError, TypeError, AttributeError):
                pass
            
            # Fallback: create label and control manually
            # Create horizontal sizer for label + control
            hSizer = wx.BoxSizer(wx.HORIZONTAL)
            
            # Create label
            labelCtrl = wx.StaticText(self, label=label)
            hSizer.Add(labelCtrl, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 5)
            
            # Create the control
            ctrl = controlClass(self, **kwargs)
            hSizer.Add(ctrl, 1, wx.EXPAND)
            
            # Add the horizontal sizer to the helper's main sizer
            sHelper.addItem(hSizer)
            
            return ctrl

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
            self.langTagCtrl = self._addLabeledControlCompat(
                sHelper,
                _("Language tag:"),
                wx.ComboBox,
                choices=self._getLanguageChoices(),
                style=wx.CB_DROPDOWN,
            )
            self.langTagCtrl.Bind(wx.EVT_TEXT, self._onLangTagChanged)
            self.langTagCtrl.Bind(wx.EVT_COMBOBOX, self._onLangTagChanged)

            # --- Reset to Factory Defaults button ---
            self.resetDefaultsButton = sHelper.addItem(
                wx.Button(self, label=_("Reset Language Pack Defaults..."))
            )
            self.resetDefaultsButton.Bind(wx.EVT_BUTTON, self._onResetDefaultsClick)

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

            # Newer language-pack setting: trill modulation timing.
            sHelper.addItem(wx.StaticText(self, label=_("Trill modulation (ms):")))
            self._addQuickTextField(
                sHelper,
                label=_("Modulation (trillModulationMs):"),
                key="trillModulationMs",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Fade (trillModulationFadeMs):"),
                key="trillModulationFadeMs",
            )

            # --- Coarticulation settings ---
            sHelper.addItem(wx.StaticText(self, label=_("Coarticulation:")))
            self._addQuickTextField(
                sHelper,
                label=_("Strength (coarticulationStrength):"),
                key="coarticulationStrength",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Transition extent (coarticulationTransitionExtent):"),
                key="coarticulationTransitionExtent",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Word-initial fade scale (coarticulationWordInitialFadeScale):"),
                key="coarticulationWordInitialFadeScale",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Labial F2 locus (coarticulationLabialF2Locus):"),
                key="coarticulationLabialF2Locus",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Alveolar F2 locus (coarticulationAlveolarF2Locus):"),
                key="coarticulationAlveolarF2Locus",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Velar F2 locus (coarticulationVelarF2Locus):"),
                key="coarticulationVelarF2Locus",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Velar pinch threshold (coarticulationVelarPinchThreshold):"),
                key="coarticulationVelarPinchThreshold",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Velar pinch F2 scale (coarticulationVelarPinchF2Scale):"),
                key="coarticulationVelarPinchF2Scale",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Velar pinch F3 (coarticulationVelarPinchF3):"),
                key="coarticulationVelarPinchF3",
            )

            # --- Phrase-final lengthening settings ---
            sHelper.addItem(wx.StaticText(self, label=_("Phrase-final lengthening:")))
            self._addQuickTextField(
                sHelper,
                label=_("Final syllable scale (phraseFinalLengtheningFinalSyllableScale):"),
                key="phraseFinalLengtheningFinalSyllableScale",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Penultimate syllable scale (phraseFinalLengtheningPenultimateSyllableScale):"),
                key="phraseFinalLengtheningPenultimateSyllableScale",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Statement scale (phraseFinalLengtheningStatementScale):"),
                key="phraseFinalLengtheningStatementScale",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Question scale (phraseFinalLengtheningQuestionScale):"),
                key="phraseFinalLengtheningQuestionScale",
            )

            # --- Microprosody settings ---
            sHelper.addItem(wx.StaticText(self, label=_("Microprosody:")))
            self._addQuickTextField(
                sHelper,
                label=_("Voiceless F0 raise Hz (microprosodyVoicelessF0RaiseHz):"),
                key="microprosodyVoicelessF0RaiseHz",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Voiceless F0 raise end Hz (microprosodyVoicelessF0RaiseEndHz):"),
                key="microprosodyVoicelessF0RaiseEndHz",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Voiced F0 lower Hz (microprosodyVoicedF0LowerHz):"),
                key="microprosodyVoicedF0LowerHz",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Minimum vowel ms (microprosodyMinVowelMs):"),
                key="microprosodyMinVowelMs",
            )

            # --- Rate reduction settings ---
            sHelper.addItem(wx.StaticText(self, label=_("Rate reduction:")))
            self._addQuickTextField(
                sHelper,
                label=_("Schwa reduction threshold (rateReductionSchwaReductionThreshold):"),
                key="rateReductionSchwaReductionThreshold",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Schwa min duration ms (rateReductionSchwaMinDurationMs):"),
                key="rateReductionSchwaMinDurationMs",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Schwa scale (rateReductionSchwaScale):"),
                key="rateReductionSchwaScale",
            )

            # --- Nasalization settings ---
            sHelper.addItem(wx.StaticText(self, label=_("Nasalization:")))
            self._addQuickTextField(
                sHelper,
                label=_("Anticipatory amplitude (nasalizationAnticipatoryAmplitude):"),
                key="nasalizationAnticipatoryAmplitude",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Anticipatory blend (nasalizationAnticipatoryBlend):"),
                key="nasalizationAnticipatoryBlend",
            )

            # --- Liquid dynamics settings ---
            sHelper.addItem(wx.StaticText(self, label=_("Liquid dynamics:")))
            self._addQuickTextField(
                sHelper,
                label=_("Lateral onglide F1 delta (liquidDynamics.lateralOnglide.f1Delta):"),
                key="liquidDynamics.lateralOnglide.f1Delta",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Lateral onglide F2 delta (liquidDynamics.lateralOnglide.f2Delta):"),
                key="liquidDynamics.lateralOnglide.f2Delta",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Lateral onglide duration pct (liquidDynamics.lateralOnglide.durationPct):"),
                key="liquidDynamics.lateralOnglide.durationPct",
            )

            # --- Length contrast settings ---
            sHelper.addItem(wx.StaticText(self, label=_("Length contrast:")))
            self._addQuickTextField(
                sHelper,
                label=_("Short vowel ceiling ms (lengthContrast.shortVowelCeilingMs):"),
                key="lengthContrast.shortVowelCeilingMs",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Long vowel floor ms (lengthContrast.longVowelFloorMs):"),
                key="lengthContrast.longVowelFloorMs",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Geminate closure scale (lengthContrast.geminateClosureScale):"),
                key="lengthContrast.geminateClosureScale",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Geminate release scale (lengthContrast.geminateReleaseScale):"),
                key="lengthContrast.geminateReleaseScale",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Pre-geminate vowel scale (lengthContrast.preGeminateVowelScale):"),
                key="lengthContrast.preGeminateVowelScale",
            )

            # --- Positional allophones settings ---
            sHelper.addItem(wx.StaticText(self, label=_("Positional allophones - Stop aspiration:")))
            self._addQuickTextField(
                sHelper,
                label=_("Word-initial stressed (positionalAllophones.stopAspiration.wordInitialStressed):"),
                key="positionalAllophones.stopAspiration.wordInitialStressed",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Word-initial (positionalAllophones.stopAspiration.wordInitial):"),
                key="positionalAllophones.stopAspiration.wordInitial",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Intervocalic (positionalAllophones.stopAspiration.intervocalic):"),
                key="positionalAllophones.stopAspiration.intervocalic",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Word-final (positionalAllophones.stopAspiration.wordFinal):"),
                key="positionalAllophones.stopAspiration.wordFinal",
            )

            sHelper.addItem(wx.StaticText(self, label=_("Positional allophones - Lateral darkness:")))
            self._addQuickTextField(
                sHelper,
                label=_("Pre-vocalic (positionalAllophones.lateralDarkness.preVocalic):"),
                key="positionalAllophones.lateralDarkness.preVocalic",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Post-vocalic (positionalAllophones.lateralDarkness.postVocalic):"),
                key="positionalAllophones.lateralDarkness.postVocalic",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Syllabic (positionalAllophones.lateralDarkness.syllabic):"),
                key="positionalAllophones.lateralDarkness.syllabic",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Dark F2 target (positionalAllophones.lateralDarkness.darkF2Target):"),
                key="positionalAllophones.lateralDarkness.darkF2Target",
            )

            sHelper.addItem(wx.StaticText(self, label=_("Positional allophones - Glottal reinforcement:")))
            self._addQuickTextField(
                sHelper,
                label=_('Contexts (format: ["#_#", "V_#"]) (positionalAllophones.glottalReinforcement.contexts):'),
                key="positionalAllophones.glottalReinforcement.contexts",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Duration ms (positionalAllophones.glottalReinforcement.durationMs):"),
                key="positionalAllophones.glottalReinforcement.durationMs",
            )

            # --- Boundary smoothing settings ---
            sHelper.addItem(wx.StaticText(self, label=_("Boundary smoothing:")))
            self._addQuickTextField(
                sHelper,
                label=_("Vowel to stop fade ms (boundarySmoothing.vowelToStopFadeMs):"),
                key="boundarySmoothing.vowelToStopFadeMs",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Stop to vowel fade ms (boundarySmoothing.stopToVowelFadeMs):"),
                key="boundarySmoothing.stopToVowelFadeMs",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Vowel to fricative fade ms (boundarySmoothing.vowelToFricFadeMs):"),
                key="boundarySmoothing.vowelToFricFadeMs",
            )

            # --- Trajectory limit settings ---
            sHelper.addItem(wx.StaticText(self, label=_("Trajectory limit:")))
            self._addQuickTextField(
                sHelper,
                label=_('Apply to formants (format: [cf2, cf3]) (trajectoryLimit.applyTo):'),
                key="trajectoryLimit.applyTo",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Max Hz per ms for CF2 (trajectoryLimit.maxHzPerMs.cf2):"),
                key="trajectoryLimit.maxHzPerMs.cf2",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Max Hz per ms for CF3 (trajectoryLimit.maxHzPerMs.cf3):"),
                key="trajectoryLimit.maxHzPerMs.cf3",
            )
            self._addQuickTextField(
                sHelper,
                label=_("Window ms (trajectoryLimit.windowMs):"),
                key="trajectoryLimit.windowMs",
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
                "trillModulationMs",
                "trillModulationFadeMs",
                "spellingDiphthongMode",
                "segmentBoundarySkipVowelToVowel",
                "segmentBoundarySkipVowelToLiquid",
                # --- Coarticulation settings ---
                "coarticulationEnabled",
                "coarticulationStrength",
                "coarticulationTransitionExtent",
                "coarticulationFadeIntoConsonants",
                "coarticulationWordInitialFadeScale",
                "coarticulationLabialF2Locus",
                "coarticulationAlveolarF2Locus",
                "coarticulationVelarF2Locus",
                "coarticulationVelarPinchEnabled",
                "coarticulationVelarPinchThreshold",
                "coarticulationVelarPinchF2Scale",
                "coarticulationVelarPinchF3",
                "coarticulationGraduated",
                "coarticulationAdjacencyMaxConsonants",
                # --- Phrase-final lengthening settings ---
                "phraseFinalLengtheningEnabled",
                "phraseFinalLengtheningFinalSyllableScale",
                "phraseFinalLengtheningPenultimateSyllableScale",
                "phraseFinalLengtheningStatementScale",
                "phraseFinalLengtheningQuestionScale",
                "phraseFinalLengtheningNucleusOnlyMode",
                # --- Microprosody settings ---
                "microprosodyEnabled",
                "microprosodyVoicelessF0RaiseEnabled",
                "microprosodyVoicelessF0RaiseHz",
                "microprosodyVoicelessF0RaiseEndHz",
                "microprosodyVoicedF0LowerEnabled",
                "microprosodyVoicedF0LowerHz",
                "microprosodyMinVowelMs",
                # --- Rate reduction settings ---
                "rateReductionEnabled",
                "rateReductionSchwaReductionThreshold",
                "rateReductionSchwaMinDurationMs",
                "rateReductionSchwaScale",
                # --- Nasalization settings ---
                "nasalizationAnticipatoryEnabled",
                "nasalizationAnticipatoryAmplitude",
                "nasalizationAnticipatoryBlend",
                # --- Liquid dynamics settings ---
                "liquidDynamics.enabled",
                "liquidDynamics.lateralOnglide.f1Delta",
                "liquidDynamics.lateralOnglide.f2Delta",
                "liquidDynamics.lateralOnglide.durationPct",
                # --- Length contrast settings ---
                "lengthContrast.enabled",
                "lengthContrast.shortVowelCeilingMs",
                "lengthContrast.longVowelFloorMs",
                "lengthContrast.geminateClosureScale",
                "lengthContrast.geminateReleaseScale",
                "lengthContrast.preGeminateVowelScale",
                # --- Positional allophones settings ---
                "positionalAllophones.enabled",
                "positionalAllophones.stopAspiration.wordInitialStressed",
                "positionalAllophones.stopAspiration.wordInitial",
                "positionalAllophones.stopAspiration.intervocalic",
                "positionalAllophones.stopAspiration.wordFinal",
                "positionalAllophones.lateralDarkness.preVocalic",
                "positionalAllophones.lateralDarkness.postVocalic",
                "positionalAllophones.lateralDarkness.syllabic",
                "positionalAllophones.lateralDarkness.darkF2Target",
                "positionalAllophones.glottalReinforcement.enabled",
                "positionalAllophones.glottalReinforcement.contexts",
                "positionalAllophones.glottalReinforcement.durationMs",
                # --- Boundary smoothing settings ---
                "boundarySmoothing.enabled",
                "boundarySmoothing.vowelToStopFadeMs",
                "boundarySmoothing.stopToVowelFadeMs",
                "boundarySmoothing.vowelToFricFadeMs",
                # --- Trajectory limit settings ---
                "trajectoryLimit.enabled",
                "trajectoryLimit.applyTo",
                "trajectoryLimit.maxHzPerMs.cf2",
                "trajectoryLimit.maxHzPerMs.cf3",
                "trajectoryLimit.windowMs",
                "trajectoryLimit.applyAcrossWordBoundary",
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
                    "coarticulationEnabled",
                    "coarticulationStrength",
                    "phraseFinalLengtheningEnabled",
                    "microprosodyEnabled",
                    "rateReductionEnabled",
                    "nasalizationAnticipatoryEnabled",
                    "liquidDynamics.enabled",
                    "lengthContrast.enabled",
                    "positionalAllophones.enabled",
                    "positionalAllophones.glottalReinforcement.enabled",
                    "boundarySmoothing.enabled",
                    "trajectoryLimit.enabled",
                ]

            self.settingKeyCtrl = self._addLabeledControlCompat(
                sHelper,
                _("Setting:"),
                wx.ComboBox,
                choices=self._knownKeys,
                style=wx.CB_DROPDOWN | wx.CB_READONLY,
            )
            self.settingKeyCtrl.Bind(wx.EVT_COMBOBOX, self._onSettingKeyChanged)

            self.valueCtrl = self._addLabeledControlCompat(sHelper, _("Value:"), wx.TextCtrl)
            self.valueCtrl.Bind(wx.EVT_TEXT, self._onGenericValueChanged)

            self.sourceLabel = sHelper.addItem(wx.StaticText(self, label=""))

            # GitHub link
            self.gitHubButton = sHelper.addItem(wx.Button(self, label=_("Open NV Speech Player on GitHub")))
            self.gitHubButton.Bind(wx.EVT_BUTTON, self._onGitHubClick)

            # Check for updates button
            self.updateButton = sHelper.addItem(wx.Button(self, label=_("Check for Add-on Updates")))
            self.updateButton.Bind(wx.EVT_BUTTON, self._onUpdateClick)

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

        def _onGitHubClick(self, evt):
            """Open the GitHub page in the default browser."""
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

        def _onUpdateClick(self, evt):
            """Check for updates and download the latest add-on if available."""
            try:
                import wx
            except Exception:
                evt.Skip()
                return

            import tempfile
            import urllib.request
            import urllib.error

            # First, check if an update is available
            wx.BeginBusyCursor()
            remoteVersion = None
            versionCheckError = None

            try:
                req = urllib.request.Request(
                    ADDON_VERSION_URL,
                    headers={"User-Agent": "NVDA-NVSpeechPlayer-Updater/1.0"},
                )
                with urllib.request.urlopen(req, timeout=15) as response:
                    remoteVersion = response.read().decode("utf-8").strip()
            except urllib.error.URLError as e:
                versionCheckError = str(e.reason) if hasattr(e, 'reason') else str(e)
            except Exception as e:
                versionCheckError = str(e)
            finally:
                wx.EndBusyCursor()

            if versionCheckError:
                # Couldn't check version - ask user if they want to download anyway
                dlg = wx.MessageDialog(
                    self,
                    _(
                        "Could not check for updates:\n{}\n\n"
                        "Would you like to download the add-on anyway?"
                    ).format(versionCheckError),
                    _("Version Check Failed"),
                    wx.YES_NO | wx.NO_DEFAULT | wx.ICON_WARNING,
                )
                result = dlg.ShowModal()
                dlg.Destroy()
                if result != wx.ID_YES:
                    evt.Skip()
                    return
            else:
                # Compare versions
                installedVersion = _getInstalledAddonVersion()

                # Check if remote version is actually newer
                if installedVersion and remoteVersion and not _isNewerVersion(remoteVersion, installedVersion):
                    wx.MessageBox(
                        _(
                            "You already have the latest version installed.\n\n"
                            "Installed version: {}\n"
                            "Latest version: {}"
                        ).format(installedVersion, remoteVersion),
                        _("No Update Available"),
                        wx.OK | wx.ICON_INFORMATION,
                        self,
                    )
                    evt.Skip()
                    return

                # Update available - confirm with user
                if installedVersion:
                    msg = _(
                        "A new version is available!\n\n"
                        "Installed version: {}\n"
                        "Latest version: {}\n\n"
                        "Download and install the update?"
                    ).format(installedVersion, remoteVersion)
                else:
                    msg = _(
                        "Latest version available: {}\n\n"
                        "Download and install?"
                    ).format(remoteVersion)

                dlg = wx.MessageDialog(
                    self,
                    msg,
                    _("Update Available"),
                    wx.YES_NO | wx.YES_DEFAULT | wx.ICON_QUESTION,
                )
                result = dlg.ShowModal()
                dlg.Destroy()

                if result != wx.ID_YES:
                    evt.Skip()
                    return

            # Show a simple busy cursor since download may take a moment
            wx.BeginBusyCursor()

            downloadPath = None
            errorMsg = None

            try:
                # Download to temp directory
                tempDir = tempfile.gettempdir()
                # Extract filename from URL
                addonFilename = ADDON_UPDATE_URL.rsplit("/", 1)[-1]
                if not addonFilename.endswith(".nvda-addon"):
                    addonFilename = "nvSpeechPlayer-update.nvda-addon"
                downloadPath = os.path.join(tempDir, addonFilename)

                # Clean up any old downloaded addon file from previous attempts
                try:
                    if os.path.isfile(downloadPath):
                        os.remove(downloadPath)
                except Exception:
                    pass  # Not critical if cleanup fails

                # Download the file
                req = urllib.request.Request(
                    ADDON_UPDATE_URL,
                    headers={"User-Agent": "NVDA-NVSpeechPlayer-Updater/1.0"},
                )
                with urllib.request.urlopen(req, timeout=60) as response:
                    data = response.read()

                # Write to temp file
                with open(downloadPath, "wb") as f:
                    f.write(data)

            except urllib.error.URLError as e:
                errorMsg = _("Failed to download add-on:\n{}").format(str(e.reason) if hasattr(e, 'reason') else str(e))
            except Exception as e:
                errorMsg = _("An error occurred during download:\n{}").format(str(e))
            finally:
                wx.EndBusyCursor()

            if errorMsg:
                wx.MessageBox(
                    errorMsg,
                    _("Download Error"),
                    wx.OK | wx.ICON_ERROR,
                    self,
                )
                evt.Skip()
                return

            # Launch the downloaded file with the default handler (NVDA's addon installer)
            # Note: os.startfile returns immediately; we can't know if user clicks "No" in NVDA's
            # installer dialog. The temp file will be cleaned up on next download attempt.
            try:
                # os.startfile is Windows-only but NVDA is Windows-only anyway
                os.startfile(downloadPath)
            except Exception as e:
                # If startfile fails, show the path so user can install manually
                wx.MessageBox(
                    _(
                        "Download complete, but failed to launch installer:\n{}\n\n"
                        "File saved to:\n{}\n\n"
                        "Please install it manually by opening the file."
                    ).format(str(e), downloadPath),
                    _("Warning"),
                    wx.OK | wx.ICON_WARNING,
                    self,
                )

            evt.Skip()

        def _onResetDefaultsClick(self, evt):
            """Reset language packs to factory defaults by copying from .defaults folder."""
            try:
                import wx
            except Exception:
                evt.Skip()
                return

            # Get the currently selected language tag
            currentLangTag = langPackYaml.normalizeLangTag(self.langTagCtrl.GetValue())

            # Create a custom dialog with a checkbox
            dlg = wx.Dialog(self, title=_("Reset to Factory Defaults"), style=wx.DEFAULT_DIALOG_STYLE)

            sizer = wx.BoxSizer(wx.VERTICAL)

            # Warning message
            msgText = wx.StaticText(
                dlg,
                label=_(
                    "This will overwrite language pack YAML files in packs/lang/ "
                    "with the factory default versions from packs/.defaults/.\n\n"
                    "Any customizations you have made will be lost."
                ),
            )
            sizer.Add(msgText, 0, wx.ALL | wx.EXPAND, 10)

            # Checkbox for single language pack reset
            resetSingleCheckbox = wx.CheckBox(
                dlg,
                label=_("Reset only the currently selected language pack ({})").format(currentLangTag),
            )
            resetSingleCheckbox.SetValue(True)  # Default to single pack (safer)
            sizer.Add(resetSingleCheckbox, 0, wx.LEFT | wx.RIGHT | wx.BOTTOM, 10)

            # Create buttons manually for better control
            btnSizer = wx.BoxSizer(wx.HORIZONTAL)
            yesBtn = wx.Button(dlg, wx.ID_YES, _("Yes"))
            noBtn = wx.Button(dlg, wx.ID_NO, _("No"))
            btnSizer.Add(yesBtn, 0, wx.RIGHT, 5)
            btnSizer.Add(noBtn, 0)
            sizer.Add(btnSizer, 0, wx.ALL | wx.ALIGN_CENTER, 10)

            # Set No as default button and handle escape
            noBtn.SetDefault()
            dlg.SetEscapeId(wx.ID_NO)
            dlg.SetAffirmativeId(wx.ID_YES)

            dlg.SetSizer(sizer)
            sizer.Fit(dlg)
            dlg.CenterOnParent()

            result = dlg.ShowModal()

            # Only proceed if user explicitly clicked Yes
            if result != wx.ID_YES:
                dlg.Destroy()
                evt.Skip()
                return

            # Get checkbox value before destroying dialog
            resetSingleOnly = resetSingleCheckbox.GetValue()
            dlg.Destroy()

            # Perform the reset
            defaultsDir = os.path.join(self._packsDir, ".defaults")
            langDir = langPackYaml.getLangDir(self._packsDir)

            if not os.path.isdir(defaultsDir):
                wx.MessageBox(
                    _(
                        "Factory defaults folder not found:\n{}\n\n"
                        "Cannot reset language packs."
                    ).format(defaultsDir),
                    _("Error"),
                    wx.OK | wx.ICON_ERROR,
                    self,
                )
                evt.Skip()
                return

            # Ensure lang directory exists
            try:
                os.makedirs(langDir, exist_ok=True)
            except Exception as e:
                wx.MessageBox(
                    _("Failed to create language pack directory:\n{}").format(str(e)),
                    _("Error"),
                    wx.OK | wx.ICON_ERROR,
                    self,
                )
                evt.Skip()
                return

            # Copy *.yaml files from .defaults to lang
            copiedCount = 0
            errors = []
            try:
                for fn in os.listdir(defaultsDir):
                    if not fn.lower().endswith(".yaml"):
                        continue

                    # If resetting single language only, check if this file matches
                    if resetSingleOnly:
                        fileTag = os.path.splitext(fn)[0].lower()
                        if fileTag != currentLangTag:
                            continue

                    srcPath = os.path.join(defaultsDir, fn)
                    dstPath = os.path.join(langDir, fn)
                    if not os.path.isfile(srcPath):
                        continue
                    try:
                        # Read and write to avoid shutil dependency issues
                        with open(srcPath, "r", encoding="utf-8") as f:
                            content = f.read()
                        with open(dstPath, "w", encoding="utf-8", newline="\n") as f:
                            f.write(content)
                        copiedCount += 1
                    except Exception as e:
                        errors.append(f"{fn}: {e}")
            except Exception as e:
                wx.MessageBox(
                    _("Failed to read defaults directory:\n{}").format(str(e)),
                    _("Error"),
                    wx.OK | wx.ICON_ERROR,
                    self,
                )
                evt.Skip()
                return

            if copiedCount == 0 and resetSingleOnly:
                wx.MessageBox(
                    _(
                        "No factory default found for language pack '{}'.\n\n"
                        "The file '{}.yaml' does not exist in the .defaults folder."
                    ).format(currentLangTag, currentLangTag),
                    _("Not Found"),
                    wx.OK | wx.ICON_WARNING,
                    self,
                )
                evt.Skip()
                return

            if errors:
                wx.MessageBox(
                    _(
                        "Reset completed with errors.\n\n"
                        "Copied {} file(s).\n\n"
                        "Errors:\n{}"
                    ).format(copiedCount, "\n".join(errors)),
                    _("Warning"),
                    wx.OK | wx.ICON_WARNING,
                    self,
                )
            else:
                if resetSingleOnly:
                    wx.MessageBox(
                        _("Successfully reset '{}' language pack to factory defaults.").format(currentLangTag),
                        _("Reset Complete"),
                        wx.OK | wx.ICON_INFORMATION,
                        self,
                    )
                else:
                    wx.MessageBox(
                        _("Successfully reset {} language pack file(s) to factory defaults.").format(copiedCount),
                        _("Reset Complete"),
                        wx.OK | wx.ICON_INFORMATION,
                        self,
                    )

            # Clear pending edits (they're now stale)
            if resetSingleOnly:
                # Only clear pending edits for the reset language
                if currentLangTag in self._pending:
                    del self._pending[currentLangTag]
            else:
                self._pending.clear()

            # Refresh the language choices in case new files appeared
            try:
                currentTag = self.langTagCtrl.GetValue()
                self.langTagCtrl.Clear()
                for choice in self._getLanguageChoices():
                    self.langTagCtrl.Append(choice)
                self.langTagCtrl.SetValue(currentTag)
            except Exception:
                pass

            # Refresh all displays to show the reset values
            self._refreshAllDisplays()

            # Trigger live reload if NV Speech Player is active
            try:
                import synthDriverHandler

                synth = synthDriverHandler.getSynth()
                if synth and synth.__class__.__module__.endswith("nvSpeechPlayer"):
                    if hasattr(synth, "reloadLanguagePack"):
                        synth.reloadLanguagePack()
            except Exception:
                pass

            evt.Skip()

        def _addQuickTextField(self, sHelper, *, label: str, key: str):
            """Add a labeled wx.TextCtrl bound to a settings key."""
            try:
                import wx
            except Exception:
                return

            ctrl = self._addLabeledControlCompat(sHelper, label, wx.TextCtrl)
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
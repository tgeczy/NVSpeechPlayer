# -*- coding: utf-8 -*-
"""TGSpeechBox — One-time config migration from nvSpeechPlayer.

When the synth driver was renamed from nvSpeechPlayer to tgSpeechBox,
the NVDA config section changed from [[nvSpeechPlayer]] to [[tgSpeechBox]].
This module copies all saved settings (voice, pitch, rate, 50+ custom keys)
so users don't lose their configuration on upgrade.

The migration runs once: if [[tgSpeechBox]] already has saved data, it's a no-op.
After copying, a deferred message box tells the user what happened.

IMPORTANT — NVDA config caveat:
    config.conf["speech"] is an AggregatedSection whose ``__contains__``
    resolves against the config *spec* (``__many__``), not just saved data.
    This means ``"tgSpeechBox" in config.conf["speech"]`` is ALWAYS True
    for any valid synth name, even if no settings have ever been saved.
    We therefore use ``.isSet()`` which checks actual profile data only.
"""

from __future__ import annotations

from logHandler import log

OLD_SECTION = "nvSpeechPlayer"
NEW_SECTION = "tgSpeechBox"


def run() -> bool:
    """Migrate config from [[nvSpeechPlayer]] to [[tgSpeechBox]] if needed.

    Returns True if migration occurred, False otherwise.
    Safe to call at any point during synth init — two isSet() lookups on the
    fast path (already-migrated or fresh install).
    """
    try:
        import config

        speech = config.conf["speech"]

        # Fast path: new section has real saved settings — nothing to do.
        if speech.isSet(NEW_SECTION):
            return False

        # No old section either — fresh install, nothing to migrate.
        if not speech.isSet(OLD_SECTION):
            return False

        # ── Migrate ──────────────────────────────────────────────
        # Read old settings from the base profile (profiles[0]).
        # We go through the base profile directly rather than the
        # AggregatedSection so we get raw saved values without
        # spec defaults mixed in.
        base = config.conf.profiles[0]
        if "speech" not in base or OLD_SECTION not in base["speech"]:
            log.debug("TGSpeechBox: old section not in base profile, skipping migration")
            return False

        oldData = base["speech"][OLD_SECTION]

        # Create the new section in the base profile and copy all keys.
        base["speech"][NEW_SECTION] = {}
        newData = base["speech"][NEW_SECTION]
        count = 0
        for key in oldData:
            val = oldData[key]
            newData[key] = val
            count += 1

        log.info(
            f"TGSpeechBox: migrated {count} settings "
            f"from [[{OLD_SECTION}]] to [[{NEW_SECTION}]]"
        )

        # Invalidate the AggregatedSection cache so subsequent reads
        # (including initSettings / loadSettings) see the new data.
        try:
            speech._cache.clear()
        except Exception:
            pass

        # Persist immediately so a crash won't lose the migration.
        try:
            config.conf.save()
        except Exception:
            log.debug("TGSpeechBox: config.conf.save() after migration failed",
                      exc_info=True)

        # Show a one-time notice (deferred — GUI may not be ready yet).
        _show_notice_deferred()
        return True

    except Exception:
        # Never prevent the synth from loading.
        log.debug("TGSpeechBox: config migration skipped", exc_info=True)
        return False


VOLUME_MARKER = "volumeScale100"


def rescale_volume() -> bool:
    """One-time rescale of a saved volume percent (#114).

    The driver historically mapped NVDA's volume percent to engine gain as
    percent/75 (internal gain 1.0 reported as ~75%), so fresh installs
    showed less than 100% while Android/Linux/SAPI default to 100. The
    mapping is now percent/100; an existing saved percent is rescaled by
    100/75 (capped at 100) so the audible gain is preserved across the
    update. A marker key in the base profile makes this run at most once —
    it is also written on fresh installs so a later upgrade can never
    mistake new-scale data for old.
    """
    try:
        import config

        base = config.conf.profiles[0]
        sect = base.setdefault("speech", {}).setdefault(NEW_SECTION, {})
        if sect.get(VOLUME_MARKER):
            return False
        changed = False
        if "volume" in sect:
            try:
                old = float(sect["volume"])
                sect["volume"] = min(100, int(round(old * 100.0 / 75.0)))
                changed = True
            except Exception:
                pass
        sect[VOLUME_MARKER] = 1
        try:
            config.conf["speech"]._cache.clear()
        except Exception:
            pass
        try:
            config.conf.save()
        except Exception:
            log.debug("TGSpeechBox: config.conf.save() after volume rescale failed",
                      exc_info=True)
        if changed:
            log.info("TGSpeechBox: rescaled saved volume to the 100%-scale (#114)")
        return changed
    except Exception:
        # Never prevent the synth from loading.
        log.debug("TGSpeechBox: volume rescale skipped", exc_info=True)
        return False


def _show_notice_deferred() -> None:
    """Schedule a message box on the wx main loop.

    Uses wx.CallAfter so this is safe to call during synth __init__
    before the NVDA GUI loop is fully running.
    """
    try:
        import wx

        def _show():
            try:
                import gui
                gui.messageBox(
                    "TGSpeechBox has migrated your settings from the old "
                    "NV Speech Player configuration.\n\n"
                    "All your voice, pitch, rate, and language settings "
                    "have been preserved.\n\n"
                    "If you still have the old NV Speech Player add-on "
                    "installed, you can safely remove it from:\n"
                    "NVDA menu > Tools > Manage add-ons\n\n"
                    "The original NV Access version (UK English only) "
                    "is still available at:\n"
                    "https://github.com/nvaccess/nvSpeechPlayer",
                    "TGSpeechBox — Settings Migrated",
                    wx.OK | wx.ICON_INFORMATION,
                )
            except Exception:
                log.debug("TGSpeechBox: migration notice dialog failed",
                          exc_info=True)

        wx.CallAfter(_show)
    except Exception:
        # wx not available — skip the dialog, migration still happened.
        log.debug("TGSpeechBox: could not defer migration notice",
                  exc_info=True)
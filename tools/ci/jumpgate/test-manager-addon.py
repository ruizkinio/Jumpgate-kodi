#!/usr/bin/env python3

import runpy
import sys
import types
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
ADDON_ROOT = ROOT / "addons" / "script.jumpgate.manager"


def verify_entrypoint():
    calls = []

    class Addon:
        def openSettings(self):
            calls.append("openSettings")

    module = types.ModuleType("xbmcaddon")
    module.Addon = Addon
    previous = sys.modules.get("xbmcaddon")
    sys.modules["xbmcaddon"] = module
    try:
        runpy.run_path(str(ADDON_ROOT / "addon.py"), run_name="__main__")
    finally:
        if previous is None:
            sys.modules.pop("xbmcaddon", None)
        else:
            sys.modules["xbmcaddon"] = previous

    if calls != ["openSettings"]:
        raise AssertionError(f"manager entrypoint calls were {calls!r}")


def verify_manifests():
    addon = ET.parse(ADDON_ROOT / "addon.xml").getroot()
    script = addon.find("./extension[@point='xbmc.python.script']")
    if script is None or script.get("library") != "addon.py":
        raise AssertionError("manager addon entrypoint is not addon.py")

    settings = ET.parse(ADDON_ROOT / "resources" / "settings.xml").getroot()
    action = settings.find(".//setting[@id='open_manager_menu']")
    if action is None or action.get("type") != "action":
        raise AssertionError("native manager action is missing")
    data = action.findtext("data")
    close = action.findtext("./control/close")
    if data != "JumpgateManager" or close != "true":
        raise AssertionError("native manager action contract is malformed")


if __name__ == "__main__":
    verify_entrypoint()
    verify_manifests()
    print("Jumpgate Manager addon contract: passed")

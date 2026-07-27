#!/usr/bin/env python3

import hashlib
import runpy
import sys
import types
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
ADDON_ROOT = ROOT / "addons" / "script.jumpgate.manager"
QR_VENDOR_HASHES = {
    "xbmc/utils/qrcodegen.cpp": (
        "1f3b3fcdac6954c32cf583ccd02ec9b5901f756a38c461acedc70be4a77d3757"
    ),
    "xbmc/utils/qrcodegen.hpp": (
        "b779c3b156cf7a57ce789d6fee4fc991ccc2913774d26c909d22bb8f26b2a793"
    ),
}


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


def verify_native_pairing_assets():
    dialog_path = (
        ADDON_ROOT
        / "resources"
        / "skins"
        / "default"
        / "1080i"
        / "DialogJumpgatePairing.xml"
    )
    dialog = ET.parse(dialog_path).getroot()
    if dialog.tag != "window" or dialog.get("type") != "dialog":
        raise AssertionError("native pairing window contract is malformed")
    if dialog.findtext("./defaultcontrol") != "20":
        raise AssertionError("native pairing dialog must focus Cancel while issuing")

    control_ids = [
        int(control.get("id"))
        for control in dialog.findall(".//control[@id]")
    ]
    required_ids = {10, 11, 12, 13, 14, 15, 16, 17, 20, 21}
    if not required_ids.issubset(control_ids) or len(control_ids) != len(set(control_ids)):
        raise AssertionError("native pairing dialog controls are missing or duplicated")

    pixel = ADDON_ROOT / "resources" / "media" / "pixel.png"
    pixel_bytes = pixel.read_bytes()
    if len(pixel_bytes) < 24 or pixel_bytes[:8] != b"\x89PNG\r\n\x1a\n":
        raise AssertionError("native pairing pixel asset is not a valid PNG")


def verify_qr_vendor_sources():
    for relative_path, expected_digest in QR_VENDOR_HASHES.items():
        digest = hashlib.sha256((ROOT / relative_path).read_bytes()).hexdigest()
        if digest != expected_digest:
            raise AssertionError(f"audited QR vendor source changed: {relative_path}")


if __name__ == "__main__":
    verify_entrypoint()
    verify_manifests()
    verify_native_pairing_assets()
    verify_qr_vendor_sources()
    print("Jumpgate Manager addon contract: passed")

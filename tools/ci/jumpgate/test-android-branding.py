#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import re
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
VERSION_FILE = ROOT / "version.txt"
APP_HEADER = ROOT / "xbmc" / "platform" / "android" / "activity" / "XBMCApp.h"
APP_SOURCE = ROOT / "xbmc" / "platform" / "android" / "activity" / "XBMCApp.cpp"
PACKAGING_MAKEFILE = ROOT / "tools" / "android" / "packaging" / "Makefile.in"
ANDROID_MANIFEST = ROOT / "tools" / "android" / "packaging" / "xbmc" / "AndroidManifest.xml.in"
ANDROID_WORKFLOW = ROOT / ".github" / "workflows" / "jumpgate-android.yml"
MANAGER_MANIFEST = ROOT / "addons" / "script.jumpgate.manager" / "addon.xml"
RELEASE_POLICY_FILES = (
    (ROOT / ".github" / "workflows" / "jumpgate-android-release.yml", False),
    (ROOT / "tools" / "ci" / "jumpgate" / "test-jumpgate-android-release-workflow.py", True),
    (ROOT / "tools" / "ci" / "jumpgate" / "test-android-branding.py", True),
    (ROOT / "tools" / "ci" / "jumpgate" / "verify-android-release.sh", True),
    (ROOT / "tools" / "ci" / "jumpgate" / "test-verify-android-release.sh", True),
)


def derive_android_version_code(source):
    match = re.fullmatch(r"([0-9]+)\.([0-9]{1,2})\.([0-9]{1,3})", source)
    if match is None or any(len(part) > limit for part, limit in zip(match.groups(), (5, 2, 3))):
        raise AssertionError("VERSION_CODE must be a bounded major.minor.patch value")
    major, minor, patch = (int(part) for part in match.groups())
    value = (major * 100 + minor) * 1000 + patch
    if not 1 <= value <= 2_100_000_000:
        raise AssertionError("VERSION_CODE must derive to Android range 1..2100000000")
    return value


def verify_release_policy_spdx():
    for path, has_shebang in RELEASE_POLICY_FILES:
        lines = path.read_text(encoding="utf-8").splitlines()
        spdx_index = 1 if has_shebang else 0
        if has_shebang and (not lines or not lines[0].startswith("#!")):
            raise AssertionError(f"release policy script lost its shebang: {path.relative_to(ROOT)}")
        if len(lines) <= spdx_index or lines[spdx_index] != "# SPDX-License-Identifier: GPL-2.0-or-later":
            raise AssertionError(f"release policy file lacks the fork license: {path.relative_to(ROOT)}")


def verify_version_code_boundaries():
    if derive_android_version_code("0.0.1") != 1:
        raise AssertionError("minimum Android versionCode derivation drifted")
    if derive_android_version_code("21000.0.0") != 2_100_000_000:
        raise AssertionError("maximum Android versionCode derivation drifted")
    for rejected in ("0.0.0", "21000.0.1", "21001.0.0", f"{'9' * 1000}.0.0"):
        try:
            derive_android_version_code(rejected)
        except AssertionError:
            continue
        raise AssertionError(f"out-of-range VERSION_CODE was accepted: {rejected[:32]!r}")


def read_version_fields():
    fields = {}
    for line in VERSION_FILE.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition(" ")
        if not separator or not value:
            raise AssertionError(f"malformed version.txt line: {line!r}")
        fields[key] = value
    return fields


def extract_jumpgate_version():
    source = APP_HEADER.read_text(encoding="utf-8")
    match = re.search(r'JUMPGATE_VERSION\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"', source)
    if match is None:
        raise AssertionError("XBMCApp.h does not define a semantic JUMPGATE_VERSION")
    return match.group(1)


def verify_identity():
    fields = read_version_fields()
    jumpgate_version = extract_jumpgate_version()

    expected = {
        "APP_NAME": "Jumpgate",
        "APP_PACKAGE": "io.github.ruizkinio.jumpgate",
        "VERSION_TAG": f"ALPHA2-Jumpgate-{jumpgate_version}",
        "VERSION_CODE": "22.0.300",
        "PACKAGE_IDENTITY": "ruizkinio.Jumpgate",
    }
    for key, value in expected.items():
        if fields.get(key) != value:
            raise AssertionError(f"{key} must be {value!r}, got {fields.get(key)!r}")

    package = fields["APP_PACKAGE"]
    if package == "org.xbmc.kodi" or not re.fullmatch(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+", package):
        raise AssertionError("Jumpgate must use a valid application ID distinct from official Kodi")

    version_code = derive_android_version_code(fields["VERSION_CODE"])
    if version_code <= 2_190_702:
        raise AssertionError("Jumpgate Android versionCode must advance beyond development builds")

    manager_version = ET.parse(MANAGER_MANIFEST).getroot().get("version")
    if manager_version != jumpgate_version:
        raise AssertionError(
            f"Jumpgate Manager version {manager_version!r} does not match {jumpgate_version!r}"
        )


def verify_package_derivation():
    app_source = APP_SOURCE.read_text(encoding="utf-8")
    if 'std::string(CCompileInfo::GetPackage()) + ".fileprovider"' not in app_source:
        raise AssertionError("Android FileProvider authority is not derived from APP_PACKAGE")
    if "org.xbmc.kodi.fileprovider" in app_source:
        raise AssertionError("Android runtime retains Kodi's hardcoded FileProvider authority")

    makefile = PACKAGING_MAKEFILE.read_text(encoding="utf-8")
    expected_target = "$(PREFIX)/lib/@APP_NAME_LC@/lib@APP_NAME_LC@.so:"
    if expected_target not in makefile:
        raise AssertionError("Android packaging target is not derived from APP_NAME")
    if "$(PREFIX)/lib/xbmc/lib@APP_NAME_LC@.so:" in makefile:
        raise AssertionError("Android packaging retains a hardcoded Kodi library target")

    manifest = ANDROID_MANIFEST.read_text(encoding="utf-8")
    if 'android:authorities="@APP_PACKAGE@.fileprovider"' not in manifest:
        raise AssertionError("Android manifest FileProvider authority is not derived from APP_PACKAGE")

    workflow = ANDROID_WORKFLOW.read_text(encoding="utf-8")
    required_workflow_contracts = (
        "CACHE_SCHEMA_VERSION: v3",
        "identity_hash=\"$(sha256sum version.txt | cut -d ' ' -f 1)\"",
        "steps.source-keys.outputs.identity_hash",
        "app_name_lc=\"$(read_version_field APP_NAME",
        'expected_core_library="lib${app_name_lc}.so"',
        'source_apk="${app_name_lc}app-${{ matrix.abi }}-release.apk"',
        "python3 tools/ci/jumpgate/test-jumpgate-android-release-workflow.py",
        "bash tools/ci/jumpgate/test-verify-android-release.sh",
        '"$EXPECTED_APK_SIGNER_SHA256" \\\n            "$expected_core_library"',
    )
    for contract in required_workflow_contracts:
        if contract not in workflow:
            raise AssertionError(f"Android workflow omits branding contract: {contract}")
    if 'source_apk="kodiapp-' in workflow:
        raise AssertionError("Android workflow retains Kodi's APK staging name")
    if "libkodi.so" in workflow:
        raise AssertionError("Android workflow retains Kodi's core-library identity")


if __name__ == "__main__":
    verify_release_policy_spdx()
    verify_version_code_boundaries()
    verify_identity()
    verify_package_derivation()
    print("Jumpgate Android branding contract: passed")

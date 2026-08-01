#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import hashlib
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from fnmatch import fnmatchcase
from pathlib import Path
from tempfile import TemporaryDirectory


ROOT = Path(__file__).resolve().parents[3]
ROOT_GITATTRIBUTES = ROOT / ".gitattributes"
VERSION_FILE = ROOT / "version.txt"
ROOT_CMAKE = ROOT / "CMakeLists.txt"
COMPILE_INFO_GENERATOR = (
    ROOT / "cmake" / "scripts" / "common" / "GenerateCompileInfo.cmake"
)
COMPILE_INFO_GENERATOR_ARGUMENT = (
    "${CMAKE_SOURCE_DIR}/cmake/scripts/common/GenerateCompileInfo.cmake"
)
COMPILE_INFO_PACKAGE_ARGUMENT = "-DAPP_PACKAGE=${APP_PACKAGE}"
APP_HEADER = ROOT / "xbmc" / "platform" / "android" / "activity" / "XBMCApp.h"
APP_SOURCE = ROOT / "xbmc" / "platform" / "android" / "activity" / "XBMCApp.cpp"
PACKAGING_MAKEFILE = ROOT / "tools" / "android" / "packaging" / "Makefile.in"
LIBANDROIDJNI_DEPENDENCY_DIR = ROOT / "tools" / "depends" / "target" / "libandroidjni"
LIBANDROIDJNI_VERSION_FILE = LIBANDROIDJNI_DEPENDENCY_DIR / "LIBANDROIDJNI-VERSION"
LIBANDROIDJNI_MAKEFILE = LIBANDROIDJNI_DEPENDENCY_DIR / "Makefile"
LIBANDROIDJNI_PATCH = (
    LIBANDROIDJNI_DEPENDENCY_DIR / "001-fix-intent-getstringextra-signature.patch"
)
LIBANDROIDJNI_FIND_MODULE = ROOT / "cmake" / "modules" / "FindLibAndroidJNI.cmake"
LIBANDROIDJNI_FIXTURE_COMMIT = "1b1def8492b5ceb71b188bebf3d0061fd502dfe0"
LIBANDROIDJNI_FIXTURE_SHA256 = (
    "d02d1fe43b1e07239926b2bb7f3d706a3d0b300c710a2c1b8eb9dd3e6e856db1"
)
LIBANDROIDJNI_INTENT_FIXTURE = (
    ROOT
    / "tools"
    / "ci"
    / "jumpgate"
    / "fixtures"
    / "libandroidjni"
    / LIBANDROIDJNI_FIXTURE_COMMIT
    / "src"
    / "Intent.cpp"
)
ANDROID_MANIFEST = (
    ROOT / "tools" / "android" / "packaging" / "xbmc" / "AndroidManifest.xml.in"
)
MAIN_ACTIVITY = (
    ROOT / "tools" / "android" / "packaging" / "xbmc" / "src" / "Main.java.in"
)
EXTERNAL_RESULT_SOURCE = (
    ROOT
    / "tools"
    / "android"
    / "packaging"
    / "xbmc"
    / "src"
    / "ExternalPlayerResultCoordinator.java.in"
)
ANDROID_APP_GRADLE = (
    ROOT / "tools" / "android" / "packaging" / "xbmc" / "build.gradle.in"
)
EXTERNAL_RESULT_TEST = (
    ROOT
    / "tools"
    / "android"
    / "packaging"
    / "xbmc"
    / "test"
    / "ExternalPlayerResultCoordinatorTest.java.in"
)
BACK_BOOL_BASE = (
    ROOT / "tools" / "android" / "packaging" / "xbmc" / "res" / "values" / "bools.xml"
)
BACK_BOOL_V36 = (
    ROOT
    / "tools"
    / "android"
    / "packaging"
    / "xbmc"
    / "res"
    / "values-v36"
    / "bools.xml"
)
EVENT_LOOP = ROOT / "xbmc" / "platform" / "android" / "activity" / "EventLoop.cpp"
JNI_MAIN_HEADER = (
    ROOT / "xbmc" / "platform" / "android" / "activity" / "JNIMainActivity.h"
)
JNI_MAIN_SOURCE = (
    ROOT / "xbmc" / "platform" / "android" / "activity" / "JNIMainActivity.cpp"
)
CREDENTIAL_STORE_HEADER = (
    ROOT
    / "xbmc"
    / "platform"
    / "android"
    / "activity"
    / "AndroidJumpgateCredentialStore.h"
)
CREDENTIAL_STORE_SOURCE = (
    ROOT
    / "xbmc"
    / "platform"
    / "android"
    / "activity"
    / "AndroidJumpgateCredentialStore.cpp"
)
WIN_SYSTEM = ROOT / "xbmc" / "windowing" / "android" / "WinSystemAndroid.cpp"
BACK_COORDINATOR_HEADER = ROOT / "xbmc" / "utils" / "JumpgateBackCoordinator.h"
BACK_COORDINATOR_SOURCE = ROOT / "xbmc" / "utils" / "JumpgateBackCoordinator.cpp"
BACK_COORDINATOR_TEST = (
    ROOT / "xbmc" / "utils" / "test" / "TestJumpgateBackCoordinator.cpp"
)
INPUT_MANAGER_HEADER = ROOT / "xbmc" / "input" / "InputManager.h"
INPUT_MANAGER_SOURCE = ROOT / "xbmc" / "input" / "InputManager.cpp"
UTILS_CMAKE = ROOT / "xbmc" / "utils" / "CMakeLists.txt"
UTILS_TEST_CMAKE = ROOT / "xbmc" / "utils" / "test" / "CMakeLists.txt"
ANDROID_WORKFLOW = ROOT / ".github" / "workflows" / "jumpgate-android.yml"
XBMC_SOURCE_ROOT = ROOT / "xbmc"
HOST_TEST_SUITE_FAMILIES = (
    "TestJumpgate",
    "TestAndroidJumpgate",
    "TestUPnPPlayerOpenPublication",
)
HOST_TEST_FILTER_PATTERNS = tuple(
    f"{family}*" for family in HOST_TEST_SUITE_FAMILIES
)
GTEST_DECLARATION = re.compile(
    r"\b(?:TEST|TEST_F|TEST_P|TYPED_TEST|TYPED_TEST_P)\s*"
    r"\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)",
    re.MULTILINE,
)
CMAKE_VARIABLE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")
MANAGER_MANIFEST = ROOT / "addons" / "script.jumpgate.manager" / "addon.xml"
ESTUARY_HOME = ROOT / "addons" / "skin.estuary" / "xml" / "Home.xml"
ESTUARY_TIMERS = ROOT / "addons" / "skin.estuary" / "xml" / "Timers.xml"
ESTUARY_VARIABLES = ROOT / "addons" / "skin.estuary" / "xml" / "Variables.xml"
RELEASE_POLICY_FILES = (
    (ROOT / ".github" / "workflows" / "jumpgate-android-release.yml", False),
    (
        ROOT
        / "tools"
        / "ci"
        / "jumpgate"
        / "test-jumpgate-android-release-workflow.py",
        True,
    ),
    (ROOT / "tools" / "ci" / "jumpgate" / "test-android-branding.py", True),
    (ROOT / "tools" / "ci" / "jumpgate" / "verify-android-release.sh", True),
    (ROOT / "tools" / "ci" / "jumpgate" / "test-verify-android-release.sh", True),
)


def verify_estuary_osd_defaults():
    marker_condition = "!Skin.HasSetting(JumpgateOSDDefaultsInitialized)"
    expected_onloads = (
        "Skin.SetBool(OSDAutoClose,true)",
        "Skin.SetString(OSDAutoCloseTime,5)",
        "Skin.SetBool(JumpgateOSDDefaultsInitialized,true)",
    )
    home = ET.parse(ESTUARY_HOME).getroot()
    guarded_onloads = [
        (node.text or "").strip()
        for node in home.findall("onload")
        if node.get("condition") == marker_condition
    ]
    if guarded_onloads != list(expected_onloads):
        raise AssertionError(
            "Estuary must initialize the five-second OSD default once, then fence user overrides"
        )

    timers = ET.parse(ESTUARY_TIMERS).getroot()
    osd_timer = next(
        (
            timer
            for timer in timers.findall("timer")
            if (timer.findtext("name") or "").strip() == "autoclosevideoosd"
        ),
        None,
    )
    if osd_timer is None:
        raise AssertionError("Estuary video OSD auto-close timer is missing")
    stop_condition = (osd_timer.findtext("stop") or "").strip()
    if (
        "String.IsEmpty(Skin.String(OSDAutoCloseTime))"
        " + Integer.IsGreaterOrEqual(Skin.TimerElapsedSecs(autoclosevideoosd), 5)"
        not in stop_condition
    ):
        raise AssertionError("Estuary OSD timer fallback is not five seconds")

    variables = ET.parse(ESTUARY_VARIABLES).getroot()
    osd_variable = next(
        (
            variable
            for variable in variables.findall("variable")
            if variable.get("name") == "SkinSettingOSDAutoCloseTime"
        ),
        None,
    )
    if osd_variable is None:
        raise AssertionError("Estuary OSD timeout display variable is missing")
    empty_default = next(
        (
            (value.text or "").strip()
            for value in osd_variable.findall("value")
            if value.get("condition")
            == "String.IsEmpty(Skin.String(OSDAutoCloseTime))"
        ),
        None,
    )
    if empty_default != "5":
        raise AssertionError("Estuary OSD timeout display default is not five seconds")


def derive_android_version_code(source):
    match = re.fullmatch(r"([0-9]+)\.([0-9]{1,2})\.([0-9]{1,3})", source)
    if match is None or any(
        len(part) > limit for part, limit in zip(match.groups(), (5, 2, 3))
    ):
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
            raise AssertionError(
                f"release policy script lost its shebang: {path.relative_to(ROOT)}"
            )
        if (
            len(lines) <= spdx_index
            or lines[spdx_index] != "# SPDX-License-Identifier: GPL-2.0-or-later"
        ):
            raise AssertionError(
                f"release policy file lacks the fork license: {path.relative_to(ROOT)}"
            )


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
        raise AssertionError(
            f"out-of-range VERSION_CODE was accepted: {rejected[:32]!r}"
        )


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
    if package == "org.xbmc.kodi" or not re.fullmatch(
        r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+", package
    ):
        raise AssertionError(
            "Jumpgate must use a valid application ID distinct from official Kodi"
        )

    version_code = derive_android_version_code(fields["VERSION_CODE"])
    if version_code <= 2_190_702:
        raise AssertionError(
            "Jumpgate Android versionCode must advance beyond development builds"
        )

    manager_version = ET.parse(MANAGER_MANIFEST).getroot().get("version")
    if manager_version != jumpgate_version:
        raise AssertionError(
            f"Jumpgate Manager version {manager_version!r} does not match {jumpgate_version!r}"
        )


def verify_package_derivation():
    app_source = APP_SOURCE.read_text(encoding="utf-8")
    if 'std::string(CCompileInfo::GetPackage()) + ".fileprovider"' not in app_source:
        raise AssertionError(
            "Android FileProvider authority is not derived from APP_PACKAGE"
        )
    if "org.xbmc.kodi.fileprovider" in app_source:
        raise AssertionError(
            "Android runtime retains Kodi's hardcoded FileProvider authority"
        )

    makefile = PACKAGING_MAKEFILE.read_text(encoding="utf-8")
    expected_target = "$(PREFIX)/lib/@APP_NAME_LC@/lib@APP_NAME_LC@.so:"
    if expected_target not in makefile:
        raise AssertionError("Android packaging target is not derived from APP_NAME")
    if "$(PREFIX)/lib/xbmc/lib@APP_NAME_LC@.so:" in makefile:
        raise AssertionError(
            "Android packaging retains a hardcoded Kodi library target"
        )

    manifest = ANDROID_MANIFEST.read_text(encoding="utf-8")
    if 'android:authorities="@APP_PACKAGE@.fileprovider"' not in manifest:
        raise AssertionError(
            "Android manifest FileProvider authority is not derived from APP_PACKAGE"
        )

    workflow = ANDROID_WORKFLOW.read_text(encoding="utf-8")
    required_workflow_contracts = (
        "CACHE_SCHEMA_VERSION: v3",
        "identity_hash=\"$(sha256sum version.txt | cut -d ' ' -f 1)\"",
        "steps.source-keys.outputs.identity_hash",
        'app_name_lc="$(read_version_field APP_NAME',
        'expected_core_library="lib${app_name_lc}.so"',
        'source_apk="${app_name_lc}app-${{ matrix.abi }}-release.apk"',
        "python3 tools/ci/jumpgate/test-jumpgate-android-release-workflow.py",
        "bash tools/ci/jumpgate/test-verify-android-release.sh",
        '"$EXPECTED_APK_SIGNER_SHA256" \\\n            "$expected_core_library"',
    )
    for contract in required_workflow_contracts:
        if contract not in workflow:
            raise AssertionError(
                f"Android workflow omits branding contract: {contract}"
            )
    if 'source_apk="kodiapp-' in workflow:
        raise AssertionError("Android workflow retains Kodi's APK staging name")
    if "libkodi.so" in workflow:
        raise AssertionError("Android workflow retains Kodi's core-library identity")


def verify_libandroidjni_intent_patch_contract():
    patch_name = LIBANDROIDJNI_PATCH.name
    patch_bytes = LIBANDROIDJNI_PATCH.read_bytes()
    if b"\r" in patch_bytes:
        raise AssertionError("libandroidjni Intent signature patch must use LF endings")
    patch = patch_bytes.decode("utf-8")
    makefile = LIBANDROIDJNI_MAKEFILE.read_text(encoding="utf-8")
    find_module = LIBANDROIDJNI_FIND_MODULE.read_text(encoding="utf-8")
    gitattributes = ROOT_GITATTRIBUTES.read_text(encoding="utf-8").splitlines()
    patch_attribute = "tools/depends/target/libandroidjni/*.patch text eol=lf"
    if gitattributes.count(patch_attribute) != 1:
        raise AssertionError(
            "root .gitattributes must enforce LF for libandroidjni patches"
        )
    version_source = LIBANDROIDJNI_VERSION_FILE.read_text(encoding="utf-8")
    version_match = re.search(
        r"^VERSION=([0-9a-f]{40})$", version_source, flags=re.MULTILINE
    )
    if version_match is None:
        raise AssertionError("LIBANDROIDJNI-VERSION lacks a pinned 40-character commit")
    dependency_commit = version_match.group(1)
    if dependency_commit != LIBANDROIDJNI_FIXTURE_COMMIT:
        raise AssertionError(
            "libandroidjni dependency commit changed to "
            f"{dependency_commit}; refresh the Intent.cpp fixture and patch contract "
            f"for {LIBANDROIDJNI_FIXTURE_COMMIT}"
        )

    try:
        fixture_bytes = LIBANDROIDJNI_INTENT_FIXTURE.read_bytes()
    except FileNotFoundError as error:
        raise AssertionError(
            "libandroidjni pinned Intent.cpp fixture is missing for "
            f"{LIBANDROIDJNI_FIXTURE_COMMIT}"
        ) from error
    fixture_sha256 = hashlib.sha256(fixture_bytes).hexdigest()
    if fixture_sha256 != LIBANDROIDJNI_FIXTURE_SHA256:
        raise AssertionError(
            "libandroidjni pinned Intent.cpp fixture drifted: expected SHA-256 "
            f"{LIBANDROIDJNI_FIXTURE_SHA256}, got {fixture_sha256}"
        )
    try:
        fixture_source = fixture_bytes.decode("utf-8")
    except UnicodeDecodeError as error:
        raise AssertionError(
            "libandroidjni pinned Intent.cpp fixture is not valid UTF-8"
        ) from error

    bad_descriptor = (
        '    "getStringExtra", "(Ljava/lang/String;I)Ljava/lang/String;",'
    )
    correct_descriptor = (
        '    "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;",'
    )
    if fixture_source.count(bad_descriptor) != 1 or correct_descriptor in fixture_source:
        raise AssertionError(
            "libandroidjni pinned Intent.cpp fixture lacks the exact unpatched descriptor"
        )

    for contract in (
        "--- a/src/Intent.cpp",
        "+++ b/src/Intent.cpp",
        f"-{bad_descriptor}",
        f"+{correct_descriptor}",
    ):
        if contract not in patch:
            raise AssertionError(
                f"libandroidjni Intent signature patch is missing {contract!r}"
            )

    deps_start = makefile.find("DEPS =")
    deps_end = makefile.find("\n\n", deps_start)
    if deps_start < 0 or deps_end < 0 or patch_name not in makefile[deps_start:deps_end]:
        raise AssertionError("libandroidjni target Makefile DEPS omits the Intent patch")
    make_patch_command = f"patch -p1 -i ../{patch_name}"
    if makefile.count(make_patch_command) != 1:
        raise AssertionError(
            "libandroidjni target Makefile must apply the Intent patch exactly once"
        )
    require_in_order(
        makefile,
        "$(ARCHIVE_TOOL) $(ARCHIVE_TOOL_FLAGS)",
        make_patch_command,
        "$(CMAKE) ..",
    )

    cmake_patch_path = (
        f"/tools/depends/target/${{${{CMAKE_FIND_PACKAGE_NAME}}_MODULE_LC}}/{patch_name}"
    )
    if find_module.count(cmake_patch_path) != 1 or find_module.count(
        'generate_patchcommand("${patches}")'
    ) != 1:
        raise AssertionError(
            "FindLibAndroidJNI must register the Intent patch exactly once"
        )
    require_in_order(
        find_module,
        cmake_patch_path,
        'generate_patchcommand("${patches}")',
        "BUILD_DEP_TARGET()",
    )

    with TemporaryDirectory(prefix="jumpgate-libandroidjni-patch-") as temporary:
        temporary_root = Path(temporary)
        temporary_source = temporary_root / "src" / "Intent.cpp"
        temporary_source.parent.mkdir(parents=True)
        shutil.copyfile(LIBANDROIDJNI_INTENT_FIXTURE, temporary_source)

        commands = (
            (
                "applicability check",
                ["git", "apply", "--check", str(LIBANDROIDJNI_PATCH)],
            ),
            ("application", ["git", "apply", str(LIBANDROIDJNI_PATCH)]),
        )
        for phase, command in commands:
            try:
                completed = subprocess.run(
                    command,
                    cwd=temporary_root,
                    check=False,
                    capture_output=True,
                    text=True,
                )
            except FileNotFoundError as error:
                raise AssertionError(
                    "git executable is required to validate the libandroidjni patch"
                ) from error
            if completed.returncode != 0:
                details = completed.stderr.strip() or completed.stdout.strip()
                raise AssertionError(
                    f"libandroidjni Intent patch {phase} failed for pinned commit "
                    f"{dependency_commit}: {details or 'git apply returned no output'}"
                )

        patched_source = temporary_source.read_text(encoding="utf-8")
        expected_source = fixture_source.replace(bad_descriptor, correct_descriptor)
        if patched_source != expected_source:
            raise AssertionError(
                "libandroidjni Intent patch did not produce only the exact corrected descriptor"
            )
        if bad_descriptor in patched_source or patched_source.count(correct_descriptor) != 1:
            raise AssertionError(
                "libandroidjni Intent patch output retains the bad descriptor or lacks "
                "one exact corrected descriptor"
            )

    patch_executable = shutil.which("patch")
    if patch_executable is not None:
        with TemporaryDirectory(prefix="jumpgate-libandroidjni-system-patch-") as temporary:
            temporary_root = Path(temporary)
            temporary_source = temporary_root / "src" / "Intent.cpp"
            temporary_source.parent.mkdir(parents=True)
            shutil.copyfile(LIBANDROIDJNI_INTENT_FIXTURE, temporary_source)
            completed = subprocess.run(
                [
                    patch_executable,
                    "--batch",
                    "--fuzz=0",
                    "-p1",
                    "-i",
                    str(LIBANDROIDJNI_PATCH),
                ],
                cwd=temporary_root,
                check=False,
                capture_output=True,
                text=True,
            )
            if completed.returncode != 0:
                details = completed.stderr.strip() or completed.stdout.strip()
                raise AssertionError(
                    "system patch could not apply the libandroidjni Intent fix without "
                    f"fuzz: {details or 'patch returned no output'}"
                )
            if temporary_source.read_text(encoding="utf-8") != expected_source:
                raise AssertionError(
                    "system patch did not produce the exact corrected Intent.cpp"
                )


def cmake_bracket_argument(source, index):
    match = re.match(r"\[(=*)\[", source[index:])
    if match is None:
        return None
    closing = "]" + match.group(1) + "]"
    content_start = index + match.end()
    content_end = source.find(closing, content_start)
    if content_end < 0:
        raise AssertionError("CMake source has an unterminated bracket argument")
    return source[content_start:content_end], content_end + len(closing)


def skip_cmake_layout(source, index):
    while index < len(source):
        if source[index].isspace():
            index += 1
            continue
        if source[index] != "#":
            break
        bracket = cmake_bracket_argument(source, index + 1)
        if bracket is not None:
            _, index = bracket
            continue
        newline = source.find("\n", index + 1)
        index = len(source) if newline < 0 else newline + 1
    return index


def parse_cmake_arguments(source, index):
    arguments = []
    token = []
    token_started = False
    depth = 1

    def finish_token():
        nonlocal token_started
        if token_started:
            arguments.append("".join(token))
            token.clear()
            token_started = False

    while index < len(source):
        character = source[index]
        if character == "#":
            finish_token()
            bracket = cmake_bracket_argument(source, index + 1)
            if bracket is not None:
                _, index = bracket
                continue
            newline = source.find("\n", index + 1)
            index = len(source) if newline < 0 else newline + 1
            continue
        if character.isspace():
            finish_token()
            index += 1
            continue
        if character == '"':
            token_started = True
            index += 1
            while index < len(source) and source[index] != '"':
                if source[index] == "\\" and index + 1 < len(source):
                    if source[index + 1] == "\n":
                        index += 2
                        continue
                    token.append(source[index + 1])
                    index += 2
                    continue
                token.append(source[index])
                index += 1
            if index >= len(source):
                raise AssertionError("CMake source has an unterminated quoted argument")
            index += 1
            continue
        bracket = cmake_bracket_argument(source, index)
        if bracket is not None:
            value, index = bracket
            token.extend(value)
            token_started = True
            continue
        if character == "(":
            depth += 1
            token.append(character)
            token_started = True
            index += 1
            continue
        if character == ")":
            depth -= 1
            if depth == 0:
                finish_token()
                return arguments, index + 1
            token.append(character)
            token_started = True
            index += 1
            continue
        if character == "\\" and index + 1 < len(source):
            if source[index + 1] == "\n":
                index += 2
                continue
            token.append(source[index + 1])
            token_started = True
            index += 2
            continue
        token.append(character)
        token_started = True
        index += 1
    raise AssertionError("CMake source has an unterminated command")


def cmake_commands(source):
    index = 0
    while index < len(source):
        index = skip_cmake_layout(source, index)
        match = re.match(r"[A-Za-z_][A-Za-z0-9_]*", source[index:])
        if match is None:
            index += 1
            continue
        name = match.group(0)
        command_end = index + match.end()
        opening = skip_cmake_layout(source, command_end)
        if opening >= len(source) or source[opening] != "(":
            index = command_end
            continue
        arguments, index = parse_cmake_arguments(source, opening + 1)
        yield name.casefold(), arguments


def verify_compile_info_package_command(source):
    generator_commands = [
        arguments
        for command, arguments in cmake_commands(source)
        if command == "add_custom_command"
        and COMPILE_INFO_GENERATOR_ARGUMENT in arguments
    ]
    if len(generator_commands) != 1:
        raise AssertionError(
            "root CMakeLists.txt must invoke GenerateCompileInfo.cmake exactly once"
        )

    arguments = generator_commands[0]
    if arguments.count(COMPILE_INFO_PACKAGE_ARGUMENT) != 1:
        raise AssertionError(
            "CompileInfo generation must propagate APP_PACKAGE exactly once"
        )
    if arguments.count("-P") != 1:
        raise AssertionError("CompileInfo generation must use exactly one -P argument")
    script_index = arguments.index("-P")
    if (
        script_index + 1 >= len(arguments)
        or arguments[script_index + 1] != COMPILE_INFO_GENERATOR_ARGUMENT
    ):
        raise AssertionError(
            "GenerateCompileInfo.cmake must immediately follow the -P argument"
        )

    package_index = arguments.index(COMPILE_INFO_PACKAGE_ARGUMENT)
    command_indices = [
        index
        for index, argument in enumerate(arguments[:script_index])
        if argument == "COMMAND"
    ]
    if not command_indices:
        raise AssertionError("CompileInfo generation lacks an applicable COMMAND")
    command_index = command_indices[-1]
    cmake_command_index = command_index + 1
    if (
        cmake_command_index >= script_index
        or arguments[cmake_command_index] != "${CMAKE_COMMAND}"
    ):
        raise AssertionError(
            "CompileInfo generation must run from COMMAND ${CMAKE_COMMAND}"
        )
    if "COMMAND" in arguments[package_index + 1 : script_index]:
        raise AssertionError(
            "CompileInfo APP_PACKAGE must not have an intervening COMMAND before -P"
        )
    if not command_index < cmake_command_index < package_index < script_index:
        raise AssertionError(
            "CompileInfo APP_PACKAGE must occur after COMMAND ${CMAKE_COMMAND} "
            "and before -P"
        )


def verify_compile_info_package_command_regressions():
    source = ROOT_CMAKE.read_text(encoding="utf-8")
    if source.count(COMPILE_INFO_PACKAGE_ARGUMENT) != 1:
        raise AssertionError(
            "CompileInfo mutation fixture requires one APP_PACKAGE argument"
        )
    if source.count(COMPILE_INFO_GENERATOR_ARGUMENT) != 1:
        raise AssertionError(
            "CompileInfo mutation fixture requires one generator argument"
        )

    missing = source.replace(COMPILE_INFO_PACKAGE_ARGUMENT, "", 1)
    generator_index = missing.index(COMPILE_INFO_GENERATOR_ARGUMENT)
    command_token = "COMMAND ${CMAKE_COMMAND}"
    command_index = missing.rfind(command_token, 0, generator_index)
    if command_index < 0:
        raise AssertionError(
            "CompileInfo mutation fixture cannot find COMMAND ${CMAKE_COMMAND}"
        )
    command_line_start = missing.rfind("\n", 0, command_index) + 1
    command_indent = missing[command_line_start:command_index]
    before_command = (
        missing[:command_index]
        + COMPILE_INFO_PACKAGE_ARGUMENT
        + "\n"
        + command_indent
        + missing[command_index:]
    )

    generator_line_start = missing.rfind("\n", 0, generator_index) + 1
    generator_indent = re.match(
        r"[ \t]*", missing[generator_line_start:generator_index]
    ).group(0)
    generator_end = generator_index + len(COMPILE_INFO_GENERATOR_ARGUMENT)
    post_script = (
        missing[:generator_end]
        + "\n"
        + generator_indent
        + COMPILE_INFO_PACKAGE_ARGUMENT
        + missing[generator_end:]
    )
    duplicate = source.replace(
        COMPILE_INFO_PACKAGE_ARGUMENT,
        f"{COMPILE_INFO_PACKAGE_ARGUMENT}\n{COMPILE_INFO_PACKAGE_ARGUMENT}",
        1,
    )
    mutations = (
        ("missing", missing, "propagate APP_PACKAGE exactly once"),
        ("duplicate", duplicate, "propagate APP_PACKAGE exactly once"),
        (
            "post-P",
            post_script,
            "must occur after COMMAND ${CMAKE_COMMAND} and before -P",
        ),
        (
            "before-COMMAND",
            before_command,
            "must not have an intervening COMMAND before -P",
        ),
    )
    for label, mutated_source, expected_error in mutations:
        try:
            verify_compile_info_package_command(mutated_source)
        except AssertionError as error:
            if expected_error not in str(error):
                raise AssertionError(
                    f"CompileInfo {label} mutation failed for the wrong reason: {error}"
                ) from error
        else:
            raise AssertionError(
                f"CompileInfo {label} mutation unexpectedly passed"
            )


def verify_compile_info_package_override():
    verify_compile_info_package_command(ROOT_CMAKE.read_text(encoding="utf-8"))

    cmake_executable = shutil.which("cmake")
    if cmake_executable is None:
        raise AssertionError("cmake is required to verify CompileInfo generation")

    override_package = "io.github.ruizkinio.jumpgate.x86diag.contract"
    with TemporaryDirectory(prefix="jumpgate-compile-info-") as temporary:
        build_root = Path(temporary)
        core_build_dir = Path("generated")
        output = build_root / core_build_dir / "xbmc" / "CompileInfo.cpp"
        output.parent.mkdir(parents=True)
        completed = subprocess.run(
            [
                cmake_executable,
                f"-DCORE_SOURCE_DIR={ROOT}",
                f"-DCORE_BUILD_DIR={core_build_dir.as_posix()}",
                f"-DCMAKE_BINARY_DIR={build_root}",
                f"-DAPP_PACKAGE={override_package}",
                "-P",
                str(COMPILE_INFO_GENERATOR),
            ],
            cwd=build_root,
            check=False,
            capture_output=True,
            text=True,
        )
        if completed.returncode != 0:
            details = "\n".join(
                part.strip()
                for part in (completed.stdout, completed.stderr)
                if part.strip()
            )
            raise AssertionError(
                "GenerateCompileInfo.cmake rejected APP_PACKAGE override: "
                f"{details or 'cmake returned no output'}"
            )
        generated = output.read_text(encoding="utf-8")
        expected = f'return "{override_package}";'
        if generated.count(expected) != 1:
            raise AssertionError(
                "generated CompileInfo.cpp did not preserve the APP_PACKAGE override"
            )


def cmake_constant_condition(arguments):
    if len(arguments) == 2 and arguments[0].casefold() == "not":
        value = cmake_constant_condition(arguments[1:])
        return None if value is None else not value
    if len(arguments) != 1:
        return None

    value = arguments[0].casefold()
    if value in ("0", "false", "off", "no", "n", "ignore", "notfound") or value.endswith(
        "-notfound"
    ):
        return False
    if value in ("1", "true", "on", "yes", "y"):
        return True
    return None


def cmake_truth_and(*values):
    if any(value is False for value in values):
        return False
    if all(value is True for value in values):
        return True
    return None


def cmake_truth_or(*values):
    if any(value is True for value in values):
        return True
    if all(value is False for value in values):
        return False
    return None


def cmake_truth_not(value):
    return None if value is None else not value


def cmake_reachable_commands(source):
    active = True
    blocks = []
    defined_commands = set()

    def require_block(command, kind):
        if not blocks or blocks[-1]["kind"] != kind:
            raise AssertionError(f"CMake source has a misplaced {command} command")
        return blocks[-1]

    for command, arguments in cmake_commands(source):
        if command == "if":
            condition = cmake_constant_condition(arguments)
            blocks.append(
                {
                    "kind": "if",
                    "parent_active": active,
                    "matched": condition,
                    "else_seen": False,
                }
            )
            active = cmake_truth_and(active, condition)
            continue
        if command == "elseif":
            frame = require_block(command, "if")
            if frame["else_seen"]:
                raise AssertionError("CMake source has a misplaced elseif command")
            condition = cmake_constant_condition(arguments)
            active = cmake_truth_and(
                frame["parent_active"],
                cmake_truth_not(frame["matched"]),
                condition,
            )
            frame["matched"] = cmake_truth_or(frame["matched"], condition)
            continue
        if command == "else":
            frame = require_block(command, "if")
            if frame["else_seen"]:
                raise AssertionError("CMake source has a misplaced else command")
            active = cmake_truth_and(
                frame["parent_active"], cmake_truth_not(frame["matched"])
            )
            frame["matched"] = True
            frame["else_seen"] = True
            continue
        if command == "endif":
            require_block(command, "if")
            active = blocks.pop()["parent_active"]
            continue
        if command in ("function", "macro"):
            if arguments:
                defined_commands.add(arguments[0].casefold())
            blocks.append({"kind": command, "parent_active": active})
            active = False
            continue
        if command in ("endfunction", "endmacro"):
            kind = command.removeprefix("end")
            require_block(command, kind)
            active = blocks.pop()["parent_active"]
            continue
        if command in ("foreach", "while"):
            condition = (
                cmake_constant_condition(arguments) if command == "while" else None
            )
            blocks.append({"kind": command, "parent_active": active})
            active = False if active is False or condition is False else None
            continue
        if command in ("endforeach", "endwhile"):
            kind = command.removeprefix("end")
            require_block(command, kind)
            active = blocks.pop()["parent_active"]
            continue
        if command in ("block", "return", "break", "continue") or command in defined_commands:
            if active is not False:
                yield "__unsupported_control__", [command], active is True
            continue
        if active is not False:
            yield command, arguments, active is True

    if blocks:
        raise AssertionError(
            f"CMake source has an unterminated {blocks[-1]['kind']} command"
        )


def expand_cmake_value(value, variables, known_variables, stack=()):
    expanded = value
    for _ in range(20):
        matches = tuple(CMAKE_VARIABLE.finditer(expanded))
        if not matches:
            break
        pieces = []
        offset = 0
        for match in matches:
            name = match.group(1)
            if name in stack:
                return ()
            if name in known_variables:
                replacements = (known_variables[name],)
            elif name in variables:
                replacements = tuple(
                    replacement
                    for candidate in variables[name]
                    for replacement in expand_cmake_value(
                        candidate,
                        variables,
                        known_variables,
                        (*stack, name),
                    )
                )
            else:
                return ()
            if not replacements:
                return ()
            pieces.extend((expanded[offset : match.start()], ";".join(replacements)))
            offset = match.end()
        pieces.append(expanded[offset:])
        updated = "".join(pieces)
        if updated == expanded:
            break
        expanded = updated
    if CMAKE_VARIABLE.search(expanded) or "$<" in expanded:
        return ()
    return tuple(part.strip() for part in expanded.split(";") if part.strip())


def resolve_cmake_cpp_sources(values, cmake_path, variables, known_variables):
    sources = set()
    for value in values:
        for expanded in expand_cmake_value(value, variables, known_variables):
            if not expanded.endswith(".cpp"):
                continue
            path = Path(expanded)
            if not path.is_absolute():
                path = cmake_path.parent / path
            sources.add(path.resolve())
    return sources


def cmake_target_is_test(target_name, cmake_path, source_root):
    try:
        relative_parts = cmake_path.relative_to(source_root).parts[:-1]
    except ValueError:
        relative_parts = cmake_path.parts[:-1]
    return "test" in target_name.casefold() or any(
        part.casefold() == "test" for part in relative_parts
    )


def cmake_target_cpp_sources(cmake_path, source_root):
    cmake_path = cmake_path.resolve()
    source_root = source_root.resolve()
    project_root = source_root.parent
    known_variables = {
        "CMAKE_CURRENT_LIST_DIR": str(cmake_path.parent),
        "CMAKE_CURRENT_SOURCE_DIR": str(cmake_path.parent),
        "CMAKE_SOURCE_DIR": str(project_root),
        "PROJECT_SOURCE_DIR": str(project_root),
    }
    variables = {}
    targets = {}
    analysis_supported = True

    def resolve_target_name(value):
        names = expand_cmake_value(value, variables, known_variables)
        return names[0] if len(names) == 1 else None

    def declare_target(target_name, values, is_test):
        if target_name is None:
            return
        targets[target_name] = (
            is_test,
            resolve_cmake_cpp_sources(
                values, cmake_path, variables, known_variables
            ),
        )

    def remove_list_items(variable, values):
        if not values:
            return
        removed_values = set()
        for value in values:
            expanded_values = expand_cmake_value(value, variables, known_variables)
            if not expanded_values:
                variables.pop(variable, None)
                return
            removed_values.update(expanded_values)
        variables[variable] = [
            expanded
            for value in variables.get(variable, [])
            for expanded in expand_cmake_value(value, variables, known_variables)
            if expanded not in removed_values
        ]

    # Variables retain only values guaranteed to survive every possible path.
    for command, arguments, guaranteed in cmake_reachable_commands(
        cmake_path.read_text(encoding="utf-8")
    ):
        if command == "__unsupported_control__":
            analysis_supported = False
            continue
        if command == "set" and arguments:
            if guaranteed:
                variables[arguments[0]] = arguments[1:]
            else:
                variables.pop(arguments[0], None)
            continue
        if command == "unset" and arguments:
            variables.pop(arguments[0], None)
            continue
        if command == "list" and len(arguments) >= 2:
            operation = arguments[0].casefold()
            variable = arguments[1]
            if operation == "append":
                if guaranteed:
                    variables.setdefault(variable, []).extend(arguments[2:])
            elif operation == "prepend":
                if guaranteed:
                    variables[variable] = arguments[2:] + variables.get(variable, [])
            elif operation == "remove_item":
                remove_list_items(variable, arguments[2:])
            elif operation not in ("remove_duplicates", "reverse", "sort"):
                variables.pop(variable, None)
            continue
        if not guaranteed:
            continue
        if command in ("core_add_library", "core_add_test_library"):
            is_test = command == "core_add_test_library"
            source_variables = (
                ("SOURCES", "SUPPORTED_SOURCES") if is_test else ("SOURCES",)
            )
            target_name = resolve_target_name(arguments[0]) if arguments else None
            declare_target(
                target_name,
                [
                    value
                    for variable in source_variables
                    for value in variables.get(variable, [])
                ],
                is_test,
            )
            if target_name is not None and not is_test:
                variables["CORE_LIBRARY"] = [target_name]
            continue
        if command in ("add_library", "add_executable") and arguments:
            target_name = resolve_target_name(arguments[0])
            if target_name is not None:
                declare_target(
                    target_name,
                    arguments[1:],
                    cmake_target_is_test(target_name, cmake_path, source_root),
                )
            continue
        if command == "target_sources" and len(arguments) >= 2:
            target_name = resolve_target_name(arguments[0])
            if target_name in targets:
                targets[target_name][1].update(
                    resolve_cmake_cpp_sources(
                        arguments[1:], cmake_path, variables, known_variables
                    )
                )

    if not analysis_supported:
        return set(), set()

    production_sources = set()
    test_sources = set()
    for is_test, sources in targets.values():
        destination = test_sources if is_test else production_sources
        destination.update(sources)
    return production_sources, test_sources


def is_jumpgate_test_source(path):
    return path.name.startswith("Test") and (
        "Jumpgate" in path.name
        or path.name.startswith("TestUPnPPlayerOpenPublication")
    )


def format_source_paths(paths):
    rendered = []
    for path in sorted(paths):
        try:
            rendered.append(path.relative_to(ROOT).as_posix())
        except ValueError:
            rendered.append(str(path))
    return ", ".join(rendered)


def verify_jumpgate_cmake_source_inventory(source_root=XBMC_SOURCE_ROOT):
    source_root = source_root.resolve()
    production_sources = {
        path.resolve()
        for path in source_root.rglob("*Jumpgate*.cpp")
        if not path.name.startswith("Test")
    }
    test_sources = {
        path.resolve()
        for path in source_root.rglob("*.cpp")
        if is_jumpgate_test_source(path)
    }

    listed_production_sources = set()
    listed_test_sources = set()
    production_target_sources = set()
    test_target_sources = set()
    for cmake_path in source_root.rglob("CMakeLists.txt"):
        production_target, test_target = cmake_target_cpp_sources(
            cmake_path, source_root
        )
        for source_path in production_target | test_target:
            if is_jumpgate_test_source(source_path):
                listed_test_sources.add(source_path)
                if source_path in test_target:
                    test_target_sources.add(source_path)
            elif "Jumpgate" in source_path.name and not source_path.name.startswith(
                "Test"
            ):
                listed_production_sources.add(source_path)
                if source_path in production_target:
                    production_target_sources.add(source_path)

    inventory_failures = (
        (
            "stale Jumpgate production CMake sources",
            listed_production_sources - production_sources,
        ),
        (
            "unlisted Jumpgate production sources",
            production_sources - production_target_sources,
        ),
        (
            "stale Jumpgate test CMake sources",
            listed_test_sources - test_sources,
        ),
        (
            "unlisted Jumpgate test sources",
            test_sources - test_target_sources,
        ),
    )
    for label, paths in inventory_failures:
        if paths:
            raise AssertionError(f"{label}: {format_source_paths(paths)}")

    return test_sources


def verify_cmake_parser_regressions():
    with TemporaryDirectory(prefix="jumpgate-cmake-parser-") as temporary:
        source_root = Path(temporary) / "xbmc"
        cmake_dir = source_root / "fixture"
        cmake_dir.mkdir(parents=True)
        cmake_path = cmake_dir / "CMakeLists.txt"
        cmake_path.write_text(
            """# set(SOURCES JumpgateLineComment.cpp)
#[==[
set(SOURCES JumpgateBracketComment.cpp)
core_add_library(bracket_comment)
]==]
message(STATUS "JumpgateMentionOnly.cpp")
message(STATUS [=[JumpgateBracketArgument.cpp]=])
set(UNRELATED JumpgateUnrelatedVariable.cpp)
set(SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/JumpgateVariable.cpp
    JumpgateRemoved.cpp
    ${UNKNOWN_SOURCE_ROOT}/JumpgateUnknown.cpp
    JumpgateTemplate.cpp.in)
list(REMOVE_ITEM SOURCES JumpgateRemoved.cpp)
list(APPEND SOURCES JumpgateAppend.cpp)
list(PREPEND SOURCES JumpgatePrepend.cpp)
if(FALSE)
  list(APPEND SOURCES JumpgateIfFalse.cpp)
  add_library(fixture_if_false STATIC JumpgateIfFalseTarget.cpp)
else()
  list(APPEND SOURCES JumpgateElse.cpp)
endif()
if(TRUE)
  list(APPEND SOURCES JumpgateIfTrue.cpp)
endif()
core_add_library(fixture)
add_library(fixture_direct STATIC JumpgateDirect.cpp)
target_sources(fixture_direct PRIVATE JumpgateTargetSources.cpp)
target_sources(fixture_missing PRIVATE TestJumpgateUnboundTarget.cpp)
set(SOURCES TestJumpgateCore.cpp)
list(APPEND SUPPORTED_SOURCES TestJumpgateSupported.cpp)
core_add_test_library(fixture_test)
""",
            encoding="utf-8",
        )

        production_sources, test_sources = cmake_target_cpp_sources(
            cmake_path, source_root
        )
        expected_production = {
            (cmake_dir / name).resolve()
            for name in (
                "JumpgateVariable.cpp",
                "JumpgateAppend.cpp",
                "JumpgatePrepend.cpp",
                "JumpgateElse.cpp",
                "JumpgateIfTrue.cpp",
                "JumpgateDirect.cpp",
                "JumpgateTargetSources.cpp",
            )
        }
        expected_tests = {
            (cmake_dir / name).resolve()
            for name in ("TestJumpgateCore.cpp", "TestJumpgateSupported.cpp")
        }
        if production_sources != expected_production:
            raise AssertionError(
                "CMake parser target fixture drifted: "
                f"expected {format_source_paths(expected_production)}, "
                f"got {format_source_paths(production_sources)}"
            )
        if test_sources != expected_tests:
            raise AssertionError(
                "CMake parser test-target fixture drifted: "
                f"expected {format_source_paths(expected_tests)}, "
                f"got {format_source_paths(test_sources)}"
            )


def verify_cmake_inventory_regressions():
    cases = (
        (
            "stale Jumpgate production CMake sources",
            (),
            "set(SOURCES JumpgateStale.cpp)\ncore_add_library(stale)\n",
            ("JumpgateStale.cpp",),
            (),
        ),
        (
            "unlisted Jumpgate production sources",
            ("JumpgateUnlisted.cpp",),
            'set(SOURCES)\nmessage(STATUS "JumpgateUnlisted.cpp")\n'
            "core_add_library(unlisted)\n",
            ("JumpgateUnlisted.cpp",),
            (),
        ),
        (
            "stale Jumpgate test CMake sources",
            (),
            "set(SOURCES TestJumpgateStale.cpp)\ncore_add_test_library(stale_test)\n",
            ("TestJumpgateStale.cpp",),
            (),
        ),
        (
            "unlisted Jumpgate test sources",
            ("TestJumpgateUnlisted.cpp",),
            'set(SOURCES)\nmessage(STATUS "TestJumpgateUnlisted.cpp")\n'
            "core_add_test_library(unlisted_test)\n",
            ("TestJumpgateUnlisted.cpp",),
            (),
        ),
        (
            "unlisted Jumpgate test sources",
            ("TestJumpgateIfFalse.cpp",),
            "if(FALSE)\n"
            "  set(SOURCES TestJumpgateIfFalse.cpp)\n"
            "  core_add_test_library(if_false_test)\n"
            "endif()\n",
            ("TestJumpgateIfFalse.cpp",),
            (),
        ),
        (
            "unlisted Jumpgate test sources",
            ("TestJumpgateUncalledFunction.cpp",),
            "function(register_hidden_test)\n"
            "  set(SOURCES TestJumpgateUncalledFunction.cpp)\n"
            "  core_add_test_library(hidden_test)\n"
            "endfunction()\n",
            ("TestJumpgateUncalledFunction.cpp",),
            (),
        ),
        (
            "unlisted Jumpgate test sources",
            ("TestJumpgateFalseLoop.cpp",),
            "while(FALSE)\n"
            "  set(SOURCES TestJumpgateFalseLoop.cpp)\n"
            "  core_add_test_library(false_loop_test)\n"
            "endwhile()\n",
            ("TestJumpgateFalseLoop.cpp",),
            (),
        ),
        (
            "unlisted Jumpgate test sources",
            ("TestJumpgateLoopPresent.cpp", "TestJumpgateUnknownLoop.cpp"),
            "set(SOURCES TestJumpgateLoopPresent.cpp)\n"
            "foreach(candidate IN LISTS UNKNOWN_TEST_SOURCES)\n"
            "  list(APPEND SOURCES TestJumpgateUnknownLoop.cpp)\n"
            "endforeach()\n"
            "core_add_test_library(unknown_loop_test)\n",
            ("TestJumpgateUnknownLoop.cpp",),
            ("TestJumpgateLoopPresent.cpp",),
        ),
        (
            "unlisted Jumpgate test sources",
            ("TestJumpgateBranchPresent.cpp", "TestJumpgateUnknownBranch.cpp"),
            "set(SOURCES TestJumpgateBranchPresent.cpp)\n"
            "if(ENABLE_UNKNOWN_BRANCH)\n"
            "  list(APPEND SOURCES TestJumpgateUnknownBranch.cpp)\n"
            "endif()\n"
            "core_add_test_library(unknown_branch_test)\n",
            ("TestJumpgateUnknownBranch.cpp",),
            ("TestJumpgateBranchPresent.cpp",),
        ),
        (
            "unlisted Jumpgate test sources",
            ("TestJumpgateRemoved.cpp",),
            "set(SOURCES TestJumpgateRemoved.cpp)\n"
            "list(REMOVE_ITEM SOURCES TestJumpgateRemoved.cpp)\n"
            "core_add_test_library(removed_test)\n",
            ("TestJumpgateRemoved.cpp",),
            (),
        ),
        (
            "unlisted Jumpgate test sources",
            ("TestJumpgateConditionalRemoval.cpp",),
            "set(SOURCES TestJumpgateConditionalRemoval.cpp)\n"
            "if(ENABLE_CONDITIONAL_REMOVAL)\n"
            "  list(REMOVE_ITEM SOURCES TestJumpgateConditionalRemoval.cpp)\n"
            "endif()\n"
            "core_add_test_library(conditional_removal_test)\n",
            ("TestJumpgateConditionalRemoval.cpp",),
            (),
        ),
        (
            "unlisted Jumpgate test sources",
            ("TestJumpgateFiltered.cpp",),
            "set(SOURCES TestJumpgateFiltered.cpp)\n"
            'list(FILTER SOURCES EXCLUDE REGEX "^TestJumpgate")\n'
            "core_add_test_library(filtered_test)\n",
            ("TestJumpgateFiltered.cpp",),
            (),
        ),
        (
            "unlisted Jumpgate test sources",
            ("TestJumpgateRemovedAt.cpp",),
            "set(SOURCES TestJumpgateRemovedAt.cpp)\n"
            "list(REMOVE_AT SOURCES 0)\n"
            "core_add_test_library(removed_at_test)\n",
            ("TestJumpgateRemovedAt.cpp",),
            (),
        ),
        (
            "unlisted Jumpgate test sources",
            ("TestJumpgateUnset.cpp",),
            "set(SOURCES TestJumpgateUnset.cpp)\n"
            "unset(SOURCES)\n"
            "core_add_test_library(unset_test)\n",
            ("TestJumpgateUnset.cpp",),
            (),
        ),
        (
            "unlisted Jumpgate test sources",
            ("TestJumpgateFamilyPresent.cpp", "TestJumpgateWrongTarget.cpp"),
            "set(SOURCES TestJumpgateFamilyPresent.cpp)\n"
            "core_add_test_library(exact_test)\n"
            "add_library(production STATIC TestJumpgateWrongTarget.cpp)\n",
            ("TestJumpgateWrongTarget.cpp",),
            ("TestJumpgateFamilyPresent.cpp",),
        ),
    )
    with TemporaryDirectory(prefix="jumpgate-cmake-inventory-") as temporary:
        for index, case in enumerate(cases):
            expected_label, files, cmake_source, required_details, forbidden_details = (
                case
            )
            source_root = Path(temporary) / str(index) / "xbmc"
            source_root.mkdir(parents=True)
            for name in files:
                (source_root / name).write_text("// policy fixture\n", encoding="utf-8")
            (source_root / "CMakeLists.txt").write_text(cmake_source, encoding="utf-8")
            try:
                verify_jumpgate_cmake_source_inventory(source_root)
            except AssertionError as error:
                message = str(error)
                if not message.startswith(f"{expected_label}:"):
                    raise AssertionError(
                        f"{expected_label} fixture failed as {error}"
                    ) from error
                if any(detail not in message for detail in required_details):
                    raise AssertionError(
                        f"{expected_label} fixture omitted exact failure details: {error}"
                    ) from error
                if any(detail in message for detail in forbidden_details):
                    raise AssertionError(
                        f"{expected_label} fixture blamed a valid target member: {error}"
                    ) from error
            else:
                raise AssertionError(f"{expected_label} fixture unexpectedly passed")


def collect_jumpgate_test_cases(source_root=XBMC_SOURCE_ROOT):
    test_sources = verify_jumpgate_cmake_source_inventory(source_root)
    test_cases = []
    empty_test_sources = []
    declared_names = {}
    duplicate_names = []
    for path in sorted(test_sources):
        declarations = GTEST_DECLARATION.findall(path.read_text(encoding="utf-8"))
        if not declarations:
            empty_test_sources.append(path)
        for suite, name in declarations:
            full_name = f"{suite}.{name}"
            if full_name in declared_names:
                duplicate_names.append((full_name, declared_names[full_name], path))
            else:
                declared_names[full_name] = path
            test_cases.append((path, suite, name))
    if empty_test_sources:
        raise AssertionError(
            "Jumpgate test sources declare no gtests: "
            f"{format_source_paths(empty_test_sources)}"
        )
    if duplicate_names:
        details = ", ".join(
            f"{name} ({format_source_paths((first, second))})"
            for name, first, second in duplicate_names
        )
        raise AssertionError(f"Jumpgate sources duplicate gtest names: {details}")
    return test_cases


def select_jumpgate_test_cases(test_cases, filter_patterns=HOST_TEST_FILTER_PATTERNS):
    return [
        (path, suite, name)
        for path, suite, name in test_cases
        if any(
            fnmatchcase(f"{suite}.{name}", pattern) for pattern in filter_patterns
        )
    ]


def parse_normalized_gtest_inventory(source):
    tests = set()
    suite = None
    for raw_line in source.splitlines():
        line = re.sub(r"\x1b\[[0-9;]*m", "", raw_line).rstrip()
        content = line.partition("#")[0].rstrip()
        if not content.strip():
            continue
        if not content[0].isspace():
            if not content.endswith("."):
                suite = None
                continue
            suite = content[:-1].strip()
            if not suite:
                raise AssertionError("built gtest inventory has an empty suite name")
            continue
        if suite is None:
            raise AssertionError(
                f"built gtest inventory has a test without a suite: {content.strip()}"
            )
        test_name = content.strip()
        full_name = f"{suite}.{test_name}"
        if full_name in tests:
            raise AssertionError(
                f"built gtest inventory duplicates normalized test: {full_name}"
            )
        tests.add(full_name)
    return tests


def reconcile_gtest_inventory(expected_names, inventory_source):
    built_names = parse_normalized_gtest_inventory(inventory_source)
    missing_names = sorted(set(expected_names) - built_names)
    unexpected_names = sorted(built_names - set(expected_names))
    if missing_names or unexpected_names:
        details = []
        if missing_names:
            details.append("missing " + ", ".join(missing_names))
        if unexpected_names:
            details.append("unexpected " + ", ".join(unexpected_names))
        raise AssertionError(
            "built Jumpgate gtest inventory differs from source declarations: "
            + "; ".join(details)
        )
    return len(built_names)


def verify_built_gtest_inventory(inventory_path):
    test_cases = collect_jumpgate_test_cases()
    selected_cases = select_jumpgate_test_cases(test_cases)
    omitted_cases = set(test_cases) - set(selected_cases)
    if omitted_cases:
        raise AssertionError(
            "Jumpgate host filter omits source-declared gtests before reconciliation"
        )
    expected_names = {f"{suite}.{name}" for _, suite, name in selected_cases}
    return reconcile_gtest_inventory(
        expected_names, Path(inventory_path).read_text(encoding="utf-8")
    )


def verify_gtest_inventory_regressions():
    expected_names = {
        "TestJumpgateSameFamily.Present",
        "TestJumpgateSameFamily.Missing",
    }
    complete_inventory = """Running main() from gtest_main.cc
TestJumpgateSameFamily.  # fixture suite
  Present
  Missing  # fixture test
"""
    if reconcile_gtest_inventory(expected_names, complete_inventory) != 2:
        raise AssertionError("exact built gtest inventory fixture count drifted")

    missing_inventory = complete_inventory.replace("  Missing  # fixture test\n", "")
    try:
        reconcile_gtest_inventory(expected_names, missing_inventory)
    except AssertionError as error:
        if "missing TestJumpgateSameFamily.Missing" not in str(error):
            raise AssertionError(
                f"same-family missing-test fixture failed as {error}"
            ) from error
    else:
        raise AssertionError("same-family missing built gtest unexpectedly passed")


def verify_exact_inventory_workflow(workflow):
    verifier = re.compile(
        r'^\s*python3\s+tools/ci/jumpgate/test-android-branding\.py\s+'
        r'--verify-gtest-inventory\s+"\$inventory_file"\s*$',
        re.MULTILINE,
    )
    matches = tuple(verifier.finditer(workflow))
    capture_index = workflow.find('tee "$inventory_file"')
    if len(matches) != 1 or capture_index < 0 or matches[0].start() < capture_index:
        raise AssertionError(
            "Android workflow must reconcile the exact normalized built gtest inventory"
        )
    if any(
        legacy in workflow
        for legacy in ("suite_families=(", "family_counts=(", "family_count=")
    ):
        raise AssertionError(
            "Android workflow must not substitute suite-family counts for exact inventory"
        )
    return matches[0]


def verify_jumpgate_host_test_policy():
    test_cases = collect_jumpgate_test_cases()

    missing_families = [
        family
        for family in HOST_TEST_SUITE_FAMILIES
        if not any(suite.startswith(family) for _, suite, _ in test_cases)
    ]
    if missing_families:
        raise AssertionError(
            "Jumpgate gtest suite families are missing from source inventory: "
            + ", ".join(f"{family}*" for family in missing_families)
        )

    workflow = ANDROID_WORKFLOW.read_text(encoding="utf-8")
    filter_values = re.findall(
        r"^\s*JUMPGATE_HOST_TEST_FILTER:\s*['\"]([^'\"]+)['\"]\s*$",
        workflow,
        re.MULTILINE,
    )
    if len(filter_values) != 1:
        raise AssertionError(
            "Android workflow must define one JUMPGATE_HOST_TEST_FILTER"
        )
    filter_patterns = tuple(filter_values[0].split(":"))
    if (
        len(filter_patterns) != len(HOST_TEST_FILTER_PATTERNS)
        or set(filter_patterns) != set(HOST_TEST_FILTER_PATTERNS)
    ):
        raise AssertionError(
            "Android workflow host filter must select exactly: "
            + ":".join(HOST_TEST_FILTER_PATTERNS)
        )

    selected_cases = select_jumpgate_test_cases(test_cases, filter_patterns)
    if not selected_cases:
        raise AssertionError("Android workflow host filter selects zero Jumpgate gtests")
    omitted_cases = set(test_cases) - set(selected_cases)
    if omitted_cases:
        omitted_names = sorted(
            f"{path.relative_to(ROOT).as_posix()}:{suite}.{name}"
            for path, suite, name in omitted_cases
        )
        raise AssertionError(
            "Android workflow host filter omits Jumpgate gtests: "
            + ", ".join(omitted_names)
        )

    inventory_command = re.compile(
        r'"\$KODI_HOST_BUILD_DIR/kodi-test"[\s\\]+'
        r'--gtest_list_tests[\s\\]+'
        r'--gtest_filter="\$JUMPGATE_HOST_TEST_FILTER"'
    )
    execution_command = re.compile(
        r'"\$KODI_HOST_BUILD_DIR/kodi-test"[\s\\]+'
        r'--gtest_filter="\$JUMPGATE_HOST_TEST_FILTER"'
    )
    if inventory_command.search(workflow) is None:
        raise AssertionError(
            "Android workflow must inventory the built gtest binary with the host filter"
        )
    if execution_command.search(workflow) is None:
        raise AssertionError(
            "Android workflow must execute the built gtest binary with the host filter"
        )
    if 'tee "$inventory_file"' not in workflow:
        raise AssertionError("built gtest inventory must remain visible in CI output")
    verify_exact_inventory_workflow(workflow)

    return len(selected_cases)


def verify_host_policy_regressions():
    workflow = ANDROID_WORKFLOW.read_text(encoding="utf-8")
    verifier = verify_exact_inventory_workflow(workflow)
    malformed_workflow = (
        workflow[: verifier.start()] + workflow[verifier.end() :]
    )
    try:
        verify_exact_inventory_workflow(malformed_workflow)
    except AssertionError as error:
        if "exact normalized" not in str(error):
            raise AssertionError(
                f"missing exact-inventory verifier fixture failed as {error}"
            ) from error
    else:
        raise AssertionError("missing exact-inventory verifier unexpectedly passed")


def verify_uri_logging_privacy():
    main_activity = MAIN_ACTIVITY.read_text(encoding="utf-8")
    title_extraction = extract_braced_block(
        main_activity, "private String extractTitleFromUri(Uri uri)"
    )
    expected = 'Log.w(TAG, "Main: Failed to extract title from URI query params", e);'
    if expected not in title_extraction:
        raise AssertionError("title extraction lacks a source-safe exception warning")
    if 'Failed to extract title from URI query params: " + uri' in title_extraction:
        raise AssertionError("title extraction logs the complete private media URI")


def verify_credential_context_wiring():
    header = CREDENTIAL_STORE_HEADER.read_text(encoding="utf-8")
    source = CREDENTIAL_STORE_SOURCE.read_text(encoding="utf-8")
    app_source = APP_SOURCE.read_text(encoding="utf-8")

    for contract in (
        "explicit CAndroidJumpgateCredentialStore(const CJNIContext& context);",
        "jni::jhobject m_context;",
    ):
        if contract not in header:
            raise AssertionError(f"Android credential store is missing {contract!r}")
    for contract in (
        "CAndroidJumpgateCredentialStore::CAndroidJumpgateCredentialStore(",
        ": m_context(context.get_raw())",
        "m_context.setGlobal();",
    ):
        if contract not in source:
            raise AssertionError(
                f"Android credential context binding is missing {contract!r}"
            )
    if source.count("const jhobject& context = m_context;") != 3:
        raise AssertionError(
            "Android credential operations are not bound to the injected context"
        )
    if "GetAppInstance" in source:
        raise AssertionError(
            "Android credential store uses the retired global app accessor"
        )
    initialization = extract_braced_block(
        app_source, "bool CXBMCApp::InitializeJumpgateProfileRuntime()"
    )
    if (
        "std::make_unique<KODI::JUMPGATE::CAndroidJumpgateCredentialStore>(*this)"
        not in initialization
    ):
        raise AssertionError(
            "CXBMCApp does not inject its exact JNI context into the credential store"
        )


def extract_braced_block(source, anchor):
    start = source.find(anchor)
    if start < 0:
        raise AssertionError(f"Back wiring is missing {anchor!r}")
    opening = source.find("{", start + len(anchor))
    if opening < 0:
        raise AssertionError(f"Back wiring has no body for {anchor!r}")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"Back wiring has an unterminated body for {anchor!r}")


def require_in_order(source, *contracts):
    offset = 0
    for contract in contracts:
        index = source.find(contract, offset)
        if index < 0:
            raise AssertionError(f"Back wiring is missing or misorders {contract!r}")
        offset = index + len(contract)


def verify_warm_task_handoff_contract(
    main_activity, external_activity, result_coordinator, app_source
):
    normal_exit = extract_braced_block(
        main_activity, "public synchronized void exitExternalPlayerMode("
    )
    post_commit_exit = normal_exit[normal_exit.find("reservation.commit()") :]
    warm_exit = extract_braced_block(post_commit_exit, "if (wasStandalone)")
    require_in_order(
        warm_exit,
        "mExternalResultProducer.isCallerResultAcknowledged(generation, requestId)",
        "mExternalPlayerMode = false",
        "mExternalResultProducer.clear(generation, requestId)",
        "prepareWarmTaskHandoff(generation, requestId, callerResultAcknowledged)",
        "mWasStandalone = false",
        "hideLoadingOverlay()",
    )
    if "handler.post(" in warm_exit or "moveTaskToBack(" in warm_exit:
        raise AssertionError("Java terminal delivery must not schedule the warm task handoff")
    if "finish()" in warm_exit or "scheduleExternalPlayerProcessExit(" in warm_exit:
        raise AssertionError("warm external exit must retain Main and its native engine")

    warm_exit_end = post_commit_exit.find(warm_exit) + len(warm_exit)
    cold_exit = extract_braced_block(post_commit_exit[warm_exit_end:], "else")
    require_in_order(
        cold_exit, "mExternalPlayerMode = false", "finishColdExternalPlayerTask()"
    )
    if "moveTaskToBack(" in cold_exit or "handoffWarmExternalPlayerTask(" in cold_exit:
        raise AssertionError("cold external exit must finish Main instead of retaining its task")

    native_exit = extract_braced_block(app_source, "bool CXBMCApp::ExitExternalPlayerMode(")
    native_warm_exit = extract_braced_block(native_exit, "if (wasStandalone)")
    require_in_order(
        native_warm_exit,
        'call_method<void>(m_context, "exitExternalPlayerMode"',
        "ReturnToStandaloneMode()",
    )
    if "HandoffWarmExternalPlayerTask(" in native_warm_exit:
        raise AssertionError("native warm exit signals readiness before result-state reset")
    native_warm_end = native_exit.find(native_warm_exit) + len(native_warm_exit)
    native_cold_exit = extract_braced_block(native_exit[native_warm_end:], "else")
    if "handoffWarmExternalPlayerTask" in native_cold_exit:
        raise AssertionError("native cold exit must not schedule a warm task handoff")
    pending_delivery = extract_braced_block(
        app_source, "void CXBMCApp::DeliverPendingExternalPlayerResult(\n"
    )
    require_in_order(
        pending_delivery,
        "const bool wasStandalone",
        "ExitExternalPlayerMode(*result, lifecycleOperation, deferPlayerCleanup)",
        "m_playbackResultState.Reset(lifecycleOperation, result->generation)",
        "if (exited && reset && wasStandalone)",
        "HandoffWarmExternalPlayerTask(result->generation, result->requestId)",
    )

    rejected_exit = extract_braced_block(
        app_source, "void CXBMCApp::DeliverRejectedExternalPlaybackResult("
    )
    require_in_order(
        rejected_exit,
        'call_method<void>(m_context, "exitExternalPlayerMode"',
        "const bool reset = m_playbackResultState.Reset(lifecycleOperation, generation)",
        "if (reset && returnToStandalone)",
        "HandoffWarmExternalPlayerTask(generation, owner->requestId)",
    )

    queued_execute = extract_braced_block(
        app_source, "void CXBMCApp::ExecuteQueuedExternalPlayerResult("
    )
    queued_cancel = extract_braced_block(
        app_source, "void CXBMCApp::CancelQueuedExternalPlayerResult("
    )
    for body in (queued_execute, queued_cancel):
        if "PostExternalPlayerResultConvergence(" not in body:
            raise AssertionError("queued terminal fallback bypasses native convergence")
        if "postExternalPlayerCancellationResult" in body:
            raise AssertionError("queued terminal fallback can clear Java without native cleanup")

    post_convergence = extract_braced_block(
        app_source, "void CXBMCApp::PostExternalPlayerResultConvergence("
    )
    require_in_order(
        post_convergence,
        '(*payload)["lifecycleToken"]',
        '(*payload)["generation"]',
        '(*payload)["requestId"]',
        '(*payload)["wasStandalone"]',
        '"postExternalPlayerResultConvergence", "(JJ)Z"',
        "ConvergeExternalPlayerResultCallback",
        "payload.release()",
    )
    java_convergence = extract_braced_block(
        main_activity, "public boolean postExternalPlayerResultConvergence("
    )
    require_in_order(java_convergence, "handler.post(", "_callNative(callbackAddress, payloadAddress)")
    convergence_callback = extract_braced_block(
        app_source, "void CXBMCApp::ConvergeExternalPlayerResultCallback("
    )
    if "ConvergeExternalPlayerResultCallback(CVariant* rawPayload)" not in app_source:
        raise AssertionError("native convergence callback does not match _callNative's exact type")
    require_in_order(
        convergence_callback,
        "AcquireAppInstance(lifecycleToken)",
        "ConvergeExternalPlayerResult(generation, requestId, wasStandalone)",
    )
    convergence = extract_braced_block(
        app_source, "void CXBMCApp::ConvergeExternalPlayerResult("
    )
    require_in_order(
        convergence,
        "BeginLifecycleOperation()",
        "owner->generation != generation",
        "owner->requestId != requestId",
        "m_wasStandalone.load(std::memory_order_relaxed) != wasStandalone",
        "DeliverPendingExternalPlayerResult(lifecycleOperation)",
    )

    activity_delivery = extract_braced_block(
        external_activity, "synchronized boolean deliver("
    )
    require_in_order(
        activity_delivery,
        "mHost.setTerminalResult(terminal)",
        "mHost.finishOwner()",
        "if (terminal.exitProcess)",
        "mHost.confirmWarmTaskHandoff(terminal.generation, terminal.requestId)",
        "mHost.clearExact(terminal.requestId, terminal.generation)",
    )
    caller_cancellation = extract_braced_block(
        external_activity, "synchronized boolean cancelForCaller()"
    )
    require_in_order(
        caller_cancellation,
        "mHost.setCanceledResult()",
        "mHost.finishOwner()",
        "mHost.confirmWarmTaskHandoff(generation, requestId)",
        "finishCancellation(requestId, generation)",
    )
    teardown_cancellation = extract_braced_block(
        external_activity, "synchronized boolean cancelForTeardown("
    )
    require_in_order(
        teardown_cancellation,
        "mHost.setCanceledResult()",
        "mHost.finishOwner()",
        "mHost.confirmWarmTaskHandoff(generation, requestId)",
        "mHost.clearExact(requestId, generation)",
    )
    confirmation = extract_braced_block(
        external_activity, "public void confirmWarmTaskHandoff("
    )
    if "Main.confirmWarmExternalPlayerResult(generation, requestId)" not in confirmation:
        raise AssertionError("warm result owner does not confirm exact caller delivery")

    cancel_handoff = extract_braced_block(
        main_activity, "private synchronized void cancelPendingWarmTaskHandoff()"
    )
    if "mWarmTaskHandoff.cancel()" not in cancel_handoff:
        raise AssertionError("warm task handoff cancellation bypasses the executable controller")
    prepare_handoff = extract_braced_block(
        main_activity, "private synchronized void prepareWarmTaskHandoff("
    )
    require_in_order(
        prepare_handoff,
        "mWarmTaskHandoff.prepare(",
        "mBackLifecycleToken",
        "generation",
        "requestId",
        "callerResultAcknowledged",
    )

    schedule_handoff = extract_braced_block(
        main_activity, "private synchronized void scheduleWarmTaskHandoffIfReady("
    )
    require_in_order(
        schedule_handoff,
        "mWarmTaskHandoff.scheduleIfReady(",
        "lifecycleToken",
        "generation",
        "requestId",
    )
    for forbidden in (
        "handler.post",
        "moveTaskToBack",
        "MainActivity",
        "mBackLifecycleToken",
        "isFinishing()",
        "isDestroyed()",
        "mExternalPlayerMode",
        "activeGeneration()",
        "preparedRequestId()",
    ):
        if forbidden in schedule_handoff:
            raise AssertionError(
                f"Main schedule adapter retains warm-task policy {forbidden!r}"
            )

    main_constructor = extract_braced_block(main_activity, "public Main()")
    require_in_order(
        main_constructor,
        "new ExternalPlayerResultCoordinator.WarmTaskHandoffController(",
        "new ExternalPlayerResultCoordinator.WarmTaskScheduler()",
        "handler.post(callback)",
        "handler.postDelayed(callback, delayMs)",
        "handler.removeCallbacks(callback)",
        "Main.this",
        "this::captureWarmTaskLifecycleState",
        "attempt -> moveTaskToBack(true)",
        "new ExternalPlayerResultCoordinator.WarmTaskHandoffObserver()",
        "WARM_TASK_HANDOFF_RETRY_DELAY_MS",
    )
    for retained_policy in (
        "WarmTaskMoveAttemptResult.MOVED",
        "WarmTaskMoveAttemptResult.REFUSED",
        "WarmTaskMoveAttemptResult.STALE",
        "try {",
        "catch (RuntimeException",
    ):
        if retained_policy in main_constructor:
            raise AssertionError(
                f"Main move adapter retains production policy {retained_policy!r}"
            )
    lifecycle_capture = extract_braced_block(
        main_activity,
        "private synchronized ExternalPlayerResultCoordinator.WarmTaskLifecycleState",
    )
    require_in_order(
        lifecycle_capture,
        "new ExternalPlayerResultCoordinator.WarmTaskLifecycleState(",
        "MainActivity == this",
        "mBackLifecycleToken",
        "isFinishing()",
        "isDestroyed()",
        "mExternalPlayerMode",
        "mExternalResultProducer.activeGeneration()",
        "mExternalResultProducer.preparedRequestId()",
    )
    if "if (" in lifecycle_capture or "moveTaskToBack" in lifecycle_capture:
        raise AssertionError("Main lifecycle adapter retains warm-task policy")
    attempt_diagnostic = extract_braced_block(
        main_constructor, "public void onAttemptFailed("
    )
    fallback_diagnostic = extract_braced_block(main_constructor, "public void onFallback()")
    for diagnostic in (attempt_diagnostic, fallback_diagnostic):
        if "requestId" in diagnostic or "generation" in diagnostic:
            raise AssertionError("warm task handoff diagnostics expose exact owner identifiers")
    if "leaving Kodi in foreground" not in fallback_diagnostic:
        raise AssertionError("warm task exhaustion has no explicit safe foreground fallback")

    native_ready = extract_braced_block(
        main_activity, "public synchronized void handoffWarmExternalPlayerTask("
    )
    require_in_order(
        native_ready,
        "mWarmTaskHandoff.markNativeReady(lifecycleToken, generation, requestId)",
        "scheduleWarmTaskHandoffIfReady(lifecycleToken, generation, requestId)",
    )
    result_delivered = extract_braced_block(
        main_activity, "private synchronized boolean confirmWarmExternalPlayerResultInternal("
    )
    require_in_order(
        result_delivered,
        "ExternalPlayerResultCoordinator.isValidRequestId(requestId)",
        "mWarmTaskHandoff.confirmCallerResult(generation, requestId)",
        "scheduleWarmTaskHandoffIfReady(mBackLifecycleToken, generation, requestId)",
        "generation == 0",
        "mExternalResultProducer.confirmCallerResult(generation, requestId)",
        "mExternalPlayerMode",
        "mWasStandalone",
        "mExternalResultProducer.confirmCallerResult(generation, requestId)",
    )

    producer = extract_braced_block(result_coordinator, "public static final class Producer")
    require_in_order(
        producer,
        "mPreparedCallerResultAcknowledged",
        "mCallerResultAcknowledged",
        'mPendingCallerResultRequestId = ""',
        "boolean callerResultAcknowledged = mPendingCallerResultRequestId.equals(requestId)",
        'mPendingCallerResultRequestId = ""',
        "mPreparedCallerResultAcknowledged = callerResultAcknowledged",
        "boolean callerResultAcknowledged = mPreparedCallerResultAcknowledged",
        "begin(generation, requestId)",
        "mPreparedCallerResultAcknowledged = false",
        "mCallerResultAcknowledged = callerResultAcknowledged",
    )
    producer_confirmation = extract_braced_block(
        producer, "public synchronized boolean confirmCallerResult("
    )
    require_in_order(
        producer_confirmation,
        "isValidRequestId(requestId)",
        "generation == 0",
        "mPreparedRequestId.equals(requestId)",
        "mPreparedCallerResultAcknowledged = true",
        "mGeneration > 0",
        "mRequestId.equals(requestId)",
        "mOwnerCanceled",
        "mCallerResultAcknowledged = true",
        "mGeneration > 0 || !mPreparedRequestId.isEmpty()",
        "mPendingCallerResultRequestId.isEmpty()",
        "mPendingCallerResultRequestId.equals(requestId)",
        "mPendingCallerResultRequestId = requestId",
        "matches(generation, requestId)",
        "mCallerResultAcknowledged = true",
    )

    lifecycle_state = extract_braced_block(
        result_coordinator, "public static final class WarmTaskLifecycleState"
    )
    lifecycle_allows = extract_braced_block(
        lifecycle_state, "private boolean allows("
    )
    require_in_order(
        lifecycle_allows,
        "attempt != null",
        "mCurrentActivity",
        "attempt.lifecycleToken() > 0",
        "attempt.lifecycleToken() == mLifecycleToken",
        "!mFinishing",
        "!mDestroyed",
        "!mExternalPlayerMode",
        "mActiveGeneration == 0",
        "!isValidRequestId(mPreparedRequestId)",
    )

    handoff_state = extract_braced_block(
        result_coordinator, "public static final class WarmTaskHandoff"
    )
    for contract in (
        "MAX_MOVE_ATTEMPTS = 3",
        "reserveAttempt(",
        "completeAttempt(",
        "cancelAttempt(",
        "generation == 0 ? ownsRequest(requestId) : owns(generation, requestId)",
        "attempt != mPendingAttempt",
        "mMoveAttempts >= MAX_MOVE_ATTEMPTS",
        "generation == 0 ? ownsRequest(requestId) : owns(generation, requestId)",
    ):
        if contract not in handoff_state:
            raise AssertionError(f"warm task handoff state is missing {contract!r}")
    handoff_completion = extract_braced_block(
        handoff_state, "public synchronized WarmTaskMoveResult completeAttempt("
    )
    require_in_order(
        handoff_completion,
        "attempt != mPendingAttempt",
        "mPendingAttempt = null",
        "if (moved)",
        "clear()",
        "WarmTaskMoveResult.MOVED",
        "WarmTaskMoveResult.RETRY",
        "WarmTaskMoveResult.EXHAUSTED",
    )

    handoff_controller = extract_braced_block(
        result_coordinator, "public static final class WarmTaskHandoffController"
    )
    for contract in (
        "WarmTaskScheduler mScheduler",
        "Object mLifecycleLock",
        "WarmTaskLifecycleStateSource mLifecycleStateSource",
        "WarmTaskMoveAdapter mMoveAdapter",
        "WarmTaskHandoffObserver mObserver",
        "mScheduler.post(task)",
        "mScheduler.postDelayed(task, mRetryDelayMs)",
        "mScheduler.removeCallbacks(pending)",
        "synchronized (mLifecycleLock)",
        "mLifecycleStateSource.capture()",
        "lifecycleState.allows(attempt)",
        "mMoveAdapter.moveTaskToBack(attempt)",
        "? WarmTaskMoveAttemptResult.MOVED",
        ": WarmTaskMoveAttemptResult.REFUSED",
        "WarmTaskMoveAttemptResult.STALE",
        "WarmTaskMoveResult.EXHAUSTED",
        "mHandoff.clear()",
        "mObserver.onFallback()",
        "task.mAttempt.lifecycleToken()",
        "task.mAttempt.generation()",
        "task.mAttempt.requestId()",
        "mRetryDelayMs",
    ):
        if contract not in handoff_controller:
            raise AssertionError(f"executable warm task controller is missing {contract!r}")
    production_move = extract_braced_block(
        handoff_controller, "private WarmTaskMoveAttemptResult move("
    )
    require_in_order(
        production_move,
        "synchronized (mLifecycleLock)",
        "mLifecycleStateSource.capture()",
        "lifecycleState == null",
        "!lifecycleState.allows(attempt)",
        "WarmTaskMoveAttemptResult.STALE",
        "mMoveAdapter.moveTaskToBack(attempt)",
        "WarmTaskMoveAttemptResult.MOVED",
        "WarmTaskMoveAttemptResult.REFUSED",
        "catch (RuntimeException e)",
        "WarmTaskMoveAttemptResult.REFUSED",
    )

    gradle = ANDROID_APP_GRADLE.read_text(encoding="utf-8")
    require_in_order(
        gradle,
        "tasks.register('compileExternalPlayerResultTests', JavaCompile)",
        "tasks.register('externalPlayerResultTests', JavaExec)",
        "dependsOn tasks.named('compileExternalPlayerResultTests')",
        "mainClass = '@APP_PACKAGE@.ExternalPlayerResultCoordinatorTest'",
        "tasks.named('check').configure",
        "dependsOn tasks.named('externalPlayerResultTests')",
    )
    if "preBuild.dependsOn 'externalPlayerResultTests'" not in gradle:
        raise AssertionError("mandatory Android APK assembly does not run result tests")
    workflow = ANDROID_WORKFLOW.read_text(encoding="utf-8")
    if 'make -C "$KODI_BUILD_DIR" apk' not in workflow:
        raise AssertionError("protected Android CI bypasses mandatory APK assembly tests")

    executable_tests = EXTERNAL_RESULT_TEST.read_text(encoding="utf-8")
    for test in (
        "prePrepareCallerAcknowledgementConvergesThroughHandoff()",
        "staleAndRejectedCallerConfirmationsStayFenced()",
        "warmTaskMoveFailureRetriesWithoutLosingState()",
        "warmTaskProductionLifecycleGuardsFenceMove()",
        "warmTaskMoveExceptionRetriesToForegroundFallback()",
        "warmTaskInitialPostRefusalRetriesExecutably()",
        "warmTaskDelayedPostRefusalExhaustsToFallback()",
        "warmTaskMoveRetriesExhaustToForegroundFallback()",
        "warmTaskNewerLaunchFencesQueuedControllerCallback()",
        "warmTaskRetryCannotAttachToNewerSameLifecycleLaunch()",
        "warmTaskDuplicateCallbacksAndNewerLaunchStayFenced()",
        "processExitRequiresCallerAndNativeCleanup()",
        "callerResultCompletionContractMatchesStremio()",
    ):
        if executable_tests.count(test) < 2:
            raise AssertionError(f"external result host test is not invoked: {test}")

    creation = extract_braced_block(main_activity, "public void onCreate(")
    require_in_order(
        creation,
        "mExternalResultProducer.prepare(initialRequestId)",
        "cancelPendingWarmTaskHandoff()",
    )
    new_intent = extract_braced_block(main_activity, "protected void onNewIntent(")
    require_in_order(
        new_intent,
        "mExternalResultProducer.prepare(externalRequestId)",
        "cancelPendingWarmTaskHandoff()",
    )
    active_admission = extract_braced_block(
        main_activity, "public synchronized boolean beginExternalPlayerMode("
    )
    require_in_order(
        active_admission,
        "mExternalResultProducer.admitPrepared(generation, requestId)",
        "cancelPendingWarmTaskHandoff()",
    )
    destruction = extract_braced_block(main_activity, "public void onDestroy()")
    require_in_order(
        destruction,
        "cancelPendingWarmTaskHandoff()",
        "mBackLifecycleToken = 0",
        "MainActivity = null",
    )


def verify_native_back_wiring():
    manifest = ANDROID_MANIFEST.read_text(encoding="utf-8")
    if (
        'android:enableOnBackInvokedCallback="@bool/enable_on_back_invoked_callback"'
        not in manifest
    ):
        raise AssertionError(
            "Main activity must use a version-qualified Back callback boolean"
        )
    if ">false</bool>" not in BACK_BOOL_BASE.read_text(encoding="utf-8"):
        raise AssertionError("raw Back delivery must remain enabled through API 35")
    if ">true</bool>" not in BACK_BOOL_V36.read_text(encoding="utf-8"):
        raise AssertionError("the framework Back callback must be enabled from API 36")

    main_activity = MAIN_ACTIVITY.read_text(encoding="utf-8")
    for forbidden in (
        "PredictiveBackApi33",
        "import android.window.OnBackInvokedCallback;",
        "public boolean onKeyDown(",
        "public boolean onKeyUp(",
        "PREDICTIVE_BACK_RETRY",
        "mPhysicalBackInProgress",
        "_requestOpenSettings",
    ):
        if forbidden in main_activity:
            raise AssertionError(f"Java retains rejected Back handling: {forbidden}")
    if main_activity.count("new OnBackAnimationCallback()") != 1:
        raise AssertionError("API 36 must register exactly one OnBackAnimationCallback")
    predictive_back = extract_braced_block(
        main_activity, "private static final class PredictiveBackApi36"
    )
    require_in_order(
        predictive_back,
        "final long lifecycleToken",
        "BackEvent.EDGE_RIGHT",
        "BACK_SOURCE_GESTURE_RIGHT",
        "BackEvent.EDGE_NONE",
        "BACK_SOURCE_BUTTON",
        "activity._onBackStarted(lifecycleToken, source);",
        "activity.handler.postDelayed(",
        "activity._onBackCancelled(lifecycleToken);",
        "activity._onBackInvoked(lifecycleToken);",
    )
    if "_onBackLongPress(lifecycleToken);" not in main_activity:
        raise AssertionError(
            "the API 36 button timer must carry its Activity lifecycle token"
        )
    require_in_order(
        extract_braced_block(main_activity, "private void registerApi36BackCallback()"),
        "Build.VERSION.SDK_INT < 36",
        "PredictiveBackApi36.register(this, mBackLifecycleToken)",
    )
    require_in_order(
        extract_braced_block(
            main_activity, "public void onCreate(Bundle savedInstanceState)"
        ),
        'mExternalPlayerMode = getIntent().getBooleanExtra("external_player_mode", false);',
        'System.loadLibrary("@APP_NAME_LC@");',
        "mBackLifecycleToken = _onBackCreated(mExternalPlayerMode);",
        "mBackLifecycleToken == 0",
        "super.onCreate(savedInstanceState);",
        "registerApi36BackCallback();",
    )
    require_in_order(
        extract_braced_block(main_activity, "public void onDestroy()"),
        "unregisterApi36BackCallback();",
        "final long lifecycleToken = mBackLifecycleToken;",
        "mBackLifecycleToken = 0;",
        "_onBackDestroyed(lifecycleToken);",
        "super.onDestroy();",
    )
    if "public long getBackLifecycleToken()" not in main_activity:
        raise AssertionError(
            "the native app cannot obtain the token from its exact Activity"
        )

    for forbidden_delay in (
        "mExternalPlayerMode ? 2000",
        "new DelayedIntent(\n              new Intent(intent),\n              500",
    ):
        if forbidden_delay in main_activity:
            raise AssertionError("external playback intent still uses an arbitrary startup delay")
    require_in_order(
        extract_braced_block(main_activity, "public void onNativeIntentReady("),
        "lifecycleToken != mBackLifecycleToken",
        "mNativeIntentReady = true",
        "dispatchPendingNativeIntents();",
    )
    require_in_order(
        extract_braced_block(main_activity, "private void dispatchPendingNativeIntents()"),
        "mPaused || !mNativeIntentReady",
        "mDelayedIntents.clear();",
        "_onNewIntent(",
    )

    app_source = APP_SOURCE.read_text(encoding="utf-8")
    initialize = extract_braced_block(app_source, "void CXBMCApp::Initialize()")
    if 'call_method<void>(m_context, "onNativeIntentReady"' not in initialize:
        raise AssertionError("native application readiness does not release queued intents")
    external_back = extract_braced_block(
        app_source, "bool CXBMCApp::ExecuteQueuedBackCommand("
    )
    require_in_order(
        external_back,
        "if (nestedKodiUiVisible)",
        "return ExecuteKodiBackCommand(false)",
        "if (osdVisible)",
        "CancelPendingExternalPlaybackFromBack()",
        "ExitExternalPlaybackForBack(payload.playbackToken)",
        "g_application.OnAction(CAction(ACTION_STOP))",
    )
    external_exit = extract_braced_block(
        app_source, "bool CXBMCApp::ExitExternalPlayerMode("
    )
    require_in_order(
        external_exit,
        "const bool wasStandalone",
        "const bool deferNativeCleanup = deferPlayerCleanup || !wasStandalone",
        "if (!deferNativeCleanup)",
        'call_method<void>(m_context, "exitExternalPlayerMode"',
    )
    finish_cold = extract_braced_block(
        main_activity, "private void finishColdExternalPlayerTask()"
    )
    require_in_order(
        finish_cold,
        "mMainView.setVisibility(View.INVISIBLE)",
        "mDecorView.setBackgroundColor(Color.BLACK)",
        "isTaskRoot()",
        "finishAndRemoveTask()",
        "finish()",
    )

    result_store = (
        ROOT
        / "tools"
        / "android"
        / "packaging"
        / "xbmc"
        / "src"
        / "ExternalPlayerResultStore.java.in"
    ).read_text(encoding="utf-8")
    for cleanup_contract in (
        "PROCESS_EXIT_CLEANUP_READY",
        "markProcessCleanupReady(",
        "processExitState.markCleanupReady(",
        "processExitState.wasCleanupReady()",
    ):
        if cleanup_contract not in result_store:
            raise AssertionError(
                f"process exit can preempt native cleanup: {cleanup_contract!r}"
            )

    result_coordinator = EXTERNAL_RESULT_SOURCE.read_text(encoding="utf-8")
    for completion_contract in (
        'CALLER_RESULT_ACTION = "com.mxtech.intent.result.VIEW"',
        'CALLER_END_BY_COMPLETION = "playback_completion"',
        "callerEndReason(terminal.completed)",
    ):
        if completion_contract not in result_coordinator + result_store:
            raise AssertionError(
                f"Stremio completion result contract is missing: {completion_contract!r}"
            )
    if "org.videolan.vlc.player.result" in result_store:
        raise AssertionError("VLC-style result cannot signal nonzero-duration completion to Stremio")

    subtitle_transport = (
        ROOT
        / "xbmc"
        / "platform"
        / "android"
        / "activity"
        / "AndroidJumpgateSubtitleTransport.cpp"
    ).read_text(encoding="utf-8")
    subtitle_process = extract_braced_block(
        subtitle_transport, "void CAndroidJumpgateSubtitleController::Process("
    )
    require_in_order(
        subtitle_process,
        "appPlayer->AddSubtitle(path)",
        "appPlayer->SetSubtitleVisible(true)",
    )

    jni_header = JNI_MAIN_HEADER.read_text(encoding="utf-8")
    jni_source = JNI_MAIN_SOURCE.read_text(encoding="utf-8")
    for contract in (
        "static jlong _onBackCreated(JNIEnv* env, jobject context, jboolean initialExternalMode)",
        "_onBackStarted(JNIEnv* env, jobject context, jlong lifecycleToken, jint source)",
        "_onBackLongPress(JNIEnv* env, jobject context, jlong lifecycleToken)",
        "_onBackCancelled(JNIEnv* env, jobject context, jlong lifecycleToken)",
        "_onBackInvoked(JNIEnv* env, jobject context, jlong lifecycleToken)",
        "_onBackDestroyed(JNIEnv* env, jobject context, jlong lifecycleToken)",
    ):
        if contract not in jni_header:
            raise AssertionError(f"JNI Back adapter is missing {contract!r}")
    if not re.search(
        r"GetJumpgateBackLifecycleToken\(\s*const ANativeActivity\* nativeActivity\)",
        jni_header,
    ):
        raise AssertionError(
            "JNI Back token lookup is not bound to the exact ANativeActivity"
        )
    for name, signature in (
        ("_onBackCreated", "(Z)J"),
        ("_onBackStarted", "(JI)V"),
        ("_onBackLongPress", "(J)V"),
        ("_onBackCancelled", "(J)V"),
        ("_onBackInvoked", "(J)V"),
        ("_onBackDestroyed", "(J)V"),
    ):
        if f'{{"{name}", "{signature}"' not in jni_source:
            raise AssertionError(f"JNI Back registration is missing {name}{signature}")
    if "CAndroidKey::XBMC_Key" in jni_source:
        raise AssertionError(
            "JNI must delegate to the Back dispatcher instead of synthesizing Back"
        )
    for contract in (
        "NewWeakGlobalRef(context)",
        "IsSameObject(context, m_activity)",
        "GetBackActivityBinding().OnCreated(env, context, initialExternalMode == JNI_TRUE)",
        "RetireAppLifecycle(token)",
        "GetBackActivityBinding().OnDestroyed(env, context, token)",
    ):
        if contract not in jni_source:
            raise AssertionError(
                f"JNI Activity identity binding is missing {contract!r}"
            )
    for callback, dispatcher_call in (
        ("_onBackStarted", "OnApi36BackStarted(token, source)"),
        ("_onBackLongPress", "OnApi36BackLongPress(token)"),
        ("_onBackCancelled", "OnApi36BackCancelled(token)"),
        ("_onBackInvoked", "OnApi36BackInvoked(token)"),
    ):
        callback_body = extract_braced_block(
            jni_source, f"CJNIMainActivity::{callback}("
        )
        if (
            "m_appInstance" in callback_body
            or "GetBackActivityBinding().IsCurrent(env, context, token)"
            not in callback_body
            or dispatcher_call not in callback_body
        ):
            raise AssertionError(
                f"JNI Back callback {callback} is not token and Activity qualified"
            )

    event_loop = EVENT_LOOP.read_text(encoding="utf-8")
    require_in_order(
        event_loop,
        "m_activityHandler->onBackInputEvent(event)",
        "onKeyboardEvent(event)",
    )
    if "CXBMCApp::Get().HandleJumpgateBackEvent" in event_loop:
        raise AssertionError(
            "an old EventLoop must not relabel Back with the current app"
        )
    app_header = APP_HEADER.read_text(encoding="utf-8")
    app_source = APP_SOURCE.read_text(encoding="utf-8")
    raw_adapter = extract_braced_block(app_source, "bool CXBMCApp::onBackInputEvent(")
    require_in_order(
        raw_adapter,
        "AKeyEvent_getDownTime(event)",
        "AKEY_EVENT_FLAG_CANCELED",
        "AKeyEvent_getRepeatCount(event)",
        "CJNIBuild::SDK_INT >= 36",
        "OnApi36RawBack",
        "OnLegacyRawUp",
    )
    for contract in (
        "SetExternalMode(m_jumpgateBackLifecycleToken, mode)",
        "SetWindowReady(m_jumpgateBackLifecycleToken, ready)",
        "jni::CJNIMainActivity::GetJumpgateBackLifecycleToken(nativeActivity)",
    ):
        if contract not in app_source:
            raise AssertionError(
                f"CXBMCApp Back lifecycle binding is missing {contract!r}"
            )
    peripheral_adapter = extract_braced_block(
        app_source, "bool CXBMCApp::onInputDeviceEvent("
    )
    if (
        "AKEYCODE_BACK" in peripheral_adapter
        or "onBackInputEvent" in peripheral_adapter
    ):
        raise AssertionError(
            "ordinary Back handling must not use the peripheral input seam"
        )
    action_executor = extract_braced_block(
        app_source, "bool CXBMCApp::DispatchBackCommand("
    )
    require_in_order(action_executor, "QueueBackCommand(command, 0, 0, context)")
    if "SendMsg(" in action_executor:
        raise AssertionError(
            "Back input must not wait for a blocked Kodi application thread"
        )
    queued_back_payload = extract_braced_block(
        app_source, "struct CXBMCApp::QueuedBackCommand"
    )
    require_in_order(
        queued_back_payload,
        "void Execute() override",
        "context->Execute(",
        "ExecuteQueuedBackCommand(*this)",
    )
    queued_back = extract_braced_block(
        app_source, "bool CXBMCApp::QueueBackCommand("
    )
    require_in_order(
        queued_back,
        "std::make_unique<QueuedBackCommand>",
        "m_jumpgateBackLifecycleToken",
        "m_playbackResultState.CurrentGeneration()",
        "messenger->PostCallback(",
    )
    external_back = extract_braced_block(
        app_source, "bool CXBMCApp::ExecuteExternalBackCommand("
    )
    require_in_order(
        external_back,
        "m_externalPlayerMode.load",
        "CancelPendingExternalPlaybackFromBack()",
        "QueueBackCommand(BackCommand::EXTERNAL_BACK",
    )
    queued_executor = extract_braced_block(
        app_source, "bool CXBMCApp::ExecuteQueuedBackCommand("
    )
    require_in_order(
        queued_executor,
        "IsCurrentLifecycle",
        "m_externalPlayerMode.load",
        "m_playbackResultState.IsCurrent",
        "CServiceBroker::GetGUI()",
        "GetDialog(WINDOW_DIALOG_VIDEO_OSD)",
        "videoOsd->IsDialogRunning()",
        "videoOsd->Close(true)",
        "g_application.OnAction(CAction(ACTION_STOP));",
    )
    if "ACTION_FULLSCREEN" in queued_executor:
        raise AssertionError(
            "external short Back must dismiss the OSD or issue ACTION_STOP directly"
        )
    kodi_back = extract_braced_block(
        app_source, "bool CXBMCApp::ExecuteKodiBackCommand("
    )
    require_in_order(
        kodi_back,
        "g_application.IsInitialized()",
        "CKey::MODIFIER_LONG",
        "KODI::KEYBOARD::KEY_HOLD_TRESHOLD + 1",
        "const CKey key",
        "ProcessKeyPress(key)",
    )
    if "CAndroidKey::XBMC_Key(AKEYCODE_BACK" in app_source:
        raise AssertionError(
            "Back command execution must not enqueue split Android key events"
        )

    new_intent = extract_braced_block(app_source, "void CXBMCApp::onNewIntent(")
    if (
        "SendMsg(" in new_intent
        or "m_playbackResultState.BeginLifecycleOperation()" in new_intent
    ):
        raise AssertionError(
            "Android's UI thread must not block on playback admission or PlayFile"
        )
    require_in_order(
        new_intent,
        "TryBeginLifecycleOperation()",
        "postNativeExternalIntentRetry",
        "BeginExternalPlaybackAdmission",
        "beginExternalPlayerMode",
        "cancellationRequested",
        "ApplyActiveJumpgateProfile",
        "QueueExternalPlayback(std::move(item)",
    )
    queued_playback = extract_braced_block(
        app_source, "bool CXBMCApp::QueueExternalPlayback("
    )
    require_in_order(
        queued_playback,
        "std::make_unique<QueuedExternalPlayback>",
        "admissionGeneration",
        "admissionToken",
        "messenger->PostCallback(",
    )
    playback_executor = extract_braced_block(
        app_source, "void CXBMCApp::ExecuteQueuedExternalPlayback("
    )
    require_in_order(
        playback_executor,
        "IsCurrentLifecycle",
        "IsLatestExternalPlaybackAdmission",
        "PostMsgOwned",
        "TMSG_MEDIA_PLAY",
    )
    if "SendMsg(" in playback_executor or "payload.item.release()" in playback_executor:
        raise AssertionError("queued playback retains a blocking or raw media hand-off")
    pending_cancel = extract_braced_block(
        app_source, "bool CXBMCApp::CancelPendingExternalPlaybackFromBack("
    )
    require_in_order(
        pending_cancel,
        "m_playbackResultState.CurrentGeneration()",
        "CancelPendingAdmission(generation)",
        "m_playbackResultState.Finish",
        "QueueExternalPlayerResult",
    )
    input_header = INPUT_MANAGER_HEADER.read_text(encoding="utf-8")
    input_source = INPUT_MANAGER_SOURCE.read_text(encoding="utf-8")
    if "bool ProcessKeyPress(const CKey& key);" not in input_header:
        raise AssertionError("the input manager lacks an atomic synthetic keypress API")
    require_in_order(
        extract_braced_block(input_source, "bool CInputManager::ProcessKeyPress("),
        "OnKey(key)",
        "OnKeyUp(key)",
    )

    create_body = extract_braced_block(app_header, "static CXBMCApp& Create(")
    require_in_order(
        create_body,
        "if (m_appinstance)",
        "Destroy();",
        "auto app = std::shared_ptr<CXBMCApp>(new CXBMCApp",
        "std::shared_ptr<KODI::JUMPGATE::CJumpgateBackDispatcher::ISink>",
        "app->m_jumpgateBackPublicationToken =",
        "dispatcher.PublishSink(app->m_jumpgateBackLifecycleToken, std::move(sink))",
        "INVALID_PUBLICATION_TOKEN",
        'throw std::runtime_error("Unable to publish CXBMCApp Back sink")',
        "m_appinstance = app;",
    )
    if "m_appinstance.reset(new CXBMCApp" in create_body:
        raise AssertionError(
            "CXBMCApp must not become globally live before Back sink publication"
        )
    require_in_order(
        extract_braced_block(app_header, "static void Destroy()"),
        "m_appinstance->m_jumpgateBackLifecycleToken",
        "m_appinstance->m_jumpgateBackPublicationToken",
        "m_appinstance.reset()",
    )

    win_system = WIN_SYSTEM.read_text(encoding="utf-8")
    require_in_order(
        extract_braced_block(win_system, "bool CWinSystemAndroid::InitWindowSystem("),
        "m_jumpgateBackLifecycleToken =",
        "GetJumpgateBackLifecycleToken()",
    )
    for function, ready in (
        ("bool CWinSystemAndroid::DestroyWindowSystem(", "false"),
        ("bool CWinSystemAndroid::CreateNewWindow(", "true"),
        ("bool CWinSystemAndroid::DestroyWindow(", "false"),
    ):
        body = extract_braced_block(win_system, function)
        require_in_order(
            body,
            "GetJumpgateBackDispatcher().SetWindowReady(",
            "m_jumpgateBackLifecycleToken",
            ready,
        )
    if "CXBMCApp::Get().SetJumpgateBackInputReady" in win_system:
        raise AssertionError(
            "stale window callbacks must retain their originating token"
        )

    coordinator_header = BACK_COORDINATOR_HEADER.read_text(encoding="utf-8")
    coordinator_source = BACK_COORDINATOR_SOURCE.read_text(encoding="utf-8")
    coordinator_test = BACK_COORDINATOR_TEST.read_text(encoding="utf-8")
    for contract in (
        "IDLE",
        "PRESSED",
        "LONG_CONSUMED",
        "COMMIT_PENDING",
        "DESTROYED",
        "PASS_THROUGH",
        "DISPATCH_EXTERNAL_BACK",
        "DISPATCH_KODI_BACK_SHORT",
        "DISPATCH_KODI_BACK_LONG",
        "OPEN_EXTERNAL_SETTINGS",
        "LifecycleToken",
        "INVALID_LIFECYCLE_TOKEN",
        "PublicationToken",
        "INVALID_PUBLICATION_TOKEN",
        "std::shared_ptr<ISink>",
    ):
        if contract not in coordinator_header:
            raise AssertionError(f"native Back coordinator is missing {contract}")
    for test_contract in (
        "StandaloneLegacyRawSequencePassesThroughUnchanged",
        "MismatchedStandaloneUpIsConsumedWithoutClearingPassThroughRouteAcrossModeFlip",
        "MismatchedExternalUpIsConsumedWithoutClearingConsumedRouteAcrossModeFlip",
        "EarlyCommitIsStoredOnceAndFlushedOnceWhenReady",
        "TransientWindowLossPreservesPendingCommitAndRawRoute",
        "StandaloneLegacySequenceKeepsPassingThroughAfterModeEntersExternal",
        "ExternalLegacySequenceKeepsConsumingAfterModeReturnsStandalone",
        "SlowApi36GesturesNeverBecomeLongPresses",
        "ShortApi36ButtonDispatchesInsteadOfBeingDropped",
        "ExternalShortBackNeverUsesFullscreenKodiKeyPair",
        "StandaloneApi36ButtonEmitsOneExplicitLongpressCommand",
        "StandaloneApi36ButtonDispatchesOneShortOrLongKodiCommand",
        "StandaloneApi36ButtonCancelDispatchesNoKodiCommand",
        "StandaloneApi36ButtonRouteSurvivesExternalModeFlip",
        "PendingStandaloneApi36CommitKeepsItsOriginAcrossWarmTransition",
        "ColdExternalModeIsEstablishedBeforeFirstApi36Sequence",
        "Api36EdgeLeftWithRawHardwareEvidenceSupportsButtonLongPress",
        "TeardownSuppressesActiveAndPendingInput",
        "LifecycleTokensAreMonotonicAndStaleActivityCannotAffectNewOwner",
        "EarlyCommitSurvivesDuplicateWindowLossAndFlushesOnceAfterPublicationAndReadiness",
        "PublicationFailsIfInitialPendingEffectDestroysLifecycle",
        "PublicationExceptionRetiresFailedInitialSink",
        "DeferredWindowReadyExceptionRetiresFailedSinkAndRetriesExactlyOnce",
        "ConcurrentReadinessQueuedDuringFailureCannotReadmitFailedPublication",
        "NewerOverlappingReadinessSurvivesOlderEffectFailureAndFlushesReplacement",
        "LifecycleReplacementCancelsFailingDeferredPublicationRetry",
        "SameLifecycleCanReplaceUnpublishedSinkAndFlushPendingCommit",
        "PublicationRejectsConcurrentSinkAndAllowsExactReplacement",
        "JniSourceAdapterKeepsGesturesShortAndButtonLongPressExclusive",
        "SinkEffectsCanReenterWindowAndExternalModeWithoutDeadlock",
        "WindowLossReturnsWhileAdmittedEffectFinishesOutsideMutex",
        "WindowLossCancelsAdmittedCommandBeforeDestinationExecution",
        "UnpublishReturnsButBlocksReplacementUntilAdmittedEffectRetires",
        "DuplicateUnpublishIsIdempotentWhileLeaseBlocksRepublish",
        "LifecycleDestroyInvalidatesTokenThenWaitsForAdmittedEffect",
        "UnpublishHasBoundedDrainWhenDestinationRemainsBlocked",
        "LifecycleReplacementHasBoundedDrainAndActivatesNewOwner",
        "SynchronousUnpublishInsideEffectNeverWaitsForItself",
        "SynchronousLifecycleDestroyInsideEffectNeverWaitsForItself",
        "KodiThreadUnpublishInsideSynchronousCallbackNeverSelfWaits",
        "KodiThreadLifecycleDestroyInsideSynchronousCallbackNeverSelfWaits",
        "SelfUnpublishPinsOwnedSinkUntilCallbackReturns",
        "LifecycleStartInsideEffectDefersActivationAndPublication",
        "ReplacementPublicationIsOwnedWhileActivationWaitsForOldDestinationExecution",
        "ActivityRecreationWaitsForAdmittedOldEffectBeforeActivation",
        "StaleActivityProducerKeepsOriginalRawAndWindowToken",
        "SameAddressPlacementNewSinkGetsFreshPublicationIdentity",
        "ExactLifecycleRetirementRejectsStaleJniProducer",
        "ThreadMessagePayloadCancelsOrTransfersExactlyOnce",
        "StandaloneApi36HoldProducesKodiLongpressTranslationAcrossInvokeAndCancel",
        "DelayedKodiQueueDrainPreservesApi36StandaloneLongpressWithoutWallClockTiming",
    ):
        if test_contract not in coordinator_test:
            raise AssertionError(
                f"native Back sequence coverage is missing {test_contract}"
            )
    if re.search(
        r"^class\s+BackDispatchModel\b",
        Path(__file__).read_text(encoding="utf-8"),
        re.MULTILINE,
    ):
        raise AssertionError("Python source models are not behavioral Back proof")
    if "JumpgateBackCoordinator.cpp" not in UTILS_CMAKE.read_text(encoding="utf-8"):
        raise AssertionError("native Back coordinator is not in the utils target")
    if "TestJumpgateBackCoordinator.cpp" not in UTILS_TEST_CMAKE.read_text(
        encoding="utf-8"
    ):
        raise AssertionError("native Back gtest is not in the utils test target")
    if "CommitShortLocked" not in coordinator_source:
        raise AssertionError(
            "all short Back sources must share the coordinator commit path"
        )
    window_lost = extract_braced_block(
        coordinator_source, "CJumpgateBackCoordinator::OnWindowLost("
    )
    if "ResetStateLocked" in window_lost or "ClearLegacySequenceLocked" in window_lost:
        raise AssertionError(
            "transient window loss must preserve pending Back and raw route state"
        )
    legacy_up = extract_braced_block(
        coordinator_source, "CJumpgateBackCoordinator::OnLegacyRawUp("
    )
    require_in_order(
        legacy_up,
        "if (!matchingSequence)",
        "return Action::CONSUME;",
        "ClearLegacySequenceLocked();",
    )
    prepare_effect = extract_braced_block(
        coordinator_source, "CJumpgateBackDispatcher::PrepareActionLocked("
    )
    set_window_ready = extract_braced_block(
        coordinator_source, "bool CJumpgateBackDispatcher::SetWindowReady("
    )
    publish_sink = extract_braced_block(
        coordinator_source, "CJumpgateBackDispatcher::PublishSink("
    )
    execute_effect = extract_braced_block(
        coordinator_source, "CJumpgateBackDispatcher::ExecuteEffect("
    )
    if any(
        effect in prepare_effect
        for effect in (
            "DispatchExternalBack(",
            "DispatchKodiBack(",
            "OpenExternalSettings(",
        )
    ):
        raise AssertionError(
            "sink effects must not execute while the dispatcher mutex is held"
        )
    require_in_order(
        prepare_effect,
        "RefreshCommandFenceLocked();",
        "context.m_readinessGeneration = m_readinessGeneration;",
        "context.m_fence = m_commandFence;",
    )
    if (
        "RefreshCommandFenceLocked();" in publish_sink
        or "RefreshCommandFenceLocked();" in set_window_ready
    ):
        raise AssertionError("execution fences must be scoped to individual commands")
    require_in_order(
        publish_sink,
        "ExecuteEffect(lease);",
        "m_activePublication->publicationToken != publicationToken",
        "return INVALID_PUBLICATION_TOKEN;",
    )
    for contract in (
        "DispatchExternalBack",
        "DispatchKodiBack",
        "OpenExternalSettings",
        "lease.context.Fail(true)",
        "lease.context.IsPending()",
        "lease.context.Cancel()",
        "MarkDispatchReturned(lease.context)",
    ):
        if contract not in execute_effect:
            raise AssertionError(
                f"out-of-lock effect execution is missing {contract!r}"
            )
    finalized_effect = extract_braced_block(
        coordinator_source,
        "CJumpgateBackDispatcher::FinalizeCommandLocked(",
    )
    require_in_order(
        finalized_effect,
        "if (delivered)",
        "m_coordinator.OnActionDelivered(context.m_action);",
        "m_coordinator.OnActionFailed(context.m_action);",
        "if (retirePublication && !publication->retired)",
        "publication->retired = true;",
        "m_activePublication.reset();",
        "if (ownsCurrentLifecycle && m_readinessGeneration == context.m_readinessGeneration)",
        "++m_readinessGeneration;",
        "m_windowReady = false;",
        "m_coordinator.OnWindowLost();",
        "ReleaseEffectLocked(publication);",
    )
    if "wait_for(20ms)" in coordinator_test:
        raise AssertionError("native Back race tests must use deterministic barriers")
    for contract in (
        "MakeKodiBackKey",
        "CKey::MODIFIER_LONG",
        "KEY_HOLD_TRESHOLD + 1",
        "CKeyboardTranslator::TranslateButton",
        "CActionTranslator::TranslateString",
        "ACTION_STOP",
    ):
        if contract not in coordinator_test:
            raise AssertionError(
                f"API 36 standalone long-press integration proof is missing {contract!r}"
            )
    api36_long_press = extract_braced_block(
        coordinator_source,
        "CJumpgateBackCoordinator::CommitApi36ButtonLongPressLocked(",
    )
    require_in_order(
        api36_long_press,
        "m_source != SequenceSource::API36_BUTTON",
        "m_state = State::LONG_CONSUMED",
        "Api36ButtonRoute::EXTERNAL",
        "Action::OPEN_EXTERNAL_SETTINGS",
        "Action::DISPATCH_KODI_BACK_LONG",
    )
    if "Api36ButtonRoute::STANDALONE" not in coordinator_source:
        raise AssertionError("API 36 button route does not preserve standalone origin")
    native_ready = extract_braced_block(
        coordinator_source, "CJumpgateBackCoordinator::OnNativeReady("
    )
    if "ResetStateLocked()" in native_ready:
        raise AssertionError(
            "pending Back must not clear before sink delivery succeeds"
        )
    require_in_order(
        native_ready,
        "m_pendingCommitInFlight",
        "m_pendingCommitInFlight = true;",
        "return m_pendingCommitExternal ? Action::DISPATCH_EXTERNAL_BACK",
        "Action::DISPATCH_KODI_BACK_SHORT;",
    )
    for contract in (
        "m_inFlightEffects",
        "m_activePublication",
        "m_publications",
        "m_activationBarrierToken",
        "m_pendingLifecycleToken",
        "CommandContext",
        "FenceState",
        "m_readinessGeneration",
        "m_commandFence",
        "executingCommandThreads",
        "BeginCommandExecution",
    ):
        if contract not in coordinator_header:
            raise AssertionError(
                f"dispatcher effect lease fencing is missing {contract!r}"
            )
    if "ISink*" in coordinator_header or "m_retiredSinks" in coordinator_header:
        raise AssertionError(
            "sink retirement must be generation-owned, not pointer-owned"
        )
    for function in (
        "bool CJumpgateBackDispatcher::UnpublishSink(",
        "bool CJumpgateBackDispatcher::OnLifecycleDestroyed(",
    ):
        body = extract_braced_block(coordinator_source, function)
        if "wait_for" in body or ".wait(" in body:
            raise AssertionError("Back lifecycle teardown must not wait for destination work")
    require_in_order(
        extract_braced_block(coordinator_source, "bool CJumpgateBackDispatcher::UnpublishSink("),
        "publication->retired = true",
        "CancelCommandFenceLocked(publicationToken)",
        "CollectRetiredPublicationLocked(publication)",
    )
    require_in_order(
        extract_braced_block(
            coordinator_source, "bool CJumpgateBackDispatcher::OnLifecycleDestroyed("
        ),
        "m_activationBarrierToken = token",
        "RetireCurrentSinkLocked()",
        "m_currentLifecycleToken = INVALID_LIFECYCLE_TOKEN",
        "ActivatePendingLifecycleLocked()",
    )

    lifecycle_started = extract_braced_block(
        coordinator_source, "CJumpgateBackDispatcher::OnLifecycleStarted("
    )
    if "wait_for" in lifecycle_started or ".wait(" in lifecycle_started:
        raise AssertionError("Activity replacement must not wait for the old destination")
    require_in_order(
        lifecycle_started,
        "m_activationBarrierToken = m_currentLifecycleToken",
        "RetireCurrentSinkLocked()",
        "m_pendingLifecycleToken = requestedToken",
        "ActivatePendingLifecycleLocked",
    )

    main_activity = MAIN_ACTIVITY.read_text(encoding="utf-8")
    require_in_order(
        main_activity,
        "native long _onBackCreated(boolean externalPlayerMode);",
        "mBackLifecycleToken = _onBackCreated(mExternalPlayerMode);",
    )
    require_in_order(
        JNI_MAIN_SOURCE.read_text(encoding="utf-8"),
        '{"_onBackCreated", "(Z)J"',
        "OnCreated(env, context, initialExternalMode == JNI_TRUE)",
    )
    if "OnUnexpectedApi36RawBack" in app_source:
        raise AssertionError(
            "API 36 native hardware evidence must be fused, not discarded"
        )
    for contract in (
        "OnApi36RawBack",
        "AKeyEvent_getAction(event)",
        "AKeyEvent_getRepeatCount(event)",
        "heldDuration",
    ):
        if contract not in app_source:
            raise AssertionError(
                f"API 36 hardware Back fusion is missing {contract!r}"
            )


def verify_back_lifecycle_rejection_contract():
    main_activity = MAIN_ACTIVITY.read_text(encoding="utf-8")
    result_coordinator = (
        ROOT
        / "tools"
        / "android"
        / "packaging"
        / "xbmc"
        / "src"
        / "ExternalPlayerResultCoordinator.java.in"
    ).read_text(encoding="utf-8")
    external_activity = (
        ROOT
        / "tools"
        / "android"
        / "packaging"
        / "xbmc"
        / "src"
        / "ExternalPlayerActivity.java.in"
    ).read_text(encoding="utf-8")
    app_source = APP_SOURCE.read_text(encoding="utf-8")
    coordinator_source = BACK_COORDINATOR_SOURCE.read_text(encoding="utf-8")
    messenger = (ROOT / "xbmc" / "messaging" / "ApplicationMessenger.cpp").read_text(
        encoding="utf-8"
    )
    messenger_header = (
        ROOT / "xbmc" / "messaging" / "ApplicationMessenger.h"
    ).read_text(encoding="utf-8")
    jni_source = JNI_MAIN_SOURCE.read_text(encoding="utf-8")

    for contract in (
        "mExternalResultProducer.prepare(",
        "recordEarlyExternalPlayerBack",
        "admitPrepared(",
    ):
        if contract not in main_activity and contract not in result_coordinator:
            raise AssertionError(f"request-owned early Back is missing {contract!r}")
    if main_activity.find("mExternalResultProducer.prepare(") > main_activity.find(
        "_onBackCreated(mExternalPlayerMode)"
    ):
        raise AssertionError("external request ownership must precede native Back admission")
    if "cancelForTeardown(" not in external_activity:
        raise AssertionError("normal Activity teardown lacks exact canceled-result delivery")
    activity_teardown = extract_braced_block(
        external_activity, "synchronized boolean cancelForTeardown("
    )
    require_in_order(
        activity_teardown,
        "canceledOwner()",
        "mHost.persist(canceledOwner.copy())",
        "mOwner = canceledOwner",
        "mHost.setCanceledResult()",
        "mHost.finishOwner()",
    )
    cancellation_copy = extract_braced_block(
        external_activity,
        "private ExternalPlayerResultCoordinator.Owner canceledOwner()",
    )
    require_in_order(
        cancellation_copy,
        "mOwner.copy()",
        "canceledOwner.expectedGeneration()",
        "canceledOwner.cancel(",
        "return canceled ? canceledOwner : null",
    )
    activity_delivery = extract_braced_block(
        external_activity, "synchronized boolean deliver("
    )
    require_in_order(
        activity_delivery,
        "mOwner.copy()",
        "completedOwner.claim(terminal)",
        "mHost.persist(completedOwner.copy())",
        "mOwner = completedOwner",
        "mHost.setTerminalResult(terminal)",
        "mHost.finishOwner()",
    )
    caller_cancellation = extract_braced_block(
        external_activity, "synchronized boolean cancelForCaller()"
    )
    require_in_order(
        caller_cancellation,
        "canceledOwner()",
        "mHost.persist(canceledOwner.copy())",
        "mOwner = canceledOwner",
        "mHost.setCanceledResult()",
        "mHost.finishOwner()",
        "finishCancellation(requestId, generation)",
    )
    process_recovery = extract_braced_block(
        external_activity, "synchronized boolean recoverAfterProcessDeath("
    )
    require_in_order(
        process_recovery,
        "replacementOwner",
        "mHost.persist(replacementOwner.copy())",
        "mHost.clearExact(staleRequestId, staleGeneration)",
        "mOwner = replacementOwner",
        "forwardOnce()",
    )

    api36_back = extract_braced_block(main_activity, "public void onBackInvoked()")
    if "recordEarlyExternalPlayerBack" in api36_back:
        raise AssertionError("API 36 Back records cancellation before native route resolution")

    main_teardown = extract_braced_block(
        main_activity, "private synchronized void terminalizeExternalPlayerResultForTeardown()"
    )
    require_in_order(
        main_teardown,
        "rejectExternalPlayerResult(",
        "mExternalResultProducer.reserveFinish(",
        "publishExternalPlayerTerminalOrClearCanceledOwner(terminal)",
        "reservation.rollback()",
        "reservation.commit()",
    )
    if "handler.post" in main_teardown or "mExternalResultProducer.finish(" in main_teardown:
        raise AssertionError("Main teardown can consume or defer its terminal before durability")

    supersession = extract_braced_block(
        main_activity, "public synchronized void supersedeExternalPlayerResult("
    )
    require_in_order(
        supersession,
        "mExternalResultProducer.reserveSupersede(",
        "publishExternalPlayerTerminalOrClearCanceledOwner(reservation.terminal())",
        "reservation.rollback()",
        "reservation.commit()",
    )

    prepared_rejection = extract_braced_block(
        main_activity, "public synchronized void rejectExternalPlayerResult("
    )
    require_in_order(
        prepared_rejection,
        "mExternalResultProducer.reservePreparedRejection(",
        "ExternalPlayerResultStore.bind(",
    )
    if "reservation.rollback()" not in prepared_rejection:
        raise AssertionError("prepared rejection cannot retry a failed durable operation")
    rejection_publish = prepared_rejection.find("ExternalPlayerResultStore.publish(")
    rejection_clear = prepared_rejection.find("ExternalPlayerResultStore.clear(")
    rejection_commit = prepared_rejection.find("reservation.commit()")
    if (
        rejection_publish < 0
        or rejection_clear < 0
        or rejection_commit < rejection_publish
        or rejection_commit < rejection_clear
    ):
        raise AssertionError("prepared rejection commits before exact durable publication")
    if "mExternalResultProducer.rejectPrepared(" in prepared_rejection:
        raise AssertionError("prepared rejection consumes ownership before durability")

    normal_exit = extract_braced_block(
        main_activity, "public synchronized void exitExternalPlayerMode("
    )
    require_in_order(
        normal_exit,
        "mExternalResultProducer.reserveFinish(",
        "ExternalPlayerResultStore.clear(this, requestId, generation)",
        "ExternalPlayerResultStore.acknowledgeCanceledExit(",
        "ExternalPlayerResultStore.publish(this, reservation.terminal())",
        "reservation.rollback()",
        "reservation.commit()",
        "mExternalPlayerMode = false",
    )
    verify_warm_task_handoff_contract(
        main_activity, external_activity, result_coordinator, app_source
    )

    for contract in (
        "class TerminalReservation",
        "reservePreparedRejection(",
        "reserveSupersede(",
        "reserveFinish(",
        "reservation.mOpen",
    ):
        if contract not in result_coordinator:
            raise AssertionError(f"durable result reservation is missing {contract!r}")

    for function in ("QueueBackCommand(", "QueueExternalPlayback("):
        body = extract_braced_block(app_source, f"bool CXBMCApp::{function}")
        if "payload.release()" in body or "PostMsg(TMSG_CALLBACK" in body:
            raise AssertionError(f"{function} retains raw callback ownership")
        if "PostCallback(" not in body:
            raise AssertionError(f"{function} lacks owned messenger hand-off")

    lifecycle_start = extract_braced_block(
        coordinator_source, "CJumpgateBackDispatcher::OnLifecycleStarted("
    )
    if "wait_for" in lifecycle_start or "AbandonActivationBarrierLocked" in coordinator_source:
        raise AssertionError("replacement may overlap a live old lifecycle effect")
    if "context.IsPending()" not in extract_braced_block(
        coordinator_source, "CJumpgateBackDispatcher::ExecuteEffect("
    ):
        raise AssertionError("dispatcher still treats enqueue acceptance as destination execution")

    for contract in (
        "TMSG_OWNED_CALLBACK",
        "PostCallback(",
        "CancelPendingOwnedCallbacks",
        "PostOwnedMsg(",
    ):
        if contract not in messenger and contract not in messenger_header:
            raise AssertionError(f"owned bounded messenger seam is missing {contract!r}")
    for contract in (
        "CJumpgateLifecycleTargetRegistry",
        "AcquireAppInstance(",
        "RetireAppInstance(",
    ):
        if contract not in jni_source:
            raise AssertionError(f"JNI target ownership is missing {contract!r}")
    if "AcquireAppInstance()" in jni_source or "AcquireAppInstance(token)" not in jni_source:
        raise AssertionError("JNI target acquisition is not exact-lifecycle qualified")

    queued_back = extract_braced_block(app_source, "bool CXBMCApp::ExecuteQueuedBackCommand(")
    require_in_order(
        queued_back,
        "videoOsd->IsDialogRunning()",
        "CancelPendingExternalPlaybackFromBack",
        "ACTION_STOP",
    )
    if "m_playbackResultState.BeginLifecycleOperation()" in extract_braced_block(
        app_source, "bool CXBMCApp::CancelPendingExternalPlaybackFromBack("
    ):
        raise AssertionError("Back cancellation can block on the playback lifecycle mutex")

    new_intent = extract_braced_block(app_source, "void CXBMCApp::onNewIntent(")
    continuation = extract_braced_block(
        app_source, "CXBMCApp::BeginExternalPlaybackContinuation()"
    )
    result_delivery = extract_braced_block(
        app_source, "void CXBMCApp::ExecuteQueuedExternalPlayerResult("
    )
    for name, body in (
        ("onNewIntent", new_intent),
        ("BeginExternalPlaybackContinuation", continuation),
        ("ExecuteQueuedExternalPlayerResult", result_delivery),
    ):
        if "m_playbackResultState.BeginLifecycleOperation()" in body:
            raise AssertionError(f"{name} can synchronously wait on lifecycle ownership")
    if "sleep_for" in result_delivery or "while (" in result_delivery:
        raise AssertionError("external result delivery polls instead of failing over asynchronously")
    if "PostAsyncCallback(" not in extract_braced_block(
        app_source, "bool CXBMCApp::QueueExternalPlayerResult("
    ):
        raise AssertionError("external result terminal lacks bounded asynchronous delivery")
    require_in_order(
        main_activity,
        "postNativeExternalIntentRetry",
        "postDeferredExternalPlayerRejection",
    )


def main(arguments):
    if arguments:
        if len(arguments) != 2 or arguments[0] != "--verify-gtest-inventory":
            raise SystemExit(
                "usage: test-android-branding.py [--verify-gtest-inventory PATH]"
            )
        built_test_count = verify_built_gtest_inventory(arguments[1])
        print(
            "Jumpgate built gtest inventory: "
            f"{built_test_count} exact source-declared tests reconciled"
        )
        return

    verify_release_policy_spdx()
    verify_version_code_boundaries()
    verify_estuary_osd_defaults()
    verify_identity()
    verify_package_derivation()
    verify_libandroidjni_intent_patch_contract()
    verify_cmake_parser_regressions()
    verify_compile_info_package_command_regressions()
    verify_compile_info_package_override()
    verify_cmake_inventory_regressions()
    verify_gtest_inventory_regressions()
    verify_host_policy_regressions()
    host_test_count = verify_jumpgate_host_test_policy()
    verify_uri_logging_privacy()
    verify_credential_context_wiring()
    verify_back_lifecycle_rejection_contract()
    verify_native_back_wiring()
    print(
        f"Jumpgate host-test policy: {host_test_count} source-declared tests selected"
    )
    print("Jumpgate Android branding contract: passed")


if __name__ == "__main__":
    main(sys.argv[1:])

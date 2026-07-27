#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile


SCRIPT_DIR = pathlib.Path(__file__).parent
VERIFIER = SCRIPT_DIR / "verify-android-python-runtime.py"
PACKAGING_MAKEFILE = SCRIPT_DIR.parents[1] / "android" / "packaging" / "Makefile.in"
ASSET_ROOT = (
    "assets/python3.11/lib/python3.11/site-packages/Cryptodome"
)
SECRET_SENTINEL = "PROVIDER_CONTENT_MUST_NOT_BE_PRINTED"
PLATFORM_BY_ABI = {
    "arm64-v8a": "aarch64-linux-android-24-release",
    "armeabi-v7a": "arm-linux-androideabi-24-release",
}


def load_verifier_module():
    specification = importlib.util.spec_from_file_location(
        "verify_android_python_runtime", VERIFIER
    )
    if specification is None or specification.loader is None:
        raise RuntimeError("unable to load runtime verifier")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


VERIFIER_MODULE = load_verifier_module()


def elf_fixture(abi: str) -> bytes:
    elf_class, machine = {
        "arm64-v8a": (2, 183),
        "armeabi-v7a": (1, 40),
    }[abi]
    header = bytearray(64)
    header[:4] = b"\x7fELF"
    header[4] = elf_class
    header[5] = 1
    header[6] = 1
    header[16:18] = (3).to_bytes(2, "little")
    header[18:20] = machine.to_bytes(2, "little")
    return bytes(header)


class RuntimeFixture:
    def __init__(self, root: pathlib.Path, abi: str = "arm64-v8a") -> None:
        self.root = root
        self.abi = abi
        self.prefix = root / "depends" / PLATFORM_BY_ABI[abi]
        self.cryptodome = (
            self.prefix / "lib" / "python3.11" / "site-packages" / "Cryptodome"
        )
        self.apk = root / "fixture.apk"
        self.runtime_relatives = {
            "__init__.py",
            "Cipher/AES.py",
            "Util/__pycache__/strxor.cpython-311.pyc",
        }
        self.native_relatives = {
            "Cipher/_raw_ecb.abi3.so",
            "Hash/_SHA256.so",
        }
        self.expected_assets = {
            f"{ASSET_ROOT}/{relative}" for relative in self.runtime_relatives
        }
        self.expected_native = {
            "libCryptodome_Cipher__raw_ecb.so",
            "libCryptodome_Hash__SHA256.so",
        }
        self._write_source()
        self.write_apk()

    def _write_source(self) -> None:
        for relative in self.runtime_relatives:
            path = self.cryptodome / pathlib.PurePosixPath(relative)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(
                f"{SECRET_SENTINEL} = 'fixture placeholder'\n", encoding="ascii"
            )
        for relative in self.native_relatives:
            path = self.cryptodome / pathlib.PurePosixPath(relative)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(elf_fixture(self.abi))

        selftest = self.cryptodome / "SelfTest"
        (selftest / "__pycache__").mkdir(parents=True)
        (selftest / "test_runtime.py").write_text("source self-test\n", encoding="ascii")
        (selftest / "__pycache__" / "test_runtime.cpython-311.pyc").write_bytes(
            b"source self-test bytecode"
        )

    def write_apk(
        self,
        assets: set[str] | None = None,
        native: set[str] | None = None,
        extra_entries: dict[str, bytes] | None = None,
    ) -> None:
        selected_assets = self.expected_assets if assets is None else assets
        selected_native = self.expected_native if native is None else native
        entries = {
            **{path: b"runtime fixture" for path in selected_assets},
            **{
                f"lib/{self.abi}/{name}": b"native fixture"
                for name in selected_native
            },
        }
        if extra_entries:
            entries.update(extra_entries)
        with zipfile.ZipFile(self.apk, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, value in sorted(entries.items()):
                archive.writestr(name, value)


class VerifyAndroidPythonRuntimeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.fixture = RuntimeFixture(pathlib.Path(self.temporary.name))

    def run_verifier(
        self,
        abi: str | None = None,
        fixture: RuntimeFixture | None = None,
    ) -> subprocess.CompletedProcess[str]:
        selected_fixture = self.fixture if fixture is None else fixture
        selected_abi = selected_fixture.abi if abi is None else abi
        return subprocess.run(
            [
                sys.executable,
                os.fspath(VERIFIER),
                os.fspath(selected_fixture.apk),
                selected_abi,
                os.fspath(selected_fixture.prefix),
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    def assert_success(self, fixture: RuntimeFixture | None = None) -> None:
        result = self.run_verifier(fixture=fixture)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertEqual(
            result.stdout,
            "Android Python runtime verified: assets=3 native=2\n",
        )

    def assert_failure(
        self,
        abi: str | None = None,
        fixture: RuntimeFixture | None = None,
        expected_reason: str | None = None,
        forbidden_entry: str | None = None,
    ) -> None:
        selected_fixture = self.fixture if fixture is None else fixture
        result = self.run_verifier(abi, selected_fixture)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        combined = result.stdout + result.stderr
        self.assertNotIn(os.fspath(selected_fixture.prefix), combined)
        self.assertNotIn(os.fspath(selected_fixture.apk), combined)
        self.assertNotIn(SECRET_SENTINEL, combined)
        self.assertIn("Android Python runtime verification failed:", result.stderr)
        if expected_reason is not None:
            self.assertIn(expected_reason, result.stderr)
        if forbidden_entry is not None:
            self.assertNotIn(forbidden_entry, combined)
            self.assertNotIn(pathlib.PurePosixPath(forbidden_entry).name, combined)

    def test_success_preserves_non_selftest_runtime(self) -> None:
        self.assert_success()

    def test_missing_runtime_python_file(self) -> None:
        assets = set(self.fixture.expected_assets)
        assets.remove(f"{ASSET_ROOT}/Cipher/AES.py")
        self.fixture.write_apk(assets=assets)
        self.assert_failure()

    def test_extra_runtime_file(self) -> None:
        self.fixture.write_apk(
            extra_entries={f"{ASSET_ROOT}/Cipher/Unexpected.py": b"extra"}
        )
        self.assert_failure()

    def test_missing_native_library(self) -> None:
        native = set(self.fixture.expected_native)
        native.remove("libCryptodome_Hash__SHA256.so")
        self.fixture.write_apk(native=native)
        self.assert_failure()

    def test_extra_native_library(self) -> None:
        native = set(self.fixture.expected_native)
        native.add("libCryptodome_Unexpected.so")
        self.fixture.write_apk(native=native)
        self.assert_failure()

    def test_unprefixed_native_library(self) -> None:
        self.fixture.write_apk(
            extra_entries={
                "lib/arm64-v8a/Cryptodome_Cipher__raw_ecb.so": b"native"
            }
        )
        self.assert_failure()

    def test_source_selftest_native_is_not_expected(self) -> None:
        source = self.fixture.cryptodome / "SelfTest" / "native.so"
        source.write_bytes(elf_fixture(self.fixture.abi))
        self.assert_success()

    def test_flattened_selftest_native_in_apk(self) -> None:
        entry = "lib/arm64-v8a/LiBCryptodome_SeLfTeSt_Cipher_native.so"
        self.fixture.write_apk(extra_entries={entry: b"self-test native"})
        self.assert_failure(
            expected_reason="APK contains flattened Cryptodome SelfTest native",
            forbidden_entry=entry,
        )

    def test_native_transform_collision(self) -> None:
        for relative in ("A_B/C.so", "A/B_C.so"):
            path = self.fixture.cryptodome / pathlib.PurePosixPath(relative)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(elf_fixture("arm64-v8a"))
        self.assert_failure()

    def test_selftest_python_file_in_apk(self) -> None:
        self.fixture.write_apk(
            extra_entries={
                f"{ASSET_ROOT}/SelfTest/Cipher/test_AES.py": b"self-test"
            }
        )
        self.assert_failure()

    def test_selftest_bytecode_in_apk(self) -> None:
        self.fixture.write_apk(
            extra_entries={
                f"{ASSET_ROOT}/SelfTest/__pycache__/test.cpython-311.pyc": b"self-test"
            }
        )
        self.assert_failure()

    def test_cryptodome_shared_object_in_assets(self) -> None:
        self.fixture.write_apk(
            extra_entries={f"{ASSET_ROOT}/Cipher/_raw_ecb.abi3.so": b"native"}
        )
        self.assert_failure()

    def test_source_symbolic_link(self) -> None:
        link = self.fixture.cryptodome / "Cipher" / "linked.py"
        try:
            link.symlink_to(self.fixture.cryptodome / "__init__.py")
        except (NotImplementedError, OSError) as error:
            self.skipTest(f"symbolic links unavailable: {error.__class__.__name__}")
        self.assert_failure()

    @unittest.skipUnless(hasattr(os, "mkfifo"), "FIFOs are unavailable")
    def test_source_special_file(self) -> None:
        os.mkfifo(self.fixture.cryptodome / "special.pipe")
        self.assert_failure()

    def test_wrong_source_abi(self) -> None:
        self.assert_failure("armeabi-v7a")

    def test_armeabi_v7a_success(self) -> None:
        fixture = RuntimeFixture(
            pathlib.Path(self.temporary.name) / "armv7",
            abi="armeabi-v7a",
        )
        self.assert_success(fixture)

    def test_abi3_transform_matches_makefile(self) -> None:
        sha_source = self.fixture.cryptodome / "Hash" / "_SHA256.so"
        sha_source.unlink()
        self.fixture.native_relatives.remove("Hash/_SHA256.so")
        self.fixture.expected_native = {"libCryptodome_Cipher__raw_ecb.so"}
        self.fixture.write_apk(native=set(self.fixture.expected_native))
        result = self.run_verifier()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout,
            "Android Python runtime verified: assets=3 native=1\n",
        )

    def test_makefile_native_transform_contract(self) -> None:
        lines = PACKAGING_MAKEFILE.read_text(encoding="ascii").splitlines()
        loops = [line.strip() for line in lines if "for i in `find Cryptodome " in line]
        self.assertEqual(len(loops), 1)
        substitutions = re.findall(r"`([^\`]*)`", loops[0])
        self.assertEqual(
            substitutions,
            [
                (
                    r"find Cryptodome -path Cryptodome/SelfTest -prune -o "
                    r"-name \*.so -print"
                ),
                (
                    'echo $$i | cut -c1- | tr "/" "_" | '
                    r"sed -e 's/\.abi[0-9]\\././'"
                ),
            ],
        )
        self.assertTrue(
            loops[0].endswith(
                ";cp $$i $$DIR/xbmc/obj/local/$(CPU)/$$FN ; done"
            )
        )

        prefix_lines = [
            line.strip()
            for line in lines
            if 'find . -name "*.so" -not -name "lib*.so"' in line
        ]
        self.assertEqual(
            prefix_lines,
            [
                (
                    r'cd xbmc/obj/local/$(CPU)/; find . -name "*.so" '
                    r'-not -name "lib*.so" | sed "s/\.\///" | '
                    r"xargs -I@ mv @ lib@"
                )
            ],
        )
        self.assertEqual(VERIFIER_MODULE.NATIVE_ABI_MARKER.pattern, r"\.abi[0-9]\.")
        for source, expected in {
            "Cipher/_raw_ecb.abi3.so": "libCryptodome_Cipher__raw_ecb.so",
            "Hash/_SHA256.so": "libCryptodome_Hash__SHA256.so",
        }.items():
            relative_parts = pathlib.PurePosixPath(source).parts
            self.assertEqual(
                VERIFIER_MODULE.transform_native_name(relative_parts),
                expected,
            )

    def test_multiple_source_trees(self) -> None:
        duplicate = (
            self.fixture.prefix
            / "lib"
            / "python3.12"
            / "site-packages"
            / "Cryptodome"
        )
        shutil.copytree(self.fixture.cryptodome, duplicate)
        self.assert_failure()

    def test_missing_source_tree(self) -> None:
        shutil.rmtree(self.fixture.cryptodome)
        self.assert_failure()


if __name__ == "__main__":
    unittest.main(verbosity=2)

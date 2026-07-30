#!/usr/bin/env python3

from __future__ import annotations

import os
import pathlib
import re
import stat
import sys
import zipfile


ABI_HEADERS = {
    "arm64-v8a": (2, 183),
    "armeabi-v7a": (1, 40),
}
PYTHON_DIRECTORY = re.compile(r"python(?P<version>[0-9]+\.[0-9]+)\Z")
NATIVE_ABI_MARKER = re.compile(r"\.abi[0-9]\.")


class VerificationError(Exception):
    pass


def reject(reason: str) -> None:
    raise VerificationError(reason)


def lstat_or_none(path: pathlib.Path) -> os.stat_result | None:
    try:
        return path.lstat()
    except FileNotFoundError:
        return None


def require_source_directory(path: pathlib.Path) -> os.stat_result:
    info = lstat_or_none(path)
    if info is None or stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        reject("source layout is invalid")
    return info


def find_cryptodome_source(target_prefix: pathlib.Path) -> tuple[str, pathlib.Path]:
    require_source_directory(target_prefix)
    library_root = target_prefix / "lib"
    require_source_directory(library_root)

    candidates: list[tuple[str, pathlib.Path]] = []
    with os.scandir(library_root) as iterator:
        python_entries = sorted(iterator, key=lambda entry: entry.name)
    for entry in python_entries:
        match = PYTHON_DIRECTORY.fullmatch(entry.name)
        if match is None:
            continue
        info = entry.stat(follow_symlinks=False)
        if entry.is_symlink() or not stat.S_ISDIR(info.st_mode):
            reject("source layout is invalid")

        python_root = pathlib.Path(entry.path)
        site_packages = python_root / "site-packages"
        site_info = lstat_or_none(site_packages)
        if site_info is None:
            continue
        if stat.S_ISLNK(site_info.st_mode) or not stat.S_ISDIR(site_info.st_mode):
            reject("source layout is invalid")

        cryptodome = site_packages / "Cryptodome"
        cryptodome_info = lstat_or_none(cryptodome)
        if cryptodome_info is None:
            continue
        if stat.S_ISLNK(cryptodome_info.st_mode) or not stat.S_ISDIR(
            cryptodome_info.st_mode
        ):
            reject("source layout is invalid")
        candidates.append((match.group("version"), cryptodome))

    if len(candidates) != 1:
        reject("source tree count mismatch")
    return candidates[0]


def validate_source_name(name: str) -> None:
    if (
        not name
        or name in {".", ".."}
        or "/" in name
        or "\\" in name
        or ":" in name
        or any(ord(character) < 32 or ord(character) == 127 for character in name)
    ):
        reject("source contains an unsafe entry")


def validate_native_abi(path: pathlib.Path, abi: str) -> None:
    expected_class, expected_machine = ABI_HEADERS[abi]
    with path.open("rb") as source:
        header = source.read(20)
    if (
        len(header) != 20
        or header[:4] != b"\x7fELF"
        or header[4] != expected_class
        or header[5] != 1
        or int.from_bytes(header[18:20], "little") != expected_machine
    ):
        reject("source native ABI mismatch")


def transform_native_name(relative_parts: tuple[str, ...]) -> str:
    source_name = pathlib.PurePosixPath("Cryptodome", *relative_parts).as_posix()
    output_name = source_name.replace("/", "_")
    output_name = NATIVE_ABI_MARKER.sub(".", output_name, count=1)
    if not output_name.startswith("lib"):
        output_name = "lib" + output_name
    if (
        not output_name.startswith("libCryptodome_")
        or not output_name.endswith(".so")
        or "/" in output_name
        or "\\" in output_name
    ):
        reject("source native name is invalid")
    return output_name


def collect_source_expectations(
    version: str, cryptodome: pathlib.Path, abi: str
) -> tuple[set[str], set[str]]:
    asset_root = pathlib.PurePosixPath(
        "assets",
        f"python{version}",
        "lib",
        f"python{version}",
        "site-packages",
        "Cryptodome",
    )
    expected_assets: set[str] = set()
    native_sources: dict[str, tuple[str, ...]] = {}
    root_info = require_source_directory(cryptodome)
    seen_directories = (
        {(root_info.st_dev, root_info.st_ino)} if root_info.st_ino else set()
    )
    pending: list[tuple[pathlib.Path, tuple[str, ...]]] = [(cryptodome, ())]

    while pending:
        directory, parent_parts = pending.pop()
        with os.scandir(directory) as iterator:
            entries = sorted(iterator, key=lambda entry: entry.name)
        for entry in entries:
            validate_source_name(entry.name)
            relative_parts = (*parent_parts, entry.name)
            if relative_parts[0] == "SelfTest":
                continue

            info = entry.stat(follow_symlinks=False)
            if entry.is_symlink() or stat.S_ISLNK(info.st_mode):
                reject("source contains a symbolic link")

            source_path = pathlib.Path(entry.path)
            if stat.S_ISDIR(info.st_mode):
                directory_key = (info.st_dev, info.st_ino)
                if info.st_ino and directory_key in seen_directories:
                    reject("source directory topology is invalid")
                if info.st_ino:
                    seen_directories.add(directory_key)
                pending.append((source_path, relative_parts))
                continue
            if not stat.S_ISREG(info.st_mode):
                reject("source contains a special file")

            if entry.name.endswith(".so"):
                validate_native_abi(source_path, abi)
                output_name = transform_native_name(relative_parts)
                if output_name in native_sources:
                    reject("native-name transform collision")
                native_sources[output_name] = relative_parts
                continue
            expected_assets.add(asset_root.joinpath(*relative_parts).as_posix())

    if not expected_assets or not native_sources:
        reject("source tree is incomplete")
    return expected_assets, set(native_sources)


def validate_archive_name(name: str, is_directory: bool) -> tuple[str, ...]:
    if (
        not name
        or name.startswith("/")
        or "\\" in name
        or any(ord(character) < 32 or ord(character) == 127 for character in name)
    ):
        reject("APK archive layout is invalid")
    trimmed = name[:-1] if is_directory else name
    parts = tuple(trimmed.split("/"))
    if (
        not trimmed
        or any(part in {"", ".", ".."} or ":" in part for part in parts)
        or re.match(r"^[A-Za-z]:", parts[0])
    ):
        reject("APK archive layout is invalid")
    return parts


def collect_archive_records(apk: pathlib.Path) -> list[tuple[tuple[str, ...], bool]]:
    apk_info = lstat_or_none(apk)
    if apk_info is None or stat.S_ISLNK(apk_info.st_mode) or not stat.S_ISREG(
        apk_info.st_mode
    ):
        reject("APK input is invalid")

    records: list[tuple[tuple[str, ...], bool]] = []
    normalized_entries: dict[str, bool] = {}
    with zipfile.ZipFile(apk) as archive:
        for info in archive.infolist():
            if info.flag_bits & 0x1:
                reject("APK archive layout is invalid")
            is_directory = info.is_dir()
            parts = validate_archive_name(info.filename, is_directory)

            file_type = 0
            if info.create_system == 3:
                file_type = stat.S_IFMT(info.external_attr >> 16)
            if is_directory:
                if file_type not in {0, stat.S_IFDIR}:
                    reject("APK archive layout is invalid")
            elif file_type not in {0, stat.S_IFREG}:
                reject("APK archive layout is invalid")

            normalized = "/".join(parts)
            if normalized in normalized_entries:
                reject("APK archive layout is invalid")
            normalized_entries[normalized] = is_directory
            records.append((parts, is_directory))

    if not records:
        reject("APK archive layout is invalid")
    regular_paths = {
        normalized
        for normalized, is_directory in normalized_entries.items()
        if not is_directory
    }
    for normalized in normalized_entries:
        parts = normalized.split("/")
        for index in range(1, len(parts)):
            if "/".join(parts[:index]) in regular_paths:
                reject("APK archive layout is invalid")
    return records


def cryptodome_package_index(folded_parts: tuple[str, ...]) -> int | None:
    for index in range(len(folded_parts) - 1):
        if (
            folded_parts[index] == "site-packages"
            and folded_parts[index + 1] == "cryptodome"
        ):
            return index + 1
    return None


def collect_apk_actuals(
    records: list[tuple[tuple[str, ...], bool]], abi: str
) -> tuple[set[str], set[str]]:
    actual_assets: set[str] = set()
    actual_native: set[str] = set()

    for parts, is_directory in records:
        folded = tuple(part.casefold() for part in parts)
        for index in range(len(folded) - 1):
            if folded[index] == "cryptodome" and folded[index + 1] == "selftest":
                reject("APK contains Cryptodome SelfTest")

        package_index = cryptodome_package_index(folded)
        if folded[0] == "assets" and package_index is not None:
            if not is_directory and parts[-1].casefold().endswith(".so"):
                reject("APK contains a Cryptodome shared object in Python assets")
            if not is_directory:
                actual_assets.add("/".join(parts))

        filename = parts[-1]
        folded_filename = folded[-1]
        if folded_filename.startswith(
            "libcryptodome_selftest_"
        ) and folded_filename.endswith(".so"):
            reject("APK contains flattened Cryptodome SelfTest native")

        is_cryptodome_native = folded_filename.startswith(
            ("libcryptodome_", "cryptodome_")
        ) and folded_filename.endswith(".so")
        if is_cryptodome_native:
            if (
                is_directory
                or len(parts) != 3
                or folded[0] != "lib"
                or parts[1] != abi
                or not filename.startswith("libCryptodome_")
            ):
                reject("APK native Cryptodome layout mismatch")
            actual_native.add(filename)

    return actual_assets, actual_native


def verify(apk: pathlib.Path, abi: str, target_prefix: pathlib.Path) -> tuple[int, int]:
    if abi not in ABI_HEADERS:
        reject("unsupported Android ABI")
    version, cryptodome = find_cryptodome_source(target_prefix)
    expected_assets, expected_native = collect_source_expectations(
        version, cryptodome, abi
    )
    records = collect_archive_records(apk)
    actual_assets, actual_native = collect_apk_actuals(records, abi)

    if actual_assets != expected_assets:
        reject("runtime asset set mismatch")
    if actual_native != expected_native:
        reject("native library set mismatch")
    return len(expected_assets), len(expected_native)


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(
            "usage: verify-android-python-runtime.py <apk> <abi> <target-prefix>",
            file=sys.stderr,
        )
        return 2
    try:
        asset_count, native_count = verify(
            pathlib.Path(argv[1]), argv[2], pathlib.Path(argv[3])
        )
    except VerificationError as error:
        print(f"Android Python runtime verification failed: {error}", file=sys.stderr)
        return 1
    except Exception:
        print(
            "Android Python runtime verification failed: invalid input state",
            file=sys.stderr,
        )
        return 1

    print(
        "Android Python runtime verified: "
        f"assets={asset_count} native={native_count}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

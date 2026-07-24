#!/usr/bin/env bash

set -euo pipefail

usage() {
  echo "usage: $0 <apk> <expected-abi> <expected-package> <expected-min-sdk> <expected-target-sdk> <expected-version-name> <expected-version-code> <expected-signer-sha256> <expected-core-library>" >&2
  exit 2
}

fail() {
  echo "$1" >&2
  exit 1
}

[[ "$#" -eq 9 ]] || usage

apk="$1"
expected_abi="$2"
expected_package="$3"
expected_min_sdk="$4"
expected_target_sdk="$5"
expected_version_name="$6"
expected_version_code="$7"
expected_signer_sha256="${8,,}"
expected_core_library="$9"

case "$expected_abi" in
  arm64-v8a)
    expected_elf_class=2
    expected_elf_machine=183
    expected_readelf_class='ELF64'
    expected_readelf_machine='AArch64'
    ;;
  armeabi-v7a)
    expected_elf_class=1
    expected_elf_machine=40
    expected_readelf_class='ELF32'
    expected_readelf_machine='ARM'
    ;;
  *)
    echo 'Unsupported expected ABI' >&2
    exit 2
    ;;
esac

if [[ ! "$expected_package" =~ ^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$ ]]; then
  echo 'Invalid expected Android package name' >&2
  exit 2
fi
if [[ ! "$expected_min_sdk" =~ ^[0-9]+$ || ! "$expected_target_sdk" =~ ^[0-9]+$ ]]; then
  echo 'Invalid expected Android SDK metadata' >&2
  exit 2
fi
if [[ -z "$expected_version_name" || ${#expected_version_name} -gt 128 ||
      ! "$expected_version_code" =~ ^[0-9]+$ ]]; then
  echo 'Invalid expected Android version metadata' >&2
  exit 2
fi
if [[ ! "$expected_signer_sha256" =~ ^[0-9a-f]{64}$ ]]; then
  echo 'Invalid expected APK signer digest' >&2
  exit 2
fi
if [[ ! "$expected_core_library" =~ ^lib[A-Za-z0-9._+-]+\.so$ ]]; then
  echo 'Invalid expected Android core library name' >&2
  exit 2
fi

: "${ANDROID_BUILD_TOOLS_ROOT:?ANDROID_BUILD_TOOLS_ROOT is required}"
aapt2="$ANDROID_BUILD_TOOLS_ROOT/aapt2"
apksigner="$ANDROID_BUILD_TOOLS_ROOT/apksigner"

find_readelf() {
  local candidate
  if [[ -n "${JUMPGATE_READELF_BIN:-}" ]]; then
    test -x "$JUMPGATE_READELF_BIN"
    printf '%s\n' "$JUMPGATE_READELF_BIN"
    return
  fi
  if [[ -n "${ANDROID_NDK_ROOT:-}" ]]; then
    for candidate in "$ANDROID_NDK_ROOT"/toolchains/llvm/prebuilt/*/bin/llvm-readelf; do
      if [[ -x "$candidate" ]]; then
        printf '%s\n' "$candidate"
        return
      fi
    done
  fi
  if command -v llvm-readelf >/dev/null; then
    command -v llvm-readelf
    return
  fi
  command -v readelf
}

readelf_bin="$(find_readelf)"
test -f "$apk"
test -s "$apk"
test -x "$aapt2"
test -x "$apksigner"
test -x "$readelf_bin"
command -v find >/dev/null
command -v od >/dev/null
command -v python3 >/dev/null
command -v sha256sum >/dev/null
command -v tr >/dev/null
command -v wc >/dev/null

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

entry_list="$work_dir/entries.txt"
badging_file="$work_dir/badging.txt"
badging_raw_file="$work_dir/badging.raw.txt"
signer_file="$work_dir/signer.txt"
signer_raw_file="$work_dir/signer.raw.txt"
extract_dir="$work_dir/extracted"
mkdir -p "$extract_dir"

if ! python3 - "$apk" "$entry_list" "$extract_dir" <<'PY'
import pathlib
import re
import shutil
import stat
import sys
import zipfile

apk = pathlib.Path(sys.argv[1])
entry_list = pathlib.Path(sys.argv[2])
extract_root = pathlib.Path(sys.argv[3])


def reject() -> None:
    raise ValueError('unsafe APK archive')


try:
    with zipfile.ZipFile(apk) as archive:
        records = []
        normalized_paths = {}
        for info in archive.infolist():
            name = info.filename
            if (
                not name
                or name.startswith('/')
                or '\\' in name
                or any(ord(character) < 32 or ord(character) == 127 for character in name)
                or info.flag_bits & 0x1
            ):
                reject()

            is_directory = info.is_dir()
            trimmed_name = name[:-1] if is_directory else name
            parts = trimmed_name.split('/')
            if (
                not trimmed_name
                or any(part in {'', '.', '..'} for part in parts)
                or any(':' in part for part in parts)
                or re.match(r'^[A-Za-z]:', parts[0])
            ):
                reject()

            file_type = 0
            if info.create_system == 3:
                file_type = stat.S_IFMT(info.external_attr >> 16)
            if file_type == stat.S_IFLNK:
                reject()
            if is_directory:
                if file_type not in {0, stat.S_IFDIR}:
                    reject()
            elif file_type not in {0, stat.S_IFREG}:
                reject()

            normalized = '/'.join(parts)
            if normalized in normalized_paths:
                reject()
            normalized_paths[normalized] = is_directory
            records.append((info, parts, is_directory))

        if not records:
            reject()

        regular_paths = {
            normalized
            for normalized, is_directory in normalized_paths.items()
            if not is_directory
        }
        for normalized in normalized_paths:
            parts = normalized.split('/')
            for index in range(1, len(parts)):
                if '/'.join(parts[:index]) in regular_paths:
                    reject()

        entry_list.write_bytes(
            ''.join(f'{info.filename}\n' for info, _, _ in records).encode('utf-8')
        )
        extract_root.mkdir(parents=True, exist_ok=True)
        records.sort(key=lambda record: (not record[2], len(record[1]), record[1]))
        for info, parts, is_directory in records:
            destination = extract_root.joinpath(*parts)
            if is_directory:
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, destination.open('xb') as target:
                shutil.copyfileobj(source, target, length=1024 * 1024)
except (OSError, RuntimeError, ValueError, zipfile.BadZipFile):
    raise SystemExit(1)
PY
then
  fail 'APK archive failed safe extraction checks'
fi

[[ -s "$entry_list" ]] || fail 'APK contains no entries'

while IFS= read -r entry; do
  if [[ "$entry" =~ [[:cntrl:]] ]]; then
    fail 'APK contains an unsafe entry name'
  fi
  case "$entry" in
    /* | .. | ../* | */.. | */../* | *\\* | *:*)
      fail 'APK contains an unsafe entry path'
      ;;
  esac

  lower_entry="${entry,,}"
  base_name="${lower_entry##*/}"
  case "$base_name" in
    *.jks | *.keystore | *.p12 | *.pfx | jumpgate_credentials*.json | \
      jumpgate_settings.json | \
      jumpgate_playback_history.json | jumpgate_resume.json)
      fail 'APK contains a forbidden credential or runtime-config artifact'
      ;;
  esac

  normalized_entry="${lower_entry%/}"
  case "/$normalized_entry/" in
    */site-packages/cryptodome/selftest/*)
      fail 'APK contains a forbidden Cryptodome SelfTest artifact'
      ;;
  esac
  case "$normalized_entry" in
    lib/"$expected_abi"/libcryptodome_selftest_*.so)
      fail 'APK contains a forbidden Cryptodome SelfTest artifact'
      ;;
  esac
done < "$entry_list"

if ! "$aapt2" dump badging "$apk" > "$badging_raw_file" 2>&1; then
  fail 'Failed to read APK package metadata'
fi
tr -d '\r' < "$badging_raw_file" > "$badging_file"
actual_package="$(sed -n "s/^package: name='\([^']*\)'.*/\1/p" "$badging_file" | head -n 1)"
version_code="$(sed -n "s/^package:.*versionCode='\([^']*\)'.*/\1/p" "$badging_file" | head -n 1)"
version_name="$(sed -n "s/^package:.*versionName='\([^']*\)'.*/\1/p" "$badging_file" | head -n 1)"
min_sdk="$(sed -n "s/^minSdkVersion:'\([^']*\)'.*/\1/p" "$badging_file" | head -n 1)"
target_sdk="$(sed -n "s/^targetSdkVersion:'\([^']*\)'.*/\1/p" "$badging_file" | head -n 1)"
[[ "$actual_package" == "$expected_package" ]] ||
  fail 'APK package identity does not match version.txt'
[[ "$version_code" == "$expected_version_code" ]] ||
  fail 'APK version code does not match version.txt'
[[ "$version_name" == "$expected_version_name" ]] ||
  fail 'APK version name does not match version.txt'
[[ "$min_sdk" == "$expected_min_sdk" ]] ||
  fail 'APK minimum SDK does not match the configured NDK API'
[[ "$target_sdk" == "$expected_target_sdk" ]] ||
  fail 'APK target SDK does not match the configured Android platform'
if grep -q '^application-debuggable' "$badging_file"; then
  fail 'Release APK is unexpectedly debuggable'
fi

if ! "$apksigner" verify --Werr --verbose --print-certs "$apk" > "$signer_raw_file" 2>&1; then
  fail 'APK signature verification failed'
fi
tr -d '\r' < "$signer_raw_file" > "$signer_file"
if ! grep -Fxq 'Verified using v2 scheme (APK Signature Scheme v2): true' "$signer_file"; then
  fail 'APK is not signed with APK Signature Scheme v2'
fi
mapfile -t signer_digests < <(
  sed -n 's/^Signer #[0-9][0-9]* certificate SHA-256 digest: //p' "$signer_file" |
    tr '[:upper:]' '[:lower:]' |
    tr -d ':'
)
if [[ "${#signer_digests[@]}" -ne 1 ||
      ! "${signer_digests[0]:-}" =~ ^[0-9a-f]{64}$ ||
      "${signer_digests[0]:-}" != "$expected_signer_sha256" ]]; then
  fail 'APK signer certificate does not match the expected signer'
fi

if find "$extract_dir" -type l -print -quit | grep -q .; then
  fail 'APK extraction produced an unexpected symbolic link'
fi

lib_root="$extract_dir/lib"
abi_root="$lib_root/$expected_abi"
[[ -d "$lib_root" && -d "$abi_root" ]] ||
  fail 'APK does not contain the expected native-library directory'
mapfile -d '' -t abi_directories < <(
  find "$lib_root" -mindepth 1 -maxdepth 1 -type d -print0 | LC_ALL=C sort -z
)
if [[ "${#abi_directories[@]}" -ne 1 ||
      "$(basename "${abi_directories[0]:-}")" != "$expected_abi" ]]; then
  fail 'APK contains an unexpected or spoofed ABI directory'
fi
if find "$lib_root" -mindepth 1 -maxdepth 1 ! -type d -print -quit | grep -q .; then
  fail 'APK native-library root contains an unexpected entry'
fi
if find "$abi_root" -mindepth 1 -type d -print -quit | grep -q .; then
  fail 'APK native libraries are not directly under the expected ABI directory'
fi
if find "$abi_root" -mindepth 1 -maxdepth 1 ! -type f -print -quit | grep -q .; then
  fail 'APK ABI directory contains a non-regular entry'
fi
if find "$abi_root" -mindepth 1 -maxdepth 1 -type f ! -name '*.so' -print -quit | grep -q .; then
  fail 'APK ABI directory contains a non-library file'
fi
if find "$extract_dir" -type f -name 'libshairplay.so*' -print -quit | grep -q .; then
  fail 'APK must not bundle Shairplay after static linkage'
fi

mapfile -d '' -t shared_libraries < <(
  find "$abi_root" -mindepth 1 -maxdepth 1 -type f -name '*.so' -print0 | LC_ALL=C sort -z
)
[[ "${#shared_libraries[@]}" -gt 0 ]] || fail 'APK contains no native shared libraries'
[[ -f "$abi_root/$expected_core_library" ]] ||
  fail 'APK does not contain the expected core native library'
mapfile -d '' -t all_shared_libraries < <(
  find "$extract_dir" -type f -name '*.so' -print0 | LC_ALL=C sort -z
)
if [[ "${#all_shared_libraries[@]}" -ne "${#shared_libraries[@]}" ]]; then
  fail 'APK contains a shared library outside the expected ABI directory'
fi

for shared_library in "${shared_libraries[@]}"; do
  file_size="$(wc -c < "$shared_library")"
  if [[ ! "$file_size" =~ ^[0-9]+$ || "$file_size" -lt 512 ]]; then
    fail 'APK contains a truncated or implausibly small native shared library'
  fi
  if ! readelf_output="$("$readelf_bin" -h -l -d -W "$shared_library" 2>/dev/null)"; then
    fail 'APK contains a malformed native shared library'
  fi
  if ! grep -Eq '^[[:space:]]*Type:[[:space:]]+DYN([[:space:]]|$)' <<< "$readelf_output"; then
    fail 'APK native library is not an ELF shared object (ET_DYN)'
  fi
  if grep -Eq '^[[:space:]]*INTERP[[:space:]]' <<< "$readelf_output"; then
    fail 'APK native library contains a program interpreter (PT_INTERP)'
  fi
  if grep -Eq '\(FLAGS_1\).*[[:space:]]PIE([[:space:]]|$)' <<< "$readelf_output"; then
    fail 'APK native library is a position-independent executable (DF_1_PIE)'
  fi
  if ! grep -Eq "^[[:space:]]*Class:[[:space:]]+${expected_readelf_class}([[:space:]]|$)" \
    <<< "$readelf_output"; then
    fail 'APK native-library ELF class does not match the matrix ABI'
  fi
  if ! grep -Eq "^[[:space:]]*Machine:[[:space:]]+${expected_readelf_machine}([[:space:](]|$)" \
    <<< "$readelf_output"; then
    fail 'APK native-library ELF machine does not match the matrix ABI'
  fi
  if ! grep -Eq '^[[:space:]]*LOAD[[:space:]]' <<< "$readelf_output"; then
    fail 'APK native shared library has no loadable program segment'
  fi

  elf_header="$(od -An -v -tx1 -N20 "$shared_library" | tr -d '[:space:]')"
  elf_header="${elf_header,,}"
  if [[ ! "$elf_header" =~ ^[0-9a-f]{40,}$ || "${elf_header:0:8}" != '7f454c46' ||
        "${elf_header:10:2}" != '01' ]]; then
    fail 'APK contains an invalid or non-little-endian ELF library'
  fi
  elf_class="$((16#${elf_header:8:2}))"
  type_low="$((16#${elf_header:32:2}))"
  type_high="$((16#${elf_header:34:2}))"
  elf_type="$((type_low + type_high * 256))"
  machine_low="$((16#${elf_header:36:2}))"
  machine_high="$((16#${elf_header:38:2}))"
  elf_machine="$((machine_low + machine_high * 256))"
  if [[ "$elf_type" -ne 3 ||
        "$elf_class" -ne "$expected_elf_class" ||
        "$elf_machine" -ne "$expected_elf_machine" ]]; then
    fail 'APK native-library ELF architecture does not match the matrix ABI'
  fi
done

if ! python3 - "$extract_dir" "$expected_abi" "$expected_core_library" <<'PY'
import base64
import bisect
import binascii
import codecs
import contextlib
import hashlib
import json
import mmap
import os
import pathlib
import re
import stat
import sys
import unicodedata
from array import array

root = pathlib.Path(sys.argv[1])
expected_abi = sys.argv[2]
expected_core_library = sys.argv[3]
allowed_path = pathlib.PurePosixPath('lib') / expected_abi / expected_core_library
allowed_der_sha256 = '8959c62b4351cbaa702942f4572d37335a7a3dfdcc6f0d2763a2afb486e3ac8f'
MAX_FILES = 100_000
MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_TOTAL_BYTES = 1024 * 1024 * 1024
MAX_DECODE_WORK_BYTES = 5 * MAX_TOTAL_BYTES
CHUNK_BYTES = 1024 * 1024
MAX_DER_BYTES = 128 * 1024
MAX_DER_ATTEMPTS = 500_000
MAX_OPENPGP_WORK = 500_000
MAX_OPENPGP_COPY_BYTES = MAX_FILE_BYTES
MAX_BASE64_CHARACTERS = 192 * 1024
MAX_BASE64_ATTEMPTS = 100_000
MAX_BASE64_DECODE_BYTES = 64 * 1024 * 1024
MAX_ARMOR_CHARACTERS = 256 * 1024
MAX_ASSIGNMENT_LINE = 64 * 1024
MAX_JSON_BYTES = 32 * 1024 * 1024
MAX_HINT_WINDOWS = 100_000
PEM_BOUNDARY = rb'-' * 5
CANONICAL_BLOCK = re.compile(
    rb'(?<!-)' + PEM_BOUNDARY + rb'BEGIN RSA PRIVATE KEY' + PEM_BOUNDARY +
    rb'(?P<body>[A-Za-z0-9+/=]{1,16384})'
    + PEM_BOUNDARY + rb'END RSA PRIVATE KEY' + PEM_BOUNDARY + rb'(?!-)'
)
DER_PRIVATE_CANDIDATE = re.compile(
    rb'(?:'
    rb'\x30[\x0b-\x7f]|'
    rb'\x30\x81[\x80-\xff]|'
    rb'\x30\x82[\x01-\xff][\x00-\xff]|'
    rb'\x30\x83[\x01-\xff][\x00-\xff]{2}|'
    rb'\x30\x84[\x01-\xff][\x00-\xff]{3}'
    rb')\x02\x01[\x00\x01]'
)
ARMOR_MARKER = re.compile(
    r'-+[ \t\r\n\x00]*'
    r'(b[ \t\r\n\x00]*e[ \t\r\n\x00]*g[ \t\r\n\x00]*i[ \t\r\n\x00]*n|'
    r'e[ \t\r\n\x00]*n[ \t\r\n\x00]*d)[ \t\r\n\x00]*'
    r'((?:[a-z0-9][ \t\r\n\x00]*){1,96})-+',
    re.IGNORECASE,
)
NAMED_SECRET = (
    r'(?:FLY_API_TOKEN|JUMPGATE_ENCRYPTION_KEY|'
    r'KODI_ANDROID_STORE_PASSWORD|KODI_ANDROID_KEY_PASSWORD)'
)
NAMED_ASSIGNMENT = re.compile(
    rf'(?<![A-Za-z0-9_])(?:export +)?'
    rf'(?:(?:"{NAMED_SECRET}")|(?:\'{NAMED_SECRET}\')|(?:{NAMED_SECRET}))'
    rf'(?![A-Za-z0-9_]) *(?:\+=|[:=]) *',
    re.IGNORECASE,
)
BASE64_CHARACTERS = frozenset(
    'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=_-'
)
PRIVATE_LABELS = {
    'privatekey', 'rsaprivatekey', 'ecprivatekey', 'dsaprivatekey',
    'encryptedprivatekey', 'opensshprivatekey', 'pgpprivatekeyblock',
}
OPENSSH_MAGIC = b'openssh-key-v1\0'
OPENPGP_SECRET_HEADERS = bytes((
    0xC5, 0xC7, 0x94, 0x95, 0x96, 0x97, 0x9C, 0x9D, 0x9E, 0x9F,
))
OPENPGP_SECRET_CANDIDATE = re.compile(
    rb'(?:'
    rb'[\xc5\xc7](?=(?:'
    rb'[\x00-\xbf\xe0-\xfe][\x03\x04\x06]|'
    rb'[\xc0-\xdf][\x00-\xff][\x03\x04\x06]|'
    rb'\xff[\x00-\xff]{4}[\x03\x04\x06]'
    rb'))|'
    rb'[\x94\x9c](?=[\x00-\xff][\x03\x04\x06])|'
    rb'[\x95\x9d](?=[\x00-\xff]{2}[\x03\x04\x06])|'
    rb'[\x96\x9e](?=[\x00-\xff]{4}[\x03\x04\x06])|'
    rb'[\x97\x9f](?=[\x03\x04\x06])'
    rb')'
)
MAX_OPENPGP_EC_POINT_BITS = 1059
OPENPGP_STANDARD_KDF_HASHES = frozenset({8, 9, 10, 12, 14})
OPENPGP_STANDARD_KDF_CIPHERS = frozenset({7, 8, 9})
# Private-use ciphers have no standardized block geometry to validate.
OPENPGP_CIPHER_BLOCK_BYTES = {
    1: 8,   # IDEA
    2: 8,   # TripleDES
    3: 8,   # CAST5
    4: 8,   # Blowfish
    7: 16,  # AES-128
    8: 16,  # AES-192
    9: 16,  # AES-256
    10: 16, # Twofish
    11: 16, # Camellia-128
    12: 16, # Camellia-192
    13: 16, # Camellia-256
}
OPENPGP_AEAD_NONCE_BYTES = {1: 16, 2: 15, 3: 12}
OPENPGP_REGISTERED_HASHES = frozenset({1, 2, 3, 8, 9, 10, 11, 12, 14})
OPENPGP_NATIVE_KEY_BYTES = {25: 32, 26: 56, 27: 32, 28: 57}
OPENPGP_PRIVATE_USE_IDS = frozenset(range(100, 111))
OPENPGP_S2K_BYTES = {0: 2, 1: 10, 3: 11, 4: 20}
OPENPGP_STREAM_TAGS = frozenset(
    (*range(1, 15), 17, 18, 19, 20, 21, *range(60, 64))
)
BASE64_BYTES = frozenset(b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=_-')
UNICODE_WHITESPACE_CODEPOINTS = (
    *range(0x09, 0x0E), 0x20, 0x85, 0xA0, 0x1680,
    *range(0x2000, 0x200B), 0x2028, 0x2029, 0x202F, 0x205F, 0x3000,
)
UTF8_WHITESPACE_BYTES = tuple(sorted(
    {chr(value).encode('utf-8') for value in UNICODE_WHITESPACE_CODEPOINTS},
    key=len,
    reverse=True,
))


scan_entry_sha256 = None
scan_phase = 'archive-structure'


def set_scan_context(record, phase: str) -> None:
    global scan_entry_sha256, scan_phase
    scan_entry_sha256 = None
    scan_phase = phase
    if record is not None:
        entry = record[1].as_posix().encode('utf-8')
        scan_entry_sha256 = hashlib.sha256(entry).hexdigest()


def fail() -> None:
    detail = {'phase': scan_phase}
    if scan_entry_sha256 is not None:
        detail['entry_sha256'] = scan_entry_sha256
    raise ValueError(
        'JUMPGATE_APK_SCAN_REJECT ' +
        json.dumps(detail, ensure_ascii=True, separators=(',', ':'), sort_keys=True)
    )


def collect_regular_files() -> list[tuple[pathlib.Path, pathlib.PurePosixPath, int]]:
    root_info = root.stat(follow_symlinks=False)
    if not stat.S_ISDIR(root_info.st_mode) or root.is_symlink():
        fail()
    files = []
    total_bytes = 0
    pending = [root]
    while pending:
        directory = pending.pop()
        with os.scandir(directory) as iterator:
            entries = sorted(iterator, key=lambda entry: entry.name)
        for entry in entries:
            if (
                not entry.name
                or entry.name in {'.', '..'}
                or '/' in entry.name
                or '\\' in entry.name
                or ':' in entry.name
                or any(ord(character) < 32 or ord(character) == 127 for character in entry.name)
                or entry.is_symlink()
            ):
                fail()
            info = entry.stat(follow_symlinks=False)
            path = pathlib.Path(entry.path)
            if stat.S_ISDIR(info.st_mode):
                pending.append(path)
                continue
            if not stat.S_ISREG(info.st_mode) or info.st_size < 0:
                fail()
            relative = pathlib.PurePosixPath(path.relative_to(root).as_posix())
            if relative.is_absolute() or any(part in {'', '.', '..'} for part in relative.parts):
                fail()
            if info.st_size > MAX_FILE_BYTES:
                fail()
            total_bytes += info.st_size
            files.append((path, relative, info.st_size))
            if len(files) > MAX_FILES or total_bytes > MAX_TOTAL_BYTES:
                fail()
    files.sort(key=lambda record: record[1].as_posix())
    if total_bytes * 5 > MAX_DECODE_WORK_BYTES:
        fail()
    return files


@contextlib.contextmanager
def mapped_file(record):
    path, _, expected_size = record
    flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0)
    descriptor = os.open(path, flags)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_size != expected_size:
            fail()
        if expected_size == 0:
            yield None
        else:
            with mmap.mmap(descriptor, length=0, access=mmap.ACCESS_READ) as data:
                yield data
    finally:
        os.close(descriptor)


class WorkBudget:
    def __init__(self):
        self.der_attempts = 0
        self.base64_attempts = 0
        self.base64_decoded_bytes = 0
        self.openpgp_operations = 0
        self.openpgp_copied_bytes = 0
        self.text_bytes = 0

    def der(self) -> None:
        self.der_attempts += 1
        if self.der_attempts > MAX_DER_ATTEMPTS:
            fail()

    def base64(self, decoded_bytes: int) -> None:
        self.base64_attempts += 1
        self.base64_decoded_bytes += decoded_bytes
        if (
            self.base64_attempts > MAX_BASE64_ATTEMPTS
            or self.base64_decoded_bytes > MAX_BASE64_DECODE_BYTES
        ):
            fail()

    def openpgp_operation(self) -> None:
        self.openpgp_operations += 1
        if self.openpgp_operations > MAX_OPENPGP_WORK:
            fail()

    def openpgp_copy(self, copied_bytes: int) -> None:
        if copied_bytes < 0:
            fail()
        self.openpgp_copied_bytes += copied_bytes
        if self.openpgp_copied_bytes > MAX_OPENPGP_COPY_BYTES:
            fail()

    def text(self, encoded_bytes: int) -> None:
        self.text_bytes += encoded_bytes
        if self.text_bytes > MAX_DECODE_WORK_BYTES:
            fail()


class OpenPgpSegmentedBody:
    def __init__(self, data, budget: WorkBudget):
        self._data = data
        self._budget = budget
        self._starts = array('I')
        self._lengths = array('I')
        self._offsets = array('I')
        self._length = 0

    def append_segment(self, start: int, length: int) -> None:
        if start < 0 or length < 0 or start + length > len(self._data):
            fail()
        if length == 0:
            return
        self._offsets.append(self._length)
        self._starts.append(start)
        self._lengths.append(length)
        self._length += length

    def __len__(self) -> int:
        return self._length

    def __getitem__(self, key):
        if isinstance(key, int):
            index = key + self._length if key < 0 else key
            if not 0 <= index < self._length:
                raise IndexError(index)
            segment_index = bisect.bisect_right(self._offsets, index) - 1
            logical_start = self._offsets[segment_index]
            source_start = self._starts[segment_index]
            return self._data[source_start + index - logical_start]
        if not isinstance(key, slice):
            raise TypeError('OpenPGP body indices must be integers or slices')

        start, stop, step = key.indices(self._length)
        positions = range(start, stop, step)
        output_length = len(positions)
        if output_length == 0:
            return b''
        self._budget.openpgp_copy(output_length)
        if step != 1:
            return bytearray(self[index] for index in positions)

        output = bytearray(output_length)
        logical = start
        written = 0
        source_view = memoryview(self._data)
        try:
            while logical < stop:
                segment_index = bisect.bisect_right(self._offsets, logical) - 1
                segment_logical = self._offsets[segment_index]
                source_start = self._starts[segment_index]
                segment_length = self._lengths[segment_index]
                within_segment = logical - segment_logical
                copied = min(segment_length - within_segment, stop - logical)
                output[written:written + copied] = source_view[
                    source_start + within_segment:source_start + within_segment + copied
                ]
                logical += copied
                written += copied
        finally:
            source_view.release()
        return output


def read_der_tlv(data, offset: int, limit: int):
    if offset < 0 or offset + 2 > limit:
        return None
    tag = data[offset]
    first_length = data[offset + 1]
    cursor = offset + 2
    if first_length < 0x80:
        length = first_length
    else:
        length_bytes = first_length & 0x7F
        if length_bytes == 0 or length_bytes > 4 or cursor + length_bytes > limit:
            return None
        encoded_length = bytes(data[cursor:cursor + length_bytes])
        if encoded_length[0] == 0:
            return None
        length = int.from_bytes(encoded_length, 'big')
        if length < 0x80:
            return None
        cursor += length_bytes
    end = cursor + length
    if length > MAX_DER_BYTES or end > limit:
        return None
    return tag, cursor, end


def der_children(data, start: int, end: int):
    children = []
    cursor = start
    while cursor < end and len(children) <= 32:
        child = read_der_tlv(data, cursor, end)
        if child is None:
            return None
        children.append(child)
        cursor = child[2]
    return children if cursor == end and len(children) <= 32 else None


def der_integer(data, item):
    if item[0] != 0x02 or item[2] <= item[1]:
        return None
    value = bytes(data[item[1]:item[2]])
    if value[0] & 0x80 or (len(value) > 1 and value[0] == 0 and not value[1] & 0x80):
        return None
    return value


def valid_der_oid(data, item) -> bool:
    if item[0] != 0x06 or item[2] <= item[1]:
        return False
    cursor = item[1]
    subidentifiers = 0
    first_group = True
    while cursor < item[2]:
        value = data[cursor]
        if first_group and value == 0x80:
            return False
        cursor += 1
        if value & 0x80:
            first_group = False
            continue
        subidentifiers += 1
        first_group = True
    return subidentifiers >= 1 and first_group


def valid_algorithm_identifier(data, item) -> bool:
    if item[0] != 0x30:
        return False
    children = der_children(data, item[1], item[2])
    return bool(
        children
        and len(children) in {1, 2}
        and valid_der_oid(data, children[0])
    )


def is_der_private_key(data, offset: int = 0, require_end: bool = False) -> bool:
    top = read_der_tlv(data, offset, len(data))
    if top is None or top[0] != 0x30 or top[2] - offset < 13:
        return False
    if require_end and top[2] != len(data):
        return False
    children = der_children(data, top[1], top[2])
    if not children:
        return False
    integers = [der_integer(data, child) for child in children]

    # PKCS#1 RSAPrivateKey.
    if (
        len(children) in {9, 10}
        and all(value is not None for value in integers[:9])
        and int.from_bytes(integers[0], 'big') in {0, 1}
        and len(integers[1]) >= 64
        and 1 <= len(integers[2]) <= 8
        and len(integers[3]) >= 32
        and len(integers[4]) >= 16
        and len(integers[5]) >= 16
    ):
        return True

    # Traditional OpenSSL DSA private key.
    if (
        len(children) == 6
        and all(value is not None for value in integers)
        and int.from_bytes(integers[0], 'big') == 0
        and len(integers[1]) >= 64
        and len(integers[2]) >= 16
        and len(integers[5]) >= 16
    ):
        return True

    # SEC1 ECPrivateKey.
    if (
        len(children) >= 2
        and integers[0] is not None
        and int.from_bytes(integers[0], 'big') == 1
        and children[1][0] == 0x04
        and 16 <= children[1][2] - children[1][1] <= 80
        and all(child[0] in {0xA0, 0xA1} for child in children[2:])
    ):
        return True

    # PKCS#8 PrivateKeyInfo / RFC 5958 OneAsymmetricKey. AlgorithmIdentifier
    # identifies an open set, so structural validity is the security boundary.
    if (
        len(children) >= 3
        and integers[0] is not None
        and int.from_bytes(integers[0], 'big') in {0, 1}
        and valid_algorithm_identifier(data, children[1])
        and children[2][0] == 0x04
        and 1 <= children[2][2] - children[2][1] <= MAX_DER_BYTES
        and all(child[0] in {0xA0, 0xA1} for child in children[3:])
    ):
        return True
    return False


def is_encrypted_private_key_info(data) -> bool:
    top = read_der_tlv(data, 0, len(data))
    if top is None or top[0] != 0x30 or top[2] != len(data):
        return False
    children = der_children(data, top[1], top[2])
    return bool(
        children
        and len(children) == 2
        and children[0][0] == 0x30
        and children[1][0] == 0x04
        and children[1][2] - children[1][1] >= 16
    )


def read_ssh_string(data, offset: int):
    if offset + 4 > len(data):
        return None
    length = int.from_bytes(data[offset:offset + 4], 'big')
    end = offset + 4 + length
    if length > MAX_DER_BYTES or end > len(data):
        return None
    return bytes(data[offset + 4:end]), end


def is_openssh_private_key(data) -> bool:
    if not bytes(data[:len(OPENSSH_MAGIC)]) == OPENSSH_MAGIC:
        return False
    offset = len(OPENSSH_MAGIC)
    values = []
    for _ in range(3):
        item = read_ssh_string(data, offset)
        if item is None:
            return False
        values.append(item[0])
        offset = item[1]
    if offset + 4 > len(data):
        return False
    key_count = int.from_bytes(data[offset:offset + 4], 'big')
    offset += 4
    if not 1 <= key_count <= 32 or len(values[0]) > 64 or len(values[1]) > 64:
        return False
    for _ in range(key_count + 1):
        item = read_ssh_string(data, offset)
        if item is None:
            return False
        offset = item[1]
    return True


def read_openpgp_mpi(data, cursor: int, end: int):
    if cursor + 2 > end:
        return None
    bits = int.from_bytes(data[cursor:cursor + 2], 'big')
    length = (bits + 7) // 8
    finish = cursor + 2 + length
    if bits == 0 or length > MAX_DER_BYTES or finish > end:
        return None
    first_octet = data[cursor + 2]
    if first_octet.bit_length() + (length - 1) * 8 != bits:
        return None
    return bits, finish


def valid_openpgp_oid(value) -> bool:
    if not value:
        return False
    cursor = 0
    while cursor < len(value):
        if value[cursor] == 0x80:
            return False
        while True:
            if cursor >= len(value):
                return False
            octet = value[cursor]
            cursor += 1
            if not octet & 0x80:
                break
    return True


def read_openpgp_new_length(data, cursor: int):
    if cursor >= len(data):
        return None
    first = data[cursor]
    cursor += 1
    if first < 192:
        return 'fixed', first, cursor
    if first < 224:
        if cursor >= len(data):
            return None
        return 'fixed', ((first - 192) << 8) + data[cursor] + 192, cursor + 1
    if first < 255:
        return 'partial', 1 << (first & 0x1F), cursor
    if cursor + 4 > len(data):
        return None
    return 'fixed', int.from_bytes(data[cursor:cursor + 4], 'big'), cursor + 4


def openpgp_semantic_candidate(data, offset: int):
    if offset >= len(data) or not data[offset] & 0x80:
        return None
    header = data[offset]
    cursor = offset + 1
    if header & 0x40:
        tag = header & 0x3F
        parsed_length = read_openpgp_new_length(data, cursor)
        if parsed_length is None:
            return None
        kind, length, body_start = parsed_length
        partial = kind == 'partial'
    else:
        tag = (header >> 2) & 0x0F
        length_type = header & 0x03
        partial = False
        if length_type == 3:
            body_start = cursor
            length = len(data) - body_start
        else:
            length_bytes = (1, 2, 4)[length_type]
            if cursor + length_bytes > len(data):
                return None
            length = int.from_bytes(data[cursor:cursor + length_bytes], 'big')
            body_start = cursor + length_bytes
    if (
        tag not in {5, 7}
        or body_start >= len(data)
        or data[body_start] not in {3, 4, 6}
    ):
        return None
    return tag, partial, body_start


def view_openpgp_body(data, cursor: int, length: int):
    source_view = memoryview(data)
    try:
        return source_view[cursor:cursor + length]
    finally:
        source_view.release()


def openpgp_packet_body(data, offset: int, budget: WorkBudget):
    if offset >= len(data) or not data[offset] & 0x80:
        return None
    header = data[offset]
    cursor = offset + 1
    if header & 0x40:
        tag = header & 0x3F
        parsed_length = read_openpgp_new_length(data, cursor)
        if parsed_length is None:
            return tag, b'', None, True, False
        kind, length, cursor = parsed_length
        if kind == 'fixed':
            end = cursor + length
            available_length = min(length, max(0, len(data) - cursor))
            viewed_length = min(available_length, MAX_DER_BYTES)
            available = view_openpgp_body(data, cursor, viewed_length)
            malformed = end > len(data)
            return (
                tag,
                available,
                end if end <= len(data) else None,
                malformed,
                available_length > viewed_length,
            )

        body = OpenPgpSegmentedBody(data, budget)
        # The byte cap bounds one packet. WorkBudget bounds aggregate parser
        # effort and body bytes materialized to join partial chunks.
        while kind == 'partial':
            budget.openpgp_operation()
            available_length = min(length, max(0, len(data) - cursor))
            copied_length = min(available_length, MAX_DER_BYTES - len(body))
            if copied_length > 0:
                body.append_segment(cursor, copied_length)
            if available_length > copied_length:
                return tag, body, None, True, True
            if available_length < length:
                return tag, body, None, True, False
            cursor += length
            parsed_length = read_openpgp_new_length(data, cursor)
            if parsed_length is None:
                return tag, body, None, True, False
            kind, length, cursor = parsed_length

        end = cursor + length
        available_length = min(length, max(0, len(data) - cursor))
        remaining_capacity = MAX_DER_BYTES - len(body)
        copied_length = min(available_length, remaining_capacity)
        if copied_length > 0:
            body.append_segment(cursor, copied_length)
        bytes_omitted = available_length > copied_length
        malformed = bytes_omitted or length > remaining_capacity
        malformed = malformed or end > len(data)
        return (
            tag,
            body,
            end if not malformed else None,
            malformed,
            bytes_omitted,
        )

    tag = (header >> 2) & 0x0F
    length_type = header & 0x03
    if length_type == 3:
        length = len(data) - cursor
        viewed_length = min(length, MAX_DER_BYTES)
        body = view_openpgp_body(data, cursor, viewed_length)
        return (
            tag,
            body,
            len(data),
            False,
            length > len(body),
        )
    length_bytes = (1, 2, 4)[length_type]
    if cursor + length_bytes > len(data):
        return tag, b'', None, True, False
    length = int.from_bytes(data[cursor:cursor + length_bytes], 'big')
    cursor += length_bytes
    end = cursor + length
    available_length = min(length, max(0, len(data) - cursor))
    viewed_length = min(available_length, MAX_DER_BYTES)
    body = view_openpgp_body(data, cursor, viewed_length)
    malformed = end > len(data)
    return (
        tag,
        body,
        end if end <= len(data) else None,
        malformed,
        available_length > viewed_length,
    )


def is_openpgp_stream_prefix(data, stop: int, budget: WorkBudget) -> bool:
    cursor = 0
    while cursor < stop:
        budget.openpgp_operation()
        packet = openpgp_packet_body(data, cursor, budget)
        if packet is None:
            return False
        tag, body, packet_end, malformed, _ = packet
        try:
            if (
                tag not in OPENPGP_STREAM_TAGS
                or packet_end is None
                or malformed
                or not cursor < packet_end <= stop
            ):
                return False
        finally:
            if isinstance(body, memoryview):
                body.release()
        cursor = packet_end
    return cursor == stop


def openpgp_public_layout(
    data, require_standard_layout: bool = False,
    recover_native_length: bool = False,
):
    end = len(data)
    if end < 6:
        return None
    version = data[0]
    if version == 3:
        if end < 8:
            return None
        algorithm = data[7]
        if algorithm not in {1, 2, 3}:
            return None
        cursor = 8
        material_end = end
    elif version == 4:
        algorithm = data[5]
        cursor = 6
        material_end = end
    elif version == 6:
        if end < 10:
            return None
        algorithm = data[5]
        material_length = int.from_bytes(data[6:10], 'big')
        recover_native = (
            recover_native_length and algorithm in OPENPGP_NATIVE_KEY_BYTES
        )
        if (
            (material_length == 0 or material_length > MAX_DER_BYTES)
            and not recover_native
        ):
            return None
        cursor = 10
        material_end = cursor + material_length
        if material_end > end and not recover_native:
            return None
    else:
        return None

    secret_shape = None
    secret_minimum = 0
    if algorithm in {1, 2, 3}:  # RSA
        for _ in range(2):
            item = read_openpgp_mpi(data, cursor, material_end)
            if item is None:
                return None
            cursor = item[1]
        secret_shape = ('mpi', 4)
        secret_minimum = 12
    elif algorithm == 17:  # DSA
        for _ in range(4):
            item = read_openpgp_mpi(data, cursor, material_end)
            if item is None:
                return None
            cursor = item[1]
        secret_shape = ('mpi', 1)
        secret_minimum = 3
    elif algorithm in {16, 20}:  # ElGamal, including historical Encrypt-or-Sign
        for _ in range(3):
            item = read_openpgp_mpi(data, cursor, material_end)
            if item is None:
                return None
            cursor = item[1]
        secret_shape = ('mpi', 1)
        secret_minimum = 3
    elif algorithm in {18, 19, 22}:  # ECDH/ECDSA/legacy EdDSA
        if cursor >= material_end or data[cursor] == 0:
            return None
        oid_length = data[cursor]
        if require_standard_layout and not 5 <= oid_length <= 32:
            return None
        if cursor + 1 + oid_length > material_end:
            return None
        if not valid_openpgp_oid(data[cursor + 1:cursor + 1 + oid_length]):
            return None
        cursor += 1 + oid_length
        point_start = cursor
        point = read_openpgp_mpi(data, cursor, material_end)
        if (
            point is None
            or data[point_start + 2] not in {0x04, 0x40}
            or (
                require_standard_layout
                and not 200 <= point[0] <= MAX_OPENPGP_EC_POINT_BITS
            )
        ):
            return None
        cursor = point[1]
        if algorithm == 18:
            if cursor >= material_end:
                return None
            kdf_length = data[cursor]
            if cursor + 1 + kdf_length > material_end:
                return None
            if require_standard_layout and (
                kdf_length != 3
                or data[cursor + 1] != 1
                or data[cursor + 2] not in OPENPGP_STANDARD_KDF_HASHES
                or data[cursor + 3] not in OPENPGP_STANDARD_KDF_CIPHERS
            ):
                return None
            cursor += 1 + kdf_length
        secret_shape = ('mpi', 1)
        secret_minimum = 3
    elif version in {4, 6} and algorithm in OPENPGP_NATIVE_KEY_BYTES:
        native_octets = OPENPGP_NATIVE_KEY_BYTES[algorithm]
        native_end = cursor + native_octets
        if (
            native_end > end
            or (
                version == 6
                and not recover_native_length
                and material_end - cursor != native_octets
            )
        ):
            return None
        cursor = native_end
        secret_shape = ('octets', native_octets)
        secret_minimum = native_octets
    else:
        return None
    if (
        version == 6
        and not (
            recover_native_length and algorithm in OPENPGP_NATIVE_KEY_BYTES
        )
        and cursor != material_end
    ):
        return None
    return version, cursor, secret_shape, secret_minimum


def read_openpgp_secret_material(data, cursor: int, end: int, secret_shape):
    shape, count = secret_shape
    if shape == 'mpi':
        for _ in range(count):
            item = read_openpgp_mpi(data, cursor, end)
            if item is None:
                return None
            cursor = item[1]
        return cursor
    if shape == 'octets':
        secret_end = cursor + count
        return secret_end if secret_end <= end else None
    return None


def valid_openpgp_s2k(specifier, protection: int) -> bool:
    if not specifier:
        return False
    s2k_type = specifier[0]
    expected_length = OPENPGP_S2K_BYTES.get(s2k_type)
    if expected_length is None or len(specifier) != expected_length:
        return False
    if s2k_type != 4:
        return specifier[1] in OPENPGP_REGISTERED_HASHES
    passes, parallelism, encoded_memory = specifier[17:20]
    minimum_memory = 3 + (parallelism - 1).bit_length()
    return (
        protection == 253
        and passes > 0
        and parallelism > 0
        and minimum_memory <= encoded_memory <= 31
    )


def valid_openpgp_encrypted_secret_length(
    secret_shape, encrypted_length: int, minimum_length: int
) -> bool:
    return encrypted_length >= minimum_length


def valid_openpgp_v4_aead_protection(
    data, cursor: int, end: int, secret_shape, secret_minimum: int
) -> bool:
    if cursor + 2 > end:
        return False
    cipher = data[cursor]
    aead = data[cursor + 1]
    cursor += 2
    block_bytes = OPENPGP_CIPHER_BLOCK_BYTES.get(cipher)
    if block_bytes != 16 and cipher not in OPENPGP_PRIVATE_USE_IDS:
        return False
    vector_bytes = OPENPGP_AEAD_NONCE_BYTES.get(aead)
    if vector_bytes is None or cursor >= end:
        return False

    s2k_bytes = OPENPGP_S2K_BYTES.get(data[cursor])
    if s2k_bytes is None:
        return False
    s2k_end = cursor + s2k_bytes
    if s2k_end > end or not valid_openpgp_s2k(data[cursor:s2k_end], 253):
        return False
    payload_start = s2k_end + vector_bytes
    return payload_start <= end and valid_openpgp_encrypted_secret_length(
        secret_shape, end - payload_start, secret_minimum + 16
    )


def valid_openpgp_v4_cfb_protection(
    data, cursor: int, end: int, protection: int,
    secret_shape, secret_minimum: int,
) -> bool:
    private_cipher = False
    if protection in {254, 255}:
        if cursor >= end:
            return False
        cipher = data[cursor]
        cursor += 1
        block_bytes = OPENPGP_CIPHER_BLOCK_BYTES.get(cipher)
        private_cipher = cipher in OPENPGP_PRIVATE_USE_IDS
        if (block_bytes is None and not private_cipher) or cursor >= end:
            return False
        s2k_bytes = OPENPGP_S2K_BYTES.get(data[cursor])
        if s2k_bytes is None:
            return False
        s2k_end = cursor + s2k_bytes
        if s2k_end > end or not valid_openpgp_s2k(
            data[cursor:s2k_end], protection
        ):
            return False
        cursor = s2k_end
        trailer_bytes = 20 if protection == 254 else 2
    else:
        block_bytes = OPENPGP_CIPHER_BLOCK_BYTES.get(protection)
        private_cipher = protection in OPENPGP_PRIVATE_USE_IDS
        if block_bytes is None and not private_cipher:
            return False
        trailer_bytes = 2

    if private_cipher:
        # The private-use cipher defines its own IV geometry. A complete packet,
        # parsed public section, and at least one opaque IV octet are sufficient.
        return end - cursor >= 1 + secret_minimum + trailer_bytes
    payload_start = cursor + block_bytes
    return payload_start <= end and valid_openpgp_encrypted_secret_length(
        secret_shape, end - payload_start, secret_minimum + trailer_bytes
    )


def valid_openpgp_v6_protection(
    data, cursor: int, end: int, protection: int,
    secret_shape, secret_minimum: int,
) -> bool:
    if cursor >= end:
        return False
    parameter_length = data[cursor]
    cursor += 1
    parameter_end = cursor + parameter_length
    if parameter_length == 0 or parameter_end > end or cursor >= parameter_end:
        return False

    cipher = data[cursor]
    cursor += 1
    block_bytes = OPENPGP_CIPHER_BLOCK_BYTES.get(cipher)
    private_cipher = cipher in OPENPGP_PRIVATE_USE_IDS
    if block_bytes is None and not private_cipher:
        return False

    if protection == 253:
        if cursor >= parameter_end:
            return False
        aead = data[cursor]
        cursor += 1
        vector_bytes = OPENPGP_AEAD_NONCE_BYTES.get(aead)
        trailer_bytes = 16
        if vector_bytes is None or (not private_cipher and block_bytes != 16):
            return False
    else:
        vector_bytes = block_bytes
        trailer_bytes = 20

    if cursor >= parameter_end:
        return False
    s2k_length = data[cursor]
    cursor += 1
    s2k_end = cursor + s2k_length
    if s2k_end > parameter_end or not valid_openpgp_s2k(
        data[cursor:s2k_end], protection
    ):
        return False
    cursor = s2k_end
    if private_cipher and protection == 254:
        # V6 length-delimits the private cipher's otherwise unknown IV.
        vector_bytes = parameter_end - cursor
        if vector_bytes <= 0:
            return False
    if parameter_end - cursor != vector_bytes:
        return False
    cursor += vector_bytes
    if cursor != parameter_end:
        return False
    return valid_openpgp_encrypted_secret_length(
        secret_shape, end - parameter_end, secret_minimum + trailer_bytes
    )


def recoverable_openpgp_secret_fields(data) -> bool:
    layout = openpgp_public_layout(data, recover_native_length=True)
    if layout is None:
        return False
    version, cursor, secret_shape, _ = layout
    end = len(data)
    if cursor >= end or data[cursor] != 0:
        return False
    cursor += 1
    secret_start = cursor
    secret_end = read_openpgp_secret_material(data, cursor, end, secret_shape)
    if secret_end is None:
        return False
    if version in {3, 4} and secret_shape[0] == 'octets':
        if secret_end + 2 > end:
            return False
        secret = data[secret_start:secret_end]
        checksum = int.from_bytes(data[secret_end:secret_end + 2], 'big')
        return sum(secret) & 0xFFFF == checksum
    if version == 6 and secret_shape[0] == 'octets':
        return secret_end == end
    return True


def is_openpgp_private_use_secret_body(
    data, complete_body_length=None
) -> bool:
    if len(data) < 7 or data[0] not in {4, 6}:
        return False
    if data[5] not in OPENPGP_PRIVATE_USE_IDS:
        return False
    if data[0] == 4:
        return True
    if len(data) < 11:
        return False
    public_length = int.from_bytes(data[6:10], 'big')
    body_length = (
        len(data) if complete_body_length is None else complete_body_length
    )
    return 10 + public_length < body_length


def is_openpgp_secret_body(data) -> bool:
    layout = openpgp_public_layout(data, recover_native_length=True)
    if layout is None:
        return False
    version, cursor, secret_shape, secret_minimum = layout
    end = len(data)
    if cursor >= end:
        return False
    protection = data[cursor]
    cursor += 1
    if protection == 0:
        cursor = read_openpgp_secret_material(data, cursor, end, secret_shape)
        if cursor is None:
            return False
        checksum_length = 0 if version == 6 else 2
        return cursor + checksum_length == end
    if version == 6:
        return protection in {253, 254} and valid_openpgp_v6_protection(
            data, cursor, end, protection, secret_shape, secret_minimum
        )
    if version == 4 and protection == 253:
        return valid_openpgp_v4_aead_protection(
            data, cursor, end, secret_shape, secret_minimum
        )
    return valid_openpgp_v4_cfb_protection(
        data, cursor, end, protection, secret_shape, secret_minimum
    )


def contains_openpgp_secret_fields(data) -> bool:
    return recoverable_openpgp_secret_fields(data) or is_openpgp_secret_body(data)


def evaluate_openpgp_secret_packet(
    data, packet_offset: int, candidate_tag: int, partial_key_packet: bool,
    body_start: int, packet, budget: WorkBudget,
):
    tag, body, packet_end, malformed, _ = packet
    if tag != candidate_tag:
        fail()

    if packet_end is not None and is_openpgp_private_use_secret_body(
        body, packet_end - body_start
    ):
        if packet_offset == 0 or is_openpgp_stream_prefix(
            data, packet_offset, budget
        ):
            return packet_end

    # Recover unprotected private fields before framing decisions. Opaque
    # protected material is conclusive only with a complete packet boundary.
    if recoverable_openpgp_secret_fields(body):
        if packet_end is not None:
            return packet_end
        fail()
    if packet_end is not None and is_openpgp_secret_body(body):
        if packet_offset == 0 or is_openpgp_stream_prefix(
            data, packet_offset, budget
        ):
            return packet_end
    # Outer lengths are not trusted to hide parseable unprotected fields.
    # Opaque protected payloads are conclusive only inside the framed body.
    # Fixed packets are contiguous, so this recovery view performs no copy.
    if not partial_key_packet:
        recovery_end = min(len(data), body_start + MAX_DER_BYTES)
        source_view = memoryview(data)
        recovery_view = source_view[body_start:recovery_end]
        try:
            recovered = recoverable_openpgp_secret_fields(recovery_view)
        finally:
            recovery_view.release()
            source_view.release()
        if recovered:
            if packet_offset == 0 or is_openpgp_stream_prefix(
                data, packet_offset, budget
            ):
                fail()
    elif packet_end is not None and len(body) < MAX_DER_BYTES:
        recovery_length = min(
            max(0, len(data) - packet_end), MAX_DER_BYTES - len(body)
        )
        if recovery_length:
            body.append_segment(packet_end, recovery_length)
            if recoverable_openpgp_secret_fields(body):
                if packet_offset == 0 or is_openpgp_stream_prefix(
                    data, packet_offset, budget
                ):
                    fail()

    if partial_key_packet:
        # RFC 4880 4.2.2.4 and RFC 9580 4.2.1.4 do not permit partial
        # lengths for key packets. Public layout and capacity omission alone
        # are not secret-key evidence in native executable collisions.
        return None
    if malformed:
        # Random packet-tag bytes are common in binaries. A malformed bounded
        # chain is private-key-like only after a structured MPI public section
        # parses. Fixed-width native public bytes have no internal boundaries,
        # so public layout alone is not secret-key evidence.
        layout = openpgp_public_layout(body, require_standard_layout=True)
        if layout is not None and layout[2][0] != 'octets':
            if packet_offset == 0 or is_openpgp_stream_prefix(
                data, packet_offset, budget
            ):
                fail()
        return None
    return None


def openpgp_secret_packet_end(data, offset: int, budget: WorkBudget):
    # Raw files can contain dense runs of tag-shaped bytes. Charge every visit,
    # including candidates rejected by the semantic prefilter.
    budget.openpgp_operation()
    candidate = openpgp_semantic_candidate(data, offset)
    if candidate is None:
        return None
    candidate_tag, partial_key_packet, body_start = candidate
    packet = openpgp_packet_body(data, offset, budget)
    if packet is None:
        return None
    body = packet[1]
    try:
        return evaluate_openpgp_secret_packet(
            data, offset, candidate_tag, partial_key_packet, body_start,
            packet, budget,
        )
    finally:
        if isinstance(body, memoryview):
            body.release()


def contains_private_binary(data, budget: WorkBudget) -> bool:
    return (
        is_der_private_key(data, 0, require_end=True)
        or is_openssh_private_key(data)
        or openpgp_secret_packet_end(data, 0, budget) is not None
    )


def private_binary_prefix(compact: str, altchars) -> bool:
    prefix_text = compact.rstrip('=')[:32]
    prefix_text += '=' * (-len(prefix_text) % 4)
    try:
        prefix = base64.b64decode(
            prefix_text.encode('ascii'), altchars=altchars, validate=True
        )
    except (UnicodeError, binascii.Error, ValueError):
        return False
    return bool(
        prefix
        and (
            prefix[0] == 0x30
            or prefix.startswith(OPENSSH_MAGIC)
            or prefix[0] in OPENPGP_SECRET_HEADERS
        )
    )


def decode_base64_candidate(
    candidate: str, budget: WorkBudget, semantic_prefix_only: bool = False
):
    compact = ''.join(character for character in candidate if not character.isspace())
    if not 16 <= len(compact) <= MAX_BASE64_CHARACTERS or len(compact) % 4 == 1:
        return []
    decoded_values = []
    for altchars in (None, b'-_'):
        if altchars is None and any(character in compact for character in '-_'):
            continue
        if semantic_prefix_only and not private_binary_prefix(compact, altchars):
            continue
        padded = compact.rstrip('=') + '=' * (-len(compact.rstrip('=')) % 4)
        try:
            decoded = base64.b64decode(
                padded.encode('ascii'), altchars=altchars, validate=True
            )
        except (UnicodeError, binascii.Error, ValueError):
            continue
        if decoded not in decoded_values:
            budget.base64(len(decoded))
            decoded_values.append(decoded)
    return decoded_values


def plausible_private_body(decoded: bytes) -> bool:
    if len(decoded) < 48 or len(set(decoded)) < 16:
        return False
    printable = sum(32 <= value < 127 for value in decoded)
    return printable * 4 < len(decoded) * 3


def private_label(label: str) -> bool:
    compact = ''.join(character for character in label.casefold() if character.isalnum())
    return compact in PRIVATE_LABELS or 'privatekey' in compact


def armor_candidates(label: str, body: str, budget: WorkBudget):
    compact_label = ''.join(character for character in label.casefold() if character.isalnum())
    lines = body.splitlines()
    if compact_label == 'pgpprivatekeyblock':
        while lines and (not lines[0].strip() or ':' in lines[0]):
            lines.pop(0)
        lines = [line for line in lines if not line.strip().startswith('=')]
    compact = ''.join(character for character in ''.join(lines) if not character.isspace())
    values = decode_base64_candidate(compact, budget)
    if values:
        return values
    values = []
    for match in re.finditer(r'[A-Za-z0-9+/_=-]{32,}', body):
        values.extend(decode_base64_candidate(match.group(0), budget))
    return values


def inspect_armor(label: str, body: str, paired: bool, budget: WorkBudget) -> None:
    compact_label = ''.join(character for character in label.casefold() if character.isalnum())
    decoded_values = armor_candidates(label, body, budget)
    for decoded in decoded_values:
        if contains_private_binary(decoded, budget):
            fail()
        if compact_label == 'encryptedprivatekey' and is_encrypted_private_key_info(decoded):
            fail()
        if plausible_private_body(decoded):
            fail()
    if paired and compact_label == 'opensshprivatekey':
        # A paired OpenSSH label with no decodable body is malformed, not a key.
        return


class ArmorDetector:
    def __init__(self, budget: WorkBudget):
        self.budget = budget
        self.buffer = ''
        self.active = None

    def append_body(self, text: str) -> None:
        if self.active is None:
            return
        if '\x00' in text:
            prefix = text.split('\x00', 1)[0]
            self.active['body'] += prefix
            inspect_armor(self.active['label'], self.active['body'], False, self.budget)
            self.active = None
            return
        self.active['body'] += text
        if len(self.active['body']) > MAX_ARMOR_CHARACTERS:
            inspect_armor(self.active['label'], self.active['body'], False, self.budget)
            self.active = None

    def handle_marker(self, kind: str, label: str) -> None:
        compact_kind = ''.join(character for character in kind.casefold() if character.isalpha())
        if compact_kind == 'begin' and private_label(label):
            if self.active is not None:
                inspect_armor(self.active['label'], self.active['body'], False, self.budget)
            self.active = {'label': label, 'body': ''}
        elif compact_kind == 'end' and self.active is not None:
            paired = private_label(label)
            inspect_armor(self.active['label'], self.active['body'], paired, self.budget)
            self.active = None

    def feed(self, text: str) -> None:
        self.buffer += text
        while True:
            match = ARMOR_MARKER.search(self.buffer)
            if match is None:
                break
            self.append_body(self.buffer[:match.start()])
            self.handle_marker(match.group(1), match.group(2))
            self.buffer = self.buffer[match.end():]
        if len(self.buffer) > 512:
            self.append_body(self.buffer[:-512])
            self.buffer = self.buffer[-512:]

    def finish(self) -> None:
        self.append_body(self.buffer)
        if self.active is not None:
            inspect_armor(self.active['label'], self.active['body'], False, self.budget)


class Base64Detector:
    def __init__(self, budget: WorkBudget):
        self.budget = budget
        self.candidate = []
        self.oversized = False

    def evaluate(self) -> None:
        if not self.oversized and len(self.candidate) >= 32:
            for decoded in decode_base64_candidate(
                ''.join(self.candidate), self.budget, semantic_prefix_only=True
            ):
                if contains_private_binary(decoded, self.budget):
                    fail()
        self.candidate = []
        self.oversized = False

    def feed(self, text: str) -> None:
        for match in re.finditer(r'[A-Za-z0-9+/=_-]+|[^\sA-Za-z0-9+/=_-]+', text):
            token = match.group(0)
            if token[0] in BASE64_CHARACTERS:
                if self.oversized:
                    continue
                self.candidate.extend(token)
                if len(self.candidate) > MAX_BASE64_CHARACTERS:
                    self.candidate = []
                    self.oversized = True
            else:
                self.evaluate()

    def finish(self) -> None:
        self.evaluate()


PLACEHOLDER_LITERALS = {
    'change_me', 'changeme', 'dummy', 'example', 'example_encryption_key',
    'fake', 'masked', 'mock', 'nil', 'none', 'not_set', 'not-set', 'null',
    'placeholder', 'redacted', 'replace_me', 'sample', 'test', 'todo',
    'undefined', 'your_api_key',
}


def is_placeholder(value: object) -> bool:
    if value is None or isinstance(value, bool):
        return True
    if not isinstance(value, str):
        return False
    text = value.strip()
    if not text:
        return True
    if text.casefold() in PLACEHOLDER_LITERALS:
        return True
    if re.fullmatch(
        r'\$\{[A-Z_][A-Z0-9_]*\}|\$[A-Z_][A-Z0-9_]*|%[A-Z_][A-Z0-9_]*%',
        text,
    ):
        return True
    return bool(re.fullmatch(r'\{\{\s*[A-Z_][A-Z0-9_]*\s*\}\}', text))


def strip_assignment_comment(value: str) -> str:
    quote = None
    escaped = False
    for index, character in enumerate(value):
        if escaped:
            escaped = False
        elif character == '\\' and quote:
            escaped = True
        elif quote:
            if character == quote:
                quote = None
        elif character in {'"', "'"}:
            quote = character
        elif character == '#' and (index == 0 or value[index - 1].isspace()):
            return value[:index].rstrip()
    return value.strip()


def assignment_value(text: str) -> str:
    value = strip_assignment_comment(text.strip())
    if not value:
        return ''
    if value[0] in {'"', "'"}:
        quote = value[0]
        escaped = False
        for index in range(1, len(value)):
            if escaped:
                escaped = False
            elif value[index] == '\\':
                escaped = True
            elif value[index] == quote:
                if index == len(value) - 1:
                    return value[1:index]
                return value
    return value


class AssignmentDetector:
    def __init__(self):
        self.buffer = ''
        self.logical_line = ''

    def inspect_line(self, line: str) -> None:
        for match in NAMED_ASSIGNMENT.finditer(line):
            if not is_placeholder(assignment_value(line[match.end():])):
                fail()

    def feed(self, text: str) -> None:
        self.buffer += text
        while '\n' in self.buffer:
            line, self.buffer = self.buffer.split('\n', 1)
            if len(line) + len(self.logical_line) > MAX_ASSIGNMENT_LINE:
                if NAMED_ASSIGNMENT.search(self.logical_line + line):
                    fail()
                line = line[-1024:]
                self.logical_line = ''
            stripped = line.rstrip()
            if stripped.endswith('\\'):
                self.logical_line += stripped[:-1] + ' '
            else:
                self.inspect_line(self.logical_line + line)
                self.logical_line = ''
        if len(self.buffer) + len(self.logical_line) > MAX_ASSIGNMENT_LINE:
            if NAMED_ASSIGNMENT.search(self.logical_line + self.buffer):
                fail()
            self.buffer = self.buffer[-1024:]
            self.logical_line = ''

    def finish(self) -> None:
        self.inspect_line(self.logical_line + self.buffer)


class SecurityTextDetector:
    def __init__(self, budget: WorkBudget):
        self.armor = ArmorDetector(budget)
        self.base64 = Base64Detector(budget)
        self.assignments = AssignmentDetector()

    def feed(self, text: str) -> None:
        normalized = []
        for source_character in text:
            for character in unicodedata.normalize('NFKC', source_character):
                category = unicodedata.category(character)
                if character in {'\r', '\n'}:
                    character = '\n'
                elif category.startswith('C'):
                    character = '\x00'
                elif character.isspace():
                    character = ' '
                elif category == 'Pd':
                    character = '-'
                normalized.append(character)
        normalized_text = ''.join(normalized)
        self.base64.feed(normalized_text)
        self.armor.feed(normalized_text)
        self.assignments.feed(normalized_text)

    def finish(self) -> None:
        self.armor.finish()
        self.base64.finish()
        self.assignments.finish()


def scan_raw_private_formats(data, budget: WorkBudget) -> None:
    # Supported private DER structures all begin with a canonical version 0/1
    # INTEGER, so locate those bounded semantic offsets without walking every
    # generic ASN.1 SEQUENCE byte in large native libraries.
    for match in DER_PRIVATE_CANDIDATE.finditer(data):
        offset = match.start()
        item = read_der_tlv(data, offset, len(data))
        if item is not None and item[0] == 0x30 and item[2] - offset >= 13:
            version_item = read_der_tlv(data, item[1], item[2])
            version = der_integer(data, version_item) if version_item is not None else None
            if version is not None and len(version) == 1 and version[0] in {0, 1}:
                budget.der()
                if is_der_private_key(data, offset):
                    fail()

    offset = data.find(OPENSSH_MAGIC)
    while offset >= 0:
        candidate = bytes(data[offset:min(len(data), offset + MAX_DER_BYTES)])
        if is_openssh_private_key(candidate):
            fail()
        offset = data.find(OPENSSH_MAGIC, offset + 1)

    # Framing and body-version filtering stay in the C regex engine. Python
    # only visits structurally plausible candidates, each charged below.
    for match in OPENPGP_SECRET_CANDIDATE.finditer(data):
        if openpgp_secret_packet_end(data, match.start(), budget) is not None:
            fail()


def utf8_whitespace_width(data, offset: int) -> int:
    for encoded in UTF8_WHITESPACE_BYTES:
        if bytes(data[offset:offset + len(encoded)]) == encoded:
            return len(encoded)
    return 0


def utf8_base64_candidate(data, offset: int):
    characters = []
    cursor = offset
    scan_limit = min(len(data), offset + MAX_BASE64_CHARACTERS * 8)
    while cursor < scan_limit:
        value = data[cursor]
        if value in BASE64_BYTES:
            characters.append(chr(value))
            if len(characters) > MAX_BASE64_CHARACTERS:
                return None
            cursor += 1
            continue
        width = utf8_whitespace_width(data, cursor)
        if not width:
            break
        cursor += width
    return ''.join(characters)


def utf16_base64_candidate(data, offset: int, encoding: str):
    characters = []
    cursor = offset
    scan_limit = min(len(data), offset + MAX_BASE64_CHARACTERS * 16)
    byteorder = 'little' if encoding == 'utf-16le' else 'big'
    while cursor + 2 <= scan_limit:
        value = int.from_bytes(data[cursor:cursor + 2], byteorder)
        if value < 128 and value in BASE64_BYTES:
            characters.append(chr(value))
            if len(characters) > MAX_BASE64_CHARACTERS:
                return None
        elif value not in UNICODE_WHITESPACE_CODEPOINTS:
            break
        cursor += 2
    return ''.join(characters)


def scan_encoded_base64_formats(data, excluded_span, budget: WorkBudget) -> None:
    for match in UTF8_BASE64_START.finditer(data):
        if excluded_span is not None and excluded_span[0] <= match.start() < excluded_span[1]:
            continue
        candidate = utf8_base64_candidate(data, match.start())
        if candidate is None:
            continue
        for decoded in decode_base64_candidate(
            candidate, budget, semantic_prefix_only=True
        ):
            if contains_private_binary(decoded, budget):
                fail()

    for encoding, pattern in UTF16_BASE64_STARTS.items():
        for match in pattern.finditer(data):
            candidate = utf16_base64_candidate(data, match.start(), encoding)
            if candidate is None:
                continue
            for decoded in decode_base64_candidate(
                candidate, budget, semantic_prefix_only=True
            ):
                if contains_private_binary(decoded, budget):
                    fail()


def unique_json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON key')
        result[key] = value
    return result


def inspect_jwk(node) -> None:
    if isinstance(node, dict):
        kty = node.get('kty')
        if isinstance(kty, str):
            private_names = set()
            normalized_kty = kty.upper()
            if normalized_kty == 'RSA':
                private_names = {'d', 'p', 'q', 'dp', 'dq', 'qi', 'oth'}
            elif normalized_kty in {'EC', 'OKP'}:
                private_names = {'d'}
            for name in private_names:
                # A private JWK member is key syntax, not a config placeholder.
                if name in node:
                    fail()
            if normalized_kty == 'OCT' and 'k' in node:
                key = node['k']
                if not isinstance(key, str) or key:
                    fail()
        for value in node.values():
            inspect_jwk(value)
    elif isinstance(node, list):
        for value in node:
            inspect_jwk(value)


def scan_json_jwk(record, data) -> None:
    path = record[0]
    if len(data) > MAX_JSON_BYTES:
        if path.suffix.lower() in {'.json', '.jwk'}:
            fail()
        return
    raw = bytes(data)
    views = []
    for encoding in ('utf-8-sig', 'utf-16le', 'utf-16be'):
        try:
            text = raw.decode(encoding)
        except UnicodeError:
            continue
        stripped = text.lstrip('\ufeff \t\r\n')
        if stripped.startswith(('{', '[')) and stripped.rstrip().endswith(('}', ']')):
            views.append(stripped)
    for text in dict.fromkeys(views):
        try:
            document = json.loads(text, object_pairs_hook=unique_json_object)
        except (TypeError, ValueError, json.JSONDecodeError):
            if path.suffix.lower() in {'.json', '.jwk'}:
                fail()
            continue
        inspect_jwk(document)


def scan_segment(data, start: int, end: int, encoding: str, budget: WorkBudget) -> None:
    budget.text(end - start)
    decoder = codecs.getincrementaldecoder(encoding)(errors='ignore')
    detector = SecurityTextDetector(budget)
    offset = start
    while offset < end:
        limit = min(offset + CHUNK_BYTES, end)
        detector.feed(decoder.decode(data[offset:limit], final=False))
        offset = limit
    detector.feed(decoder.decode(b'', final=True))
    detector.finish()


def byte_alternation(values) -> bytes:
    return b'(?:' + b'|'.join(re.escape(value) for value in values) + b')'


UTF8_WHITESPACE_PATTERN = byte_alternation(UTF8_WHITESPACE_BYTES)
UTF8_SEPARATOR_PATTERN = (
    b'(?:[\x00-\x20]|'
    + b'|'.join(
        re.escape(value) for value in UTF8_WHITESPACE_BYTES if len(value) > 1
    )
    + b'){0,4}'
)


def utf16_whitespace_pattern(encoding: str) -> bytes:
    values = [chr(value).encode(encoding) for value in UNICODE_WHITESPACE_CODEPOINTS]
    return byte_alternation(values)


def utf16_word_pattern(word: str, encoding: str):
    pieces = []
    whitespace = utf16_whitespace_pattern(encoding)
    if encoding == 'utf-16le':
        separator = rb'(?:(?:[\x00-\x1f]\x00)|' + whitespace + rb'){0,4}'
        for character in word:
            letters = bytes(sorted({ord(character.lower()), ord(character.upper())}))
            pieces.append(b'[' + letters + b']\x00')
            pieces.append(separator)
    else:
        separator = rb'(?:(?:\x00[\x00-\x1f])|' + whitespace + rb'){0,4}'
        for character in word:
            letters = bytes(sorted({ord(character.lower()), ord(character.upper())}))
            pieces.append(b'\x00[' + letters + b']')
            pieces.append(separator)
    return re.compile(b''.join(pieces))


def byte_word_pattern(word: str):
    pieces = []
    for character in word:
        letters = bytes(sorted({ord(character.lower()), ord(character.upper())}))
        pieces.append(b'[' + letters + b']')
        pieces.append(UTF8_SEPARATOR_PATTERN)
    return re.compile(b''.join(pieces))


NAMED_SECRET_NAMES = (
    'FLY_API_TOKEN', 'JUMPGATE_ENCRYPTION_KEY',
    'KODI_ANDROID_STORE_PASSWORD', 'KODI_ANDROID_KEY_PASSWORD',
)
UTF8_TEXT_HINTS = [
    (byte_word_pattern('BEGIN'), 512, MAX_ARMOR_CHARACTERS * 4),
] + [
    (byte_word_pattern(name), 1024, MAX_ASSIGNMENT_LINE * 4)
    for name in NAMED_SECRET_NAMES
]
UTF16_TEXT_HINTS = {
    encoding: [
        (utf16_word_pattern('BEGIN', encoding), 1024, MAX_ARMOR_CHARACTERS * 2),
    ] + [
        (utf16_word_pattern(name, encoding), 2048, MAX_ASSIGNMENT_LINE * 2)
        for name in NAMED_SECRET_NAMES
    ]
    for encoding in ('utf-16le', 'utf-16be')
}


def utf16_ascii_unit(character: str, encoding: str) -> bytes:
    return character.encode(encoding)


def utf16_ascii_class(byte_class: bytes, encoding: str) -> bytes:
    if encoding == 'utf-16le':
        return byte_class + b'\x00'
    return b'\x00' + byte_class


UTF8_BASE64_UNIT = rb'[A-Za-z0-9+/=_-]'
UTF8_BASE64_SPACE = rb'(?:' + UTF8_WHITESPACE_PATTERN + rb'){0,16}'
UTF8_BASE64_START = re.compile(
    rb'(?<![A-Za-z0-9+/=_-])(?:'
    rb'M' + UTF8_BASE64_SPACE + rb'[A-I]|'
    rb'b' + UTF8_BASE64_SPACE + rb'3' + UTF8_BASE64_SPACE + rb'B'
    + UTF8_BASE64_SPACE + rb'l|'
    rb'[xln]' + UTF8_BASE64_SPACE + UTF8_BASE64_UNIT
    + rb')'
)


def utf16_base64_start_pattern(encoding: str):
    unit = utf16_ascii_class(rb'[A-Za-z0-9+/=_-]', encoding)
    der_length = utf16_ascii_class(rb'[A-I]', encoding)
    pgp_start = utf16_ascii_class(rb'[xln]', encoding)
    whitespace = rb'(?:' + utf16_whitespace_pattern(encoding) + rb'){0,16}'
    encoded = lambda character: re.escape(utf16_ascii_unit(character, encoding))
    previous = (
        rb'(?<![A-Za-z0-9+/=_-]\x00)'
        if encoding == 'utf-16le'
        else rb'(?<!\x00[A-Za-z0-9+/=_-])'
    )
    return re.compile(
        previous + rb'(?:'
        + encoded('M') + whitespace + der_length + rb'|'
        + encoded('b') + whitespace + encoded('3') + whitespace
        + encoded('B') + whitespace + encoded('l') + rb'|'
        + pgp_start + whitespace + unit + rb')'
    )


UTF16_BASE64_STARTS = {
    encoding: utf16_base64_start_pattern(encoding)
    for encoding in ('utf-16le', 'utf-16be')
}


def hint_windows(data, hints, encoded_scale: int = 1):
    windows = []
    for pattern, before, after in hints:
        for match in pattern.finditer(data):
            if len(windows) >= MAX_HINT_WINDOWS:
                fail()
            phase = match.start() % encoded_scale
            start = max(0, match.start() - before)
            start += (phase - start) % encoded_scale
            end = min(len(data), match.end() + after)
            windows.append((start, end))
    windows.sort()
    merged = []
    for start, end in windows:
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


def exclude_window_span(windows, excluded_span):
    if excluded_span is None:
        return windows
    result = []
    for start, end in windows:
        if start < excluded_span[0]:
            result.append((start, min(end, excluded_span[0])))
        if end > excluded_span[1]:
            result.append((max(start, excluded_span[1]), end))
    return [(start, end) for start, end in result if start < end]


def scan_known_encodings(record, data, accepted_span, budget: WorkBudget) -> None:
    utf8_windows = hint_windows(data, UTF8_TEXT_HINTS)
    for start, end in exclude_window_span(utf8_windows, accepted_span):
        scan_segment(data, start, end, 'utf-8', budget)
    for encoding in ('utf-16le', 'utf-16be'):
        for start, end in hint_windows(data, UTF16_TEXT_HINTS[encoding], encoded_scale=2):
            scan_segment(data, start, end, encoding, budget)


def main() -> None:
    if os.environ.get('JUMPGATE_TEST_FORCE_SECRET_SCANNER_ERROR') == '1':
        raise RuntimeError('forced scanner error')
    set_scan_context(None, 'archive-structure')
    files = collect_regular_files()
    budget = WorkBudget()
    expected_record = next((record for record in files if record[1] == allowed_path), None)
    if expected_record is None:
        fail()

    set_scan_context(expected_record, 'allowed-airplay-key')
    with mapped_file(expected_record) as data:
        if data is None:
            fail()
        candidates = list(CANONICAL_BLOCK.finditer(data))
        if len(candidates) != 1:
            fail()
        accepted = candidates[0]
        allowed_body = accepted.group('body')
        try:
            allowed_der = base64.b64decode(allowed_body, validate=True)
        except (binascii.Error, ValueError):
            fail()
        if (
            base64.b64encode(allowed_der) != allowed_body
            or hashlib.sha256(allowed_der).hexdigest() != allowed_der_sha256
        ):
            fail()
        accepted_span = accepted.span()

    # Known plaintext encodings and normalizations are covered. Compression,
    # encryption, and arbitrary cryptographic obfuscation are intentionally out of scope.
    for record in files:
        set_scan_context(record, 'file-read')
        with mapped_file(record) as data:
            if data is None:
                continue
            set_scan_context(record, 'raw-private-format')
            scan_raw_private_formats(data, budget)
            set_scan_context(record, 'json-private-key')
            scan_json_jwk(record, data)
            record_span = accepted_span if record[1] == allowed_path else None
            set_scan_context(record, 'encoded-private-format')
            scan_encoded_base64_formats(data, record_span, budget)
            set_scan_context(record, 'text-secret')
            scan_known_encodings(record, data, record_span, budget)

    expected_copy_probe = os.environ.get(
        'JUMPGATE_TEST_OPENPGP_EXPECT_COPIED_BYTES'
    )
    if expected_copy_probe is not None:
        set_scan_context(None, 'openpgp-copy-count-probe')
        if not re.fullmatch(r'[0-9]+', expected_copy_probe):
            fail()
        expected_copied_bytes = int(expected_copy_probe)
        if (
            expected_copied_bytes > MAX_OPENPGP_COPY_BYTES
            or budget.openpgp_copied_bytes != expected_copied_bytes
        ):
            fail()

    expected_operation_probe = os.environ.get(
        'JUMPGATE_TEST_OPENPGP_EXPECT_OPERATIONS'
    )
    if expected_operation_probe is not None:
        set_scan_context(None, 'openpgp-operation-count-probe')
        if not re.fullmatch(r'[0-9]+', expected_operation_probe):
            fail()
        expected_operations = int(expected_operation_probe)
        if (
            expected_operations > MAX_OPENPGP_WORK
            or budget.openpgp_operations != expected_operations
        ):
            fail()

    work_probe = os.environ.get('JUMPGATE_TEST_OPENPGP_WORK')
    if work_probe is not None:
        set_scan_context(None, 'openpgp-work-budget-probe')
        if not re.fullmatch(r'[0-9]+', work_probe):
            fail()
        target_operations = int(work_probe)
        if (
            target_operations > MAX_OPENPGP_WORK + 1
            or budget.openpgp_operations > target_operations
        ):
            fail()
        fixed_probe = bytes((0xC5, 6, 4, 0, 0, 0, 0, 0))
        while budget.openpgp_operations < target_operations:
            if openpgp_secret_packet_end(fixed_probe, 0, budget) is not None:
                fail()
        if budget.openpgp_operations != target_operations:
            fail()

    copy_probe = os.environ.get('JUMPGATE_TEST_OPENPGP_COPY_WORK')
    if copy_probe is not None:
        set_scan_context(None, 'openpgp-copy-budget-probe')
        if copy_probe not in {'exact', 'overflow'}:
            fail()
        remaining = MAX_OPENPGP_COPY_BYTES - budget.openpgp_copied_bytes
        if remaining < 0:
            fail()
        budget.openpgp_copy(remaining)
        if copy_probe == 'overflow':
            probe = OpenPgpSegmentedBody(b'\x00', budget)
            probe.append_segment(0, 1)
            probe[:1]


try:
    main()
except BaseException as error:
    message = str(error)
    if message.startswith('JUMPGATE_APK_SCAN_REJECT '):
        print(message, file=sys.stderr)
    raise SystemExit(1)
PY
then
  fail 'APK contains apparent private signing, deployment, or runtime secret material'
fi

if ! python3 - "$extract_dir" <<'PY'
import hashlib
import json
import os
import pathlib
import re
import sys
import yaml

root = pathlib.Path(sys.argv[1])

HIGH_RISK_KEYS = {
    'access_token', 'api_key', 'api_secret', 'api_token', 'auth_token', 'authorization',
    'bearer', 'bearer_token', 'client_secret', 'client_token',
    'connection_string', 'database_url', 'encryption_key', 'id_token',
    'oauth_token', 'password', 'passphrase', 'private_key', 'redis_url',
    'refresh_token', 'secret', 'secret_key', 'secret_token', 'secret_value',
    'session_token', 'token', 'token_data', 'token_key', 'token_secret',
    'token_value',
}
BENIGN_TOKEN_METADATA_KEYS = {
    'token_count', 'token_description', 'token_endpoint', 'token_expires',
    'token_expires_at', 'token_expiry', 'token_format', 'token_issuer',
    'token_label', 'token_length', 'token_lifetime', 'token_metadata',
    'token_name', 'token_scope', 'token_scopes', 'token_status', 'token_ttl',
    'token_type', 'token_url', 'token_uri', 'token_version',
}
HIGH_RISK_COMPACT = {key.replace('_', '') for key in HIGH_RISK_KEYS}
HIGH_RISK_SUFFIXES = (
    '_api_key', '_encryption_key', '_passphrase', '_password', '_private_key',
    '_secret', '_token',
)
TEXT_SUFFIXES = {'.cfg', '.conf', '.env', '.ini', '.properties', '.txt'}
ASSIGNMENT = re.compile(
    r'''^\s*(?:export\s+)?(?:"([^"]+)"|'([^']+)'|([A-Za-z_][A-Za-z0-9_.-]*))'''
    r'''\s*[:=]\s*(.*?)\s*$''',
)
GETTEXT_CATALOG_ENTRY = re.compile(
    r'^(?:assets/)?addons/'
    r'[a-z0-9]+(?:[._-][a-z0-9]+)*/lang/_strings/'
    r'[a-z0-9]+(?:[_@-][a-z0-9]+)*\.json$'
)
# Catalog exemptions authenticate exact reviewed bytes. Any catalog change requires
# a reviewed SHA-256 refresh; do not replace this map with message-key heuristics.
AUTHENTICATED_GETTEXT_CATALOGS = {
    'addons/webinterface.default/lang/_strings/ca.json':
        '3cf7893600ad0b6a06a5c8f7b819f8628d221758b3595b33dbbdf4f008bd52fb',
    'addons/webinterface.default/lang/_strings/de.json':
        '71962a3e5d06ff4c1e6f8b4a6dcf74ee2f0ab65fd10b73953c61d879e228c3a4',
    'addons/webinterface.default/lang/_strings/en.json':
        '3635a6232502075988c7e7d4f50dfa50a25ff0a6d8b9986f4d434ccfb978cff6',
    'addons/webinterface.default/lang/_strings/et.json':
        '35b5e614b902ed788e2bc6102a1da989a4552ccc41535b5317a228b31eb5d24c',
    'addons/webinterface.default/lang/_strings/fi.json':
        '6149652b2aa36687a7c487f83da4447c51793b05f4ab02fcc1f22583966eb611',
    'addons/webinterface.default/lang/_strings/fr.json':
        '0570b7c22a4f4d7a520038ca84a975ffa4e8f0f177c583eba76662db11ed1a93',
    'addons/webinterface.default/lang/_strings/hr.json':
        '7ba2b179ba47657aecd29f2d2017a245ff8a903597269e5ccaf169449b10bfb3',
    'addons/webinterface.default/lang/_strings/hu.json':
        '13361d638b5e06245254cea75bda8d214d59014f13bb8e50afdeb6d3b5fc8262',
    'addons/webinterface.default/lang/_strings/it.json':
        '7102338e7cdf4a47af49eb75ba5e3f51aa7fc3923e321bac1a6f975b6f347694',
    'addons/webinterface.default/lang/_strings/ja.json':
        'd2edffca36008913bab04328eb1fe8a5f3d5ce375252550f9eacbd8debee4d64',
    'addons/webinterface.default/lang/_strings/ko.json':
        'b6c3900e142171193c3ff38b1f03bbf412a36a4e603d2b29344d3032cfee13ad',
    'addons/webinterface.default/lang/_strings/nl.json':
        'b630c74bf1a292e6db9aa9369fc75e06daa0195b4477ab39e88b019606af9f43',
    'addons/webinterface.default/lang/_strings/ru.json':
        'ca03a5ef31e4bb5a4326fed9b5940d710250f7a29b4246d06a5c4ae04baa4bfb',
    'addons/webinterface.default/lang/_strings/sk.json':
        '8f939ff044bfa27e8f85d338c45b8eec2dd5ff120752b15eff153afd087064c7',
    'addons/webinterface.default/lang/_strings/zh_tw.json':
        '5a73da1376e81bee87400f7d35564af4d5773c7f6b6d0e1fcc343b2d347ee6e1',
}

scan_entry_sha256 = None
scan_phase = None


class SecretFound(Exception):
    pass


def normalized_entry(path: pathlib.Path) -> str:
    relative = pathlib.PurePosixPath(path.relative_to(root).as_posix())
    if relative.is_absolute() or any(part in {'', '.', '..'} for part in relative.parts):
        raise ValueError('invalid packaged configuration path')
    return relative.as_posix()


def set_scan_context(entry: str, phase: str) -> None:
    global scan_entry_sha256, scan_phase
    scan_entry_sha256 = None
    scan_phase = phase
    scan_entry_sha256 = hashlib.sha256(entry.encode('utf-8')).hexdigest()


def candidate_files() -> list[tuple[pathlib.Path, str]]:
    candidates = []
    for path in root.rglob('*'):
        if path.is_file():
            candidates.append((path, normalized_entry(path)))
    sort_key = lambda record: record[1].encode('utf-8')
    # The test hook perturbs discovery order; the final production sort remains decisive.
    if os.environ.get('JUMPGATE_TEST_REVERSE_CONFIG_CANDIDATES') == '1':
        candidates.sort(key=sort_key, reverse=True)
    candidates.sort(key=sort_key)
    return candidates


def normalize_key(key: object) -> str:
    text = str(key)
    text = re.sub(r'([a-z0-9])([A-Z])', r'\1_\2', text)
    return re.sub(r'[^a-z0-9]+', '_', text.lower()).strip('_')


def is_high_risk_key(key: object) -> bool:
    normalized = normalize_key(key)
    if normalized in BENIGN_TOKEN_METADATA_KEYS:
        return False
    compact = normalized.replace('_', '')
    return (
        normalized in HIGH_RISK_KEYS
        or compact in HIGH_RISK_COMPACT
        or normalized.endswith(HIGH_RISK_SUFFIXES)
        or compact.endswith(('accesstoken', 'apikey', 'apisecret', 'authtoken',
                             'clientsecret', 'encryptionkey', 'password',
                             'passphrase', 'privatekey', 'refreshtoken'))
    )


PLACEHOLDER_LITERALS = {
    'change_me', 'changeme', 'dummy', 'example', 'example_encryption_key',
    'fake', 'masked', 'mock', 'nil', 'none', 'not_set', 'not-set', 'null',
    'placeholder', 'redacted', 'replace_me', 'sample', 'test', 'todo',
    'undefined', 'your_api_key',
}


def is_placeholder(value: object, seen: set[int] | None = None) -> bool:
    if seen is None:
        seen = set()
    if value is None or isinstance(value, bool):
        return True
    if isinstance(value, (list, tuple)):
        if id(value) in seen:
            return True
        seen.add(id(value))
        return not value or all(is_placeholder(item, seen) for item in value)
    if isinstance(value, dict):
        if id(value) in seen:
            return True
        seen.add(id(value))
        return not value or all(is_placeholder(item, seen) for item in value.values())
    if not isinstance(value, str):
        return False

    text = value.strip()
    if not text:
        return True
    if text.casefold() in PLACEHOLDER_LITERALS:
        return True
    if re.fullmatch(
        r'\$\{[A-Z_][A-Z0-9_]*\}|\$[A-Z_][A-Z0-9_]*|%[A-Z_][A-Z0-9_]*%',
        text,
    ):
        return True
    return bool(re.fullmatch(r'\{\{\s*[A-Z_][A-Z0-9_]*\s*\}\}', text))


def inspect_structure(
    node: object,
    message_ids: dict | None = None,
    seen: set[int] | None = None,
) -> None:
    if seen is None:
        seen = set()
    if isinstance(node, (dict, list)):
        if id(node) in seen:
            return
        seen.add(id(node))
    if isinstance(node, dict):
        for key, value in node.items():
            if (
                node is not message_ids
                and is_high_risk_key(key)
                and not is_placeholder(value)
            ):
                raise SecretFound
            inspect_structure(value, message_ids, seen)
    elif isinstance(node, list):
        for value in node:
            inspect_structure(value, message_ids, seen)


def unique_json_object(pairs: list[tuple[object, object]]) -> dict:
    mapping = {}
    for key, value in pairs:
        if key in mapping:
            raise ValueError('duplicate JSON key')
        mapping[key] = value
    return mapping


def authenticated_gettext_message_ids(
    document: object,
    entry: str,
    raw: bytes,
) -> dict | None:
    if GETTEXT_CATALOG_ENTRY.fullmatch(entry) is None:
        return None
    if not isinstance(document, dict) or set(document) != {'domain', 'locale_data'}:
        return None
    if document.get('domain') != 'messages':
        return None
    locale_data = document.get('locale_data')
    if not isinstance(locale_data, dict) or set(locale_data) != {'messages'}:
        return None
    messages = locale_data.get('messages')
    if not isinstance(messages, dict):
        return None
    metadata = messages.get('')
    if (
        not isinstance(metadata, dict)
        or set(metadata) != {'domain', 'plural_forms', 'lang'}
        or metadata.get('domain') != 'messages'
        or not isinstance(metadata.get('plural_forms'), str)
        or not isinstance(metadata.get('lang'), str)
        or not metadata['lang']
    ):
        return None
    for message_id, translations in messages.items():
        if message_id == '':
            continue
        if (
            not isinstance(message_id, str)
            or not message_id
            or not isinstance(translations, list)
            or not translations
            or any(not isinstance(value, str) for value in translations)
        ):
            return None
    canonical_entry = entry[7:] if entry.startswith('assets/') else entry
    expected_digest = AUTHENTICATED_GETTEXT_CATALOGS.get(canonical_entry)
    if expected_digest is None or hashlib.sha256(raw).hexdigest() != expected_digest:
        return None
    return messages


def inspect_json(path: pathlib.Path, entry: str) -> None:
    raw = path.read_bytes()
    document = json.loads(raw, object_pairs_hook=unique_json_object)
    inspect_structure(document, authenticated_gettext_message_ids(document, entry, raw))


class UniqueKeySafeLoader(yaml.SafeLoader):
    pass


def construct_unique_mapping(loader, node, deep: bool = False) -> dict:
    loader.flatten_mapping(node)
    mapping = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        try:
            duplicate = key in mapping
        except TypeError as error:
            raise yaml.constructor.ConstructorError(
                'while constructing a mapping', node.start_mark,
                'found an unhashable key', key_node.start_mark,
            ) from error
        if duplicate:
            raise yaml.constructor.ConstructorError(
                'while constructing a mapping', node.start_mark,
                'found a duplicate key', key_node.start_mark,
            )
        mapping[key] = loader.construct_object(value_node, deep=deep)
    return mapping


UniqueKeySafeLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG,
    construct_unique_mapping,
)


def inspect_yaml(path: pathlib.Path) -> None:
    with path.open('r', encoding='utf-8-sig') as stream:
        for document in yaml.load_all(stream, Loader=UniqueKeySafeLoader):
            inspect_structure(document)


def strip_inline_comment(value: str) -> str:
    quote = None
    escaped = False
    index = 0
    while index < len(value):
        character = value[index]
        if escaped:
            escaped = False
        elif character == '\\' and quote:
            escaped = True
        elif quote:
            if character == quote:
                quote = None
        elif character in {'"', "'"}:
            quote = character
        elif character in {'#', ';'} and (index == 0 or value[index - 1].isspace()):
            return value[:index].rstrip()
        elif value[index:index + 2] == '//' and (index == 0 or value[index - 1].isspace()):
            return value[:index].rstrip()
        index += 1
    return value.strip()


def parse_scalar(value: str) -> str:
    value = strip_inline_comment(value).strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {'"', "'"}:
        return value[1:-1]
    return value


def inspect_assignments(path: pathlib.Path) -> None:
    with path.open('r', encoding='utf-8-sig', errors='replace') as stream:
        for line in stream:
            stripped = line.lstrip()
            if not stripped or stripped.startswith(('#', ';', '//')):
                continue
            match = ASSIGNMENT.match(line.rstrip('\r\n'))
            if not match:
                continue
            key = next(group for group in match.groups()[:3] if group is not None)
            if is_high_risk_key(key) and not is_placeholder(parse_scalar(match.group(4))):
                raise SecretFound


def scan_configuration() -> None:
    if os.environ.get('JUMPGATE_TEST_FORCE_CONFIG_SCANNER_ERROR') == '1':
        raise RuntimeError('forced configuration scanner error')
    for path, entry in candidate_files():
        lower_name = path.name.lower()
        if path.suffix.lower() == '.json':
            set_scan_context(entry, 'config-json')
            inspect_json(path, entry)
        elif path.suffix.lower() in {'.yaml', '.yml'}:
            set_scan_context(entry, 'config-yaml')
            inspect_yaml(path)
        elif (
            path.suffix.lower() in TEXT_SUFFIXES
            or lower_name == '.env'
            or lower_name.startswith('.env.')
        ):
            set_scan_context(entry, 'config-text')
            with path.open('rb') as stream:
                binary_prefix = stream.read(4096)
            if b'\x00' not in binary_prefix:
                inspect_assignments(path)


def scanner_status() -> int:
    try:
        scan_configuration()
    except SecretFound:
        if scan_entry_sha256 is None or scan_phase is None:
            return 2
        detail = {'entry_sha256': scan_entry_sha256, 'phase': scan_phase}
        print(
            'JUMPGATE_APK_SCAN_REJECT ' +
            json.dumps(detail, ensure_ascii=True, separators=(',', ':'), sort_keys=True),
            file=sys.stderr,
        )
        return 1
    return 0


try:
    status = scanner_status()
except BaseException:
    status = 2
raise SystemExit(status)
PY
then
  fail 'APK contains a non-placeholder secret assignment in packaged configuration'
fi

apk_dir="$(cd "$(dirname "$apk")" && pwd)"
apk_name="$(basename "$apk")"
(
  cd "$apk_dir"
  sha256sum "$apk_name" > "$apk_name.sha256"
)

printf 'Verified APK against expected signer: package=%s version=%s abi=%s core=%s signer_sha256=%s\n' \
  "$actual_package" "$version_name" "$expected_abi" "$expected_core_library" \
  "$expected_signer_sha256"

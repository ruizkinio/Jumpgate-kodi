#!/usr/bin/env bash

set -euo pipefail

usage() {
  echo "usage: $0 <apk> <expected-abi> <expected-package> <expected-min-sdk> <expected-target-sdk> <expected-version-name> <expected-version-code> <expected-signer-sha256>" >&2
  exit 2
}

fail() {
  echo "$1" >&2
  exit 1
}

[[ "$#" -eq 8 ]] || usage

apk="$1"
expected_abi="$2"
expected_package="$3"
expected_min_sdk="$4"
expected_target_sdk="$5"
expected_version_name="$6"
expected_version_code="$7"
expected_signer_sha256="${8,,}"

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
  fail 'APK signer certificate does not match the ephemeral CI certificate'
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
[[ -f "$abi_root/libkodi.so" ]] || fail 'APK does not contain the Kodi core library'
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

if ! python3 - "$extract_dir" "$expected_abi" <<'PY'
import base64
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

root = pathlib.Path(sys.argv[1])
expected_abi = sys.argv[2]
allowed_path = pathlib.PurePosixPath('lib') / expected_abi / 'libkodi.so'
allowed_der_sha256 = '8959c62b4351cbaa702942f4572d37335a7a3dfdcc6f0d2763a2afb486e3ac8f'
MAX_FILES = 100_000
MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_TOTAL_BYTES = 1024 * 1024 * 1024
MAX_DECODE_WORK_BYTES = 5 * MAX_TOTAL_BYTES
CHUNK_BYTES = 1024 * 1024
MAX_DER_BYTES = 128 * 1024
MAX_DER_ATTEMPTS = 500_000
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


def fail() -> None:
    raise ValueError('private key material rejected')


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

    def text(self, encoded_bytes: int) -> None:
        self.text_bytes += encoded_bytes
        if self.text_bytes > MAX_DECODE_WORK_BYTES:
            fail()


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
    value = data[cursor + 2:finish]
    if not value or value[0].bit_length() + (len(value) - 1) * 8 != bits:
        return None
    return bits, finish


MAX_OPENPGP_PARTIAL_CHUNKS = 128


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


def openpgp_packet_body(data, offset: int):
    if offset >= len(data) or not data[offset] & 0x80:
        return None
    header = data[offset]
    cursor = offset + 1
    if header & 0x40:
        tag = header & 0x3F
        parsed_length = read_openpgp_new_length(data, cursor)
        if parsed_length is None:
            return tag, b'', None, True
        kind, length, cursor = parsed_length
        if kind == 'fixed':
            end = cursor + length
            available = bytes(data[cursor:min(end, len(data), cursor + MAX_DER_BYTES)])
            malformed = length > MAX_DER_BYTES or end > len(data)
            return tag, available, end if not malformed else None, malformed

        body = bytearray()
        for _ in range(MAX_OPENPGP_PARTIAL_CHUNKS):
            if length > MAX_DER_BYTES - len(body) or cursor + length > len(data):
                available = min(len(data) - cursor, MAX_DER_BYTES - len(body))
                if available > 0:
                    body.extend(data[cursor:cursor + available])
                return tag, bytes(body), None, True
            body.extend(data[cursor:cursor + length])
            cursor += length
            parsed_length = read_openpgp_new_length(data, cursor)
            if parsed_length is None:
                return tag, bytes(body), None, True
            kind, length, cursor = parsed_length
            if kind == 'partial':
                continue
            end = cursor + length
            if length > MAX_DER_BYTES - len(body) or end > len(data):
                available = min(len(data) - cursor, MAX_DER_BYTES - len(body))
                if available > 0:
                    body.extend(data[cursor:cursor + available])
                return tag, bytes(body), None, True
            body.extend(data[cursor:end])
            return tag, bytes(body), end, False
        return tag, bytes(body), None, True

    tag = (header >> 2) & 0x0F
    length_type = header & 0x03
    if length_type == 3:
        length = len(data) - cursor
        body = bytes(data[cursor:min(len(data), cursor + MAX_DER_BYTES)])
        return tag, body, len(data) if length <= MAX_DER_BYTES else None, length > MAX_DER_BYTES
    length_bytes = (1, 2, 4)[length_type]
    if cursor + length_bytes > len(data):
        return tag, b'', None, True
    length = int.from_bytes(data[cursor:cursor + length_bytes], 'big')
    cursor += length_bytes
    end = cursor + length
    body = bytes(data[cursor:min(end, len(data), cursor + MAX_DER_BYTES)])
    malformed = length > MAX_DER_BYTES or end > len(data)
    return tag, body, end if not malformed else None, malformed


def openpgp_public_layout(data):
    end = len(data)
    if end < 10 or data[0] != 4:
        return None
    algorithm = data[5]
    cursor = 6
    secret_mpi_count = 0
    if algorithm in {1, 2, 3}:  # RSA
        public = []
        for _ in range(2):
            item = read_openpgp_mpi(data, cursor, end)
            if item is None:
                return None
            public.append(item[0])
            cursor = item[1]
        if public[0] < 512 or not 2 <= public[1] <= 64:
            return None
        secret_mpi_count = 4
    elif algorithm == 17:  # DSA
        public = []
        for _ in range(4):
            item = read_openpgp_mpi(data, cursor, end)
            if item is None:
                return None
            public.append(item[0])
            cursor = item[1]
        if public[0] < 512 or public[1] < 160:
            return None
        secret_mpi_count = 1
    elif algorithm == 16:  # ElGamal
        for _ in range(3):
            item = read_openpgp_mpi(data, cursor, end)
            if item is None or item[0] < 512:
                return None
            cursor = item[1]
        secret_mpi_count = 1
    elif algorithm in {18, 19, 22}:  # ECDH/ECDSA/legacy EdDSA
        if cursor >= end or not 5 <= data[cursor] <= 32:
            return None
        oid_length = data[cursor]
        if cursor + 1 + oid_length > end:
            return None
        cursor += 1 + oid_length
        point = read_openpgp_mpi(data, cursor, end)
        if point is None or point[0] < 200:
            return None
        cursor = point[1]
        if algorithm == 18:
            if cursor >= end or not 3 <= data[cursor] <= 16:
                return None
            kdf_length = data[cursor]
            if cursor + 1 + kdf_length > end:
                return None
            cursor += 1 + kdf_length
        secret_mpi_count = 1
    else:
        return None
    return cursor, secret_mpi_count


def is_openpgp_secret_body(data) -> bool:
    layout = openpgp_public_layout(data)
    if layout is None:
        return False
    cursor, secret_mpi_count = layout
    end = len(data)

    if cursor >= end:
        return False
    protection = data[cursor]
    cursor += 1
    if protection == 0:
        for _ in range(secret_mpi_count):
            item = read_openpgp_mpi(data, cursor, end)
            if item is None:
                return False
            cursor = item[1]
        if cursor + 2 != end:
            return False
    elif protection in {254, 255}:
        if end - cursor < 16:
            return False
    elif not 1 <= protection <= 13 or end - cursor < 16:
        return False
    return True


def openpgp_secret_packet_end(data, offset: int):
    packet = openpgp_packet_body(data, offset)
    if packet is None:
        return None
    tag, body, packet_end, malformed = packet
    if tag not in {5, 7}:
        return None
    if malformed:
        # Random packet-tag bytes are common in binaries. A malformed bounded
        # chain is private-key-like only after its public key section parses.
        if openpgp_public_layout(body) is not None:
            fail()
        return None
    if is_openpgp_secret_body(body):
        return packet_end
    return None


def contains_private_binary(data) -> bool:
    return (
        is_der_private_key(data, 0, require_end=True)
        or is_openssh_private_key(data)
        or openpgp_secret_packet_end(data, 0) is not None
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
        if contains_private_binary(decoded):
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
                if contains_private_binary(decoded):
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

    for header in OPENPGP_SECRET_HEADERS:
        needle = bytes([header])
        offset = data.find(needle)
        while offset >= 0:
            if openpgp_secret_packet_end(data, offset) is not None:
                fail()
            offset = data.find(needle, offset + 1)


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
            if contains_private_binary(decoded):
                fail()

    for encoding, pattern in UTF16_BASE64_STARTS.items():
        for match in pattern.finditer(data):
            candidate = utf16_base64_candidate(data, match.start(), encoding)
            if candidate is None:
                continue
            for decoded in decode_base64_candidate(
                candidate, budget, semantic_prefix_only=True
            ):
                if contains_private_binary(decoded):
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
    files = collect_regular_files()
    budget = WorkBudget()
    expected_record = next((record for record in files if record[1] == allowed_path), None)
    if expected_record is None:
        fail()

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
        with mapped_file(record) as data:
            if data is None:
                continue
            scan_raw_private_formats(data, budget)
            scan_json_jwk(record, data)
            record_span = accepted_span if record[1] == allowed_path else None
            scan_encoded_base64_formats(data, record_span, budget)
            scan_known_encodings(record, data, record_span, budget)


try:
    main()
except BaseException:
    raise SystemExit(1)
PY
then
  fail 'APK contains apparent private signing, deployment, or runtime secret material'
fi

if ! python3 - "$extract_dir" <<'PY'
import json
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


class SecretFound(Exception):
    pass


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


def inspect_structure(node: object, seen: set[int] | None = None) -> None:
    if seen is None:
        seen = set()
    if isinstance(node, (dict, list)):
        if id(node) in seen:
            return
        seen.add(id(node))
    if isinstance(node, dict):
        for key, value in node.items():
            if is_high_risk_key(key) and not is_placeholder(value):
                raise SecretFound
            inspect_structure(value, seen)
    elif isinstance(node, list):
        for value in node:
            inspect_structure(value, seen)


def unique_json_object(pairs: list[tuple[object, object]]) -> dict:
    mapping = {}
    for key, value in pairs:
        if key in mapping:
            raise ValueError('duplicate JSON key')
        mapping[key] = value
    return mapping


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


try:
    for path in root.rglob('*'):
        if not path.is_file():
            continue
        lower_name = path.name.lower()
        if path.suffix.lower() == '.json':
            inspect_structure(json.loads(path.read_bytes(), object_pairs_hook=unique_json_object))
        elif path.suffix.lower() in {'.yaml', '.yml'}:
            inspect_yaml(path)
        elif (
            path.suffix.lower() in TEXT_SUFFIXES
            or lower_name == '.env'
            or lower_name.startswith('.env.')
        ):
            with path.open('rb') as stream:
                binary_prefix = stream.read(4096)
            if b'\x00' not in binary_prefix:
                inspect_assignments(path)
except SecretFound:
    raise SystemExit(1)
except (OSError, UnicodeError, ValueError, json.JSONDecodeError, yaml.YAMLError, RecursionError):
    raise SystemExit(2)
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

printf 'Verified ephemeral CI APK: package=%s version=%s abi=%s\n' \
  "$actual_package" "$version_name" "$expected_abi"

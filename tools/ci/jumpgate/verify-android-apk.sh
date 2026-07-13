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
    /* | .. | ../* | */.. | */../* | *\\*)
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

private_material_pattern='-----BEGIN ([A-Z0-9]+ )?PRIVATE KEY-----|(FLY_API_TOKEN|JUMPGATE_ENCRYPTION_KEY|KODI_ANDROID_STORE_PASSWORD|KODI_ANDROID_KEY_PASSWORD)[[:space:]]*[:=][[:space:]]*[^[:space:]]{8,}'
if LC_ALL=C grep -aErIq -- "$private_material_pattern" "$extract_dir"; then
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
    lowered = text.lower()
    if lowered in {
        'change_me', 'changeme', 'dummy', 'example', 'fake', 'masked', 'mock',
        'nil', 'none', 'not_set', 'not-set', 'null', 'placeholder', 'redacted',
        'replace_me', 'sample', 'test', 'todo', 'undefined',
    }:
        return True
    if len(set(text)) == 1 and text[0] in '*#xX0._-':
        return True
    if re.fullmatch(r'\$\{[^{}]+\}|\$[A-Z_][A-Z0-9_]*|%[A-Z_][A-Z0-9_]*%', text):
        return True
    if re.fullmatch(r'\{\{.+\}\}|<[^<>]+>', text):
        return True
    if re.fullmatch(
        r'(?i)(?:your|example|dummy|sample|fake|mock|redacted|masked|placeholder|'
        r'replace(?:_me)?|change(?:_me)?|changeme|not[-_ ]?a[-_ ]?real|test)'
        r'(?:[-_ ].*)?',
        text,
    ):
        return True
    if 'example.com' in lowered or 'example.org' in lowered or 'example.net' in lowered:
        return True
    return lowered in {'localhost', '127.0.0.1', '::1'}


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

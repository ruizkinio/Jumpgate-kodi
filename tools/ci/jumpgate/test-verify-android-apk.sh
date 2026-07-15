#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
verifier="$script_dir/verify-android-apk.sh"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

expected_package='org.xbmc.kodi'
expected_min_sdk='24'
expected_target_sdk='36'
expected_version_name='22.0-ALPHA2'
expected_version_code='2190702'
mock_signer_sha256='abababababababababababababababababababababababababababababababab'
allowed_rsa_der_sha256='8959c62b4351cbaa702942f4572d37335a7a3dfdcc6f0d2763a2afb486e3ac8f'

command -v python3 >/dev/null
command -v sha256sum >/dev/null
python3 -c 'import yaml' >/dev/null
test -f "$verifier"

find_tool() {
  local root="$1"
  local name="$2"
  local candidate
  for candidate in "$root/$name" "$root/$name.exe"; do
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return
    fi
  done
  return 1
}

sdk_root="${JUMPGATE_TEST_ANDROID_SDK_ROOT:-${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}}"
ndk_root="${JUMPGATE_TEST_NDK_ROOT:-${ANDROID_NDK_ROOT:-}}"
build_tools_root="${JUMPGATE_TEST_BUILD_TOOLS_ROOT:-${ANDROID_BUILD_TOOLS_ROOT:-}}"
[[ -n "$sdk_root" && -d "$sdk_root" ]] || {
  echo 'Android SDK root is required for real-tool verifier tests' >&2
  exit 1
}
[[ -n "$ndk_root" && -d "$ndk_root" ]] || {
  echo 'Android NDK root is required for real-tool verifier tests' >&2
  exit 1
}
[[ -n "$build_tools_root" && -d "$build_tools_root" ]] || {
  echo 'Android Build Tools root is required for real-tool verifier tests' >&2
  exit 1
}

prebuilt_root="$(find "$ndk_root/toolchains/llvm/prebuilt" -mindepth 1 -maxdepth 1 \
  -type d -print -quit)"
[[ -n "$prebuilt_root" ]] || {
  echo 'NDK LLVM prebuilt toolchain was not found' >&2
  exit 1
}
real_clang="$(find_tool "$prebuilt_root/bin" clang)"
real_readelf="$(find_tool "$prebuilt_root/bin" llvm-readelf)"
real_aapt2="$(find_tool "$build_tools_root" aapt2)"
real_zipalign="$(find_tool "$build_tools_root" zipalign)"
real_apksigner_jar="$build_tools_root/lib/apksigner.jar"
android_jar="$sdk_root/platforms/android-${expected_target_sdk}/android.jar"
test -f "$real_clang"
test -f "$real_readelf"
test -f "$real_aapt2"
test -f "$real_zipalign"
test -f "$real_apksigner_jar"
test -f "$android_jar"

if [[ -n "${JUMPGATE_TEST_JAVA:-}" ]]; then
  real_java="$JUMPGATE_TEST_JAVA"
elif [[ -n "${JAVA_HOME:-}" ]] && [[ -f "$JAVA_HOME/bin/java" ]]; then
  real_java="$JAVA_HOME/bin/java"
elif [[ -n "${JAVA_HOME:-}" ]] && [[ -f "$JAVA_HOME/bin/java.exe" ]]; then
  real_java="$JAVA_HOME/bin/java.exe"
else
  real_java="$(command -v java)"
fi
if [[ -n "${JUMPGATE_TEST_KEYTOOL:-}" ]]; then
  real_keytool="$JUMPGATE_TEST_KEYTOOL"
elif [[ -n "${JAVA_HOME:-}" ]] && [[ -f "$JAVA_HOME/bin/keytool" ]]; then
  real_keytool="$JAVA_HOME/bin/keytool"
elif [[ -n "${JAVA_HOME:-}" ]] && [[ -f "$JAVA_HOME/bin/keytool.exe" ]]; then
  real_keytool="$JAVA_HOME/bin/keytool.exe"
else
  real_keytool="$(command -v keytool)"
fi
test -f "$real_java"
test -f "$real_keytool"

mock_tools="$work_dir/mock-build-tools"
real_tools_adapter="$work_dir/real-build-tools"
readelf_log="$work_dir/readelf.log"
readelf_wrapper="$work_dir/llvm-readelf-wrapper"
mkdir -p "$mock_tools" "$real_tools_adapter"

cat > "$mock_tools/aapt2" <<'MOCK_AAPT2'
#!/usr/bin/env bash
set -euo pipefail
[[ "$#" -eq 3 && "$1" == 'dump' && "$2" == 'badging' ]]
printf "package: name='%s' versionCode='%s' versionName='%s' platformBuildVersionName='16' platformBuildVersionCode='36' compileSdkVersion='36' compileSdkVersionCodename='16'\n" \
  "${MOCK_PACKAGE:-org.xbmc.kodi}" \
  "${MOCK_VERSION_CODE:-2190702}" \
  "${MOCK_VERSION_NAME:-22.0-ALPHA2}"
printf "minSdkVersion:'%s'\n" "${MOCK_MIN_SDK:-24}"
printf "targetSdkVersion:'%s'\n" "${MOCK_TARGET_SDK:-36}"
printf "application: label='Jumpgate CI' icon=''\n"
if [[ "${MOCK_DEBUGGABLE:-false}" == 'true' ]]; then
  printf 'application-debuggable\n'
fi
MOCK_AAPT2

cat > "$mock_tools/apksigner" <<'MOCK_APKSIGNER'
#!/usr/bin/env bash
set -euo pipefail
arguments=" $* "
[[ "$arguments" == *' verify '* ]]
[[ "$arguments" == *' --Werr '* ]]
[[ "$arguments" == *' --verbose '* ]]
[[ "$arguments" == *' --print-certs '* ]]
if [[ "${MOCK_APKSIGNER_FAIL:-false}" == 'true' ]]; then
  exit 1
fi
printf 'Verifies\n'
printf 'Verified using v1 scheme (JAR signing): true\n'
printf 'Verified using v2 scheme (APK Signature Scheme v2): %s\n' "${MOCK_V2:-true}"
printf 'Signer #1 certificate SHA-256 digest: %s\n' \
  "${MOCK_SIGNER_DIGEST:-abababababababababababababababababababababababababababababababab}"
if [[ -n "${MOCK_SECOND_SIGNER_DIGEST:-}" ]]; then
  printf 'Signer #2 certificate SHA-256 digest: %s\n' "$MOCK_SECOND_SIGNER_DIGEST"
fi
MOCK_APKSIGNER

export JUMPGATE_REAL_AAPT2="$real_aapt2"
export JUMPGATE_REAL_JAVA="$real_java"
export JUMPGATE_REAL_APKSIGNER_JAR="$real_apksigner_jar"
export JUMPGATE_REAL_READELF="$real_readelf"
export JUMPGATE_READELF_LOG="$readelf_log"

cat > "$real_tools_adapter/aapt2" <<'REAL_AAPT2'
#!/usr/bin/env bash
set -euo pipefail
exec "$JUMPGATE_REAL_AAPT2" "$@"
REAL_AAPT2

cat > "$real_tools_adapter/apksigner" <<'REAL_APKSIGNER'
#!/usr/bin/env bash
set -euo pipefail
exec "$JUMPGATE_REAL_JAVA" -jar "$JUMPGATE_REAL_APKSIGNER_JAR" "$@"
REAL_APKSIGNER

cat > "$readelf_wrapper" <<'REAL_READELF'
#!/usr/bin/env bash
set -euo pipefail
library="${!#}"
basename "$library" >> "$JUMPGATE_READELF_LOG"
exec "$JUMPGATE_REAL_READELF" "$@"
REAL_READELF

chmod +x "$mock_tools/aapt2" "$mock_tools/apksigner" \
  "$real_tools_adapter/aapt2" "$real_tools_adapter/apksigner" "$readelf_wrapper"

fixture_source="$work_dir/fixture.c"
cat > "$fixture_source" <<'C_SOURCE'
__attribute__((visibility("default"))) int jumpgate_fixture(void)
{
  return 42;
}
C_SOURCE

target_for_abi() {
  case "$1" in
    arm64-v8a) printf 'aarch64-linux-android24\n' ;;
    armeabi-v7a) printf 'armv7a-linux-androideabi24\n' ;;
    *) return 1 ;;
  esac
}

compile_shared() {
  local output="$1"
  local abi="$2"
  local target
  target="$(target_for_abi "$abi")"
  mkdir -p "$(dirname "$output")"
  "$real_clang" "--target=$target" -fPIC -shared -nostdlib \
    -Wl,--build-id=none -Wl,-soname,"$(basename "$output")" \
    "$fixture_source" -o "$output"
}

compile_relocatable() {
  local output="$1"
  local abi="$2"
  local target
  target="$(target_for_abi "$abi")"
  mkdir -p "$(dirname "$output")"
  "$real_clang" "--target=$target" -fPIC -c "$fixture_source" -o "$output"
}

compile_executable() {
  local output="$1"
  local abi="$2"
  local target
  target="$(target_for_abi "$abi")"
  mkdir -p "$(dirname "$output")"
  "$real_clang" "--target=$target" -fPIC -nostdlib -no-pie \
    -Wl,--build-id=none -Wl,-e,jumpgate_fixture "$fixture_source" -o "$output"
}

compile_pie() {
  local output="$1"
  local abi="$2"
  local target
  target="$(target_for_abi "$abi")"
  mkdir -p "$(dirname "$output")"
  "$real_clang" "--target=$target" -fPIC -nostdlib -pie \
    -Wl,--build-id=none -Wl,-e,jumpgate_fixture "$fixture_source" -o "$output"
}

remove_program_header_type() {
  local library="$1"
  local program_header_type="$2"
  python3 - "$library" "$program_header_type" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
target_type = int(sys.argv[2])
data = bytearray(path.read_bytes())
if data[:4] != b'\x7fELF' or data[5] != 1:
    raise SystemExit(1)
elf_class = data[4]
if elf_class == 2:
    header_offset, offset_size, entry_size_offset, count_offset = 32, 8, 54, 56
elif elf_class == 1:
    header_offset, offset_size, entry_size_offset, count_offset = 28, 4, 42, 44
else:
    raise SystemExit(1)
program_offset = int.from_bytes(data[header_offset:header_offset + offset_size], 'little')
entry_size = int.from_bytes(data[entry_size_offset:entry_size_offset + 2], 'little')
entry_count = int.from_bytes(data[count_offset:count_offset + 2], 'little')
if entry_size < 4 or program_offset + entry_size * entry_count > len(data):
    raise SystemExit(1)
changed = 0
for index in range(entry_count):
    offset = program_offset + index * entry_size
    if int.from_bytes(data[offset:offset + 4], 'little') == target_type:
        data[offset:offset + 4] = (0).to_bytes(4, 'little')
        changed += 1
if not changed:
    raise SystemExit(1)
path.write_bytes(data)
PY
}

remove_load_segments() {
  remove_program_header_type "$1" 1
}

remove_interp_segments() {
  remove_program_header_type "$1" 3
}

make_apk() {
  local payload="$1"
  local apk="$2"
  python3 - "$payload" "$apk" <<'PY'
import pathlib
import sys
import zipfile

root = pathlib.Path(sys.argv[1])
apk = pathlib.Path(sys.argv[2])
with zipfile.ZipFile(apk, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(root.rglob('*')):
        if path.is_file():
            archive.write(path, path.relative_to(root).as_posix())
PY
}

copy_fixture() {
  local source="$1"
  local destination="$2"
  mkdir -p "$destination"
  cp -R "$source"/. "$destination"/
}

append_allowed_rsa_key() {
  printf '\0' >> "$1"
  cat "$allowed_rsa_pem" >> "$1"
  printf '\0' >> "$1"
}

remove_allowed_rsa_key() {
  python3 - "$1" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
data = path.read_bytes()
boundary = b'-' * 5
begin = boundary + b'BEGIN RSA PRIVATE KEY' + boundary
end = boundary + b'END RSA PRIVATE KEY' + boundary
start = data.find(begin)
finish = data.find(end, start) + len(end)
if start < 0 or finish < len(end) or data.find(begin, finish) >= 0:
    raise SystemExit(1)
path.write_bytes(data[:start] + data[finish:])
PY
}

mutate_allowed_rsa_key() {
  local path="$1"
  local replacement="$2"
  python3 - "$path" "$replacement" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
replacement = sys.argv[2].encode('ascii')
data = bytearray(path.read_bytes())
boundary = b'-' * 5
begin = boundary + b'BEGIN RSA PRIVATE KEY' + boundary
end = boundary + b'END RSA PRIVATE KEY' + boundary
body_start = data.find(begin) + len(begin)
body_end = data.find(end, body_start)
if body_start < len(begin) or body_end < 0 or len(replacement) != 1:
    raise SystemExit(1)
for index in range(body_start, body_end):
    if data[index] in b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/':
        if data[index] == replacement[0]:
            continue
        data[index] = replacement[0]
        path.write_bytes(data)
        break
else:
    raise SystemExit(1)
PY
}

write_marker_variant() {
  local output="$1"
  local variant="$2"
  python3 - "$output" "$variant" "$allowed_rsa_body" <<'PY'
import pathlib
import sys

output = pathlib.Path(sys.argv[1])
variant = sys.argv[2]
body = pathlib.Path(sys.argv[3]).read_text(encoding='ascii')
boundary = '-' * 5
variants = {
    'ascii': f'{boundary}BEGIN PRIVATE KEY{boundary}\n{body}\n'
             f'{boundary}END PRIVATE KEY{boundary}'.encode(),
    'missing-space': f'{boundary}BEGINRSAPRIVATEKEY{boundary}\n{body}\n'
                     f'{boundary}ENDRSAPRIVATEKEY{boundary}'.encode(),
    'mixed-case': f'{boundary}bEgIn RsA pRiVaTe KeY{boundary}\n{body}\n'
                  f'{boundary}eNd RsA pRiVaTe KeY{boundary}'.encode(),
    'single-hyphen': f'-BEGIN RSA PRIVATE KEY-\n{body}\n-END RSA PRIVATE KEY-'.encode(),
    'nul-control': (
        b'-----B\0E\x01GIN R\0SA PRI\x02VATE K\0EY-----\nMAA=\n'
        b'-----E\0ND R\x03SA PRI\0VATE K\x04EY-----'
    ).replace(b'MAA=', body.encode()),
    'nbsp-utf8': (
        f'{boundary}BEGIN\u00a0RSA\u00a0PRIVATE\u00a0KEY{boundary}\n{body}\n'
        f'{boundary}END\u00a0RSA\u00a0PRIVATE\u00a0KEY{boundary}'
    ).encode('utf-8'),
    'utf16le': (f'\ufeff{boundary}BEGIN RSA PRIVATE KEY{boundary}\n{body}\n'
                f'{boundary}END RSA PRIVATE KEY{boundary}').encode('utf-16le'),
    'utf16be': (f'\ufeff{boundary}BEGIN RSA PRIVATE KEY{boundary}\n{body}\n'
                f'{boundary}END RSA PRIVATE KEY{boundary}').encode('utf-16be'),
}
try:
    payload = variants[variant]
except KeyError:
    raise SystemExit(1)
output.write_bytes(b'\0' + payload + b'\0')
PY
}

write_private_armor() {
  local output="$1"
  local label="$2"
  local body_file="$3"
  python3 - "$output" "$label" "$body_file" <<'PY'
import base64
import pathlib
import sys

output = pathlib.Path(sys.argv[1])
label = sys.argv[2]
body = pathlib.Path(sys.argv[3]).read_bytes()
try:
    body.decode('ascii')
    encoded = body
except UnicodeDecodeError:
    encoded = base64.b64encode(body)
lines = [encoded[index:index + 64] for index in range(0, len(encoded), 64)]
output.write_bytes(b'-----BEGIN ' + label.encode() + b'-----\n' +
                   b'\n'.join(lines) + b'\n-----END ' + label.encode() + b'-----\n')
PY
}

write_named_assignment_variant() {
  local output="$1"
  local variant="$2"
  python3 - "$output" "$variant" <<'PY'
import pathlib
import sys

output = pathlib.Path(sys.argv[1])
variant = sys.argv[2]
values = {
    'spaced-quoted': 'KODI_ANDROID_STORE_PASSWORD = "a b c d e f g h"\n'.encode(),
    'utf16le': ('\ufeffJUMPGATE_ENCRYPTION_KEY : "little endian secret value"\n'
                ).encode('utf-16le'),
    'utf16be': ('\ufeffKODI_ANDROID_KEY_PASSWORD = \'big endian secret value\'\n'
                ).encode('utf-16be'),
    'quoted-key': '"FLY_API_TOKEN" = "quoted key secret value"\n'.encode(),
    'append': 'FLY_API_TOKEN += "appended live secret value"\n'.encode(),
    'quoted-suffix': 'FLY_API_TOKEN="REDACTED" trailing-live-secret\n'.encode(),
    'sentinel-prefix': 'JUMPGATE_ENCRYPTION_KEY=EXAMPLE_live_secret\n'.encode(),
    'continuation': ('JUMPGATE_ENCRYPTION_KEY="continued shell \\\n+secret value"\n').encode(),
}
try:
    payload = values[variant]
except KeyError:
    raise SystemExit(1)
output.write_bytes(payload)
PY
}

verify_apk() {
  local apk="$1"
  local abi="$2"
  shift 2
  env "$@" \
    ANDROID_BUILD_TOOLS_ROOT="$mock_tools" \
    JUMPGATE_READELF_BIN="$readelf_wrapper" \
    bash "$verifier" \
      "$apk" \
      "$abi" \
      "$expected_package" \
      "$expected_min_sdk" \
      "$expected_target_sdk" \
      "$expected_version_name" \
      "$expected_version_code" \
      "$mock_signer_sha256"
}

verify_real_apk() {
  local apk="$1"
  local abi="$2"
  local signer_sha256="$3"
  env \
    ANDROID_BUILD_TOOLS_ROOT="$real_tools_adapter" \
    JUMPGATE_READELF_BIN="$readelf_wrapper" \
    bash "$verifier" \
      "$apk" \
      "$abi" \
      "$expected_package" \
      "$expected_min_sdk" \
      "$expected_target_sdk" \
      "$expected_version_name" \
      "$expected_version_code" \
      "$signer_sha256"
}

expect_failure() {
  local label="$1"
  local apk="$2"
  local abi="$3"
  shift 3
  if verify_apk "$apk" "$abi" "$@" >/dev/null 2>&1; then
    echo "Expected verifier failure: $label" >&2
    exit 1
  fi
}

expect_failure_reason() {
  local label="$1"
  local apk="$2"
  local abi="$3"
  local expected_reason="$4"
  local output status
  shift 4
  set +e
  output="$(verify_apk "$apk" "$abi" "$@" 2>&1)"
  status="$?"
  set -e
  if [[ "$status" -eq 0 ]]; then
    echo "Expected verifier failure: $label" >&2
    exit 1
  fi
  if [[ "$output" != *"$expected_reason"* ]]; then
    echo "Verifier failed for an unexpected reason: $label" >&2
    exit 1
  fi
}

expect_failure_without_value() {
  local label="$1"
  local apk="$2"
  local abi="$3"
  local forbidden_value="$4"
  local output status
  shift 4
  set +e
  output="$(verify_apk "$apk" "$abi" "$@" 2>&1)"
  status="$?"
  set -e
  if [[ "$status" -eq 0 ]]; then
    echo "Expected verifier failure: $label" >&2
    exit 1
  fi
  if [[ "$output" == *"$forbidden_value"* ]]; then
    echo "Verifier disclosed a matched secret value: $label" >&2
    exit 1
  fi
}

entry_sha256() {
  local digest
  digest="$(printf '%s' "$1" | sha256sum)"
  digest="${digest%% *}"
  [[ "$digest" =~ ^[0-9a-f]{64}$ ]]
  printf '%s\n' "$digest"
}

expect_failure_diagnostic() {
  local label="$1"
  local apk="$2"
  local abi="$3"
  local expected_entry="$4"
  local expected_phase="$5"
  local output status diagnostic expected_entry_sha256 entry_name
  shift 5
  set +e
  output="$(verify_apk "$apk" "$abi" "$@" 2>&1)"
  status="$?"
  set -e
  if [[ "$status" -eq 0 ]]; then
    echo "Expected verifier failure: $label" >&2
    exit 1
  fi
  expected_entry_sha256="$(entry_sha256 "$expected_entry")"
  diagnostic="JUMPGATE_APK_SCAN_REJECT {\"entry_sha256\":\"$expected_entry_sha256\",\"phase\":\"$expected_phase\"}"
  if [[ "$output" != *"$diagnostic"* ]]; then
    echo "Verifier omitted the sanitized finding diagnostic: $label" >&2
    exit 1
  fi
  entry_name="${expected_entry##*/}"
  if [[ "$output" == *"$expected_entry"* || "$output" == *"$entry_name"* ]]; then
    echo "Verifier disclosed a scanned archive entry path: $label" >&2
    exit 1
  fi
}

expect_failure_diagnostic_without_value() {
  local label="$1"
  local apk="$2"
  local abi="$3"
  local expected_entry="$4"
  local expected_phase="$5"
  local forbidden_value="$6"
  local output status diagnostic expected_entry_sha256 entry_name
  shift 6
  set +e
  output="$(verify_apk "$apk" "$abi" "$@" 2>&1)"
  status="$?"
  set -e
  if [[ "$status" -eq 0 ]]; then
    echo "Expected verifier failure: $label" >&2
    exit 1
  fi
  expected_entry_sha256="$(entry_sha256 "$expected_entry")"
  diagnostic="JUMPGATE_APK_SCAN_REJECT {\"entry_sha256\":\"$expected_entry_sha256\",\"phase\":\"$expected_phase\"}"
  if [[ "$output" != *"$diagnostic"* ]]; then
    echo "Verifier omitted the sanitized finding diagnostic: $label" >&2
    exit 1
  fi
  entry_name="${expected_entry##*/}"
  if [[ "$output" == *"$expected_entry"* || "$output" == *"$entry_name"* ]]; then
    echo "Verifier disclosed a scanned archive entry path: $label" >&2
    exit 1
  fi
  if [[ "$output" == *"$forbidden_value"* ]]; then
    echo "Verifier disclosed a matched secret value: $label" >&2
    exit 1
  fi
}

expect_failure_without_diagnostic() {
  local label="$1"
  local apk="$2"
  local abi="$3"
  local expected_reason="$4"
  local forbidden_detail="$5"
  local forbidden_entry="$6"
  local output status entry_name
  shift 6
  set +e
  output="$(verify_apk "$apk" "$abi" "$@" 2>&1)"
  status="$?"
  set -e
  if [[ "$status" -eq 0 ]]; then
    echo "Expected verifier failure: $label" >&2
    exit 1
  fi
  if ! grep -Fqx -- "$expected_reason" <<< "$output"; then
    echo "Verifier omitted the exact outer failure: $label" >&2
    exit 1
  fi
  if [[ "$output" == *'JUMPGATE_APK_SCAN_REJECT '* ]]; then
    echo "Verifier emitted a finding diagnostic for a scanner error: $label" >&2
    exit 1
  fi
  if [[ "$output" == *'Traceback (most recent call last)'* ]]; then
    echo "Verifier disclosed a scanner traceback: $label" >&2
    exit 1
  fi
  if [[ -n "$forbidden_detail" && "$output" == *"$forbidden_detail"* ]]; then
    echo "Verifier disclosed scanner exception text: $label" >&2
    exit 1
  fi
  if [[ -n "$forbidden_entry" ]]; then
    entry_name="${forbidden_entry##*/}"
    if [[ "$output" == *"$forbidden_entry"* || "$output" == *"$entry_name"* ]]; then
      echo "Verifier disclosed a scanner-error archive entry path: $label" >&2
      exit 1
    fi
  fi
}

base_arm64="$work_dir/base-arm64"
base_armv7="$work_dir/base-armv7"
allowed_rsa_pem="$work_dir/allowed-rsa-key.pem"
allowed_rsa_der="$work_dir/allowed-rsa-key.der"
allowed_rsa_body="$work_dir/allowed-rsa-key.b64"
airtunes_source="$script_dir/../../../xbmc/network/AirTunesServer.cpp"
python3 - \
  "$airtunes_source" \
  "$allowed_rsa_pem" \
  "$allowed_rsa_der" \
  "$allowed_rsa_body" \
  "$allowed_rsa_der_sha256" <<'PY'
import base64
import hashlib
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_bytes()
pem_output = pathlib.Path(sys.argv[2])
der_output = pathlib.Path(sys.argv[3])
body_output = pathlib.Path(sys.argv[4])
expected_digest = sys.argv[5]
boundary = b'-' * 5
begin = boundary + b'BEGIN RSA PRIVATE KEY' + boundary
end = boundary + b'END RSA PRIVATE KEY' + boundary
start = source.find(begin)
finish = source.find(end, start) + len(end)
if start < 0 or finish < len(end) or source.find(begin, finish) >= 0:
    raise SystemExit(1)
pem = source[start:finish].replace(b'\\\r\n', b'').replace(b'\\\n', b'')
match = re.fullmatch(
    begin + rb'(?P<body>[A-Za-z0-9+/=]+)' + end,
    pem,
)
if not match:
    raise SystemExit(1)
der = base64.b64decode(match.group('body'), validate=True)
if hashlib.sha256(der).hexdigest() != expected_digest:
    raise SystemExit(1)
pem_output.write_bytes(pem)
der_output.write_bytes(der)
body_output.write_bytes(match.group('body'))
PY
private_format_dir="$work_dir/private-formats"
mkdir -p "$private_format_dir"
python3 - "$allowed_rsa_der" "$private_format_dir" <<'PY'
import pathlib
import sys

allowed_rsa = pathlib.Path(sys.argv[1]).read_bytes()
root = pathlib.Path(sys.argv[2])


def length(value: int) -> bytes:
    if value < 0x80:
        return bytes([value])
    encoded = value.to_bytes((value.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(encoded)]) + encoded


def tlv(tag: int, value: bytes) -> bytes:
    return bytes([tag]) + length(len(value)) + value


def ssh_string(value: bytes) -> bytes:
    return len(value).to_bytes(4, 'big') + value


rsa_algorithm = tlv(0x30, tlv(0x06, bytes.fromhex('2a864886f70d010101')) + b'\x05\x00')
pkcs8 = tlv(0x30, b'\x02\x01\x00' + rsa_algorithm + tlv(0x04, allowed_rsa))
dh_parameters = tlv(0x30, tlv(0x02, b'\x01' * 64) + tlv(0x02, b'\x02'))
dh_algorithm = tlv(
    0x30,
    tlv(0x06, bytes.fromhex('2a864886f70d010301')) + dh_parameters,
)
dh_pkcs8 = tlv(
    0x30,
    b'\x02\x01\x00' + dh_algorithm + tlv(0x04, tlv(0x02, b'\x07')),
)
compact_pkcs8 = tlv(
    0x30,
    b'\x02\x01\x00' + tlv(0x30, tlv(0x06, b'\x2a')) + tlv(0x04, b'\x01'),
)
pkcs8_public_control = tlv(
    0x30,
    b'\x02\x01\x00' + dh_algorithm + tlv(0x03, b'\x00public-key-bits'),
)
sec1 = tlv(0x30, b'\x02\x01\x01' + tlv(0x04, bytes(range(1, 33))))
dsa_values = [b'\x00', b'\x01' * 64, b'\x02' * 20, b'\x03' * 64,
              b'\x04' * 64, b'\x05' * 20]
dsa = tlv(0x30, b''.join(tlv(0x02, value) for value in dsa_values))
openssh = (
    b'openssh-key-v1\0'
    + ssh_string(b'none')
    + ssh_string(b'none')
    + ssh_string(b'')
    + (1).to_bytes(4, 'big')
    + ssh_string(b'public-key-fixture')
    + ssh_string(b'private-key-fixture-data')
)

for name, value in {
    'pkcs8.der': pkcs8,
    'dh-pkcs8.der': dh_pkcs8,
    'compact-pkcs8.der': compact_pkcs8,
    'pkcs8-public-control.der': pkcs8_public_control,
    'sec1.der': sec1,
    'dsa.der': dsa,
    'openssh.bin': openssh,
}.items():
    (root / name).write_bytes(value)
PY
compile_shared "$base_arm64/lib/arm64-v8a/libkodi.so" arm64-v8a
compile_shared "$base_arm64/lib/arm64-v8a/libhelper.so" arm64-v8a
compile_shared "$base_armv7/lib/armeabi-v7a/libkodi.so" armeabi-v7a
compile_shared "$base_armv7/lib/armeabi-v7a/libhelper.so" armeabi-v7a
append_allowed_rsa_key "$base_arm64/lib/arm64-v8a/libkodi.so"
append_allowed_rsa_key "$base_armv7/lib/armeabi-v7a/libkodi.so"
mkdir -p "$base_arm64/assets" "$base_armv7/assets"
cat > "$base_arm64/assets/placeholders.json" <<'JSON'
{"auth":{"accessToken":"${JUMPGATE_ACCESS_TOKEN}"},"api_key":"YOUR_API_KEY","client_secret":"REDACTED","password":"","oauth_metadata":{"token_type":"Bearer","token_endpoint":"https://example.com/oauth/token","token_expiry":3600}}
JSON
cat > "$base_arm64/assets/placeholders.yaml" <<'YAML'
"refresh_token": "{{ JUMPGATE_REFRESH_TOKEN }}"
encryption-key: EXAMPLE_ENCRYPTION_KEY
oauth_metadata: {token_type: Bearer, token_endpoint: "https://example.org/oauth/token", token_expiry: 3600}
YAML
cat > "$base_arm64/assets/placeholders.properties" <<'PROPERTIES'
client.secret=REPLACE_ME
PROPERTIES
cat > "$base_arm64/assets/.env.example" <<'ENV_FILE'
API_KEY=${JUMPGATE_API_KEY}
ENV_FILE
cat > "$base_arm64/assets/secret-scan-controls.dat" <<'CONTROLS'
KODI_ANDROID_STORE_PASSWORD is injected by CI
KODI_ANDROID_STORE_PASSWORD_LENGTH=32
FLY_API_TOKEN is an environment variable name
"KODI_ANDROID_STORE_PASSWORD" = "${KODI_ANDROID_STORE_PASSWORD}"
export FLY_API_TOKEN=$FLY_API_TOKEN
JUMPGATE_ENCRYPTION_KEY += "${JUMPGATE_ENCRYPTION_KEY}" # injected by CI
CONTROLS
python3 - "$base_arm64/assets/cryptodome-ecc-parser-constants.bin" <<'PY'
import pathlib
import sys

# Representative parser constants contain labels but no plausible key body.
boundary = b'-' * 5
constants = (
    boundary + b'BEGIN PRIVATE KEY' + boundary + b'\0' +
    boundary + b'END PRIVATE KEY' + boundary + b'\0' +
    boundary + b'BEGIN EC PRIVATE KEY' + boundary + b'\0' +
    boundary + b'END EC PRIVATE KEY' + boundary + b'\0' +
    boundary + b'BEGIN PUBLIC KEY' + boundary + b'\0' +
    boundary + b'END PUBLIC KEY' + boundary + b'\0'
)
pathlib.Path(sys.argv[1]).write_bytes(constants)
PY
cat > "$base_arm64/assets/public-jwk-controls.json" <<'JSON'
{"keys":[{"kty":"RSA","n":"public-modulus","e":"AQAB"},{"kty":"EC","crv":"P-256","x":"public-x","y":"public-y"},{"kty":"OKP","crv":"Ed25519","x":"public-x"}],"unrelated":{"kty":"custom","d":"description"}}
JSON
cp "$private_format_dir/pkcs8-public-control.der" \
  "$base_arm64/assets/pkcs8-public-control.der"
printf 'safe path component\n' > "$base_arm64/assets/stream_secret.txt"
cp -R "$base_arm64/assets"/. "$base_armv7/assets"/

arm64_apk="$work_dir/valid-arm64.apk"
armv7_apk="$work_dir/valid-armv7.apk"
make_apk "$base_arm64" "$arm64_apk"
make_apk "$base_armv7" "$armv7_apk"

: > "$readelf_log"
verify_apk "$arm64_apk" arm64-v8a >/dev/null
[[ "$(wc -l < "$readelf_log")" -eq 2 ]]
grep -Fxq 'libhelper.so' "$readelf_log"
grep -Fxq 'libkodi.so' "$readelf_log"
test -s "$arm64_apk.sha256"

: > "$readelf_log"
verify_apk "$armv7_apk" armeabi-v7a >/dev/null
[[ "$(wc -l < "$readelf_log")" -eq 2 ]]
grep -Fxq 'libhelper.so' "$readelf_log"
grep -Fxq 'libkodi.so' "$readelf_log"
test -s "$armv7_apk.sha256"

benign_cryptodome_runtime="$work_dir/benign-cryptodome-runtime"
copy_fixture "$base_arm64" "$benign_cryptodome_runtime"
benign_cryptodome_root="$benign_cryptodome_runtime/assets/python3.11/lib/python3.11/site-packages/Cryptodome"
mkdir -p "$benign_cryptodome_root/Cipher/__pycache__"
printf 'benign sibling runtime module\n' > "$benign_cryptodome_root/Cipher/AES.py"
printf 'benign sibling bytecode\n' > \
  "$benign_cryptodome_root/Cipher/__pycache__/AES.cpython-311.pyc"
benign_cryptodome_apk="$work_dir/benign-cryptodome-runtime.apk"
make_apk "$benign_cryptodome_runtime" "$benign_cryptodome_apk"
verify_apk "$benign_cryptodome_apk" arm64-v8a >/dev/null

selftest_py="$work_dir/cryptodome-selftest-py"
copy_fixture "$base_arm64" "$selftest_py"
selftest_py_entry='assets/python3.11/lib/python3.11/site-packages/Cryptodome/SelfTest/Cipher/test_AES.py'
mkdir -p "$selftest_py/$(dirname "$selftest_py_entry")"
printf 'SELFTEST_PY_CONTENT_MUST_NOT_BE_PRINTED\n' > "$selftest_py/$selftest_py_entry"
selftest_py_apk="$work_dir/cryptodome-selftest-py.apk"
make_apk "$selftest_py" "$selftest_py_apk"
expect_failure_without_diagnostic \
  cryptodome-selftest-py \
  "$selftest_py_apk" \
  arm64-v8a \
  'APK contains a forbidden Cryptodome SelfTest artifact' \
  'SELFTEST_PY_CONTENT_MUST_NOT_BE_PRINTED' \
  "$selftest_py_entry"

selftest_pyc="$work_dir/cryptodome-selftest-pyc"
copy_fixture "$base_arm64" "$selftest_pyc"
selftest_pyc_entry='assets/python3.11/lib/python3.11/site-packages/Cryptodome/SelfTest/__pycache__/loader.cpython-311.pyc'
mkdir -p "$selftest_pyc/$(dirname "$selftest_pyc_entry")"
printf 'SELFTEST_PYC_CONTENT_MUST_NOT_BE_PRINTED\n' > "$selftest_pyc/$selftest_pyc_entry"
selftest_pyc_apk="$work_dir/cryptodome-selftest-pyc.apk"
make_apk "$selftest_pyc" "$selftest_pyc_apk"
expect_failure_without_diagnostic \
  cryptodome-selftest-pyc \
  "$selftest_pyc_apk" \
  arm64-v8a \
  'APK contains a forbidden Cryptodome SelfTest artifact' \
  'SELFTEST_PYC_CONTENT_MUST_NOT_BE_PRINTED' \
  "$selftest_pyc_entry"

flattened_selftest_native="$work_dir/cryptodome-flattened-selftest-native"
copy_fixture "$base_arm64" "$flattened_selftest_native"
flattened_selftest_native_entry='lib/arm64-v8a/LiBCryptodome_SeLfTeSt_Cipher_native.so'
printf 'FLATTENED_SELFTEST_NATIVE_CONTENT_MUST_NOT_BE_PRINTED\n' > \
  "$flattened_selftest_native/$flattened_selftest_native_entry"
flattened_selftest_native_apk="$work_dir/cryptodome-flattened-selftest-native.apk"
make_apk "$flattened_selftest_native" "$flattened_selftest_native_apk"
expect_failure_without_diagnostic \
  cryptodome-flattened-selftest-native \
  "$flattened_selftest_native_apk" \
  arm64-v8a \
  'APK contains a forbidden Cryptodome SelfTest artifact' \
  'FLATTENED_SELFTEST_NATIVE_CONTENT_MUST_NOT_BE_PRINTED' \
  "$flattened_selftest_native_entry"

der_noise="$work_dir/der-parser-noise"
copy_fixture "$base_arm64" "$der_noise"
python3 - "$der_noise/assets/cryptodome-der-parser-noise.bin" <<'PY'
import pathlib
import sys

# ASN.1-heavy parser/runtime binaries may exceed the global semantic-key budget;
# only DER SEQUENCEs beginning with a private-key version are charged to it.
pathlib.Path(sys.argv[1]).write_bytes((b'\x30\x18' + b'parser-constant-data-000') * 500_001)
PY
der_noise_apk="$work_dir/der-parser-noise.apk"
make_apk "$der_noise" "$der_noise_apk"
verify_apk "$der_noise_apk" arm64-v8a >/dev/null

manifest="$work_dir/AndroidManifest.xml"
cat > "$manifest" <<'MANIFEST'
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="org.xbmc.kodi"
    android:versionCode="2190702"
    android:versionName="22.0-ALPHA2">
    <uses-sdk android:minSdkVersion="24" android:targetSdkVersion="36" />
    <application android:label="Jumpgate CI" android:extractNativeLibs="true" />
</manifest>
MANIFEST

test_keystore="$work_dir/test-signing.p12"
test_certificate="$work_dir/test-signing.cer"
export JUMPGATE_TEST_STORE_PASSWORD='DUMMY_JUMPGATE_CI_FIXTURE_PASSWORD'
"$real_keytool" -genkeypair \
  -keystore "$test_keystore" \
  -storetype PKCS12 \
  -alias jumpgate-ci \
  -dname 'CN=Jumpgate CI Fixture,O=Jumpgate,C=NL' \
  -keyalg RSA \
  -keysize 2048 \
  -validity 2 \
  -storepass "$JUMPGATE_TEST_STORE_PASSWORD" \
  -keypass "$JUMPGATE_TEST_STORE_PASSWORD" \
  -noprompt >/dev/null 2>&1
"$real_keytool" -exportcert \
  -keystore "$test_keystore" \
  -storetype PKCS12 \
  -alias jumpgate-ci \
  -storepass "$JUMPGATE_TEST_STORE_PASSWORD" \
  -file "$test_certificate" >/dev/null 2>&1
real_signer_sha256="$(sha256sum "$test_certificate" | cut -d ' ' -f 1)"
[[ "$real_signer_sha256" =~ ^[0-9a-f]{64}$ ]]

make_real_apk() {
  local payload="$1"
  local abi="$2"
  local output="$3"
  local unsigned_apk="$work_dir/real-$abi-unsigned.apk"
  local aligned_apk="$work_dir/real-$abi-aligned.apk"
  local badging="$work_dir/real-$abi-badging.txt"

  "$real_aapt2" link \
    -o "$unsigned_apk" \
    -I "$android_jar" \
    --manifest "$manifest"
  python3 - "$payload" "$unsigned_apk" <<'PY'
import pathlib
import sys
import zipfile

root = pathlib.Path(sys.argv[1])
apk = pathlib.Path(sys.argv[2])
with zipfile.ZipFile(apk, 'a', compression=zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(root.rglob('*')):
        if path.is_file():
            archive.write(path, path.relative_to(root).as_posix())
PY
  "$real_zipalign" -f -p 4 "$unsigned_apk" "$aligned_apk"
  "$real_java" -jar "$real_apksigner_jar" sign \
    --ks "$test_keystore" \
    --ks-key-alias jumpgate-ci \
    --ks-pass env:JUMPGATE_TEST_STORE_PASSWORD \
    --key-pass env:JUMPGATE_TEST_STORE_PASSWORD \
    --v1-signing-enabled true \
    --v2-signing-enabled true \
    --out "$output" \
    "$aligned_apk" >/dev/null 2>&1
  "$real_aapt2" dump badging "$output" | tr -d '\r' > "$badging"
  grep -Fxq "minSdkVersion:'24'" "$badging"
  grep -Fxq "targetSdkVersion:'36'" "$badging"
  verify_real_apk "$output" "$abi" "$real_signer_sha256" >/dev/null
  test -s "$output.sha256"
}

make_real_apk "$base_arm64" arm64-v8a "$work_dir/real-arm64.apk"
make_real_apk "$base_armv7" armeabi-v7a "$work_dir/real-armv7.apk"

no_libraries="$work_dir/no-libraries"
mkdir -p "$no_libraries/assets"
printf 'fixture\n' > "$no_libraries/assets/file.txt"
no_libraries_apk="$work_dir/no-libraries.apk"
make_apk "$no_libraries" "$no_libraries_apk"
expect_failure no-libraries "$no_libraries_apk" arm64-v8a

spoof_library="$work_dir/spoof-library"
mkdir -p "$spoof_library/lib/arm64-v8a/libkodi.so"
printf 'not a library\n' > "$spoof_library/lib/arm64-v8a/libkodi.so/file.txt"
spoof_library_apk="$work_dir/spoof-library.apk"
make_apk "$spoof_library" "$spoof_library_apk"
expect_failure spoof-library "$spoof_library_apk" arm64-v8a

wrong_architecture="$work_dir/wrong-architecture"
copy_fixture "$base_arm64" "$wrong_architecture"
cp "$base_armv7/lib/armeabi-v7a/libhelper.so" \
  "$wrong_architecture/lib/arm64-v8a/libwrong-abi.so"
wrong_architecture_apk="$work_dir/wrong-architecture.apk"
make_apk "$wrong_architecture" "$wrong_architecture_apk"
expect_failure wrong-architecture "$wrong_architecture_apk" arm64-v8a

extra_abi="$work_dir/extra-abi"
copy_fixture "$base_arm64" "$extra_abi"
mkdir -p "$extra_abi/lib/armeabi-v7a"
cp "$base_armv7/lib/armeabi-v7a/libhelper.so" "$extra_abi/lib/armeabi-v7a/"
extra_abi_apk="$work_dir/extra-abi.apk"
make_apk "$extra_abi" "$extra_abi_apk"
expect_failure extra-abi "$extra_abi_apk" arm64-v8a

for abi in arm64-v8a armeabi-v7a; do
  if [[ "$abi" == 'arm64-v8a' ]]; then
    base_fixture="$base_arm64"
  else
    base_fixture="$base_armv7"
  fi

  et_rel="$work_dir/et-rel-$abi"
  copy_fixture "$base_fixture" "$et_rel"
  compile_relocatable "$et_rel/lib/$abi/libhelper.so" "$abi"
  et_rel_apk="$work_dir/et-rel-$abi.apk"
  make_apk "$et_rel" "$et_rel_apk"
  expect_failure "et-rel-$abi" "$et_rel_apk" "$abi"

  et_exec="$work_dir/et-exec-$abi"
  copy_fixture "$base_fixture" "$et_exec"
  compile_executable "$et_exec/lib/$abi/libhelper.so" "$abi"
  et_exec_apk="$work_dir/et-exec-$abi.apk"
  make_apk "$et_exec" "$et_exec_apk"
  expect_failure "et-exec-$abi" "$et_exec_apk" "$abi"

  pie="$work_dir/pie-$abi"
  copy_fixture "$base_fixture" "$pie"
  compile_pie "$pie/lib/$abi/libhelper.so" "$abi"
  pie_readelf="$("$real_readelf" -h -l -d -W "$pie/lib/$abi/libhelper.so")"
  grep -Eq '^[[:space:]]*Type:[[:space:]]+DYN([[:space:]]|$)' <<< "$pie_readelf"
  grep -Eq '^[[:space:]]*INTERP[[:space:]]' <<< "$pie_readelf"
  grep -Eq '\(FLAGS_1\).*[[:space:]]PIE([[:space:]]|$)' <<< "$pie_readelf"
  pie_apk="$work_dir/pie-$abi.apk"
  make_apk "$pie" "$pie_apk"
  expect_failure_reason "pie-$abi" "$pie_apk" "$abi" 'PT_INTERP'

  df1_pie="$work_dir/df1-pie-$abi"
  copy_fixture "$base_fixture" "$df1_pie"
  compile_pie "$df1_pie/lib/$abi/libhelper.so" "$abi"
  remove_interp_segments "$df1_pie/lib/$abi/libhelper.so"
  df1_pie_readelf="$("$real_readelf" -h -l -d -W "$df1_pie/lib/$abi/libhelper.so")"
  grep -Eq '^[[:space:]]*Type:[[:space:]]+DYN([[:space:]]|$)' <<< "$df1_pie_readelf"
  if grep -Eq '^[[:space:]]*INTERP[[:space:]]' <<< "$df1_pie_readelf"; then
    echo "DF_1_PIE-only fixture still contains PT_INTERP: $abi" >&2
    exit 1
  fi
  grep -Eq '\(FLAGS_1\).*[[:space:]]PIE([[:space:]]|$)' <<< "$df1_pie_readelf"
  grep -Eq '^[[:space:]]*LOAD[[:space:]]' <<< "$df1_pie_readelf"
  df1_pie_apk="$work_dir/df1-pie-$abi.apk"
  make_apk "$df1_pie" "$df1_pie_apk"
  expect_failure_reason "df1-pie-$abi" "$df1_pie_apk" "$abi" 'DF_1_PIE'

  truncated="$work_dir/truncated-$abi"
  copy_fixture "$base_fixture" "$truncated"
  head -c 128 "$base_fixture/lib/$abi/libhelper.so" > \
    "$truncated/lib/$abi/libhelper.so"
  truncated_apk="$work_dir/truncated-$abi.apk"
  make_apk "$truncated" "$truncated_apk"
  expect_failure "truncated-$abi" "$truncated_apk" "$abi"

  no_load="$work_dir/no-load-$abi"
  copy_fixture "$base_fixture" "$no_load"
  remove_load_segments "$no_load/lib/$abi/libhelper.so"
  no_load_apk="$work_dir/no-load-$abi.apk"
  make_apk "$no_load" "$no_load_apk"
  expect_failure "no-load-$abi" "$no_load_apk" "$abi"
done

outside_abi="$work_dir/outside-abi"
copy_fixture "$base_arm64" "$outside_abi"
cp "$base_arm64/lib/arm64-v8a/libhelper.so" "$outside_abi/assets/rogue.so"
outside_abi_apk="$work_dir/outside-abi.apk"
make_apk "$outside_abi" "$outside_abi_apk"
expect_failure outside-abi "$outside_abi_apk" arm64-v8a

non_library="$work_dir/non-library"
copy_fixture "$base_arm64" "$non_library"
printf 'unexpected\n' > "$non_library/lib/arm64-v8a/README.txt"
non_library_apk="$work_dir/non-library.apk"
make_apk "$non_library" "$non_library_apk"
expect_failure non-library "$non_library_apk" arm64-v8a

bundled_shairplay="$work_dir/bundled-shairplay"
copy_fixture "$base_arm64" "$bundled_shairplay"
cp "$base_arm64/lib/arm64-v8a/libhelper.so" \
  "$bundled_shairplay/lib/arm64-v8a/libshairplay.so"
bundled_shairplay_apk="$work_dir/bundled-shairplay.apk"
make_apk "$bundled_shairplay" "$bundled_shairplay_apk"
expect_failure_reason \
  bundled-shairplay \
  "$bundled_shairplay_apk" \
  arm64-v8a \
  'must not bundle Shairplay'

missing_core="$work_dir/missing-core"
copy_fixture "$base_arm64" "$missing_core"
rm "$missing_core/lib/arm64-v8a/libkodi.so"
missing_core_apk="$work_dir/missing-core.apk"
make_apk "$missing_core" "$missing_core_apk"
expect_failure missing-core "$missing_core_apk" arm64-v8a

mutated_rsa_key="$work_dir/mutated-rsa-key"
copy_fixture "$base_arm64" "$mutated_rsa_key"
mutate_allowed_rsa_key "$mutated_rsa_key/lib/arm64-v8a/libkodi.so" A
mutated_rsa_key_apk="$work_dir/mutated-rsa-key.apk"
make_apk "$mutated_rsa_key" "$mutated_rsa_key_apk"
expect_failure_reason mutated-rsa-key "$mutated_rsa_key_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

raw_der_asset="$work_dir/raw-der-asset"
copy_fixture "$base_arm64" "$raw_der_asset"
cp "$allowed_rsa_der" "$raw_der_asset/assets/copied-key.der"
raw_der_asset_apk="$work_dir/raw-der-asset.apk"
make_apk "$raw_der_asset" "$raw_der_asset_apk"
expect_failure_diagnostic raw-der-asset "$raw_der_asset_apk" arm64-v8a \
  'assets/copied-key.der' 'raw-private-format'

raw_der_expected_lib="$work_dir/raw-der-expected-lib"
copy_fixture "$base_arm64" "$raw_der_expected_lib"
cat "$allowed_rsa_der" >> "$raw_der_expected_lib/lib/arm64-v8a/libkodi.so"
raw_der_expected_lib_apk="$work_dir/raw-der-expected-lib.apk"
make_apk "$raw_der_expected_lib" "$raw_der_expected_lib_apk"
expect_failure_reason raw-der-expected-lib "$raw_der_expected_lib_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

for private_format in \
  pkcs8.der dh-pkcs8.der compact-pkcs8.der sec1.der dsa.der openssh.bin; do
  private_format_label="${private_format//./-}"
  private_format_fixture="$work_dir/raw-$private_format_label"
  copy_fixture "$base_arm64" "$private_format_fixture"
  cp "$private_format_dir/$private_format" \
    "$private_format_fixture/assets/$private_format"
  private_format_apk="$work_dir/raw-$private_format_label.apk"
  make_apk "$private_format_fixture" "$private_format_apk"
  expect_failure_reason \
    "raw-$private_format_label" \
    "$private_format_apk" \
    arm64-v8a \
    'private signing, deployment, or runtime secret material'
done

base64url_pkcs8="$work_dir/base64url-pkcs8"
copy_fixture "$base_arm64" "$base64url_pkcs8"
python3 - "$private_format_dir/pkcs8.der" "$base64url_pkcs8/assets/key.txt" <<'PY'
import base64
import pathlib
import sys

encoded = base64.urlsafe_b64encode(pathlib.Path(sys.argv[1]).read_bytes()).rstrip(b'=')
pathlib.Path(sys.argv[2]).write_bytes(b' \n'.join(
    encoded[index:index + 17] for index in range(0, len(encoded), 17)
))
PY
base64url_pkcs8_apk="$work_dir/base64url-pkcs8.apk"
make_apk "$base64url_pkcs8" "$base64url_pkcs8_apk"
expect_failure_reason base64url-pkcs8 "$base64url_pkcs8_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

openssh_armor="$work_dir/openssh-armor"
copy_fixture "$base_arm64" "$openssh_armor"
write_private_armor \
  "$openssh_armor/assets/id_test" \
  'OPENSSH PRIVATE KEY' \
  "$private_format_dir/openssh.bin"
openssh_armor_apk="$work_dir/openssh-armor.apk"
make_apk "$openssh_armor" "$openssh_armor_apk"
expect_failure_reason openssh-armor "$openssh_armor_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

semantic_base64_copy="$work_dir/semantic-base64-copy"
copy_fixture "$base_arm64" "$semantic_base64_copy"
python3 - "$allowed_rsa_body" "$semantic_base64_copy/assets/copied-key.bin" <<'PY'
import pathlib
import sys

body = pathlib.Path(sys.argv[1]).read_bytes()
pathlib.Path(sys.argv[2]).write_bytes(b' \n'.join(bytes([value]) for value in body))
PY
semantic_base64_copy_apk="$work_dir/semantic-base64-copy.apk"
make_apk "$semantic_base64_copy" "$semantic_base64_copy_apk"
expect_failure_reason semantic-base64-copy "$semantic_base64_copy_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

wrong_path_rsa_key="$work_dir/wrong-path-rsa-key"
copy_fixture "$base_arm64" "$wrong_path_rsa_key"
remove_allowed_rsa_key "$wrong_path_rsa_key/lib/arm64-v8a/libkodi.so"
append_allowed_rsa_key "$wrong_path_rsa_key/assets/copied-key.pem"
wrong_path_rsa_key_apk="$work_dir/wrong-path-rsa-key.apk"
make_apk "$wrong_path_rsa_key" "$wrong_path_rsa_key_apk"
expect_failure_reason wrong-path-rsa-key "$wrong_path_rsa_key_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

cross_file_duplicate="$work_dir/cross-file-duplicate"
copy_fixture "$base_arm64" "$cross_file_duplicate"
append_allowed_rsa_key "$cross_file_duplicate/assets/copied-key.pem"
cross_file_duplicate_apk="$work_dir/cross-file-duplicate.apk"
make_apk "$cross_file_duplicate" "$cross_file_duplicate_apk"
expect_failure_reason cross-file-duplicate "$cross_file_duplicate_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

duplicate_rsa_key="$work_dir/duplicate-rsa-key"
copy_fixture "$base_arm64" "$duplicate_rsa_key"
append_allowed_rsa_key "$duplicate_rsa_key/lib/arm64-v8a/libkodi.so"
duplicate_rsa_key_apk="$work_dir/duplicate-rsa-key.apk"
make_apk "$duplicate_rsa_key" "$duplicate_rsa_key_apk"
expect_failure_reason duplicate-rsa-key "$duplicate_rsa_key_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

additional_private_key="$work_dir/additional-private-key"
copy_fixture "$base_arm64" "$additional_private_key"
write_private_armor \
  "$work_dir/additional-private-key.pem" \
  'PRIVATE KEY' \
  "$allowed_rsa_body"
cat "$work_dir/additional-private-key.pem" >> \
  "$additional_private_key/lib/arm64-v8a/libkodi.so"
additional_private_key_apk="$work_dir/additional-private-key.apk"
make_apk "$additional_private_key" "$additional_private_key_apk"
expect_failure_reason additional-private-key "$additional_private_key_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

malformed_rsa_key="$work_dir/malformed-rsa-key"
copy_fixture "$base_arm64" "$malformed_rsa_key"
mutate_allowed_rsa_key "$malformed_rsa_key/lib/arm64-v8a/libkodi.so" '!'
malformed_rsa_key_apk="$work_dir/malformed-rsa-key.apk"
make_apk "$malformed_rsa_key" "$malformed_rsa_key_apk"
expect_failure_reason malformed-rsa-key "$malformed_rsa_key_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

unterminated_rsa_key="$work_dir/unterminated-rsa-key"
copy_fixture "$base_arm64" "$unterminated_rsa_key"
python3 - "$unterminated_rsa_key/lib/arm64-v8a/libkodi.so" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
data = bytearray(path.read_bytes())
boundary = b'-' * 5
end = boundary + b'END RSA PRIVATE KEY' + boundary
end_offset = data.find(end)
if end_offset < 0:
    raise SystemExit(1)
data[end_offset + len(end) - 1] = ord('!')
path.write_bytes(data)
PY
unterminated_rsa_key_apk="$work_dir/unterminated-rsa-key.apk"
make_apk "$unterminated_rsa_key" "$unterminated_rsa_key_apk"
expect_failure_reason unterminated-rsa-key "$unterminated_rsa_key_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

four_hyphen_rsa_key="$work_dir/four-hyphen-rsa-key"
copy_fixture "$base_arm64" "$four_hyphen_rsa_key"
python3 - "$allowed_rsa_pem" "$four_hyphen_rsa_key/assets/recoverable-key.bin" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_bytes()
output = pathlib.Path(sys.argv[2])
five = b'-' * 5
four = b'-' * 4
malformed = source.replace(
    five + b'BEGIN RSA PRIVATE KEY' + five,
    four + b'BEGIN RSA PRIVATE KEY' + four,
).replace(
    five + b'END RSA PRIVATE KEY' + five,
    four + b'END RSA PRIVATE KEY' + four,
)
if malformed == source:
    raise SystemExit(1)
output.write_bytes(b'\0' + malformed + b'\0')
PY
four_hyphen_rsa_key_apk="$work_dir/four-hyphen-rsa-key.apk"
make_apk "$four_hyphen_rsa_key" "$four_hyphen_rsa_key_apk"
expect_failure_reason four-hyphen-rsa-key "$four_hyphen_rsa_key_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

openpgp_private_key="$work_dir/openpgp-private-key"
copy_fixture "$base_arm64" "$openpgp_private_key"
openpgp_packet="$work_dir/openpgp-secret-key.pgp"
openpgp_old_packet="$work_dir/openpgp-old-indeterminate.pgp"
openpgp_partial_packet="$work_dir/openpgp-partial.pgp"
openpgp_malformed_partial="$work_dir/openpgp-malformed-partial.pgp"
openpgp_nonsecret_partial="$work_dir/openpgp-nonsecret-partial.pgp"
openpgp_dex_collision="$work_dir/openpgp-dex-collision.bin"
openpgp_ec_malformed="$work_dir/openpgp-ec-malformed.pgp"
openpgp_oid_collision="$work_dir/openpgp-oid-collision.bin"
openpgp_point_bounds_collision="$work_dir/openpgp-point-bounds-collision.bin"
openpgp_kdf_policy_collision="$work_dir/openpgp-kdf-policy-collision.bin"
openpgp_ec_sha3="$work_dir/openpgp-ec-sha3.pgp"
openpgp_ec_sha224="$work_dir/openpgp-ec-sha224.pgp"
openpgp_ec_malformed_kdf="$work_dir/openpgp-ec-malformed-kdf.pgp"
openpgp_ec_standard_variants="$work_dir/openpgp-ec-standard-variants"
openpgp_ec_control_variants="$work_dir/openpgp-ec-control-variants"
mkdir -p "$openpgp_ec_standard_variants" "$openpgp_ec_control_variants"
python3 - \
  "$openpgp_private_key/assets/private-key.asc" \
  "$openpgp_packet" \
  "$openpgp_old_packet" \
  "$openpgp_partial_packet" \
  "$openpgp_malformed_partial" \
  "$openpgp_nonsecret_partial" \
  "$openpgp_dex_collision" \
  "$openpgp_ec_malformed" \
  "$openpgp_oid_collision" \
  "$openpgp_point_bounds_collision" \
  "$openpgp_kdf_policy_collision" \
  "$openpgp_ec_sha3" \
  "$openpgp_ec_sha224" \
  "$openpgp_ec_malformed_kdf" \
  "$openpgp_ec_standard_variants" \
  "$openpgp_ec_control_variants" <<'PY'
import base64
import pathlib
import sys


def mpi(value: int) -> bytes:
    encoded = value.to_bytes((value.bit_length() + 7) // 8, 'big')
    return value.bit_length().to_bytes(2, 'big') + encoded


def mpi_bytes(value: bytes) -> bytes:
    bits = value[0].bit_length() + (len(value) - 1) * 8
    return bits.to_bytes(2, 'big') + value


def crc24(data: bytes) -> int:
    crc = 0xB704CE
    for octet in data:
        crc ^= octet << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= 0x1864CFB
    return crc & 0xFFFFFF


def partial_packet(tag: int, value: bytes, chunk_size: int = 64) -> bytes:
    exponent = chunk_size.bit_length() - 1
    if chunk_size != 1 << exponent or not 0 <= tag <= 63:
        raise ValueError('invalid partial-packet fixture')
    output = bytearray([0xC0 | tag])
    cursor = 0
    while len(value) - cursor > chunk_size:
        output.append(224 + exponent)
        output.extend(value[cursor:cursor + chunk_size])
        cursor += chunk_size
    remaining = value[cursor:]
    if len(remaining) >= 192:
        raise ValueError('fixture final chunk is not one-octet length')
    output.append(len(remaining))
    output.extend(remaining)
    return bytes(output)


def fixed_packet(tag: int, value: bytes) -> bytes:
    if len(value) < 192:
        length = bytes([len(value)])
    elif len(value) <= 8383:
        adjusted = len(value) - 192
        length = bytes([192 + (adjusted >> 8), adjusted & 0xFF])
    else:
        length = b'\xff' + len(value).to_bytes(4, 'big')
    return bytes([0xC0 | tag]) + length + value


def ec_public(oid: bytes, point: bytes, kdf: bytes) -> bytes:
    return (
        b'\x04' + (0).to_bytes(4, 'big') + b'\x12' + bytes([len(oid)]) +
        oid + mpi_bytes(point) + kdf
    )


def truncated_old_secret_packet(public: bytes) -> bytes:
    return b'\x96' + (len(public) + 32).to_bytes(4, 'big') + public


def complete_ec_secret_packet(public: bytes) -> bytes:
    secret = mpi((1 << 255) + 0x13579)
    body = public + b'\x00' + secret + (sum(secret) & 0xFFFF).to_bytes(2, 'big')
    return fixed_packet(5, body)


# Deterministic non-key MPIs create a structurally valid packet without sensitive data.
n = (1 << 511) + 0x12345
e = 65537
d = (1 << 510) + 0x23457
p = (1 << 255) + 0x34567
q = (1 << 255) + 0x45679
u = (1 << 254) + 0x56789
public = b'\x04' + (0).to_bytes(4, 'big') + b'\x01' + mpi(n) + mpi(e)
secret = mpi(d) + mpi(p) + mpi(q) + mpi(u)
body = public + b'\x00' + secret + (sum(secret) & 0xFFFF).to_bytes(2, 'big')
if len(body) < 192:
    packet_length = bytes([len(body)])
else:
    adjusted_length = len(body) - 192
    packet_length = bytes([192 + (adjusted_length >> 8), adjusted_length & 0xFF])
packet = b'\xc5' + packet_length + body
encoded = base64.b64encode(packet)
lines = [encoded[index:index + 64] for index in range(0, len(encoded), 64)]
checksum = base64.b64encode(crc24(packet).to_bytes(3, 'big'))
boundary = b'-' * 5
armor = b'\n'.join([
    boundary + b'BEGIN PGP PRIVATE KEY BLOCK' + boundary,
    b'',
    *lines,
    b'=' + checksum,
    boundary + b'END PGP PRIVATE KEY BLOCK' + boundary,
    b'',
])
pathlib.Path(sys.argv[1]).write_bytes(armor)
pathlib.Path(sys.argv[2]).write_bytes(packet)
pathlib.Path(sys.argv[3]).write_bytes(b'\x97' + body)
pathlib.Path(sys.argv[4]).write_bytes(partial_packet(5, body))
pathlib.Path(sys.argv[5]).write_bytes(b'\xc5\xe7' + body[:128])
pathlib.Path(sys.argv[6]).write_bytes(partial_packet(11, b'nonsecret-packet-data-' * 12))

# This malformed old-format packet reproduces the structural collision found in
# a real classes.dex. Every public field except the EC point prefix is valid.
dex_oid = bytes.fromhex('121548c28b03c2a2060f08c29001120a08c29101120508')
dex_public = ec_public(dex_oid, b'\x05' + b'\x01' * 64, b'\x03\x01\x08\x07')
pathlib.Path(sys.argv[7]).write_bytes(truncated_old_secret_packet(dex_public))

# A standards-shaped P-256 ECDH public section remains rejectable even when the
# packet is truncated before its secret material.
p256_oid = bytes.fromhex('2A8648CE3D030107')
p256_point = b'\x04' + b'\x11' * 64
standard_ec_public = ec_public(p256_oid, p256_point, b'\x03\x01\x0e\x07')
pathlib.Path(sys.argv[8]).write_bytes(truncated_old_secret_packet(standard_ec_public))

# Each control below violates one independent malformed-packet predicate.
pathlib.Path(sys.argv[9]).write_bytes(truncated_old_secret_packet(
    ec_public(bytes.fromhex('2A8000'), p256_point, b'\x03\x01\x08\x07')
))
pathlib.Path(sys.argv[10]).write_bytes(truncated_old_secret_packet(
    ec_public(p256_oid, b'\x04' + b'\x22' * 133, b'\x03\x01\x08\x07')
))
sha224_public = ec_public(p256_oid, p256_point, b'\x03\x01\x0b\x07')
pathlib.Path(sys.argv[11]).write_bytes(truncated_old_secret_packet(sha224_public))

# Complete secret packets are private material even when they use a stronger,
# policy-invalid, or malformed-but-bounded KDF declaration.
sha3_public = ec_public(p256_oid, p256_point, b'\x03\x01\x0e\x07')
malformed_kdf_public = ec_public(p256_oid, p256_point, b'\x04\x02\x08\x07\x00')
pathlib.Path(sys.argv[12]).write_bytes(complete_ec_secret_packet(sha3_public))
pathlib.Path(sys.argv[13]).write_bytes(complete_ec_secret_packet(sha224_public))
pathlib.Path(sys.argv[14]).write_bytes(complete_ec_secret_packet(malformed_kdf_public))

# Accepted malformed-packet boundaries cover every permitted KDF id, the native
# point prefix, and the maximum P-521 point width. Each must remain key-like.
standard_variants = {
    'native-hash8-cipher8': ec_public(
        bytes.fromhex('2B060104019755010501'),
        b'\x40' + b'\x31' * 32,
        b'\x03\x01\x08\x08',
    ),
    'p521-hash12-cipher9': ec_public(
        bytes.fromhex('2B81040023'), b'\x04' + b'\x41' * 132, b'\x03\x01\x0c\x09'
    ),
    'p256-hash9-cipher7': ec_public(
        p256_oid, p256_point, b'\x03\x01\x09\x07'
    ),
    'p256-hash10-cipher7': ec_public(
        p256_oid, p256_point, b'\x03\x01\x0a\x07'
    ),
}
variant_dir = pathlib.Path(sys.argv[15])
for name, variant_public in standard_variants.items():
    (variant_dir / f'{name}.pgp').write_bytes(truncated_old_secret_packet(variant_public))

# These controls isolate each KDF field rejected for malformed packets.
control_variants = {
    'kdf-length': ec_public(p256_oid, p256_point, b'\x04\x01\x08\x07\x00'),
    'kdf-reserved': ec_public(p256_oid, p256_point, b'\x03\x02\x08\x07'),
    'kdf-cipher': ec_public(p256_oid, p256_point, b'\x03\x01\x08\x06'),
}
control_dir = pathlib.Path(sys.argv[16])
for name, variant_public in control_variants.items():
    (control_dir / f'{name}.bin').write_bytes(truncated_old_secret_packet(variant_public))
PY
openpgp_private_key_apk="$work_dir/openpgp-private-key.apk"
make_apk "$openpgp_private_key" "$openpgp_private_key_apk"
expect_failure_reason openpgp-private-key "$openpgp_private_key_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

openpgp_raw="$work_dir/openpgp-raw"
copy_fixture "$base_arm64" "$openpgp_raw"
cp "$openpgp_packet" "$openpgp_raw/assets/private-key.pgp"
openpgp_raw_apk="$work_dir/openpgp-raw.apk"
make_apk "$openpgp_raw" "$openpgp_raw_apk"
expect_failure_reason openpgp-raw "$openpgp_raw_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

for packet_variant in old-indeterminate partial malformed-partial; do
  openpgp_variant="$work_dir/openpgp-$packet_variant"
  copy_fixture "$base_arm64" "$openpgp_variant"
  cp "$work_dir/openpgp-$packet_variant.pgp" \
    "$openpgp_variant/assets/private-key.pgp"
  openpgp_variant_apk="$work_dir/openpgp-$packet_variant.apk"
  make_apk "$openpgp_variant" "$openpgp_variant_apk"
  expect_failure_reason "openpgp-$packet_variant" "$openpgp_variant_apk" arm64-v8a \
    'private signing, deployment, or runtime secret material'
done

openpgp_nonsecret="$work_dir/openpgp-nonsecret-control"
copy_fixture "$base_arm64" "$openpgp_nonsecret"
cp "$openpgp_nonsecret_partial" "$openpgp_nonsecret/assets/nonsecret-packet.pgp"
openpgp_nonsecret_apk="$work_dir/openpgp-nonsecret-control.apk"
make_apk "$openpgp_nonsecret" "$openpgp_nonsecret_apk"
verify_apk "$openpgp_nonsecret_apk" arm64-v8a >/dev/null

openpgp_dex_control="$work_dir/openpgp-dex-control"
copy_fixture "$base_arm64" "$openpgp_dex_control"
cp "$openpgp_dex_collision" "$openpgp_dex_control/classes.dex"
openpgp_dex_control_apk="$work_dir/openpgp-dex-control.apk"
make_apk "$openpgp_dex_control" "$openpgp_dex_control_apk"
verify_apk "$openpgp_dex_control_apk" arm64-v8a >/dev/null

for packet_control in oid-collision point-bounds-collision kdf-policy-collision; do
  openpgp_control="$work_dir/openpgp-$packet_control-control"
  copy_fixture "$base_arm64" "$openpgp_control"
  cp "$work_dir/openpgp-$packet_control.bin" "$openpgp_control/classes.dex"
  openpgp_control_apk="$work_dir/openpgp-$packet_control-control.apk"
  make_apk "$openpgp_control" "$openpgp_control_apk"
  verify_apk "$openpgp_control_apk" arm64-v8a >/dev/null
done

for packet_control in kdf-length kdf-reserved kdf-cipher; do
  openpgp_control="$work_dir/openpgp-$packet_control-control"
  copy_fixture "$base_arm64" "$openpgp_control"
  cp "$openpgp_ec_control_variants/$packet_control.bin" \
    "$openpgp_control/classes.dex"
  openpgp_control_apk="$work_dir/openpgp-$packet_control-control.apk"
  make_apk "$openpgp_control" "$openpgp_control_apk"
  verify_apk "$openpgp_control_apk" arm64-v8a >/dev/null
done

openpgp_ec_truncated="$work_dir/openpgp-ec-truncated"
copy_fixture "$base_arm64" "$openpgp_ec_truncated"
cp "$openpgp_ec_malformed" "$openpgp_ec_truncated/assets/private-key.pgp"
openpgp_ec_truncated_apk="$work_dir/openpgp-ec-truncated.apk"
make_apk "$openpgp_ec_truncated" "$openpgp_ec_truncated_apk"
expect_failure_reason openpgp-ec-truncated "$openpgp_ec_truncated_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

for packet_variant in \
  native-hash8-cipher8 \
  p521-hash12-cipher9 \
  p256-hash9-cipher7 \
  p256-hash10-cipher7; do
  openpgp_boundary="$work_dir/openpgp-$packet_variant-boundary"
  copy_fixture "$base_arm64" "$openpgp_boundary"
  cp "$openpgp_ec_standard_variants/$packet_variant.pgp" \
    "$openpgp_boundary/assets/private-key.pgp"
  openpgp_boundary_apk="$work_dir/openpgp-$packet_variant-boundary.apk"
  make_apk "$openpgp_boundary" "$openpgp_boundary_apk"
  expect_failure_reason "openpgp-$packet_variant" \
    "$openpgp_boundary_apk" arm64-v8a \
    'private signing, deployment, or runtime secret material'
done

for packet_variant in ec-sha3 ec-sha224 ec-malformed-kdf; do
  openpgp_complete="$work_dir/openpgp-$packet_variant-complete"
  copy_fixture "$base_arm64" "$openpgp_complete"
  cp "$work_dir/openpgp-$packet_variant.pgp" \
    "$openpgp_complete/assets/private-key.pgp"
  openpgp_complete_apk="$work_dir/openpgp-$packet_variant-complete.apk"
  make_apk "$openpgp_complete" "$openpgp_complete_apk"
  expect_failure_reason "openpgp-$packet_variant" "$openpgp_complete_apk" arm64-v8a \
    'private signing, deployment, or runtime secret material'
done

private_key_block="$work_dir/private-key-block"
copy_fixture "$base_arm64" "$private_key_block"
write_private_armor \
  "$private_key_block/assets/private-key-block.asc" \
  'VENDOR PRIVATE KEY BLOCK' \
  "$allowed_rsa_body"
private_key_block_apk="$work_dir/private-key-block.apk"
make_apk "$private_key_block" "$private_key_block_apk"
expect_failure_reason private-key-block "$private_key_block_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

# Plain known encodings and conservative text normalization are in scope;
# compression, encryption, and arbitrary cryptographic obfuscation are not.
for marker_variant in \
  ascii \
  missing-space \
  mixed-case \
  single-hyphen \
  nul-control \
  nbsp-utf8 \
  utf16le \
  utf16be; do
  marker_fixture="$work_dir/marker-$marker_variant"
  copy_fixture "$base_arm64" "$marker_fixture"
  write_marker_variant "$marker_fixture/assets/marker.bin" "$marker_variant"
  marker_apk="$work_dir/marker-$marker_variant.apk"
  make_apk "$marker_fixture" "$marker_apk"
  expect_failure_reason "marker-$marker_variant" "$marker_apk" arm64-v8a \
    'private signing, deployment, or runtime secret material'
done

nested_library="$work_dir/nested-library"
copy_fixture "$base_arm64" "$nested_library"
mkdir -p "$nested_library/lib/arm64-v8a/nested"
cp "$base_arm64/lib/arm64-v8a/libhelper.so" \
  "$nested_library/lib/arm64-v8a/nested/libnested.so"
nested_library_apk="$work_dir/nested-library.apk"
make_apk "$nested_library" "$nested_library_apk"
expect_failure nested-library "$nested_library_apk" arm64-v8a

expect_failure min-sdk "$arm64_apk" arm64-v8a MOCK_MIN_SDK=23
expect_failure target-sdk "$arm64_apk" arm64-v8a MOCK_TARGET_SDK=35
expect_failure package "$arm64_apk" arm64-v8a MOCK_PACKAGE=org.example.spoof
expect_failure version-name "$arm64_apk" arm64-v8a MOCK_VERSION_NAME=22.0
expect_failure version-code "$arm64_apk" arm64-v8a MOCK_VERSION_CODE=2190703
expect_failure debuggable "$arm64_apk" arm64-v8a MOCK_DEBUGGABLE=true
expect_failure v1-only "$arm64_apk" arm64-v8a MOCK_V2=false
expect_failure wrong-signer "$arm64_apk" arm64-v8a \
  MOCK_SIGNER_DIGEST=cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd
expect_failure multiple-signers "$arm64_apk" arm64-v8a \
  MOCK_SECOND_SIGNER_DIGEST=cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd
expect_failure signer-warning "$arm64_apk" arm64-v8a MOCK_APKSIGNER_FAIL=true
expect_failure_without_diagnostic \
  scanner-forced-error \
  "$arm64_apk" \
  arm64-v8a \
  'APK contains apparent private signing, deployment, or runtime secret material' \
  'forced scanner error' \
  '' \
  JUMPGATE_TEST_FORCE_SECRET_SCANNER_ERROR=1

expect_failure_without_diagnostic \
  config-scanner-forced-error \
  "$arm64_apk" \
  arm64-v8a \
  'APK contains a non-placeholder secret assignment in packaged configuration' \
  'forced configuration scanner error' \
  '' \
  JUMPGATE_TEST_FORCE_CONFIG_SCANNER_ERROR=1

mixed_case_secrets="$work_dir/mixed-case-secrets"
copy_fixture "$base_arm64" "$mixed_case_secrets"
mixed_case_first_value='uppercase-entry-live-secret'
mixed_case_second_value='lowercase-entry-live-secret'
printf '{"api_key":"%s"}\n' "$mixed_case_first_value" > \
  "$mixed_case_secrets/assets/Z-competing.json"
printf '{"api_key":"%s"}\n' "$mixed_case_second_value" > \
  "$mixed_case_secrets/assets/a-competing.json"
mixed_case_secrets_apk="$work_dir/mixed-case-secrets.apk"
make_apk "$mixed_case_secrets" "$mixed_case_secrets_apk"
expect_failure_diagnostic_without_value \
  mixed-case-secrets "$mixed_case_secrets_apk" arm64-v8a \
  'assets/Z-competing.json' 'config-json' "$mixed_case_first_value" \
  JUMPGATE_TEST_REVERSE_CONFIG_CANDIDATES=1

gettext_catalog="$work_dir/gettext-catalog"
copy_fixture "$base_arm64" "$gettext_catalog"
gettext_catalog_source="$script_dir/../../../addons/webinterface.default/lang/_strings/en.json"
gettext_catalog_path='assets/addons/webinterface.default/lang/_strings/en.json'
test -f "$gettext_catalog_source"
mkdir -p "$gettext_catalog/$(dirname "$gettext_catalog_path")"
cp "$gettext_catalog_source" "$gettext_catalog/$gettext_catalog_path"
gettext_catalog_apk="$work_dir/gettext-catalog.apk"
make_apk "$gettext_catalog" "$gettext_catalog_apk"
verify_apk "$gettext_catalog_apk" arm64-v8a >/dev/null

mutate_gettext_catalog() {
  local path="$1"
  local value="$2"
  local mode="$3"
  python3 - "$path" "$value" "$mode" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
value = sys.argv[2]
mode = sys.argv[3]
document = json.loads(path.read_bytes())
messages = document['locale_data']['messages']
message_id = 'Set your personal API key'
original = messages.get(message_id)
if not isinstance(original, list) or not original or not all(
    isinstance(item, str) for item in original
):
    raise SystemExit(1)
if mode == 'string':
    messages[message_id] = [value]
elif mode == 'object':
    messages[message_id] = [{'translation': value}]
else:
    raise SystemExit(1)
path.write_text(
    json.dumps(document, ensure_ascii=True, separators=(',', ':')),
    encoding='utf-8',
)
PY
}

gettext_modified_catalog="$work_dir/gettext-modified-catalog"
copy_fixture "$base_arm64" "$gettext_modified_catalog"
gettext_modified_catalog_value='modified-catalog-live-secret'
gettext_modified_catalog_path='addons/webinterface.default/lang/_strings/en.json'
mkdir -p "$gettext_modified_catalog/$(dirname "$gettext_modified_catalog_path")"
cp "$gettext_catalog_source" \
  "$gettext_modified_catalog/$gettext_modified_catalog_path"
mutate_gettext_catalog \
  "$gettext_modified_catalog/$gettext_modified_catalog_path" \
  "$gettext_modified_catalog_value" \
  string
gettext_modified_catalog_apk="$work_dir/gettext-modified-catalog.apk"
make_apk "$gettext_modified_catalog" "$gettext_modified_catalog_apk"
expect_failure_diagnostic_without_value \
  gettext-modified-catalog \
  "$gettext_modified_catalog_apk" \
  arm64-v8a \
  "$gettext_modified_catalog_path" \
  'config-json' \
  "$gettext_modified_catalog_value"

gettext_schema_mimic="$work_dir/gettext-schema-mimic"
copy_fixture "$base_arm64" "$gettext_schema_mimic"
gettext_schema_mimic_value='schema-mimic-live-secret'
gettext_schema_mimic_path='assets/catalog-schema-mimic.json'
cat > "$gettext_schema_mimic/$gettext_schema_mimic_path" <<JSON
{
  "domain": "messages",
  "locale_data": {
    "messages": {
      "": {
        "domain": "messages",
        "plural_forms": "nplurals=2; plural=(n != 1);",
        "lang": "en_gb"
      },
      "Set your personal API key": ["$gettext_schema_mimic_value"]
    }
  }
}
JSON
gettext_schema_mimic_apk="$work_dir/gettext-schema-mimic.apk"
make_apk "$gettext_schema_mimic" "$gettext_schema_mimic_apk"
expect_failure_diagnostic_without_value \
  gettext-schema-mimic \
  "$gettext_schema_mimic_apk" \
  arm64-v8a \
  "$gettext_schema_mimic_path" \
  'config-json' \
  "$gettext_schema_mimic_value"

gettext_identifier_secret="$work_dir/gettext-identifier-secret"
copy_fixture "$base_arm64" "$gettext_identifier_secret"
gettext_identifier_secret_value='identifier-message-live-secret'
gettext_identifier_secret_path='addons/plugin.video.fixture/lang/_strings/en_us.json'
mkdir -p "$gettext_identifier_secret/$(dirname "$gettext_identifier_secret_path")"
cat > "$gettext_identifier_secret/$gettext_identifier_secret_path" <<JSON
{
  "domain": "messages",
  "locale_data": {
    "messages": {
      "": {
        "domain": "messages",
        "plural_forms": "nplurals=2; plural=(n != 1);",
        "lang": "en_us"
      },
      "api_key": ["$gettext_identifier_secret_value"]
    }
  }
}
JSON
gettext_identifier_secret_apk="$work_dir/gettext-identifier-secret.apk"
make_apk "$gettext_identifier_secret" "$gettext_identifier_secret_apk"
expect_failure_diagnostic_without_value \
  gettext-identifier-secret \
  "$gettext_identifier_secret_apk" \
  arm64-v8a \
  "$gettext_identifier_secret_path" \
  'config-json' \
  "$gettext_identifier_secret_value"

gettext_malformed_mutation="$work_dir/gettext-malformed-mutation"
copy_fixture "$base_arm64" "$gettext_malformed_mutation"
gettext_malformed_mutation_value='malformed-catalog-live-secret'
gettext_malformed_mutation_path='addons/webinterface.default/lang/_strings/en.json'
mkdir -p \
  "$gettext_malformed_mutation/$(dirname "$gettext_malformed_mutation_path")"
cp "$gettext_catalog_source" \
  "$gettext_malformed_mutation/$gettext_malformed_mutation_path"
mutate_gettext_catalog \
  "$gettext_malformed_mutation/$gettext_malformed_mutation_path" \
  "$gettext_malformed_mutation_value" \
  object
gettext_malformed_mutation_apk="$work_dir/gettext-malformed-mutation.apk"
make_apk "$gettext_malformed_mutation" "$gettext_malformed_mutation_apk"
expect_failure_diagnostic_without_value \
  gettext-malformed-mutation \
  "$gettext_malformed_mutation_apk" \
  arm64-v8a \
  "$gettext_malformed_mutation_path" \
  'config-json' \
  "$gettext_malformed_mutation_value"

json_secret="$work_dir/json-secret"
copy_fixture "$base_arm64" "$json_secret"
json_secret_value='s3cr3t!'
printf '{"nested":{"apiKey":"%s"}}\n' "$json_secret_value" > \
  "$json_secret/assets/private.json"
json_secret_apk="$work_dir/json-secret.apk"
make_apk "$json_secret" "$json_secret_apk"
expect_failure_diagnostic_without_value \
  json-short-secret "$json_secret_apk" arm64-v8a \
  'assets/private.json' 'config-json' "$json_secret_value"

secret_filename="$work_dir/secret-filename"
copy_fixture "$base_arm64" "$secret_filename"
secret_filename_marker='password-token-file-live-secret'
secret_filename_value='filename-payload-live-secret'
secret_filename_path="assets/$secret_filename_marker.json"
printf '{"api_key":"%s"}\n' "$secret_filename_value" > \
  "$secret_filename/$secret_filename_path"
secret_filename_apk="$work_dir/secret-filename.apk"
make_apk "$secret_filename" "$secret_filename_apk"
expect_failure_diagnostic_without_value \
  secret-filename "$secret_filename_apk" arm64-v8a \
  "$secret_filename_path" 'config-json' "$secret_filename_value"

json_sequence_secret="$work_dir/json-sequence-secret"
copy_fixture "$base_arm64" "$json_sequence_secret"
json_sequence_value='json-sequence-live-secret'
printf '[{"profile":"primary"},{"credentials":{"client_secret":"%s"}}]\n' \
  "$json_sequence_value" > "$json_sequence_secret/assets/sequence.json"
json_sequence_apk="$work_dir/json-sequence-secret.apk"
make_apk "$json_sequence_secret" "$json_sequence_apk"
expect_failure_without_value json-sequence-secret "$json_sequence_apk" arm64-v8a \
  "$json_sequence_value"

github_token_json="$work_dir/github-token-json"
copy_fixture "$base_arm64" "$github_token_json"
github_token_json_value='github-live-token-value'
printf '{"github_token":"%s"}\n' "$github_token_json_value" > \
  "$github_token_json/assets/github-token.json"
github_token_json_apk="$work_dir/github-token-json.apk"
make_apk "$github_token_json" "$github_token_json_apk"
expect_failure_without_value github-token-json "$github_token_json_apk" arm64-v8a \
  "$github_token_json_value"

yaml_secret="$work_dir/yaml-secret"
copy_fixture "$base_arm64" "$yaml_secret"
yaml_fixture_value='0123456789abcdef'
yaml_fixture_value+='0123456789ABCDEF'
printf 'refresh-token: %s\n' "$yaml_fixture_value" > \
  "$yaml_secret/assets/private.yaml"
yaml_secret_apk="$work_dir/yaml-secret.apk"
make_apk "$yaml_secret" "$yaml_secret_apk"
expect_failure_diagnostic_without_value \
  yaml-unquoted-secret "$yaml_secret_apk" arm64-v8a \
  'assets/private.yaml' 'config-yaml' "$yaml_fixture_value"

deploy_token_yaml="$work_dir/deploy-token-yaml"
copy_fixture "$base_arm64" "$deploy_token_yaml"
deploy_token_yaml_value='deploy-live-token-value'
printf 'deploy_token: "%s"\n' "$deploy_token_yaml_value" > \
  "$deploy_token_yaml/assets/deploy-token.yaml"
deploy_token_yaml_apk="$work_dir/deploy-token-yaml.apk"
make_apk "$deploy_token_yaml" "$deploy_token_yaml_apk"
expect_failure_without_value deploy-token-yaml "$deploy_token_yaml_apk" arm64-v8a \
  "$deploy_token_yaml_value"

yaml_sequence_secret="$work_dir/yaml-sequence-secret"
copy_fixture "$base_arm64" "$yaml_sequence_secret"
yaml_sequence_value='sequence-live-secret'
cat > "$yaml_sequence_secret/assets/sequence.yaml" <<YAML
profiles:
  - name: primary
    api_key: "$yaml_sequence_value"
YAML
yaml_sequence_apk="$work_dir/yaml-sequence-secret.apk"
make_apk "$yaml_sequence_secret" "$yaml_sequence_apk"
expect_failure_without_value yaml-sequence-secret "$yaml_sequence_apk" arm64-v8a \
  "$yaml_sequence_value"

yaml_flow_secret="$work_dir/yaml-flow-secret"
copy_fixture "$base_arm64" "$yaml_flow_secret"
yaml_flow_value='flow-map-live-secret'
printf 'oauth: {client_secret: "%s", token_type: Bearer}\n' "$yaml_flow_value" > \
  "$yaml_flow_secret/assets/flow.yaml"
yaml_flow_apk="$work_dir/yaml-flow-secret.apk"
make_apk "$yaml_flow_secret" "$yaml_flow_apk"
expect_failure_without_value yaml-flow-secret "$yaml_flow_apk" arm64-v8a \
  "$yaml_flow_value"

yaml_nested_secret="$work_dir/yaml-nested-secret"
copy_fixture "$base_arm64" "$yaml_nested_secret"
yaml_nested_value='nested-live-secret'
cat > "$yaml_nested_secret/assets/nested.yaml" <<YAML
services:
  trakt:
    credentials:
      password: "$yaml_nested_value"
YAML
yaml_nested_apk="$work_dir/yaml-nested-secret.apk"
make_apk "$yaml_nested_secret" "$yaml_nested_apk"
expect_failure_without_value yaml-nested-secret "$yaml_nested_apk" arm64-v8a \
  "$yaml_nested_value"

token_secret="$work_dir/token-secret"
copy_fixture "$base_arm64" "$token_secret"
token_secret_value='token-value-live-secret'
printf 'token_secret: "%s"\n' "$token_secret_value" > \
  "$token_secret/assets/token-secret.yaml"
token_secret_apk="$work_dir/token-secret.apk"
make_apk "$token_secret" "$token_secret_apk"
expect_failure_without_value token-secret "$token_secret_apk" arm64-v8a \
  "$token_secret_value"

properties_secret="$work_dir/properties-secret"
copy_fixture "$base_arm64" "$properties_secret"
properties_secret_value='plain-short-secret'
printf 'client.secret=%s\n' "$properties_secret_value" > \
  "$properties_secret/assets/private.properties"
properties_secret_apk="$work_dir/properties-secret.apk"
make_apk "$properties_secret" "$properties_secret_apk"
expect_failure_without_value properties-secret "$properties_secret_apk" arm64-v8a \
  "$properties_secret_value"

env_secret="$work_dir/env-secret"
copy_fixture "$base_arm64" "$env_secret"
env_secret_value='live-token'
printf 'API_TOKEN=%s\n' "$env_secret_value" > "$env_secret/assets/.env"
env_secret_apk="$work_dir/env-secret.apk"
make_apk "$env_secret" "$env_secret_apk"
expect_failure_without_value env-secret "$env_secret_apk" arm64-v8a \
  "$env_secret_value"

github_token_env="$work_dir/github-token-env"
copy_fixture "$base_arm64" "$github_token_env"
github_token_env_value='github-actions-live-token'
printf 'GITHUB_TOKEN=%s\n' "$github_token_env_value" > \
  "$github_token_env/assets/.env"
github_token_env_apk="$work_dir/github-token-env.apk"
make_apk "$github_token_env" "$github_token_env_apk"
expect_failure_without_value github-token-env "$github_token_env_apk" arm64-v8a \
  "$github_token_env_value"

text_secret="$work_dir/text-secret"
copy_fixture "$base_arm64" "$text_secret"
text_secret_value='short-pass'
printf 'password: "%s"\n' "$text_secret_value" > \
  "$text_secret/assets/private.txt"
text_secret_apk="$work_dir/text-secret.apk"
make_apk "$text_secret" "$text_secret_apk"
expect_failure_diagnostic_without_value \
  text-secret "$text_secret_apk" arm64-v8a \
  'assets/private.txt' 'config-text' "$text_secret_value"

yaml_parser_error="$work_dir/yaml-parser-error"
copy_fixture "$base_arm64" "$yaml_parser_error"
cat > "$yaml_parser_error/assets/duplicate.yaml" <<'YAML'
setting: first
setting: second
YAML
yaml_parser_error_apk="$work_dir/yaml-parser-error.apk"
make_apk "$yaml_parser_error" "$yaml_parser_error_apk"
expect_failure_without_diagnostic \
  yaml-parser-error \
  "$yaml_parser_error_apk" \
  arm64-v8a \
  'APK contains a non-placeholder secret assignment in packaged configuration' \
  'found a duplicate key' \
  'assets/duplicate.yaml'

path_traversal_apk="$work_dir/path-traversal.apk"
python3 - "$path_traversal_apk" <<'PY'
import pathlib
import sys
import zipfile

with zipfile.ZipFile(pathlib.Path(sys.argv[1]), 'w') as archive:
    archive.writestr('../outside.txt', 'unsafe')
PY
expect_failure path-traversal "$path_traversal_apk" arm64-v8a

symlink_apk="$work_dir/symlink.apk"
python3 - "$symlink_apk" <<'PY'
import pathlib
import stat
import sys
import zipfile

link = zipfile.ZipInfo('lib/arm64-v8a/libkodi.so')
link.create_system = 3
link.external_attr = (stat.S_IFLNK | 0o777) << 16
with zipfile.ZipFile(pathlib.Path(sys.argv[1]), 'w') as archive:
    archive.writestr(link, '../../../outside')
PY
expect_failure symlink "$symlink_apk" arm64-v8a

ads_path_apk="$work_dir/ntfs-ads-path.apk"
python3 - "$arm64_apk" "$ads_path_apk" <<'PY'
import pathlib
import shutil
import sys
import zipfile

source = pathlib.Path(sys.argv[1])
output = pathlib.Path(sys.argv[2])
shutil.copyfile(source, output)
with zipfile.ZipFile(output, 'a', compression=zipfile.ZIP_DEFLATED) as archive:
    archive.writestr('assets/runtime:secret.dat', b'not extracted on NTFS')
PY
expect_failure_reason ntfs-ads-path "$ads_path_apk" arm64-v8a \
  'archive failed safe extraction checks'

named_runtime_secret="$work_dir/named-runtime-secret"
copy_fixture "$base_arm64" "$named_runtime_secret"
named_runtime_secret_value='archive-runtime-secret-value'
printf 'KODI_ANDROID_STORE_PASSWORD=%s\n' "$named_runtime_secret_value" > \
  "$named_runtime_secret/assets/runtime.dat"
named_runtime_secret_apk="$work_dir/named-runtime-secret.apk"
make_apk "$named_runtime_secret" "$named_runtime_secret_apk"
expect_failure_without_value named-runtime-secret "$named_runtime_secret_apk" arm64-v8a \
  "$named_runtime_secret_value"

for jwk_type in rsa ec okp; do
  jwk_fixture="$work_dir/private-jwk-$jwk_type"
  copy_fixture "$base_arm64" "$jwk_fixture"
  case "$jwk_type" in
    rsa)
      printf '%s\n' \
        '{"kty":"RSA","n":"public-modulus","e":"AQAB","d":"private-exponent","p":"prime-one","q":"prime-two","dp":"dp-value","dq":"dq-value","qi":"qi-value"}' \
        > "$jwk_fixture/assets/private.jwk"
      ;;
    ec)
      printf '%s\n' \
        '{"kty":"EC","crv":"P-256","x":"public-x","y":"public-y","d":"private-scalar"}' \
        > "$jwk_fixture/assets/private.jwk"
      ;;
    okp)
      printf '%s\n' \
        '{"kty":"OKP","crv":"Ed25519","x":"public-x","d":"private-seed"}' \
        > "$jwk_fixture/assets/private.jwk"
      ;;
  esac
  jwk_apk="$work_dir/private-jwk-$jwk_type.apk"
  make_apk "$jwk_fixture" "$jwk_apk"
  expect_failure_reason "private-jwk-$jwk_type" "$jwk_apk" arm64-v8a \
    'private signing, deployment, or runtime secret material'
done

placeholder_private_jwk="$work_dir/placeholder-private-jwk"
copy_fixture "$base_arm64" "$placeholder_private_jwk"
printf '%s\n' \
  '{"kty":"RSA","n":"public-modulus","e":"AQAB","d":"REDACTED"}' \
  > "$placeholder_private_jwk/assets/private.jwk"
placeholder_private_jwk_apk="$work_dir/placeholder-private-jwk.apk"
make_apk "$placeholder_private_jwk" "$placeholder_private_jwk_apk"
expect_failure_reason placeholder-private-jwk "$placeholder_private_jwk_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

oct_private_jwk="$work_dir/oct-private-jwk"
copy_fixture "$base_arm64" "$oct_private_jwk"
printf '%s\n' \
  '{"kty":"oct","k":"c3ltbWV0cmljLXNlY3JldA"}' \
  > "$oct_private_jwk/assets/private.jwk"
oct_private_jwk_apk="$work_dir/oct-private-jwk.apk"
make_apk "$oct_private_jwk" "$oct_private_jwk_apk"
expect_failure_reason oct-private-jwk "$oct_private_jwk_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

for assignment_variant in \
  spaced-quoted utf16le utf16be quoted-key continuation \
  append quoted-suffix sentinel-prefix; do
  assignment_fixture="$work_dir/named-assignment-$assignment_variant"
  copy_fixture "$base_arm64" "$assignment_fixture"
  write_named_assignment_variant \
    "$assignment_fixture/assets/named-assignment.bin" \
    "$assignment_variant"
  assignment_apk="$work_dir/named-assignment-$assignment_variant.apk"
  make_apk "$assignment_fixture" "$assignment_apk"
  expect_failure_reason \
    "named-assignment-$assignment_variant" \
    "$assignment_apk" \
    arm64-v8a \
    'private signing, deployment, or runtime secret material'
done

sentinel_prefix_config="$work_dir/sentinel-prefix-config"
copy_fixture "$base_arm64" "$sentinel_prefix_config"
printf '%s\n' '{"api_key":"EXAMPLE_live_secret"}' \
  > "$sentinel_prefix_config/assets/prefix-secret.json"
sentinel_prefix_config_apk="$work_dir/sentinel-prefix-config.apk"
make_apk "$sentinel_prefix_config" "$sentinel_prefix_config_apk"
expect_failure_reason sentinel-prefix-config "$sentinel_prefix_config_apk" arm64-v8a \
  'non-placeholder secret assignment in packaged configuration'

private_key="$work_dir/private-key"
copy_fixture "$base_arm64" "$private_key"
write_private_armor \
  "$private_key/assets/private.txt" \
  'PRIVATE KEY' \
  "$allowed_rsa_body"
private_key_apk="$work_dir/private-key.apk"
make_apk "$private_key" "$private_key_apk"
expect_failure private-key "$private_key_apk" arm64-v8a

printf 'APK verifier real-tool and adversarial tests: passed\n'

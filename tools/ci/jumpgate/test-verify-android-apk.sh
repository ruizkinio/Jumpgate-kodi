#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
verifier="$script_dir/verify-android-apk.sh"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

expected_package='com.example.fixtureplayer'
expected_min_sdk='24'
expected_target_sdk='36'
expected_version_name='22.0-ALPHA2'
expected_version_code='2190702'
expected_core_library='libfixture.so'
mock_signer_sha256='abababababababababababababababababababababababababababababababab'
allowed_rsa_der_sha256='8959c62b4351cbaa702942f4572d37335a7a3dfdcc6f0d2763a2afb486e3ac8f'

command -v python3 >/dev/null
command -v sha256sum >/dev/null
python3 -c 'import yaml' >/dev/null
test -f "$verifier"
if grep -Fqi 'ephemeral CI certificate' "$verifier"; then
  echo 'APK verifier still hard-codes an ephemeral-CI-only signer diagnostic' >&2
  exit 1
fi
grep -Fq 'does not match the expected signer' "$verifier"
grep -Fq 'Verified APK against expected signer:' "$verifier"

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
  "${MOCK_PACKAGE:-com.example.fixtureplayer}" \
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
  {
    printf '\0'
    cat "$allowed_rsa_pem"
    printf '\0'
  } >> "$1"
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
      "$mock_signer_sha256" \
      "$expected_core_library"
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
      "$signer_sha256" \
      "$expected_core_library"
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

expect_failure_phase_diagnostic() {
  local label="$1"
  local apk="$2"
  local abi="$3"
  local expected_phase="$4"
  local output status diagnostic
  shift 4
  set +e
  output="$(verify_apk "$apk" "$abi" "$@" 2>&1)"
  status="$?"
  set -e
  if [[ "$status" -eq 0 ]]; then
    echo "Expected verifier failure: $label" >&2
    exit 1
  fi
  diagnostic="JUMPGATE_APK_SCAN_REJECT {\"phase\":\"$expected_phase\"}"
  if ! grep -Fqx -- "$diagnostic" <<< "$output"; then
    echo "Verifier omitted the sanitized phase diagnostic: $label" >&2
    exit 1
  fi
  if [[ "$output" == *'Traceback (most recent call last)'* ]]; then
    echo "Verifier disclosed a scanner traceback: $label" >&2
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
compile_shared "$base_arm64/lib/arm64-v8a/$expected_core_library" arm64-v8a
compile_shared "$base_arm64/lib/arm64-v8a/libhelper.so" arm64-v8a
compile_shared "$base_armv7/lib/armeabi-v7a/$expected_core_library" armeabi-v7a
compile_shared "$base_armv7/lib/armeabi-v7a/libhelper.so" armeabi-v7a
append_allowed_rsa_key "$base_arm64/lib/arm64-v8a/$expected_core_library"
append_allowed_rsa_key "$base_armv7/lib/armeabi-v7a/$expected_core_library"
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
arm64_verify_output="$(verify_apk "$arm64_apk" arm64-v8a)"
[[ "$arm64_verify_output" == \
  "Verified APK against expected signer: package=$expected_package version=$expected_version_name abi=arm64-v8a core=$expected_core_library signer_sha256=$mock_signer_sha256" ]]
[[ "$(wc -l < "$readelf_log")" -eq 2 ]]
grep -Fxq 'libhelper.so' "$readelf_log"
grep -Fxq "$expected_core_library" "$readelf_log"
test -s "$arm64_apk.sha256"

: > "$readelf_log"
verify_apk "$armv7_apk" armeabi-v7a >/dev/null
[[ "$(wc -l < "$readelf_log")" -eq 2 ]]
grep -Fxq 'libhelper.so' "$readelf_log"
grep -Fxq "$expected_core_library" "$readelf_log"
test -s "$armv7_apk.sha256"

verify_apk "$arm64_apk" arm64-v8a \
  JUMPGATE_TEST_OPENPGP_WORK=500000 >/dev/null
expect_failure_phase_diagnostic \
  openpgp-work-budget-500001 \
  "$arm64_apk" \
  arm64-v8a \
  'openpgp-work-budget-probe' \
  JUMPGATE_TEST_OPENPGP_WORK=500001
verify_apk "$arm64_apk" arm64-v8a \
  JUMPGATE_TEST_OPENPGP_COPY_WORK=exact >/dev/null
expect_failure_phase_diagnostic \
  openpgp-copy-budget \
  "$arm64_apk" \
  arm64-v8a \
  'openpgp-copy-budget-probe' \
  JUMPGATE_TEST_OPENPGP_COPY_WORK=overflow
verify_apk "$arm64_apk" arm64-v8a \
  JUMPGATE_TEST_OPENPGP_CHECKSUM_WORK=exact >/dev/null
expect_failure_phase_diagnostic \
  openpgp-checksum-budget \
  "$arm64_apk" \
  arm64-v8a \
  'openpgp-checksum-budget-probe' \
  JUMPGATE_TEST_OPENPGP_CHECKSUM_WORK=overflow

openpgp_cross_packet_work="$work_dir/openpgp-cross-packet-work"
copy_fixture "$base_arm64" "$openpgp_cross_packet_work"
python3 - \
  "$openpgp_cross_packet_work/assets/fixed-candidates-a.pgp" \
  "$openpgp_cross_packet_work/assets/fixed-candidates-b.pgp" <<'PY'
import pathlib
import sys

candidate = bytes((0xC5, 6, 4, 0, 0, 0, 0, 0))
pathlib.Path(sys.argv[1]).write_bytes(candidate * 250_000)
pathlib.Path(sys.argv[2]).write_bytes(candidate * 250_001)
PY
openpgp_cross_packet_work_apk="$work_dir/openpgp-cross-packet-work.apk"
make_apk "$openpgp_cross_packet_work" "$openpgp_cross_packet_work_apk"
expect_failure_reason \
  openpgp-cross-packet-work \
  "$openpgp_cross_packet_work_apk" \
  arm64-v8a \
  'private signing, deployment, or runtime secret material'

openpgp_prefilter_spam="$work_dir/openpgp-prefilter-spam"
copy_fixture "$base_arm64" "$openpgp_prefilter_spam"
python3 - "$openpgp_prefilter_spam/assets/nonsemantic-candidates.pgp" <<'PY'
import pathlib
import sys

candidate = bytes((0xC5, 1, 2))
pathlib.Path(sys.argv[1]).write_bytes(candidate * 500_001)
PY
openpgp_prefilter_spam_apk="$work_dir/openpgp-prefilter-spam.apk"
make_apk "$openpgp_prefilter_spam" "$openpgp_prefilter_spam_apk"
verify_apk "$openpgp_prefilter_spam_apk" arm64-v8a \
  JUMPGATE_TEST_OPENPGP_EXPECT_OPERATIONS=0 \
  JUMPGATE_TEST_OPENPGP_EXPECT_COPIED_BYTES=0 >/dev/null

openpgp_fixed_overlap="$work_dir/openpgp-fixed-overlap"
copy_fixture "$base_arm64" "$openpgp_fixed_overlap"
python3 - "$openpgp_fixed_overlap/assets/fixed-overlap-candidates.pgp" <<'PY'
import pathlib
import sys

def mpi(bits: int, value: bytes) -> bytes:
    return bits.to_bytes(2, 'big') + value


public = (
    b'\x04' + b'\x00' * 4 + b'\x01' +
    mpi(512, b'\x80' + b'\x01' * 63) +
    mpi(17, b'\x01\x00\x01')
)
# Every candidate reaches a valid RSA public layout. The invalid protection
# octet keeps it non-secret, while the fixed body extends over later frames.
candidate = b'\xC5\xFF' + (128 * 1024).to_bytes(4, 'big') + public + b'\x0E'
pathlib.Path(sys.argv[1]).write_bytes(
    candidate * 10_000 + b'\x00' * (128 * 1024)
)
PY
openpgp_fixed_overlap_apk="$work_dir/openpgp-fixed-overlap.apk"
make_apk "$openpgp_fixed_overlap" "$openpgp_fixed_overlap_apk"
verify_apk "$openpgp_fixed_overlap_apk" arm64-v8a \
  JUMPGATE_TEST_OPENPGP_EXPECT_OPERATIONS=10000 \
  JUMPGATE_TEST_OPENPGP_EXPECT_COPIED_BYTES=0 >/dev/null

openpgp_partial_overlap="$work_dir/openpgp-partial-overlap"
copy_fixture "$base_arm64" "$openpgp_partial_overlap"
python3 - "$openpgp_partial_overlap/assets/partial-overlap-candidates.pgp" <<'PY'
import pathlib
import sys

def mpi(bits: int, value: bytes) -> bytes:
    return bits.to_bytes(2, 'big') + value


public = (
    b'\x04' + b'\x00' * 4 + b'\x01' +
    mpi(512, b'\x80' + b'\x01' * 63) +
    mpi(17, b'\x01\x00\x01')
)
# Every semantic candidate spans the remaining bounded source window and
# reaches the segmented slice path twice in the recoverable-field layout check.
candidate = b'\xC5\xFE' + public + b'\x0E'
pathlib.Path(sys.argv[1]).write_bytes(candidate * 10_000)
PY
openpgp_partial_overlap_apk="$work_dir/openpgp-partial-overlap.apk"
make_apk "$openpgp_partial_overlap" "$openpgp_partial_overlap_apk"
verify_apk "$openpgp_partial_overlap_apk" arm64-v8a \
  JUMPGATE_TEST_OPENPGP_EXPECT_OPERATIONS=20000 \
  JUMPGATE_TEST_OPENPGP_EXPECT_COPIED_BYTES=40000 >/dev/null

set +e
invalid_core_output="$(
  env \
    ANDROID_BUILD_TOOLS_ROOT="$mock_tools" \
    JUMPGATE_READELF_BIN="$readelf_wrapper" \
    bash "$verifier" \
      "$arm64_apk" \
      arm64-v8a \
      "$expected_package" \
      "$expected_min_sdk" \
      "$expected_target_sdk" \
      "$expected_version_name" \
      "$expected_version_code" \
      "$mock_signer_sha256" \
      '../libfixture.so' 2>&1
)"
invalid_core_status="$?"
set -e
[[ "$invalid_core_status" -eq 2 ]]
[[ "$invalid_core_output" == *'Invalid expected Android core library name'* ]]

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
    package="com.example.fixtureplayer"
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
mkdir -p "$spoof_library/lib/arm64-v8a/$expected_core_library"
printf 'not a library\n' > "$spoof_library/lib/arm64-v8a/$expected_core_library/file.txt"
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
rm "$missing_core/lib/arm64-v8a/$expected_core_library"
missing_core_apk="$work_dir/missing-core.apk"
make_apk "$missing_core" "$missing_core_apk"
expect_failure missing-core "$missing_core_apk" arm64-v8a

mutated_rsa_key="$work_dir/mutated-rsa-key"
copy_fixture "$base_arm64" "$mutated_rsa_key"
mutate_allowed_rsa_key "$mutated_rsa_key/lib/arm64-v8a/$expected_core_library" A
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
cat "$allowed_rsa_der" >> "$raw_der_expected_lib/lib/arm64-v8a/$expected_core_library"
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
remove_allowed_rsa_key "$wrong_path_rsa_key/lib/arm64-v8a/$expected_core_library"
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
append_allowed_rsa_key "$duplicate_rsa_key/lib/arm64-v8a/$expected_core_library"
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
  "$additional_private_key/lib/arm64-v8a/$expected_core_library"
additional_private_key_apk="$work_dir/additional-private-key.apk"
make_apk "$additional_private_key" "$additional_private_key_apk"
expect_failure_reason additional-private-key "$additional_private_key_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

malformed_rsa_key="$work_dir/malformed-rsa-key"
copy_fixture "$base_arm64" "$malformed_rsa_key"
mutate_allowed_rsa_key "$malformed_rsa_key/lib/arm64-v8a/$expected_core_library" '!'
malformed_rsa_key_apk="$work_dir/malformed-rsa-key.apk"
make_apk "$malformed_rsa_key" "$malformed_rsa_key_apk"
expect_failure_reason malformed-rsa-key "$malformed_rsa_key_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

unterminated_rsa_key="$work_dir/unterminated-rsa-key"
copy_fixture "$base_arm64" "$unterminated_rsa_key"
python3 - "$unterminated_rsa_key/lib/arm64-v8a/$expected_core_library" <<'PY'
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
openpgp_partial_subkey_collision="$work_dir/openpgp-partial-subkey-collision.pgp"
openpgp_partial_subkey_complete="$work_dir/openpgp-partial-subkey-complete.pgp"
openpgp_partial_subkey_recoverable="$work_dir/openpgp-partial-subkey-recoverable.pgp"
openpgp_partial_subkey_no_checksum="$work_dir/openpgp-partial-subkey-no-checksum.pgp"
openpgp_partial_subkey_one_checksum="$work_dir/openpgp-partial-subkey-one-checksum.pgp"
openpgp_partial_subkey_complete_variants="$work_dir/openpgp-partial-subkey-complete-variants"
openpgp_fixed_subkey_control="$work_dir/openpgp-fixed-subkey-control.pgp"
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
openpgp_version_controls="$work_dir/openpgp-version-controls"
openpgp_v6_pass_controls="$work_dir/openpgp-v6-pass-controls"
openpgp_byte_cap_controls="$work_dir/openpgp-byte-cap-controls"
openpgp_max_fragmentation="$work_dir/openpgp-max-fragmentation.pgp"
mkdir -p \
  "$openpgp_ec_standard_variants" \
  "$openpgp_ec_control_variants" \
  "$openpgp_partial_subkey_complete_variants" \
  "$openpgp_version_controls" \
  "$openpgp_v6_pass_controls" \
  "$openpgp_byte_cap_controls"
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
  "$openpgp_ec_control_variants" \
  "$openpgp_partial_subkey_collision" \
  "$openpgp_partial_subkey_complete" \
  "$openpgp_partial_subkey_recoverable" \
  "$openpgp_partial_subkey_no_checksum" \
  "$openpgp_partial_subkey_one_checksum" \
  "$openpgp_partial_subkey_complete_variants" \
  "$openpgp_fixed_subkey_control" \
  "$openpgp_version_controls" \
  "$openpgp_v6_pass_controls" \
  "$openpgp_byte_cap_controls" \
  "$openpgp_max_fragmentation" <<'PY'
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


def private_key_armor(packet: bytes) -> bytes:
    encoded = base64.b64encode(packet)
    lines = [encoded[index:index + 64] for index in range(0, len(encoded), 64)]
    checksum = base64.b64encode(crc24(packet).to_bytes(3, 'big'))
    boundary = b'-' * 5
    return b'\n'.join([
        boundary + b'BEGIN PGP PRIVATE KEY BLOCK' + boundary,
        b'',
        *lines,
        b'=' + checksum,
        boundary + b'END PGP PRIVATE KEY BLOCK' + boundary,
        b'',
    ])


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


def overdeclared_fixed_packet(tag: int, value: bytes) -> bytes:
    declared_length = len(value) + 1
    if declared_length >= 192:
        raise ValueError('overdeclared fixture needs one-octet length')
    return bytes([0xC0 | tag, declared_length]) + value


def old_fixed_packet(tag: int, declared_length: int, value: bytes) -> bytes:
    if not 0 <= tag <= 15 or not 0 <= declared_length <= 0xFFFFFFFF:
        raise ValueError('invalid old-format fixture')
    if declared_length <= 0xFF:
        length_type = 0
        length = declared_length.to_bytes(1, 'big')
    elif declared_length <= 0xFFFF:
        length_type = 1
        length = declared_length.to_bytes(2, 'big')
    else:
        length_type = 2
        length = declared_length.to_bytes(4, 'big')
    return bytes([0x80 | (tag << 2) | length_type]) + length + value


def rsa_public(version: int, modulus: int, exponent: int) -> bytes:
    material = mpi(modulus) + mpi(exponent)
    if version == 3:
        return b'\x03' + (0).to_bytes(4, 'big') + b'\x00\x00\x01' + material
    if version == 4:
        return b'\x04' + (0).to_bytes(4, 'big') + b'\x01' + material
    if version == 6:
        return (
            b'\x06' + (0).to_bytes(4, 'big') + b'\x01' +
            len(material).to_bytes(4, 'big') + material
        )
    raise ValueError('unsupported fixture key version')


def dsa_public(
    version: int, prime: int, subgroup: int, generator: int, value: int
) -> bytes:
    material = mpi(prime) + mpi(subgroup) + mpi(generator) + mpi(value)
    prefix = bytes([version]) + (0).to_bytes(4, 'big') + b'\x11'
    if version == 4:
        return prefix + material
    if version == 6:
        return prefix + len(material).to_bytes(4, 'big') + material
    raise ValueError('unsupported DSA fixture version')


def elgamal_public(
    version: int, prime: int, generator: int, value: int, algorithm: int = 16
) -> bytes:
    if algorithm not in {16, 20}:
        raise ValueError('unsupported ElGamal fixture algorithm')
    material = mpi(prime) + mpi(generator) + mpi(value)
    if version == 4:
        return (
            b'\x04' + (0).to_bytes(4, 'big') + bytes([algorithm]) + material
        )
    if version == 6:
        return (
            b'\x06' + (0).to_bytes(4, 'big') + bytes([algorithm]) +
            len(material).to_bytes(4, 'big') + material
        )
    raise ValueError('unsupported ElGamal fixture version')


def private_algorithm_public(version: int, algorithm: int, material: bytes) -> bytes:
    if not 100 <= algorithm <= 110:
        raise ValueError('fixture algorithm is not private-use')
    prefix = bytes([version]) + (0).to_bytes(4, 'big') + bytes([algorithm])
    if version == 4:
        return prefix + material
    if version == 6:
        return prefix + len(material).to_bytes(4, 'big') + material
    raise ValueError('unsupported private-use fixture version')


def native_public(version: int, algorithm: int, material: bytes) -> bytes:
    prefix = bytes([version]) + (0).to_bytes(4, 'big') + bytes([algorithm])
    if version == 4:
        return prefix + material
    if version == 6:
        return prefix + len(material).to_bytes(4, 'big') + material
    raise ValueError('unsupported native-key fixture version')


def ec_public_version(
    version: int, algorithm: int, oid: bytes, point: bytes, kdf: bytes = b''
) -> bytes:
    if algorithm not in {18, 19, 22} or not 0 < len(oid) <= 255:
        raise ValueError('invalid EC fixture')
    material = bytes([len(oid)]) + oid + mpi_bytes(point) + kdf
    prefix = bytes([version]) + (0).to_bytes(4, 'big') + bytes([algorithm])
    if version == 4:
        return prefix + material
    if version == 6:
        return prefix + len(material).to_bytes(4, 'big') + material
    raise ValueError('unsupported EC fixture version')


def ec_public(oid: bytes, point: bytes, kdf: bytes) -> bytes:
    return ec_public_version(4, 18, oid, point, kdf)


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
elgamal_prime = (1 << 1023) + 0xC5D7
elgamal_generator = 2
elgamal_secret_value = (1 << 255) + 0x6A5B
elgamal_public_value = pow(
    elgamal_generator, elgamal_secret_value, elgamal_prime
)
public = rsa_public(4, n, e)
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

# This tag-7, 2^30 partial-length prefix reproduces the incomplete ARMv7
# executable-code collision without copying candidate bytes. Complete and
# recoverable private bodies remain rejection controls despite invalid framing.
pathlib.Path(sys.argv[17]).write_bytes(
    bytes([0xC0 | 7, 224 + 30]) + body[:128]
)
pathlib.Path(sys.argv[18]).write_bytes(partial_packet(7, body))
pathlib.Path(sys.argv[19]).write_bytes(
    bytes([0xC0 | 7, 224 + 30]) + body + b'non-packet trailing data'
)
pathlib.Path(sys.argv[20]).write_bytes(
    bytes([0xC0 | 7, 224 + 30]) + body[:-2]
)
pathlib.Path(sys.argv[21]).write_bytes(
    bytes([0xC0 | 7, 224 + 30]) + body[:-1]
)
complete_variant_dir = pathlib.Path(sys.argv[22])
for name, variant_body in {
    'no-checksum': body[:-2],
    'one-checksum': body[:-1],
    'zero-checksum': body[:-2] + b'\x00\x00',
    'trailing': body + b'non-packet trailing data',
}.items():
    (complete_variant_dir / f'{name}.pgp').write_bytes(
        partial_packet(7, variant_body)
    )
(complete_variant_dir / 'over-128-one-byte-chunks.pgp').write_bytes(
    partial_packet(7, body, chunk_size=1)
)
pathlib.Path(sys.argv[23]).write_bytes(fixed_packet(7, body))

version_control_dir = pathlib.Path(sys.argv[24])
v6_pass_control_dir = pathlib.Path(sys.argv[25])
byte_cap_control_dir = pathlib.Path(sys.argv[26])

def write_rejection_matrix(name: str, key_body: bytes) -> None:
    for tag in (5, 7):
        for framing, encode in (('fixed', fixed_packet), ('partial', partial_packet)):
            output = f'{name}-{framing}-tag{tag}.pgp'
            (version_control_dir / output).write_bytes(encode(tag, key_body))


reclassified_rejection_count = 0


def write_reclassified_rejection_matrix(name: str, key_body: bytes) -> None:
    global reclassified_rejection_count
    write_rejection_matrix(name, key_body)
    reclassified_rejection_count += 4


def write_fixed_rejection(name: str, key_body: bytes, tag: int = 5) -> None:
    output = f'{name}-fixed-tag{tag}.pgp'
    (version_control_dir / output).write_bytes(fixed_packet(tag, key_body))


version_bodies = {}
for version in (3, 4, 6):
    version_public = rsa_public(version, n, e)
    checksum = (
        (sum(secret) & 0xFFFF).to_bytes(2, 'big')
        if version in {3, 4} else b''
    )
    version_body = version_public + b'\x00' + secret + checksum
    version_bodies[version] = version_body
    write_rejection_matrix(f'v{version}-unprotected', version_body)

    if version in {3, 4}:
        boundary_variants = {
            'no-checksum': version_body[:-2],
            'one-checksum': version_body[:-1],
            'zero-checksum': version_body[:-2] + b'\x00\x00',
            'trailing-data': version_body + b'isolated trailing data',
        }
    else:
        boundary_variants = {
            'trailing-data': version_body + b'isolated trailing data',
        }
    for variant_name, variant_body in boundary_variants.items():
        for tag in (5, 7):
            write_fixed_rejection(
                f'v{version}-unprotected-{variant_name}', variant_body, tag
            )

    for tag in (5, 7):
        secret_boundary = len(version_public) + 3
        underdeclared = version_body[:secret_boundary]
        remaining = version_body[secret_boundary:]
        (version_control_dir / f'v{version}-underdeclared-new-tag{tag}.pgp').write_bytes(
            fixed_packet(tag, underdeclared) + remaining
        )
        (version_control_dir / f'v{version}-underdeclared-old-tag{tag}.pgp').write_bytes(
            old_fixed_packet(tag, len(underdeclared), version_body)
        )
        (version_control_dir / f'v{version}-underdeclared-partial-tag{tag}.pgp').write_bytes(
            partial_packet(tag, underdeclared) + remaining
        )
        (version_control_dir / f'v{version}-zero-outer-length-tag{tag}.pgp').write_bytes(
            bytes((0xC0 | tag, 0)) + version_body
        )
        (version_control_dir / f'v{version}-public-only-outer-length-tag{tag}.pgp').write_bytes(
            fixed_packet(tag, version_public) + version_body[len(version_public):]
        )

# OpenPGP MPIs are structural fields, not a cryptographic-strength policy. Keys
# below historical size floors and unusual values must still be recognized.
for version in (4, 6):
    dsa_secret = mpi(7)
    elgamal_policy_secret = mpi(11)
    policy_bodies = {
        'rsa-low-modulus': (
            rsa_public(version, (1 << 127) + 0x123, e), secret
        ),
        'rsa-large-exponent': (
            rsa_public(version, n, (1 << 64) + 3), secret
        ),
        'dsa-below-old-floors': (
            dsa_public(version, (1 << 127) + 5, (1 << 79) + 3, 2, 3),
            dsa_secret,
        ),
        'elgamal-below-old-floor-and-range': (
            elgamal_public(version, 127, 131, 137), elgamal_policy_secret
        ),
    }
    for policy_name, (policy_public, policy_secret) in policy_bodies.items():
        policy_checksum = (
            (sum(policy_secret) & 0xFFFF).to_bytes(2, 'big')
            if version == 4 else b''
        )
        write_rejection_matrix(
            f'v{version}-{policy_name}',
            policy_public + b'\x00' + policy_secret + policy_checksum,
        )

# Complete EC secret packets are classified by their length-delimited shape,
# not by curve-size policy. The strict malformed-binary heuristic remains
# separately bounded below.
standard_oid = bytes.fromhex('2A8648CE3D030107')
standard_point = b'\x04' + b'\x11' * 64
ec_policy_variants = {
    'ecdh-short-oid': (18, b'\x2a', standard_point, b'\x03\x01\x08\x07'),
    'ecdsa-long-oid': (19, b'\x2a' + b'\x01' * 39, standard_point, b''),
    'legacy-eddsa-short-point': (22, standard_oid, b'\x04\x01', b''),
    'ecdsa-oversized-point': (
        19, standard_oid, b'\x04' + b'\x01' * 140, b''
    ),
    'ecdh-short-kdf': (18, standard_oid, standard_point, b'\x01\x00'),
}
ec_policy_secret = mpi(5)
for version in (4, 6):
    ec_checksum = (
        (sum(ec_policy_secret) & 0xFFFF).to_bytes(2, 'big')
        if version == 4 else b''
    )
    for name, (algorithm, oid, point, kdf) in ec_policy_variants.items():
        ec_policy_public = ec_public_version(
            version, algorithm, oid, point, kdf
        )
        ec_policy_body = (
            ec_policy_public + b'\x00' + ec_policy_secret + ec_checksum
        )
        write_fixed_rejection(f'v{version}-{name}', ec_policy_body)
        write_fixed_rejection(f'v{version}-{name}', ec_policy_body, 7)
for name, (algorithm, oid, point, kdf) in ec_policy_variants.items():
    malformed = truncated_old_secret_packet(
        ec_public_version(4, algorithm, oid, point, kdf)
    )
    (v6_pass_control_dir / f'{name}-malformed-after-binary-noise.pgp').write_bytes(
        b'\x00opaque-binary-prefix\x01' + malformed
    )

# Private-use public-key algorithms have vendor-defined material layouts. A
# complete secret-key packet at the file root or in a valid packet stream is
# authoritative without guessing those layouts.
private_algorithm_packets = {}
for algorithm in range(100, 111):
    for version in (4, 6):
        private_body = (
            private_algorithm_public(version, algorithm, b'\x42' * 8) +
            b'\x00\x51\x52\x53'
        )
        packet = fixed_packet(5, private_body)
        private_algorithm_packets[(version, algorithm)] = packet
        write_fixed_rejection(
            f'v{version}-private-algorithm-{algorithm}', private_body
        )
        write_fixed_rejection(
            f'v{version}-private-algorithm-{algorithm}', private_body, 7
        )
for algorithm in (100, 110):
    packet = private_algorithm_packets[(6, algorithm)]
    for prefix_tag in (11, 60, 63):
        (version_control_dir / (
            f'v6-private-algorithm-{algorithm}-after-tag{prefix_tag}.pgp'
        )).write_bytes(
            fixed_packet(prefix_tag, b'benign-openpgp-prefix') + packet
        )
    (v6_pass_control_dir / (
        f'v6-private-algorithm-{algorithm}-after-binary-noise.pgp'
    )).write_bytes(b'\x00opaque-binary-prefix\x01' + packet)

weak_malformed_public = rsa_public(
    4, (1 << 127) + 0x123, (1 << 64) + 3
)
weak_malformed_packet = old_fixed_packet(
    5, len(weak_malformed_public) + 32, weak_malformed_public
)
(version_control_dir / 'weak-rsa-malformed-root.pgp').write_bytes(
    weak_malformed_packet
)
(v6_pass_control_dir / 'weak-rsa-malformed-after-binary-noise.pgp').write_bytes(
    b'\x00opaque-binary-prefix\x01' + weak_malformed_packet
)
weak_unprotected_body = (
    weak_malformed_public + b'\x00' + secret +
    (sum(secret) & 0xFFFF).to_bytes(2, 'big')
)
weak_underdeclared_packet = (
    fixed_packet(5, weak_malformed_public) +
    weak_unprotected_body[len(weak_malformed_public):]
)
(version_control_dir / 'weak-rsa-underdeclared-root.pgp').write_bytes(
    weak_underdeclared_packet
)
(v6_pass_control_dir / 'weak-rsa-underdeclared-after-binary-noise.pgp').write_bytes(
    b'\x00opaque-binary-prefix\x01' + weak_underdeclared_packet
)

# The outer five-octet framing match starts two bytes before a complete old
# indeterminate packet. Prefilter matches must overlap or the inner key is skipped.
overlapping_body = bytearray(version_bodies[4])
overlapping_body[3] = 4  # Also makes the outer candidate's body version-shaped.
(version_control_dir / 'overlapping-prefilter-old-tag5.pgp').write_bytes(
    b'\xC5\xFF\x97' + overlapping_body
)

elgamal_public_keys = {}
elgamal_secret = mpi(elgamal_secret_value)
for version in (4, 6):
    public_key = elgamal_public(
        version, elgamal_prime, elgamal_generator, elgamal_public_value
    )
    elgamal_public_keys[version] = public_key
    checksum = (
        (sum(elgamal_secret) & 0xFFFF).to_bytes(2, 'big')
        if version == 4 else b''
    )
    write_rejection_matrix(
        f'v{version}-elgamal-unprotected',
        public_key + b'\x00' + elgamal_secret + checksum,
    )

elgamal20_public = elgamal_public(
    4,
    elgamal_prime,
    elgamal_generator,
    elgamal_public_value,
    algorithm=20,
)
elgamal20_body = (
    elgamal20_public + b'\x00' + elgamal_secret +
    (sum(elgamal_secret) & 0xFFFF).to_bytes(2, 'big')
)
write_rejection_matrix('v4-elgamal20-unprotected', elgamal20_body)
(version_control_dir / 'v4-elgamal20-armored.pgp').write_bytes(
    private_key_armor(fixed_packet(5, elgamal20_body))
)

native_sizes = {25: 32, 26: 56, 27: 32, 28: 57}
native_public_values = {}
native_secret_values = {}
for algorithm, octets in native_sizes.items():
    public_value = bytes(((algorithm + index) % 251) + 1 for index in range(octets))
    secret_value = bytes(((algorithm * 3 + index) % 251) + 1 for index in range(octets))
    native_public_values[algorithm] = public_value
    native_secret_values[algorithm] = secret_value
    for version in (4, 6):
        checksum = (
            (sum(secret_value) & 0xFFFF).to_bytes(2, 'big')
            if version == 4 else b''
        )
        native_body = (
            native_public(version, algorithm, public_value) + b'\x00' +
            secret_value + checksum
        )
        write_rejection_matrix(
            f'v{version}-native-algorithm-{algorithm}', native_body
        )
        all_zero_body = (
            native_public(version, algorithm, bytes(octets)) + b'\x00' +
            secret_value + checksum
        )
        all_zero_name = (
            f'v4-all-zero-native-public-{algorithm}'
            if version == 4 else f'all-zero-native-public-{algorithm}'
        )
        write_reclassified_rejection_matrix(all_zero_name, all_zero_body)
        public_key = native_public(version, algorithm, public_value)
        private_suffix = native_body[len(public_key):]
        for tag in (5, 7):
            for framing, public_only in (
                ('new', overdeclared_fixed_packet(tag, public_key)),
                ('old', old_fixed_packet(tag, len(public_key) + 1, public_key)),
            ):
                (v6_pass_control_dir / (
                    f'v{version}-native-{algorithm}-public-only-'
                    f'overdeclared-{framing}-tag{tag}.pgp'
                )).write_bytes(public_only)
            for framing, underdeclared in (
                ('new', fixed_packet(tag, public_key) + private_suffix),
                ('old', old_fixed_packet(tag, len(public_key), native_body)),
            ):
                (version_control_dir / (
                    f'v{version}-native-{algorithm}-secret-underdeclared-'
                    f'{framing}-tag{tag}.pgp'
                )).write_bytes(underdeclared)

v6_public = rsa_public(6, n, e)
v6_public_material = v6_public[10:]
cipher_blocks = {
    1: 8, 2: 8, 3: 8, 4: 8,
    7: 16, 8: 16, 9: 16, 10: 16, 11: 16, 12: 16, 13: 16,
}
aead_nonces = {1: 16, 2: 15, 3: 12}
def argon2_specifier(passes: int, parallelism: int, encoded_memory: int) -> bytes:
    return (
        b'\x04' + b'\x25' * 16 +
        bytes((passes, parallelism, encoded_memory))
    )


s2k_specifiers = {
    0: b'\x00\x08',
    1: b'\x01\x08' + b'\x21' * 8,
    3: b'\x03\x08' + b'\x23' * 8 + b'\x60',
    4: argon2_specifier(3, 1, 16),
}


def protection_parameters(
    usage: int,
    cipher: int = 7,
    aead: int = 2,
    s2k: bytes = s2k_specifiers[3],
    vector: bytes | None = None,
) -> bytes:
    output = bytearray([cipher])
    if usage == 253:
        output.append(aead)
        vector_length = aead_nonces[aead]
    elif vector is not None:
        vector_length = len(vector)
    else:
        vector_length = cipher_blocks[cipher]
    output.append(len(s2k))
    output.extend(s2k)
    output.extend(bytes([0x31 + usage % 7]) * vector_length if vector is None else vector)
    return bytes(output)


def v4_aead_parameters(
    cipher: int = 7,
    aead: int = 2,
    s2k: bytes = s2k_specifiers[3],
    vector: bytes | None = None,
) -> bytes:
    vector_length = aead_nonces[aead]
    nonce = b'\x34' * vector_length if vector is None else vector
    return bytes((cipher, aead)) + s2k + nonce


def v4_cfb_parameters(
    protection: int,
    cipher: int = 7,
    s2k: bytes = s2k_specifiers[3],
    vector: bytes | None = None,
) -> bytes:
    effective_cipher = cipher if protection in {254, 255} else protection
    output = bytearray()
    if protection in {254, 255}:
        output.append(cipher)
        output.extend(s2k)
    vector_length = (
        len(vector) if vector is not None else cipher_blocks[effective_cipher]
    )
    output.extend(
        bytes([0x35 + protection % 7]) * vector_length
        if vector is None else vector
    )
    return bytes(output)


def v4_aead_body(
    parameters: bytes,
    payload: bytes,
    public_key: bytes = public,
) -> bytes:
    return public_key + b'\xFD' + parameters + payload


def v4_cfb_body(
    protection: int,
    parameters: bytes,
    payload: bytes,
    public_key: bytes = public,
) -> bytes:
    return public_key + bytes((protection,)) + parameters + payload


def protected_body(
    usage: int,
    parameters: bytes,
    payload: bytes,
    public_key: bytes = v6_public,
) -> bytes:
    return public_key + bytes([usage, len(parameters)]) + parameters + payload


valid_parameters = {
    253: protection_parameters(253),
    254: protection_parameters(254),
}
valid_v4_253 = v4_aead_parameters()
write_rejection_matrix(
    'v4-rsa-protected-253',
    v4_aead_body(valid_v4_253, b'\x6F' * 64),
)
valid_v4_cfb = {
    7: v4_cfb_parameters(7),
    254: v4_cfb_parameters(254),
    255: v4_cfb_parameters(255),
}
for protection, parameters in valid_v4_cfb.items():
    write_rejection_matrix(
        f'v4-rsa-protected-{protection}',
        v4_cfb_body(protection, parameters, b'\x6E' * 64),
    )
for algorithm, octets in native_sizes.items():
    v4_native_public = native_public(
        4, algorithm, native_public_values[algorithm]
    )
    write_rejection_matrix(
        f'v4-native-{algorithm}-protected-253',
        v4_aead_body(
            valid_v4_253,
            b'\x70' * (octets + 16),
            v4_native_public,
        ),
    )
    write_rejection_matrix(
        f'v4-native-{algorithm}-overlong-protected-253',
        v4_aead_body(
            valid_v4_253,
            b'\x78' * (octets + 16 + 1),
            v4_native_public,
        ),
    )
    for protection, parameters in valid_v4_cfb.items():
        trailer_octets = 20 if protection == 254 else 2
        write_rejection_matrix(
            f'v4-native-{algorithm}-protected-{protection}',
            v4_cfb_body(
                protection,
                parameters,
                b'\x79' * (octets + trailer_octets),
                v4_native_public,
            ),
        )
        write_reclassified_rejection_matrix(
            f'v4-native-{algorithm}-overlong-protected-{protection}',
            v4_cfb_body(
                protection,
                parameters,
                b'\x7d' * (octets + trailer_octets + 1),
                v4_native_public,
            ),
        )
write_rejection_matrix(
    'v4-elgamal-protected-254',
    v4_cfb_body(
        254, valid_v4_cfb[254], b'\x67' * 32, elgamal_public_keys[4]
    ),
)
write_rejection_matrix(
    'v4-elgamal20-protected-253',
    v4_aead_body(valid_v4_253, b'\x68' * 32, elgamal20_public),
)
for protection, parameters in valid_v4_cfb.items():
    write_rejection_matrix(
        f'v4-elgamal20-protected-{protection}',
        v4_cfb_body(
            protection, parameters, b'\x69' * 32, elgamal20_public
        ),
    )
for usage, parameters in valid_parameters.items():
    trailer_octets = 16 if usage == 253 else 20
    write_rejection_matrix(
        f'v6-elgamal-protected-{usage}',
        protected_body(
            usage,
            parameters,
            b'\x68' * (3 + trailer_octets),
            elgamal_public_keys[6],
        ),
    )
for usage, parameters in valid_parameters.items():
    write_rejection_matrix(
        f'v6-protected-{usage}',
        protected_body(usage, parameters, bytes([usage & 0xFF]) * 64),
    )
for algorithm, octets in native_sizes.items():
    public_key = native_public(6, algorithm, native_public_values[algorithm])
    for usage, parameters in valid_parameters.items():
        trailer_octets = 16 if usage == 253 else 20
        write_fixed_rejection(
            f'v6-native-{algorithm}-protected-{usage}',
            protected_body(
                usage,
                parameters,
                b'\x65' * (octets + trailer_octets),
                public_key,
            ),
        )
        write_reclassified_rejection_matrix(
            f'native-{algorithm}-overlong-protected-{usage}',
            protected_body(
                usage,
                parameters,
                b'\x65' * (octets + trailer_octets + 1),
                public_key,
            ),
        )

# Every registered cipher family, AEAD mode, S2K shape, and hash accepted by
# the parser gets an independent valid protected-key control.
for cipher in cipher_blocks:
    parameters = protection_parameters(254, cipher=cipher)
    write_fixed_rejection(
        f'v6-protected-cipher-{cipher}', protected_body(254, parameters, b'\x61' * 64)
    )
for aead in aead_nonces:
    parameters = protection_parameters(253, aead=aead)
    write_fixed_rejection(
        f'v6-protected-aead-{aead}', protected_body(253, parameters, b'\x62' * 64)
    )
for s2k_type, s2k in s2k_specifiers.items():
    usage = 253 if s2k_type == 4 else 254
    parameters = protection_parameters(usage, s2k=s2k)
    write_fixed_rejection(
        f'v6-protected-s2k-{s2k_type}', protected_body(usage, parameters, b'\x63' * 64)
    )
for hash_id in (1, 2, 3, 8, 9, 10, 11, 12, 14):
    parameters = protection_parameters(254, s2k=bytes((0, hash_id)))
    write_fixed_rejection(
        f'v6-protected-hash-{hash_id}', protected_body(254, parameters, b'\x64' * 64)
    )

# Exercise every private-use cipher id through a length-delimited V6 AEAD
# packet, then cover each distinct V4/V6 protection grammar at both range ends.
private_cipher_packets = {}
for cipher in range(100, 111):
    parameters = protection_parameters(253, cipher=cipher)
    private_body = protected_body(253, parameters, b'\x6b' * 64)
    private_cipher_packets[cipher] = fixed_packet(5, private_body)
    write_fixed_rejection(f'v6-private-cipher-{cipher}-aead', private_body)
for cipher in (100, 110):
    private_vector = bytes([cipher]) * 8
    write_fixed_rejection(
        f'v4-private-cipher-{cipher}-aead',
        v4_aead_body(
            v4_aead_parameters(cipher=cipher), b'\x6c' * 64
        ),
    )
    for protection in (254, 255):
        write_fixed_rejection(
            f'v4-private-cipher-{cipher}-protected-{protection}',
            v4_cfb_body(
                protection,
                v4_cfb_parameters(
                    protection, cipher=cipher, vector=private_vector
                ),
                b'\x6d' * 64,
            ),
        )
    write_fixed_rejection(
        f'v4-private-cipher-{cipher}-direct',
        v4_cfb_body(
            cipher,
            v4_cfb_parameters(cipher, vector=private_vector),
            b'\x6e' * 64,
        ),
    )
    parameters = protection_parameters(
        254, cipher=cipher, vector=private_vector
    )
    write_fixed_rejection(
        f'v6-private-cipher-{cipher}-cfb',
        protected_body(254, parameters, b'\x6f' * 64),
    )
for cipher in (100, 110):
    packet = private_cipher_packets[cipher]
    for prefix_tag in (11, 60, 63):
        (version_control_dir / (
            f'v6-private-cipher-{cipher}-after-tag{prefix_tag}.pgp'
        )).write_bytes(
            fixed_packet(prefix_tag, b'benign-openpgp-prefix') + packet
        )
    (v6_pass_control_dir / (
        f'v6-private-cipher-{cipher}-after-binary-noise.pgp'
    )).write_bytes(b'\x00opaque-binary-prefix\x01' + packet)

valid_253 = valid_parameters[253]
valid_254 = valid_parameters[254]
# The protected packet bytes are identical; only their left context differs.
context_protected_packet = fixed_packet(
    5, protected_body(254, valid_254, b'\x6a' * 64)
)
(v6_pass_control_dir / 'protected-after-binary-noise.pgp').write_bytes(
    b'\x00\x01opaque-binary-prefix\x02' + context_protected_packet
)
(version_control_dir / 'protected-after-benign-packet.pgp').write_bytes(
    fixed_packet(11, b'benign-openpgp-prefix') + context_protected_packet
)
pass_bodies = {
    'v4-aead-eight-byte-cipher': v4_aead_body(
        v4_aead_parameters(cipher=1), b'\x70' * 64
    ),
    'v6-aead-eight-byte-cipher': protected_body(
        253, protection_parameters(253, cipher=1), b'\x7e' * 64
    ),
    'v4-aead-invalid-cipher': v4_aead_body(
        b'\x06' + valid_v4_253[1:], b'\x71' * 64
    ),
    'v4-aead-invalid-aead': v4_aead_body(
        bytes((7, 4)) + valid_v4_253[2:], b'\x72' * 64
    ),
    'v4-aead-missing-s2k': v4_aead_body(b'\x07\x02', b'\x73' * 64),
    'v4-aead-invalid-s2k-type': v4_aead_body(
        b'\x07\x02\x02\x08' + b'\x34' * 15, b'\x74' * 64
    ),
    'v4-aead-invalid-s2k-hash': v4_aead_body(
        b'\x07\x02\x00\x04' + b'\x34' * 15, b'\x75' * 64
    ),
    'v4-aead-truncated-nonce-and-payload': v4_aead_body(
        valid_v4_253[:-1], b'\x76' * 28
    ),
    'v4-aead-insufficient-payload': v4_aead_body(
        valid_v4_253, b'\x77' * 27
    ),
    'v4-cfb-invalid-cipher-254': v4_cfb_body(
        254, b'\x06' + valid_v4_cfb[254][1:], b'\x7a' * 64
    ),
    'v4-cfb-invalid-s2k-254': v4_cfb_body(
        254, b'\x07\x02\x08' + b'\x36' * 16, b'\x7b' * 64
    ),
    'v4-cfb-short-iv-254': v4_cfb_body(
        254, valid_v4_cfb[254][:-1], b'\x7c' * 31
    ),
    'usage-255': protected_body(255, valid_254, b'\x51' * 64),
    'legacy-usage': v6_public + b'\x07' + b'\x53' * 64,
    'missing-parameter-length': v6_public + b'\xfd',
    'zero-parameter-length': v6_public + b'\xfd\x00' + b'\x54' * 64,
    'overrunning-parameter-length': v6_public + b'\xfe\x20' + b'\x41' * 8,
    'underdeclared-parameter-length': (
        v6_public + b'\xfd' + bytes([len(valid_253) - 1]) +
        valid_253 + b'\x55' * 64
    ),
    'invalid-cipher': protected_body(
        253, b'\x06' + valid_253[1:], b'\x56' * 64
    ),
    'invalid-aead': protected_body(
        253, bytes((7, 4)) + valid_253[2:], b'\x57' * 64
    ),
    'missing-s2k-length': protected_body(253, b'\x07\x02', b'\x58' * 64),
    'zero-s2k-length': protected_body(
        253, b'\x07\x02\x00' + b'\x31' * 15, b'\x59' * 64
    ),
    'overrunning-s2k-length': protected_body(
        253, b'\x07\x02\x20\x03\x08', b'\x5a' * 64
    ),
    'underdeclared-s2k-length': protected_body(
        253, b'\x07\x02\x0a' + s2k_specifiers[3] + b'\x31' * 15, b'\x5b' * 64
    ),
    'invalid-s2k-type': protected_body(
        253, b'\x07\x02\x02\x02\x08' + b'\x31' * 15, b'\x5c' * 64
    ),
    'invalid-s2k-size': protected_body(
        253, b'\x07\x02\x03\x00\x08\x00' + b'\x31' * 15, b'\x5d' * 64
    ),
    'invalid-s2k-hash': protected_body(
        253, b'\x07\x02\x02\x00\x04' + b'\x31' * 15, b'\x5e' * 64
    ),
    'argon2-with-cfb': protected_body(
        254,
        protection_parameters(254, s2k=s2k_specifiers[4]),
        b'\x69' * 64,
    ),
    'argon2-zero-passes': protected_body(
        253,
        protection_parameters(253, s2k=argon2_specifier(0, 1, 16)),
        b'\x6a' * 64,
    ),
    'argon2-zero-parallelism': protected_body(
        253,
        protection_parameters(253, s2k=argon2_specifier(3, 0, 16)),
        b'\x6b' * 64,
    ),
    'argon2-memory-below-parallelism': protected_body(
        253,
        protection_parameters(253, s2k=argon2_specifier(3, 4, 4)),
        b'\x6c' * 64,
    ),
    'argon2-memory-over-limit': protected_body(
        253,
        protection_parameters(253, s2k=argon2_specifier(3, 1, 32)),
        b'\x6d' * 64,
    ),
    'wrong-nonce-length': protected_body(
        253,
        bytes((7, 2, len(s2k_specifiers[3]))) + s2k_specifiers[3] + b'\x31' * 14,
        b'\x5f' * 64,
    ),
    'wrong-iv-length': protected_body(
        254,
        bytes((7, len(s2k_specifiers[3]))) + s2k_specifiers[3] + b'\x37' * 15,
        b'\x60' * 64,
    ),
    'insufficient-aead-payload': protected_body(253, valid_253, b'\x52' * 27),
    'insufficient-sha1-payload': protected_body(254, valid_254, b'\x52' * 31),
    'underdeclared-public-material': (
        v6_public[:6] + (len(v6_public_material) - 1).to_bytes(4, 'big') +
        v6_public_material + b'\x00' + secret
    ),
    'overdeclared-public-material': (
        v6_public[:6] + (len(v6_public_material) + 1).to_bytes(4, 'big') +
        v6_public_material + b'\x00' + secret
    ),
    'unbounded-public-material': (
        v6_public[:6] + (128 * 1024 + 1).to_bytes(4, 'big') +
        v6_public_material + b'\x00' + secret
    ),
    'zero-native-public-length': (
        b'\x06' + (0).to_bytes(4, 'big') + b'\x19' + (0).to_bytes(4, 'big') +
        b'\x00' + native_secret_values[25]
    ),
}
for algorithm, octets in native_sizes.items():
    v4_public_key = native_public(4, algorithm, native_public_values[algorithm])
    pass_bodies[f'v4-native-{algorithm}-insufficient-protected-253'] = (
        v4_aead_body(
            valid_v4_253,
            b'\x78' * (octets + 16 - 1),
            v4_public_key,
        )
    )
    for protection, parameters in valid_v4_cfb.items():
        trailer_octets = 20 if protection == 254 else 2
        pass_bodies[
            f'v4-native-{algorithm}-insufficient-protected-{protection}'
        ] = v4_cfb_body(
            protection,
            parameters,
            b'\x7c' * (octets + trailer_octets - 1),
            v4_public_key,
        )
    public_key = native_public(6, algorithm, native_public_values[algorithm])
    for usage, parameters in valid_parameters.items():
        trailer_octets = 16 if usage == 253 else 20
        pass_bodies[f'native-{algorithm}-insufficient-protected-{usage}'] = (
            protected_body(
                usage,
                parameters,
                b'\x66' * (octets + trailer_octets - 1),
                public_key,
            )
        )
native_25_public = native_public_values[25]
native_25_header = b'\x06' + (0).to_bytes(4, 'big') + b'\x19'
underdeclared_native_public = (
    native_25_header + (31).to_bytes(4, 'big') + native_25_public +
    b'\x00' + native_secret_values[25]
)
overdeclared_native_public = (
    native_25_header + (33).to_bytes(4, 'big') + native_25_public +
    b'\x00' + native_secret_values[25]
)
write_reclassified_rejection_matrix(
    'underdeclared-native-public', underdeclared_native_public
)
write_reclassified_rejection_matrix(
    'overdeclared-native-public', overdeclared_native_public
)
if reclassified_rejection_count != 120:
    raise AssertionError('unexpected native fixture reclassification count')
for name, pass_body in pass_bodies.items():
    for tag in (5, 7):
        for framing, encode in (('fixed', fixed_packet), ('partial', partial_packet)):
            output = f'{name}-{framing}-tag{tag}.pgp'
            (v6_pass_control_dir / output).write_bytes(encode(tag, pass_body))

max_body_bytes = 128 * 1024
max_fragmentation_path = pathlib.Path(sys.argv[27])
cap_pass_dir = byte_cap_control_dir / 'pass'
cap_reject_dir = byte_cap_control_dir / 'reject'
cap_pass_dir.mkdir()
cap_reject_dir.mkdir()

def capped_partial_prefix(tag: int, prefix: bytes) -> bytes:
    return bytes([0xC0 | tag, 224 + 30]) + prefix


(cap_pass_dir / 'arbitrary-tag5.pgp').write_bytes(
    capped_partial_prefix(5, b'\x80' * (max_body_bytes + 32))
)
public_only_prefix = public + b'\x80'
(cap_pass_dir / 'public-layout-tag7.pgp').write_bytes(capped_partial_prefix(
    7,
    public_only_prefix + b'\x80' * (max_body_bytes + 32 - len(public_only_prefix)),
))
native_public_only = native_public(6, 25, native_public_values[25]) + b'\x0e'
fixed_over_cap_body = native_public_only + b'\x80' * (
    max_body_bytes + 1 - len(native_public_only)
)
if len(fixed_over_cap_body) != max_body_bytes + 1:
    raise AssertionError('fixed over-cap fixture has the wrong body length')
(cap_pass_dir / 'fixed-over-max-native-public-only-tag7.pgp').write_bytes(
    fixed_packet(7, fixed_over_cap_body)
)

v4_private_over_cap = private_algorithm_public(4, 100, b'')
v4_private_over_cap += b'\x42' * (
    max_body_bytes + 1 - len(v4_private_over_cap)
)
v6_private_over_cap = private_algorithm_public(
    6, 110, b'\x43' * max_body_bytes
) + b'\x00\x51\x52\x53'
private_cipher_over_cap = protected_body(
    253,
    protection_parameters(253, cipher=100),
    b'\x44' * max_body_bytes,
)
over_cap_packets = {
    'v4-private-algorithm-over-cap-new': fixed_packet(5, v4_private_over_cap),
    'v4-private-algorithm-over-cap-old': old_fixed_packet(
        5, len(v4_private_over_cap), v4_private_over_cap
    ),
    'v6-private-algorithm-over-cap-after-tag60': (
        fixed_packet(60, b'benign-openpgp-prefix') +
        fixed_packet(5, v6_private_over_cap)
    ),
    'v6-private-cipher-over-cap': fixed_packet(5, private_cipher_over_cap),
    'v6-private-cipher-over-cap-after-tag63': (
        fixed_packet(63, b'benign-openpgp-prefix') +
        fixed_packet(5, private_cipher_over_cap)
    ),
    'v6-private-algorithm-after-over-cap-tag60': (
        fixed_packet(60, b'\x45' * (max_body_bytes + 1)) +
        private_algorithm_packets[(6, 100)]
    ),
}
for name, packet in over_cap_packets.items():
    (version_control_dir / f'{name}.pgp').write_bytes(packet)
max_fragmentation_body = native_public_only + b'\x80' * (
    max_body_bytes - len(native_public_only)
)
if len(max_fragmentation_body) != max_body_bytes:
    raise AssertionError('fragmentation fixture has the wrong body length')
max_fragmentation_path.write_bytes(
    partial_packet(7, max_fragmentation_body, chunk_size=1)
)
(cap_reject_dir / 'recoverable-secret-tag5.pgp').write_bytes(capped_partial_prefix(
    5,
    body + b'\x80' * (max_body_bytes + 32 - len(body)),
))
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

openpgp_old_indeterminate="$work_dir/openpgp-old-indeterminate"
copy_fixture "$base_arm64" "$openpgp_old_indeterminate"
cp "$openpgp_old_packet" "$openpgp_old_indeterminate/assets/private-key.pgp"
openpgp_old_indeterminate_apk="$work_dir/openpgp-old-indeterminate.apk"
make_apk "$openpgp_old_indeterminate" "$openpgp_old_indeterminate_apk"
expect_failure_reason openpgp-old-indeterminate \
  "$openpgp_old_indeterminate_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

openpgp_partial="$work_dir/openpgp-partial"
copy_fixture "$base_arm64" "$openpgp_partial"
cp "$openpgp_partial_packet" "$openpgp_partial/assets/private-key.pgp"
openpgp_partial_apk="$work_dir/openpgp-partial.apk"
make_apk "$openpgp_partial" "$openpgp_partial_apk"
expect_failure_reason openpgp-partial "$openpgp_partial_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

openpgp_malformed="$work_dir/openpgp-malformed-partial"
copy_fixture "$base_arm64" "$openpgp_malformed"
cp "$openpgp_malformed_partial" "$openpgp_malformed/assets/collision.pgp"
openpgp_malformed_apk="$work_dir/openpgp-malformed-partial.apk"
make_apk "$openpgp_malformed" "$openpgp_malformed_apk"
verify_apk "$openpgp_malformed_apk" arm64-v8a >/dev/null

openpgp_partial_subkey="$work_dir/openpgp-partial-subkey-collision"
copy_fixture "$base_arm64" "$openpgp_partial_subkey"
cp "$openpgp_partial_subkey_collision" \
  "$openpgp_partial_subkey/assets/partial-subkey-collision.pgp"
openpgp_partial_subkey_apk="$work_dir/openpgp-partial-subkey-collision.apk"
make_apk "$openpgp_partial_subkey" "$openpgp_partial_subkey_apk"
verify_apk "$openpgp_partial_subkey_apk" arm64-v8a >/dev/null

openpgp_partial_subkey_complete_dir="$work_dir/openpgp-partial-subkey-complete"
copy_fixture "$base_arm64" "$openpgp_partial_subkey_complete_dir"
cp "$openpgp_partial_subkey_complete" \
  "$openpgp_partial_subkey_complete_dir/assets/partial-subkey.pgp"
openpgp_partial_subkey_complete_apk="$work_dir/openpgp-partial-subkey-complete.apk"
make_apk "$openpgp_partial_subkey_complete_dir" \
  "$openpgp_partial_subkey_complete_apk"
expect_failure_reason openpgp-partial-subkey-complete \
  "$openpgp_partial_subkey_complete_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

openpgp_partial_subkey_recoverable_dir="$work_dir/openpgp-partial-subkey-recoverable"
copy_fixture "$base_arm64" "$openpgp_partial_subkey_recoverable_dir"
cp "$openpgp_partial_subkey_recoverable" \
  "$openpgp_partial_subkey_recoverable_dir/assets/recoverable-subkey.pgp"
openpgp_partial_subkey_recoverable_apk="$work_dir/openpgp-partial-subkey-recoverable.apk"
make_apk "$openpgp_partial_subkey_recoverable_dir" \
  "$openpgp_partial_subkey_recoverable_apk"
expect_failure_reason openpgp-partial-subkey-recoverable \
  "$openpgp_partial_subkey_recoverable_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

for checksum_variant in no-checksum one-checksum; do
  openpgp_checksum_dir="$work_dir/openpgp-partial-subkey-$checksum_variant"
  copy_fixture "$base_arm64" "$openpgp_checksum_dir"
  cp "$work_dir/openpgp-partial-subkey-$checksum_variant.pgp" \
    "$openpgp_checksum_dir/assets/recoverable-subkey.pgp"
  openpgp_checksum_apk="$work_dir/openpgp-partial-subkey-$checksum_variant.apk"
  make_apk "$openpgp_checksum_dir" "$openpgp_checksum_apk"
  expect_failure_reason "openpgp-partial-subkey-$checksum_variant" \
    "$openpgp_checksum_apk" arm64-v8a \
    'private signing, deployment, or runtime secret material'
done

for boundary_variant in \
  no-checksum one-checksum zero-checksum trailing over-128-one-byte-chunks; do
  openpgp_complete_boundary_dir="$work_dir/openpgp-partial-complete-$boundary_variant"
  copy_fixture "$base_arm64" "$openpgp_complete_boundary_dir"
  cp "$openpgp_partial_subkey_complete_variants/$boundary_variant.pgp" \
    "$openpgp_complete_boundary_dir/assets/recoverable-subkey.pgp"
  openpgp_complete_boundary_apk="$work_dir/openpgp-partial-complete-$boundary_variant.apk"
  make_apk "$openpgp_complete_boundary_dir" "$openpgp_complete_boundary_apk"
  expect_failure_reason "openpgp-partial-complete-$boundary_variant" \
    "$openpgp_complete_boundary_apk" arm64-v8a \
    'private signing, deployment, or runtime secret material'
done

openpgp_fixed_subkey="$work_dir/openpgp-fixed-subkey-control"
copy_fixture "$base_arm64" "$openpgp_fixed_subkey"
cp "$openpgp_fixed_subkey_control" \
  "$openpgp_fixed_subkey/assets/fixed-subkey-control.pgp"
openpgp_fixed_subkey_apk="$work_dir/openpgp-fixed-subkey-control.apk"
make_apk "$openpgp_fixed_subkey" "$openpgp_fixed_subkey_apk"
expect_failure_reason openpgp-fixed-subkey-control \
  "$openpgp_fixed_subkey_apk" arm64-v8a \
  'private signing, deployment, or runtime secret material'

openpgp_embedded_payloads="$work_dir/openpgp-embedded-context-payloads"
mkdir -p "$openpgp_embedded_payloads"
python3 - "$openpgp_embedded_payloads" <<'PY'
import pathlib
import sys


def mpi(value: int) -> bytes:
    encoded = value.to_bytes((value.bit_length() + 7) // 8, 'big')
    return value.bit_length().to_bytes(2, 'big') + encoded


def packet_length(length: int) -> bytes:
    if length < 192:
        return bytes([length])
    if length <= 8383:
        adjusted = length - 192
        return bytes([192 + (adjusted >> 8), adjusted & 0xFF])
    return b'\xff' + length.to_bytes(4, 'big')


def fixed_packet(tag: int, body: bytes, declared_length=None) -> bytes:
    length = len(body) if declared_length is None else declared_length
    return bytes([0xC0 | tag]) + packet_length(length) + body


def partial_packet(tag: int, body: bytes, chunk_size: int = 64) -> bytes:
    exponent = chunk_size.bit_length() - 1
    output = bytearray([0xC0 | tag])
    cursor = 0
    while len(body) - cursor > chunk_size:
        output.append(224 + exponent)
        output.extend(body[cursor:cursor + chunk_size])
        cursor += chunk_size
    remaining = body[cursor:]
    output.extend((len(remaining),))
    output.extend(remaining)
    return bytes(output)


def rsa_public(version: int, modulus: int, exponent: int) -> bytes:
    material = mpi(modulus) + mpi(exponent)
    if version == 3:
        return b'\x03' + (0).to_bytes(4, 'big') + b'\x00\x00\x01' + material
    if version == 4:
        return b'\x04' + (0).to_bytes(4, 'big') + b'\x01' + material
    raise ValueError('unsupported embedded RSA fixture version')


n = (1 << 511) + 0x12345
e = 65537
d = (1 << 510) + 0x23457
p = (1 << 255) + 0x34567
q = (1 << 255) + 0x45679
u = (1 << 254) + 0x56789
rsa_secret = mpi(d) + mpi(p) + mpi(q) + mpi(u)

elgamal_prime = (1 << 1023) + 0xC5D7
elgamal_generator = 2
elgamal_secret = (1 << 255) + 0x6A5B
elgamal_value = pow(elgamal_generator, elgamal_secret, elgamal_prime)
elgamal_public = (
    b'\x04' + (0).to_bytes(4, 'big') + b'\x14' +
    mpi(elgamal_prime) + mpi(elgamal_generator) + mpi(elgamal_value)
)
elgamal_secret = mpi(elgamal_secret)

native_public_value = bytes(range(1, 33))
native_secret = bytes(range(65, 97))
native_public = (
    b'\x06' + (0).to_bytes(4, 'big') + b'\x19' +
    len(native_public_value).to_bytes(4, 'big') + native_public_value
)

output = pathlib.Path(sys.argv[1])
for name, public, secret, checksum_required in (
    ('v3-rsa', rsa_public(3, n, e), rsa_secret, True),
    ('v4-rsa', rsa_public(4, n, e), rsa_secret, True),
    ('v4-elgamal20', elgamal_public, elgamal_secret, True),
    ('v6-native25', native_public, native_secret, False),
):
    if checksum_required:
        (output / f'{name}-checksum-bytes.txt').write_text(
            str(len(secret)), encoding='ascii'
        )
    checksum = (
        (sum(secret) & 0xFFFF).to_bytes(2, 'big')
        if checksum_required else b''
    )
    body = public + b'\x00' + secret + checksum
    for tag in (5, 7):
        fixtures = {
            'valid-fixed': fixed_packet(tag, body),
            'valid-partial': partial_packet(tag, body),
            'outer-overdeclared': fixed_packet(tag, body, len(body) + 1),
        }
        if checksum_required:
            bad_checksum = body[:-1] + bytes([body[-1] ^ 1])
            fixtures['bad-checksum-fixed'] = fixed_packet(tag, bad_checksum)
        else:
            for variant, declared_length in (
                ('public-underdeclared-fixed', len(native_public_value) - 1),
                ('public-overdeclared-fixed', len(native_public_value) + 1),
            ):
                malformed_public = (
                    public[:6] + declared_length.to_bytes(4, 'big') + public[10:]
                )
                fixtures[variant] = fixed_packet(
                    tag, malformed_public + b'\x00' + secret
                )
        for variant, packet in fixtures.items():
            (output / f'{name}-{variant}-tag{tag}.pgp').write_bytes(packet)
PY

for abi in arm64-v8a armeabi-v7a; do
  if [[ "$abi" == arm64-v8a ]]; then
    embedded_base="$base_arm64"
  else
    embedded_base="$base_armv7"
  fi
  for key_shape in v3-rsa v4-rsa v4-elgamal20 v6-native25; do
    for tag in 5 7; do
      variants=(valid-fixed valid-partial outer-overdeclared)
      if [[ "$key_shape" == v6-native25 ]]; then
        variants+=(public-underdeclared-fixed public-overdeclared-fixed)
      else
        variants+=(bad-checksum-fixed)
      fi
      for variant in "${variants[@]}"; do
        embedded_label="openpgp-embedded-$abi-$key_shape-$variant-tag$tag"
        embedded_fixture="$work_dir/$embedded_label"
        embedded_library="lib/$abi/libhelper.so"
        copy_fixture "$embedded_base" "$embedded_fixture"
        cat "$openpgp_embedded_payloads/$key_shape-$variant-tag$tag.pgp" >> \
          "$embedded_fixture/$embedded_library"
        if [[ "$variant" == 'outer-overdeclared' ]]; then
          printf '\0benign-elf-suffix' >> "$embedded_fixture/$embedded_library"
        fi
        embedded_apk="$work_dir/$embedded_label.apk"
        make_apk "$embedded_fixture" "$embedded_apk"
        if [[ "$variant" == 'valid-fixed' ]]; then
          expect_failure_diagnostic \
            "$embedded_label" \
            "$embedded_apk" \
            "$abi" \
            "$embedded_library" \
            'raw-private-format'
        elif [[ "$variant" == 'bad-checksum-fixed' ]]; then
          checksum_bytes="$(
            cat "$openpgp_embedded_payloads/$key_shape-checksum-bytes.txt"
          )"
          verify_apk "$embedded_apk" "$abi" \
            JUMPGATE_TEST_OPENPGP_EXPECT_CHECKSUM_BYTES="$checksum_bytes" \
            >/dev/null
        else
          verify_apk "$embedded_apk" "$abi" >/dev/null
        fi
      done
    done
  done
done

for version_control in "$openpgp_version_controls"/*.pgp; do
  control_name="${version_control##*/}"
  control_name="${control_name%.pgp}"
  control_fixture="$work_dir/openpgp-$control_name"
  copy_fixture "$base_arm64" "$control_fixture"
  cp "$version_control" "$control_fixture/assets/$control_name.pgp"
  control_apk="$work_dir/openpgp-$control_name.apk"
  make_apk "$control_fixture" "$control_apk"
  if [[ "$control_name" == 'v4-underdeclared-new-tag5' ]]; then
    expect_failure_diagnostic \
      "openpgp-$control_name" \
      "$control_apk" \
      arm64-v8a \
      "assets/$control_name.pgp" \
      'raw-private-format'
  else
    expect_failure_reason \
      "openpgp-$control_name" \
      "$control_apk" \
      arm64-v8a \
      'private signing, deployment, or runtime secret material'
  fi
done

openpgp_v6_pass="$work_dir/openpgp-v6-malformed-controls"
copy_fixture "$base_arm64" "$openpgp_v6_pass"
mkdir -p "$openpgp_v6_pass/assets/openpgp-v6-controls"
cp "$openpgp_v6_pass_controls"/*.pgp \
  "$openpgp_v6_pass/assets/openpgp-v6-controls/"
openpgp_v6_pass_apk="$work_dir/openpgp-v6-malformed-controls.apk"
make_apk "$openpgp_v6_pass" "$openpgp_v6_pass_apk"
verify_apk "$openpgp_v6_pass_apk" arm64-v8a >/dev/null

openpgp_byte_cap_pass="$work_dir/openpgp-byte-cap-pass"
copy_fixture "$base_arm64" "$openpgp_byte_cap_pass"
mkdir -p "$openpgp_byte_cap_pass/assets/openpgp-byte-cap"
cp "$openpgp_byte_cap_controls/pass"/*.pgp \
  "$openpgp_byte_cap_pass/assets/openpgp-byte-cap/"
openpgp_byte_cap_pass_apk="$work_dir/openpgp-byte-cap-pass.apk"
make_apk "$openpgp_byte_cap_pass" "$openpgp_byte_cap_pass_apk"
verify_apk "$openpgp_byte_cap_pass_apk" arm64-v8a >/dev/null

openpgp_max_fragmentation_fixture="$work_dir/openpgp-max-fragmentation"
copy_fixture "$base_arm64" "$openpgp_max_fragmentation_fixture"
cp "$openpgp_max_fragmentation" \
  "$openpgp_max_fragmentation_fixture/assets/max-fragmentation.pgp"
openpgp_max_fragmentation_apk="$work_dir/openpgp-max-fragmentation.apk"
make_apk "$openpgp_max_fragmentation_fixture" "$openpgp_max_fragmentation_apk"
# One candidate operation plus 131071 one-byte partial chunks reaches the cap.
verify_apk "$openpgp_max_fragmentation_apk" arm64-v8a \
  JUMPGATE_TEST_OPENPGP_EXPECT_OPERATIONS=131072 >/dev/null

openpgp_byte_cap_reject="$work_dir/openpgp-byte-cap-reject"
copy_fixture "$base_arm64" "$openpgp_byte_cap_reject"
cp "$openpgp_byte_cap_controls/reject/recoverable-secret-tag5.pgp" \
  "$openpgp_byte_cap_reject/assets/recoverable-secret-tag5.pgp"
openpgp_byte_cap_reject_apk="$work_dir/openpgp-byte-cap-reject.apk"
make_apk "$openpgp_byte_cap_reject" "$openpgp_byte_cap_reject_apk"
expect_failure_reason \
  openpgp-byte-cap-recoverable \
  "$openpgp_byte_cap_reject_apk" \
  arm64-v8a \
  'private signing, deployment, or runtime secret material'

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
python3 - "$symlink_apk" "$expected_core_library" <<'PY'
import pathlib
import stat
import sys
import zipfile

link = zipfile.ZipInfo(f'lib/arm64-v8a/{sys.argv[2]}')
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

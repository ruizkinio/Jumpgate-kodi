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

base_arm64="$work_dir/base-arm64"
base_armv7="$work_dir/base-armv7"
compile_shared "$base_arm64/lib/arm64-v8a/libkodi.so" arm64-v8a
compile_shared "$base_arm64/lib/arm64-v8a/libhelper.so" arm64-v8a
compile_shared "$base_armv7/lib/armeabi-v7a/libkodi.so" armeabi-v7a
compile_shared "$base_armv7/lib/armeabi-v7a/libhelper.so" armeabi-v7a
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

json_secret="$work_dir/json-secret"
copy_fixture "$base_arm64" "$json_secret"
json_secret_value='s3cr3t!'
printf '{"nested":{"apiKey":"%s"}}\n' "$json_secret_value" > \
  "$json_secret/assets/private.json"
json_secret_apk="$work_dir/json-secret.apk"
make_apk "$json_secret" "$json_secret_apk"
expect_failure_without_value json-short-secret "$json_secret_apk" arm64-v8a \
  "$json_secret_value"

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
yaml_secret_value='0123456789abcdef0123456789ABCDEF'
printf 'refresh-token: %s\n' "$yaml_secret_value" > \
  "$yaml_secret/assets/private.yaml"
yaml_secret_apk="$work_dir/yaml-secret.apk"
make_apk "$yaml_secret" "$yaml_secret_apk"
expect_failure_without_value yaml-unquoted-secret "$yaml_secret_apk" arm64-v8a \
  "$yaml_secret_value"

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
expect_failure_without_value text-secret "$text_secret_apk" arm64-v8a \
  "$text_secret_value"

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

private_key="$work_dir/private-key"
copy_fixture "$base_arm64" "$private_key"
printf '%s\n' '-----BEGIN PRIVATE KEY-----' > "$private_key/assets/private.txt"
private_key_apk="$work_dir/private-key.apk"
make_apk "$private_key" "$private_key_apk"
expect_failure private-key "$private_key_apk" arm64-v8a

printf 'APK verifier real-tool and adversarial tests: passed\n'

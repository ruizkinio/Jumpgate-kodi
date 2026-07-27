#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail
export LC_ALL=C

usage() {
  echo "usage: $0 <arm64-apk> <arm64-sbom> <armv7-apk> <armv7-sbom> <expected-package> <expected-min-sdk> <expected-target-sdk> <expected-version-name> <expected-version-code> <expected-core-library> <expected-signer-sha256> <source-sha> <reviewed-ref> <release-tag> <run-id> <run-attempt> <output-dir>" >&2
  exit 2
}

fail() {
  echo "$1" >&2
  exit 1
}

[[ "$#" -eq 17 ]] || usage

arm64_apk="$1"
arm64_sbom="$2"
armv7_apk="$3"
armv7_sbom="$4"
expected_package="$5"
expected_min_sdk="$6"
expected_target_sdk="$7"
expected_version_name="$8"
expected_version_code="$9"
expected_core_library="${10}"
expected_signer_sha256="${11,,}"
source_sha="${12,,}"
reviewed_ref="${13}"
release_tag="${14}"
run_id="${15}"
run_attempt="${16}"
output_dir="${17}"

[[ "$expected_package" =~ ^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$ ]] ||
  fail 'Invalid expected Android package'
[[ "$expected_min_sdk" =~ ^[0-9]+$ && "$expected_target_sdk" =~ ^[0-9]+$ ]] ||
  fail 'Invalid expected Android SDK metadata'
[[ "$expected_version_name" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$ ]] ||
  fail 'Invalid expected Android version name'
[[ "$expected_version_name" =~ -Jumpgate-((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*))$ ]] ||
  fail 'Expected Android version name lacks a canonical Jumpgate semantic version'
jumpgate_semver="${BASH_REMATCH[1]}"
[[ "$expected_version_code" =~ ^(0|[1-9][0-9]{0,9})$ ]] ||
  fail 'Invalid expected Android version code'
if [[ "$expected_version_code" == 0 ||
      ${#expected_version_code} -gt 10 ||
      (${#expected_version_code} -eq 10 &&
       "$expected_version_code" > 2100000000) ]]; then
  fail 'Expected Android version code is outside range 1..2100000000'
fi
[[ "$expected_core_library" =~ ^lib[A-Za-z0-9._+-]+\.so$ ]] ||
  fail 'Invalid expected Android core library name'
[[ "$expected_signer_sha256" =~ ^[0-9a-f]{64}$ ]] ||
  fail 'Invalid expected release signer digest'
[[ "$source_sha" =~ ^[0-9a-f]{40}$ ]] || fail 'Invalid source commit SHA'
[[ -n "$reviewed_ref" && ${#reviewed_ref} -le 256 &&
   "$reviewed_ref" != *$'\n'* && "$reviewed_ref" != *$'\r'* ]] ||
  fail 'Invalid reviewed source ref'
[[ "$release_tag" == "v$jumpgate_semver" ]] ||
  fail 'Release tag does not match the canonical Jumpgate semantic version'
[[ "$run_id" =~ ^[1-9][0-9]*$ && "$run_attempt" =~ ^[1-9][0-9]*$ ]] ||
  fail 'Invalid GitHub run identity'
[[ -d "$output_dir" && ! -L "$output_dir" ]] || fail 'Release output directory is invalid'
output_dir="$(cd "$output_dir" && pwd -P)"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
apk_verifier="$script_dir/verify-android-apk.sh"
[[ -f "$apk_verifier" && ! -L "$apk_verifier" ]] || fail 'APK verifier is unavailable'
: "${ANDROID_BUILD_TOOLS_ROOT:?ANDROID_BUILD_TOOLS_ROOT is required}"
aapt2="$ANDROID_BUILD_TOOLS_ROOT/aapt2"
apksigner="$ANDROID_BUILD_TOOLS_ROOT/apksigner"
test -x "$aapt2"
test -x "$apksigner"
command -v python3 >/dev/null
command -v sha256sum >/dev/null

arm64_apk_name="Jumpgate-${expected_version_name}-arm64-v8a.apk"
arm64_sbom_name="Jumpgate-${expected_version_name}-arm64-v8a.spdx.json"
armv7_apk_name="Jumpgate-${expected_version_name}-armeabi-v7a.apk"
armv7_sbom_name="Jumpgate-${expected_version_name}-armeabi-v7a.spdx.json"
metadata_name="Jumpgate-${expected_version_name}-metadata.json"
checksums_name='SHA256SUMS'
metadata_path="$output_dir/$metadata_name"
checksums_path="$output_dir/$checksums_name"

[[ "$(basename "$arm64_apk")" == "$arm64_apk_name" ]] ||
  fail 'arm64-v8a APK has a non-canonical public name'
[[ "$(basename "$arm64_sbom")" == "$arm64_sbom_name" ]] ||
  fail 'arm64-v8a SBOM has a non-canonical public name'
[[ "$(basename "$armv7_apk")" == "$armv7_apk_name" ]] ||
  fail 'armeabi-v7a APK has a non-canonical public name'
[[ "$(basename "$armv7_sbom")" == "$armv7_sbom_name" ]] ||
  fail 'armeabi-v7a SBOM has a non-canonical public name'

for release_file in "$arm64_apk" "$arm64_sbom" "$armv7_apk" "$armv7_sbom"; do
  [[ -f "$release_file" && -s "$release_file" && ! -L "$release_file" ]] ||
    fail 'Release input is not a non-empty regular file'
  release_parent="$(cd "$(dirname "$release_file")" && pwd -P)"
  [[ "$release_parent" == "$output_dir" ]] ||
    fail 'Release inputs must be staged directly in the output directory'
done
[[ "$arm64_apk" != "$armv7_apk" && "$arm64_sbom" != "$armv7_sbom" ]] ||
  fail 'Release ABI inputs must be distinct'
[[ ! -e "$metadata_path" && ! -e "$checksums_path" ]] ||
  fail 'Release metadata or SHA256SUMS already exists; refusing to overwrite'

work_dir="$(mktemp -d)"
metadata_tmp="$work_dir/metadata.json"
checksums_tmp="$work_dir/SHA256SUMS"
trap 'rm -rf "$work_dir"' EXIT

recorded_apk_hash() {
  local apk="$1"
  local sidecar="$apk.sha256"
  local digest filename extra
  [[ -f "$sidecar" && -s "$sidecar" && ! -L "$sidecar" ]] ||
    fail 'Signed APK hash sidecar is absent or invalid'
  [[ "$(wc -l < "$sidecar")" -eq 1 ]] || fail 'Signed APK hash sidecar is malformed'
  read -r digest filename extra < "$sidecar"
  filename="${filename#\*}"
  [[ "$digest" =~ ^[0-9a-f]{64}$ && "$filename" == "$(basename "$apk")" && -z "${extra:-}" ]] ||
    fail 'Signed APK hash sidecar is malformed'
  printf '%s\n' "$digest"
}

validate_sbom() {
  local sbom="$1"
  local apk_name="$2"
  local apk_hash="$3"
  local expected_abi="$4"
  python3 - "$sbom" "$apk_name" "$apk_hash" "$expected_abi" "$expected_version_name" <<'PY'
import json
import pathlib
import re
import sys
import urllib.parse

path = pathlib.Path(sys.argv[1])
expected_name = sys.argv[2]
expected_hash = sys.argv[3]
expected_abi = sys.argv[4]
expected_version = sys.argv[5]
expected_root_id = f'SPDXRef-Package-Jumpgate-APK-{expected_abi}'
try:
    document = json.loads(path.read_text(encoding='utf-8'))
except (OSError, UnicodeError, json.JSONDecodeError):
    raise SystemExit(1)
if not isinstance(document, dict):
    raise SystemExit(1)
if document.get('spdxVersion') != 'SPDX-2.3':
    raise SystemExit(1)
if document.get('SPDXID') != 'SPDXRef-DOCUMENT':
    raise SystemExit(1)
if document.get('name') != expected_name:
    raise SystemExit(1)
if document.get('dataLicense') != 'CC0-1.0':
    raise SystemExit(1)
namespace = document.get('documentNamespace')
if not isinstance(namespace, str) or not namespace:
    raise SystemExit(1)
parsed_namespace = urllib.parse.urlparse(namespace)
if parsed_namespace.scheme not in {'http', 'https'} or not parsed_namespace.netloc:
    raise SystemExit(1)
creation = document.get('creationInfo')
if not isinstance(creation, dict):
    raise SystemExit(1)
creators = creation.get('creators')
if not isinstance(creators, list) or not creators or not all(
    isinstance(creator, str) and creator.strip() for creator in creators
):
    raise SystemExit(1)
packages = document.get('packages')
if not isinstance(packages, list) or len(packages) < 2:
    raise SystemExit(1)

package_by_id = {}
for package in packages:
    if not isinstance(package, dict):
        raise SystemExit(1)
    package_id = package.get('SPDXID')
    package_name = package.get('name')
    if (
        not isinstance(package_id, str)
        or re.fullmatch(r'SPDXRef-[A-Za-z0-9.-]+', package_id) is None
        or package_id in package_by_id
        or not isinstance(package_name, str)
        or not package_name.strip()
    ):
        raise SystemExit(1)
    package_by_id[package_id] = package

described = document.get('documentDescribes')
if described != [expected_root_id]:
    raise SystemExit(1)
root = package_by_id.get(expected_root_id)
if not isinstance(root, dict):
    raise SystemExit(1)
if root.get('name') != expected_name or root.get('packageFileName') != expected_name:
    raise SystemExit(1)
if root.get('versionInfo') != expected_version:
    raise SystemExit(1)
if root.get('primaryPackagePurpose') != 'APPLICATION':
    raise SystemExit(1)
if root.get('filesAnalyzed') is not False:
    raise SystemExit(1)
if root.get('downloadLocation') != 'NOASSERTION':
    raise SystemExit(1)
if root.get('licenseConcluded') != 'GPL-2.0-or-later':
    raise SystemExit(1)
if root.get('licenseDeclared') != 'GPL-2.0-or-later':
    raise SystemExit(1)
if root.get('copyrightText') != 'NOASSERTION':
    raise SystemExit(1)
checksums = root.get('checksums')
if checksums != [{'algorithm': 'SHA256', 'checksumValue': expected_hash}]:
    raise SystemExit(1)

inventory_ids = set(package_by_id) - {expected_root_id}
if not inventory_ids:
    raise SystemExit(1)
relationships = document.get('relationships')
if not isinstance(relationships, list) or not relationships:
    raise SystemExit(1)
document_describes = []
root_contains = []
for relationship in relationships:
    if not isinstance(relationship, dict):
        raise SystemExit(1)
    element = relationship.get('spdxElementId')
    relationship_type = relationship.get('relationshipType')
    related = relationship.get('relatedSpdxElement')
    if not all(isinstance(value, str) and value for value in (element, relationship_type, related)):
        raise SystemExit(1)
    if element == 'SPDXRef-DOCUMENT' and relationship_type == 'DESCRIBES':
        document_describes.append(related)
    if element == expected_root_id and relationship_type == 'CONTAINS':
        root_contains.append(related)
if document_describes != [expected_root_id]:
    raise SystemExit(1)
if len(root_contains) != len(set(root_contains)) or set(root_contains) != inventory_ids:
    raise SystemExit(1)
PY
}

inspect_apk() {
  local apk="$1"
  local expected_abi="$2"
  local badging="$work_dir/$expected_abi.badging"
  local signer_output="$work_dir/$expected_abi.signer"
  local package version_code version_name min_sdk target_sdk signer

  "$aapt2" dump badging "$apk" | tr -d '\r' > "$badging"
  package="$(sed -n "s/^package: name='\([^']*\)'.*/\1/p" "$badging")"
  version_code="$(sed -n "s/^package:.*versionCode='\([^']*\)'.*/\1/p" "$badging")"
  version_name="$(sed -n "s/^package:.*versionName='\([^']*\)'.*/\1/p" "$badging")"
  min_sdk="$(sed -n "s/^minSdkVersion:'\([^']*\)'.*/\1/p" "$badging")"
  target_sdk="$(sed -n "s/^targetSdkVersion:'\([^']*\)'.*/\1/p" "$badging")"
  [[ -n "$package" && "$package" != *$'\n'* ]] || fail 'APK package metadata is ambiguous'
  [[ -n "$version_code" && "$version_code" != *$'\n'* ]] ||
    fail 'APK version code metadata is ambiguous'
  [[ -n "$version_name" && "$version_name" != *$'\n'* ]] ||
    fail 'APK version name metadata is ambiguous'
  [[ -n "$min_sdk" && "$min_sdk" != *$'\n'* ]] || fail 'APK minimum SDK metadata is ambiguous'
  [[ -n "$target_sdk" && "$target_sdk" != *$'\n'* ]] ||
    fail 'APK target SDK metadata is ambiguous'

  "$apksigner" verify --Werr --verbose --print-certs "$apk" |
    tr -d '\r' > "$signer_output"
  grep -Fxq 'Verified using v2 scheme (APK Signature Scheme v2): true' "$signer_output" ||
    fail 'APK is not signed with APK Signature Scheme v2'
  signer="$(
    sed -n 's/^Signer #[0-9][0-9]* certificate SHA-256 digest: //p' "$signer_output" |
      tr '[:upper:]' '[:lower:]' |
      tr -d ':'
  )"
  [[ "$signer" =~ ^[0-9a-f]{64}$ ]] || fail 'APK signer metadata is ambiguous'

  python3 - "$apk" "$expected_abi" "$expected_core_library" <<'PY'
import pathlib
import sys
import zipfile

apk = pathlib.Path(sys.argv[1])
expected_abi = sys.argv[2]
expected_core_library = sys.argv[3]
try:
    with zipfile.ZipFile(apk) as archive:
        libraries = [
            info.filename
            for info in archive.infolist()
            if not info.is_dir()
            and info.filename.startswith('lib/')
            and info.filename.endswith('.so')
        ]
except (OSError, RuntimeError, zipfile.BadZipFile):
    raise SystemExit(1)
abis = {
    name.split('/', 2)[1]
    for name in libraries
    if len(name.split('/', 2)) == 3
}
if abis != {expected_abi}:
    raise SystemExit(1)
if f'lib/{expected_abi}/{expected_core_library}' not in libraries:
    raise SystemExit(1)
PY

  printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$package" "$version_name" "$version_code" "$min_sdk" "$target_sdk" "$signer"
}

arm64_recorded_hash="$(recorded_apk_hash "$arm64_apk")"
armv7_recorded_hash="$(recorded_apk_hash "$armv7_apk")"
arm64_hash="$(sha256sum "$arm64_apk" | cut -d ' ' -f 1)"
armv7_hash="$(sha256sum "$armv7_apk" | cut -d ' ' -f 1)"
[[ "$arm64_hash" == "$arm64_recorded_hash" ]] ||
  fail 'arm64-v8a APK does not match its signing-job hash'
[[ "$armv7_hash" == "$armv7_recorded_hash" ]] ||
  fail 'armeabi-v7a APK does not match its signing-job hash'

bash "$apk_verifier" \
  "$arm64_apk" arm64-v8a "$expected_package" "$expected_min_sdk" \
  "$expected_target_sdk" "$expected_version_name" "$expected_version_code" \
  "$expected_signer_sha256" "$expected_core_library"
bash "$apk_verifier" \
  "$armv7_apk" armeabi-v7a "$expected_package" "$expected_min_sdk" \
  "$expected_target_sdk" "$expected_version_name" "$expected_version_code" \
  "$expected_signer_sha256" "$expected_core_library"
[[ "$(sha256sum "$arm64_apk" | cut -d ' ' -f 1)" == "$arm64_hash" ]] ||
  fail 'arm64-v8a APK changed during aggregate verification'
[[ "$(sha256sum "$armv7_apk" | cut -d ' ' -f 1)" == "$armv7_hash" ]] ||
  fail 'armeabi-v7a APK changed during aggregate verification'

IFS=$'\t' read -r arm64_package arm64_version_name arm64_version_code \
  arm64_min_sdk arm64_target_sdk arm64_signer < <(inspect_apk "$arm64_apk" arm64-v8a)
IFS=$'\t' read -r armv7_package armv7_version_name armv7_version_code \
  armv7_min_sdk armv7_target_sdk armv7_signer < <(inspect_apk "$armv7_apk" armeabi-v7a)

[[ "$arm64_package" == "$expected_package" && "$armv7_package" == "$expected_package" ]] ||
  fail 'Release APK package identity does not match the expected package'
[[ "$arm64_version_name" == "$expected_version_name" &&
   "$armv7_version_name" == "$expected_version_name" ]] ||
  fail 'Release APK version name does not match the expected version'
[[ "$arm64_version_code" == "$expected_version_code" &&
   "$armv7_version_code" == "$expected_version_code" ]] ||
  fail 'Release APK version code does not match the expected version'
[[ "$arm64_min_sdk" == "$expected_min_sdk" && "$armv7_min_sdk" == "$expected_min_sdk" ]] ||
  fail 'Release APK minimum SDK does not match the expected SDK'
[[ "$arm64_target_sdk" == "$expected_target_sdk" &&
   "$armv7_target_sdk" == "$expected_target_sdk" ]] ||
  fail 'Release APK target SDK does not match the expected SDK'
[[ "$arm64_signer" == "$expected_signer_sha256" &&
   "$armv7_signer" == "$expected_signer_sha256" ]] ||
  fail 'Release APK signer does not match the expected certificate'
[[ "$arm64_package" == "$armv7_package" &&
   "$arm64_version_name" == "$armv7_version_name" &&
   "$arm64_version_code" == "$armv7_version_code" &&
   "$arm64_min_sdk" == "$armv7_min_sdk" &&
   "$arm64_target_sdk" == "$armv7_target_sdk" &&
   "$arm64_signer" == "$armv7_signer" ]] ||
  fail 'Release ABIs do not share package, version, SDK, and signer identity'

validate_sbom "$arm64_sbom" "$arm64_apk_name" "$arm64_hash" arm64-v8a ||
  fail 'arm64-v8a SPDX SBOM is invalid or not bound to its exact APK'
validate_sbom "$armv7_sbom" "$armv7_apk_name" "$armv7_hash" armeabi-v7a ||
  fail 'armeabi-v7a SPDX SBOM is invalid or not bound to its exact APK'
arm64_sbom_hash="$(sha256sum "$arm64_sbom" | cut -d ' ' -f 1)"
armv7_sbom_hash="$(sha256sum "$armv7_sbom" | cut -d ' ' -f 1)"

python3 - \
  "$metadata_tmp" \
  "$expected_package" "$expected_version_name" "$expected_version_code" \
  "$expected_min_sdk" "$expected_target_sdk" "$expected_core_library" \
  "$expected_signer_sha256" \
  "$source_sha" "$reviewed_ref" "$release_tag" "$run_id" "$run_attempt" \
  "$arm64_apk" "$arm64_hash" "$arm64_sbom" "$arm64_sbom_hash" \
  "$armv7_apk" "$armv7_hash" "$armv7_sbom" "$armv7_sbom_hash" <<'PY'
import json
import pathlib
import sys

(
    output,
    package_name,
    version_name,
    version_code,
    min_sdk,
    target_sdk,
    core_library,
    signer,
    source_sha,
    reviewed_ref,
    release_tag,
    run_id,
    run_attempt,
    arm64_apk,
    arm64_hash,
    arm64_sbom,
    arm64_sbom_hash,
    armv7_apk,
    armv7_hash,
    armv7_sbom,
    armv7_sbom_hash,
) = sys.argv[1:]


def artifact(abi, apk_path, apk_hash, sbom_path, sbom_hash):
    apk = pathlib.Path(apk_path)
    sbom = pathlib.Path(sbom_path)
    return {
        'abi': abi,
        'mediaType': 'application/vnd.android.package-archive',
        'name': apk.name,
        'sha256': apk_hash,
        'size': apk.stat().st_size,
        'sbom': {
            'format': 'SPDX-2.3',
            'name': sbom.name,
            'sha256': sbom_hash,
            'size': sbom.stat().st_size,
        },
    }


document = {
    'android': {
        'minSdk': int(min_sdk),
        'coreNativeLibrary': core_library,
        'packageName': package_name,
        'signerCertificateSha256': signer,
        'targetSdk': int(target_sdk),
        'versionCode': int(version_code),
        'versionName': version_name,
    },
    'artifacts': [
        artifact('arm64-v8a', arm64_apk, arm64_hash, arm64_sbom, arm64_sbom_hash),
        artifact('armeabi-v7a', armv7_apk, armv7_hash, armv7_sbom, armv7_sbom_hash),
    ],
    'githubActions': {
        'runAttempt': int(run_attempt),
        'runId': int(run_id),
    },
    'license': 'GPL-2.0-or-later',
    'platform': 'android',
    'product': 'Jumpgate',
    'releaseTag': release_tag,
    'schemaVersion': 1,
    'source': {
        'commit': source_sha,
        'reviewedRef': reviewed_ref,
    },
}
pathlib.Path(output).write_text(
    json.dumps(document, ensure_ascii=True, indent=2, sort_keys=True) + '\n',
    encoding='utf-8',
)
PY

metadata_hash="$(sha256sum "$metadata_tmp" | cut -d ' ' -f 1)"
{
  printf '%s  %s\n' "$arm64_hash" "$arm64_apk_name"
  printf '%s  %s\n' "$arm64_sbom_hash" "$arm64_sbom_name"
  printf '%s  %s\n' "$armv7_hash" "$armv7_apk_name"
  printf '%s  %s\n' "$armv7_sbom_hash" "$armv7_sbom_name"
  printf '%s  %s\n' "$metadata_hash" "$metadata_name"
} | LC_ALL=C sort -k2 > "$checksums_tmp"
[[ "$(wc -l < "$checksums_tmp")" -eq 5 ]] || fail 'SHA256SUMS generation failed'

mv "$metadata_tmp" "$metadata_path"
mv "$checksums_tmp" "$checksums_path"
printf 'Verified Android release parity: package=%s version=%s versionCode=%s signer_sha256=%s\n' \
  "$expected_package" "$expected_version_name" "$expected_version_code" \
  "$expected_signer_sha256"

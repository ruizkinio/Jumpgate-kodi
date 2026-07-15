#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_validator="$script_dir/verify-android-release.sh"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

test -f "$source_validator"
command -v python3 >/dev/null
command -v sha256sum >/dev/null

fixture_tools="$work_dir/tools"
mock_build_tools="$work_dir/android-build-tools"
mkdir -p "$fixture_tools" "$mock_build_tools"
cp "$source_validator" "$fixture_tools/verify-android-release.sh"

cat > "$fixture_tools/verify-android-apk.sh" <<'FAKE_VERIFIER'
#!/usr/bin/env bash
set -euo pipefail
[[ "$#" -eq 9 ]]
apk="$1"
abi="$2"
[[ "$abi" == 'arm64-v8a' || "$abi" == 'armeabi-v7a' ]]
[[ "$3" == 'com.example.fixtureplayer' ]]
[[ "$4" == '24' && "$5" == '36' ]]
[[ "$6" == '22.0-FIXTURE-Jumpgate-1.0.0' &&
   "$7" == "${MOCK_EXPECTED_VERSION_CODE:-22000001}" ]]
[[ "$8" == 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' ]]
[[ "$9" == 'libfixture.so' ]]
(
  cd "$(dirname "$apk")"
  sha256sum "$(basename "$apk")" > "$(basename "$apk").sha256"
)
printf 'fake verifier accepted %s\n' "$abi"
FAKE_VERIFIER

cat > "$mock_build_tools/aapt2" <<'MOCK_AAPT2'
#!/usr/bin/env bash
set -euo pipefail
[[ "$#" -eq 3 && "$1" == 'dump' && "$2" == 'badging' ]]
apk="$3"
package='com.example.fixtureplayer'
version_name='22.0-FIXTURE-Jumpgate-1.0.0'
version_code="${MOCK_VERSION_CODE:-22000001}"
min_sdk='24'
target_sdk='36'
if [[ "$apk" == *'armeabi-v7a'* ]]; then
  package="${MOCK_ARMV7_PACKAGE:-$package}"
  version_name="${MOCK_ARMV7_VERSION_NAME:-$version_name}"
  version_code="${MOCK_ARMV7_VERSION_CODE:-$version_code}"
  min_sdk="${MOCK_ARMV7_MIN_SDK:-$min_sdk}"
  target_sdk="${MOCK_ARMV7_TARGET_SDK:-$target_sdk}"
fi
printf "package: name='%s' versionCode='%s' versionName='%s'\n" \
  "$package" "$version_code" "$version_name"
printf "minSdkVersion:'%s'\n" "$min_sdk"
printf "targetSdkVersion:'%s'\n" "$target_sdk"
MOCK_AAPT2

cat > "$mock_build_tools/apksigner" <<'MOCK_APKSIGNER'
#!/usr/bin/env bash
set -euo pipefail
apk="${!#}"
signer='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
if [[ "$apk" == *'armeabi-v7a'* ]]; then
  signer="${MOCK_ARMV7_SIGNER:-$signer}"
fi
printf 'Verified using v1 scheme (JAR signing): true\n'
printf 'Verified using v2 scheme (APK Signature Scheme v2): true\n'
printf 'Signer #1 certificate SHA-256 digest: %s\n' "$signer"
MOCK_APKSIGNER

chmod +x "$fixture_tools/verify-android-apk.sh" \
  "$mock_build_tools/aapt2" "$mock_build_tools/apksigner"
export ANDROID_BUILD_TOOLS_ROOT="$mock_build_tools"

expected_package='com.example.fixtureplayer'
expected_version_name='22.0-FIXTURE-Jumpgate-1.0.0'
expected_version_code='22000001'
expected_core_library='libfixture.so'
expected_signer='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
source_sha='cccccccccccccccccccccccccccccccccccccccc'

make_case() {
  local case_dir="$1"
  local arm64_core="${2:-$expected_core_library}"
  local armv7_core="${3:-$expected_core_library}"
  local armv7_abi="${4:-armeabi-v7a}"
  mkdir -p "$case_dir"
  python3 - \
    "$case_dir/Jumpgate-${expected_version_name}-arm64-v8a.apk" \
    arm64-v8a "$arm64_core" \
    "$case_dir/Jumpgate-${expected_version_name}-armeabi-v7a.apk" \
    "$armv7_abi" "$armv7_core" <<'PY'
import pathlib
import sys
import zipfile

for index in (1, 4):
    apk = pathlib.Path(sys.argv[index])
    abi = sys.argv[index + 1]
    core = sys.argv[index + 2]
    with zipfile.ZipFile(apk, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(f'lib/{abi}/{core}', b'fixture core library')
        archive.writestr(f'lib/{abi}/libhelper.so', b'fixture helper library')
        archive.writestr('assets/fixture.txt', b'fixture')
PY
  for abi in arm64-v8a armeabi-v7a; do
    apk_name="Jumpgate-${expected_version_name}-${abi}.apk"
    (
      cd "$case_dir"
      sha256sum "$apk_name" > "$apk_name.sha256"
    )
    python3 - \
      "$case_dir/Jumpgate-${expected_version_name}-${abi}.spdx.json" \
      "$apk_name" "$abi" "$expected_version_name" <<'PY'
import hashlib
import json
import pathlib
import sys

sbom = pathlib.Path(sys.argv[1])
apk_name = sys.argv[2]
abi = sys.argv[3]
version_name = sys.argv[4]
apk_hash = hashlib.sha256((sbom.parent / apk_name).read_bytes()).hexdigest()
root_id = f'SPDXRef-Package-Jumpgate-APK-{abi}'
component_id = f'SPDXRef-Package-Fixture-Component-{abi}'
document = {
    'SPDXID': 'SPDXRef-DOCUMENT',
    'creationInfo': {'creators': ['Tool: release-validator-test']},
    'dataLicense': 'CC0-1.0',
    'documentDescribes': [root_id],
    'documentNamespace': f'https://example.invalid/{apk_name}',
    'name': apk_name,
    'packages': [
        {
            'SPDXID': component_id,
            'copyrightText': 'NOASSERTION',
            'downloadLocation': 'NOASSERTION',
            'filesAnalyzed': False,
            'licenseConcluded': 'NOASSERTION',
            'licenseDeclared': 'NOASSERTION',
            'name': 'fixture-native-component',
        },
        {
            'SPDXID': root_id,
            'checksums': [{'algorithm': 'SHA256', 'checksumValue': apk_hash}],
            'copyrightText': 'NOASSERTION',
            'downloadLocation': 'NOASSERTION',
            'filesAnalyzed': False,
            'licenseConcluded': 'GPL-2.0-or-later',
            'licenseDeclared': 'GPL-2.0-or-later',
            'name': apk_name,
            'packageFileName': apk_name,
            'primaryPackagePurpose': 'APPLICATION',
            'versionInfo': version_name,
        },
    ],
    'relationships': [
        {
            'spdxElementId': 'SPDXRef-DOCUMENT',
            'relationshipType': 'DESCRIBES',
            'relatedSpdxElement': root_id,
        },
        {
            'spdxElementId': root_id,
            'relationshipType': 'CONTAINS',
            'relatedSpdxElement': component_id,
        },
    ],
    'spdxVersion': 'SPDX-2.3',
}
sbom.write_text(json.dumps(document), encoding='utf-8')
PY
  done
}

run_validator() {
  local case_dir="$1"
  bash "$fixture_tools/verify-android-release.sh" \
    "$case_dir/Jumpgate-${expected_version_name}-arm64-v8a.apk" \
    "$case_dir/Jumpgate-${expected_version_name}-arm64-v8a.spdx.json" \
    "$case_dir/Jumpgate-${expected_version_name}-armeabi-v7a.apk" \
    "$case_dir/Jumpgate-${expected_version_name}-armeabi-v7a.spdx.json" \
    "$expected_package" \
    24 \
    36 \
    "$expected_version_name" \
    "$expected_version_code" \
    "$expected_core_library" \
    "$expected_signer" \
    "$source_sha" \
    refs/tags/reviewed-fixture \
    "${RELEASE_TAG_OVERRIDE:-v1.0.0}" \
    123456 \
    2 \
    "$case_dir"
}

mutate_sbom() {
  local case_dir="$1"
  local abi="$2"
  local mutation="$3"
  python3 - \
    "$case_dir/Jumpgate-${expected_version_name}-${abi}.spdx.json" \
    "$mutation" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
mutation = sys.argv[2]
document = json.loads(path.read_text(encoding='utf-8'))
root_id = document['documentDescribes'][0]
root = next(package for package in document['packages'] if package['SPDXID'] == root_id)

if mutation == 'wrong-hash':
    root['checksums'][0]['checksumValue'] = 'f' * 64
elif mutation == 'wrong-name':
    root['name'] = 'different.apk'
elif mutation == 'wrong-license':
    root['licenseConcluded'] = 'MIT'
elif mutation == 'missing-license':
    root.pop('licenseDeclared')
elif mutation == 'empty-packages':
    document['packages'] = []
    document['relationships'] = []
    document['documentDescribes'] = []
elif mutation == 'root-only':
    document['packages'] = [root]
    document['relationships'] = [
        relation
        for relation in document['relationships']
        if relation['relationshipType'] == 'DESCRIBES'
    ]
elif mutation == 'unrelated-inventory':
    document['relationships'] = [
        relation
        for relation in document['relationships']
        if relation['relationshipType'] != 'CONTAINS'
    ]
elif mutation == 'missing-describes':
    document['relationships'] = [
        relation
        for relation in document['relationships']
        if relation['relationshipType'] != 'DESCRIBES'
    ]
elif mutation == 'wrong-document-root':
    document['documentDescribes'] = [
        package['SPDXID'] for package in document['packages'] if package is not root
    ]
else:
    raise SystemExit(f'unknown mutation: {mutation}')

path.write_text(json.dumps(document), encoding='utf-8')
PY
}

expect_failure() {
  local label="$1"
  shift
  if "$@" >"$work_dir/$label.stdout" 2>"$work_dir/$label.stderr"; then
    echo "Expected release validator failure: $label" >&2
    exit 1
  fi
}

success_dir="$work_dir/success"
make_case "$success_dir"
success_output="$(run_validator "$success_dir")"
[[ "$success_output" == *'Verified Android release parity:'* ]]
python3 - \
  "$success_dir/Jumpgate-${expected_version_name}-metadata.json" \
  "$expected_package" "$expected_core_library" "$expected_signer" <<'PY'
import json
import pathlib
import sys

document = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8'))
assert document['product'] == 'Jumpgate'
assert document['android']['packageName'] == sys.argv[2]
assert document['android']['coreNativeLibrary'] == sys.argv[3]
assert document['android']['signerCertificateSha256'] == sys.argv[4]
assert [item['abi'] for item in document['artifacts']] == ['arm64-v8a', 'armeabi-v7a']
assert document['githubActions'] == {'runAttempt': 2, 'runId': 123456}
assert document['license'] == 'GPL-2.0-or-later'
assert document['releaseTag'] == 'v1.0.0'
PY
(
  cd "$success_dir"
  [[ "$(wc -l < SHA256SUMS)" -eq 5 ]]
  sha256sum --check --strict SHA256SUMS
)
expect_failure non-overwrite run_validator "$success_dir"

for boundary_version_code in 1 2100000000; do
  boundary_dir="$work_dir/version-code-boundary-$boundary_version_code"
  make_case "$boundary_dir"
  expected_version_code="$boundary_version_code"
  export MOCK_EXPECTED_VERSION_CODE="$boundary_version_code"
  export MOCK_VERSION_CODE="$boundary_version_code"
  boundary_output="$(run_validator "$boundary_dir")"
  [[ "$boundary_output" == *'Verified Android release parity:'* ]]
done
expected_version_code='22000001'
unset MOCK_EXPECTED_VERSION_CODE MOCK_VERSION_CODE

for invalid_version_code in 0 2100000001 9999999999999999999999999999999999999999; do
  invalid_label="version-code-range-${invalid_version_code:0:16}"
  invalid_dir="$work_dir/$invalid_label"
  make_case "$invalid_dir"
  expected_version_code="$invalid_version_code"
  expect_failure "$invalid_label" run_validator "$invalid_dir"
done
expected_version_code='22000001'

version_mismatch_dir="$work_dir/version-mismatch"
make_case "$version_mismatch_dir"
export MOCK_ARMV7_VERSION_CODE=99999999
expect_failure version-parity run_validator "$version_mismatch_dir"
unset MOCK_ARMV7_VERSION_CODE

signer_mismatch_dir="$work_dir/signer-mismatch"
make_case "$signer_mismatch_dir"
export MOCK_ARMV7_SIGNER='bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
expect_failure signer-parity run_validator "$signer_mismatch_dir"
unset MOCK_ARMV7_SIGNER

hash_mismatch_dir="$work_dir/hash-mismatch"
make_case "$hash_mismatch_dir"
printf 'tampered' >> "$hash_mismatch_dir/Jumpgate-${expected_version_name}-arm64-v8a.apk"
expect_failure recorded-hash run_validator "$hash_mismatch_dir"

wrong_abi_dir="$work_dir/wrong-abi"
make_case "$wrong_abi_dir" "$expected_core_library" "$expected_core_library" arm64-v8a
expect_failure abi-scope run_validator "$wrong_abi_dir"

wrong_core_dir="$work_dir/wrong-core"
make_case "$wrong_core_dir" libother.so libother.so
expect_failure core-library run_validator "$wrong_core_dir"

wrong_sbom_hash_dir="$work_dir/wrong-sbom-hash"
make_case "$wrong_sbom_hash_dir"
mutate_sbom "$wrong_sbom_hash_dir" arm64-v8a wrong-hash
expect_failure sbom-apk-hash run_validator "$wrong_sbom_hash_dir"

wrong_sbom_name_dir="$work_dir/wrong-sbom-name"
make_case "$wrong_sbom_name_dir"
mutate_sbom "$wrong_sbom_name_dir" arm64-v8a wrong-name
expect_failure sbom-apk-name run_validator "$wrong_sbom_name_dir"

wrong_license_dir="$work_dir/wrong-license"
make_case "$wrong_license_dir"
mutate_sbom "$wrong_license_dir" arm64-v8a wrong-license
expect_failure sbom-wrong-license run_validator "$wrong_license_dir"

missing_license_dir="$work_dir/missing-license"
make_case "$missing_license_dir"
mutate_sbom "$missing_license_dir" arm64-v8a missing-license
expect_failure sbom-missing-license run_validator "$missing_license_dir"

empty_sbom_dir="$work_dir/empty-sbom"
make_case "$empty_sbom_dir"
mutate_sbom "$empty_sbom_dir" arm64-v8a empty-packages
expect_failure sbom-empty run_validator "$empty_sbom_dir"

root_only_sbom_dir="$work_dir/root-only-sbom"
make_case "$root_only_sbom_dir"
mutate_sbom "$root_only_sbom_dir" arm64-v8a root-only
expect_failure sbom-no-inventory run_validator "$root_only_sbom_dir"

unrelated_sbom_dir="$work_dir/unrelated-sbom"
make_case "$unrelated_sbom_dir"
mutate_sbom "$unrelated_sbom_dir" arm64-v8a unrelated-inventory
expect_failure sbom-unrelated-inventory run_validator "$unrelated_sbom_dir"

missing_describes_dir="$work_dir/missing-describes"
make_case "$missing_describes_dir"
mutate_sbom "$missing_describes_dir" arm64-v8a missing-describes
expect_failure sbom-missing-describes run_validator "$missing_describes_dir"

wrong_document_root_dir="$work_dir/wrong-document-root"
make_case "$wrong_document_root_dir"
mutate_sbom "$wrong_document_root_dir" arm64-v8a wrong-document-root
expect_failure sbom-wrong-document-root run_validator "$wrong_document_root_dir"

wrong_release_tag_dir="$work_dir/wrong-release-tag"
make_case "$wrong_release_tag_dir"
export RELEASE_TAG_OVERRIDE='v1.0.1'
expect_failure release-tag-binding run_validator "$wrong_release_tag_dir"
unset RELEASE_TAG_OVERRIDE

printf 'Android release aggregate validator tests: passed\n'

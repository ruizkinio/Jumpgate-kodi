#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
verifier="$script_dir/verify-shairplay-crypto-symbols.sh"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/test-shairplay-crypto-symbols.XXXXXX")"
trap 'rm -rf "$work_dir"' EXIT

cat > "$work_dir/expected-legacy.txt" <<'EOF'
AES_cbc_decrypt
AES_cbc_encrypt
AES_convert_key
AES_set_key
MD5_Final
MD5_Init
MD5_Update
RC4_crypt
RC4_setup
SHA1_Final
SHA1_Init
SHA1_Update
hmac_md5
hmac_sha1
EOF
sed 's/^/shairplay_crypto_/' \
  "$work_dir/expected-legacy.txt" > "$work_dir/expected-namespaced.txt"

assert_exact_unique_set() {
  local label="$1"
  local expected="$2"
  local actual="$3"
  local actual_count
  local actual_unique_count

  actual_count="$(awk 'NF { count++ } END { print count + 0 }' "$actual")"
  sort -u "$actual" > "$actual.sorted"
  actual_unique_count="$(awk 'NF { count++ } END { print count + 0 }' "$actual.sorted")"
  if [[ "$actual_count" -ne 14 || "$actual_unique_count" -ne 14 ]]; then
    echo "$label contract must contain exactly 14 unique entries" >&2
    exit 1
  fi
  if ! diff -u "$expected" "$actual.sorted"; then
    echo "$label contract does not match the exact expected symbol set" >&2
    exit 1
  fi
}

awk '
  index($0, "legacy.txt") && index($0, "<<\047EOF\047") { capture = 1; next }
  capture && $0 == "EOF" { exit }
  capture { print }
' "$verifier" > "$work_dir/verifier-legacy.txt"
assert_exact_unique_set \
  'Verifier legacy' \
  "$work_dir/expected-legacy.txt" \
  "$work_dir/verifier-legacy.txt"

grep -Fqx \
  "sed 's/^/shairplay_crypto_/' \"\$work_dir/legacy.txt\" > \"\$work_dir/expected.txt\"" \
  "$verifier" || {
    echo 'Verifier namespaced contract is not derived with the required prefix' >&2
    exit 1
  }
sed 's/^/shairplay_crypto_/' \
  "$work_dir/verifier-legacy.txt" > "$work_dir/verifier-namespaced.txt"
assert_exact_unique_set \
  'Verifier namespaced' \
  "$work_dir/expected-namespaced.txt" \
  "$work_dir/verifier-namespaced.txt"

shairplay_archive="$work_dir/libshairplay.a"
crypto_archive="$work_dir/libcrypto.a"
printf 'fixture\n' > "$shairplay_archive"
printf 'fixture\n' > "$crypto_archive"

cat > "$work_dir/fake-nm" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

archive="${!#}"
query=''
for argument in "$@"; do
  case "$argument" in
    --defined-only)
      query='defined'
      ;;
    --undefined-only)
      query='undefined'
      ;;
  esac
done

case "$(basename "$archive")" in
  libshairplay.a)
    if [[ "$query" == defined ]]; then
      while IFS= read -r symbol; do
        if [[ "${FAKE_NM_MODE:-pass}" == missing &&
              "$symbol" == shairplay_crypto_AES_set_key ]]; then
          continue
        fi
        printf '%s[crypto.o]: %s T 0 1\n' "$archive" "$symbol"
      done <<'SYMBOLS'
shairplay_crypto_AES_cbc_decrypt
shairplay_crypto_AES_cbc_encrypt
shairplay_crypto_AES_convert_key
shairplay_crypto_AES_set_key
shairplay_crypto_MD5_Final
shairplay_crypto_MD5_Init
shairplay_crypto_MD5_Update
shairplay_crypto_RC4_crypt
shairplay_crypto_RC4_setup
shairplay_crypto_SHA1_Final
shairplay_crypto_SHA1_Init
shairplay_crypto_SHA1_Update
shairplay_crypto_hmac_md5
shairplay_crypto_hmac_sha1
SYMBOLS
      printf '%s[weak.o]: shared_weak W 0 1\n' "$archive"
      if [[ "${FAKE_NM_MODE:-pass}" == legacy ]]; then
        printf '%s[crypto.o]: MD5_Init T 0 1\n' "$archive"
      elif [[ "${FAKE_NM_MODE:-pass}" == legacy-weak ]]; then
        printf '%s[crypto.o]: MD5_Init W 0 1\n' "$archive"
      elif [[ "${FAKE_NM_MODE:-pass}" == collision ]]; then
        printf '%s[crypto.o]: shared_strong T 0 1\n' "$archive"
      fi
    elif [[ "$query" == undefined ]]; then
      if [[ "${FAKE_NM_MODE:-pass}" == undefined-legacy ]]; then
        : "${FAKE_NM_UNDEFINED_SYMBOL:?undefined symbol is required}"
        case "${FAKE_NM_UNDEFINED_FORMAT:?undefined format is required}" in
          llvm)
            printf '/tmp/llvm owner/libshairplay.a[caller.o]: %s U 0 0\n' \
              "$FAKE_NM_UNDEFINED_SYMBOL"
            ;;
          gnu)
            printf '/tmp/gnu owner/libshairplay.a:caller.o: %s U\n' \
              "$FAKE_NM_UNDEFINED_SYMBOL"
            ;;
          *)
            echo "Unexpected undefined nm format: $FAKE_NM_UNDEFINED_FORMAT" >&2
            exit 2
            ;;
        esac
      else
        printf '%s[caller.o]: shairplay_crypto_MD5_Init U 0 0\n' "$archive"
      fi
    else
      echo "Unexpected nm query for $archive" >&2
      exit 2
    fi
    ;;
  libcrypto.a)
    if [[ "$query" == defined ]]; then
      printf '%s[md5.o]: MD5_Init T 0 1\n' "$archive"
      printf '%s[weak.o]: shared_weak W 0 1\n' "$archive"
      if [[ "${FAKE_NM_MODE:-pass}" == collision ]]; then
        printf '%s[crypto.o]: shared_strong T 0 1\n' "$archive"
      fi
    else
      echo "Unexpected nm query for $archive" >&2
      exit 2
    fi
    ;;
  *)
    echo "Unexpected archive: $archive" >&2
    exit 2
    ;;
esac
EOF
chmod +x "$work_dir/fake-nm"

run_success() {
  local mode="$1"
  local output="$work_dir/$mode.out"
  if ! FAKE_NM_MODE="$mode" bash "$verifier" \
      "$work_dir/fake-nm" "$shairplay_archive" "$crypto_archive" > "$output" 2>&1; then
    cat "$output" >&2
    echo "Expected success for mode: $mode" >&2
    exit 1
  fi
}

run_failure() {
  local mode="$1"
  local expected="$2"
  local output="$work_dir/$mode.out"
  if FAKE_NM_MODE="$mode" bash "$verifier" \
      "$work_dir/fake-nm" "$shairplay_archive" "$crypto_archive" > "$output" 2>&1; then
    cat "$output" >&2
    echo "Expected failure for mode: $mode" >&2
    exit 1
  fi
  grep -Fq "$expected" "$output" || {
    cat "$output" >&2
    echo "Missing diagnostic for mode $mode: $expected" >&2
    exit 1
  }
}

run_undefined_legacy_failure() {
  local format="$1"
  local symbol="$2"
  local output="$work_dir/undefined-legacy-$format-$symbol.out"
  local rejected="$work_dir/undefined-legacy-$format-$symbol.rejected"

  if FAKE_NM_MODE=undefined-legacy \
      FAKE_NM_UNDEFINED_FORMAT="$format" \
      FAKE_NM_UNDEFINED_SYMBOL="$symbol" \
      bash "$verifier" \
        "$work_dir/fake-nm" "$shairplay_archive" "$crypto_archive" > "$output" 2>&1; then
    cat "$output" >&2
    echo "Expected undefined legacy failure for $format: $symbol" >&2
    exit 1
  fi
  grep -Fxf "$work_dir/expected-legacy.txt" "$output" > "$rejected" || true
  if [[ "$(cat "$rejected")" != "$symbol" ]]; then
    cat "$output" >&2
    echo "Failure did not identify exactly $symbol for $format" >&2
    exit 1
  fi
  grep -Fq 'legacy generic crypto symbols are referenced undefined' "$output" || {
    cat "$output" >&2
    echo "Missing undefined legacy diagnostic for $format: $symbol" >&2
    exit 1
  }
}

run_success pass
run_success undefined-namespaced
run_failure legacy 'legacy generic crypto symbols remain'
run_failure legacy-weak 'legacy generic crypto symbols remain'
while IFS= read -r symbol; do
  for format in llvm gnu; do
    run_undefined_legacy_failure "$format" "$symbol"
  done
done < "$work_dir/expected-legacy.txt"
run_failure missing 'expected namespaced crypto symbols are missing'
run_failure collision 'strong global definitions intersect'

echo 'Shairplay crypto symbol verifier tests passed.'

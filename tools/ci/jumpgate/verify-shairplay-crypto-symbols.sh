#!/usr/bin/env bash

set -euo pipefail
export LC_ALL=C

usage() {
  echo "Usage: $0 <nm> <libshairplay.a> <libcrypto.a>" >&2
  exit 2
}

fail() {
  echo "Shairplay crypto symbol verification failed: $*" >&2
  exit 1
}

[[ $# -eq 3 ]] || usage

nm_bin="$1"
shairplay_archive="$2"
crypto_archive="$3"

[[ -x "$nm_bin" ]] || fail "nm is not executable: $nm_bin"
[[ -s "$shairplay_archive" ]] || fail "archive is missing or empty: $shairplay_archive"
[[ -s "$crypto_archive" ]] || fail "archive is missing or empty: $crypto_archive"

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/shairplay-crypto-symbols.XXXXXX")"
trap 'rm -rf "$work_dir"' EXIT

cat > "$work_dir/legacy.txt" <<'EOF'
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
LC_ALL=C sort -o "$work_dir/legacy.txt" "$work_dir/legacy.txt"
sed 's/^/shairplay_crypto_/' "$work_dir/legacy.txt" > "$work_dir/expected.txt"

assert_symbol_contract() {
  local label="$1"
  local symbols="$2"
  local count
  local unique_count

  count="$(awk 'NF { count++ } END { print count + 0 }' "$symbols")"
  unique_count="$(sort -u "$symbols" | awk 'NF { count++ } END { print count + 0 }')"
  if [[ "$count" -ne 14 || "$unique_count" -ne 14 ]]; then
    fail "internal $label symbol contract must contain exactly 14 unique entries"
  fi
}

assert_symbol_contract legacy "$work_dir/legacy.txt"
assert_symbol_contract namespaced "$work_dir/expected.txt"

write_global_definitions() {
  local archive="$1"
  local output_prefix="$2"

  "$nm_bin" \
    --format=posix \
    --print-file-name \
    --defined-only \
    --extern-only \
    "$archive" > "$output_prefix.nm"

  awk '
    {
      line = $0
      sub(/^.*:[[:space:]]+/, "", line)
      split(line, field, /[[:space:]]+/)
      if (length(field[2]) == 1) {
        print field[1]
      }
    }
  ' "$output_prefix.nm" |
    sort -u > "$output_prefix.all"

  awk '
    {
      line = $0
      sub(/^.*:[[:space:]]+/, "", line)
      split(line, field, /[[:space:]]+/)
      if (length(field[2]) == 1 && field[2] !~ /^[UVWvw]$/) {
        print field[1]
      }
    }
  ' "$output_prefix.nm" |
    sort -u > "$output_prefix.strong"

  [[ -s "$output_prefix.all" ]] || fail "no global definitions found in $archive"
  [[ -s "$output_prefix.strong" ]] || fail "no strong global definitions found in $archive"
}

write_undefined_references() {
  local archive="$1"
  local output="$2"

  "$nm_bin" \
    --format=posix \
    --print-file-name \
    --undefined-only \
    --extern-only \
    "$archive" |
    awk '
      {
        line = $0
        sub(/^.*:[[:space:]]+/, "", line)
        split(line, field, /[[:space:]]+/)
        if (length(field[2]) == 1) {
          print field[1]
        }
      }
    ' |
    sort -u > "$output"
}

write_global_definitions "$shairplay_archive" "$work_dir/shairplay"
write_global_definitions "$crypto_archive" "$work_dir/crypto"
write_undefined_references "$shairplay_archive" "$work_dir/shairplay.undefined"

comm -12 "$work_dir/legacy.txt" "$work_dir/shairplay.all" > "$work_dir/legacy-found.txt"
if [[ -s "$work_dir/legacy-found.txt" ]]; then
  cat "$work_dir/legacy-found.txt" >&2
  fail 'legacy generic crypto symbols remain in libshairplay.a'
fi

comm -12 "$work_dir/legacy.txt" "$work_dir/shairplay.undefined" > "$work_dir/legacy-undefined.txt"
if [[ -s "$work_dir/legacy-undefined.txt" ]]; then
  cat "$work_dir/legacy-undefined.txt" >&2
  fail 'legacy generic crypto symbols are referenced undefined by libshairplay.a'
fi

comm -23 "$work_dir/expected.txt" "$work_dir/shairplay.strong" > "$work_dir/expected-missing.txt"
if [[ -s "$work_dir/expected-missing.txt" ]]; then
  cat "$work_dir/expected-missing.txt" >&2
  fail 'expected namespaced crypto symbols are missing from libshairplay.a'
fi

comm -12 "$work_dir/shairplay.strong" "$work_dir/crypto.strong" > "$work_dir/collisions.txt"
if [[ -s "$work_dir/collisions.txt" ]]; then
  cat "$work_dir/collisions.txt" >&2
  fail 'strong global definitions intersect between libshairplay.a and libcrypto.a'
fi

echo 'Shairplay and OpenSSL static archive symbols are isolated.'

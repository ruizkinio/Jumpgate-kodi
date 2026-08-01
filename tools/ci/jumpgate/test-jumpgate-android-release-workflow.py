#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import ast
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Iterator

import yaml


class UniqueKeyLoader(yaml.SafeLoader):
    pass


def construct_unique_mapping(loader: UniqueKeyLoader, node: yaml.MappingNode, deep: bool = False):
    mapping = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in mapping:
            raise AssertionError(f"duplicate YAML key: {key!r}")
        mapping[key] = loader.construct_object(value_node, deep=deep)
    return mapping


UniqueKeyLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG,
    construct_unique_mapping,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def find_bash() -> str:
    candidates: list[pathlib.Path] = []
    if os.name == "nt":
        for base in filter(None, (os.environ.get("ProgramFiles"), os.environ.get("ProgramW6432"))):
            candidates.extend(
                (
                    pathlib.Path(base) / "Git" / "bin" / "bash.exe",
                    pathlib.Path(base) / "Git" / "usr" / "bin" / "bash.exe",
                )
            )
    discovered = shutil.which("bash")
    if discovered:
        candidates.append(pathlib.Path(discovered))
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise AssertionError("Git Bash or a compatible bash executable is required for policy tests")


def run_bash(script: str, *, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    return subprocess.run(
        [find_bash(), "-c", script],
        cwd=root,
        env=merged_env,
        check=False,
        capture_output=True,
        text=True,
    )


def extract_bash_function(script: str, function_name: str) -> str:
    lines = script.splitlines()
    signature = f"{function_name}() {{"
    for start, line in enumerate(lines):
        if line.strip() != signature:
            continue
        indent = line[: len(line) - len(line.lstrip())]
        for end in range(start + 1, len(lines)):
            if lines[end] == f"{indent}}}":
                return "\n".join(item[len(indent) :] for item in lines[start : end + 1])
    raise AssertionError(f"unable to extract Bash function {function_name}")


def strings(value: object, path: tuple[str, ...] = ()) -> Iterator[tuple[tuple[str, ...], str]]:
    if isinstance(value, str):
        yield path, value
    elif isinstance(value, dict):
        for key, item in value.items():
            yield from strings(item, path + (str(key),))
    elif isinstance(value, list):
        for index, item in enumerate(value):
            yield from strings(item, path + (str(index),))


root = pathlib.Path(__file__).resolve().parents[3]
workflow_path = root / ".github" / "workflows" / "jumpgate-android-release.yml"
validator_path = root / "tools" / "ci" / "jumpgate" / "verify-android-release.sh"
apk_verifier_path = root / "tools" / "ci" / "jumpgate" / "verify-android-apk.sh"
apk_verifier_test_path = root / "tools" / "ci" / "jumpgate" / "test-verify-android-apk.sh"
validator_test_path = root / "tools" / "ci" / "jumpgate" / "test-verify-android-release.sh"
workflow_text = workflow_path.read_text(encoding="utf-8")
validator_text = validator_path.read_text(encoding="utf-8")
apk_verifier_text = apk_verifier_path.read_text(encoding="utf-8")
apk_verifier_test_text = apk_verifier_test_path.read_text(encoding="utf-8")
validator_test_text = validator_test_path.read_text(encoding="utf-8")
workflow = yaml.load(workflow_text, Loader=UniqueKeyLoader)

require(isinstance(workflow, dict), "workflow must be a YAML mapping")
require(workflow.get("name") == "Jumpgate Android Release", "release workflow name drifted")
triggers = workflow.get("on")
require(isinstance(triggers, dict), "workflow trigger mapping is missing")
require(set(triggers) == {"workflow_dispatch"}, "release workflow must be manual-only")
dispatch = triggers["workflow_dispatch"]
require(isinstance(dispatch, dict), "workflow_dispatch inputs are missing")
inputs = dispatch.get("inputs")
require(set(inputs or {}) == {"reviewed_ref", "release_tag"}, "release inputs must be exact")
for input_name in ("reviewed_ref", "release_tag"):
    require(inputs[input_name].get("required") is True, f"{input_name} must be required")
    require(inputs[input_name].get("type") == "string", f"{input_name} must be a string")
require("pull_request" not in workflow_text, "release workflow must not mention pull requests")
require("\npush:" not in workflow_text, "release workflow must not have a push trigger")
require("schedule:" not in workflow_text, "release workflow must not have a schedule trigger")
require(workflow.get("permissions") == {"contents": "read"}, "top-level permissions must be read-only")
require(
    workflow.get("concurrency") == {"group": "jumpgate-android-release", "cancel-in-progress": False},
    "all Android releases must share one non-cancelling concurrency lane",
)
require(len(re.findall(r"(?m)^\s*concurrency:\s*$", workflow_text)) == 1,
        "release workflow must define exactly one global concurrency lane")
dynamic_default_branch = "${{ github.event.repository.default_branch }}"
require(workflow.get("env", {}).get("CANONICAL_DEFAULT_BRANCH") == dynamic_default_branch,
        "canonical default branch must derive from repository event metadata")
for literal_branch in ("master", "main"):
    require(
        re.search(
            rf"(?i)(?<![A-Za-z0-9_.-]){literal_branch}(?![A-Za-z0-9_.-])",
            workflow_text,
        )
        is None,
        f"release workflow contains a literal {literal_branch} branch pin",
    )
require(workflow.get("env", {}).get("CACHE_SCHEMA_VERSION") == "release-v4",
        "release cache namespace must remain isolated from mutable CI caches")

jobs = workflow.get("jobs")
expected_jobs = {
    "validate_release_inputs",
    "build_candidates",
    "sign_release_apks",
    "assemble_release",
    "publish_draft_release",
}
require(set(jobs or {}) == expected_jobs, "release job graph drifted")
require(all("concurrency" not in job for job in jobs.values()),
        "release concurrency must remain global rather than job-specific")
manual_guard = "github.event_name == 'workflow_dispatch'"
repository_guard = "github.repository == 'ruizkinio/Jumpgate-kodi'"
default_branch_ref_guard = (
    "github.ref == format('refs/heads/{0}', github.event.repository.default_branch)"
)
for job_name, job in jobs.items():
    condition = str(job.get("if", ""))
    require(manual_guard in condition, f"{job_name} lacks a workflow_dispatch guard")
    require(repository_guard in condition, f"{job_name} lacks the canonical-repository guard")
    require(condition.count(default_branch_ref_guard) == 1,
            f"{job_name} can run workflow code from a non-default branch")
    require("permissions" in job, f"{job_name} must declare least-privilege permissions")

require(
    jobs["validate_release_inputs"]["permissions"] == {"contents": "read"},
    "input validation permissions are too broad",
)
require(
    jobs["build_candidates"]["permissions"] == {"contents": "read"},
    "candidate build permissions are too broad",
)
require(
    jobs["sign_release_apks"]["permissions"] == {"actions": "read", "contents": "read"},
    "signing permissions are too broad",
)
require(
    jobs["assemble_release"]["permissions"]
    == {"actions": "read", "attestations": "write", "contents": "read", "id-token": "write"},
    "attestation permissions are incomplete or too broad",
)
require(
    jobs["publish_draft_release"]["permissions"] == {"actions": "read", "contents": "read"},
    "publishing permissions are incomplete or too broad",
)

environment_jobs = {
    name
    for name, job in jobs.items()
    if isinstance(job.get("environment"), dict)
    and job["environment"].get("name") == "android-release"
}
require(
    environment_jobs == {"sign_release_apks", "publish_draft_release"},
    "protected environment must gate signing and publishing only",
)


def checkout_ref(job_name: str) -> str:
    checkout_steps = [
        step
        for step in jobs[job_name].get("steps", [])
        if isinstance(step, dict) and str(step.get("uses", "")).startswith("actions/checkout@")
    ]
    require(len(checkout_steps) == 1, f"{job_name} must have exactly one checkout")
    settings = checkout_steps[0].get("with")
    require(isinstance(settings, dict), f"{job_name} checkout settings are missing")
    require(settings.get("fetch-depth") == 1, f"{job_name} checkout must remain shallow and exact")
    require(settings.get("persist-credentials") is False,
            f"{job_name} checkout must not persist repository credentials")
    return str(settings.get("ref", ""))


require(checkout_ref("validate_release_inputs") == "${{ github.workflow_sha }}",
        "input validation must inspect the trusted workflow invocation commit")
require(
    checkout_ref("build_candidates") == "${{ needs.validate_release_inputs.outputs.source_sha }}",
    "candidate build must use the resolved reviewed commit",
)
for trusted_job in ("sign_release_apks", "assemble_release"):
    require(
        checkout_ref(trusted_job) == "${{ github.workflow_sha }}",
        f"{trusted_job} must execute release validators from the trusted workflow revision",
    )


def named_step(job_name: str, step_name: str) -> dict:
    matches = [
        step
        for step in jobs[job_name].get("steps", [])
        if isinstance(step, dict) and step.get("name") == step_name
    ]
    require(len(matches) == 1, f"{job_name} must contain exactly one {step_name!r} step")
    return matches[0]


release_inputs_step = named_step("validate_release_inputs", "Resolve immutable release inputs")
release_inputs_text = str(release_inputs_step.get("run", ""))
require(release_inputs_step.get("env", {}).get("WORKFLOW_SHA") == "${{ github.workflow_sha }}",
        "input validation must receive github.workflow_sha directly")
for branch_invariant in (
    'git check-ref-format "refs/heads/$CANONICAL_DEFAULT_BRANCH"',
    '[[ "$GITHUB_REF" == "refs/heads/$CANONICAL_DEFAULT_BRANCH" ]]',
    'git ls-remote --exit-code --heads origin',
    'awk -v ref="refs/heads/$CANONICAL_DEFAULT_BRANCH"',
    'echo "default_branch=$CANONICAL_DEFAULT_BRANCH"',
):
    require(branch_invariant in release_inputs_text,
            f"default-branch validation is missing: {branch_invariant}")
for source_invariant in (
    '[[ "$source_sha" == "$workflow_sha" ]]',
    "Reviewed source must exactly match github.workflow_sha",
    '[[ "$remote_default_sha" == "$source_sha" ]]',
    "Workflow commit is no longer the canonical default-branch head",
):
    require(source_invariant in release_inputs_text,
            f"trusted-source invariant is missing: {source_invariant}")
require("inputs.reviewed_ref" not in checkout_ref("validate_release_inputs"),
        "untrusted reviewed_ref must never select the checked-out build source")
require(
    jobs["validate_release_inputs"]["outputs"].get("default_branch")
    == "${{ steps.release_inputs.outputs.default_branch }}",
    "validated default branch is not exported",
)
require(
    jobs["validate_release_inputs"]["outputs"].get("reviewed_ref_kind")
    == "${{ steps.release_inputs.outputs.reviewed_ref_kind }}",
    "reviewed ref kind is not exported",
)
require(
    jobs["validate_release_inputs"]["outputs"].get("reviewed_ref_object_sha")
    == "${{ steps.release_inputs.outputs.reviewed_ref_object_sha }}",
    "reviewed ref object identity is not exported",
)
require('canonical_release_tag="v$jumpgate_semver"' in release_inputs_text,
        "release tag is not derived from the Jumpgate semantic version")
require('[[ "$RELEASE_TAG" == "$canonical_release_tag" ]]' in release_inputs_text,
        "release tag is not required to equal canonical v<Jumpgate-semver>")

version_code_scripts = {
    "input validation": release_inputs_text,
    "APK staging": str(named_step("build_candidates", "Verify and stage APK").get("run", "")),
}
for label, script in version_code_scripts.items():
    function = extract_bash_function(script, "derive_android_version_code")
    length_guard = "${#code_major} -le 5"
    arithmetic = "major_value=$((10#$code_major))"
    require(length_guard in function and arithmetic in function,
            f"{label} does not validate VERSION_CODE length before arithmetic")
    require(function.index(length_guard) < function.index(arithmetic),
            f"{label} performs VERSION_CODE arithmetic before its length guard")
    require("value >= 1 && value <= 2100000000" in function,
            f"{label} omits the Android versionCode range")
    version_result = run_bash(
        function
        + """
set -euo pipefail
[[ "$(derive_android_version_code 0.0.1)" == 1 ]]
[[ "$(derive_android_version_code 21000.0.0)" == 2100000000 ]]
if derive_android_version_code 0.0.0 >/dev/null; then exit 10; fi
if derive_android_version_code 21000.0.1 >/dev/null; then exit 11; fi
if derive_android_version_code 21001.0.0 >/dev/null; then exit 12; fi
huge_major="$(printf '9%.0s' {1..1000})"
if derive_android_version_code "$huge_major.0.0" >/dev/null; then exit 13; fi
"""
    )
    require(
        version_result.returncode == 0,
        f"{label} VERSION_CODE boundary tests failed: {version_result.stderr}",
    )

history_step = named_step(
    "validate_release_inputs",
    "Enforce canonical semantic version and Android versionCode monotonicity",
)
history_text = str(history_step.get("run", ""))
for history_guard in (
    '"ls-remote", "--tags", "--refs", "origin", "refs/tags/v*"',
    "/releases?per_page=100&page={page}",
    "if release_tags != set(tag_refs):",
    "if not release_tags and not tag_refs:",
    "Strict first-release path: no canonical releases or v<semver> tags",
    "current_semver = tuple(int(part) for part in current_tag_match.groups())",
    "prior_semver = tuple(int(part) for part in prior_tag_match.groups())",
    "if current_semver <= prior_semver:",
    "Current Jumpgate semantic version must be greater than every prior",
    'prior_version_file = run_git("show", f"{prior_commit}:version.txt")',
    "if current_version_code <= prior_version_code:",
    "Current Android versionCode must be greater than every prior canonical",
    "history_fingerprint=",
):
    require(history_guard in history_text, f"release-history policy is missing: {history_guard}")


def python_heredocs(script: str) -> list[str]:
    return re.findall(r"<<'PY'[^\n]*\n(.*?)\nPY(?:\n|$)", script, flags=re.DOTALL)


history_python = python_heredocs(history_text)
require(len(history_python) == 1, "release-history step must have one auditable Python policy block")
history_tree = ast.parse(history_python[0], filename="jumpgate-release-history-policy.py")
version_code_monotonic_if_nodes = [
    node
    for node in ast.walk(history_tree)
    if isinstance(node, ast.If)
    and ast.unparse(node.test) == "current_version_code <= prior_version_code"
]
require(len(version_code_monotonic_if_nodes) == 1,
        "versionCode monotonicity must remain one explicit fail-closed comparison")
require(any(isinstance(node, ast.Raise) for node in version_code_monotonic_if_nodes[0].body),
        "versionCode monotonicity comparison must terminate the release on failure")
semantic_monotonic_if_nodes = [
    node
    for node in ast.walk(history_tree)
    if isinstance(node, ast.If)
    and ast.unparse(node.test) == "current_semver <= prior_semver"
]
require(len(semantic_monotonic_if_nodes) == 1,
        "semantic-version monotonicity must remain one explicit fail-closed comparison")
require(any(isinstance(node, ast.Raise) for node in semantic_monotonic_if_nodes[0].body),
        "semantic-version monotonicity comparison must terminate the release on failure")
require(
    jobs["validate_release_inputs"]["outputs"].get("history_fingerprint")
    == "${{ steps.release_history.outputs.history_fingerprint }}",
    "validated release-history fingerprint is not exported",
)

require("Reviewed source is not branded as Jumpgate" in workflow_text,
        "release input validation does not enforce the Jumpgate application identity")
require("io.github.ruizkinio.jumpgate" in workflow_text,
        "release input validation does not enforce the Jumpgate package identity")
require("identity_hash=\"$(sha256sum version.txt" in workflow_text,
        "release caches are not scoped to the application identity")
require(workflow_text.count("steps.source-keys.outputs.identity_hash") == 4,
        "dependency and Gradle restore/save caches must include the identity hash")

cache_steps = [
    step
    for step in jobs["build_candidates"].get("steps", [])
    if isinstance(step, dict) and str(step.get("uses", "")).startswith("actions/cache/")
]
require(len(cache_steps) == 8, "release workflow must have four exact restores and four exact saves")
for step in cache_steps:
    settings = step.get("with")
    require(isinstance(settings, dict), f"cache settings are absent for {step.get('name')}")
    cache_key = str(settings.get("key", ""))
    require(cache_key.startswith("jumpgate-android-release-${{ env.CACHE_SCHEMA_VERSION }}-"),
            f"cache is outside the isolated release namespace: {step.get('name')}")
    require("${{ needs.validate_release_inputs.outputs.source_sha }}" in cache_key,
            f"cache can restore state from a foreign source commit: {step.get('name')}")
    require("restore-keys" not in settings,
            f"cache uses a broad prefix fallback: {step.get('name')}")
require("restore-keys:" not in workflow_text,
        "release workflow must never use mutable cache prefix fallbacks")

build_step_names = [step.get("name") for step in jobs["build_candidates"].get("steps", [])]
verification_index = build_step_names.index("Verify and stage APK")
for step in cache_steps:
    if str(step.get("uses", "")).startswith("actions/cache/save@"):
        require(build_step_names.index(step.get("name")) > verification_index,
                f"cache is saved before APK verification: {step.get('name')}")

signing_secrets = {
    "JUMPGATE_ANDROID_RELEASE_KEYSTORE_P12_BASE64",
    "JUMPGATE_ANDROID_RELEASE_KEYSTORE_PASSWORD",
    "JUMPGATE_ANDROID_RELEASE_KEY_ALIAS",
    "JUMPGATE_ANDROID_RELEASE_KEY_PASSWORD",
    "JUMPGATE_ANDROID_RELEASE_SIGNER_SHA256",
}
release_app_secrets = {
    "JUMPGATE_ANDROID_RELEASE_APP_CLIENT_ID",
    "JUMPGATE_ANDROID_RELEASE_APP_ID",
    "JUMPGATE_ANDROID_RELEASE_APP_PRIVATE_KEY",
}
expected_secrets = signing_secrets | release_app_secrets
secret_hits: list[tuple[tuple[str, ...], str]] = []
secret_names: set[str] = set()
for path, value in strings(workflow):
    names = set(re.findall(r"secrets\.([A-Z0-9_]+)", value))
    if names:
        secret_hits.append((path, value))
        secret_names.update(names)
require(secret_names == expected_secrets, "release secret contract drifted")
require(secret_hits, "release secrets are not referenced")
for path, value in secret_hits:
    names = set(re.findall(r"secrets\.([A-Z0-9_]+)", value))
    if names & signing_secrets:
        require(path[:2] == ("jobs", "sign_release_apks"),
                "Android signing secret escaped the signing job")
    if names & release_app_secrets:
        require(path[:2] == ("jobs", "publish_draft_release"),
                "release App secret escaped the protected publication job")
secret_step_locations = {
    (job_name, step.get("name"))
    for job_name, job in jobs.items()
    for step in job.get("steps", [])
    if re.search(r"secrets\.[A-Z0-9_]+", "\n".join(value for _, value in strings(step)))
}
require(
    secret_step_locations
    == {
        ("sign_release_apks", "Import one protected PKCS12 and sign both ABIs"),
        ("publish_draft_release", "Mint repository-scoped dedicated release App token"),
        ("publish_draft_release", "Create new tag and draft release without overwrite APIs"),
    },
    "release secret escaped its isolated protected step",
)

signing_steps = jobs["sign_release_apks"].get("steps", [])
signing_step_names = [step.get("name") for step in signing_steps]
binding_step_name = "Revalidate reviewed source binding before secret access"
secret_step_name = "Import one protected PKCS12 and sign both ABIs"
require(
    signing_step_names.index(binding_step_name) + 1 == signing_step_names.index(secret_step_name),
    "reviewed source must be revalidated immediately before the secret-bearing signing step",
)
binding_step = named_step("sign_release_apks", binding_step_name)
binding_env = binding_step.get("env", {})
for name, expected in (
    ("REVIEWED_REF", "${{ needs.validate_release_inputs.outputs.reviewed_ref }}"),
    ("REVIEWED_REF_KIND", "${{ needs.validate_release_inputs.outputs.reviewed_ref_kind }}"),
    (
        "REVIEWED_REF_OBJECT_SHA",
        "${{ needs.validate_release_inputs.outputs.reviewed_ref_object_sha }}",
    ),
    ("SOURCE_SHA", "${{ needs.validate_release_inputs.outputs.source_sha }}"),
):
    require(binding_env.get(name) == expected, f"signing boundary omits validated {name}")

binding_script = str(binding_step.get("run", ""))
for binding_guard in (
    "git ls-remote --exit-code --tags origin",
    '"$REVIEWED_REF" "$REVIEWED_REF^{}"',
    '"$current_object_sha" == "$REVIEWED_REF_OBJECT_SHA"',
    '"$current_source_sha" == "$SOURCE_SHA"',
    "Reviewed tag was deleted before signing",
    "Reviewed tag moved before signing",
):
    require(binding_guard in binding_script, f"signing reviewed-ref guard is missing: {binding_guard}")

with tempfile.TemporaryDirectory(prefix="jumpgate-signing-binding-") as temporary:
    fake_git_dir = pathlib.Path(temporary)
    fake_git = fake_git_dir / "git"
    fake_git.write_text(
        """#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
case "$1" in
  check-ref-format)
    [[ "$2" == 'refs/tags/reviewed-fixture' ]]
    ;;
  ls-remote)
    case "$FAKE_GIT_SCENARIO" in
      stable)
        printf '%s\\t%s\\n' \\
          bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb refs/tags/reviewed-fixture \\
          cccccccccccccccccccccccccccccccccccccccc 'refs/tags/reviewed-fixture^{}'
        ;;
      moved)
        printf '%s\\t%s\\n' \\
          dddddddddddddddddddddddddddddddddddddddd refs/tags/reviewed-fixture \\
          cccccccccccccccccccccccccccccccccccccccc 'refs/tags/reviewed-fixture^{}'
        ;;
      deleted)
        exit 2
        ;;
      *)
        exit 99
        ;;
    esac
    ;;
  *)
    exit 98
    ;;
esac
""",
        encoding="utf-8",
        newline="\n",
    )
    fake_git.chmod(0o755)
    binding_prefix = """
fake_git_dir="$FAKE_GIT_DIR"
if command -v cygpath >/dev/null 2>&1; then
  fake_git_dir="$(cygpath -u "$fake_git_dir")"
fi
export PATH="$fake_git_dir:$PATH"
"""
    binding_base_env = {
        "FAKE_GIT_DIR": str(fake_git_dir),
        "REVIEWED_REF": "refs/tags/reviewed-fixture",
        "REVIEWED_REF_KIND": "tag",
        "REVIEWED_REF_OBJECT_SHA": "b" * 40,
        "SOURCE_SHA": "c" * 40,
    }
    signing_results = {}
    for scenario in ("stable", "moved", "deleted"):
        scenario_env = dict(binding_base_env, FAKE_GIT_SCENARIO=scenario)
        signing_results[scenario] = run_bash(binding_prefix + binding_script, env=scenario_env)
    require(signing_results["stable"].returncode == 0,
            f"stable reviewed tag failed signing-boundary validation: {signing_results['stable'].stderr}")
    require(signing_results["moved"].returncode != 0,
            "moved reviewed tag passed signing-boundary validation")
    require("Reviewed tag moved before signing" in signing_results["moved"].stderr,
            "moved reviewed tag did not fail through the sealed signing guard")
    require(signing_results["deleted"].returncode != 0,
            "deleted reviewed tag passed signing-boundary validation")
    require("Reviewed tag was deleted before signing" in signing_results["deleted"].stderr,
            "deleted reviewed tag did not fail through the sealed signing guard")

signing_job_text = "\n".join(value for _, value in strings(jobs["sign_release_apks"]))
build_job_text = "\n".join(value for _, value in strings(jobs["build_candidates"]))
publish_job_text = "\n".join(value for _, value in strings(jobs["publish_draft_release"]))
for required_guard in (
    "PKCS12 keystore secret is absent",
    "strict base64",
    "PrivateKeyEntry",
    "expected signer fingerprint",
    "androiddebugkey",
    "CN=Android Debug",
    "--ks-type PKCS12",
):
    require(required_guard in signing_job_text, f"signing guard is missing: {required_guard}")
require("secrets." not in build_job_text, "reviewed source build must not receive release secrets")
require(".android/debug.keystore" not in workflow_text, "release workflow permits debug-keystore fallback")

matrix = jobs["build_candidates"]["strategy"]["matrix"]["include"]
require(
    {entry["abi"] for entry in matrix} == {"arm64-v8a", "armeabi-v7a"},
    "release build matrix must contain exactly both supported ABIs",
)
require(len(matrix) == 2, "release build matrix contains an unexpected entry")

action_pattern = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@[0-9a-f]{40}$")
action_steps = []
artifact_steps = []
for job_name, job in jobs.items():
    for step in job.get("steps", []):
        action = step.get("uses")
        if not action:
            continue
        action_steps.append((job_name, step))
        require(action_pattern.fullmatch(action) is not None, f"action is not SHA-pinned: {action}")
        if "/upload-artifact@" in action or "/download-artifact@" in action:
            artifact_steps.append((job_name, step))
require(action_steps, "release workflow has no actions")
require(artifact_steps, "release workflow has no artifact transfers")
release_app_token_step = named_step(
    "publish_draft_release",
    "Mint repository-scoped dedicated release App token",
)
require(
    release_app_token_step.get("uses")
    == "actions/create-github-app-token@bcd2ba49218906704ab6c1aa796996da409d3eb1",
    "dedicated release App token action is absent or not pinned to v3.2.0",
)
require(
    release_app_token_step.get("with")
    == {
        "client-id": "${{ secrets.JUMPGATE_ANDROID_RELEASE_APP_CLIENT_ID }}",
        "private-key": "${{ secrets.JUMPGATE_ANDROID_RELEASE_APP_PRIVATE_KEY }}",
        "owner": "${{ github.repository_owner }}",
        "repositories": "${{ github.event.repository.name }}",
        "permission-administration": "write",
        "permission-contents": "write",
        "permission-metadata": "read",
    },
    "release App token must be repository-scoped with only required explicit permissions",
)
publish_step_names = [step.get("name") for step in jobs["publish_draft_release"].get("steps", [])]
require(
    publish_step_names.index("Mint repository-scoped dedicated release App token") + 1
    == publish_step_names.index("Create new tag and draft release without overwrite APIs"),
    "dedicated release App token must be minted immediately before publication",
)
app_token_expression = "${{ steps.release_app_token.outputs.token }}"
app_token_hits = [
    path
    for path, value in strings(workflow)
    if app_token_expression in value
]
require(
    len(app_token_hits) == 1
    and app_token_hits[0][:2] == ("jobs", "publish_draft_release")
    and app_token_hits[0][-2:] == ("with", "github-token"),
    "dedicated release App token escaped the protected GitHub Script input",
)
require("${{ github.token }}" not in publish_job_text,
        "publication must not fall back to the generic workflow token")
for job_name, step in artifact_steps:
    settings = step.get("with", {})
    artifact_name = str(settings.get("name", ""))
    require("${{ github.run_id }}" in artifact_name, f"{job_name} artifact omits run_id")
    require("${{ github.run_attempt }}" in artifact_name, f"{job_name} artifact omits run_attempt")
    require("pattern" not in settings, f"{job_name} uses broad artifact pattern matching")
    require("run-id" not in settings, f"{job_name} can download a foreign workflow run")
    require("repository" not in settings, f"{job_name} can download a foreign repository artifact")

require("actions/attest-build-provenance@96278af6caaf10aea03fd8d33a09a777ca52d62f" in workflow_text,
        "build provenance action is absent or unpinned")
require("SYFT_VERSION: 1.46.0" in workflow_text, "Syft version must be immutable")
require(
    "SYFT_LINUX_AMD64_SHA256: d654f678b709eb53c393d38519d5ed7d2e57205529404018614cfefa0fb2b5ca"
    in workflow_text,
    "Syft archive checksum must be pinned",
)
require("spdx.json" in workflow_text and "SHA256SUMS" in workflow_text, "SBOM/checksum outputs are missing")

assemble_text = "\n".join(value for _, value in strings(jobs["assemble_release"]))
for token in (
    "jumpgate-android-release-signed-arm64-v8a-${{ github.run_id }}-${{ github.run_attempt }}",
    "jumpgate-android-release-signed-armeabi-v7a-${{ github.run_id }}-${{ github.run_attempt }}",
    "verify-android-release.sh",
    "EXPECTED_CORE_LIBRARY",
    "Syft found no package inventory",
    'document.get("spdxVersion") != "SPDX-2.3"',
    'root_id = f"SPDXRef-Package-Jumpgate-APK-{abi}"',
    '"checksumValue": apk_hash',
    '"primaryPackagePurpose": "APPLICATION"',
    '"licenseConcluded": "GPL-2.0-or-later"',
    '"licenseDeclared": "GPL-2.0-or-later"',
    '"documentDescribes"',
    '"relationshipType": "DESCRIBES"',
    '"relationshipType": "CONTAINS"',
):
    require(token in assemble_text, f"aggregate release guard is missing: {token}")
require(assemble_text.count('"GPL-2.0-or-later"') == 2,
        "SPDX normalization must license only the synthetic Jumpgate APK root")
assemble_python = python_heredocs(
    str(named_step("assemble_release", "Generate SPDX SBOMs and release metadata").get("run", ""))
)
require(len(assemble_python) == 1, "SPDX normalization must remain one auditable Python block")
ast.parse(assemble_python[0], filename="jumpgate-spdx-normalizer.py")
require(
    '${{ env.RELEASE_DIR }}/Jumpgate-${{ needs.validate_release_inputs.outputs.version_name }}-arm64-v8a.apk'
    in assemble_text,
    "provenance does not attest the exact arm64 APK assembled from the workflow SHA",
)

publish_step = named_step(
    "publish_draft_release",
    "Create new tag and draft release without overwrite APIs",
)
publish_env = publish_step.get("env", {})
for name, expected in (
    ("VALIDATED_DEFAULT_BRANCH", "${{ needs.validate_release_inputs.outputs.default_branch }}"),
    ("REVIEWED_REF", "${{ needs.validate_release_inputs.outputs.reviewed_ref }}"),
    ("REVIEWED_REF_KIND", "${{ needs.validate_release_inputs.outputs.reviewed_ref_kind }}"),
    (
        "REVIEWED_REF_OBJECT_SHA",
        "${{ needs.validate_release_inputs.outputs.reviewed_ref_object_sha }}",
    ),
    ("RUN_ID", "${{ github.run_id }}"),
    ("RUN_ATTEMPT", "${{ github.run_attempt }}"),
    ("EXPECTED_RELEASE_APP_ID", "${{ secrets.JUMPGATE_ANDROID_RELEASE_APP_ID }}"),
):
    require(publish_env.get(name) == expected, f"publication omits validated {name}")
publish_script = str(publish_step.get("with", {}).get("script", ""))
publish_text = "\n".join(value for _, value in strings(jobs["publish_draft_release"]))
for required_create_only_token in (
    "github.rest.repos.getRelease({",
    "getReleaseByTag",
    "getReleaseAsset",
    "getRef",
    "createTag",
    "createRef",
    "createRelease",
    "uploadReleaseAsset",
    "waitForFinalizedReleaseAsset",
    "draft: true",
    "prerelease: false",
    "refusing to overwrite",
    "listReleases",
    "listMatchingRefs",
    "EXPECTED_HISTORY_FINGERPRINT",
    "Canonical release history changed after monotonicity validation",
    "Published source must exactly match github.workflow_sha",
    "const validatedDefaultBranch = process.env.VALIDATED_DEFAULT_BRANCH;",
    "Validated default branch is malformed",
    "Repository default branch changed after source validation",
    "Repository default-branch head changed after source validation",
    "Release tag is outside the canonical v<semver> namespace",
    "const ownershipMarker = [",
    "jumpgate-android-release-owner-v1",
    "const annotatedTagMessage = [",
    "sha: ownedTagObjectSha",
    "assertPostCreationHistory(initialHistory)",
    "Owned annotated tag did not become visible in canonical history",
    "Math.min(250 * (2 ** attempt), 5000)",
    "assertReviewedRefBinding('immediately before release creation')",
    "current_user_can_bypass === 'always'",
    "getRepoRuleset",
    "active.length !== 1",
    "ruleset.source_type === 'Repository'",
    "sameStrings(includes, ['refs/tags/v*'])",
    "excludes.length === 0",
    "sameStrings([...ruleTypes].sort(), requiredRules)",
    "bypassActors.length === 1",
    "actor.actor_type === 'Integration'",
    "actor.actor_id === expectedReleaseAppId",
    "actor.bypass_mode === 'always'",
    "Dedicated release App ruleset drifted",
    "GitHub has no compare-and-delete ref API",
    "JUMPGATE_RELEASE_MANUAL_RECONCILIATION_REQUIRED",
    "publicationPhase = 'post-upload-revalidation'",
    "assertReviewedRefBinding('after final asset upload')",
    "assertFinalReleaseState(release, expectedAssets)",
    "release_id: originalRelease.id",
    "observed.id !== originalRelease.id",
    "observed.html_url !== originalRelease.html_url",
    "releaseState.tag_name !== tag",
    "releaseState.name !== releaseName",
    "releaseState.target_commitish !== sourceSha",
    "releaseState.draft !== true",
    "releaseState.prerelease !== false",
    "asset.id !== expected.id",
    "asset.size !== expected.size",
    "asset.digest !== expected.digest",
    "asset.content_type !== expected.contentType",
    "asset.state !== 'uploaded'",
    "observedAssets.length !== expectedAssets.size",
    "expectedAssets.get(asset.name)",
    "sameStrings([...seenNames].sort(), [...expectedAssets.keys()].sort())",
):
    require(required_create_only_token in publish_text, f"create-only release guard is missing: {required_create_only_token}")
for forbidden_overwrite_token in (
    "updateRelease",
    "updateRef",
    "--clobber",
    "gh release",
    "softprops/action-gh-release",
):
    require(forbidden_overwrite_token not in publish_text, f"overwriting release path is present: {forbidden_overwrite_token}")
require("prerelease: true" not in publish_script,
        "protected draft release must not be marked prerelease")
require("'Jumpgate-' + versionName + '-arm64-v8a.apk'" in publish_text,
        "arm64 public asset name drifted")
require("'Jumpgate-' + versionName + '-armeabi-v7a.apk'" in publish_text,
        "armv7 public asset name drifted")
for cleanup_token in (
    "async function readOwnedTagState()",
    "ref.object.type !== 'tag'",
    "ref.object.sha !== ownedTagObjectSha",
    "response.data.message === annotatedTagMessage",
    "object.type === 'commit'",
    "object.sha === sourceSha",
    "function emitManualReconciliation(reason)",
    "automatic_tag_deletion=disabled",
    "cause=github_no_compare_and_delete_ref_api",
    "action=inspect_exact_tag_and_release_before_retry",
    "if (releaseRefMayExist) emitManualReconciliation(publicationPhase);",
):
    require(cleanup_token in publish_text, f"manual reconciliation contract is missing: {cleanup_token}")
for forbidden_tag_cleanup in (
    "deleteRef",
    "deleteOwnedTagAfterConfirmedReleaseDeletion",
    "releaseDeletionConfirmed",
):
    require(forbidden_tag_cleanup not in publish_text,
            f"automatic release-tag cleanup remains: {forbidden_tag_cleanup}")
require(publish_text.count("github.rest.repos.deleteRelease") == 1,
        "release cleanup must have exactly one auditable deletion path")
require(publish_step.get("with", {}).get("github-token") == app_token_expression,
        "publication does not use the dedicated release App token")
publication_api_methods = set(
    re.findall(r"github\.rest\.([A-Za-z0-9_]+\.[A-Za-z0-9_]+)", publish_script)
)
require(
    publication_api_methods
    == {
        "git.createRef",
        "git.createTag",
        "git.getRef",
        "git.getTag",
        "git.listMatchingRefs",
        "repos.createRelease",
        "repos.deleteRelease",
        "repos.get",
        "repos.getRelease",
        "repos.getReleaseAsset",
        "repos.getReleaseByTag",
        "repos.getRepoRuleset",
        "repos.getRepoRulesets",
        "repos.listReleases",
        "repos.uploadReleaseAsset",
    },
    "publication GitHub API allowlist drifted",
)
for forbidden_escape_hatch in (
    "github.request",
    "github.graphql",
    "createRepoRuleset",
    "updateRepoRuleset",
    "deleteRepoRuleset",
):
    require(forbidden_escape_hatch not in publish_script,
            f"publication can escape its API allowlist: {forbidden_escape_hatch}")
require(
    "const canonicalTagPattern = /^v(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\."
    "(0|[1-9][0-9]*)$/;" in publish_script,
    "publication semantic-tag regex must require literal dot separators",
)
require(publish_script.count("await assertReviewedRefBinding(") == 3,
        "reviewed tag must be rebound before tag creation, release creation, and success")
require(publish_script.count("await assertRequiredTagRuleset();") == 3,
        "tag ruleset must be revalidated before tag creation, release creation, and success")
require(publish_script.count("await assertRepositorySource();") == 3,
        "default/source head must be revalidated before tag creation, release creation, and success")
require(publish_script.count("await assertOwnedTag();") == 4,
        "owned tag checks must cover listing convergence, createRef ambiguity, release creation, and final success")
require(publish_script.count("github.rest.repos.getReleaseByTag({") == 1,
        "tag lookup must be limited to initial absence and recovery helpers")
require(publish_script.count("github.rest.repos.getRelease({") == 1,
        "terminal identity and asset validation must use one exact release-ID lookup")

create_tag_index = publish_script.index("const tagObjectResponse = await github.rest.git.createTag({")
create_ref_index = publish_script.index("await github.rest.git.createRef({", create_tag_index)
post_history_index = publish_script.index("await assertPostCreationHistory(initialHistory);", create_ref_index)
post_source_index = publish_script.index("await assertRepositorySource();", post_history_index)
post_ruleset_index = publish_script.index("await assertRequiredTagRuleset();", post_source_index)
post_owned_index = publish_script.index("await assertOwnedTag();", post_ruleset_index)
post_reviewed_index = publish_script.index(
    "await assertReviewedRefBinding('immediately before release creation');",
    post_owned_index,
)
release_index = publish_script.index("const response = await github.rest.repos.createRelease({")
require(
    create_tag_index
    < create_ref_index
    < post_history_index
    < post_source_index
    < post_ruleset_index
    < post_owned_index
    < post_reviewed_index
    < release_index,
    "post-tag history, source, ruleset, ownership, and reviewed-ref checks must precede release creation",
)

upload_index = publish_script.index("const response = await github.rest.repos.uploadReleaseAsset({")
post_upload_phase_index = publish_script.index(
    "publicationPhase = 'post-upload-revalidation';", upload_index
)
final_source_index = publish_script.index("await assertRepositorySource();", post_upload_phase_index)
final_ruleset_index = publish_script.index("await assertRequiredTagRuleset();", final_source_index)
final_owned_index = publish_script.index("await assertOwnedTag();", final_ruleset_index)
final_reviewed_index = publish_script.index(
    "await assertReviewedRefBinding('after final asset upload');", final_owned_index
)
final_release_index = publish_script.index(
    "const finalRelease = await assertFinalReleaseState(release, expectedAssets);",
    final_reviewed_index,
)
output_index = publish_script.index("core.setOutput('release-url', finalRelease.html_url);", final_release_index)
require(
    upload_index
    < post_upload_phase_index
    < final_source_index
    < final_ruleset_index
    < final_owned_index
    < final_reviewed_index
    < final_release_index
    < output_index,
    "all terminal source, ruleset, tag, reviewed-ref, release, and asset checks must follow uploads",
)


def run_publication_scenarios(scenarios: list[str]) -> dict[str, dict]:
    node = shutil.which("node")
    require(node is not None, "Node.js is required for executable publication-policy tests")
    with tempfile.TemporaryDirectory(prefix="jumpgate-publication-policy-") as temporary:
        temporary_path = pathlib.Path(temporary)
        script_path = temporary_path / "publish-script.js"
        harness_path = temporary_path / "publish-harness.js"
        release_root = temporary_path / "release-assets"
        script_path.write_text(publish_script, encoding="utf-8", newline="\n")
        harness_path.write_text(
            r"""// SPDX-License-Identifier: GPL-2.0-or-later
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const scriptPath = process.argv[2];
const releaseRoot = process.argv[3];
const scenarios = process.argv.slice(4);
const script = fs.readFileSync(scriptPath, 'utf8');
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
const execute = new AsyncFunction(
  'github', 'context', 'core', 'require', 'process', 'setTimeout', script,
);

const SOURCE = 'c'.repeat(40);
const PRIOR_OBJECT = 'a'.repeat(40);
const REVIEWED_OBJECT = 'b'.repeat(40);
const OWNED_OBJECT = 'd'.repeat(40);
const REVIEWED_MOVED_OBJECT = 'e'.repeat(40);
const REPLACEMENT_OBJECT = 'f'.repeat(40);
const PRIOR_MOVED_OBJECT = '1'.repeat(40);
const EXTRA_OBJECT = '2'.repeat(40);
const HEAD_MOVED = '3'.repeat(40);
const RELEASE_APP_ID = 424242;
const RELEASE_TAG = 'v1.0.0';
const PRIOR_TAG = 'v0.9.0';
const EXTRA_TAG = 'v0.9.1';
const REVIEWED_REF = 'refs/tags/reviewed-fixture';
const FULL_RELEASE_REF = 'refs/tags/' + RELEASE_TAG;

function apiError(status, message) {
  const error = new Error(message);
  error.status = status;
  return error;
}

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function expectedHistoryFingerprint() {
  return crypto.createHash('sha256').update(
    'release\t' + PRIOR_TAG + '\n' + 'tag\t' + PRIOR_TAG + '\t' + PRIOR_OBJECT + '\n',
    'utf8',
  ).digest('hex');
}

function protectedRuleset(enforcement = 'active') {
  return {
    id: 42,
    name: 'Protect Jumpgate release tags',
    target: 'tag',
    source_type: 'Repository',
    source: 'ruizkinio/Jumpgate-kodi',
    enforcement,
    current_user_can_bypass: 'always',
    conditions: {
      ref_name: {include: ['refs/tags/v*'], exclude: []},
    },
    rules: [{type: 'creation'}, {type: 'update'}, {type: 'deletion'}],
    bypass_actors: [
      {actor_id: RELEASE_APP_ID, actor_type: 'Integration', bypass_mode: 'always'},
    ],
  };
}

function rulesetSummary(ruleset) {
  return {
    id: ruleset.id,
    name: ruleset.name,
    target: ruleset.target,
    source_type: ruleset.source_type,
    source: ruleset.source,
    enforcement: ruleset.enforcement,
  };
}

async function runScenario(scenario) {
  const releaseDir = path.join(releaseRoot, scenario);
  fs.mkdirSync(releaseDir, {recursive: true});
  const versionName = '22.0-FIXTURE-Jumpgate-1.0.0';
  const assetNames = [
    'Jumpgate-' + versionName + '-arm64-v8a.apk',
    'Jumpgate-' + versionName + '-armeabi-v7a.apk',
    'Jumpgate-' + versionName + '-arm64-v8a.spdx.json',
    'Jumpgate-' + versionName + '-armeabi-v7a.spdx.json',
    'Jumpgate-' + versionName + '-metadata.json',
    'Jumpgate-' + versionName + '-provenance.json',
    'SHA256SUMS',
  ];
  for (const name of assetNames) {
    fs.writeFileSync(path.join(releaseDir, name), 'fixture:' + name);
  }

  Object.assign(process.env, {
    ATTESTATION_URL: 'https://example.invalid/attestation',
    EXPECTED_VERSION_NAME: versionName,
    EXPECTED_HISTORY_FINGERPRINT: expectedHistoryFingerprint(),
    EXPECTED_RELEASE_APP_ID: scenario === 'malformed_release_app_id'
      ? '9007199254740992'
      : String(RELEASE_APP_ID),
    RELEASE_DIR: releaseDir,
    RELEASE_TAG,
    REVIEWED_REF,
    REVIEWED_REF_KIND: 'tag',
    REVIEWED_REF_OBJECT_SHA: REVIEWED_OBJECT,
    RUN_ATTEMPT: '2',
    RUN_ID: '123456',
    SOURCE_SHA: SOURCE,
    VALIDATED_DEFAULT_BRANCH: 'trunk',
    WORKFLOW_SHA: SOURCE,
  });

  const calls = {
    createRef: 0,
    createRelease: 0,
    createTag: 0,
    defaultHead: 0,
    deleteRef: 0,
    deleteRelease: 0,
    getRelease: 0,
    getReleaseByTag: 0,
    getReleaseAsset: 0,
    getRepoRuleset: 0,
    listMatchingRefs: 0,
    listReleases: 0,
    releaseTagRef: 0,
    repositoryMetadata: 0,
    reviewedRef: 0,
    rulesets: 0,
    uploadReleaseAsset: 0,
  };
  const logs = {error: [], warning: []};
  const outputs = {};
  const retryDelays = [];
  let release = null;
  let releaseTagRef = null;
  let ownedMessage = null;
  let finalMutationApplied = false;
  const tagObjects = new Map([
    [REVIEWED_OBJECT, {
      sha: REVIEWED_OBJECT,
      tag: 'reviewed-fixture',
      message: 'Reviewed fixture',
      object: {type: 'commit', sha: SOURCE},
    }],
    [REVIEWED_MOVED_OBJECT, {
      sha: REVIEWED_MOVED_OBJECT,
      tag: 'reviewed-fixture',
      message: 'Moved reviewed fixture',
      object: {type: 'commit', sha: SOURCE},
    }],
    [REPLACEMENT_OBJECT, {
      sha: REPLACEMENT_OBJECT,
      tag: RELEASE_TAG,
      message: 'Unowned replacement with the same target',
      object: {type: 'commit', sha: SOURCE},
    }],
  ]);

  function replacementRef() {
    return {ref: FULL_RELEASE_REF, object: {type: 'tag', sha: REPLACEMENT_OBJECT}};
  }

  function releaseAsset(id, name, data) {
    return {
      id,
      name,
      size: data.length,
      digest: 'sha256:' + crypto.createHash('sha256').update(data).digest('hex'),
      content_type: name.endsWith('.apk')
        ? 'application/vnd.android.package-archive'
        : 'application/octet-stream',
      state: 'uploaded',
    };
  }

  function protectedRelease(id = 77, htmlUrl = 'https://example.invalid/release', assets = []) {
    return {
      id,
      tag_name: RELEASE_TAG,
      target_commitish: SOURCE,
      name: 'Jumpgate ' + versionName,
      draft: true,
      prerelease: false,
      html_url: htmlUrl,
      assets: clone(assets),
    };
  }

  function applyPostUploadReleaseMutation() {
    if (finalMutationApplied || calls.uploadReleaseAsset !== assetNames.length) return;
    finalMutationApplied = true;
    if (scenario === 'post_upload_release_deleted') {
      release = null;
    } else if (scenario === 'post_upload_release_recreated') {
      release = protectedRelease(
        88,
        'https://example.invalid/recreated',
        release ? release.assets : [],
      );
    } else if (scenario === 'post_upload_release_draft_mutated') {
      release.draft = false;
    } else if (scenario === 'post_upload_release_state_mutated') {
      release.prerelease = true;
    } else if (scenario === 'post_upload_asset_id_replaced') {
      release.assets[0].id += 10000;
    } else if (scenario === 'post_upload_asset_size_mutated') {
      release.assets[0].size += 1;
    } else if (scenario === 'post_upload_asset_digest_mutated') {
      release.assets[0].digest = 'sha256:' + '0'.repeat(64);
    } else if (scenario === 'post_upload_asset_content_type_mutated') {
      release.assets[0].content_type = 'text/plain';
    } else if (scenario === 'post_upload_asset_state_mutated') {
      release.assets[0].state = 'open';
    } else if (scenario === 'post_upload_asset_extra') {
      release.assets.push(releaseAsset(9999, 'unexpected-release-asset.bin', Buffer.from('extra')));
    } else if (scenario === 'post_upload_asset_missing') {
      release.assets.pop();
    }
  }

  const repos = {
    async listReleases() {
      calls.listReleases += 1;
      const data = [{tag_name: PRIOR_TAG}];
      if (scenario === 'history_changed_after_tag' && calls.listReleases >= 2) {
        data.push({tag_name: EXTRA_TAG});
      }
      return {data};
    },
    async getReleaseByTag() {
      calls.getReleaseByTag += 1;
      if (release === null) throw apiError(404, 'release not found');
      return {data: clone(release)};
    },
    async getRelease({release_id: releaseId}) {
      calls.getRelease += 1;
      applyPostUploadReleaseMutation();
      if (scenario === 'post_upload_release_lookup_transient') {
        if (calls.getRelease === 1) throw apiError(404, 'simulated transient release absence');
        if (calls.getRelease === 2) throw apiError(502, 'simulated transient release lookup');
      } else if (scenario === 'post_upload_release_lookup_failure') {
        throw apiError(502, 'simulated persistent release lookup failure');
      } else if (scenario === 'post_upload_release_lookup_forbidden') {
        throw apiError(403, 'simulated non-retryable release lookup failure');
      } else if (scenario === 'post_upload_release_id_mismatch') {
        const observed = protectedRelease(88, 'https://example.invalid/recreated', release.assets);
        return {data: observed};
      }
      if (release === null || release.id !== releaseId) {
        throw apiError(404, 'release not found');
      }
      return {data: clone(release)};
    },
    async getReleaseAsset({asset_id: assetId}) {
      calls.getReleaseAsset += 1;
      if (scenario === 'asset_finalization_lookup_failure') {
        throw apiError(502, 'simulated finalized-asset lookup failure');
      }
      if (release === null) throw apiError(404, 'release not found');
      const asset = release.assets.find((candidate) => candidate.id === assetId);
      if (!asset) throw apiError(404, 'asset not found');
      const observed = clone(asset);
      if (scenario === 'asset_finalization_never_completes') {
        observed.state = 'open';
        observed.digest = null;
      } else if (scenario === 'asset_finalization_digest_mismatch') {
        observed.digest = 'sha256:' + '0'.repeat(64);
      }
      return {data: observed};
    },
    async getRepoRulesets(params) {
      calls.rulesets += 1;
      if (params.targets !== 'tag' || params.includes_parents !== true) {
        throw new Error('ruleset enumeration is not scoped to effective tag rulesets');
      }
      if (scenario === 'missing_ruleset') return {data: []};
      if (scenario === 'disabled_ruleset') {
        return {data: [rulesetSummary(protectedRuleset('disabled'))]};
      }
      if (scenario === 'ruleset_disappears_after_tag' && calls.rulesets >= 2) {
        return {data: []};
      }
      const data = [rulesetSummary(protectedRuleset())];
      if (scenario === 'extra_active_ruleset') {
        const extra = protectedRuleset();
        extra.id = 43;
        extra.name = 'Unexpected second active tag ruleset';
        data.push(rulesetSummary(extra));
      }
      return {data};
    },
    async getRepoRuleset(params) {
      calls.getRepoRuleset += 1;
      if (params.ruleset_id !== 42 || params.includes_parents !== true) {
        throw new Error('unexpected ruleset detail request');
      }
      const data = protectedRuleset();
      if (scenario === 'extra_bypass_actor') {
        data.bypass_actors.push({actor_id: 5, actor_type: 'RepositoryRole', bypass_mode: 'always'});
      } else if (scenario === 'missing_bypass_actor') {
        data.bypass_actors = [];
      } else if (scenario === 'wrong_bypass_actor') {
        data.bypass_actors[0].actor_id = RELEASE_APP_ID + 1;
      } else if (scenario === 'wrong_bypass_actor_type') {
        data.bypass_actors[0].actor_type = 'RepositoryRole';
      } else if (scenario === 'wrong_bypass_mode') {
        data.bypass_actors[0].bypass_mode = 'exempt';
      } else if (scenario === 'wrong_effective_bypass') {
        data.current_user_can_bypass = 'never';
      } else if (scenario === 'redacted_bypass_actors') {
        delete data.bypass_actors;
      } else if (scenario === 'ruleset_exclusion') {
        data.conditions.ref_name.exclude = ['refs/tags/v1.0.0'];
      } else if (scenario === 'ruleset_mutated_after_tag' && calls.getRepoRuleset >= 2) {
        data.bypass_actors.push({actor_id: 5, actor_type: 'RepositoryRole', bypass_mode: 'always'});
      } else if (
        scenario === 'post_upload_ruleset_mutated'
        && calls.getRepoRuleset >= 3
        && calls.uploadReleaseAsset === assetNames.length
      ) {
        data.bypass_actors.push({actor_id: 5, actor_type: 'RepositoryRole', bypass_mode: 'always'});
      }
      return {data};
    },
    async get() {
      calls.repositoryMetadata += 1;
      return {data: {default_branch: 'trunk'}};
    },
    async createRelease(params) {
      calls.createRelease += 1;
      if (
        params.tag_name !== RELEASE_TAG
        || params.target_commitish !== SOURCE
        || params.name !== 'Jumpgate ' + versionName
        || params.draft !== true
        || params.prerelease !== false
        || !releaseTagRef
        || releaseTagRef.object.sha !== OWNED_OBJECT
      ) {
        throw new Error('release creation was not bound to the exact owned tag and source');
      }
      release = protectedRelease();
      if (scenario === 'ambiguous_release_visible') {
        throw apiError(502, 'simulated ambiguous release creation');
      }
      return {data: clone(release)};
    },
    async uploadReleaseAsset(params) {
      calls.uploadReleaseAsset += 1;
      if ([
        'asset_delete_failure',
        'asset_delete_ambiguous',
        'asset_release_still_visible',
        'asset_release_recreated',
        'asset_recreated_same_target_tag',
        'asset_cleanup_success_preserves_tag',
      ].includes(scenario)) {
        throw apiError(502, 'simulated asset upload failure');
      }
      if (
        params.release_id !== 77
        || !assetNames.includes(params.name)
        || params.headers['content-type'] !== 'application/octet-stream'
        || !Buffer.isBuffer(params.data)
        || params.headers['content-length'] !== params.data.length
      ) {
        throw new Error('asset upload was not bound to the expected release bytes');
      }
      const uploaded = releaseAsset(1000 + calls.uploadReleaseAsset, params.name, params.data);
      release.assets.push(uploaded);
      const accepted = clone(uploaded);
      accepted.state = 'open';
      accepted.digest = null;
      return {data: accepted};
    },
    async deleteRelease() {
      calls.deleteRelease += 1;
      if (scenario === 'asset_delete_failure') {
        throw apiError(500, 'simulated release deletion failure');
      }
      if (scenario === 'asset_delete_ambiguous') {
        release = null;
        throw apiError(502, 'simulated ambiguous release deletion');
      }
      if (scenario === 'asset_release_still_visible') {
        return {status: 204};
      }
      if (scenario === 'asset_release_recreated') {
        release = {id: 88, tag_name: RELEASE_TAG, html_url: 'https://example.invalid/recreated'};
        return {status: 204};
      }
      release = null;
      if (scenario === 'asset_recreated_same_target_tag') releaseTagRef = replacementRef();
      return {status: 204};
    },
  };

  const git = {
    async listMatchingRefs() {
      calls.listMatchingRefs += 1;
      const priorObject = scenario === 'prior_tag_moved_after_tag' && calls.listMatchingRefs >= 2
        ? PRIOR_MOVED_OBJECT
        : PRIOR_OBJECT;
      const data = [{ref: 'refs/tags/' + PRIOR_TAG, object: {type: 'tag', sha: priorObject}}];
      if (calls.listMatchingRefs >= 2) {
        if ([
          'owned_tag_replaced_after_tag',
          'post_create_tag_listing_missing_exact_ref_replaced',
        ].includes(scenario)) releaseTagRef = replacementRef();
        const releaseTagVisible = ![
          'post_create_tag_listing_never_converges',
          'post_create_tag_listing_missing_exact_ref_replaced',
        ].includes(scenario)
          && !(
            scenario === 'post_create_tag_listing_transient'
            && calls.listMatchingRefs < 4
          );
        if (releaseTagRef && releaseTagVisible) data.push(clone(releaseTagRef));
        if (scenario === 'history_changed_after_tag') {
          data.push({ref: 'refs/tags/' + EXTRA_TAG, object: {type: 'tag', sha: EXTRA_OBJECT}});
        }
      }
      return {data};
    },
    async getRef({ref}) {
      if (ref === 'heads/trunk') {
        calls.defaultHead += 1;
        const movedAfterTag = scenario === 'default_head_moved_after_tag'
          && calls.defaultHead >= 2;
        const movedAfterUpload = scenario === 'post_upload_default_head_moved'
          && calls.defaultHead >= 3
          && calls.uploadReleaseAsset === assetNames.length;
        const sha = movedAfterTag || movedAfterUpload ? HEAD_MOVED : SOURCE;
        return {data: {ref: 'refs/heads/trunk', object: {type: 'commit', sha}}};
      }
      if (ref === REVIEWED_REF.slice('refs/'.length)) {
        calls.reviewedRef += 1;
        if (scenario === 'reviewed_tag_deleted_after_tag' && calls.reviewedRef >= 2) {
          throw apiError(404, 'reviewed tag deleted');
        }
        const movedAfterTag = scenario === 'reviewed_tag_moved_after_tag'
          && calls.reviewedRef >= 2;
        const replacedAfterUpload = scenario === 'post_upload_reviewed_tag_replaced'
          && calls.reviewedRef >= 3
          && calls.uploadReleaseAsset === assetNames.length;
        const sha = movedAfterTag || replacedAfterUpload
          ? REVIEWED_MOVED_OBJECT
          : REVIEWED_OBJECT;
        return {data: {ref: REVIEWED_REF, object: {type: 'tag', sha}}};
      }
      if (ref === 'tags/' + RELEASE_TAG) {
        calls.releaseTagRef += 1;
        if (
          scenario === 'post_upload_owned_tag_replaced'
          && calls.releaseTagRef >= 3
          && calls.uploadReleaseAsset === assetNames.length
        ) {
          releaseTagRef = replacementRef();
        }
        if (releaseTagRef === null) throw apiError(404, 'tag not found');
        return {data: clone(releaseTagRef)};
      }
      throw new Error('unexpected getRef request: ' + ref);
    },
    async getTag({tag_sha: tagSha}) {
      const value = tagObjects.get(tagSha);
      if (!value) throw apiError(404, 'tag object not found: ' + tagSha);
      return {data: clone(value)};
    },
    async createTag(params) {
      calls.createTag += 1;
      if (
        params.tag !== RELEASE_TAG
        || params.object !== SOURCE
        || params.type !== 'commit'
        || !params.message.includes('jumpgate-android-release-owner-v1')
        || !params.message.includes('123456:2:' + RELEASE_TAG + ':' + SOURCE)
      ) {
        throw new Error('annotated ownership tag contract drifted');
      }
      ownedMessage = params.message;
      const data = {
        sha: OWNED_OBJECT,
        tag: RELEASE_TAG,
        message: ownedMessage,
        object: {type: 'commit', sha: SOURCE},
      };
      tagObjects.set(OWNED_OBJECT, data);
      return {data: clone(data)};
    },
    async createRef(params) {
      calls.createRef += 1;
      if (params.ref !== FULL_RELEASE_REF || params.sha !== OWNED_OBJECT) {
        throw new Error('release ref was not created from the annotated tag object');
      }
      releaseTagRef = {ref: FULL_RELEASE_REF, object: {type: 'tag', sha: OWNED_OBJECT}};
      if (scenario === 'ambiguous_create_ref') {
        throw apiError(502, 'simulated ambiguous createRef response');
      }
      return {data: clone(releaseTagRef)};
    },
    async deleteRef() {
      calls.deleteRef += 1;
      throw new Error('automatic release-tag deletion is forbidden');
    },
  };

  const github = {
    rest: {repos, git},
    async paginate(method, params) {
      const response = await method(params);
      return response.data;
    },
  };
  const core = {
    error(value) { logs.error.push(String(value)); },
    warning(value) { logs.warning.push(String(value)); },
    setOutput(name, value) { outputs[name] = value; },
  };
  const context = {repo: {owner: 'ruizkinio', repo: 'Jumpgate-kodi'}};

  let ok = false;
  let error = null;
  try {
    await execute(github, context, core, require, process, (callback, delay) => {
      retryDelays.push(delay);
      callback();
      return 0;
    });
    ok = true;
  } catch (caught) {
    error = caught && caught.message ? caught.message : String(caught);
  }
  return {
    scenario,
    ok,
    error,
    calls,
    logs,
    outputs,
    retryDelays,
    releaseExists: release !== null,
    releaseDraft: release && release.draft,
    releaseId: release && release.id,
    releasePrerelease: release && release.prerelease,
    releaseAssetNames: release && Array.isArray(release.assets)
      ? release.assets.map((asset) => asset.name)
      : null,
    tagObjectSha: releaseTagRef && releaseTagRef.object.sha,
  };
}

(async () => {
  const results = [];
  for (const scenario of scenarios) results.push(await runScenario(scenario));
  console.log('JUMPGATE_RESULTS=' + JSON.stringify(results));
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
""",
            encoding="utf-8",
            newline="\n",
        )
        completed = subprocess.run(
            [node, str(harness_path), str(script_path), str(release_root), *scenarios],
            cwd=root,
            check=False,
            capture_output=True,
            text=True,
        )
        require(completed.returncode == 0,
                f"publication mock harness failed: {completed.stderr}")
        result_lines = [
            line.removeprefix("JUMPGATE_RESULTS=")
            for line in completed.stdout.splitlines()
            if line.startswith("JUMPGATE_RESULTS=")
        ]
        require(len(result_lines) == 1,
                f"publication mock harness returned malformed output: {completed.stdout}")
        results = json.loads(result_lines[0])
        require(len(results) == len(scenarios), "publication mock harness dropped a scenario")
        return {item["scenario"]: item for item in results}


publication_scenarios = [
    "stable",
    "ambiguous_create_ref",
    "post_create_tag_listing_transient",
    "post_create_tag_listing_never_converges",
    "post_create_tag_listing_missing_exact_ref_replaced",
    "default_head_moved_after_tag",
    "history_changed_after_tag",
    "prior_tag_moved_after_tag",
    "owned_tag_replaced_after_tag",
    "reviewed_tag_moved_after_tag",
    "reviewed_tag_deleted_after_tag",
    "missing_ruleset",
    "disabled_ruleset",
    "extra_active_ruleset",
    "extra_bypass_actor",
    "missing_bypass_actor",
    "wrong_bypass_actor",
    "wrong_bypass_actor_type",
    "wrong_bypass_mode",
    "wrong_effective_bypass",
    "redacted_bypass_actors",
    "ruleset_exclusion",
    "malformed_release_app_id",
    "ruleset_disappears_after_tag",
    "ruleset_mutated_after_tag",
    "ambiguous_release_visible",
    "post_upload_default_head_moved",
    "post_upload_owned_tag_replaced",
    "post_upload_reviewed_tag_replaced",
    "post_upload_ruleset_mutated",
    "post_upload_release_deleted",
    "post_upload_release_recreated",
    "post_upload_release_draft_mutated",
    "post_upload_release_state_mutated",
    "post_upload_release_lookup_transient",
    "post_upload_release_lookup_failure",
    "post_upload_release_lookup_forbidden",
    "post_upload_release_id_mismatch",
    "post_upload_asset_id_replaced",
    "post_upload_asset_size_mutated",
    "post_upload_asset_digest_mutated",
    "post_upload_asset_content_type_mutated",
    "post_upload_asset_state_mutated",
    "post_upload_asset_extra",
    "post_upload_asset_missing",
    "asset_finalization_digest_mismatch",
    "asset_finalization_never_completes",
    "asset_finalization_lookup_failure",
    "asset_delete_failure",
    "asset_delete_ambiguous",
    "asset_release_still_visible",
    "asset_release_recreated",
    "asset_recreated_same_target_tag",
    "asset_cleanup_success_preserves_tag",
]
publication_results = run_publication_scenarios(publication_scenarios)

for stable_scenario in (
    "stable",
    "ambiguous_create_ref",
    "post_create_tag_listing_transient",
    "post_upload_release_lookup_transient",
):
    result = publication_results[stable_scenario]
    expected_release_reads = 3 if stable_scenario == "post_upload_release_lookup_transient" else 1
    expected_retry_delays = (
        [250, 500]
        if stable_scenario in {
            "post_create_tag_listing_transient",
            "post_upload_release_lookup_transient",
        }
        else []
    )
    require(result["ok"], f"{stable_scenario} publication failed: {result['error']}")
    require(result["calls"]["createTag"] == 1 and result["calls"]["createRelease"] == 1,
            f"{stable_scenario} did not create one annotated tag and release")
    require(result["calls"]["uploadReleaseAsset"] == 7,
            f"{stable_scenario} did not upload the exact release payload")
    require(result["calls"]["getReleaseAsset"] == 7,
            f"{stable_scenario} did not finalize every accepted release asset")
    require(
        result["calls"]["repositoryMetadata"] == 3
        and result["calls"]["defaultHead"] == 3
        and result["calls"]["rulesets"] == 3
        and result["calls"]["getRepoRuleset"] == 3
        and result["calls"]["reviewedRef"] == 3
        and result["calls"]["getReleaseByTag"] == 1
        and result["calls"]["getRelease"] == expected_release_reads,
        f"{stable_scenario} did not execute every terminal revalidation exactly once",
    )
    require(result["retryDelays"] == expected_retry_delays,
            f"{stable_scenario} release lookup retry schedule drifted")
    expected_release_ref_reads = {
        "ambiguous_create_ref": 4,
        "post_create_tag_listing_transient": 5,
    }.get(stable_scenario, 3)
    require(result["calls"]["releaseTagRef"] == expected_release_ref_reads,
            f"{stable_scenario} release-tag ownership call count drifted")
    expected_history_reads = 4 if stable_scenario == "post_create_tag_listing_transient" else 2
    require(result["calls"]["listMatchingRefs"] == expected_history_reads,
            f"{stable_scenario} canonical tag-list read count drifted")
    require(result["calls"]["deleteRelease"] == 0,
            f"{stable_scenario} unexpectedly entered release cleanup")
    require(result["calls"]["deleteRef"] == 0,
            f"{stable_scenario} unexpectedly deleted a release tag")
    require(result["tagObjectSha"] == "d" * 40,
            f"{stable_scenario} release ref is not bound to the owned annotated object")
    require(result["outputs"].get("release-url") == "https://example.invalid/release",
            f"{stable_scenario} did not export the created release URL")
    require(result["releaseId"] == 77
            and result["releaseDraft"] is True
            and result["releasePrerelease"] is False,
            f"{stable_scenario} did not preserve the original non-prerelease draft")
    require(len(result["releaseAssetNames"] or []) == 7
            and len(set(result["releaseAssetNames"])) == 7,
            f"{stable_scenario} final release asset set drifted")

expected_publication_failures = {
    "post_create_tag_listing_never_converges": (
        "Owned annotated tag did not become visible in canonical history"
    ),
    "post_create_tag_listing_missing_exact_ref_replaced": (
        "Release tag is absent or no longer points to the exact owned annotated tag object"
    ),
    "default_head_moved_after_tag": "Repository default-branch head changed after source validation",
    "history_changed_after_tag": "Canonical release set changed after annotated-tag creation",
    "prior_tag_moved_after_tag": "Canonical tag moved after annotated-tag creation",
    "owned_tag_replaced_after_tag": "Owned annotated tag is missing or replaced",
    "reviewed_tag_moved_after_tag": "Reviewed tag moved immediately before release creation",
    "reviewed_tag_deleted_after_tag": "Reviewed tag was deleted immediately before release creation",
    "missing_ruleset": "Publication requires exactly one active tag ruleset",
    "disabled_ruleset": "Publication requires exactly one active tag ruleset",
    "extra_active_ruleset": "Publication requires exactly one active tag ruleset",
    "extra_bypass_actor": "Dedicated release App ruleset drifted",
    "missing_bypass_actor": "Dedicated release App ruleset drifted",
    "wrong_bypass_actor": "Dedicated release App ruleset drifted",
    "wrong_bypass_actor_type": "Dedicated release App ruleset drifted",
    "wrong_bypass_mode": "Dedicated release App ruleset drifted",
    "wrong_effective_bypass": "Dedicated release App ruleset drifted",
    "redacted_bypass_actors": "Dedicated release App ruleset drifted",
    "ruleset_exclusion": "Dedicated release App ruleset drifted",
    "malformed_release_app_id": "Configured release App Integration actor ID is malformed",
    "ruleset_disappears_after_tag": "Publication requires exactly one active tag ruleset",
    "ruleset_mutated_after_tag": "Dedicated release App ruleset drifted",
    "ambiguous_release_visible": "simulated ambiguous release creation",
    "post_upload_default_head_moved": "Repository default-branch head changed after source validation",
    "post_upload_owned_tag_replaced": "Release tag is absent or no longer points to the exact owned annotated tag object",
    "post_upload_reviewed_tag_replaced": "Reviewed tag moved after final asset upload",
    "post_upload_ruleset_mutated": "Dedicated release App ruleset drifted",
    "post_upload_release_deleted": "Unable to revalidate the original draft release after asset upload",
    "post_upload_release_recreated": "Unable to revalidate the original draft release after asset upload",
    "post_upload_release_draft_mutated": "GitHub returned an unexpected protected draft release identity or state",
    "post_upload_release_state_mutated": "GitHub returned an unexpected protected draft release identity or state",
    "post_upload_release_lookup_failure": "Unable to revalidate the original draft release after asset upload",
    "post_upload_release_lookup_forbidden": "Unable to revalidate the original draft release after asset upload",
    "post_upload_release_id_mismatch": "Original draft release was deleted or replaced after asset upload",
    "post_upload_asset_id_replaced": "Final release asset binding changed after upload",
    "post_upload_asset_size_mutated": "Final release asset binding changed after upload",
    "post_upload_asset_digest_mutated": "Final release asset binding changed after upload",
    "post_upload_asset_content_type_mutated": "Final release asset binding changed after upload",
    "post_upload_asset_state_mutated": "Final release asset binding changed after upload",
    "post_upload_asset_extra": "Final release asset set changed after upload",
    "post_upload_asset_missing": "Final release asset set changed after upload",
    "asset_finalization_digest_mismatch": "GitHub returned an unexpected finalized asset binding",
    "asset_finalization_never_completes": "Release asset did not finalize with an exact digest binding",
    "asset_finalization_lookup_failure": "Unable to verify uploaded release asset",
    "asset_delete_failure": "simulated asset upload failure",
    "asset_delete_ambiguous": "simulated asset upload failure",
    "asset_release_still_visible": "simulated asset upload failure",
    "asset_release_recreated": "simulated asset upload failure",
    "asset_recreated_same_target_tag": "simulated asset upload failure",
    "asset_cleanup_success_preserves_tag": "simulated asset upload failure",
}
for scenario, error_fragment in expected_publication_failures.items():
    result = publication_results[scenario]
    require(not result["ok"], f"adversarial publication scenario unexpectedly passed: {scenario}")
    require(error_fragment in (result["error"] or ""),
            f"{scenario} failed outside its sealed guard: {result['error']}")

post_create_visibility_failure = publication_results["post_create_tag_listing_never_converges"]
require(post_create_visibility_failure["calls"]["listMatchingRefs"] == 11,
        "non-convergent tag listing escaped its bounded ten-read limit")
require(post_create_visibility_failure["calls"]["releaseTagRef"] == 11,
        "non-convergent tag listing did not revalidate exact ownership on every read")
require(
    post_create_visibility_failure["retryDelays"]
    == [250, 500, 1000, 2000, 4000, 5000, 5000, 5000, 5000],
    "non-convergent tag listing retry schedule drifted",
)
require(post_create_visibility_failure["calls"]["createRelease"] == 0
        and post_create_visibility_failure["calls"]["uploadReleaseAsset"] == 0
        and not post_create_visibility_failure["releaseExists"],
        "non-convergent tag listing advanced into release creation")
require(post_create_visibility_failure["tagObjectSha"] == "d" * 40,
        "non-convergent tag listing did not preserve the exact owned tag")

post_create_replacement = publication_results[
    "post_create_tag_listing_missing_exact_ref_replaced"
]
require(post_create_replacement["calls"]["listMatchingRefs"] == 2
        and post_create_replacement["calls"]["releaseTagRef"] == 2,
        "stale listing did not fail immediately on exact-ref replacement")
require(not post_create_replacement["retryDelays"],
        "exact-ref replacement was retried as harmless listing lag")
require(post_create_replacement["calls"]["createRelease"] == 0
        and post_create_replacement["calls"]["uploadReleaseAsset"] == 0,
        "exact-ref replacement advanced into release creation")
require(post_create_replacement["tagObjectSha"] == "f" * 40,
        "exact-ref replacement fixture did not retain the hostile replacement")

asset_finalization_failures = {
    "asset_finalization_digest_mismatch": 1,
    "asset_finalization_never_completes": 7,
    "asset_finalization_lookup_failure": 7,
}
for scenario, expected_reads in asset_finalization_failures.items():
    result = publication_results[scenario]
    require(result["calls"]["uploadReleaseAsset"] == 1,
            f"{scenario} continued uploading after the first unverified asset")
    require(result["calls"]["getReleaseAsset"] == expected_reads,
            f"{scenario} did not enforce its bounded finalization reads")
    require(result["calls"]["deleteRelease"] == 1 and not result["releaseExists"],
            f"{scenario} did not confirm cleanup of its incomplete draft")
    require(result["tagObjectSha"] == "d" * 40,
            f"{scenario} removed or replaced the owned annotated tag")

for scenario, result in publication_results.items():
    require(result["calls"]["deleteRef"] == 0,
            f"{scenario} attempted automatic release-tag deletion")
    require(str(424242) not in json.dumps(result["logs"]),
            f"{scenario} logged the protected expected release App ID")
require("9007199254740992" not in json.dumps(
            publication_results["malformed_release_app_id"]["logs"]
        ), "malformed protected release App ID was logged")

manual_reconciliation_reasons = {
    "post_create_tag_listing_never_converges": "post-create-revalidation",
    "post_create_tag_listing_missing_exact_ref_replaced": "post-create-revalidation",
    "default_head_moved_after_tag": "post-create-revalidation",
    "history_changed_after_tag": "post-create-revalidation",
    "prior_tag_moved_after_tag": "post-create-revalidation",
    "owned_tag_replaced_after_tag": "post-create-revalidation",
    "reviewed_tag_moved_after_tag": "post-create-revalidation",
    "reviewed_tag_deleted_after_tag": "post-create-revalidation",
    "ruleset_disappears_after_tag": "post-create-revalidation",
    "ruleset_mutated_after_tag": "post-create-revalidation",
    "ambiguous_release_visible": "draft-release-creation",
    "post_upload_default_head_moved": "post-upload-revalidation",
    "post_upload_owned_tag_replaced": "post-upload-revalidation",
    "post_upload_reviewed_tag_replaced": "post-upload-revalidation",
    "post_upload_ruleset_mutated": "post-upload-revalidation",
    "post_upload_release_deleted": "post-upload-revalidation",
    "post_upload_release_recreated": "post-upload-revalidation",
    "post_upload_release_draft_mutated": "post-upload-revalidation",
    "post_upload_release_state_mutated": "post-upload-revalidation",
    "post_upload_release_lookup_failure": "post-upload-revalidation",
    "post_upload_release_lookup_forbidden": "post-upload-revalidation",
    "post_upload_release_id_mismatch": "post-upload-revalidation",
    "post_upload_asset_id_replaced": "post-upload-revalidation",
    "post_upload_asset_size_mutated": "post-upload-revalidation",
    "post_upload_asset_digest_mutated": "post-upload-revalidation",
    "post_upload_asset_content_type_mutated": "post-upload-revalidation",
    "post_upload_asset_state_mutated": "post-upload-revalidation",
    "post_upload_asset_extra": "post-upload-revalidation",
    "post_upload_asset_missing": "post-upload-revalidation",
    "asset_finalization_digest_mismatch": "release-asset-or-cleanup-failure",
    "asset_finalization_never_completes": "release-asset-or-cleanup-failure",
    "asset_finalization_lookup_failure": "release-asset-or-cleanup-failure",
    "asset_delete_failure": "release-asset-or-cleanup-failure",
    "asset_delete_ambiguous": "release-asset-or-cleanup-failure",
    "asset_release_still_visible": "release-asset-or-cleanup-failure",
    "asset_release_recreated": "release-asset-or-cleanup-failure",
    "asset_recreated_same_target_tag": "release-asset-or-cleanup-failure",
    "asset_cleanup_success_preserves_tag": "release-asset-or-cleanup-failure",
}
for scenario, reason in manual_reconciliation_reasons.items():
    diagnostics = [
        entry
        for entry in publication_results[scenario]["logs"]["error"]
        if entry.startswith("JUMPGATE_RELEASE_MANUAL_RECONCILIATION_REQUIRED ")
    ]
    require(len(diagnostics) == 1 and f"reason={reason}" in diagnostics[0],
            f"{scenario} lacks one stable manual-reconciliation diagnostic")
    for field in (
        "tag=v1.0.0",
        f"source={'c' * 40}",
        "run=123456/2",
        "automatic_tag_deletion=disabled",
        "cause=github_no_compare_and_delete_ref_api",
    ):
        require(field in diagnostics[0], f"{scenario} reconciliation diagnostic omits {field}")

post_upload_failures = {
    "post_upload_default_head_moved",
    "post_upload_owned_tag_replaced",
    "post_upload_reviewed_tag_replaced",
    "post_upload_ruleset_mutated",
    "post_upload_release_deleted",
    "post_upload_release_recreated",
    "post_upload_release_draft_mutated",
    "post_upload_release_state_mutated",
    "post_upload_release_lookup_failure",
    "post_upload_release_lookup_forbidden",
    "post_upload_release_id_mismatch",
    "post_upload_asset_id_replaced",
    "post_upload_asset_size_mutated",
    "post_upload_asset_digest_mutated",
    "post_upload_asset_content_type_mutated",
    "post_upload_asset_state_mutated",
    "post_upload_asset_extra",
    "post_upload_asset_missing",
}
for scenario in post_upload_failures:
    result = publication_results[scenario]
    require(result["calls"]["uploadReleaseAsset"] == 7,
            f"{scenario} did not interleave after every asset upload")
    require(result["calls"]["deleteRelease"] == 0,
            f"{scenario} attempted release cleanup after terminal-state ambiguity")
    require("release-url" not in result["outputs"],
            f"{scenario} exported success despite terminal-state ambiguity")
require(publication_results["post_upload_default_head_moved"]["calls"]["defaultHead"] == 3,
        "post-upload source-head fixture missed the terminal source check")
require(publication_results["post_upload_ruleset_mutated"]["calls"]["getRepoRuleset"] == 3,
        "post-upload ruleset fixture missed the terminal detail check")
require(publication_results["post_upload_owned_tag_replaced"]["calls"]["releaseTagRef"] == 3,
        "post-upload owned-tag fixture missed the terminal ownership check")
require(publication_results["post_upload_reviewed_tag_replaced"]["calls"]["reviewedRef"] == 3,
        "post-upload reviewed-ref fixture missed the terminal binding check")
terminal_release_read_counts = {
    "post_upload_release_deleted": 7,
    "post_upload_release_recreated": 7,
    "post_upload_release_draft_mutated": 1,
    "post_upload_release_state_mutated": 1,
    "post_upload_release_lookup_failure": 7,
    "post_upload_release_lookup_forbidden": 1,
    "post_upload_release_id_mismatch": 1,
    "post_upload_asset_id_replaced": 1,
    "post_upload_asset_size_mutated": 1,
    "post_upload_asset_digest_mutated": 1,
    "post_upload_asset_content_type_mutated": 1,
    "post_upload_asset_state_mutated": 1,
    "post_upload_asset_extra": 1,
    "post_upload_asset_missing": 1,
}
for scenario, expected_reads in terminal_release_read_counts.items():
    result = publication_results[scenario]
    require(result["calls"]["getReleaseByTag"] == 1,
            f"{scenario} repeated a tag-based release lookup after creation")
    require(result["calls"]["getRelease"] == expected_reads,
            f"{scenario} missed its bounded exact-ID terminal release snapshot")

full_release_retry_delays = [250, 500, 1000, 2000, 2000, 2000]
for scenario in (
    "post_upload_release_deleted",
    "post_upload_release_recreated",
    "post_upload_release_lookup_failure",
):
    require(publication_results[scenario]["retryDelays"] == full_release_retry_delays,
            f"{scenario} exact-ID retry schedule drifted")
for scenario in ("post_upload_release_lookup_forbidden", "post_upload_release_id_mismatch"):
    require(not publication_results[scenario]["retryDelays"],
            f"{scenario} retried a non-retryable terminal result")
require(publication_results["post_upload_owned_tag_replaced"]["tagObjectSha"] == "f" * 40,
        "post-upload replacement tag was not preserved")
for scenario in post_upload_failures - {
    "post_upload_owned_tag_replaced",
}:
    require(publication_results[scenario]["tagObjectSha"] == "d" * 40,
            f"{scenario} did not preserve the owned annotated tag")
require(not publication_results["post_upload_release_deleted"]["releaseExists"],
        "post-upload release-deletion fixture did not remove the release externally")
require(publication_results["post_upload_release_recreated"]["releaseId"] == 88,
        "post-upload release-recreation fixture did not replace the original ID")
for scenario in post_upload_failures - {
    "post_upload_release_deleted",
    "post_upload_release_recreated",
}:
    require(publication_results[scenario]["releaseExists"]
            and publication_results[scenario]["releaseId"] == 77,
            f"{scenario} did not preserve the original release")
require(publication_results["post_upload_release_draft_mutated"]["releaseDraft"] is False,
        "post-upload release-draft fixture did not mutate draft state")
require(publication_results["post_upload_release_state_mutated"]["releasePrerelease"] is True,
        "post-upload release-state fixture did not mutate prerelease state")
require("unexpected-release-asset.bin" in (
            publication_results["post_upload_asset_extra"]["releaseAssetNames"] or []
        ), "post-upload extra-asset fixture did not add its adversarial asset")
require(len(publication_results["post_upload_asset_missing"]["releaseAssetNames"] or []) == 6,
        "post-upload missing-asset fixture did not remove an asset")

pre_tag_ruleset_failures = {
    "missing_ruleset",
    "disabled_ruleset",
    "extra_active_ruleset",
    "extra_bypass_actor",
    "missing_bypass_actor",
    "wrong_bypass_actor",
    "wrong_bypass_actor_type",
    "wrong_bypass_mode",
    "wrong_effective_bypass",
    "redacted_bypass_actors",
    "ruleset_exclusion",
    "malformed_release_app_id",
}
for scenario in pre_tag_ruleset_failures:
    result = publication_results[scenario]
    require(result["calls"]["createTag"] == 0,
            f"{scenario} created a tag before proving the exact App ruleset")
    require(not any(
        entry.startswith("JUMPGATE_RELEASE_MANUAL_RECONCILIATION_REQUIRED ")
        for entry in result["logs"]["error"]
    ), f"{scenario} requested tag reconciliation before creating a tag")

require(publication_results["ambiguous_release_visible"]["releaseExists"],
        "ambiguous visible release fixture did not preserve release/tag consistency")
require(publication_results["asset_delete_failure"]["releaseExists"],
        "failed release deletion fixture unexpectedly removed the release")
require(not publication_results["asset_delete_ambiguous"]["releaseExists"],
        "ambiguous release deletion fixture did not model an absent release")
require(publication_results["asset_delete_ambiguous"]["tagObjectSha"] == "d" * 40,
        "ambiguous release deletion did not preserve the owned tag")
require(publication_results["asset_release_still_visible"]["releaseExists"],
        "visible-release cleanup fixture did not preserve the release")
require(publication_results["asset_release_recreated"]["releaseExists"],
        "release-recreation fixture did not preserve the recreated release")
require(publication_results["asset_recreated_same_target_tag"]["tagObjectSha"] == "f" * 40,
        "same-target recreated tag was not preserved")
require(publication_results["asset_cleanup_success_preserves_tag"]["tagObjectSha"] == "d" * 40,
        "confirmed draft-release deletion removed the owned tag")
require(not publication_results["asset_cleanup_success_preserves_tag"]["releaseExists"],
        "confirmed draft-release deletion fixture unexpectedly retained the release")
require(publication_results["ruleset_disappears_after_tag"]["calls"]["createTag"] == 1,
        "ruleset disappearance fixture did not interleave after tag creation")
require(publication_results["ruleset_mutated_after_tag"]["calls"]["getRepoRuleset"] == 2,
        "ruleset mutation fixture did not alter the post-tag detail revalidation")

for parity_token in (
    '"$arm64_package" == "$armv7_package"',
    '"$arm64_version_name" == "$armv7_version_name"',
    '"$arm64_version_code" == "$armv7_version_code"',
    '"$arm64_signer" == "$armv7_signer"',
    "expected_core_library",
    "refusing to overwrite",
    '[[ "$release_tag" == "v$jumpgate_semver" ]]',
    "documentDescribes",
    "SPDXRef-Package-Jumpgate-APK-",
    "primaryPackagePurpose",
    "root.get('licenseConcluded') != 'GPL-2.0-or-later'",
    "root.get('licenseDeclared') != 'GPL-2.0-or-later'",
    "'license': 'GPL-2.0-or-later'",
    "Expected Android version code is outside range 1..2100000000",
    "${#expected_version_code} -gt 10",
    "checksumValue': expected_hash",
    "inventory_ids",
    "root_contains",
):
    require(parity_token in validator_text, f"release validator policy is missing: {parity_token}")
require("libkodi.so" not in validator_text, "release validator hard-codes Kodi's old core library")
require("<expected-core-library>" in apk_verifier_text, "APK verifier core-library contract is missing")
require("allowed_path" in apk_verifier_text and "expected_core_library" in apk_verifier_text,
        "embedded-key exception is not scoped to the expected core library")
for pairing_asset in (
    "assets/addons/script.jumpgate.manager/resources/media/pixel.png",
    "assets/addons/script.jumpgate.manager/resources/skins/default/1080i/DialogJumpgatePairing.xml",
):
    require(pairing_asset in apk_verifier_text,
            f"APK verifier omits required pairing asset: {pairing_asset}")
for pairing_fixture in (
    "missing-pairing-asset",
    "malformed-pairing-asset",
    "malformed-pairing-png",
):
    require(pairing_fixture in apk_verifier_test_text,
            f"APK verifier tests omit the {pairing_fixture} fixture")
for adversarial_fixture in (
    "wrong-hash",
    "wrong-name",
    "wrong-license",
    "missing-license",
    "empty-packages",
    "root-only",
    "unrelated-inventory",
    "missing-describes",
    "wrong-document-root",
    "release-tag-binding",
    "version-code-boundary",
    "version-code-range",
):
    require(adversarial_fixture in validator_test_text,
            f"release verifier adversarial fixture is missing: {adversarial_fixture}")

print("Jumpgate Android release workflow policy tests: passed")

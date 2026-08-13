#!/usr/bin/env python3

"""Fail-closed contract and behavioral tests for release reconciliation."""

from __future__ import annotations

import json
import re
import subprocess
import tempfile
from pathlib import Path

import yaml


class UniqueKeyLoader(yaml.SafeLoader):
    pass


def construct_mapping(loader: UniqueKeyLoader, node: yaml.MappingNode, deep: bool = False):
    mapping = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in mapping:
            raise AssertionError(f"duplicate YAML key: {key}")
        mapping[key] = loader.construct_object(value_node, deep=deep)
    return mapping


UniqueKeyLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG,
    construct_mapping,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


root = Path(__file__).resolve().parents[3]
workflow_path = root / ".github" / "workflows" / "jumpgate-release-reconcile.yml"
workflow_text = workflow_path.read_text(encoding="utf-8")
workflow = yaml.load(workflow_text, Loader=UniqueKeyLoader)

require(isinstance(workflow, dict), "reconciliation workflow must be a YAML mapping")
require(workflow.get("name") == "Jumpgate Release Reconciliation", "workflow name drifted")
require(set(workflow.get("on", {})) == {"workflow_dispatch"}, "workflow must remain manual-only")
dispatch = workflow["on"]["workflow_dispatch"]
expected_inputs = {
    "release_tag",
    "release_id",
    "tag_object_sha",
    "source_sha",
    "reviewed_ref",
    "owner_run_id",
    "owner_run_attempt",
    "confirmation",
}
require(set(dispatch.get("inputs", {})) == expected_inputs, "reconciliation inputs drifted")
for name, descriptor in dispatch["inputs"].items():
    require(descriptor.get("required") is True, f"{name} must be required")
    require(descriptor.get("type") == "string", f"{name} must be a string")

require(workflow.get("permissions") == {"contents": "read"}, "top-level permissions must be read-only")
require(
    workflow.get("concurrency")
    == {"group": "jumpgate-android-release", "cancel-in-progress": False},
    "reconciliation must share the non-cancelling release lane",
)
require("\npush:" not in workflow_text, "workflow must not have a push trigger")
require("pull_request:" not in workflow_text, "workflow must not have a pull-request trigger")
require("schedule:" not in workflow_text, "workflow must not have a schedule trigger")

jobs = workflow.get("jobs")
require(set(jobs or {}) == {"reconcile_owned_draft"}, "workflow must have one protected job")
job = jobs["reconcile_owned_draft"]
condition = str(job.get("if", ""))
for invariant in (
    "github.event_name == 'workflow_dispatch'",
    "github.repository == 'ruizkinio/Jumpgate-kodi'",
    "github.ref == format('refs/heads/{0}', github.event.repository.default_branch)",
):
    require(invariant in condition, f"protected job guard is missing: {invariant}")
require(job.get("runs-on") == "ubuntu-24.04", "runner must be fixed")
require(job.get("timeout-minutes") == 15, "timeout must stay bounded")
require(job.get("environment") == {"name": "android-release"}, "environment gate drifted")
require(
    job.get("permissions") == {"actions": "read", "contents": "read"},
    "job token permissions must remain read-only",
)

steps = job.get("steps")
require(isinstance(steps, list) and len(steps) == 3, "workflow must contain exactly three steps")
owner_step, token_step, delete_step = steps
require(
    owner_step.get("uses")
    == "actions/github-script@ed597411d8f924073f98dfc5c65a23a2325f34cd",
    "owner-run preflight action is not commit-pinned",
)
require(owner_step.get("with", {}).get("github-token") == "${{ github.token }}", "owner-run proof must use the read-only job token")
require(
    token_step.get("uses")
    == "actions/create-github-app-token@bcd2ba49218906704ab6c1aa796996da409d3eb1",
    "release App token action is not commit-pinned",
)
require(
    delete_step.get("uses")
    == "actions/github-script@ed597411d8f924073f98dfc5c65a23a2325f34cd",
    "GitHub script action is not commit-pinned",
)
require(
    set(token_step.get("with", {}))
    == {
        "client-id",
        "private-key",
        "owner",
        "repositories",
        "permission-administration",
        "permission-contents",
        "permission-metadata",
    },
    "release App token scope drifted",
)
for permission in ("permission-administration", "permission-contents", "permission-metadata"):
    require(token_step["with"][permission] in {"read", "write"}, f"{permission} is malformed")
require(token_step["with"]["permission-administration"] == "write", "ruleset proof needs administration write")
require(token_step["with"]["permission-contents"] == "write", "exact cleanup needs contents write")

secret_pattern = re.compile(r"secrets\.([A-Z0-9_]+)")
secrets = set(secret_pattern.findall(workflow_text))
require(
    secrets
    == {
        "JUMPGATE_ANDROID_RELEASE_APP_CLIENT_ID",
        "JUMPGATE_ANDROID_RELEASE_APP_ID",
        "JUMPGATE_ANDROID_RELEASE_APP_PRIVATE_KEY",
    },
    "reconciliation secret contract drifted",
)

owner_script = str(owner_step.get("with", {}).get("script", ""))
require("Claimed owner run is not the successful canonical release workflow" in owner_script,
        "owner-run preflight does not bind the canonical successful release run")
require("core.setOutput('verified', 'true')" in owner_script,
        "owner-run preflight does not export its bounded result")
script = str(delete_step.get("with", {}).get("script", ""))
for required in (
    "Explicit reconciliation confirmation does not match the exact target",
    "Release-tag ruleset does not isolate mutation to the dedicated release App",
    "Claimed owner run was not verified by the read-only preflight",
    "Draft release does not match the exact workflow-owned identity",
    "Annotated tag does not match the exact workflow-owned identity",
    "github.rest.repos.deleteRelease",
    "github.rest.git.deleteRef",
    "waitForReleaseAbsent",
    "waitForTagAbsent",
):
    require(required in script, f"reconciliation script is missing: {required}")
for forbidden in (
    "repos.updateRelease",
    "git.updateRef",
    "git.createRef",
    "repos.createRelease",
    "git.createTag",
):
    require(forbidden not in script, f"reconciliation may not create or overwrite state: {forbidden}")
require(script.count("github.rest.repos.deleteRelease") == 1, "release deletion must have one exact call site")
require(script.count("github.rest.git.deleteRef") == 1, "tag deletion must have one exact call site")


def behavioral_harness(owner_github_script: str, delete_github_script: str) -> str:
    return f"""
const RELEASE_ID = 363467895;
const RUN_ID = 30688329845;
const RUN_ATTEMPT = 1;
const SOURCE = 'f0c235a26d267108d3c56e11b606fa4162adab4e';
const TAG_OBJECT = '0b01f78646c585e9f61e09643551634c35e66ec1';
const WORKFLOW = 'a8dd8f66eeca5efcf6f26cd350a7b2bebb068c00';
const TAG = 'v3.0.0';
const REPO = 'ruizkinio/Jumpgate-kodi';
const APP_ID = 4308220;
const AUTHOR = 'jumpgate-release-publisher[bot]';

function apiError(status, message) {{ const error = new Error(message); error.status = status; return error; }}

function assets() {{
  const version = '22.0-ALPHA2-Jumpgate-3.0.0';
  const names = [
    [`Jumpgate-${{version}}-arm64-v8a.apk`, 'application/vnd.android.package-archive'],
    [`Jumpgate-${{version}}-arm64-v8a.spdx.json`, 'application/octet-stream'],
    [`Jumpgate-${{version}}-armeabi-v7a.apk`, 'application/vnd.android.package-archive'],
    [`Jumpgate-${{version}}-armeabi-v7a.spdx.json`, 'application/octet-stream'],
    [`Jumpgate-${{version}}-metadata.json`, 'application/octet-stream'],
    [`Jumpgate-${{version}}-provenance.json`, 'application/octet-stream'],
    ['SHA256SUMS', 'application/octet-stream'],
  ];
  return names.map(([name, content_type], index) => ({{
    id: 100 + index, name, content_type, state: 'uploaded', size: 1000 + index,
    digest: 'sha256:' + String(index + 1).repeat(64),
  }}));
}}

function ownedRelease() {{
  return {{
    id: RELEASE_ID, tag_name: TAG, target_commitish: SOURCE,
    name: 'Jumpgate 22.0-ALPHA2-Jumpgate-3.0.0', draft: true,
    prerelease: false, immutable: false, published_at: null,
    author: {{login: AUTHOR}}, assets: assets(),
    body: [
      'Protected Android release candidate.', '', `Reviewed source: ${{SOURCE}}`,
      `Commit: ${{SOURCE}}`, `Annotated tag object: ${{TAG_OBJECT}}`,
      `Artifact attestation: https://github.com/${{REPO}}/attestations/38310402`, '',
      'This draft must complete device UAT before publication.',
    ].join('\\n'),
  }};
}}

function ownedTag() {{
  return {{
    sha: TAG_OBJECT, tag: TAG,
    message: [
      'Jumpgate Android release tag', '',
      `Ownership: jumpgate-android-release-owner-v1:${{REPO}}:${{RUN_ID}}:${{RUN_ATTEMPT}}:${{TAG}}:${{SOURCE}}`,
      `Reviewed source: ${{SOURCE}}`,
    ].join('\\n'),
    object: {{type: 'commit', sha: SOURCE}},
    tagger: {{name: AUTHOR, email: '305594533+jumpgate-release-publisher[bot]@users.noreply.github.com'}},
  }};
}}

async function runScenario(scenario) {{
  let release = scenario === 'tag_only' ? null : ownedRelease();
  let tag = scenario === 'release_only' ? null : ownedTag();
  if (scenario === 'wrong_release') release.author.login = 'attacker';
  if (scenario === 'wrong_tag') tag.object.sha = '0'.repeat(40);
  const calls = {{deleteRelease: 0, deleteRef: 0, notices: 0}};
  process.env = {{
    ...process.env,
    EXPECTED_RELEASE_APP_ID: String(APP_ID), EXPECTED_RELEASE_AUTHOR: AUTHOR,
    EXPECTED_RELEASE_WORKFLOW_ID: '321201768', OWNER_RUN_ATTEMPT: String(RUN_ATTEMPT),
    OWNER_RUN_ID: String(RUN_ID), RELEASE_ID: String(RELEASE_ID), RELEASE_TAG: TAG,
    REVIEWED_REF: SOURCE, SOURCE_SHA: SOURCE, TAG_OBJECT_SHA: TAG_OBJECT,
    WORKFLOW_SHA: WORKFLOW,
    RECONCILE_CONFIRMATION: scenario === 'wrong_confirmation'
      ? 'NO' : `RECONCILE ${{TAG}} ${{RELEASE_ID}} ${{TAG_OBJECT}}`,
  }};
  const context = {{
    repo: {{owner: 'ruizkinio', repo: 'Jumpgate-kodi'}}, eventName: 'workflow_dispatch',
    ref: 'refs/heads/master', payload: {{repository: {{default_branch: 'master'}}}},
  }};
  let ownerRunVerified = null;
  const core = {{
    notice() {{ calls.notices += 1; }},
    setOutput(name, value) {{ if (name === 'verified') ownerRunVerified = value; }},
  }};
  const repos = {{
    async get() {{ return {{data: {{full_name: REPO, default_branch: 'master'}}}}; }},
    async getRepoRulesets() {{ return {{data: [{{id: 19007104, enforcement: 'active', target: 'tag'}}]}}; }},
    async getRepoRuleset() {{
      const actor = scenario === 'wrong_ruleset' ? 999 : APP_ID;
      return {{data: {{
        enforcement: 'active', target: 'tag',
        conditions: {{ref_name: {{include: ['refs/tags/v*'], exclude: []}}}},
        rules: [{{type: 'creation'}}, {{type: 'update'}}, {{type: 'deletion'}}],
        bypass_actors: [{{actor_type: 'Integration', actor_id: actor, bypass_mode: 'always'}}],
      }}}};
    }},
    async getRelease() {{ if (!release) throw apiError(404, 'missing'); return {{data: structuredClone(release)}}; }},
    async listReleases() {{ return {{data: release ? [structuredClone(release)] : []}}; }},
    async deleteRelease({{release_id}}) {{
      if (release_id !== RELEASE_ID) throw new Error('wrong release delete target');
      calls.deleteRelease += 1; release = null; return {{status: 204}};
    }},
  }};
  const git = {{
    async getRef({{ref}}) {{
      if (ref === 'heads/master') return {{data: {{ref: 'refs/heads/master', object: {{type: 'commit', sha: WORKFLOW}}}}}};
      if (!tag) throw apiError(404, 'missing');
      return {{data: {{ref: `refs/tags/${{TAG}}`, object: {{type: 'tag', sha: TAG_OBJECT}}}}}};
    }},
    async getTag() {{ if (!tag) throw apiError(404, 'missing'); return {{data: structuredClone(tag)}}; }},
    async listMatchingRefs() {{
      return {{data: tag ? [{{ref: `refs/tags/${{TAG}}`, object: {{type: 'tag', sha: TAG_OBJECT}}}}] : []}};
    }},
    async deleteRef({{ref}}) {{
      if (ref !== `tags/${{TAG}}`) throw new Error('wrong tag delete target');
      calls.deleteRef += 1; tag = null; return {{status: 204}};
    }},
  }};
  const actions = {{
    async getWorkflowRun() {{
      return {{data: {{
        id: RUN_ID, run_attempt: RUN_ATTEMPT, name: `Release ${{TAG}} from ${{SOURCE}}`,
        workflow_id: 321201768, path: '.github/workflows/jumpgate-android-release.yml',
        event: 'workflow_dispatch', head_branch: 'master',
        head_sha: scenario === 'wrong_run' ? '0'.repeat(40) : SOURCE,
        head_repository: {{full_name: REPO}}, status: 'completed', conclusion: 'success',
      }}}};
    }},
  }};
  const github = {{
    rest: {{repos, git, actions}},
    async paginate(operation, args) {{ return (await operation(args)).data; }},
  }};
  let ok = true;
  let error = null;
  try {{
    await (async () => {{
{owner_github_script}
    }})();
    process.env.OWNER_RUN_VERIFIED = ownerRunVerified;
    await (async () => {{
{delete_github_script}
    }})();
  }} catch (caught) {{ ok = false; error = String(caught.message || caught); }}
  return {{scenario, ok, error, calls, releasePresent: release !== null, tagPresent: tag !== null}};
}}

const scenarios = [
  'stable', 'tag_only', 'release_only', 'wrong_confirmation',
  'wrong_release', 'wrong_tag', 'wrong_ruleset', 'wrong_run',
];
const results = [];
for (const scenario of scenarios) results.push(await runScenario(scenario));
console.log(JSON.stringify(results));
"""


with tempfile.TemporaryDirectory(prefix="jumpgate-reconcile-test-") as temporary:
    harness_path = Path(temporary) / "harness.mjs"
    harness_path.write_text(behavioral_harness(owner_script, script), encoding="utf-8")
    completed = subprocess.run(
        ["node", str(harness_path)],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )

results = {result["scenario"]: result for result in json.loads(completed.stdout)}
for scenario, expected_deletes in {
    "stable": (1, 1),
    "tag_only": (0, 1),
    "release_only": (1, 0),
}.items():
    result = results[scenario]
    require(result["ok"], f"{scenario} failed: {result['error']}")
    require(
        (result["calls"]["deleteRelease"], result["calls"]["deleteRef"]) == expected_deletes,
        f"{scenario} deleted the wrong state",
    )
    require(not result["releasePresent"] and not result["tagPresent"], f"{scenario} left state behind")
    require(result["calls"]["notices"] == 1, f"{scenario} did not emit one terminal notice")

for scenario in (
    "wrong_confirmation",
    "wrong_release",
    "wrong_tag",
    "wrong_ruleset",
    "wrong_run",
):
    result = results[scenario]
    require(not result["ok"], f"{scenario} unexpectedly succeeded")
    require(
        result["calls"]["deleteRelease"] == 0 and result["calls"]["deleteRef"] == 0,
        f"{scenario} mutated state before failing",
    )

print("Jumpgate release reconciliation workflow contract passed")

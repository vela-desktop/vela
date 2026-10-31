// SPDX-License-Identifier: GPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const yaml = readFileSync(new URL('../.github/workflows/branch-freshness.yml', import.meta.url), 'utf8');
const block = yaml.split('          script: |\n')[1];
assert.ok(block, 'workflow script exists');
const script = block.split('\n').map(line => line.startsWith('            ') ? line.slice(12) : line).join('\n');
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;

async function simulate(mode) {
  const statuses = [];
  const warnings = [];
  const failures = [];
  const head = 'h'.repeat(40), base = 'b'.repeat(40), merge = 'm'.repeat(40);
  let branchReads = 0, prReads = 0;
  const pr = { number: 143, state: 'open', base: { ref: 'main' }, head: { sha: head },
    mergeable: mode === 'conflict' ? false :
      ['unavailable', 'stale-unavailable'].includes(mode) ? null : true,
    merge_commit_sha: merge };
  const github = {
    paginate: async () => [pr],
    rest: {
      pulls: {
        list: () => {},
        get: async () => {
          prReads++;
          if (mode === 'head-moved' && prReads === 2) {
            return { data: { ...pr, head: { sha: 'n'.repeat(40) } } };
          }
          return { data: pr };
        }
      },
      repos: {
        getBranch: async () => {
          branchReads++;
          return { data: { commit: { sha: mode === 'base-moved' && branchReads === 2 ?
            'n'.repeat(40) : base } } };
        },
        compareCommits: async () => {
          if (mode === 'api-error') throw new Error('Injected compare error');
          const behind = ['stale', 'stale-unavailable', 'stale-merge', 'docs-only'].includes(mode);
          return { data: {
            ahead_by: behind ? 1 : 0,
            files: mode === 'docs-only' ? [{ filename: 'docs/overview.md' }] :
              behind ? [{ filename: 'src/main.cpp' }] : []
          } };
        },
        createCommitStatus: async status => { statuses.push(status); }
      },
      git: {
        getCommit: async () => {
          if (mode === 'merge-404' || mode === 'merge-422') {
            const error = new Error('Synthetic merge unavailable');
            error.status = mode === 'merge-404' ? 404 : 422;
            throw error;
          }
          return { data: { parents: [
            { sha: mode === 'stale-merge' ? 'o'.repeat(40) : base },
            { sha: head }
          ] } };
        }
      }
    }
  };
  const core = {
    warning: message => warnings.push(message),
    info: () => {},
    error: message => warnings.push(message),
    setFailed: message => failures.push(message)
  };
  const context = { repo: { owner: 'vela-desktop', repo: 'vela' },
    serverUrl: 'https://github.com', runId: 123 };
  await new AsyncFunction('github', 'core', 'context', script)(github, core, context);
  return { statuses, warnings, failures, head, merge };
}

for (const [mode, terminal, hasVerifiedMerge] of [
  ['success', 'success', true],
  ['stale', 'failure', true],
  ['docs-only', 'success', true],
  ['conflict', 'failure', false],
  ['unavailable', 'success', false],
  ['stale-unavailable', 'failure', false],
  ['stale-merge', 'failure', false],
  ['merge-404', 'success', false],
  ['merge-422', 'success', false],
  ['api-error', 'error', false],
  ['base-moved', 'error', false],
  ['head-moved', 'error', false]
]) {
  test('freshness verdict: ' + mode, async () => {
    const { statuses, warnings, failures, head, merge } = await simulate(mode);
    assert.deepEqual(statuses.map(status => status.state),
      ['pending', terminal, ...(hasVerifiedMerge ? [terminal] : [])]);
    assert.equal(statuses[0].sha, head);
    assert.equal(statuses[1].sha, head);
    if (hasVerifiedMerge) assert.equal(statuses[2].sha, merge);
    // A missing/stale synthetic merge may not receive a falsely successful verdict.
    if (!hasVerifiedMerge) assert.ok(statuses.every(status => status.sha !== merge));
    // An individual PR's transient API failure must not fail the push or PR workflow.
    assert.deepEqual(failures, []);
    if (['api-error', 'base-moved', 'head-moved'].includes(mode)) {
      assert.ok(warnings.length > 0, 'transient errors remain visible as annotations');
    }
  });
}

// SPDX-License-Identifier: GPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const yaml = readFileSync(new URL('../.github/workflows/branch-freshness.yml', import.meta.url), 'utf8');
const block = yaml.split('          script: |\n')[1];
assert.ok(block, 'workflow script exists');
const script = block.split('\n').map(line => line.startsWith('            ') ? line.slice(12) : line).join('\n')
  .replace('const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));',
           'const sleep = async () => {};');
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;

async function simulate(mode) {
  const statuses = [];
  const errors = [];
  const head = 'h'.repeat(40), base = 'b'.repeat(40), merge = 'm'.repeat(40);
  const pr = { number: 143, state: 'open', base: { ref: 'main' }, head: { sha: head },
    mergeable: mode === 'conflict' ? false : mode === 'unavailable' ? null : true,
    merge_commit_sha: merge };
  const github = {
    paginate: async () => [pr],
    rest: {
      pulls: { list: () => {}, get: async () => ({ data: pr }) },
      repos: {
        getBranch: async () => ({ data: { commit: { sha: base } } }),
        compareCommits: async () => ({ data: {
          ahead_by: mode === 'stale' ? 1 : 0,
          files: mode === 'stale' ? [{ filename: 'src/main.cpp' }] : []
        } }),
        createCommitStatus: async x => { statuses.push(x); }
      },
      git: { getCommit: async () => ({ data: { parents: [{ sha: base }, { sha: head }] } }) }
    }
  };
  const core = { warning: () => {}, info: () => {}, error: x => errors.push(x),
    setFailed: x => errors.push(x) };
  const context = { repo: { owner: 'vela-desktop', repo: 'vela' },
    serverUrl: 'https://github.com', runId: 123 };
  await new AsyncFunction('github', 'core', 'context', script)(github, core, context);
  return { statuses, errors, head, merge };
}

for (const [mode, terminal, shouldFail] of [
  ['success', 'success', false],
  ['stale', 'failure', false],
  ['conflict', 'failure', false],
  ['unavailable', 'error', true]
]) {
  test('freshness verdict: ' + mode, async () => {
    const { statuses, errors, head, merge } = await simulate(mode);
    assert.equal(statuses[0].sha, head);
    assert.equal(statuses[0].state, 'pending');
    assert.equal(statuses.at(-1).state, terminal);
    assert.equal(statuses.at(-1).sha, mode === 'conflict' || mode === 'unavailable' ? head : merge);
    assert.equal(errors.length > 0, shouldFail);
    if (mode === 'success' || mode === 'stale') {
      assert.equal(statuses.at(-2).sha, head);
      assert.equal(statuses.at(-2).state, terminal);
    }
  });
}

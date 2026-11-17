// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const yaml = readFileSync(new URL('../.github/workflows/gpu-gate.yml', import.meta.url), 'utf8');
const block = yaml.split('          script: |\n')[1];
assert.ok(block, 'workflow script exists');
const script = block.split('\n').map(line => line.startsWith('            ') ? line.slice(12) : line).join('\n');
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;

const HEAD = 'a'.repeat(40);
const OTHER = 'b'.repeat(40);
const full = (conclusion, sha = HEAD, status = 'completed') =>
  ({ head_sha: sha, display_title: 'Full GPU suite (X) on branch', status, conclusion });
const partial = conclusion =>
  ({ head_sha: HEAD, display_title: 'Partial GPU run (X) on branch', status: 'completed', conclusion });

// Runs the workflow script against fake GitHub data; returns the statuses
// it published and whether it failed the job.
async function run({ files = ['compositor/src/view.c'], amd = [], nvidia = [], labels = [],
  event = 'pull_request_target', runSha = HEAD } = {}) {
  const statuses = [];
  const failures = [];
  const pr = { number: 7, head: { sha: HEAD }, labels: labels.map(name => ({ name })) };
  const other = { number: 8, head: { sha: OTHER }, labels: [] };
  const github = {
    paginate: async (method, options) => method(options),
    rest: {
      pulls: {
        list: () => [pr, other],
        listFiles: () => files.map(filename => ({ filename })),
      },
      actions: {
        listWorkflowRuns: async ({ workflow_id, head_sha }) => {
          const all = workflow_id === 'gpu-amd.yml' ? amd : nvidia;
          return { data: { workflow_runs: all.filter(item => item.head_sha === head_sha) } };
        },
      },
      repos: { createCommitStatus: async status => { statuses.push(status); } },
    },
  };
  const context = {
    eventName: event,
    repo: { owner: 'vela-desktop', repo: 'vela' },
    serverUrl: 'https://github.com',
    runId: 1,
    payload: event === 'pull_request_target' ? { pull_request: pr }
      : event === 'workflow_run' ? { workflow_run: { head_sha: runSha } } : {},
  };
  const core = { info() {}, warning() {}, setFailed: message => failures.push(message) };
  await new AsyncFunction('github', 'context', 'core', script)(github, context, core);
  return { statuses, failures };
}

test('a GPU-affecting PR without runs is pending on both vendors', async () => {
  const { statuses } = await run();
  assert.equal(statuses.length, 1);
  assert.equal(statuses[0].context, 'gpu-ci/amd-nvidia');
  assert.equal(statuses[0].sha, HEAD);
  assert.equal(statuses[0].state, 'pending');
  assert.match(statuses[0].description, /AMD, NVIDIA$/);
});

test('full successful runs from both vendors on the exact SHA pass', async () => {
  const { statuses } = await run({ amd: [full('success')], nvidia: [full('failure'), full('success')] });
  assert.equal(statuses[0].state, 'success');
});

test('runs on another commit do not count', async () => {
  const { statuses } = await run({ amd: [full('success', OTHER)], nvidia: [full('success', OTHER)] });
  assert.equal(statuses[0].state, 'pending');
});

test('partial runs do not count', async () => {
  const { statuses } = await run({ amd: [partial('success')], nvidia: [full('success')] });
  assert.equal(statuses[0].state, 'pending');
  assert.match(statuses[0].description, /: AMD$/);
});

test('a run still going keeps the status pending', async () => {
  const { statuses } = await run({ amd: [full(null, HEAD, 'in_progress')], nvidia: [full('success')] });
  assert.equal(statuses[0].state, 'pending');
  assert.match(statuses[0].description, /AMD running/);
});

test('failed runs with nothing else to wait for fail the status', async () => {
  const { statuses } = await run({ amd: [full('failure')], nvidia: [full('success')] });
  assert.equal(statuses[0].state, 'failure');
  assert.match(statuses[0].description, /AMD failed/);
});

test('documentation and translations are exempt', async () => {
  const { statuses } = await run({ files: ['docs/roadmap.md', 'README.md', 'shell/i18n/vela-shell_it.ts'] });
  assert.equal(statuses[0].state, 'success');
});

test('one GPU-affecting file among docs still needs the runs', async () => {
  const { statuses } = await run({ files: ['docs/roadmap.md', '.github/workflows/build.yml'] });
  assert.equal(statuses[0].state, 'pending');
});

test('the maintainer label exempts a PR', async () => {
  const { statuses } = await run({ labels: ['gpu: not affected'] });
  assert.equal(statuses[0].state, 'success');
});

test('a finished GPU run re-evaluates only the PRs at its SHA', async () => {
  const { statuses } = await run({ event: 'workflow_run', amd: [full('success')], nvidia: [full('success')] });
  assert.deepEqual(statuses.map(status => status.sha), [HEAD]);
  assert.equal(statuses[0].state, 'success');
});

test('a manual dispatch re-evaluates every open PR', async () => {
  const { statuses } = await run({ event: 'workflow_dispatch' });
  assert.deepEqual(statuses.map(status => status.sha).sort(), [HEAD, OTHER]);
});

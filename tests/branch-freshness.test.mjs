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
  const head = 'h'.repeat(40), head2 = 'j'.repeat(40);
  const base = 'b'.repeat(40), merge = 'm'.repeat(40), merge2 = 'z'.repeat(40);
  const prs = [{
    number: 143, state: 'open', base: { ref: 'main' }, head: { sha: head },
    mergeable: ['conflict', 'unchanged-conflict', 'stale-to-conflict'].includes(mode) ? false :
      ['unavailable', 'stale-unavailable', 'unknown-recovered',
        'unknown-conflict', 'unknown-head-moved'].includes(mode) ? null : true,
    merge_commit_sha: merge
  }];
  if (mode === 'multi-api-error') {
    prs.push({
      number: 144, state: 'open', base: { ref: 'main' }, head: { sha: head2 },
      mergeable: true, merge_commit_sha: merge2
    });
  }
  if (mode === 'missing-head') prs[0].head = undefined;
  // What earlier runs left on each commit, newest first. By default HEAD
  // carries the success of the previous run, which this run must invalidate.
  const prior = new Map([[head, [{ state: 'success', description: 'Branch includes current main' }]]]);
  const staleVerdict = { state: 'failure', description: 'Update branch: main changed relevant files' };
  const conflictVerdict = { state: 'failure', description: 'Merge conflict with main; resolve conflicts' };
  if (['unchanged-stale', 'stale-to-success', 'stale-to-conflict', 'limit-stale'].includes(mode)) prior.set(head, [staleVerdict]);
  if (['unchanged-conflict', 'limit-conflict-api-error'].includes(mode)) prior.set(head, [conflictVerdict]);
  if (mode === 'leftover-pending') prior.set(head, [{ state: 'pending', description: 'Evaluating current base' }]);
  if (mode === 'no-prior') prior.delete(head);
  if (mode === 'unchanged-merge') prior.set(merge, [{ state: 'success', description: 'Branch includes current main' }]);
  if (mode === 'other-contexts') {
    prior.set(head, [{ state: 'success', description: 'Quality gate', context: 'sonar' }, staleVerdict]);
  }
  let branchReads = 0;
  const prReads = new Map();

  const github = {
    paginate: async () => prs,
    rest: {
      pulls: {
        list: () => {},
        get: async ({ pull_number }) => {
          const count = (prReads.get(pull_number) || 0) + 1;
          prReads.set(pull_number, count);
          const pr = prs.find(item => item.number === pull_number);
          if (mode === 'missing-head') throw new Error('Injected PR API error');
          if (pull_number === 143 && mode === 'head-moved' && count === 2) {
            return { data: { ...pr, head: { sha: 'n'.repeat(40) } } };
          }
          if (pull_number === 143 && mode === 'unknown-head-moved' && count === 3) {
            return { data: { ...pr, head: { sha: 'n'.repeat(40) } } };
          }
          if (pull_number === 143 && count === 3 && mode === 'unknown-recovered') {
            return { data: { ...pr, mergeable: true } };
          }
          if (pull_number === 143 && count === 3 && mode === 'unknown-conflict') {
            return { data: { ...pr, mergeable: false } };
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
        compareCommits: async ({ base: comparisonHead }) => {
          if (['api-error', 'limit-conflict-api-error', 'leftover-pending'].includes(mode) ||
              (mode === 'multi-api-error' && comparisonHead === head)) {
            throw new Error('Injected compare error');
          }
          const behind = ['stale', 'stale-unavailable', 'stale-merge', 'docs-only',
            'unchanged-stale', 'limit-stale', 'other-contexts'].includes(mode);
          return { data: {
            ahead_by: behind ? 1 : 0,
            files: mode === 'docs-only' ? [{ filename: 'docs/overview.md' }] :
              behind ? [{ filename: 'src/main.cpp' }] : []
          } };
        },
        listCommitStatusesForRef: async ({ ref }) => {
          if (mode === 'status-read-fails' && ref === head) throw new Error('Injected status read error');
          const published = statuses.filter(status => status.sha === ref).reverse();
          const earlier = (prior.get(ref) || []).map(status =>
            ({ context: 'branch-freshness/relevant-base', ...status }));
          return { data: [...published, ...earlier] };
        },
        createCommitStatus: async status => {
          if (mode.startsWith('limit-') && status.sha === head) {
            const error = new Error('This SHA and context has reached the maximum number of statuses.');
            error.status = 422;
            throw error;
          }
          if ((mode === 'status-pending-fails' ||
               mode === 'status-pending-and-error-fail') && status.state === 'pending') {
            throw new Error('Injected pending publication error');
          }
          if (mode === 'status-pending-and-error-fail' && status.state === 'error') {
            throw new Error('Injected error publication failure');
          }
          if ((mode === 'status-merge-publish-fails' ||
               mode === 'status-merge-and-error-fail') &&
              status.sha === merge && status.state === 'success') {
            throw new Error('Injected merge publication failure');
          }
          if (mode === 'status-merge-and-error-fail' &&
              status.sha === merge && status.state === 'error') {
            throw new Error('Injected merge error publication failure');
          }
          if (mode === 'status-head-final-and-error-fail' &&
              status.sha === head && status.state !== 'pending') {
            throw new Error('Injected HEAD terminal publication failure');
          }
          statuses.push(status);
        }
      },
      git: {
        getCommit: async ({ commit_sha }) => {
          if (['merge-404', 'merge-422', 'merge-500'].includes(mode)) {
            const error = new Error('Injected getCommit failure');
            error.status = mode === 'merge-404' ? 404 : mode === 'merge-422' ? 422 : 500;
            throw error;
          }
          return { data: { parents: [
            { sha: mode === 'stale-merge' ? 'o'.repeat(40) : base },
            { sha: commit_sha === merge2 ? head2 : head }
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
  const names = new Map([[head, 'head'], [head2, 'head2'], [merge, 'merge'], [merge2, 'merge2']]);
  return { statuses, warnings, failures, prReads,
    events: statuses.map(status => names.get(status.sha) + ':' + status.state) };
}

const scenarios = [
  ['success', ['head:pending', 'head:success', 'merge:success']],
  ['stale', ['head:pending', 'head:failure', 'merge:failure']],
  ['docs-only', ['head:pending', 'head:success', 'merge:success']],
  ['conflict', ['head:pending', 'head:failure']],
  ['unavailable', ['head:pending', 'head:error']],
  ['stale-unavailable', ['head:pending', 'head:failure']],
  ['unknown-recovered', ['head:pending', 'head:success', 'merge:success']],
  ['unknown-conflict', ['head:pending', 'head:failure']],
  ['unknown-head-moved', ['head:pending', 'head:error']],
  ['stale-merge', ['head:pending', 'head:failure']],
  ['merge-404', ['head:pending', 'head:success']],
  ['merge-422', ['head:pending', 'head:error', 'merge:error']],
  ['merge-500', ['head:pending', 'head:error', 'merge:error']],
  ['api-error', ['head:pending', 'head:error']],
  ['base-moved', ['head:pending', 'head:error']],
  ['head-moved', ['head:pending', 'head:error']],
  ['status-pending-fails', ['head:error']],
  ['status-pending-and-error-fail', [] , true],
  ['status-merge-publish-fails',
    ['head:pending', 'head:success', 'head:error', 'merge:error']],
  ['status-merge-and-error-fail',
    ['head:pending', 'head:success', 'head:error'], true],
  ['status-head-final-and-error-fail', ['head:pending', 'merge:error'], true],
  ['missing-head', [], true],
  ['multi-api-error',
    ['head:pending', 'head:error', 'head2:success', 'merge2:success']],
  // Only changes are published: GitHub caps statuses per commit and context.
  // The synthetic merge is a new commit for each base, so it gets its verdict.
  ['unchanged-stale', ['merge:failure']],
  ['unchanged-conflict', []],
  ['other-contexts', ['merge:failure']],
  ['unchanged-merge', ['head:pending', 'head:success']],
  ['stale-to-success', ['head:success', 'merge:success']],
  ['stale-to-conflict', ['head:failure']],
  ['no-prior', ['head:success', 'merge:success']],
  ['leftover-pending', ['head:error']],
  ['status-read-fails', ['head:error']],
  // A commit at the cap that already fails keeps blocking the merge.
  ['limit-stale', ['merge:failure']],
  ['limit-conflict-api-error', []],
  ['limit-success', [], true]
];

for (const [mode, expectedEvents, shouldFail = false] of scenarios) {
  test('freshness verdict: ' + mode, async () => {
    const { events, statuses, warnings, failures, prReads } = await simulate(mode);
    assert.deepEqual(events, expectedEvents);
    assert.equal(failures.length > 0, shouldFail,
      'only failures to publish fail-closed statuses may fail the workflow');
    if (['unavailable', 'stale-unavailable', 'unknown-recovered',
      'unknown-conflict', 'unknown-head-moved'].includes(mode)) {
      assert.equal(prReads.get(143), 3, 'unknown mergeability gets one extra read');
    }
    if (mode === 'multi-api-error') {
      assert.ok(statuses.some(status => status.sha === 'j'.repeat(40) &&
        status.state === 'success'), 'a failed PR does not prevent checking the next one');
    }
    if (mode === 'stale-to-conflict') {
      assert.match(statuses[0].description, /Merge conflict/, 'a new reason replaces the old one');
    }
    if (mode === 'stale') {
      assert.ok(statuses.every(status => !/[0-9]/.test(status.description)),
        'a stale verdict stays the same while main moves on');
    }
    if (['merge-422', 'merge-500', 'api-error', 'base-moved',
      'head-moved', 'status-pending-fails', 'status-merge-publish-fails'].includes(mode)) {
      assert.ok(warnings.length > 0, 'API failures remain visible in annotations');
    }
    if (['unavailable', 'stale-merge', 'merge-404'].includes(mode)) {
      assert.ok(statuses.every(status => status.sha !== 'm'.repeat(40)),
        'an unverified synthetic merge must not receive success');
    }
  });
}

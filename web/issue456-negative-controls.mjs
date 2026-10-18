// Run with: node web/issue456-negative-controls.mjs
// Mutants live in a temporary copy. Never edits the working tree or snapshots.
import assert from 'node:assert/strict';
import { cpSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const root = fileURLToPath(new URL('../', import.meta.url));
const scratch = mkdtempSync(join(tmpdir(), 'pgwt-issue456-controls-'));
for (const path of ['web/static/lib', 'web/static/views', 'web/static/dev/fixtures',
    'tests/web_unit/transitions.test.mjs']) {
    const destination = join(scratch, path);
    mkdirSync(dirname(destination), { recursive: true });
    cpSync(join(root, path), destination, { recursive: true });
}

function run(label, pattern, expectedStatus, evidence) {
    const args = ['--test'];
    if (pattern) args.push('--test-name-pattern=' + pattern);
    args.push('tests/web_unit/transitions.test.mjs');
    const result = spawnSync(process.execPath, args, { cwd: scratch, encoding: 'utf8' });
    if (result.error) throw result.error;
    const output = result.stdout + result.stderr;
    writeFileSync(join(scratch, label + '.txt'), output);
    assert.equal(result.status, expectedStatus, label + '\n' + output);
    if (evidence) assert.match(output, evidence, label + ' failed for the wrong reason');
    console.log(label + ': exit ' + result.status + ' (expected ' + expectedStatus + ')');
}

run('green-before', null, 0);

const builderPath = join(scratch, 'web/static/lib/builders/transitions.js');
const builder = readFileSync(builderPath, 'utf8');
try {
    // NC1: restore permissive catalog admission and the old [] fallback. The
    // view must now silently render, so the visible-error assertion MUST fail.
    assert.ok(builder.includes('export function transitionsDataError(data) {'));
    assert.ok(builder.includes('const { links, nodes } = data;'));
    writeFileSync(builderPath, builder
        .replace('export function transitionsDataError(data) {',
            'export function transitionsDataError(data) { return null; // NC1')
        .replace('const { links, nodes } = data;', 'const { links, nodes = [] } = data;'));
    run('negative1-silent-nodes-less', '#456 nodes-less response', 1, /AssertionError.*[\s\S]*role="alert"/);
} finally {
    writeFileSync(builderPath, builder);
}

const fixturePath = join(scratch, 'web/static/dev/fixtures/issue456-transitions.json');
const fixture = readFileSync(fixturePath, 'utf8');
try {
    // NC2 changes ONLY the node catalog: total, links, idle and simplify counts
    // remain byte-for-byte unchanged. Run the SAME golden disclosure test.
    const payload = JSON.parse(fixture);
    const links = JSON.stringify(payload.links);
    payload.nodes = payload.nodes.filter(n => n.name !== 'Lock:relation');
    assert.equal(payload.nodes.length, 40);
    assert.equal(payload.total, 3466745);
    assert.equal(JSON.stringify(payload.links), links);
    writeFileSync(fixturePath, JSON.stringify(payload));
    run('negative2-missing-lock', '#456 frozen replay', 1, /Transitions data incomplete: missing node: Lock:relation/);

    // Non-vacuity control: ten empty responses agree, but must FAIL before any
    // determinism comparison. This is replay coverage, not ten new servers.
    writeFileSync(fixturePath, JSON.stringify({ nodes: [], links: [], total: 0 }));
    run('negative3-empty-determinism', '#456 ten fresh', 1, /payload.nodes.length > 0/);
} finally {
    writeFileSync(fixturePath, fixture);
}

run('green-restored', null, 0);
console.log('Logs: ' + scratch);

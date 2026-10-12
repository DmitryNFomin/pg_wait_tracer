#!/usr/bin/env node
// record-web.mjs — Headed Playwright recording of the pgwt web UI.
//
// Drives the demo flow: drag-zoom the AAS chart into the workload burst →
// Events → Sessions → Queries → Transitions (with simplify slider at 0%).
// Records video; ffmpeg post-processes to gif.
//
// Prerequisites:
//   npm install  (in demos/ — installs Playwright)
//   ffmpeg on PATH
//   pgwt server already serving on http://localhost:8384/  (see record-web.sh)
//
// Output: demos/web.gif

import { chromium } from 'playwright';
import { spawnSync } from 'node:child_process';
import { mkdirSync, rmSync, readdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const URL = process.env.PGWT_URL || 'http://localhost:8384/';
const VIDEO_DIR = join(HERE, '.video');
const OUTPUT = join(HERE, 'web.gif');

rmSync(VIDEO_DIR, { recursive: true, force: true });
mkdirSync(VIDEO_DIR, { recursive: true });

const browser = await chromium.launch({ headless: false });
const context = await browser.newContext({
    viewport: { width: 1500, height: 810 },
    deviceScaleFactor: 1,
    recordVideo: { dir: VIDEO_DIR, size: { width: 1500, height: 810 } },
});
const page = await context.newPage();
page.setDefaultTimeout(30000);
const errors = [];
page.on('pageerror', error => errors.push(error.message));
const evidence = [];
async function mountSeq() {
    return page.evaluate(() => window.__pgwt?.viewMount()?.seq || 0);
}
async function waitMount(tab, after = 0) {
    await page.waitForFunction(({ tab, after }) => {
        const mount = window.__pgwt?.viewMount();
        return mount?.id === tab && mount.seq > after;
    }, { tab, after });
    await page.locator('#status.connected').waitFor();
    if (await page.locator('.pane-error, #version-skew:visible, #degraded-overlay:visible').count()) {
        throw new Error('UI reports a pane error, version skew, or degraded connection');
    }
}
async function snapshot(name) {
    await page.screenshot({ path: join(VIDEO_DIR, `${name}.png`) });
    const state = await page.evaluate(() => ({
        tab: window.__pgwt.activeTab,
        from: window.__pgwt.timeRange.from,
        to: window.__pgwt.timeRange.to,
        rows: document.querySelectorAll('#table-container tbody tr').length,
        status: document.querySelector('#status').textContent,
        slider: document.querySelector('#dfg-slider')?.value,
    }));
    evidence.push({ name, ...state });
    console.log(`==> Verified ${name}: ${JSON.stringify(state)}`);
}
try {
    console.log(`==> Loading ${URL}`);
    await page.goto(URL);
    await waitMount('overview');

    // Fit the actual offline capture before brushing. A fixed middle-of-screen
    // drag on the default 15-minute range used to select an empty window.
    let seq = await mountSeq();
    await page.locator('#time-range').click();
    await page.locator('#time-picker button[data-range="0"]').click();
    await waitMount('overview', seq);
    await page.locator('#aas-chart-container canvas').first().waitFor();
    await page.waitForTimeout(1500);
    await snapshot('overview');

    console.log('==> Drag-zoom into the workload burst');
    const before = await page.evaluate(() => window.__pgwt.timeRange.to - window.__pgwt.timeRange.from);
    seq = await mountSeq();
    const plot = page.locator('#aas-chart-container .u-over');
    const box = await (await plot.count() ? plot : page.locator('#aas-chart-container')).boundingBox();
    if (!box) throw new Error('AAS plot has no visible bounds');
    const y = box.y + box.height * 0.5;
    await page.mouse.move(box.x + box.width * 0.25, y);
    await page.mouse.down();
    await page.mouse.move(box.x + box.width * 0.75, y, { steps: 12 });
    await page.mouse.up();
    await waitMount('overview', seq);
    const after = await page.evaluate(() => window.__pgwt.timeRange.to - window.__pgwt.timeRange.from);
    if (!(after > 0 && after < before * 0.8)) throw new Error('Drag did not zoom the trace window');
    await page.waitForTimeout(1500);
    await snapshot('zoom');

    for (const tab of ['events', 'sessions', 'queries', 'transitions']) {
        console.log(`==> Click tab: ${tab}`);
        seq = await mountSeq();
        await page.locator(`#tabs button[data-tab="${tab}"]`).click();
        await waitMount(tab, seq);
        if (tab !== 'transitions') {
            await page.locator('#table-container tbody tr').first().waitFor();
        } else {
            await page.locator('#dfg-container canvas').waitFor();
            const slider = page.locator('#dfg-slider');
            await slider.focus();
            await slider.press('Home');
            await page.waitForFunction(() => document.querySelector('#dfg-slider')?.value === '0');
        }
        await page.waitForTimeout(1500);
        await snapshot(tab);
    }
    // Keep the full graph visible as the final shot.
    await page.mouse.wheel(0, 200);
    await page.waitForTimeout(1500);
    await snapshot('transitions-full');
    if (errors.length) throw new Error(`Browser errors: ${errors.join('; ')}`);
    writeFileSync(join(VIDEO_DIR, 'evidence.json'), JSON.stringify(evidence, null, 2));
} catch (error) {
    await page.screenshot({ path: join(VIDEO_DIR, 'failure.png') }).catch(() => {});
    throw error;
} finally {
    console.log('==> Closing browser ...');
    await context.close();
    await browser.close();
}

// Find the video file (Playwright names it with a hash)
const files = readdirSync(VIDEO_DIR).filter(f => f.endsWith('.webm'));
if (!files.length) {
    console.error('FATAL: no video file produced');
    process.exit(1);
}
const video = join(VIDEO_DIR, files[0]);

console.log(`==> Converting ${video} -> ${OUTPUT}`);
// Two-pass palette for compact, high-quality gif
const PALETTE = join(VIDEO_DIR, 'palette.png');
const fps = process.env.GIF_FPS || '8';
// Lossy WebM noise changes otherwise static pixels every frame and bloats
// GIF deltas. Denoise it before quantization, at a README-friendly width.
const filter = `fps=${fps},scale=1200:-1:flags=lanczos,hqdn3d=4:4:6:6`;
const r1 = spawnSync('ffmpeg', ['-y', '-i', video, '-vf',
    `${filter},palettegen=stats_mode=full`,
    '-frames:v', '1', '-update', '1', PALETTE], { stdio: 'inherit' });
if (r1.status !== 0) process.exit(r1.status || 1);

const r2 = spawnSync('ffmpeg', ['-y', '-i', video, '-i', PALETTE, '-lavfi',
    `${filter} [x]; [x][1:v] paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle`,
    OUTPUT], { stdio: 'inherit' });
if (r2.status !== 0) process.exit(r2.status || 1);

console.log(`==> ${OUTPUT} ready`);

// Renders every view of the screen in Chromium against the simulator and
// fails on any console error. With SCREENSHOT_DIR set, saves a 1920x1080
// screenshot of each view there. Skips when Chromium is not installed
// (npx playwright install chromium).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import { startConsole } from '../src/app.js';
import { ROUNDS, waitFor, sleep } from './helpers.js';

async function launch() {
  try {
    const { chromium } = await import('playwright');
    return await chromium.launch();
  } catch (err) {
    return { error: err.message.split('\n')[0] };
  }
}

test('the charging overview renders and its keys reach the console', { timeout: 120000 }, async (t) => {
  const browser = await launch();
  if (browser.error) {
    t.skip(`Chromium nicht verfügbar: ${browser.error}`);
    return;
  }
  const dir = process.env.SCREENSHOT_DIR;
  if (dir) await fs.mkdir(dir, { recursive: true });
  // A download of 20-25 s, so the screenshot catches the update running.
  const app = await startConsole({ simulate: 50, httpPort: 0, inbox: null, simTiming: { fleetDownloadMin: 20000, fleetDownloadMax: 25000 } });
  const game = app.game;
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  const errors = [];
  page.on('console', (m) => {
    if (m.type() === 'error') errors.push(m.text());
  });
  page.on('pageerror', (e) => errors.push(String(e)));
  try {
    await page.goto(app.url);
    await waitFor(() => app.link.connected, { what: 'link' });
    await page.keyboard.press('f');
    await waitFor(() => game.snapshot().view === 'fleet', { what: 'F opens the overview' });
    await waitFor(() => game.snapshot().fleet.devices.length === 50, { timeoutMs: 8000, what: '50 devices' });
    await page.waitForFunction('document.querySelectorAll(".fleet-grid .ftile").length === 50');
    assert.equal(await page.locator('.ftile.error').count(), 1);
    assert.equal(await page.locator('.ftile.outdated').count(), 1);

    // U twice sends the update; the outdated device shows its progress.
    await page.keyboard.press('u');
    assert.equal(game.messages.some((m) => /Update an alle/.test(m.text)), false);
    await page.keyboard.press('u');
    await waitFor(() => game.snapshot().fleet.devices.some((d) => d.upd === 'downloading' && d.pct >= 15), { timeoutMs: 15000, what: 'update progress' });
    await page.waitForSelector('.ftile.updating .ft-upd');
    await sleep(700);
    if (dir) await page.screenshot({ path: path.join(dir, 'screen-ladeuebersicht.png') });
    assert.match(await page.textContent('#fcounts'), /laden.*voll.*veraltet.*Update läuft.*Fehler/);

    // E and L set the charge target, F leads back to the game.
    await page.keyboard.press('e');
    await waitFor(() => game.snapshot().fleet.devices.filter((d) => d.target === 'event').length >= 45, { timeoutMs: 40000, what: 'target event' });
    await page.keyboard.press('f');
    await waitFor(() => game.snapshot().view === 'start', { what: 'F closes the overview' });
    await page.waitForSelector('.hero h1');
    assert.deepEqual(errors, []);
  } finally {
    await browser.close();
    await app.close();
  }
});

test('every view renders without console errors', { timeout: 180000 }, async (t) => {
  const browser = await launch();
  if (browser.error) {
    t.skip(`Chromium nicht verfügbar: ${browser.error}`);
    return;
  }
  const dir = process.env.SCREENSHOT_DIR;
  if (dir) await fs.mkdir(dir, { recursive: true });
  const app = await startConsole({ simulate: 50, httpPort: 0, inbox: null, simTiming: { endRelease: 1000 } });
  const game = app.game;
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  const errors = [];
  page.on('console', (m) => {
    if (m.type() === 'error') errors.push(m.text());
  });
  page.on('pageerror', (e) => errors.push(String(e)));

  const shot = async (name, selector) => {
    await page.waitForSelector(selector, { timeout: 5000 });
    await sleep(700); // let entrance animations finish
    if (dir) await page.screenshot({ path: path.join(dir, `screen-${name}.png`) });
  };
  const act = async (action, args) => {
    const r = await game.act(action, args);
    assert.equal(r.ok, true, `${action}: ${r.error}`);
  };

  try {
    await page.goto(app.url);
    await waitFor(() => app.link.connected, { what: 'link' });
    game.load({ kind: 'session', value: { title: 'Workshop am Nachmittag', rounds: [ROUNDS.scale, ROUNDS.race, ROUNDS.choice] } });
    await shot('start', '.hero h1');

    await act('next');
    await waitFor(() => game.snapshot().counts.admitted >= 30, { timeoutMs: 20000, what: 'joins' });
    await shot('login', '.login .tiles .tile');
    await waitFor(() => game.snapshot().counts.admitted === 50, { timeoutMs: 20000, what: '50 admits' });

    await act('next');
    await act('next');
    await waitFor(() => game.snapshot().round.results.voters >= 40, { timeoutMs: 20000, what: 'votes' });
    await sleep(1500);
    await shot('skala', '.columns .led.on');
    await page.keyboard.press('t');
    await shot('skala-hell', ':root[data-theme="light"] .columns');
    await page.keyboard.press('t');

    await act('next');
    await act('next');
    await act('next');
    await shot('rennen-countdown', '.countdown');
    await waitFor(() => game.snapshot().round.winner, { timeoutMs: 6000, what: 'winner' });
    await sleep(800);
    await shot('rennen', '.winner');

    await act('next');
    await act('next');
    await act('next');
    await waitFor(() => game.snapshot().round.results.answered >= 30, { timeoutMs: 20000, what: 'answers' });
    await shot('auswahl', '.answered');
    await waitFor(() => game.snapshot().round.results.answered >= 40, { timeoutMs: 20000, what: 'answers' });
    await act('next');
    await act('next');
    await shot('auswahl-aufloesung', '.answer.right');

    await act('idle');
    await shot('ruhe', '.tiles.small .tile');

    // The presenter strip toggles with S; the screen asks the console for "next" on the arrow key.
    await page.keyboard.press('s');
    assert.equal(await page.isHidden('#strip'), true);
    await page.keyboard.press('s');
    assert.equal(await page.isVisible('#strip'), true);
    await act('end');
    await waitFor(() => game.snapshot().view === 'start', { timeoutMs: 5000, what: 'end' });
    await page.keyboard.press('ArrowRight');
    await waitFor(() => game.snapshot().view === 'login', { timeoutMs: 5000, what: 'next from the keyboard' });
    await page.waitForSelector('.login');

    assert.deepEqual(errors, []);
  } finally {
    await browser.close();
    await app.close();
  }
});

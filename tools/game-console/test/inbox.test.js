import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { Inbox } from '../src/inbox.js';
import { ROUNDS } from './helpers.js';

async function makeInbox() {
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'buzzer-inbox-'));
  const applied = [];
  const inbox = new Inbox({ dir, apply: (input) => {
    applied.push(input);
    return { ok: true, message: 'geladen' };
  } });
  await inbox.start();
  inbox.stop(); // the tests poll by hand
  return { dir, inbox, applied };
}

const exists = (p) => fs.access(p).then(() => true, () => false);

test('a valid round file is applied and moved to done/', async () => {
  const { dir, inbox, applied } = await makeInbox();
  await fs.writeFile(path.join(dir, 'runde-4.json'), JSON.stringify(ROUNDS.scale));
  await inbox.poll(); // first sight
  assert.equal(applied.length, 0);
  await inbox.poll(); // unchanged since: take it
  assert.equal(applied.length, 1);
  assert.equal(applied[0].kind, 'round');
  assert.equal(applied[0].value.id, 'runde-4');
  assert.equal(await exists(path.join(dir, 'done', 'runde-4.json')), true);
  assert.equal(await exists(path.join(dir, 'runde-4.json')), false);
});

test('an invalid file goes to failed/ with a German error file', async () => {
  const { dir, inbox, applied } = await makeInbox();
  await fs.writeFile(path.join(dir, 'quiz.json'), JSON.stringify({ ...ROUNDS.choice, answers: ['nur eine'], color: 'rot' }));
  await inbox.poll();
  await inbox.poll();
  assert.equal(applied.length, 0);
  assert.equal(await exists(path.join(dir, 'failed', 'quiz.json')), true);
  const report = JSON.parse(await fs.readFile(path.join(dir, 'failed', 'quiz.error.json'), 'utf8'));
  assert.equal(report.ok, false);
  assert.equal(report.file, 'quiz.json');
  assert.deepEqual(report.errors.map((e) => e.path).sort(), ['answers', 'color']);
  assert.match(report.error, /^Abgelehnt: 2 Fehler\./);
  assert.match(report.error, /Unbekanntes Feld "color"/);
});

test('broken JSON is reported, not retried forever', async () => {
  const { dir, inbox } = await makeInbox();
  await fs.writeFile(path.join(dir, 'kaputt.json'), '{"format": "buzzer-round/0",');
  await inbox.poll();
  await inbox.poll();
  const report = JSON.parse(await fs.readFile(path.join(dir, 'failed', 'kaputt.error.json'), 'utf8'));
  assert.match(report.errors[0].message, /Kein gültiges JSON/);
});

test('a file still being written waits until it stops changing', async () => {
  const { dir, inbox, applied } = await makeInbox();
  const file = path.join(dir, 'sitzung.json');
  const text = JSON.stringify({ format: 'buzzer-session/0', title: 'Workshop', rounds: [ROUNDS.race, ROUNDS.choice] });
  await fs.writeFile(file, text.slice(0, 20));
  await inbox.poll();
  await fs.writeFile(file, text);
  await inbox.poll(); // changed since the last look: wait
  assert.equal(applied.length, 0);
  await inbox.poll();
  assert.equal(applied.length, 1);
  assert.equal(applied[0].kind, 'session');
  assert.equal(applied[0].value.rounds.length, 2);
});

test('other files, hidden files and the result folders are left alone', async () => {
  const { dir, inbox, applied } = await makeInbox();
  await fs.writeFile(path.join(dir, 'notiz.txt'), 'hallo');
  await fs.writeFile(path.join(dir, '.runde.json.tmp'), '{}');
  await fs.writeFile(path.join(dir, '.versteckt.json'), JSON.stringify(ROUNDS.race));
  await inbox.poll();
  await inbox.poll();
  assert.equal(applied.length, 0);
  assert.deepEqual((await fs.readdir(dir)).sort(), ['.runde.json.tmp', '.versteckt.json', 'done', 'failed', 'notiz.txt']);
});

test('several files are taken oldest first', async () => {
  const { dir, inbox, applied } = await makeInbox();
  const a = path.join(dir, 'b-zweite.json');
  const b = path.join(dir, 'a-erste.json');
  await fs.writeFile(a, JSON.stringify({ ...ROUNDS.race, id: 'zweite' }));
  await fs.writeFile(b, JSON.stringify({ ...ROUNDS.race, id: 'erste' }));
  const old = new Date(Date.now() - 60000);
  await fs.utimes(b, old, old);
  await inbox.poll();
  await inbox.poll();
  assert.deepEqual(applied.map((x) => x.value.id), ['erste', 'zweite']);
});

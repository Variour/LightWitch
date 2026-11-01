import { test } from 'node:test';
import assert from 'node:assert/strict';
import { validateInput, validateRound, errorResult, parseJsonText } from '../src/validate.js';
import { ROUNDS } from './helpers.js';

const fails = (input, path, re) => {
  const r = validateInput(input);
  assert.equal(r.ok, false, `expected rejection of ${JSON.stringify(input)}`);
  const e = r.errors.find((x) => x.path === path);
  assert.ok(e, `no error at ${path}: ${JSON.stringify(r.errors)}`);
  if (re) assert.match(e.message, re);
  return r;
};

test('every example of the round format passes', () => {
  for (const r of Object.values(ROUNDS)) assert.equal(validateInput(r).ok, true, r.id);
  const session = {
    format: 'buzzer-session/0',
    title: 'Workshop am Nachmittag',
    rounds: [ROUNDS.scale, { format: 'buzzer-round/0', id: 'quiz-3', title: 'Quizfrage 3', type: 'choice', answers: ['Ja', 'Nein'], correct: null }],
  };
  const r = validateInput(session);
  assert.equal(r.ok, true);
  assert.equal(r.kind, 'session');
  assert.equal(r.value.rounds.length, 2);
});

test('defaults are filled in', () => {
  const r = validateInput({ format: 'buzzer-round/0', id: 'a', type: 'scale' });
  assert.deepEqual(r.value, { format: 'buzzer-round/0', id: 'a', title: '', question: '', type: 'scale', min: 0, max: 9, redMax: 3, greenMin: 7 });
  const c = validateInput({ format: 'buzzer-round/0', id: 'b', type: 'choice', answers: ['x', 'y'] });
  assert.equal(c.value.correct, null);
  const s = validateInput({ format: 'buzzer-session/0' });
  assert.deepEqual(s.value, { format: 'buzzer-session/0', title: '', rounds: [] });
});

test('unknown fields and fields of another type are named', () => {
  fails({ ...ROUNDS.race, timeLimit: 30 }, 'timeLimit', /Unbekanntes Feld "timeLimit"/);
  fails({ ...ROUNDS.race, answers: ['a', 'b'] }, 'answers', /gehört nicht zum Rundentyp "race"/);
  fails({ ...ROUNDS.scale, correct: 1 }, 'correct', /gehört nicht zum Rundentyp "scale"/);
  fails({ format: 'buzzer-session/0', rounds: [], channel: 6 }, 'channel', /Unbekanntes Feld "channel"/);
});

test('format, id and type are checked', () => {
  fails({ ...ROUNDS.race, format: 'buzzer-round/1' }, 'format', /buzzer-round\/0/);
  fails({ ...ROUNDS.race, id: 'Runde 4' }, 'id', /a-z, 0-9/);
  fails({ ...ROUNDS.race, id: 'x'.repeat(33) }, 'id');
  fails({ format: 'buzzer-round/0', id: 'a' }, 'type', /Pflichtfeld "type"/);
  fails({ format: 'buzzer-round/0', id: 'a', type: 'quiz' }, 'type');
  fails([1, 2], '(Datei)', /JSON-Objekt/);
});

test('text lengths count characters, not bytes', () => {
  assert.equal(validateInput({ ...ROUNDS.race, title: 'ä'.repeat(40) }).ok, true);
  fails({ ...ROUNDS.race, title: 'ä'.repeat(41) }, 'title', /höchstens 40 Zeichen/);
  fails({ ...ROUNDS.race, question: 'x'.repeat(201) }, 'question', /200/);
  fails({ ...ROUNDS.race, title: 4 }, 'title', /Text/);
});

test('scale rules', () => {
  fails({ ...ROUNDS.scale, max: 0 }, 'max', /größer als "min"/);
  fails({ ...ROUNDS.scale, min: 0, max: 30, redMax: 3, greenMin: 7 }, 'max', /24 Stufen/);
  fails({ ...ROUNDS.scale, redMax: 7, greenMin: 7 }, 'redMax', /kleiner als "greenMin"/);
  fails({ ...ROUNDS.scale, min: 4, redMax: 3 }, 'redMax', /nicht kleiner als "min"/);
  fails({ ...ROUNDS.scale, greenMin: 10 }, 'greenMin', /nicht größer als "max"/);
  fails({ ...ROUNDS.scale, max: 9.5 }, 'max', /ganze Zahl/);
  fails({ ...ROUNDS.scale, min: '0' }, 'min', /ganze Zahl/);
  assert.equal(validateInput({ ...ROUNDS.scale, min: 1, max: 10, redMax: 3, greenMin: 8 }).ok, true);
  assert.equal(validateInput({ ...ROUNDS.scale, min: 0, max: 23, redMax: 5, greenMin: 18 }).ok, true);
});

test('choice rules', () => {
  fails({ ...ROUNDS.choice, answers: ['nur eine'], correct: null }, 'answers', /2 bis 4/);
  fails({ ...ROUNDS.choice, answers: ['a', 'b', 'c', 'd', 'e'], correct: null }, 'answers', /2 bis 4/);
  fails({ ...ROUNDS.choice, answers: ['a', ''], correct: null }, 'answers[1]', /1 bis 80/);
  fails({ ...ROUNDS.choice, answers: ['a', 'b'], correct: 3 }, 'correct', /zwischen 1 und 2/);
  fails({ ...ROUNDS.choice, correct: 0 }, 'correct');
  fails({ format: 'buzzer-round/0', id: 'q', type: 'choice' }, 'answers', /Pflichtfeld "answers"/);
});

test('idle has no question', () => {
  fails({ ...ROUNDS.idle, question: 'Kaffee?' }, 'question', /keine Frage/);
  assert.equal(validateInput({ ...ROUNDS.idle, question: '' }).ok, true);
});

test('session rounds are checked one by one, ids must be unique', () => {
  const r = fails({ format: 'buzzer-session/0', rounds: [ROUNDS.race, ROUNDS.race] }, 'rounds[1].id', /doppelt/);
  assert.equal(r.errors.length, 1);
  fails({ format: 'buzzer-session/0', rounds: [ROUNDS.race, { ...ROUNDS.scale, max: 50 }] }, 'rounds[1].max');
  fails({ format: 'buzzer-session/0', rounds: Array.from({ length: 101 }, (_, i) => ({ ...ROUNDS.race, id: `r${i}` })) }, 'rounds', /100/);
  fails({ format: 'buzzer-session/0', title: 'x'.repeat(61) }, 'title', /60/);
});

test('messages name the round and read as German text', () => {
  const r = validateRound({ ...ROUNDS.scale, max: 0 });
  assert.match(r.errors[0].message, /^Runde "runde-4": /);
  const res = errorResult(r.errors);
  assert.equal(res.ok, false);
  assert.match(res.error, /^Abgelehnt: 1 Fehler\.\n- Runde "runde-4": "max" muss größer/);
});

test('JSON text with a byte order mark parses, broken JSON names the problem', () => {
  assert.deepEqual(parseJsonText('\uFEFF{"a":1}'), { ok: true, value: { a: 1 } });
  const bad = parseJsonText('{"a":');
  assert.equal(bad.ok, false);
  assert.match(bad.errors[0].message, /Kein gültiges JSON/);
});

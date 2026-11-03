// Keeps the Claude skill in skill/SKILL.md in step with the console: every
// JSON example in it, also the round in a notes block example, must pass the
// console's validator, and the frontmatter must have what the Claude skill
// loader needs.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { validateInput, parseJsonText } from '../src/validate.js';

const SKILL = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'skill', 'SKILL.md');

test('skill frontmatter has a name and a description', async () => {
  const text = await fs.readFile(SKILL, 'utf8');
  const head = /^---\n([\s\S]*?)\n---\n/.exec(text);
  assert.ok(head, 'frontmatter between --- lines');
  const name = /^name: (.+)$/m.exec(head[1]);
  const description = /^description: (.+)$/m.exec(head[1]);
  assert.equal(name?.[1], 'buzzer-rounds');
  assert.ok(description, 'description line');
  assert.ok(description[1].length <= 1024, 'description at most 1024 characters');
  assert.doesNotMatch(description[1], /[<>]/, 'description without angle brackets');
});

test('every JSON example in the skill passes the console validator', async () => {
  const text = await fs.readFile(SKILL, 'utf8');
  const blocks = [...text.matchAll(/```json\n([\s\S]*?)```/g)].map((m) => m[1]);
  assert.ok(blocks.length >= 10, `expected the skill's examples, found ${blocks.length} JSON blocks`);
  for (const block of blocks) {
    const parsed = parseJsonText(block);
    assert.equal(parsed.ok, true, `not JSON:\n${block}`);
    const result = validateInput(parsed.value);
    assert.equal(result.ok, true, `${JSON.stringify(result.errors)}\n${block}`);
  }
});

test('every notes block example in the skill holds a valid round', async () => {
  const text = await fs.readFile(SKILL, 'utf8');
  const blocks = [...text.matchAll(/```\n(Buzzer\n[\s\S]*?)```/g)].map((m) => m[1]);
  assert.ok(blocks.length >= 1, 'expected a notes block example');
  for (const block of blocks) {
    const parsed = parseJsonText(block.slice(block.indexOf('{')));
    assert.equal(parsed.ok, true, `not JSON:\n${block}`);
    const result = validateInput(parsed.value);
    assert.equal(result.ok, true, `${JSON.stringify(result.errors)}\n${block}`);
  }
});

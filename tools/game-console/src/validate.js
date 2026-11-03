// Checks the skill's JSON against the round format v0 (`buzzer-round/0`,
// `buzzer-session/0`). Unknown fields and fields of another round type are
// rejected and named: the skill is a language model, so fail loudly.
// Messages are German, ready to show to the presenter.

export const ROUND_FORMAT = 'buzzer-round/0';
export const SESSION_FORMAT = 'buzzer-session/0';

const COMMON_FIELDS = ['format', 'id', 'title', 'question', 'type'];
const TYPE_FIELDS = {
  scale: ['min', 'max', 'redMax', 'greenMin'],
  race: [],
  choice: ['answers', 'correct'],
  idle: [],
};
const TYPE_NAMES = { scale: 'Skala', race: 'Buzzer-Rennen', choice: 'Auswahl', idle: 'Ruhe' };
const ALL_TYPE_FIELDS = Object.values(TYPE_FIELDS).flat();
const SESSION_FIELDS = ['format', 'title', 'rounds'];
const ID_RE = /^[a-z0-9-]{1,32}$/;

const chars = (s) => [...s].length;
const isInt = (v) => typeof v === 'number' && Number.isInteger(v);
const kindOf = (v) => {
  if (v === null) return 'null';
  if (Array.isArray(v)) return 'eine Liste';
  if (typeof v === 'string') return 'ein Text';
  if (typeof v === 'number') return Number.isInteger(v) ? 'eine Zahl' : 'eine Kommazahl';
  if (typeof v === 'boolean') return 'ein Wahrheitswert';
  return 'ein Objekt';
};

function at(prefix, field) {
  return prefix ? `${prefix}.${field}` : field;
}

function checkString(obj, field, max, errors, prefix, { min = 0 } = {}) {
  if (!(field in obj)) return;
  const v = obj[field];
  if (typeof v !== 'string') {
    errors.push({ path: at(prefix, field), message: `"${field}" muss ein Text sein, ist aber ${kindOf(v)}.` });
    return;
  }
  const n = chars(v);
  if (n < min || n > max) {
    errors.push({
      path: at(prefix, field),
      message: min > 0
        ? `"${field}" muss ${min} bis ${max} Zeichen lang sein (ist ${n}).`
        : `"${field}" darf höchstens ${max} Zeichen lang sein (ist ${n}).`,
    });
  }
}

function checkInt(obj, field, errors, prefix) {
  const v = obj[field];
  if (!isInt(v)) {
    errors.push({ path: at(prefix, field), message: `"${field}" muss eine ganze Zahl sein, ist aber ${kindOf(v)}.` });
    return false;
  }
  return true;
}

// Returns { ok, round, errors }. `round` carries every default filled in.
export function validateRound(input, prefix = '') {
  const errors = [];
  const label = (o) => (o && typeof o.id === 'string' && o.id ? `Runde "${o.id}"` : 'Runde');
  if (!input || typeof input !== 'object' || Array.isArray(input)) {
    return { ok: false, errors: [{ path: prefix || '(Datei)', message: `Eine Runde muss ein JSON-Objekt sein, ist aber ${kindOf(input)}.` }] };
  }
  if (input.format !== ROUND_FORMAT) {
    errors.push({ path: at(prefix, 'format'), message: `"format" muss genau "${ROUND_FORMAT}" sein.` });
  }
  if (!('id' in input)) {
    errors.push({ path: at(prefix, 'id'), message: 'Pflichtfeld "id" fehlt.' });
  } else if (typeof input.id !== 'string' || !ID_RE.test(input.id)) {
    errors.push({ path: at(prefix, 'id'), message: '"id" muss 1 bis 32 Zeichen aus a-z, 0-9 und "-" haben, z. B. "runde-4".' });
  }
  checkString(input, 'title', 40, errors, prefix);
  checkString(input, 'question', 200, errors, prefix);

  const type = input.type;
  const typeOk = typeof type === 'string' && type in TYPE_FIELDS;
  if (!('type' in input)) {
    errors.push({ path: at(prefix, 'type'), message: 'Pflichtfeld "type" fehlt: "scale", "race", "choice" oder "idle".' });
  } else if (!typeOk) {
    errors.push({ path: at(prefix, 'type'), message: '"type" muss "scale", "race", "choice" oder "idle" sein.' });
  }

  for (const field of Object.keys(input)) {
    if (COMMON_FIELDS.includes(field)) continue;
    if (typeOk && TYPE_FIELDS[type].includes(field)) continue;
    if (typeOk && ALL_TYPE_FIELDS.includes(field)) {
      errors.push({ path: at(prefix, field), message: `Feld "${field}" gehört nicht zum Rundentyp "${type}" (${TYPE_NAMES[type]}).` });
    } else if (typeOk || !ALL_TYPE_FIELDS.includes(field)) {
      errors.push({ path: at(prefix, field), message: `Unbekanntes Feld "${field}".` });
    }
  }

  const round = {
    format: ROUND_FORMAT,
    id: input.id,
    title: typeof input.title === 'string' ? input.title : '',
    question: typeof input.question === 'string' ? input.question : '',
    type,
  };

  if (type === 'scale') {
    const s = { min: 0, max: 9, redMax: 3, greenMin: 7 };
    let intsOk = true;
    for (const f of ['min', 'max', 'redMax', 'greenMin']) {
      if (f in input) {
        if (checkInt(input, f, errors, prefix)) s[f] = input[f];
        else intsOk = false;
      }
    }
    if (intsOk) {
      if (s.min < 0 || s.min > 98) {
        errors.push({ path: at(prefix, 'min'), message: `"min" muss zwischen 0 und 98 liegen (ist ${s.min}).` });
      } else if (s.max <= s.min || s.max > 99) {
        errors.push({ path: at(prefix, 'max'), message: `"max" muss größer als "min" (${s.min}) und höchstens 99 sein (ist ${s.max}).` });
      } else if (s.max - s.min + 1 > 24) {
        errors.push({ path: at(prefix, 'max'), message: `Höchstens 24 Stufen: "max" - "min" + 1 ist ${s.max - s.min + 1}.` });
      } else if (s.redMax < s.min) {
        errors.push({ path: at(prefix, 'redMax'), message: `"redMax" (${s.redMax}) darf nicht kleiner als "min" (${s.min}) sein.` });
      } else if (s.redMax >= s.greenMin) {
        errors.push({ path: at(prefix, 'redMax'), message: `"redMax" (${s.redMax}) muss kleiner als "greenMin" (${s.greenMin}) sein.` });
      } else if (s.greenMin > s.max) {
        errors.push({ path: at(prefix, 'greenMin'), message: `"greenMin" (${s.greenMin}) darf nicht größer als "max" (${s.max}) sein.` });
      }
    }
    Object.assign(round, s);
  } else if (type === 'choice') {
    const answers = input.answers;
    if (!('answers' in input)) {
      errors.push({ path: at(prefix, 'answers'), message: 'Pflichtfeld "answers" fehlt: 2 bis 4 Antworten als Liste von Texten.' });
    } else if (!Array.isArray(answers)) {
      errors.push({ path: at(prefix, 'answers'), message: `"answers" muss eine Liste mit 2 bis 4 Texten sein, ist aber ${kindOf(answers)}.` });
    } else if (answers.length < 2 || answers.length > 4) {
      errors.push({ path: at(prefix, 'answers'), message: `"answers" braucht 2 bis 4 Antworten (sind ${answers.length}).` });
    } else {
      answers.forEach((a, i) => {
        const p = `${at(prefix, 'answers')}[${i}]`;
        if (typeof a !== 'string') errors.push({ path: p, message: `Antwort ${i + 1} muss ein Text sein, ist aber ${kindOf(a)}.` });
        else if (chars(a) < 1 || chars(a) > 80) errors.push({ path: p, message: `Antwort ${i + 1} muss 1 bis 80 Zeichen lang sein (ist ${chars(a)}).` });
      });
    }
    round.answers = Array.isArray(answers) ? answers : [];
    round.correct = null;
    if ('correct' in input && input.correct !== null) {
      const n = Array.isArray(answers) ? answers.length : 0;
      if (!isInt(input.correct)) {
        errors.push({ path: at(prefix, 'correct'), message: `"correct" muss eine ganze Zahl oder null sein, ist aber ${kindOf(input.correct)}.` });
      } else if (input.correct < 1 || (n >= 2 && input.correct > n)) {
        errors.push({ path: at(prefix, 'correct'), message: `"correct" muss zwischen 1 und ${n || 4} liegen (Nummer der richtigen Antwort) oder null sein.` });
      } else {
        round.correct = input.correct;
      }
    }
  } else if (type === 'idle') {
    if (typeof input.question === 'string' && input.question !== '') {
      errors.push({ path: at(prefix, 'question'), message: 'Eine Ruhe-Runde ("idle") hat keine Frage: "question" leer lassen oder weglassen.' });
    }
  }

  if (errors.length) {
    for (const e of errors) e.message = `${label(input)}: ${e.message}`;
    return { ok: false, errors };
  }
  return { ok: true, round, errors: [] };
}

// Returns { ok, session, errors }.
export function validateSession(input) {
  const errors = [];
  if (input.format !== SESSION_FORMAT) {
    errors.push({ path: 'format', message: `"format" muss genau "${SESSION_FORMAT}" sein.` });
  }
  for (const field of Object.keys(input)) {
    if (!SESSION_FIELDS.includes(field)) errors.push({ path: field, message: `Unbekanntes Feld "${field}" in der Sitzung.` });
  }
  checkString(input, 'title', 60, errors, '');
  const rounds = [];
  if ('rounds' in input) {
    if (!Array.isArray(input.rounds)) {
      errors.push({ path: 'rounds', message: `"rounds" muss eine Liste von Runden sein, ist aber ${kindOf(input.rounds)}.` });
    } else if (input.rounds.length > 100) {
      errors.push({ path: 'rounds', message: `Höchstens 100 Runden pro Sitzung (sind ${input.rounds.length}).` });
    } else {
      const seen = new Map();
      input.rounds.forEach((r, i) => {
        const res = validateRound(r, `rounds[${i}]`);
        errors.push(...res.errors);
        if (res.ok) {
          if (seen.has(r.id)) {
            errors.push({ path: `rounds[${i}].id`, message: `Runde "${r.id}": die id kommt doppelt vor (auch in Runde ${seen.get(r.id) + 1}).` });
          } else {
            seen.set(r.id, i);
          }
          rounds.push(res.round);
        }
      });
    }
  }
  if (errors.length) return { ok: false, errors };
  return { ok: true, session: { format: SESSION_FORMAT, title: input.title ?? '', rounds }, errors: [] };
}

// Dispatches on `format`. Returns { ok, kind: 'round' | 'session', value, errors }.
export function validateInput(input) {
  if (!input || typeof input !== 'object' || Array.isArray(input)) {
    return { ok: false, errors: [{ path: '(Datei)', message: `Erwartet ein JSON-Objekt mit "format", gefunden ${kindOf(input)}.` }] };
  }
  if (input.format === SESSION_FORMAT) {
    const r = validateSession(input);
    return { ok: r.ok, kind: 'session', value: r.session, errors: r.errors };
  }
  if (input.format === ROUND_FORMAT) {
    const r = validateRound(input);
    return { ok: r.ok, kind: 'round', value: r.round, errors: r.errors };
  }
  return {
    ok: false,
    errors: [{ path: 'format', message: `"format" muss "${ROUND_FORMAT}" (eine Runde) oder "${SESSION_FORMAT}" (eine Sitzung) sein.` }],
  };
}

// The error shape every input path returns (HTTP body, inbox error file, CLI).
export function errorResult(errors, summary) {
  const head = summary ?? (errors.length === 1 ? 'Abgelehnt: 1 Fehler.' : `Abgelehnt: ${errors.length} Fehler.`);
  return { ok: false, error: `${head}\n${errors.map((e) => `- ${e.message}`).join('\n')}`, errors };
}

// Parses JSON text with a German message on failure.
export function parseJsonText(text) {
  const t = text.replace(/^\uFEFF/, '');
  try {
    return { ok: true, value: JSON.parse(t) };
  } catch (e) {
    return { ok: false, errors: [{ path: '(Datei)', message: `Kein gültiges JSON: ${e.message}` }] };
  }
}

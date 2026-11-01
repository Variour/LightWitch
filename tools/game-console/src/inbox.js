import { EventEmitter } from 'node:events';
import fs from 'node:fs/promises';
import path from 'node:path';
import { validateInput, parseJsonText, errorResult } from './validate.js';

const POLL_MS = 500;

// Watches a folder for round and session files from the skill. A file is
// taken once it looked the same on two polls in a row (fully written), then
// validated and applied, and moved to done/ or, with <name>.error.json next
// to it, to failed/. Polling works the same on macOS, Windows and synced or
// network folders.
export class Inbox extends EventEmitter {
  constructor({ dir, apply, pollMs = POLL_MS }) {
    super();
    this.dir = path.resolve(dir);
    this.apply = apply; // ({ kind, value }) => { ok, message }
    this.pollMs = pollMs;
    this.seen = new Map(); // name -> "size:mtime"
    this.timer = null;
    this.busy = false;
  }

  async start() {
    await fs.mkdir(path.join(this.dir, 'done'), { recursive: true });
    await fs.mkdir(path.join(this.dir, 'failed'), { recursive: true });
    this.timer = setInterval(() => this.poll(), this.pollMs);
  }

  stop() {
    clearInterval(this.timer);
    this.timer = null;
  }

  async poll() {
    if (this.busy) return;
    this.busy = true;
    try {
      let names;
      try {
        names = await fs.readdir(this.dir);
      } catch {
        return;
      }
      const files = [];
      for (const name of names) {
        if (!name.toLowerCase().endsWith('.json') || name.startsWith('.')) continue;
        let st;
        try {
          st = await fs.stat(path.join(this.dir, name));
        } catch {
          continue;
        }
        if (!st.isFile()) continue;
        const sig = `${st.size}:${st.mtimeMs}`;
        const before = this.seen.get(name);
        this.seen.set(name, sig);
        if (before === sig) files.push({ name, mtime: st.mtimeMs });
      }
      for (const name of this.seen.keys()) if (!names.includes(name)) this.seen.delete(name);
      files.sort((a, b) => a.mtime - b.mtime || a.name.localeCompare(b.name));
      for (const f of files) {
        await this._process(f.name);
        this.seen.delete(f.name);
      }
    } finally {
      this.busy = false;
    }
  }

  async _process(name) {
    const src = path.join(this.dir, name);
    let text;
    try {
      text = await fs.readFile(src, 'utf8');
    } catch {
      return;
    }
    const parsed = parseJsonText(text);
    let result;
    if (!parsed.ok) {
      result = errorResult(parsed.errors);
    } else {
      const v = validateInput(parsed.value);
      result = v.ok ? this.apply({ kind: v.kind, value: v.value }) : errorResult(v.errors);
    }
    const target = path.join(this.dir, result.ok ? 'done' : 'failed', name);
    try {
      await fs.rename(src, target);
      if (!result.ok) {
        const base = name.replace(/\.json$/i, '');
        const report = { file: name, at: new Date().toISOString(), ...result };
        await fs.writeFile(path.join(this.dir, 'failed', `${base}.error.json`), JSON.stringify(report, null, 2) + '\n');
      }
    } catch (err) {
      this.emit('problem', `Datei ${name} ließ sich nicht verschieben: ${err.message}`);
    }
    this.emit('processed', { name, result });
  }
}

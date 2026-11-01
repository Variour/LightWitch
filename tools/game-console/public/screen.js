// Live screen for the quiz buzzers. Shows what the console sends over the
// WebSocket and decides nothing; keys only ask the console for an action.
(() => {
  'use strict';

  const stage = document.getElementById('stage');
  const strip = document.getElementById('strip');
  const dot = document.getElementById('dot');
  const root = document.documentElement;

  let state = null;
  let online = false;
  let viewKey = '';
  let fresh = true; // first fill of a view: no pop animations
  let frame = 0;
  let flash = null; // { text, level, until }
  let lastWinner = null;

  const store = {
    get(k) {
      try {
        return localStorage.getItem(k);
      } catch {
        return null;
      }
    },
    set(k, v) {
      try {
        localStorage.setItem(k, v);
      } catch {
        // private window or blocked storage: the setting lasts for this tab
      }
    },
  };

  const params = new URLSearchParams(location.search);
  root.dataset.theme = params.get('theme') || store.get('theme') || 'dark';
  strip.hidden = (params.get('strip') || store.get('strip')) === 'off';

  const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
  const de = (n, digits = 0) => Number(n).toLocaleString('de-DE', { minimumFractionDigits: digits, maximumFractionDigits: digits });

  const SYMBOLS = [
    '<svg viewBox="0 0 100 100" aria-hidden="true"><path d="M50 14 L88 82 L12 82 Z"/></svg>',
    '<svg viewBox="0 0 100 100" aria-hidden="true"><circle cx="50" cy="50" r="35"/></svg>',
    '<svg viewBox="0 0 100 100" aria-hidden="true"><path d="M22 22 L78 78 M78 22 L22 78"/></svg>',
    '<svg viewBox="0 0 100 100" aria-hidden="true"><rect x="19" y="19" width="62" height="62"/></svg>',
  ];
  const SYMBOL_NAMES = ['Dreieck', 'Kreis', 'Kreuz', 'Quadrat'];
  const LEDS = 16; // segments per column, like the buzzer's score column
  const RANK_ROWS = 8;

  // ── Connection ───────────────────────────────────────────────────────
  function connect() {
    const ws = new WebSocket(`ws://${location.host}/ws`);
    ws.onopen = () => {
      online = true;
    };
    ws.onmessage = (e) => {
      state = JSON.parse(e.data);
      schedule();
    };
    ws.onclose = () => {
      online = false;
      schedule();
      setTimeout(connect, 1000);
    };
  }

  async function act(action, args = {}) {
    try {
      const res = await fetch('/api/action', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action, ...args }),
      });
      const r = await res.json();
      showFlash(r.ok ? r.message : r.error, r.ok ? 'ok' : 'bad');
    } catch {
      showFlash('Konsole nicht erreichbar.', 'bad');
    }
  }

  function showFlash(text, level) {
    flash = { text: String(text || '').split('\n')[0], level, until: Date.now() + 4000 };
    schedule();
    setTimeout(schedule, 4100);
  }

  document.addEventListener('keydown', (e) => {
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    const k = e.key;
    if (k === 'ArrowRight' || k === 'PageDown' || k === ' ' || k === 'Enter') {
      e.preventDefault();
      act('next');
    } else if (k === 'l' || k === 'L') {
      if (state && state.session) act(state.session.login ? 'login-close' : 'login-open');
    } else if (k === 'a' || k === 'A') {
      act('admit', { mac: 'all' });
    } else if (k === 's' || k === 'S') {
      strip.hidden = !strip.hidden;
      store.set('strip', strip.hidden ? 'off' : 'on');
      schedule();
    } else if (k === 't' || k === 'T') {
      root.dataset.theme = root.dataset.theme === 'light' ? 'dark' : 'light';
      store.set('theme', root.dataset.theme);
    }
  });

  function schedule() {
    if (!frame) frame = requestAnimationFrame(render);
  }

  // ── Rendering ────────────────────────────────────────────────────────
  function render() {
    frame = 0;
    renderStrip();
    if (!state) return;
    const s = state;
    const r = s.round;
    const key = [s.view, r ? r.roundId : '', r ? r.phase : '', r && r.revealed ? 'rev' : ''].join(':');
    if (key !== viewKey) {
      viewKey = key;
      fresh = true;
      stage.innerHTML = VIEWS[s.view].build(s);
    }
    VIEWS[s.view].update(s);
    fresh = false;
  }

  function head(title, phaseText, phaseClass, question) {
    const q = question ? `<h2 class="question${question.length > 110 ? ' long' : ''}">${esc(question)}</h2>` : '';
    return `<header class="head"><div class="head-row"><div class="round-title">${esc(title)}</div>${phaseText ? `<span class="phase ${phaseClass || ''}" id="phase">${esc(phaseText)}</span>` : ''}</div>${q}</header>`;
  }

  function phaseLabel(r) {
    if (r.phase === 'announced') return ['Gesperrt', ''];
    if (r.phase === 'open') {
      if (r.type === 'race' && r.startAt && r.startAt > Date.now()) return ['Gleich geht’s los', 'soon'];
      return ['Offen', 'open'];
    }
    return [r.revealed ? 'Aufgelöst' : 'Geschlossen', ''];
  }

  // Keeps one element per key, so new tiles pop in and old ones stay put.
  function syncTiles(container, items, keyOf, paint) {
    const old = new Map([...container.children].map((el) => [el.dataset.key, el]));
    let prev = null;
    for (const item of items) {
      const key = String(keyOf(item));
      let el = old.get(key);
      if (el) old.delete(key);
      else {
        el = document.createElement('div');
        el.dataset.key = key;
        if (!fresh) {
          el.dataset.pop = '1';
          el.addEventListener('animationend', () => {
            delete el.dataset.pop;
            el.classList.remove('pop');
          }, { once: true });
        }
      }
      const [cls, html] = paint(item);
      const fullCls = cls + (el.dataset.pop ? ' pop' : '');
      if (el.className !== fullCls) el.className = fullCls;
      if (el.dataset.html !== html) {
        el.innerHTML = html;
        el.dataset.html = html;
      }
      const want = prev ? prev.nextSibling : container.firstChild;
      if (want !== el) container.insertBefore(el, want);
      prev = el;
    }
    for (const el of old.values()) el.remove();
  }

  const setText = (id, text) => {
    const el = document.getElementById(id);
    if (el && el.textContent !== text) el.textContent = text;
  };

  // Tile size for the number of tiles: normal, small, tiny.
  const density = (count, small, tiny) => {
    if (count > tiny) return ' small tiny';
    if (count > small) return ' small';
    return '';
  };

  const batteryBar = (bat) => (typeof bat === 'number'
    ? `<span class="bat${bat < 20 ? ' low' : ''}"><i style="width:${Math.max(4, Math.min(100, bat))}%"></i></span>`
    : '');

  const VIEWS = {
    start: {
      build: () => '<section class="hero"><h1 id="title"></h1><p id="sub"></p></section>',
      update: (s) => {
        setText('title', s.title || 'Quiz mit Buzzern');
        setText('sub', s.ended ? 'Danke fürs Mitmachen!' : 'Gleich geht’s los.');
      },
    },

    login: {
      build: (s) => `${head(s.title, 'Anmeldung offen', 'open')}
        <div class="login">
          <div class="login-left">
            <div class="ring" aria-hidden="true"></div>
            <h1>Buzzer drücken zum Mitmachen</h1>
            <p>Solange der Ring am Buzzer blau pulsiert, ist die Anmeldung offen.</p>
          </div>
          <div class="login-right">
            <div class="count-line"><b id="count">0</b><span>dabei</span></div>
            <div class="last-pressed" id="last"></div>
            <div class="tiles" id="tiles"></div>
          </div>
        </div>`,
      update: (s) => {
        document.getElementById('count').textContent = s.counts.admitted;
        const last = document.getElementById('last');
        last.innerHTML = s.lastPressed && s.lastPressed.n ? `Zuletzt gedrückt: <b>${esc(s.lastPressed.n)}</b>` : '';
        const items = [
          ...s.participants.map((p) => ({ key: p.mac, n: p.n, cls: p.admitted ? (p.pressed ? 'pressed' : '') : 'pending' })),
          ...s.waiting.map((w) => ({ key: w.mac, n: w.n ? `${w.n}?` : '?', cls: 'waiting' })),
        ];
        const tiles = document.getElementById('tiles');
        tiles.className = `tiles${density(items.length, 24, 60)}`;
        syncTiles(tiles, items, (i) => i.key, (i) => [`tile ${i.cls}`, esc(i.n)]);
      },
    },

    idle: {
      build: (s) => `${head(s.title, '', '', '')}
          <section class="hero" style="flex:0 0 auto"><h1 id="title"></h1></section>
          <p class="stat" id="stat"></p>
          <div class="tiles small" id="tiles"></div>`,
      update: (s) => {
        const r = s.round;
        if (s.session && s.session.ending) setText('title', 'Danke fürs Mitmachen!');
        else setText('title', r && r.type === 'idle' && r.title ? r.title : 'Pause');
        document.getElementById('stat').innerHTML = `<b>${s.counts.admitted}</b> Teilnehmer, <b>${s.counts.online}</b> online`;
        const tiles = document.getElementById('tiles');
        tiles.className = `tiles${density(s.participants.length, 0, 95)}`;
        syncTiles(tiles, s.participants, (p) => p.mac,
          (p) => [`tile${p.online ? '' : ' offline'}${p.admitted ? '' : ' pending'}`, `${esc(p.n)}${batteryBar(p.bat)}`]);
      },
    },

    scale: {
      build: (s) => {
        const r = s.round;
        const [pt, pc] = phaseLabel(r);
        const zoneName = { red: 'rot', yellow: 'gelb', green: 'grün' };
        const range = (a, b) => (a === b ? `${a}` : `${a} bis ${b}`);
        const legend = `<div class="legend">
            <span><i class="zone-red"></i>${range(r.min, r.redMax)} ${zoneName.red}</span>
            <span><i class="zone-yellow"></i>${range(r.redMax + 1, r.greenMin - 1)} ${zoneName.yellow}</span>
            <span><i class="zone-green"></i>${range(r.greenMin, r.max)} ${zoneName.green}</span></div>`;
        const cols = r.results.counts.map((c) => `<div class="col zone-${c.zone}" data-v="${c.v}">
            <div class="count" data-count></div>
            <div class="leds">${'<i class="led"></i>'.repeat(LEDS)}</div>
            <div class="val">${c.v}</div></div>`).join('');
        return `${head(r.title, pt, pc, r.question)}
          <div class="body">
            <div class="scale-chart">
              <div class="columns">${cols}</div>
              <div class="scale-foot"><span class="stat" id="voters"></span><span class="stat" id="mean"></span></div>
            </div>
            <div class="scale-tiles">${r.phase === 'announced' ? `<div class="scale-intro"><p class="big-note">Abstimmen mit den Tasten hoch (grün) und runter (rot), sobald die Runde offen ist.</p>${legend}</div>` : '<div class="tiles small" id="tiles"></div>'}</div>
          </div>`;
      },
      update: (s) => {
        const res = s.round.results;
        const full = Math.max(5, ...res.counts.map((c) => c.count));
        for (const c of res.counts) {
          const col = stage.querySelector(`.col[data-v="${c.v}"]`);
          if (!col) continue;
          const cnt = col.querySelector('[data-count]');
          cnt.textContent = c.count;
          cnt.classList.toggle('zero', c.count === 0);
          const lit = c.count === 0 ? 0 : Math.max(1, Math.round((c.count / full) * LEDS));
          col.querySelectorAll('.led').forEach((led, i) => led.classList.toggle('on', i < lit));
        }
        document.getElementById('voters').innerHTML = `<b>${res.voters}</b> von ${res.participants} haben abgestimmt`;
        document.getElementById('mean').innerHTML = res.mean === null ? '' : `Mittelwert <b>${de(res.mean, 1)}</b>`;
        const tiles = document.getElementById('tiles');
        if (tiles) {
          tiles.className = `tiles${density(res.entries.length, 0, 56)}`;
          syncTiles(tiles, res.entries, (e) => e.n,
            (e) => [`tile vote zone-${e.zone}`, `<span class="n">${esc(e.n)}</span><span class="v">${esc(e.v)}</span>`]);
        }
      },
    },

    race: {
      build: (s) => {
        const r = s.round;
        const [pt, pc] = phaseLabel(r);
        return `${head(r.title, pt, pc, r.question)}
          <div class="body">
            <div class="race-left" id="left"></div>
            <div class="ranking" id="ranking"></div>
          </div>`;
      },
      update: (s) => {
        const r = s.round;
        const ranking = r.results.ranking;
        const left = document.getElementById('left');
        const now = Date.now();
        let html;
        let leftKey;
        if (r.phase === 'announced') {
          leftKey = 'locked';
          html = '<p class="big-note">Buzzer gesperrt.</p>';
        } else if (r.phase === 'open' && r.startAt && r.startAt > now) {
          leftKey = 'countdown';
          html = `<div class="countdown">${de((r.startAt - now) / 1000, 1)}</div><p class="big-note">Drücken, sobald der Ring weiß leuchtet.</p>`;
          requestAnimationFrame(schedule);
        } else if (!ranking.length) {
          leftKey = 'go';
          html = r.phase === 'open' ? '<div class="go">Los!</div>' : '<p class="big-note">Niemand hat gedrückt.</p>';
        } else {
          const w = r.winner || ranking[0].n;
          const entry = ranking.find((e) => e.n === w);
          leftKey = `w${w}|${r.winner}|${entry && entry.tie}|${entry && entry.t}|${r.invalid}`;
          html = `<div class="winner-label">${r.winner ? 'Sieger' : 'Am schnellsten'}</div>
            <div class="winner${lastWinner !== w ? ' reveal' : ''}">${esc(w)}</div>
            ${entry ? `<div class="winner-time">${de(entry.t)} ms${entry.tie ? ' <span class="warn">Gleichstand</span>' : ''}</div>` : ''}
            ${r.invalid ? '<p class="invalid">Rangliste ungültig: das Gateway wurde neu gestartet.</p>' : ''}`;
          lastWinner = w;
        }
        if (left.dataset.key !== leftKey || leftKey === 'countdown') {
          left.dataset.key = leftKey;
          left.innerHTML = html;
        }
        const phase = document.getElementById('phase');
        const [pt, pc] = phaseLabel(r);
        if (phase && phase.textContent !== pt) {
          phase.textContent = pt;
          phase.className = `phase ${pc}`;
        }
        const shown = ranking.slice(0, RANK_ROWS);
        const rows = shown.map((e) => `<div class="rank-row${e.rank === 1 ? ' first' : ''}">
            <span class="r">${e.rank}.</span><span class="num">${esc(e.n)}</span>
            <span class="ms">${de(e.t)} ms</span>${e.tie ? '<span class="tie">Gleichstand</span>' : '<span></span>'}</div>`).join('');
        const more = ranking.length > shown.length ? `<div class="more">und ${ranking.length - shown.length} weitere</div>` : '';
        const ranks = document.getElementById('ranking');
        const htmlR = rows + more;
        if (ranks.dataset.html !== htmlR) {
          ranks.innerHTML = htmlR;
          ranks.dataset.html = htmlR;
        }
      },
    },

    choice: {
      build: (s) => {
        const r = s.round;
        const [pt, pc] = phaseLabel(r);
        const answers = r.answers.length ? r.answers : ['Antwort 1', 'Antwort 2', 'Antwort 3', 'Antwort 4'];
        const rows = answers.map((a, i) => {
          const mark = r.revealed ? (r.correct === i + 1 ? ' right' : ' wrong') : '';
          return `<div class="answer k${i + 1}${mark}" data-a="${i}">
            <span class="fill" style="width:0"></span>
            <span class="key" title="${SYMBOL_NAMES[i]}">${SYMBOLS[i]}</span>
            <span class="text">${esc(a)}</span><span class="cnt"></span></div>`;
        }).join('');
        return `${head(r.title, pt, pc, r.question)}
          <div class="body">
            <div class="answers">${rows}</div>
            <div class="choice-side" id="side"></div>
          </div>`;
      },
      update: (s) => {
        const r = s.round;
        const res = r.results;
        const side = document.getElementById('side');
        let html;
        if (r.phase === 'announced') {
          html = '<p class="big-note">Antworten mit den Tasten, sobald die Runde offen ist.</p>';
        } else if (r.phase === 'open') {
          const n = Math.max(1, Math.min(res.participants, 50));
          const on = res.participants ? Math.round((res.answered / res.participants) * n) : 0;
          html = `<div class="answered"><b>${res.answered}</b><span>von ${res.participants} haben geantwortet</span></div>
            <div class="progress">${Array.from({ length: n }, (_, i) => `<i${i < on ? ' class="on"' : ''}></i>`).join('')}</div>`;
        } else if (r.revealed) {
          const right = res.right || [];
          html = `<p class="stat"><b>${right.length}</b> von ${res.answered} lagen richtig</p>
            ${right.length ? `<p class="right-list"><b>${right.map(esc).join(', ')}</b></p>` : ''}`;
        } else {
          html = `<p class="stat"><b>${res.answered}</b> von ${res.participants} haben geantwortet</p>${r.hasCorrect ? '<p class="big-note">Auflösung folgt.</p>' : ''}`;
        }
        if (side.dataset.html !== html) {
          side.innerHTML = html;
          side.dataset.html = html;
        }
        if (res.counts) {
          const total = res.counts.reduce((a, b) => a + b, 0) || 1;
          res.counts.forEach((c, i) => {
            const row = stage.querySelector(`.answer[data-a="${i}"]`);
            if (!row) return;
            row.querySelector('.fill').style.width = `${(c / total) * 100}%`;
            row.querySelector('.cnt').innerHTML = `${c}<small>${de((c / total) * 100)} %</small>`;
          });
        }
      },
    },
  };

  function renderStrip() {
    const s = state;
    dot.hidden = !strip.hidden || (online && s && s.link.state === 'connected');
    if (strip.hidden) return;
    const parts = [];
    if (!online || !s) {
      parts.push('<span class="bad">Konsole nicht erreichbar</span>');
    } else {
      const l = s.link;
      const where = l.mode === 'sim' ? 'Simulator' : `USB ${esc(l.port || '')}`;
      if (l.state === 'connected') parts.push(`<span class="ok">● ${where} verbunden</span>`);
      else if (l.state === 'handshake') parts.push(`<span class="warn">● ${where}: warte auf Antwort</span>`);
      else parts.push(`<span class="bad">● Gateway getrennt${l.status ? `: ${esc(l.status)}` : ''}</span>`);
      parts.push(`<span>${s.counts.online} von ${s.counts.admitted} online${s.counts.waiting ? `, ${s.counts.waiting} warten` : ''}</span>`);
      const c = l.counters;
      if (c) {
        const cls = c.dropped || c.rxOverflow ? 'warn' : '';
        parts.push(`<span class="${cls}">verworfen ${c.dropped}, Funkpuffer voll ${c.rxOverflow}</span>`);
      }
      if (s.session) {
        parts.push(`<span>Kanal ${s.session.channel}, Anmeldung ${s.session.login ? 'offen' : 'zu'}${s.settings.autoAdmit ? '' : ' (Aufnahme von Hand)'}</span>`);
      }
      parts.push(`<span>Nächste: ${s.plan.next ? esc(s.plan.next.title || s.plan.next.id) : '-'}</span>`);
      parts.push('<span class="sep"></span>');
      const last = s.messages[s.messages.length - 1];
      if (flash && flash.until > Date.now()) parts.push(`<span class="msg ${flash.level}">${esc(flash.text)}</span>`);
      else if (s.warning) parts.push(`<span class="msg warn">${esc(s.warning)}</span>`);
      else if (last && last.level !== 'info' && s.now - last.at < 60000) parts.push(`<span class="msg ${last.level === 'error' ? 'bad' : 'warn'}">${esc(last.text)}</span>`);
    }
    parts.push('<span><kbd>→</kbd> Weiter <kbd>L</kbd> Anmeldung <kbd>S</kbd> Leiste <kbd>T</kbd> Hell/Dunkel</span>');
    const html = parts.join('');
    if (strip.dataset.html !== html) {
      strip.innerHTML = html;
      strip.dataset.html = html;
    }
  }

  connect();
  render();
})();

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
  let updateArmedUntil = 0; // U pressed once in the charging overview

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

  // Keys of the charging overview. The presenter keys lead back to the game.
  function fleetKey(e) {
    const k = e.key.length === 1 ? e.key.toLowerCase() : e.key;
    if (k === 'f' || k === 'ArrowRight' || k === 'PageDown' || k === ' ' || k === 'Enter' || k === 'Escape') {
      e.preventDefault();
      act('fleet-view', { mode: 'off' });
    } else if (k === 'e') {
      act('charge', { target: 'event' });
    } else if (k === 'l') {
      act('charge', { target: 'store' });
    } else if (k === 'u') {
      if (Date.now() < updateArmedUntil) {
        updateArmedUntil = 0;
        act('update-all');
      } else {
        updateArmedUntil = Date.now() + 4000;
        showFlash('Nochmal U: Update an alle senden.', 'ok');
      }
    } else {
      return false;
    }
    return true;
  }

  document.addEventListener('keydown', (e) => {
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    const k = e.key;
    if (state && state.view === 'fleet' && fleetKey(e)) return;
    if (k === 'f' || k === 'F') {
      act('fleet-view', { mode: 'on' });
    } else if (k === 'ArrowRight' || k === 'PageDown' || k === ' ' || k === 'Enter') {
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
    const key = s.view === 'fleet' ? 'fleet' : [s.view, r ? r.roundId : '', r ? r.phase : '', r && r.revealed ? 'rev' : ''].join(':');
    if (key !== viewKey) {
      viewKey = key;
      fresh = true;
      stage.innerHTML = VIEWS[s.view].build(s);
    }
    VIEWS[s.view].update(s);
    updateReady(s);
    fresh = false;
  }

  // A round that requires a lage: how many hold their buzzer that way, while
  // announced and, in a race, until the release.
  function updateReady(s) {
    const el = document.getElementById('ready');
    if (!el) return;
    const r = s.round;
    const res = r && r.results;
    const waiting = r && (r.phase === 'announced' || (r.type === 'race' && r.phase === 'open' && r.startAt && r.startAt > Date.now()));
    const html = res && typeof res.ready === 'number' && waiting
      ? `<b>${res.ready}</b> von ${res.participants} ${r.hold === 'liegend' ? 'liegen bereit' : 'bereit'}`
      : '';
    if (el.dataset.html !== html) {
      el.innerHTML = html;
      el.dataset.html = html;
      el.hidden = !html;
    }
  }

  function head(title, phaseText, phaseClass, question) {
    const q = question ? `<h2 class="question${question.length > 110 ? ' long' : ''}">${esc(question)}</h2>` : '';
    return `<header class="head"><div class="head-row"><div class="round-title">${esc(title)}</div>${phaseText ? `<span class="phase ${phaseClass || ''}" id="phase">${esc(phaseText)}</span>` : ''}</div>${q}<p class="stat" id="ready" hidden></p></header>`;
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

  const STATE_NAMES = { active: 'aktiv', charging: 'am Netz', standby: 'Standby' };
  const CHARGE_NAMES = { none: 'ohne Lader', charging: 'lädt', full: 'voll', discharging: 'Akku', fault: 'Ladefehler' };
  const TARGETS = { store: ['Lagern', 80], event: ['Event', 100] };

  const duration = (sec) => {
    if (sec < 60) return `${sec} s`;
    if (sec < 3600) return `${Math.floor(sec / 60)} min`;
    return `${Math.floor(sec / 3600)} h`;
  };

  function fleetTile(d) {
    const cls = ['ftile', `chg-${d.chg || 'none'}`];
    if (d.error) cls.push('error');
    else if (d.updating) cls.push('updating');
    else if (d.fwStatus === 'outdated') cls.push('outdated');
    if (d.stale) cls.push('stale');
    const bat = typeof d.bat === 'number' ? d.bat : null;
    const target = TARGETS[d.target];
    const chgWord = d.chg ? CHARGE_NAMES[d.chg] : '';
    let fw = `<span class="ft-fw">${esc(d.fw || '?')}</span>`;
    if (d.fwStatus === 'outdated') fw += '<span class="ft-old">veraltet</span>';
    else if (d.fwStatus === 'current') fw += '<span class="ft-ok">aktuell</span>';
    let extra = d.stale ? `<div class="ft-row ft-age">keine Meldung seit ${duration(d.ageS)}</div>` : '';
    if (d.updating) {
      const label = d.upd === 'checking' ? 'prüft' : d.upd === 'installing' ? 'installiert' : `Update ${d.pct ?? 0} %`;
      const pct = d.upd === 'checking' ? 0 : d.upd === 'installing' ? 100 : (d.pct ?? 0);
      extra = `<div class="ft-row ft-upd"><span>${label}</span><span class="ft-ubar"><i style="width:${pct}%"></i></span></div>`;
    }
    const problems = [];
    if (d.upd === 'failed') problems.push(d.updErrText ? `Update fehlgeschlagen: ${d.updErrText}` : 'Update fehlgeschlagen');
    problems.push(...d.errors);
    if (problems.length) extra += `<div class="ft-row ft-err">${esc(problems.join(', '))}</div>`;
    if (d.wifi === false && !(d.upd === 'failed' && d.updErr === 'no_wifi')) extra += '<div class="ft-row ft-nowifi">kein WLAN</div>';
    const html = `<div class="ft-row ft-top"><span class="ft-n">${d.n ? esc(d.n) : '-'}</span><span class="ft-state st-${d.state || 'none'}">${esc(STATE_NAMES[d.state] || '?')}</span></div>
      <div class="ft-row ft-bat"><b>${bat === null ? '?' : bat}<small> %</small></b><span class="ft-chg">${esc(chgWord)}</span></div>
      <div class="ft-row ft-barrow"><span class="ft-bar"><i style="width:${bat ?? 0}%"></i>${target ? `<em style="left:${target[1]}%"></em>` : ''}</span><span class="ft-target">${target ? `→ ${target[0]}` : '?'}</span></div>
      <div class="ft-row ft-fwrow">${fw}</div>${extra}`;
    return [cls.join(' '), html];
  }

  const VIEWS = {
    fleet: {
      build: () => `<header class="head"><div class="head-row"><div class="fleet-title">Ladeübersicht</div><div class="fleet-ref" id="fref"></div></div>
          <div class="fleet-link" id="flink" hidden></div>
          <div class="fleet-counts" id="fcounts"></div></header>
        <div class="fleet-grid" id="fgrid"></div>`,
      update: (s) => {
        const f = s.fleet;
        const c = f.counts;
        const ref = f.ref.fw ? `Referenz-Firmware <b>${esc(f.ref.fw)}</b> (${f.ref.source === 'console' ? 'in der Konsole gesetzt' : 'Gateway'})` : 'Referenz-Firmware unbekannt';
        const refEl = document.getElementById('fref');
        if (refEl.dataset.html !== ref) {
          refEl.innerHTML = ref;
          refEl.dataset.html = ref;
        }
        // The gateway updates itself with `update-all` and restarts: say so plainly.
        const linkEl = document.getElementById('flink');
        let linkHtml = '';
        let linkCls = 'fleet-link';
        if (s.link.state !== 'connected') {
          linkHtml = 'Gateway getrennt. Nach einem Update startet es neu und meldet sich gleich wieder.';
          linkCls += ' bad';
        } else if (s.link.fwChange) {
          linkHtml = `Gateway wieder verbunden, neue Firmware <b>${esc(s.link.fwChange.to)}</b> (vorher ${esc(s.link.fwChange.from)}).`;
          linkCls += ' ok';
        }
        if (linkEl.dataset.html !== linkHtml || linkEl.className !== linkCls) {
          linkEl.innerHTML = linkHtml;
          linkEl.dataset.html = linkHtml;
          linkEl.className = linkCls;
          linkEl.hidden = !linkHtml;
        }
        const counts = [
          `<span class="fc fc-charging"><i></i><b>${c.charging}</b> laden</span>`,
          `<span class="fc fc-full"><i></i><b>${c.full}</b> voll</span>`,
          `<span class="fc fc-outdated${c.outdated ? '' : ' zero'}"><i></i><b>${c.outdated}</b> veraltet</span>`,
          c.updating ? `<span class="fc fc-updating"><i></i><b>${c.updating}</b> Update läuft</span>` : '',
          `<span class="fc fc-error${c.errors ? '' : ' zero'}"><i></i><b>${c.errors}</b> Fehler</span>`,
          `<span class="fc fc-total">${c.total} Gerät${c.total === 1 ? '' : 'e'}${c.silent ? `, ${c.silent} ohne Meldung` : ''}</span>`,
        ].join('');
        const cEl = document.getElementById('fcounts');
        if (cEl.dataset.html !== counts) {
          cEl.innerHTML = counts;
          cEl.dataset.html = counts;
        }
        const grid = document.getElementById('fgrid');
        const n = f.devices.length;
        grid.className = `fleet-grid${n > 50 ? ' tiny' : n > 30 ? ' small' : n > 12 ? ' mid' : ''}`;
        if (!n) {
          grid.innerHTML = '<p class="big-note">Noch kein Gerät gemeldet. Geräte außerhalb einer Sitzung melden sich etwa alle 30 s.</p>';
          return;
        }
        if (grid.querySelector('.big-note')) grid.innerHTML = '';
        syncTiles(grid, f.devices, (d) => d.mac, fleetTile);
      },
    },

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

    // The board itself is in PowerPoint; the screen shows the count and the winner.
    move: {
      build: (s) => {
        const r = s.round;
        const [pt, pc] = phaseLabel(r);
        return `${head(r.title, pt, pc, r.question)}
          <div class="body">
            <div class="move-side" id="finished"></div>
            <div class="move-side" id="first"></div>
          </div>`;
      },
      update: (s) => {
        const res = s.round.results;
        const fin = `<div class="answered"><b>${res.finished}</b><span>von ${res.participants} im Ziel</span></div>`;
        const finEl = document.getElementById('finished');
        if (finEl.dataset.html !== fin) {
          finEl.innerHTML = fin;
          finEl.dataset.html = fin;
        }
        const w = res.winner;
        const first = w === null ? '' : `<div class="winner-label">Zuerst im Ziel</div><div class="winner${lastWinner !== w ? ' reveal' : ''}">${esc(w)}</div>`;
        const firstEl = document.getElementById('first');
        const key = w === null ? '' : String(w);
        if (firstEl.dataset.key !== key) {
          firstEl.dataset.key = key;
          firstEl.innerHTML = first;
          if (w !== null) lastWinner = w;
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
    if (s && s.view === 'fleet') parts.push('<span><kbd>F</kbd> Zurück <kbd>E</kbd> Ziel Event <kbd>L</kbd> Ziel Lagern <kbd>U</kbd> Update an alle <kbd>T</kbd> Hell/Dunkel</span>');
    else parts.push('<span><kbd>→</kbd> Weiter <kbd>L</kbd> Anmeldung <kbd>F</kbd> Ladeübersicht <kbd>S</kbd> Leiste <kbd>T</kbd> Hell/Dunkel</span>');
    const html = parts.join('');
    if (strip.dataset.html !== html) {
      strip.innerHTML = html;
      strip.dataset.html = html;
    }
  }

  connect();
  render();
})();

// Lamp file for LightWitch (format "lightwitch-lamp" v1, defined in the LightWitch
// brightness work, docs/lamp-format.md), project file and share links.
import { centroid, normal, edgePts, mid, sub, norm, roundV } from './geometry.js';

// Module placement: p = grid centroid, n = LED-side normal, u = from the IN edge (base)
// midpoint towards the apex. World = p + x*r + y*u with r = u x n.
export function placement(t, rot) {
  const [P, Q] = edgePts(t, rot), R = t.v[(rot + 2) % 3];
  return { p: roundV(centroid(t)), n: roundV(normal(t)), u: roundV(norm(sub(R, mid(P, Q)))) };
}

export function lampFile(project, wiring, modules) {
  const used = [...new Set(project.tris.map(t => t.type))];
  const types = {}; used.forEach(k => { types[k] = modules.types[k]; });
  const outputs = wiring.chains.map(c => ({
    id: c.id,
    modules: c.path.map(i => { const t = project.tris[i]; return { id: t.id, t: t.type, ...placement(t, wiring.per[i].rot) }; }),
  }));
  return { format: 'lightwitch-lamp', version: 1, name: project.name, types, outputs };
}

// Project file: only what cannot be derived. Chain positions are stored once locked so a
// soldered lamp keeps its wiring.
export function projectFile(project) {
  return {
    format: 'lightwitch-lamp-project', version: 1, name: project.name, controller: project.controller, pack: project.pack,
    tris: project.tris.map(t => { const o = { id: t.id, v: t.v, type: t.type, out: t.out }; if (Number.isInteger(t.pos)) o.pos = t.pos; return o; }),
  };
}
export function readProject(obj) {
  if (!obj || obj.format !== 'lightwitch-lamp-project') throw new Error('Keine Projektdatei des Lampen-Konfigurators');
  if (obj.version !== 1) throw new Error('Projektversion ' + obj.version + ' wird nicht unterstützt');
  if (!Array.isArray(obj.tris)) throw new Error('Projekt ohne Dreiecke');
  return { ...obj, tris: obj.tris.map(t => ({ ...t })) };
}

// deflate-raw + base64url, the encoding of #lamp= and #p= links.
const b64url = bytes => { let s = ''; for (const b of bytes) s += String.fromCharCode(b); return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, ''); };
const unb64url = s => { const b = atob(s.replace(/-/g, '+').replace(/_/g, '/')); return Uint8Array.from(b, c => c.charCodeAt(0)); };
async function pipe(bytes, stream) { return new Uint8Array(await new Response(new Blob([bytes]).stream().pipeThrough(stream)).arrayBuffer()); }
export async function pack(obj) { return b64url(await pipe(new TextEncoder().encode(JSON.stringify(obj)), new CompressionStream('deflate-raw'))); }
export async function unpack(s) { return JSON.parse(new TextDecoder().decode(await pipe(unb64url(s), new DecompressionStream('deflate-raw')))); }

// Device address as typed: "laterne", "laterne.local", "192.168.1.20", "http://..."
export function deviceUrl(addr) {
  let a = String(addr || '').trim().replace(/\/+$/, '');
  if (!a) return null;
  if (!/^https?:\/\//.test(a)) { if (!/[.:]/.test(a)) a += '.local'; a = 'http://' + a; }
  return a + '/';
}

import { createHmac, timingSafeEqual } from 'crypto';
import express from 'express';

const DEV_NO_AUTH = process.env.DEV_NO_AUTH === 'true';

const CLIENT_ID = process.env.GITHUB_CLIENT_ID;
const CLIENT_SECRET = process.env.GITHUB_CLIENT_SECRET;
const SESSION_SECRET = process.env.SESSION_SECRET;
const TOKEN_SECRET = process.env.TOKEN_SECRET;
const AUTH_ORIGIN = process.env.AUTH_ORIGIN?.replace(/\/$/, '');
const ALLOWED_USERS = process.env.ALLOWED_GITHUB_USERS
  ? new Set(process.env.ALLOWED_GITHUB_USERS.split(',').map(u => u.trim()).filter(Boolean))
  : null;

// Auth server: has GitHub credentials and no AUTH_ORIGIN.
// Auth client: has AUTH_ORIGIN and no GitHub credentials.
// Both modes require TOKEN_SECRET, SESSION_SECRET, and ALLOWED_GITHUB_USERS.
const IS_AUTH_SERVER = !!CLIENT_ID;

if (!DEV_NO_AUTH) {
  const missing = [];
  if (!CLIENT_ID && !AUTH_ORIGIN)         missing.push('GITHUB_CLIENT_ID (or AUTH_ORIGIN for PR containers)');
  if (IS_AUTH_SERVER && !CLIENT_SECRET)   missing.push('GITHUB_CLIENT_SECRET');
  if (!SESSION_SECRET)                    missing.push('SESSION_SECRET');
  if (!TOKEN_SECRET)                      missing.push('TOKEN_SECRET');
  if (!ALLOWED_USERS)                     missing.push('ALLOWED_GITHUB_USERS');
  if (missing.length) {
    console.error('Auth misconfigured — missing env vars:\n' + missing.map(v => `  - ${v}`).join('\n'));
    console.error('Set DEV_NO_AUTH=true to bypass auth for local development.');
  }
}

const SESSION_MAX_AGE = 7 * 24 * 60 * 60; // 7 days in seconds
const TOKEN_TTL_MS = 5 * 60 * 1000;       // 5 minutes (cross-container token)

// --- Cookie helpers ---

function getCookie(req, name) {
  for (const part of (req.headers.cookie || '').split(';')) {
    const [k, ...v] = part.trim().split('=');
    if (k === name) return decodeURIComponent(v.join('='));
  }
  return null;
}

function hmacSign(data, secret) {
  return createHmac('sha256', secret).update(data).digest('base64url');
}

function safeEqual(a, b) {
  try { return timingSafeEqual(Buffer.from(a), Buffer.from(b)); } catch { return false; }
}

function makeSessionValue(username) {
  const expires = Date.now() + SESSION_MAX_AGE * 1000;
  const payload = `${username}|${expires}`;
  return `${payload}|${hmacSign(payload, SESSION_SECRET)}`;
}

function parseSessionValue(value) {
  if (!value || !SESSION_SECRET) return null;
  const lastPipe = value.lastIndexOf('|');
  if (lastPipe < 0) return null;
  const payload = value.slice(0, lastPipe);
  const sig = value.slice(lastPipe + 1);
  if (!safeEqual(hmacSign(payload, SESSION_SECRET), sig)) return null;
  const [username, expires] = payload.split('|');
  if (Date.now() > parseInt(expires)) return null;
  return username || null;
}

function cookieFlags(req) {
  const secure = req.secure || req.headers['x-forwarded-proto'] === 'https';
  return `HttpOnly; SameSite=Lax; Path=/${secure ? '; Secure' : ''}`;
}

function setSessionCookie(req, res, username) {
  res.setHeader('Set-Cookie',
    `bl_session=${encodeURIComponent(makeSessionValue(username))}; ${cookieFlags(req)}; Max-Age=${SESSION_MAX_AGE}`);
}

function clearSessionCookie(req, res) {
  res.setHeader('Set-Cookie', `bl_session=; ${cookieFlags(req)}; Max-Age=0`);
}

export function getSessionUser(req) {
  return parseSessionValue(getCookie(req, 'bl_session'));
}

// --- Cross-container token (short-lived, used once during redirect) ---

function makeToken(username) {
  const expires = Date.now() + TOKEN_TTL_MS;
  const payload = `${username}|${expires}`;
  return Buffer.from(`${payload}|${hmacSign(payload, TOKEN_SECRET)}`).toString('base64url');
}

function parseToken(token) {
  try {
    const decoded = Buffer.from(token, 'base64url').toString();
    const lastPipe = decoded.lastIndexOf('|');
    const payload = decoded.slice(0, lastPipe);
    const sig = decoded.slice(lastPipe + 1);
    if (!safeEqual(hmacSign(payload, TOKEN_SECRET), sig)) return null;
    const [username, expires] = payload.split('|');
    if (Date.now() > parseInt(expires)) return null;
    return username || null;
  } catch { return null; }
}

// --- GitHub OAuth ---

async function exchangeCode(code) {
  const res = await fetch('https://github.com/login/oauth/access_token', {
    method: 'POST',
    headers: { Accept: 'application/json', 'Content-Type': 'application/json' },
    body: JSON.stringify({ client_id: CLIENT_ID, client_secret: CLIENT_SECRET, code }),
  });
  return (await res.json()).access_token;
}

async function getGithubUsername(accessToken) {
  const res = await fetch('https://api.github.com/user', {
    headers: { Authorization: `Bearer ${accessToken}`, 'User-Agent': 'batterylight' },
  });
  return (await res.json()).login;
}

// --- Router ---

const router = express.Router();

router.get('/login', (req, res) => {
  const returnTo = req.query.return_to || '/';

  if (!IS_AUTH_SERVER) {
    // Delegate to the stable auth server
    return res.redirect(`${AUTH_ORIGIN}/auth/login?return_to=${encodeURIComponent(returnTo)}`);
  }

  const state = Buffer.from(JSON.stringify({ returnTo })).toString('base64url');
  const params = new URLSearchParams({ client_id: CLIENT_ID, scope: 'read:user', state });
  res.redirect(`https://github.com/login/oauth/authorize?${params}`);
});

router.get('/callback', async (req, res) => {
  const { code, state: stateParam } = req.query;
  if (!code) return res.status(400).send('Missing code');

  let returnTo = '/';
  try { returnTo = JSON.parse(Buffer.from(stateParam, 'base64url').toString()).returnTo || '/'; }
  catch { /* use default */ }

  try {
    const accessToken = await exchangeCode(code);
    if (!accessToken) return res.status(401).send('GitHub OAuth failed — invalid code');

    const username = await getGithubUsername(accessToken);
    if (!username) return res.status(401).send('Could not determine GitHub username');

    if (ALLOWED_USERS && !ALLOWED_USERS.has(username)) {
      return res.status(403).send(`Access denied for @${username}`);
    }

    let returnUrl;
    try { returnUrl = new URL(returnTo); } catch { returnUrl = new URL('/', `${req.protocol}://${req.headers.host}`); }

    const thisOrigin = `${req.protocol}://${req.headers.host}`;

    if (returnUrl.origin === thisOrigin || returnTo === '/') {
      setSessionCookie(req, res, username);
      return res.redirect(returnTo === '/' ? '/' : returnUrl.pathname + returnUrl.search);
    }

    // Cross-origin return (PR container): issue a short-lived token
    const tokenUrl = new URL('/auth/token', returnUrl.origin);
    tokenUrl.searchParams.set('token', makeToken(username));
    res.redirect(tokenUrl.toString());
  } catch (err) {
    console.error('Auth callback error:', err);
    res.status(500).send('Authentication error');
  }
});

// PR containers land here after the auth server redirects back with a token
router.get('/token', (req, res) => {
  const username = parseToken(req.query.token || '');
  if (!username) return res.status(401).send('Invalid or expired token — please try logging in again');

  if (ALLOWED_USERS && !ALLOWED_USERS.has(username)) {
    return res.status(403).send(`Access denied for @${username}`);
  }

  setSessionCookie(req, res, username);
  res.redirect('/');
});

router.get('/logout', (req, res) => {
  clearSessionCookie(req, res);
  res.redirect('/');
});

export { router as authRouter };

// --- Middleware ---

export function requireAuth(req, res, next) {
  if (DEV_NO_AUTH) return next();

  // Always allow auth routes through
  if (req.path.startsWith('/auth/')) return next();

  const username = getSessionUser(req);
  if (!username) {
    // Build the return_to URL for this container
    const returnTo = IS_AUTH_SERVER
      ? req.originalUrl
      : `${req.protocol}://${req.headers.host}/auth/token`;
    return res.redirect(`/auth/login?return_to=${encodeURIComponent(returnTo)}`);
  }

  if (ALLOWED_USERS && !ALLOWED_USERS.has(username)) {
    return res.status(403).send('Access denied');
  }

  next();
}

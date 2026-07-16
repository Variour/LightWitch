import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';
import express from 'express';

process.env.SESSION_SECRET = 'test-session-secret';
process.env.TOKEN_SECRET = 'test-token-secret';
process.env.GITHUB_CLIENT_ID = 'test-client-id';
process.env.GITHUB_CLIENT_SECRET = 'test-client-secret';
process.env.ALLOWED_GITHUB_USERS = 'alice';

const { authRouter, requireAuth, getSessionUser } = await import('./auth.js');

const realFetch = globalThis.fetch;

// Stubs the two outbound GitHub API calls made during /auth/callback,
// passing through anything else (i.e. our own test requests) to the real fetch.
function withGithubUser(login, fn) {
  globalThis.fetch = async (url, opts) => {
    const href = typeof url === 'string' ? url : url.href;
    if (href === 'https://github.com/login/oauth/access_token') {
      return new Response(JSON.stringify({ access_token: 'mock-access-token' }), { status: 200 });
    }
    if (href === 'https://api.github.com/user') {
      return new Response(JSON.stringify({ login }), { status: 200 });
    }
    return realFetch(url, opts);
  };
  return fn().finally(() => { globalThis.fetch = realFetch; });
}

function stateFor(returnTo) {
  return Buffer.from(JSON.stringify({ returnTo })).toString('base64url');
}

function sessionCookieFrom(res) {
  const setCookie = res.headers.get('set-cookie');
  const match = setCookie && setCookie.match(/bl_session=([^;]*)/);
  return match ? match[1] : null;
}

let server, baseUrl;

before(() => new Promise(resolve => {
  const app = express();
  app.use('/auth', authRouter);
  app.use(requireAuth);
  app.get('/protected', (req, res) => res.json({ user: getSessionUser(req) }));
  server = app.listen(0, () => {
    baseUrl = `http://localhost:${server.address().port}`;
    resolve();
  });
}));

after(() => new Promise(resolve => server.close(resolve)));

describe('requireAuth', () => {
  test('redirects unauthenticated requests to /auth/login', async () => {
    const res = await fetch(`${baseUrl}/protected`, { redirect: 'manual' });
    assert.equal(res.status, 302);
    assert.match(res.headers.get('location'), /^\/auth\/login\?return_to=/);
  });
});

describe('GET /auth/login', () => {
  test('redirects to GitHub OAuth with the client id', async () => {
    const res = await fetch(`${baseUrl}/auth/login`, { redirect: 'manual' });
    assert.equal(res.status, 302);
    const location = new URL(res.headers.get('location'));
    assert.equal(location.origin + location.pathname, 'https://github.com/login/oauth/authorize');
    assert.equal(location.searchParams.get('client_id'), 'test-client-id');
  });
});

describe('GET /auth/callback', () => {
  test('sets a session cookie and redirects home for an allowed user', () => withGithubUser('alice', async () => {
    const res = await fetch(`${baseUrl}/auth/callback?code=abc&state=${stateFor('/')}`, { redirect: 'manual' });
    assert.equal(res.status, 302);
    assert.equal(res.headers.get('location'), '/');
    const cookie = sessionCookieFrom(res);
    assert.ok(cookie, 'expected a bl_session cookie to be set');

    const protectedRes = await fetch(`${baseUrl}/protected`, {
      headers: { cookie: `bl_session=${cookie}` },
    });
    assert.equal(protectedRes.status, 200);
    assert.deepEqual(await protectedRes.json(), { user: 'alice' });
  }));

  test('rejects a user not in ALLOWED_GITHUB_USERS', () => withGithubUser('mallory', async () => {
    const res = await fetch(`${baseUrl}/auth/callback?code=abc&state=${stateFor('/')}`, { redirect: 'manual' });
    assert.equal(res.status, 403);
  }));

  test('issues a cross-origin token when return_to is a different origin', () => withGithubUser('alice', async () => {
    const returnTo = 'http://pr-123.example.test/dashboard';
    const res = await fetch(`${baseUrl}/auth/callback?code=abc&state=${stateFor(returnTo)}`, { redirect: 'manual' });
    assert.equal(res.status, 302);
    const location = new URL(res.headers.get('location'));
    assert.equal(location.origin, 'http://pr-123.example.test');
    assert.equal(location.pathname, '/auth/token');
    const token = location.searchParams.get('token');
    assert.ok(token);

    // The PR container redeems the token against its own /auth/token endpoint.
    const tokenRes = await fetch(`${baseUrl}/auth/token?token=${token}`, { redirect: 'manual' });
    assert.equal(tokenRes.status, 302);
    assert.equal(tokenRes.headers.get('location'), '/');
    assert.ok(sessionCookieFrom(tokenRes));
  }));
});

describe('GET /auth/token', () => {
  test('rejects an invalid token', async () => {
    const res = await fetch(`${baseUrl}/auth/token?token=not-a-real-token`);
    assert.equal(res.status, 401);
  });
});

describe('session cookie integrity', () => {
  test('rejects a tampered session cookie', async () => {
    const res = await fetch(`${baseUrl}/protected`, {
      headers: { cookie: 'bl_session=alice|9999999999999|forged-signature' },
      redirect: 'manual',
    });
    assert.equal(res.status, 302);
    assert.match(res.headers.get('location'), /^\/auth\/login\?return_to=/);
  });
});

describe('GET /auth/logout', () => {
  test('clears the session cookie', () => withGithubUser('alice', async () => {
    const callbackRes = await fetch(`${baseUrl}/auth/callback?code=abc&state=${stateFor('/')}`, { redirect: 'manual' });
    const cookie = sessionCookieFrom(callbackRes);

    const res = await fetch(`${baseUrl}/auth/logout`, {
      headers: { cookie: `bl_session=${cookie}` },
      redirect: 'manual',
    });
    assert.equal(res.status, 302);
    assert.match(res.headers.get('set-cookie'), /bl_session=;.*Max-Age=0/);
  }));
});

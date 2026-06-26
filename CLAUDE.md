never regenerate the index.html.gz

## Commit Messages

Use [scoped commits](https://scopedcommits.com/): `scope: description`

- **scope** — the subsystem or area the commit touches, e.g. `web`, `mqtt`, `ota`, `peers`, `ci`, `config`
- **description** — a short summary of the change

Examples: `web: add scene editor`, `mqtt: handle reconnect on timeout`, `ci: bump platformio version`

If a commit spans multiple scopes, use a more general scope, list both separated by a comma, or use `treewide`.

## General guidance
- Never make assumptions, ask to clarify
- Challenge requests if necessary, be critical
- Never assume default states

## Working on Issues

Use `/issue <id>` to start work on an issue and `/open-pr` when ready to open a pull request.

## UI Validation

After implementing any change that touches `data/` or `server/`, validate the UI with Playwright before marking the task complete. Use the `/run` skill, which handles all the mechanics:

- Start the mock server: `DEV_NO_AUTH=true node server/index.js` (server exits immediately without this flag)
- Playwright Chromium: `executablePath: '/opt/pw-browsers/chromium'`
- UI base URL: `http://localhost:8080`

Validate **what the current change affects** — not a fixed list. Take screenshots and report what you see.

## Feature Implementation Checklist

When implementing any new feature or API change, verify all of the following:

1. **API parity** — every new or changed endpoint in `src/web/WebServer.h` must have a matching route in `server/index.js` with the same HTTP method, path, request body shape, and response shape
2. **Config/group schema parity** — if `GET /api/config` or group objects gain a field on the device side, add it to `MOCK_CONFIG` (and its `groups` array) in `server/index.js`
3. **WebSocket event parity** — if the device broadcasts a new WS event type (`_pushPeers`, `_pushGroups`, `_pushLog`, etc.), the mock's `wss.on('connection', …)` handler must emit a representative version of that event
4. **Peer/self field parity** — if `_buildPeersJson` adds or changes a field, update `MOCK_SELF` and `MOCK_PEERS` in `server/index.js` accordingly
5. **Mock data breadth** — mock data must exercise whatever the new feature depends on (e.g. add a second group to `MOCK_CONFIG` if the feature involves group switching or peer assignment)

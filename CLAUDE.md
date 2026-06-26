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
- Before starting with the implementation think through the request, identify any issues
- Never assume default states

## Release Notes

Firmware releases use GitHub's auto-generated release notes, driven by PR labels. Every PR must have exactly one of these labels:

- `enhancement` or `feature` — new functionality
- `bug` or `fix` — bug fixes
- `dependencies` — dependency updates (Dependabot applies this automatically)
- `skip-changelog` — changes that should not appear in release notes (e.g. CI tweaks, docs, dev tooling fixes)

Use the tool `github: issue write` to add the label

## Working on Issues

- When working on an issue, always create a feature branch
- Once done implementing the changes open a pull request into main
- Never merge pull requests unless specifically asked
- Add test checklists to PRs
- when updating a branch with an open pr, review the test checklist and update it if necessary
- if an issue contains research finish that first and confirm with the user, before making any write actions

## Feature Implementation Checklist

When implementing any new feature or API change, verify all of the following:

1. **API parity** — every new or changed endpoint in `src/web/WebServer.h` must have a matching route in `server/index.js` with the same HTTP method, path, request body shape, and response shape
2. **Config/group schema parity** — if `GET /api/config` or group objects gain a field on the device side, add it to `MOCK_CONFIG` (and its `groups` array) in `server/index.js`
3. **WebSocket event parity** — if the device broadcasts a new WS event type (`_pushPeers`, `_pushGroups`, `_pushLog`, etc.), the mock's `wss.on('connection', …)` handler must emit a representative version of that event
4. **Peer/self field parity** — if `_buildPeersJson` adds or changes a field, update `MOCK_SELF` and `MOCK_PEERS` in `server/index.js` accordingly
5. **Mock data breadth** — mock data must exercise whatever the new feature depends on (e.g. add a second group to `MOCK_CONFIG` if the feature involves group switching or peer assignment)

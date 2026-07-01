Verify all of the following before marking a feature or API change complete:

1. **API parity** — every new or changed endpoint in `src/web/WebServer.h` must have a matching route in `server/index.js` with the same HTTP method, path, request body shape, and response shape
2. **Config/group schema parity** — if `GET /api/config` or group objects gain a field on the device side, add it to `MOCK_CONFIG` (and its `groups` array) in `server/index.js`
3. **WebSocket event parity** — if the device broadcasts a new WS event type (`_pushPeers`, `_pushGroups`, `_pushLog`, etc.), the mock's `wss.on('connection', …)` handler must emit a representative version of that event
4. **Peer/self field parity** — if `_buildPeersJson` adds or changes a field, update `MOCK_SELF` and `MOCK_PEERS` in `server/index.js` accordingly
5. **Mock data breadth** — mock data must exercise whatever the new feature depends on (e.g. add a second group to `MOCK_CONFIG` if the feature involves group switching or peer assignment)

Run `/validate-ui` after completing the checklist if any `data/` or `server/` files were changed, keeping the check focused on the affected UI flow.

Use `/grill-me` to stress-test the implementation before opening a PR.

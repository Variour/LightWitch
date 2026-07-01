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

After any change to `data/` or `server/`, run `/validate-ui` with a targeted check of the affected UI flow only.

## Feature Implementation

After implementing a feature or API change, run `/feature-checklist`.

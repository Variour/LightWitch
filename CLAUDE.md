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
- Surface all missing tools, dependencies, commands, or capabilities you encounter. Do not silently work around them, and do not stop after reporting only the first missing item when more are known.
- When starting work and the worktree is clean, automatically switch to `main` and fast-forward from `origin/main` without asking. Only stop to ask if the worktree is dirty or the switch/update cannot be done safely.


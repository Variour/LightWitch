never regenerate the index.html.gz

## GitHub content

All content on GitHub — issues, pull requests, comments, commit messages, and labels — is always written in English.

## Commit Messages

Use [scoped commits](https://scopedcommits.com/): `scope: description`

- **scope** — the subsystem or area the commit touches, e.g. `web`, `mqtt`, `ota`, `peers`, `ci`, `config`
- **description** — a short summary of the change

Examples: `web: add scene editor`, `mqtt: handle reconnect on timeout`, `ci: bump platformio version`

If a commit spans multiple scopes, use a more general scope, list both separated by a comma, or use `treewide`.

## General guidance
- Never make assumptions, ask to clarify
- Challenge requests if necessary, be critical
- If a simpler approach exists, say so. Push back when warranted.
- No features beyond what was asked
- No "flexibility" or "configurability" that wasn't requested.
- Never assume default states
- Surface all missing tools, dependencies, commands, or capabilities you encounter. Do not silently work around them, and do not stop after reporting only the first missing item when more are known.
- After changing `src/`, build each affected environment on its own (`pio run -e esp32c5`, `pio run -e esp32c3`, ...) and report errors, new warnings and the `[app-size]` line. Build `esp32c3` whenever the change reaches it: its app slot has the least room. Run `pio run` without `-e`, uploads (`-t upload`, `-t uploadfs`, `upload_all`) and OTA only when the user asks.
- When starting work and the worktree is clean, automatically switch to `main` and fast-forward from `origin/main` without asking. Only stop to ask if the worktree is dirty or the switch/update cannot be done safely.
- In docs, comments, and commit messages: state what to do, not what happens if you don't. Avoid spelling out failure modes, error messages, or negative-case explanations unless the user asked for troubleshooting info.


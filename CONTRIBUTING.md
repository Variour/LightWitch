# Contributing

## Workflow

1. Fork the repository and create a branch from `main`.
2. Make your change.
3. Open a pull request against `main`.

## Commit messages

Use [scoped commits](https://scopedcommits.com/): `scope: description`, e.g. `web: add scene editor`, `mqtt: handle reconnect on timeout`.

Commonly used scopes:

| Scope | Use for |
|---|---|
| `web` | UI and web server |
| `ci` | GitHub Actions workflows |
| `config` | Settings and config storage |
| `mesh` | Mesh sync between devices |
| `ota` | Over-the-air firmware updates |
| `docs` | Documentation |
| `wifi` | WiFi connectivity |
| `sound` | Sound-reactive features |
| `mqtt` | MQTT integration |
| `treewide` | Changes that span multiple scopes |

If a commit doesn't fit an existing scope, use the subsystem it touches.

## Local checks

```bash
npm ci
npm run lint
npm test
```

C++ sources are formatted with `clang-format`; check with:

```bash
find src \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 clang-format --dry-run --Werror
```

## Building and flashing

See [docs/development.md](docs/development.md).

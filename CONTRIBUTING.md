# Contributing

## Workflow

1. Fork the repository and create a branch from `main`.
2. Make your change.
3. Open a pull request against `main`.

## Commit messages

Use [scoped commits](https://scopedcommits.com/): `scope: description`, e.g. `web: add scene editor`, `mqtt: handle reconnect on timeout`.

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

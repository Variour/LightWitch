Validate only the UI behavior touched by the current change.

## Rules

- Do the smallest useful check, not a broad regression pass.
- Cover only the changed UI flow(s), ideally 1–2 happy-path interactions.
- Prefer quick assertions over screenshots; take screenshots only when they add value.
- Reuse the repo helpers instead of writing setup boilerplate from scratch.

## Steps

1. Start the mock server: `npm run dev:no-auth`
2. UI base URL: `http://127.0.0.1:8080`
3. Use a tiny Playwright script that imports `./scripts/ui/helpers.mjs`
4. Exercise only the affected feature(s), then stop
5. Report exactly what you checked and what you intentionally did not check

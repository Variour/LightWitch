---
name: validate-ui
description: Launch the batterylight mock server and use Playwright to validate only the changed UI flow.
---

Validate only the UI behavior affected by the current change.

## Principles

- Do the lightest useful check, not a full UI tour.
- Focus on the changed flow(s), usually 1–2 happy-path interactions.
- Reuse the repo helpers in `scripts/ui/helpers.mjs`.
- Prefer assertions over screenshots; capture screenshots only when they help explain the result.

## Start the mock server

```bash
npm run dev:no-auth > /tmp/batterylight-ui.log 2>&1 &
SERVER_PID=$!
for i in $(seq 1 20); do curl -sf http://127.0.0.1:8080/ > /dev/null && break; sleep 0.5; done
```

## Validate with Playwright

Run a tiny inline script with `node --input-type=module` and import the repo helper:

```js
import { launchUi, saveScreenshot } from './scripts/ui/helpers.mjs';

const { browser, page } = await launchUi();

// Navigate only to the area changed by this work.
// Add a couple of assertions or interactions for that flow.
// Call saveScreenshot(page, 'name') only when useful.

await browser.close();
```

Do not run a fixed regression checklist. Stop once the changed behavior has been exercised with enough confidence.

## Clean up

```bash
kill $SERVER_PID 2>/dev/null
pkill -f "node server/index.js" 2>/dev/null
```

Report what you checked, what you skipped, and any issues you noticed.

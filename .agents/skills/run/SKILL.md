---
name: run
description: Launch the batterylight mock server and use Playwright to validate the UI. Use after implementing any change that touches data/ or server/.
---

Validate the UI after a change. The server requires `DEV_NO_AUTH=true` or it will exit immediately.

## Start the mock server

```bash
cd /home/user/batteryLight
npm install --silent 2>/dev/null
DEV_NO_AUTH=true node server/index.js &
SERVER_PID=$!
# Poll until ready (up to 10 s)
for i in $(seq 1 20); do curl -sf http://localhost:8080/ > /dev/null && break; sleep 0.5; done
```

## Validate with Playwright

Write a script to the scratchpad directory and run it with `node --input-type=module`:

```js
import { chromium } from 'playwright';
const browser = await chromium.launch({
  executablePath: '/opt/pw-browsers/chromium',
  headless: true,
});
const page = await (await browser.newContext()).newPage();
await page.goto('http://localhost:8080/');
// Navigate to whatever pages are relevant to the current change.
// Take screenshots with page.screenshot({ path: '...', fullPage: true }).
// Interact with the UI to exercise the changed feature.
await browser.close();
```

If `playwright` is not resolvable, run `npm install playwright` first (it uses the pre-installed browser — no download needed).

Decide what to navigate to and what to check **based on what was just changed** — do not run through a fixed list.

## Clean up

```bash
kill $SERVER_PID 2>/dev/null; pkill -f "node server/index.js" 2>/dev/null
```

Report findings with screenshots. Flag anything that looks wrong.

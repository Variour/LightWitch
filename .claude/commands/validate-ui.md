Validate the UI with Playwright after changes to `data/` or `server/`.

## Steps

1. Start the mock server: `DEV_NO_AUTH=true node server/index.js`
2. Launch Playwright Chromium with `executablePath: '/opt/pw-browsers/chromium'`
3. UI base URL: `http://localhost:8080`
4. Validate what the current change affects — not a fixed list
5. Take screenshots and report what you see

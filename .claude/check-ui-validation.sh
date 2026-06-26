#!/bin/bash
# Stop hook: block if data/ has uncommitted changes, reminding Claude to validate first.
REPO="$(dirname "$0")/.."
if git -C "$REPO" diff --name-only 2>/dev/null | grep -q '^data/'; then
  printf '{"decision":"block","reason":"data/ has uncommitted changes. Run the /run skill to validate the UI with Playwright (DEV_NO_AUTH=true node server/index.js) before marking this complete."}\n'
fi

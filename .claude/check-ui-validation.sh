#!/bin/bash
# Stop hook: block if data/ or server/ has uncommitted changes, reminding Claude to validate first.
REPO="$(dirname "$0")/.."
if git -C "$REPO" diff --name-only HEAD -- data/ server/ 2>/dev/null | grep -Eq '^(data|server)/'; then
  printf '{"decision":"block","reason":"data/ or server/ has uncommitted changes. Run /validate-ui with a targeted Playwright check of the affected UI flow before marking this complete."}\n'
fi

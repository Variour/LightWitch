Explain the development workflow and what to do next based on the current state of the repository.

## Workflow overview

1. `/issue <id>` — start work on an issue: reads it, creates a branch, confirms approach
2. *(implement the changes)*
3. `/validate-ui` — validate the UI with Playwright if `data/` or `server/` was touched
4. `/feature-checklist` — verify API/config/WS/mock parity for any feature or API change
5. `/open-pr` — open a pull request with the correct label and test checklist

Use `/grill-me` at any point to stress-test a plan or implementation.

## What to do next

Look at the current git state and working directory, then tell the user which step they are at and what to do next:

- No branch / on main → suggest `/issue <id>`
- On a feature branch with uncommitted changes → suggest committing, then `/validate-ui` or `/feature-checklist` depending on what changed
- On a feature branch, changes committed, no open PR → suggest `/validate-ui` and `/feature-checklist` if not yet done, then `/open-pr`
- Open PR exists → remind about the test checklist and suggest pushing any remaining fixes

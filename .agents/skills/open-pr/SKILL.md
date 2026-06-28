---
name: open-pr
description: Validate approved work, commit and push the branch, then open or update a pull request into main.
disable-model-invocation: true
compatibility: Requires a GitHub repository checkout and GitHub CLI (`gh`) authenticated with access to the repo.
---

# Validation and Pull Request Workflow

Use this after the implementation has been reviewed and the user wants to ship it.

## Required behavior

1. Confirm the work is approved.
   - If the user still wants implementation changes, stop and return to implementation.

2. Follow repository shipping rules.
   - Do not assume missing details such as an issue number or branch naming scheme.

3. Validate before publishing.
   - Run `/feature-checklist` for feature or API changes.
   - Run `/validate-ui` if any `data/` or `server/` files changed.
   - If validation fails, stop and report it unless the user explicitly wants to continue anyway.

4. Publish the branch.
   - Use the required commit-message format.
   - Commit and push the approved changes.

5. Open or update the pull request.
   - Reuse an existing PR for the branch if there is one.
   - Otherwise open a new PR targeting `main`.
   - Keep the title short.
   - Include a bullet summary and reviewer-facing test checklist.
   - If the related issue number is known, include a closing reference. Do not invent one.

6. Apply exactly one PR label.
   - `enhancement` — new functionality
   - `bug` — bug fixes
   - `dependencies` — dependency updates
   - `skip-changelog` — CI tweaks, docs, dev tooling

7. Report back with the validation performed, commit, branch, and PR URL.
   - Never merge the PR unless explicitly asked.

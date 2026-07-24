---
name: open-pr
description: Validate approved work, commit and push the branch, then open or update a pull request into main.
disable-model-invocation: true
compatibility: Requires a GitHub repository checkout and GitHub CLI (`gh`) authenticated with access to the repo.
---

# Validation and Pull Request Workflow

Use this after the implementation has been reviewed and the user wants to ship it.

## Required behavior

1. Follow repository shipping rules.
   - Do not assume missing details such as an issue number or branch naming scheme.

2. Validate before publishing.
   - Run `/feature-checklist` for feature or API changes.
   - If any `.cpp`/`.h` files under `src/` changed, format only those files and confirm the check CI runs would pass:
     ```bash
     FILES=$(git diff --name-only --diff-filter=ACMR main...HEAD -- src | grep -E '\.(cpp|h)$')
     [ -n "$FILES" ] && clang-format -i $FILES
     [ -n "$FILES" ] && clang-format --dry-run --Werror $FILES
     ```
     Do not run this across all of `src/` — only the files this change touches.

3. Publish the branch.
   - Create a branch name from issue id if available and a concise description.
   - Use the required commit-message format.
   - Commit and push the approved changes.

4. Open or update the pull request.
   - Reuse an existing PR for the branch if there is one.
   - Otherwise open a new PR targeting `main`.
   - Keep the title short.
   - Include a bullet summary and reviewer-facing test checklist.
   - If the related issue number is known, include a closing reference. Do not invent one.

5. Apply exactly one PR label.
   - `enhancement` — new functionality, must be user facing
   - `bug` — bug fixes
   - `dependencies` — dependency updates
   - `security` — security fixes or hardening
   - `performance` — performance improvements
   - `skip-changelog` — CI tweaks, docs, dev tooling

6. Report back with the validation performed, commit, branch, and PR URL.
   - Never merge the PR unless explicitly asked.

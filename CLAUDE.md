never regenerate the index.html.gz

## General guidance
- Never make assumptions, ask to clarify
- Challenge requests if necessary, be critical
- Before starting with the implementation think through the request, identify any issues
- Never assume default states

## Release Notes

Firmware releases use GitHub's auto-generated release notes, driven by PR labels. Every PR should have exactly one of these labels:

- `enhancement` or `feature` — new functionality
- `bug` or `fix` — bug fixes
- `dependencies` — dependency updates (Dependabot applies this automatically)
- `skip-changelog` — changes that should not appear in release notes (e.g. CI tweaks, docs, dev tooling fixes)

PRs without a label will appear under "Other Changes". Label before merging, not after.

## Working on Issues

- When working on an issue, always create a feature branch
- Once done implementing the changes open a pull request into main
- Never merge pull requests unless specifically asked
- Add test checklists to PRs
- when updating a branch with an open pr, review the test checklist and update it if necessary
- if an issue contains research finish that first and confirm with the user, before making any write actions

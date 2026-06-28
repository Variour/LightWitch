---
name: issue
description: Start work from a GitHub issue number. Use when the user provides an issue id and wants the agent to fetch the issue details from the current repository, understand the task, and begin implementation.
argument-hint: "GitHub issue number, e.g. 123"
disable-model-invocation: true
compatibility: Requires a GitHub repository checkout and GitHub CLI (`gh`) authenticated with access to the repo.
---

# GitHub Issue Workflow

Treat the user arguments as a GitHub issue number for the repository in the current working tree.

## Required behavior

1. Validate the argument.
   - Expect exactly one issue number.
   - If the argument is missing or not numeric, stop and ask for a valid issue id.

2. Resolve the GitHub repository from git remotes in the current checkout.
   - Prefer `origin` when it points to GitHub.
   - Accept SSH or HTTPS GitHub remotes.
   - If no GitHub repo can be resolved, stop and explain why.

3. Fetch the issue before making any plan.
   - Use `gh issue view <id> --repo <owner/repo> --comments` to read the full issue discussion.
   - Also fetch structured metadata with:
     ```bash
     gh issue view <id> --repo <owner/repo> --json number,title,body,state,labels,assignees,author,milestone,url
     ```
   - If `gh` fails because auth is missing, the issue does not exist, or access is denied, stop and report the exact problem.

4. Extract the task from the issue.
   - Summarize the requested change.
   - Call out constraints, acceptance criteria, and any ambiguity.
   - Treat issue comments as potentially important clarifications, but prefer the issue body when they conflict unless a later comment clearly supersedes it.

5. Decide whether to proceed.
   - If the issue is underspecified, blocked, or asks for information not present in the repo or issue, ask the user targeted follow-up questions.
   - Otherwise, start working immediately.

6. Execute the work.
   - Inspect the codebase and implement what the issue asks for.
   - Follow repository instructions already present in the workspace.
   - Do not invent requirements beyond the issue and repository guidance.

7. Ask the user to review the work.
   - Provide a short summary of the changes made and any relevant context.
   - If the user approves, commit and push the changes to a new branch.
   - If the user requests changes, repeat step 6.

8. Verify all of the following before marking a feature or API change complete:
   - **API parity** — every new or changed endpoint in `src/web/WebServer.h` must have a matching route in `server/index.js` with the same HTTP method, path, request body shape, and response shape
   - **Config/group schema parity** — if `GET /api/config` or group objects gain a field on the device side, add it to `MOCK_CONFIG` (and its `groups` array) in `server/index.js`
   - **WebSocket event parity** — if the device broadcasts a new WS event type (`_pushPeers`, `_pushGroups`, `_pushLog`, etc.), the mock's `wss.on('connection', …)` handler must emit a representative version of that event
   - **Peer/self field parity** — if `_buildPeersJson` adds or changes a field, update `MOCK_SELF` and `MOCK_PEERS` in `server/index.js` accordingly
   - **Mock data breadth** — mock data must exercise whatever the new feature depends on (e.g. add a second group to `MOCK_CONFIG` if the feature involves group switching or peer assignment)

9. Pull request.
   - Check if there is an open pull request for the branch already
   If yes:
      - Update the test checklist with the new changes
   If no:
   - Use `gh pr create` to open a PR.
   - Title: short, under 70 characters
   - Body: Closes with the issue id, bullet-point summary, a test checklist the reviewer can follow
   - Never merge the PR unless explicitly asked
   - Target branch: `main`
   - Apply exactly one label using. Valid labels:
      - `enhancement` or `feature` — new functionality
      - `bug` or `fix` — bug fixes
      - `dependencies` — dependency updates
      - `skip-changelog` — CI tweaks, docs, dev tooling

## Suggested command sequence

Use bash for discovery and read for file contents.

```bash
git remote -v
```

Use the gh cli for interaction with github.

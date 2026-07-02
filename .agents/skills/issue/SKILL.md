---
name: issue
description: Start work from a GitHub issue number. Use when the user provides an issue id and wants the agent to fetch the issue details from the current repository, understand the task, and begin implementation.
argument-hint: "GitHub issue number followed by optional instructions, e.g. 123 no implementation yet"
disable-model-invocation: true
compatibility: Requires a GitHub repository checkout and GitHub CLI (`gh`) authenticated with access to the repo.
---

# GitHub Issue Workflow

Treat the user arguments as a GitHub issue number for the repository in the current working tree, followed by optional extra instructions for how to handle the issue.

## Required behavior

1. Validate and parse the argument.
   - Expect the first whitespace-delimited token to be the issue number.
   - Treat any remaining text after the first token as additional user instructions that must be considered while working on the issue.
   - If the first token is missing or not numeric, stop and ask for a valid issue id.

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
   - Consider both the issue content and any additional user instructions that followed the issue number.
   - If the issue is underspecified, blocked, or asks for information not present in the repo or issue, ask the user targeted follow-up questions.
   - Otherwise, start working immediately.

6. Prepare the repository for implementation.
   - Check that the worktree is clean before starting. If there are local changes, stop and ask the user what to do.
   - If the worktree is clean, automatically switch to `main` without asking the user.
   - Fast-forward local `main` from `origin/main` before implementation begins.
   - Only stop and ask if checkout, fetch, or fast-forward cannot be done safely.

7. Execute the work.
   - Inspect the codebase and implement what the issue asks for.
   - Follow repository instructions already present in the workspace.
   - Consider any additional user instructions that followed the issue number, as long as they do not conflict with the issue, repository guidance, or system/developer instructions.
   - Do not invent requirements beyond the issue, the user's explicit follow-up instructions, and repository guidance.

8. Ask the user to review the work.
   - Provide a short summary of the changes made and any relevant context.
   - Call out any risks, follow-up work, or ambiguity that still needs a decision.
   - If the user requests changes, repeat step 7.
   - If the user approves and wants to ship the work, hand off to `/open-pr`.

Do not validate, commit, push, or open/update a pull request as part of this skill.

## Suggested command sequence

Use bash for discovery and read for file contents.

```bash
git remote -v
```

Use the gh cli for interaction with github.

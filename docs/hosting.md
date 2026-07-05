# Online hosting (Azure Container Apps)

This hosts the **mock web UI** (the same `server/index.js` used for [local development](development.md#local-web-ui-development)) as a Docker container on Azure Container Apps — not a connection to real hardware. It exists so PR reviewers and testers can click through the UI without running anything locally. There's a permanent `latest` deployment plus per-PR preview environments.

## Authentication

Access is restricted to specific GitHub accounts via OAuth. The production container handles the OAuth flow; PR preview containers delegate to it, so only one OAuth App registration is needed regardless of how many PR environments exist.

**One-time setup:**

1. **Register a GitHub OAuth App** at github.com/settings/developers
   - Authorization callback URL: `https://<prod-fqdn>/auth/callback`

2. **Add GitHub Actions secrets** (repo → Settings → Secrets → Actions):

   | Secret | Where used | Value |
   |---|---|---|
   | `GITHUB_OAUTH_CLIENT_ID` | Prod container | Client ID from the OAuth App |
   | `GITHUB_OAUTH_CLIENT_SECRET` | Prod container | Client Secret from the OAuth App |
   | `ALLOWED_GITHUB_USERS` | All containers | Comma-separated GitHub usernames, e.g. `alice,bob,charlie` |
   | `AUTH_TOKEN_SECRET` | All containers | Random secret shared across all containers — `openssl rand -base64 32` |
   | `AUTH_SESSION_SECRET` | All containers | Random secret for session cookies — `openssl rand -base64 32` |

   Existing secrets (`AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, `AZURE_SUBSCRIPTION_ID`, `AZURE_RESOURCE_GROUP`, `GHCR_PASSWORD_B64`) are unchanged.

3. **Deploy infrastructure** by running the *Deploy infrastructure* workflow manually (or pushing a change to `infra/`).

## Deployments

| Event | Result |
|---|---|
| Push to `main` | Updates the permanent `batterylight-latest` container app |
| Open / push to a PR | Creates or updates a `batterylight-pr-<N>` container app; URL posted as a PR comment |
| PR closed / merged | PR container app and its registry image are cleaned up on next push to `main` |

## Docker images

Every push to `main` and every pull request builds a Docker image pushed to the GitHub Container Registry. Run any image locally:

```sh
docker run --rm -p 8080:8080 ghcr.io/<owner>/batterylight:latest      # main branch
docker run --rm -p 8080:8080 ghcr.io/<owner>/batterylight:pr-<N>      # specific PR
```

Auth is disabled when running locally (no environment variables set).

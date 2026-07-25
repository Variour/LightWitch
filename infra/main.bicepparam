using './main.bicep'

param location = 'germanywestcentral'
param appName = 'batterylight'
param ghcrPasswordB64          = readEnvironmentVariable('GHCR_PASSWORD_B64')
param githubOauthClientId      = readEnvironmentVariable('GITHUB_OAUTH_CLIENT_ID')
param githubOauthClientSecret  = readEnvironmentVariable('GITHUB_OAUTH_CLIENT_SECRET')
param allowedGithubUsers       = readEnvironmentVariable('ALLOWED_GITHUB_USERS')
param authTokenSecret          = readEnvironmentVariable('AUTH_TOKEN_SECRET')
param authSessionSecret        = readEnvironmentVariable('AUTH_SESSION_SECRET')

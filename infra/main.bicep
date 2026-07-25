@description('Azure region for all resources')
param location string = resourceGroup().location

@description('Name prefix used for all resources')
param appName string = 'batterylight'

@description('Container image for the latest build')
param latestImage string = 'ghcr.io/variour/lightwitch:latest'

@description('ghcr.io pull secret (base64-encoded Docker config JSON)')
@secure()
param ghcrPasswordB64 string

@description('GitHub OAuth App client ID')
param githubOauthClientId string

@description('GitHub OAuth App client secret')
@secure()
param githubOauthClientSecret string

@description('Comma-separated list of allowed GitHub usernames')
param allowedGithubUsers string

@description('Shared HMAC secret for cross-container tokens')
@secure()
param authTokenSecret string

@description('HMAC secret for session cookies')
@secure()
param authSessionSecret string

// Container App Environment (no Log Analytics, no VNet)
resource env 'Microsoft.App/managedEnvironments@2024-03-01' = {
  name: '${appName}-env'
  location: location
  properties: {}
}

// Permanent app for the latest main build
resource latestApp 'Microsoft.App/containerApps@2024-03-01' = {
  name: '${appName}-latest'
  location: location
  properties: {
    environmentId: env.id
    configuration: {
      activeRevisionsMode: 'Single'
      ingress: {
        external: true
        targetPort: 8080
        transport: 'http'
      }
      registries: [
        {
          server: 'ghcr.io'
          username: 'variour'
          passwordSecretRef: 'ghcr-password'
        }
      ]
      secrets: [
        { name: 'ghcr-password',              value: ghcrPasswordB64 }
        { name: 'github-oauth-client-secret', value: githubOauthClientSecret }
        { name: 'auth-token-secret',          value: authTokenSecret }
        { name: 'auth-session-secret',        value: authSessionSecret }
      ]
    }
    template: {
      containers: [
        {
          name: appName
          image: latestImage
          resources: {
            cpu: json('0.25')
            memory: '0.5Gi'
          }
          env: [
            { name: 'GITHUB_CLIENT_ID',      value: githubOauthClientId }
            { name: 'GITHUB_CLIENT_SECRET',  secretRef: 'github-oauth-client-secret' }
            { name: 'ALLOWED_GITHUB_USERS',  value: allowedGithubUsers }
            { name: 'TOKEN_SECRET',          secretRef: 'auth-token-secret' }
            { name: 'SESSION_SECRET',        secretRef: 'auth-session-secret' }
          ]
        }
      ]
      scale: {
        minReplicas: 0
        maxReplicas: 3
      }
    }
  }
}

output environmentId string = env.id
output environmentName string = env.name
output latestAppFqdn string = latestApp.properties.configuration.ingress.fqdn

@description('Azure region for all resources')
param location string = resourceGroup().location

@description('Name prefix used for all resources')
param appName string = 'batterylight'

@description('Container image for the latest build')
param latestImage string = 'ghcr.io/variour/batterylight:latest'

@description('ghcr.io pull secret (base64-encoded Docker config JSON)')
@secure()
param ghcrPasswordB64 string

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
        {
          name: 'ghcr-password'
          value: ghcrPasswordB64
        }
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

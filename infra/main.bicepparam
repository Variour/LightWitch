using './main.bicep'

param location = 'germanywestcentral'
param appName = 'batterylight'
param ghcrPasswordB64 = readEnvironmentVariable('GHCR_PASSWORD_B64')

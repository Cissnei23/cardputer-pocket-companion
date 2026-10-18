$ErrorActionPreference = 'Stop'
$env:BRIDGE_HOST = '0.0.0.0'
$env:BRIDGE_PORT = '8765'
. (Join-Path $PSScriptRoot '.bridge-token.ps1')
Set-Location -LiteralPath $PSScriptRoot
node.exe server.js


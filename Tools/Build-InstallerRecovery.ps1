$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
& (Join-Path $root 'Build.ps1') -SkipInstallers
Write-Host ('Recovery programs: '+(Join-Path $root 'bin\InstallerRecovery'))

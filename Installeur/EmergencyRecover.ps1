. (Join-Path $PSScriptRoot 'Common.ps1')
& (Join-Path $PSScriptRoot 'Uninstall.ps1') -Emergency
exit $LASTEXITCODE

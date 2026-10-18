$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
& (Join-Path $root 'Build.ps1') -SkipManaged
$out=Join-Path $root 'bin\ExampleUIBridge'
New-Item -ItemType Directory -Path $out -Force|Out-Null
Copy-Item -LiteralPath (Join-Path $root 'bin\Native\InterceptorViewerBridge.dll') -Destination $out -Force
Write-Host ('Native viewer bridge: '+(Join-Path $out 'InterceptorViewerBridge.dll'))

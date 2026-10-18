param([string]$OutputDirectory='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
& (Join-Path $root 'Build.ps1') -SkipManaged
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'bin\DiagnosticWorker149'}
New-Item -ItemType Directory -Path $OutputDirectory -Force|Out-Null
Copy-Item -LiteralPath (Join-Path $root 'bin\Native\InterceptorDiagnosticWorker.exe') -Destination $OutputDirectory -Force
Write-Host ('Diagnostic worker: '+(Join-Path $OutputDirectory 'InterceptorDiagnosticWorker.exe'))

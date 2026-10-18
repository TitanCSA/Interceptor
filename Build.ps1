param(
    [switch]$SkipInstallers,
    [switch]$SkipManaged,
    [string]$BuildDirectory
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'obj\PublicBuild'}
$BuildDirectory=[IO.Path]::GetFullPath($BuildDirectory)
if(![Environment]::Is64BitProcess){throw 'Run 64-bit Windows PowerShell.'}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if(!(Test-Path -LiteralPath $vswhere)){throw 'Install Visual Studio 2022 or Build Tools with Desktop development with C++ and a Windows SDK.'}
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Visual Studio C++ x64 tools were not found.'}
$cmakeCommand=Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmake=if($cmakeCommand){$cmakeCommand.Source}else{Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'}
if(!(Test-Path -LiteralPath $cmake)){throw 'Install CMake or the Visual Studio C++ CMake tools component.'}

# This tree intentionally contains no kernel sources, WDK projects or signing tools.
& $cmake -S $root -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 ('-DCMAKE_GENERATOR_INSTANCE='+$vs)
if($LASTEXITCODE){throw 'CMake configuration failed.'}
& $cmake --build $BuildDirectory --config Release --parallel
if($LASTEXITCODE){throw 'User-mode C++ compilation failed.'}
$native=Join-Path $root 'bin\Native'
& (Join-Path $native 'InterceptorDiagnosticWorker.exe') --self-test
if($LASTEXITCODE){throw 'Diagnostic worker self-test failed.'}
if($SkipManaged){Write-Host 'Native SDK, examples, helper, bridge and recovery binding built. No installation performed.';return}

$compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if(!(Test-Path -LiteralPath $compiler)){throw 'The Windows .NET Framework C# compiler was not found.'}
$recovery=Join-Path $root 'bin\InstallerRecovery'
New-Item -ItemType Directory -Path $recovery -Force|Out-Null
& $compiler /nologo /target:exe /platform:x64 /optimize+ /warn:4 /warnaserror+ /utf8output /codepage:65001 ('/out:'+(Join-Path $recovery 'InterceptorRecovery.exe')) /reference:System.ServiceProcess.dll /reference:System.Management.dll /reference:System.Web.Extensions.dll (Join-Path $root 'InstallerRecovery\RecoveryService.cs') (Join-Path $root 'InstallerRecovery\DeviceState.cs')
if($LASTEXITCODE){throw 'Recovery service compilation failed.'}
& (Join-Path $recovery 'InterceptorRecovery.exe') --self-test
if($LASTEXITCODE){throw 'Recovery service self-test failed.'}
Copy-Item -LiteralPath (Join-Path $native 'RecoveryBinding.dll') -Destination $recovery -Force
& (Join-Path $root 'Tools\Build-ExampleUI.ps1') -BridgePath (Join-Path $native 'InterceptorViewerBridge.dll') -OutputDirectory (Join-Path $root 'bin\ExampleUI') -NoPublish
if($SkipInstallers){Write-Host 'All public user-mode sources built. Installer packaging skipped; no installation performed.';return}

# Keep the Microsoft-signed kernel and catalogs exactly as distributed. Only the
# newly built user-mode helper and recovery programs are refreshed for packaging.
$payload=Join-Path $root 'Installeur\Payload'
if(!(Test-Path -LiteralPath (Join-Path $payload 'SHA256.json'))){throw 'The signed binary payload is missing. Restore Installeur/Payload from this release before packaging installers.'}
. (Join-Path $root 'Installeur\Common.ps1')
$mode=Assert-InterceptorPayload $payload
if($mode -ne 'Microsoft'){throw 'This public build requires the Microsoft-signed release payload.'}
$kernelBefore=(Get-FileHash -LiteralPath (Join-Path $payload 'Interceptor.sys') -Algorithm SHA256).Hash
Copy-Item -LiteralPath (Join-Path $native 'InterceptorExample.exe') -Destination (Join-Path $payload 'InterceptorExample.exe') -Force
$hashes=Get-Content -LiteralPath (Join-Path $payload 'SHA256.json') -Raw|ConvertFrom-Json
$hashes.'InterceptorExample.exe'=(Get-FileHash -LiteralPath (Join-Path $payload 'InterceptorExample.exe') -Algorithm SHA256).Hash
$hashes|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $payload 'SHA256.json') -Encoding UTF8
$recoveryPayload=Join-Path $root 'Installeur\Recovery'
New-Item -ItemType Directory -Path $recoveryPayload -Force|Out-Null
foreach($name in @('InterceptorRecovery.exe','RecoveryBinding.dll')){Copy-Item -LiteralPath (Join-Path $recovery $name) -Destination $recoveryPayload -Force}
& (Join-Path $root 'Tools\Build-InstallerUI.ps1') -DiagnosticWorkerPath (Join-Path $native 'InterceptorDiagnosticWorker.exe')
$references=Join-Path ${env:ProgramFiles(x86)} 'Reference Assemblies\Microsoft\Framework\.NETFramework\v4.6'
if(!(Test-Path -LiteralPath (Join-Path $references 'System.dll'))){$references=Join-Path $root 'obj\InstallerUI\net46\build\.NETFramework\v4.6'}
& (Join-Path $root 'Tools\Build-InstallerConsole.ps1') -ReferenceDirectory $references
if((Get-FileHash -LiteralPath (Join-Path $payload 'Interceptor.sys') -Algorithm SHA256).Hash -ne $kernelBefore){throw 'Unexpected change to the signed kernel.'}
Write-Host 'Built SDK, eight console examples, UI viewer, UI installer and console installer.'
Write-Host 'All builds and self-tests were user-mode only. No driver installed and no Windows setting changed.'

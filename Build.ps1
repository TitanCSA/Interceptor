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
& $compiler /nologo /target:exe /platform:x64 /optimize+ /warn:4 /warnaserror+ /utf8output /codepage:65001 ('/out:'+(Join-Path $recovery 'InterceptorRecovery.exe')) /reference:System.ServiceProcess.dll /reference:System.Management.dll /reference:System.Web.Extensions.dll (Join-Path $root 'installer\recovery\RecoveryService.cs') (Join-Path $root 'installer\recovery\DeviceState.cs')
if($LASTEXITCODE){throw 'Recovery service compilation failed.'}
& (Join-Path $recovery 'InterceptorRecovery.exe') --self-test
if($LASTEXITCODE){throw 'Recovery service self-test failed.'}
Copy-Item -LiteralPath (Join-Path $native 'RecoveryBinding.dll') -Destination $recovery -Force
& (Join-Path $root 'tools\Build-ExampleUI.ps1') -BridgePath (Join-Path $native 'InterceptorViewerBridge.dll') -OutputDirectory (Join-Path $root 'bin\ExampleUI')
if($SkipInstallers){Write-Host 'All public user-mode sources built. Installer packaging skipped; no installation performed.';return}

# Verify signed source files first. Generated helper/recovery binaries and their
# runtime manifest belong in the build directory, never the source tree.
$engine=Join-Path $root 'installer\engine'
$sourcePayload=Join-Path $engine 'Payload'
$sourceManifest=Join-Path $sourcePayload 'SHA256.json'
if(!(Test-Path -LiteralPath $sourceManifest)){throw 'The signed payload is missing. Restore installer/engine/Payload from this release before packaging installers.'}
$expected=Get-Content -LiteralPath $sourceManifest -Raw|ConvertFrom-Json
$signedFiles=@('Interceptor.sys','InterceptorKeyboard.inf','InterceptorMouse.inf','InterceptorControl.inf','InterceptorKeyboard.cat','InterceptorMouse.cat','InterceptorControl.cat','Signing.json')
if(@($expected.PSObject.Properties).Count -ne $signedFiles.Count){throw 'The signed source manifest must contain exactly the distributed driver files and signing policy.'}
$hashes=[ordered]@{}
foreach($name in $signedFiles){
    $path=Join-Path $sourcePayload $name
    if(!(Test-Path -LiteralPath $path -PathType Leaf) -or $expected.$name -notmatch '^[0-9A-Fa-f]{64}$' -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $expected.$name){throw "Signed source payload missing or modified: $name"}
    $hashes[$name]=[string]$expected.$name
}
$package=Join-Path $BuildDirectory 'package'
$payload=Join-Path $package 'Payload'
$recoveryPayload=Join-Path $package 'Recovery'
New-Item -ItemType Directory -Path $package,$payload,$recoveryPayload -Force|Out-Null
foreach($name in @('Install.ps1','Uninstall.ps1','Associate.ps1','CompleteAssociation.ps1','Common.ps1','Lifecycle.ps1','EmergencyRecover.ps1','FinalizeUninstall.ps1')){Copy-Item -LiteralPath (Join-Path $engine $name) -Destination $package -Force}
foreach($name in $signedFiles){Copy-Item -LiteralPath (Join-Path $sourcePayload $name) -Destination $payload -Force}
Copy-Item -LiteralPath (Join-Path $native 'InterceptorExample.exe') -Destination $payload -Force
$hashes['InterceptorExample.exe']=(Get-FileHash -LiteralPath (Join-Path $payload 'InterceptorExample.exe') -Algorithm SHA256).Hash
$hashes|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $payload 'SHA256.json') -Encoding UTF8
foreach($name in @('InterceptorRecovery.exe','RecoveryBinding.dll')){Copy-Item -LiteralPath (Join-Path $recovery $name) -Destination $recoveryPayload -Force}
. (Join-Path $package 'Common.ps1')
if((Assert-InterceptorPayload $payload) -ne 'Microsoft'){throw 'This public build requires the Microsoft-signed release payload.'}
& (Join-Path $root 'tools\Build-InstallerUI.ps1') -DiagnosticWorkerPath (Join-Path $native 'InterceptorDiagnosticWorker.exe') -PayloadDirectory $package
$references=Join-Path ${env:ProgramFiles(x86)} 'Reference Assemblies\Microsoft\Framework\.NETFramework\v4.6'
if(!(Test-Path -LiteralPath (Join-Path $references 'System.dll'))){$references=Join-Path $root 'obj\InstallerUI\net46\build\.NETFramework\v4.6'}
& (Join-Path $root 'tools\Build-InstallerConsole.ps1') -ReferenceDirectory $references
Write-Host 'Built SDK, eight console examples, UI viewer, UI installer and console installer.'
Write-Host 'All builds and self-tests were user-mode only. No driver installed and no Windows setting changed.'

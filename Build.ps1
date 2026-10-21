param(
    [ValidateSet('All','Native','InstallerUI','InstallerConsole','ExamplesConsole','ExamplesUI')][string]$Target='All',
    [string]$BuildDirectory
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$releaseState=Get-Content -LiteralPath (Join-Path $root 'docs\release-state.json') -Raw|ConvertFrom-Json
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

# Every target builds user-mode code only. The driver and recovery binaries are
# distributed as verified release files and are never compiled by this script.
& $cmake -S $root -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 ('-DCMAKE_GENERATOR_INSTANCE='+$vs)
if($LASTEXITCODE){throw 'CMake configuration failed.'}
$nativeTargets=switch($Target){
    'InstallerUI'{@('InterceptorExample','InterceptorDiagnosticWorker')}
    'InstallerConsole'{@('InterceptorExample')}
    'ExamplesUI'{@('InterceptorViewerBridge')}
    'ExamplesConsole'{@(Get-ChildItem -LiteralPath (Join-Path $root 'ExamplesConsole') -Filter '*.cpp' -File | ForEach-Object BaseName)}
    default{@()}
}
$nativeArguments=@('--build',$BuildDirectory,'--config','Release','--parallel')
if($nativeTargets.Count){$nativeArguments+=@('--target')+$nativeTargets}
& $cmake @nativeArguments
if($LASTEXITCODE){throw 'User-mode C++ compilation failed.'}
$native=Join-Path $root 'bin\Native'
if($Target -in @('All','Native','InstallerUI')){
    & (Join-Path $native 'InterceptorExample.exe') --self-test-setup
    if($LASTEXITCODE){throw 'Installer backend policy self-test failed.'}
    & (Join-Path $native 'InterceptorDiagnosticWorker.exe') --self-test
    if($LASTEXITCODE){throw 'Diagnostic worker self-test failed.'}
}
if($Target -in @('All','ExamplesUI')){
    & (Join-Path $root 'ExamplesUI\Build.ps1') -BridgePath (Join-Path $native 'InterceptorViewerBridge.dll') -OutputDirectory (Join-Path $root 'bin\ExampleUI') -IntermediateDirectory (Join-Path $BuildDirectory 'ExamplesUI')
}
if($Target -eq 'All' -and $releaseState.SigningPending){
    Write-Host ('Installer EXEs are pending the matching Microsoft-signed '+$releaseState.DriverVersion+' package. SDK and examples built; old payloads were not repackaged.')
}else{
    if($Target -in @('All','InstallerUI')){& (Join-Path $root 'InstallerUI\Build.ps1') -SkipNative -BuildDirectory $BuildDirectory}
    if($Target -in @('All','InstallerConsole')){& (Join-Path $root 'InstallerConsole\Build.ps1') -SkipNative -BuildDirectory $BuildDirectory}
}
Write-Host "Build completed: $Target. Driver and recovery release binaries were not rebuilt."
Write-Host 'All builds and self-tests were user-mode only. No driver installed and no Windows setting changed.'

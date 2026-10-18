param([switch]$SkipNative,[string]$BuildDirectory)
$ErrorActionPreference='Stop'
$source=$PSScriptRoot
$root=Split-Path $source -Parent
if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'obj\PublicBuild'}
$BuildDirectory=[IO.Path]::GetFullPath($BuildDirectory)
if(!$SkipNative){& (Join-Path $root 'Build.ps1') -Target InstallerConsole -BuildDirectory $BuildDirectory;return}
. (Join-Path $root 'InstallerUI\Build.Common.ps1')
$build=Join-Path $BuildDirectory 'InstallerConsole'
$out=Join-Path $root 'bin\InstallerConsole'
New-Item -ItemType Directory -Path $build,$out -Force|Out-Null
$prepared=New-InstallerPackage $build (Join-Path $root 'bin\Native')
$references=Get-InstallerReferenceDirectory
$compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$shared=Join-Path $root 'InstallerUI\Package.cs'
$exe=Join-Path $out 'InterceptorSetupConsole.exe'
$arguments=@('/nologo','/noconfig','/nostdlib+','/target:exe','/platform:x64','/optimize+','/debug:pdbonly','/warn:4','/warnaserror+','/utf8output','/codepage:65001',('/out:'+$exe),('/win32manifest:'+(Join-Path $root 'InstallerUI\App.manifest')))
foreach($name in @('mscorlib.dll','System.dll','System.Core.dll','System.Web.Extensions.dll','System.IO.Compression.dll')){$arguments+=('/reference:'+(Join-Path $references $name))}
$arguments+=@(('/resource:'+$prepared.ZipPath+',Payload.zip'),('/resource:'+$prepared.HashesPath+',PayloadHashes.json'))
$arguments+=@($shared,(Join-Path $source 'Program.cs'),(Join-Path $source 'AssemblyInfo.cs'))
& $compiler @arguments
if($LASTEXITCODE){throw 'Console installer compilation failed.'}
$savedConsoleEncoding=[Console]::OutputEncoding
try{
    [Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
    $testOutput=& $exe --self-test
    $testExitCode=$LASTEXITCODE
}finally{[Console]::OutputEncoding=$savedConsoleEncoding}
if($testExitCode){throw "Console installer self-test failed: $testExitCode"}
$test=$testOutput|ConvertFrom-Json
if(!$test.passed -or $test.installedAnything -ne $false){throw 'Console installer self-test returned an invalid result.'}
[IO.File]::WriteAllText((Join-Path $out 'self-test.json'),($test|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
$sourceHashes=[ordered]@{}
foreach($path in @($shared,(Join-Path $source 'Program.cs'),(Join-Path $source 'AssemblyInfo.cs'),(Join-Path $root 'InstallerUI\App.manifest'),(Join-Path $root 'InstallerUI\Build.Common.ps1'),$PSCommandPath)){$sourceHashes[$path.Substring($root.Length+1)]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
$manifest=[ordered]@{
    builtAt=[DateTime]::UtcNow.ToString('o');framework='.NET Framework 4.6';architecture='x64';subsystem='Console'
    sourcePackage='InstallerUI/Package';payloadZipSHA256=$prepared.ZipSHA256;resources=$prepared.Files;recoveryBinaries=$prepared.RecoveryHashes;sourceHashes=$sourceHashes
    driverVersion=$test.driverVersion;driverSigningMode=$test.driverSigningMode;exeSHA256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
}
$manifest|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $out 'build-manifest.json') -Encoding UTF8
Write-Host "Console installer: $exe"
Write-Host 'Built independently from InstallerUI/Package and shared Package.cs; no UI executable is required.'
Write-Host 'The existing signed driver and pinned recovery binaries were preserved. No installation performed.'

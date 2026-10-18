param(
    [string]$SetupPath,
    [string]$OutputDirectory,
    [string]$IntermediateDirectory,
    [string]$ReferenceDirectory
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$SetupPath){$SetupPath=Join-Path $root 'bin\InstallerUI\InterceptorSetup.exe'}
$SetupPath=(Resolve-Path -LiteralPath $SetupPath).Path
$source=Join-Path $root 'InstallerConsole'
$shared=Join-Path $root 'InstallerUI\Package.cs'
$build=if($IntermediateDirectory){[IO.Path]::GetFullPath($IntermediateDirectory)}else{Join-Path $root 'obj\InstallerConsole'}
$out=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $root 'bin\InstallerConsole'}
New-Item -ItemType Directory -Force -Path $build,$out | Out-Null

# Reuse the exact resources of the selected UI delivery. Do not repackage a
# possibly newer, unsigned kernel from bin/Release or rebuild the UI.
$setupHash=(Get-FileHash -LiteralPath $SetupPath -Algorithm SHA256).Hash
$assembly=[Reflection.Assembly]::LoadFile($SetupPath)
$resourceHashes=[ordered]@{}
foreach($name in @('Payload.zip','PayloadHashes.json')) {
    $input=$assembly.GetManifestResourceStream($name)
    if(!$input){throw "UI setup resource missing: $name"}
    $output=$null
    try {
        $output=[IO.File]::Open((Join-Path $build $name),[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::None)
        $input.CopyTo($output)
    } finally {if($output){$output.Dispose()};$input.Dispose()}
    $resourceHashes[$name]=(Get-FileHash -LiteralPath (Join-Path $build $name) -Algorithm SHA256).Hash
}
if((Get-FileHash -LiteralPath $SetupPath -Algorithm SHA256).Hash -ne $setupHash){throw 'UI setup changed during resource extraction.'}

if(!$ReferenceDirectory){$ReferenceDirectory=Join-Path ${env:ProgramFiles(x86)} 'Reference Assemblies\Microsoft\Framework\.NETFramework\v4.6'}
if(!(Test-Path -LiteralPath (Join-Path $ReferenceDirectory 'System.dll'))) {
    $ReferenceDirectory=Join-Path $root 'obj\InstallerUI\net46\build\.NETFramework\v4.6'
}
if(!(Test-Path -LiteralPath (Join-Path $ReferenceDirectory 'System.dll'))) {
    throw 'The .NET Framework 4.6 reference assemblies are required. Supply -ReferenceDirectory or build the UI first.'
}
$compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$exe=Join-Path $out 'InterceptorSetupConsole.exe'
$arguments=@('/nologo','/noconfig','/nostdlib+','/target:exe','/platform:x64','/optimize+','/debug:pdbonly','/warn:4','/warnaserror+','/utf8output','/codepage:65001',('/out:'+$exe),('/win32manifest:'+(Join-Path $root 'InstallerUI\App.manifest')))
foreach($name in @('mscorlib.dll','System.dll','System.Core.dll','System.Web.Extensions.dll','System.IO.Compression.dll')) {
    $arguments+=('/reference:'+(Join-Path $ReferenceDirectory $name))
}
foreach($name in $resourceHashes.Keys){$arguments+=('/resource:'+(Join-Path $build $name)+','+$name)}
$arguments+=@($shared,(Join-Path $source 'Program.cs'),(Join-Path $source 'AssemblyInfo.cs'))
& $compiler @arguments
if($LASTEXITCODE){throw 'Console installer compilation failed.'}

# Pure verification: this does not invoke the installation engine on the host.
$testOutput=& $exe --self-test
if($LASTEXITCODE){throw "Console installer self-test failed: $LASTEXITCODE"}
$test=$testOutput | ConvertFrom-Json
if(!$test.passed -or $test.installedAnything -ne $false){throw 'Invalid console installer self-test result.'}
[IO.File]::WriteAllText((Join-Path $out 'self-test.json'),($test|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
$sourceHashes=[ordered]@{}
foreach($path in @($shared,(Join-Path $source 'Program.cs'),(Join-Path $source 'AssemblyInfo.cs'),(Join-Path $root 'InstallerUI\App.manifest'),$PSCommandPath)) {
    $sourceHashes[$path.Substring($root.Length+1)]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}
$manifest=[ordered]@{
    builtAt=[DateTime]::UtcNow.ToString('o');framework='.NET Framework 4.6';architecture='x64';subsystem='Console'
    sourceUI=$SetupPath;sourceUISHA256=$setupHash;resources=$resourceHashes;sourceHashes=$sourceHashes
    driverVersion=$test.driverVersion;driverSigningMode=$test.driverSigningMode
    exeSHA256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
}
$manifest|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $out 'build-manifest.json') -Encoding UTF8
if(Test-Path -LiteralPath (Join-Path $source 'README.md')){Copy-Item -LiteralPath (Join-Path $source 'README.md') -Destination (Join-Path $out 'README.md')}
Write-Host "Console installer: $exe"
Write-Host "Exact UI payload reused: $SetupPath (driver $($test.driverVersion), $($test.driverSigningMode))."
Write-Host 'No installation performed on this machine.'

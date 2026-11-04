param([switch]$Rebuild)
$ErrorActionPreference='Stop'
if(![Environment]::Is64BitProcess){throw 'Run this build from 64-bit Windows PowerShell.'}
$source=$PSScriptRoot
$repository=Split-Path $source -Parent
$buildRoot=Join-Path $repository 'obj\DriverBuild'
$projects=@(Get-ChildItem -LiteralPath $source -Recurse -Filter '*.vcxproj' -File |
    Where-Object {$_.FullName.Substring($source.Length+1) -notmatch '^TrialClock[\\/]'} |
    Sort-Object FullName)
if(!$projects.Count){throw 'No primary driver projects were found.'}

$programFilesX86=[Environment]::GetFolderPath('ProgramFilesX86')
$vswhere=Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
if(!(Test-Path -LiteralPath $vswhere -PathType Leaf)){throw 'Install Visual Studio 2022 C++ tools and Windows Driver Kit 10.0.26100.0.'}
$vs=& $vswhere -latest -products * -version '[17.0,18.0)' -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Visual Studio 2022 with C++ x64 tools and MSBuild is required.'}
$msbuild=Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if(!(Test-Path -LiteralPath $msbuild -PathType Leaf)){throw 'Visual Studio 2022 x64 MSBuild was not found.'}
$vcRoot=Join-Path $vs 'VC\Tools\MSVC'
$toolset=Get-ChildItem -LiteralPath $vcRoot -Directory |
    Where-Object {
        (Test-Path -LiteralPath (Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'lib\spectre\x64\libcmt.lib') -PathType Leaf)
    } |
    Sort-Object {[version]$_.Name} -Descending |
    Select-Object -First 1
if(!$toolset){throw 'Install matching MSVC C++ x64 tools and x64 Spectre-mitigated libraries.'}
$kit=Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10 -ErrorAction SilentlyContinue
$kitRoot=if($kit -and $kit.KitsRoot10){$kit.KitsRoot10}else{Join-Path $programFilesX86 'Windows Kits\10'}
if(!(Test-Path -LiteralPath (Join-Path $kitRoot 'build\10.0.26100.0\WindowsDriver.common.targets') -PathType Leaf)){
    throw 'Install Windows SDK and WDK 10.0.26100.0 with Visual Studio 2022 integration.'
}

$target=if($Rebuild){'Rebuild'}else{'Build'}
Write-Host ('Using MSVC '+$toolset.Name+' with x64 MSBuild. Output: '+$buildRoot)
foreach($project in $projects){
    $relative=$project.FullName.Substring($source.Length+1)
    $projectKey=[IO.Path]::ChangeExtension($relative,$null).TrimEnd('.')
    $projectBuild=Join-Path $buildRoot $projectKey
    $native=Join-Path $projectBuild 'bin'
    $intermediate=Join-Path $projectBuild 'obj'
    $clock=Join-Path $projectBuild 'clock'
    New-Item -ItemType Directory -Path $native,$intermediate,$clock -Force | Out-Null
    $arguments=@(
        $project.FullName,'/nologo','/m','/v:minimal',('/t:'+$target),
        '/p:Configuration=Release','/p:Platform=x64',('/p:VCToolsVersion='+$toolset.Name),
        '/p:SignMode=Off','/p:EnableTestSign=false','/p:EnableInf2cat=false',
        '/p:BuildSharedDemoClock=true',
        ('/p:OutDir='+$native+'/'),('/p:IntDir='+$intermediate+'/'),
        ('/p:ResidentClockOut='+$clock+'/')
    )
    & $msbuild @arguments
    if($LASTEXITCODE){throw ('Driver compilation failed: '+$relative+' (exit '+$LASTEXITCODE+').')}
}
Write-Host ('Unsigned Release x64 compilation completed. Outputs: '+$buildRoot)
Write-Host 'This build does not install or load a driver, sign files, or change Windows security or network configuration.'

param([Parameter(Mandatory=$true)][string]$BridgePath,[string]$OutputDirectory,[string]$IntermediateDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$source=Join-Path $root 'examples\viewer'
$build=if($IntermediateDirectory){[IO.Path]::GetFullPath($IntermediateDirectory)}else{Join-Path $root 'obj\ExampleUI'}
$out=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $root 'bin\ExampleUI'}
New-Item -ItemType Directory -Force -Path $build,$out | Out-Null
$bridge=[IO.Path]::GetFullPath($BridgePath)
if(!(Test-Path -LiteralPath $bridge)){throw 'Native viewer bridge is missing.'}
$compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$refs=Join-Path ${env:ProgramFiles(x86)} 'Reference Assemblies\Microsoft\Framework\.NETFramework\v4.6'
if(!(Test-Path -LiteralPath (Join-Path $refs 'System.dll'))){
    $refs=Join-Path $root 'obj\InstallerUI\net46\build\.NETFramework\v4.6'
    if(!(Test-Path -LiteralPath (Join-Path $refs 'System.dll'))){
        $refs=Join-Path $build 'net46\build\.NETFramework\v4.6'
        if(!(Test-Path -LiteralPath (Join-Path $refs 'System.dll'))){
            $nupkg=Join-Path $build 'net46.nupkg'
            Invoke-WebRequest -UseBasicParsing -Uri 'https://api.nuget.org/v3-flatcontainer/microsoft.netframework.referenceassemblies.net46/1.0.3/microsoft.netframework.referenceassemblies.net46.1.0.3.nupkg' -OutFile $nupkg
            if((Get-FileHash -LiteralPath $nupkg -Algorithm SHA256).Hash -ne 'DCBB79BB3868DBFBB64C643116FAA63D888EE9ECCBA0C4E965E9992ED7C4E35D'){throw 'Unexpected .NET reference package digest.'}
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            [IO.Compression.ZipFile]::ExtractToDirectory($nupkg,(Join-Path $build 'net46'))
        }
    }
}
$exe=Join-Path $out 'InterceptorExampleUI.exe'
$arguments=@('/nologo','/noconfig','/nostdlib+','/target:winexe','/platform:x64','/optimize+','/debug:pdbonly','/warn:4','/warnaserror+','/utf8output','/codepage:65001',('/out:'+$exe),('/win32manifest:'+(Join-Path $source 'App.manifest')),('/resource:'+$bridge+',InterceptorViewerBridge.dll'))
$icon=Join-Path $root 'obj\InstallerUI\Interceptor.ico'
if(Test-Path -LiteralPath $icon){$arguments+=('/win32icon:'+$icon)}
foreach($name in @('mscorlib.dll','System.dll','System.Core.dll','System.Xaml.dll','System.Xml.dll','System.Web.Extensions.dll','WindowsBase.dll','PresentationCore.dll','PresentationFramework.dll')){
    $path=Join-Path $refs $name
    if(!(Test-Path -LiteralPath $path)){$path=Join-Path (Join-Path $refs 'WPF') $name}
    if(!(Test-Path -LiteralPath $path)){throw "Missing reference: $name"}
    $arguments+=('/reference:'+$path)
}
$arguments+=@(Get-ChildItem -LiteralPath $source -Filter '*.cs' -File | ForEach-Object FullName)
& $compiler @arguments
if($LASTEXITCODE){throw 'Example UI compile failed.'}
$manifest=[ordered]@{builtAt=(Get-Date).ToString('o');framework='Microsoft .NET Framework 4.6';architecture='x64';bridgeSha256=(Get-FileHash -LiteralPath $bridge -Algorithm SHA256).Hash;exeSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash;sourceHashes=[ordered]@{}}
foreach($file in (Get-ChildItem -LiteralPath $source -File)){$manifest.sourceHashes[$file.Name]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'build-manifest.json') -Encoding UTF8
$test=Join-Path $out 'self-test.json'
$p=Start-Process -FilePath $exe -ArgumentList @('--self-test',('"'+$test+'"')) -Wait -PassThru -WindowStyle Hidden
if($p.ExitCode -ne 0){throw "Example UI self-test failed: $($p.ExitCode). See $test"}
Write-Host "Example UI built: $exe. Self-test passed; no capture or installation."

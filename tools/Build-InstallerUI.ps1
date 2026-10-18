param([string]$OutputDirectory,[string]$IntermediateDirectory,[Parameter(Mandatory=$true)][string]$DiagnosticWorkerPath,[Parameter(Mandatory=$true)][string]$PayloadDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$source=Join-Path $root 'installer\ui'
$build=if($IntermediateDirectory){[IO.Path]::GetFullPath($IntermediateDirectory)}else{Join-Path $root 'obj\InstallerUI'}
$out=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $root 'bin\InstallerUI'}
New-Item -ItemType Directory -Force -Path $build,$out | Out-Null
$DiagnosticWorkerPath=(Resolve-Path -LiteralPath $DiagnosticWorkerPath).Path
$workerHash=(Get-FileHash -LiteralPath $DiagnosticWorkerPath).Hash
[IO.File]::WriteAllText((Join-Path $build 'DiagnosticWorker.sha256'),$workerHash,[Text.Encoding]::ASCII)
$payload=(Resolve-Path -LiteralPath $PayloadDirectory).Path
foreach($name in @('Install.ps1','Uninstall.ps1','Associate.ps1','CompleteAssociation.ps1','Common.ps1','Lifecycle.ps1','EmergencyRecover.ps1','FinalizeUninstall.ps1','Recovery\InterceptorRecovery.exe','Recovery\RecoveryBinding.dll','Payload\SHA256.json')) { if(!(Test-Path -LiteralPath (Join-Path $payload $name))) { throw "Missing installer payload: $name" } }
$driverManifest=Get-Content -LiteralPath (Join-Path $payload 'Payload\SHA256.json') -Raw | ConvertFrom-Json
. (Join-Path $payload 'Common.ps1')
$signingMode=Assert-InterceptorPayload (Join-Path $payload 'Payload')
foreach($name in $driverManifest.PSObject.Properties.Name) {
    $path=Join-Path $payload ('Payload\'+$name)
    if(!(Test-Path -LiteralPath $path -PathType Leaf) -or !$driverManifest.$name -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $driverManifest.$name){throw "Staged installer payload missing or mismatched: $name. Run Build.ps1 to regenerate it."}
}
$hashes=[ordered]@{}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipPath=Join-Path $build 'Payload.zip'
$stream=[IO.File]::Open($zipPath,[IO.FileMode]::Create)
try {
    $zip=New-Object IO.Compression.ZipArchive($stream,[IO.Compression.ZipArchiveMode]::Create,$true)
    try {
        $selected=@('Install.ps1','Uninstall.ps1','Associate.ps1','CompleteAssociation.ps1','Common.ps1','Lifecycle.ps1','EmergencyRecover.ps1','FinalizeUninstall.ps1','Recovery\InterceptorRecovery.exe','Recovery\RecoveryBinding.dll')
        $selected+=@('Payload\Interceptor.sys','Payload\InterceptorExample.exe','Payload\InterceptorKeyboard.inf','Payload\InterceptorMouse.inf','Payload\InterceptorControl.inf','Payload\interceptorkeyboard.cat','Payload\interceptormouse.cat','Payload\interceptorcontrol.cat','Payload\SHA256.json','Payload\Signing.json')
        foreach($relative in ($selected | Sort-Object)) {
            $file=Get-Item -LiteralPath (Join-Path $payload $relative)
            if(($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Refusing payload symlink: $($file.FullName)" }
            $name=$file.FullName.Substring($payload.Length+1).Replace('\','/')
            $hashes[$name]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$file.FullName,$name,[IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $zip.Dispose() }
} finally { $stream.Dispose() }
$hashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $build 'PayloadHashes.json') -Encoding UTF8
# Native executable icon, generated from simple vector geometry. PNG frames in
# ICO are supported by all targeted Windows versions; no image runtime is shipped.
Add-Type -AssemblyName System.Drawing
$frames=New-Object Collections.Generic.List[byte[]]
foreach($size in @(16,32,48,64,128,256)) {
    $bitmap=New-Object Drawing.Bitmap($size,$size)
    $graphics=[Drawing.Graphics]::FromImage($bitmap)
    $shape=New-Object Drawing.Drawing2D.GraphicsPath
    $blue=New-Object Drawing.SolidBrush([Drawing.ColorTranslator]::FromHtml('#1464E8'))
    $yellow=New-Object Drawing.SolidBrush([Drawing.ColorTranslator]::FromHtml('#FFE365'))
    $memory=New-Object IO.MemoryStream
    try {
        $graphics.SmoothingMode=[Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $radius=[single]($size*0.48);$edge=[single]($size*0.03);$extent=[single]($size-$edge*2)
        $shape.AddArc($edge,$edge,$radius,$radius,180,90)
        $shape.AddArc($edge+$extent-$radius,$edge,$radius,$radius,270,90)
        $shape.AddArc($edge+$extent-$radius,$edge+$extent-$radius,$radius,$radius,0,90)
        $shape.AddArc($edge,$edge+$extent-$radius,$radius,$radius,90,90)
        $shape.CloseFigure();$graphics.FillPath($blue,$shape)
        $graphics.FillRectangle($yellow,[single]($size*0.43),[single]($size*0.24),[single]($size*0.14),[single]($size*0.52))
        $bitmap.Save($memory,[Drawing.Imaging.ImageFormat]::Png);$frames.Add($memory.ToArray())
    } finally {$memory.Dispose();$blue.Dispose();$yellow.Dispose();$shape.Dispose();$graphics.Dispose();$bitmap.Dispose()}
}
$iconPath=Join-Path $build 'Interceptor.ico'
$icon=[IO.File]::Create($iconPath);$writer=New-Object IO.BinaryWriter($icon)
try {
    $writer.Write([uint16]0);$writer.Write([uint16]1);$writer.Write([uint16]$frames.Count)
    $offset=6+16*$frames.Count;$index=0
    foreach($size in @(16,32,48,64,128,256)) {
        $dimension=if($size -eq 256){0}else{$size}
        $writer.Write([byte]$dimension);$writer.Write([byte]$dimension);$writer.Write([byte]0);$writer.Write([byte]0);$writer.Write([uint16]1);$writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$index].Length);$writer.Write([uint32]$offset);$offset+=$frames[$index].Length;$index++
    }
    foreach($frame in $frames){$writer.Write($frame)}
} finally {$writer.Dispose();$icon.Dispose()}
$compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$refs=Join-Path ${env:ProgramFiles(x86)} 'Reference Assemblies\Microsoft\Framework\.NETFramework\v4.6'
if(!(Test-Path -LiteralPath (Join-Path $refs 'System.dll'))) {
    $refs=Join-Path $build 'net46\build\.NETFramework\v4.6'
    if(!(Test-Path -LiteralPath (Join-Path $refs 'System.dll'))) {
        $nupkg=Join-Path $build 'net46.nupkg'
        Invoke-WebRequest -UseBasicParsing -Uri 'https://api.nuget.org/v3-flatcontainer/microsoft.netframework.referenceassemblies.net46/1.0.3/microsoft.netframework.referenceassemblies.net46.1.0.3.nupkg' -OutFile $nupkg
        if((Get-FileHash -LiteralPath $nupkg -Algorithm SHA256).Hash -ne 'DCBB79BB3868DBFBB64C643116FAA63D888EE9ECCBA0C4E965E9992ED7C4E35D'){throw 'Unexpected .NET reference package digest.'}
        [IO.Compression.ZipFile]::ExtractToDirectory($nupkg,(Join-Path $build 'net46'))
    }
}
$references=@('mscorlib.dll','System.dll','System.Core.dll','System.Xaml.dll','System.Xml.dll','System.Web.Extensions.dll','System.IO.Compression.dll','WindowsBase.dll','PresentationCore.dll','PresentationFramework.dll')
$arguments=@('/nologo','/noconfig','/nostdlib+','/target:winexe','/platform:x64','/optimize+','/debug:pdbonly','/warn:4','/warnaserror+','/utf8output','/codepage:65001',('/out:'+(Join-Path $out 'InterceptorSetup.exe')),('/win32manifest:'+(Join-Path $source 'App.manifest')),('/win32icon:'+$iconPath),('/resource:'+(Join-Path $source 'MainWindow.xaml')+',MainWindow.xaml'),('/resource:'+$zipPath+',Payload.zip'),('/resource:'+(Join-Path $build 'PayloadHashes.json')+',PayloadHashes.json'),('/resource:'+$DiagnosticWorkerPath+',InterceptorDiagnosticWorker.exe'),('/resource:'+(Join-Path $build 'DiagnosticWorker.sha256')+',DiagnosticWorker.sha256'))
foreach($name in $references){$path=Join-Path $refs $name;if(!(Test-Path -LiteralPath $path)){$path=Join-Path (Join-Path $refs 'WPF') $name};$arguments+=('/reference:'+$path)}
$arguments+=@(Get-ChildItem -LiteralPath $source -Filter '*.cs' -File | ForEach-Object FullName)
& $compiler @arguments
if($LASTEXITCODE){throw 'Installer UI compile failed.'}
$buildInfo=[ordered]@{ builtAt=(Get-Date).ToString('o');framework='Microsoft .NET Framework 4.6 (inbox on Windows 10/11)';architecture='x64';driverSigningMode=$signingMode;payloadFiles=$hashes.Count;payloadZipSha256=(Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash;exeSha256=(Get-FileHash -LiteralPath (Join-Path $out 'InterceptorSetup.exe') -Algorithm SHA256).Hash;sourceHashes=[ordered]@{} }
$buildInfo.diagnosticWorkerSha256=$workerHash
foreach($file in (Get-ChildItem -LiteralPath $source -File -Recurse)){$buildInfo.sourceHashes[$file.FullName.Substring($source.Length+1)]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
$buildInfo | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'build-manifest.json') -Encoding UTF8
$testPath=Join-Path $out 'self-test.json'
$p=Start-Process -FilePath (Join-Path $out 'InterceptorSetup.exe') -ArgumentList @('--self-test',('"'+$testPath+'"')) -Wait -PassThru -WindowStyle Hidden
if($p.ExitCode -ne 0){throw "Installer self-test failed: $($p.ExitCode)"}
Write-Host "Single-file setup: $(Join-Path $out 'InterceptorSetup.exe')"
Write-Host "Embedded payload: $($hashes.Count) files. No driver installed on this machine."

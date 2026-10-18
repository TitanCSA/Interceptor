param([switch]$SkipNative,[string]$BuildDirectory)
$ErrorActionPreference='Stop'
$source=$PSScriptRoot
$root=Split-Path $source -Parent
if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'obj\PublicBuild'}
$BuildDirectory=[IO.Path]::GetFullPath($BuildDirectory)
if(!$SkipNative){& (Join-Path $root 'Build.ps1') -Target InstallerUI -BuildDirectory $BuildDirectory;return}
. (Join-Path $source 'Build.Common.ps1')
$build=Join-Path $BuildDirectory 'InstallerUI'
$out=Join-Path $root 'bin\InstallerUI'
$native=Join-Path $root 'bin\Native'
New-Item -ItemType Directory -Path $build,$out -Force|Out-Null
$DiagnosticWorkerPath=Join-Path $native 'InterceptorDiagnosticWorker.exe'
$workerHash=Assert-InstallerFile $DiagnosticWorkerPath
[IO.File]::WriteAllText((Join-Path $build 'DiagnosticWorker.sha256'),$workerHash,[Text.Encoding]::ASCII)
$prepared=New-InstallerPackage $build $native
$zipPath=$prepared.ZipPath
$hashes=$prepared.Files
$signingMode=$prepared.SigningMode
$refs=Get-InstallerReferenceDirectory
$compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
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
$references=@('mscorlib.dll','System.dll','System.Core.dll','System.Xaml.dll','System.Xml.dll','System.Web.Extensions.dll','System.IO.Compression.dll','WindowsBase.dll','PresentationCore.dll','PresentationFramework.dll')
$arguments=@('/nologo','/noconfig','/nostdlib+','/target:winexe','/platform:x64','/optimize+','/debug:pdbonly','/warn:4','/warnaserror+','/utf8output','/codepage:65001',('/out:'+(Join-Path $out 'InterceptorSetup.exe')),('/win32manifest:'+(Join-Path $source 'App.manifest')),('/win32icon:'+$iconPath),('/resource:'+(Join-Path $source 'MainWindow.xaml')+',MainWindow.xaml'),('/resource:'+$zipPath+',Payload.zip'),('/resource:'+(Join-Path $build 'PayloadHashes.json')+',PayloadHashes.json'),('/resource:'+$DiagnosticWorkerPath+',InterceptorDiagnosticWorker.exe'),('/resource:'+(Join-Path $build 'DiagnosticWorker.sha256')+',DiagnosticWorker.sha256'))
foreach($name in $references){$path=Join-Path $refs $name;if(!(Test-Path -LiteralPath $path)){$path=Join-Path (Join-Path $refs 'WPF') $name};$arguments+=('/reference:'+$path)}
$arguments+=@(Get-ChildItem -LiteralPath $source -Filter '*.cs' -File | ForEach-Object FullName)
& $compiler @arguments
if($LASTEXITCODE){throw 'Installer UI compile failed.'}
$buildInfo=[ordered]@{ builtAt=(Get-Date).ToString('o');framework='Microsoft .NET Framework 4.6 (inbox on Windows 10/11)';architecture='x64';driverSigningMode=$signingMode;payloadFiles=$hashes.Count;payloadZipSha256=(Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash;exeSha256=(Get-FileHash -LiteralPath (Join-Path $out 'InterceptorSetup.exe') -Algorithm SHA256).Hash;sourceHashes=[ordered]@{} }
$buildInfo.diagnosticWorkerSha256=$workerHash
$buildInfo.recoveryBinaries=$prepared.RecoveryHashes
$buildInfo.sourcePackage='InstallerUI/Package'
foreach($file in (Get-ChildItem -LiteralPath $source -File -Recurse)){$buildInfo.sourceHashes[$file.FullName.Substring($source.Length+1)]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
$buildInfo | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'build-manifest.json') -Encoding UTF8
$testPath=Join-Path $out 'self-test.json'
$p=Start-Process -FilePath (Join-Path $out 'InterceptorSetup.exe') -ArgumentList @('--self-test',('"'+$testPath+'"')) -Wait -PassThru -WindowStyle Hidden
if($p.ExitCode -ne 0){throw "Installer self-test failed: $($p.ExitCode)"}
$test=Get-Content -LiteralPath $testPath -Raw -Encoding UTF8|ConvertFrom-Json
if(!$test.passed -or $test.installedAnything -ne $false){throw 'Installer UI self-test returned an invalid result.'}
Write-Host "Single-file setup: $(Join-Path $out 'InterceptorSetup.exe')"
Write-Host "Embedded payload: $($hashes.Count) files. No driver installed on this machine."

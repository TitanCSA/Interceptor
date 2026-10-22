$ErrorActionPreference='Stop'
$script:InstallerSourceRoot=$PSScriptRoot
$script:InstallerRepositoryRoot=Split-Path $PSScriptRoot -Parent

function Get-InstallerReferenceDirectory([string]$Requested='') {
    $references=if($Requested){[IO.Path]::GetFullPath($Requested)}else{Join-Path ${env:ProgramFiles(x86)} 'Reference Assemblies\Microsoft\Framework\.NETFramework\v4.6'}
    if(Test-Path -LiteralPath (Join-Path $references 'System.dll')){return $references}
    $cache=Join-Path $script:InstallerRepositoryRoot 'obj\NetFramework46'
    $references=Join-Path $cache 'build\.NETFramework\v4.6'
    if(!(Test-Path -LiteralPath (Join-Path $references 'System.dll'))){
        New-Item -ItemType Directory -Path $cache -Force|Out-Null
        $archive=Join-Path $cache 'reference-assemblies.nupkg'
        Invoke-WebRequest -UseBasicParsing -Uri 'https://api.nuget.org/v3-flatcontainer/microsoft.netframework.referenceassemblies.net46/1.0.3/microsoft.netframework.referenceassemblies.net46.1.0.3.nupkg' -OutFile $archive
        if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne 'DCBB79BB3868DBFBB64C643116FAA63D888EE9ECCBA0C4E965E9992ED7C4E35D'){throw 'Unexpected .NET reference package digest.'}
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($archive,$cache)
    }
    return $references
}

function Assert-InstallerFile([string]$Path,[string]$ExpectedHash='') {
    $file=Get-Item -LiteralPath $Path -ErrorAction Stop
    if($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw "Expected a regular file: $Path"}
    $hash=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    if($ExpectedHash -and ($ExpectedHash -notmatch '^[A-Fa-f0-9]{64}$' -or $hash -ne $ExpectedHash)){throw "Source package hash mismatch: $Path"}
    return $hash
}

function New-InstallerPackage([string]$IntermediateDirectory,[string]$NativeDirectory) {
    $source=Join-Path $script:InstallerSourceRoot 'Package'
    $sourcePayload=Join-Path $source 'Payload'
    $sourceRecovery=Join-Path $source 'Recovery'
    $driverNames=@('Interceptor.sys','InterceptorKeyboard.inf','InterceptorMouse.inf','InterceptorControl.inf','interceptorkeyboard.cat','interceptormouse.cat','interceptorcontrol.cat','Signing.json')
    $recoveryNames=@('InterceptorRecovery.exe','RecoveryBinding.dll')
    $engineNames=@('Install.ps1','Uninstall.ps1','Associate.ps1','CompleteAssociation.ps1','Common.ps1','Lifecycle.ps1','Maintenance.ps1','EmergencyRecover.ps1','FinalizeUninstall.ps1')
    $driverManifest=Get-Content -LiteralPath (Join-Path $sourcePayload 'SHA256.json') -Raw|ConvertFrom-Json
    $recoveryManifest=Get-Content -LiteralPath (Join-Path $sourceRecovery 'SHA256.json') -Raw|ConvertFrom-Json
    if(@($driverManifest.PSObject.Properties).Count -ne $driverNames.Count){throw 'The signed source manifest must contain exactly the eight distributed driver files.'}
    if(@($recoveryManifest.PSObject.Properties).Count -ne $recoveryNames.Count){throw 'The recovery manifest must contain exactly the two precompiled recovery binaries.'}
    $driverHashes=[ordered]@{};$recoveryHashes=[ordered]@{}
    foreach($name in $driverNames){
        if($driverManifest.$name -notmatch '^[A-Fa-f0-9]{64}$'){throw "Missing driver manifest entry: $name"}
        $driverHashes[$name]=Assert-InstallerFile (Join-Path $sourcePayload $name) $driverManifest.$name
    }
    foreach($name in $recoveryNames){
        if($recoveryManifest.$name -notmatch '^[A-Fa-f0-9]{64}$'){throw "Missing recovery manifest entry: $name"}
        $recoveryHashes[$name]=Assert-InstallerFile (Join-Path $sourceRecovery $name) $recoveryManifest.$name
    }
    $helper=Join-Path $NativeDirectory 'InterceptorExample.exe'
    $driverHashes['InterceptorExample.exe']=Assert-InstallerFile $helper
    New-Item -ItemType Directory -Path $IntermediateDirectory -Force|Out-Null
    $package=Join-Path $IntermediateDirectory ('package-'+[Guid]::NewGuid().ToString('N'))
    $payload=Join-Path $package 'Payload';$recovery=Join-Path $package 'Recovery'
    New-Item -ItemType Directory -Path $package,$payload,$recovery|Out-Null
    foreach($name in $engineNames){Assert-InstallerFile (Join-Path $source $name)|Out-Null;Copy-Item -LiteralPath (Join-Path $source $name) -Destination $package}
    foreach($name in $driverNames){Copy-Item -LiteralPath (Join-Path $sourcePayload $name) -Destination $payload}
    Copy-Item -LiteralPath $helper -Destination (Join-Path $payload 'InterceptorExample.exe')
    $driverHashes|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $payload 'SHA256.json') -Encoding UTF8
    foreach($name in $recoveryNames){Copy-Item -LiteralPath (Join-Path $sourceRecovery $name) -Destination $recovery}
    . (Join-Path $package 'Common.ps1')
    $signingMode=Assert-InterceptorPayload $payload
    if($signingMode -ne 'Microsoft'){throw 'The public installers require the Microsoft-signed release driver.'}
    Assert-InterceptorUsbPackage $payload
    foreach($name in $recoveryNames){Assert-InstallerFile (Join-Path $recovery $name) $recoveryHashes[$name]|Out-Null}
    $selected=@($engineNames)+@($driverNames|ForEach-Object {'Payload/'+$_})+@('Payload/InterceptorExample.exe','Payload/SHA256.json')+@($recoveryNames|ForEach-Object {'Recovery/'+$_})
    $hashes=[ordered]@{}
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipPath=Join-Path $IntermediateDirectory 'Payload.zip'
    $stream=[IO.File]::Open($zipPath,[IO.FileMode]::Create)
    try{
        $zip=[IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create,$true)
        try{
            foreach($relative in ($selected|Sort-Object)){
                $file=Join-Path $package $relative
                $hashes[$relative]=Assert-InstallerFile $file
                $entry=$zip.CreateEntry($relative,[IO.Compression.CompressionLevel]::Optimal)
                # Both independent builds produce the same embedded package.
                $entry.LastWriteTime=[DateTimeOffset]::new(2000,1,1,0,0,0,[TimeSpan]::Zero)
                $input=[IO.File]::OpenRead($file)
                try{$output=$entry.Open();try{$input.CopyTo($output)}finally{$output.Dispose()}}finally{$input.Dispose()}
            }
        }finally{$zip.Dispose()}
    }finally{$stream.Dispose()}
    $hashesPath=Join-Path $IntermediateDirectory 'PayloadHashes.json'
    $hashes|ConvertTo-Json|Set-Content -LiteralPath $hashesPath -Encoding UTF8
    [pscustomobject]@{Directory=$package;ZipPath=$zipPath;HashesPath=$hashesPath;Files=$hashes;RecoveryHashes=$recoveryHashes;SigningMode=$signingMode;ZipSHA256=(Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash}
}

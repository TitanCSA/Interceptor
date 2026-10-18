$ErrorActionPreference='Stop'
function Assert-Administrator {
    if(!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Privileges administrateur requis.' }
    if(![Environment]::Is64BitProcess) { throw 'Utiliser Windows PowerShell x64.' }
    if([Environment]::OSVersion.Version.Build -lt 16299) { throw 'Windows 10 version 1709 ou plus recent, ou Windows 11 x64 requis.' }
}
function Add-InterceptorPackage([string]$Path) {
    if([Environment]::OSVersion.Version.Build -lt 14393) { & pnputil.exe -a $Path }
    else { & pnputil.exe /add-driver $Path }
    if($LASTEXITCODE -notin @(0,3010)) { throw "Ajout au magasin en echec: $Path ($LASTEXITCODE)" }
}
function Remove-InterceptorPackage([string]$Name) {
    # --restore is run first, so the legacy path need not forcibly remove a
    # still-bound package on Windows 10 1507/1511.
    if([Environment]::OSVersion.Version.Build -lt 14393) { & pnputil.exe -d $Name|ForEach-Object {Write-Host $_} }
    else { & pnputil.exe /delete-driver $Name /uninstall|ForEach-Object {Write-Host $_} }
    if($LASTEXITCODE -notin @(0,3010)) { throw "Suppression du paquet $Name en echec ($LASTEXITCODE). Fermez les clients et redemarrez." }
    $script:InterceptorNeedsReboot=$script:InterceptorNeedsReboot -or $LASTEXITCODE -eq 3010
}
function Start-InterceptorLog([string]$Name) {
    $folder=Join-Path $env:ProgramData 'Interceptor'
    Initialize-InterceptorStorage
    Start-Transcript -Path (Join-Path $folder ($Name+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'.log')) | Out-Null
}
function Get-InterceptorPackages {
    @(Get-WindowsDriver -Online | Where-Object {
        $_.ProviderName -eq 'Interceptor Project' -and
        [IO.Path]::GetFileName($_.OriginalFileName) -in @('interceptorkeyboard.inf','interceptormouse.inf','interceptorcontrol.inf')
    })
}
function Test-RunningTestMode {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class InterceptorCodeIntegrity {
    [StructLayout(LayoutKind.Sequential)] public struct Info { public UInt32 Length, Options; }
    [DllImport("ntdll.dll")] private static extern Int32 NtQuerySystemInformation(Int32 kind,ref Info data,UInt32 size,out UInt32 returned);
    public static bool Enabled() { var data=new Info { Length=8 }; UInt32 returned; var status=NtQuerySystemInformation(103,ref data,8,out returned); if(status<0) throw new InvalidOperationException("Cannot query running code-integrity state"); return (data.Options & 2)!=0; }
}
'@
    [InterceptorCodeIntegrity]::Enabled()
}
function Get-InterceptorSigningMode([string]$Payload) {
    $path=Join-Path $Payload 'Signing.json'
    if(!(Test-Path -LiteralPath $path -PathType Leaf)){return 'Test'}
    $policy=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if($policy.Schema -ne 1 -or $policy.Mode -notin @('Test','Microsoft')){throw 'Politique de signature du paquet invalide.'}
    return [string]$policy.Mode
}
function Assert-InterceptorPayload([string]$Payload) {
    $manifest=Get-Content -LiteralPath (Join-Path $Payload 'SHA256.json') -Raw | ConvertFrom-Json
    $mode=Get-InterceptorSigningMode $Payload
    $names=@('Interceptor.sys','InterceptorExample.exe','InterceptorKeyboard.inf','InterceptorMouse.inf','InterceptorControl.inf','InterceptorKeyboard.cat','InterceptorMouse.cat','InterceptorControl.cat')
    if($mode -eq 'Test'){$names+= 'InterceptorTest.cer'}
    if(Test-Path -LiteralPath (Join-Path $Payload 'Signing.json')){$names+= 'Signing.json'}
    foreach($name in $names) {
        $path=Join-Path $Payload $name
        if(!(Test-Path -LiteralPath $path -PathType Leaf) -or !$manifest.$name -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $manifest.$name){throw "Paquet incomplet ou modifie: $name"}
    }
    if($mode -eq 'Microsoft') {
        foreach($name in @('Interceptor.sys','InterceptorKeyboard.cat','InterceptorMouse.cat','InterceptorControl.cat')) {
            $signature=Get-AuthenticodeSignature -LiteralPath (Join-Path $Payload $name)
            if($signature.Status -ne 'Valid' -or !$signature.SignerCertificate -or
               $signature.SignerCertificate.Subject -ne 'CN=Microsoft Windows Hardware Compatibility Publisher, O=Microsoft Corporation, L=Redmond, S=Washington, C=US') {
                throw "Signature Microsoft invalide: $name"
            }
        }
    }
    return $mode
}
. (Join-Path $PSScriptRoot 'Lifecycle.ps1')

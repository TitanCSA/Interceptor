$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$script:InterceptorService='Interceptor'
function Assert-Administrator {
    if(!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Privileges administrateur requis.' }
    if(![Environment]::Is64BitProcess) { throw 'Utiliser Windows PowerShell x64.' }
    if([Environment]::OSVersion.Version.Build -lt 16299) { throw 'Windows 10 version 1709 ou plus recent, ou Windows 11 x64 requis.' }
}
function Assert-InterceptorInstallationBoundary {
    foreach($name in @('InterceptorKeyboardMouse','InterceptorGamepadController')){
        if(Test-Path -LiteralPath ('HKLM:\SYSTEM\CurrentControlSet\Services\'+$name)){
            $script:InterceptorLastExitCode=1638
            throw ('Incompatible Interceptor edition is still installed or pending removal: '+$name+'. Uninstall that edition and restart Windows before installing this product. Its driver and services were not changed.')
        }
    }
}
function Add-InterceptorPackage([string]$Path) {
    $result=Invoke-InterceptorProcess (Get-InterceptorWindowsTool 'pnputil.exe') @('/add-driver',$Path) -TimeoutMs 300000
    if($result.ExitCode -notin @(0,3010)){throw ('Driver Store add failed: '+$Path+'; exit '+$result.ExitCode)}
    $script:InterceptorNeedsReboot=$script:InterceptorNeedsReboot -or $result.ExitCode -eq 3010
}
function Remove-InterceptorPackage([string]$Name) {
    if($Name -notmatch '^oem[0-9]+\.inf$'){throw 'Invalid owned Driver Store package name.'}
    $result=Invoke-InterceptorProcess (Get-InterceptorWindowsTool 'pnputil.exe') @('/delete-driver',$Name,'/uninstall') -TimeoutMs 300000
    if($result.ExitCode -notin @(0,3010)){throw ('Driver Store remove failed: '+$Name+'; exit '+$result.ExitCode)}
    $script:InterceptorNeedsReboot=$script:InterceptorNeedsReboot -or $result.ExitCode -eq 3010
}
function Start-InterceptorLog([string]$Name) {
    $folder=Join-Path $env:ProgramData 'Interceptor'
    Initialize-InterceptorStorage
    $script:InterceptorOperationLabel=$Name
    $script:InterceptorLogPath=Join-Path $folder ($Name+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[Guid]::NewGuid().ToString('N')+'.log')
    Start-Transcript -Path $script:InterceptorLogPath|Out-Null
    Write-Host ('Setup log: '+$script:InterceptorLogPath)
}
function Get-InterceptorPackages {
    @(Get-WindowsDriver -Online | Where-Object {
        $_.ProviderName -eq 'Interceptor Project' -and
        [IO.Path]::GetFileName($_.OriginalFileName) -in @('interceptorkeyboard.inf','interceptormouse.inf','interceptorcontrol.inf')
    })
}
function Get-InterceptorSigningMode([string]$Payload) {
    $path=Join-Path $Payload 'Signing.json'
    if(!(Test-Path -LiteralPath $path -PathType Leaf)){throw 'Package signing policy is missing.'}
    $policy=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if($policy.Schema -ne 1 -or $policy.Mode -notin @('Development','Microsoft')){throw 'Politique de signature du paquet invalide.'}
    return [string]$policy.Mode
}
function Assert-InterceptorPayload([string]$Payload,[switch]$AllowDevelopment) {
    $manifest=Get-Content -LiteralPath (Join-Path $Payload 'SHA256.json') -Raw | ConvertFrom-Json
    $mode=Get-InterceptorSigningMode $Payload
    $names=@('Interceptor.sys','InterceptorKeyboard.inf','InterceptorMouse.inf','InterceptorControl.inf','interceptorkeyboard.cat','interceptormouse.cat','interceptorcontrol.cat','InterceptorExample.exe')
    if($mode -eq 'Development' -and !$AllowDevelopment){throw 'This product has a new driver identity. Microsoft signing is required before installation. Submit the product CAB and import its signed return; no Windows setting was changed.'}
    if(Test-Path -LiteralPath (Join-Path $Payload 'Signing.json')){$names+= 'Signing.json'}
    foreach($name in $names) {
        $path=Join-Path $Payload $name
        if(!(Test-Path -LiteralPath $path -PathType Leaf) -or !$manifest.$name -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $manifest.$name){throw "Paquet incomplet ou modifie: $name"}
    }
    foreach($name in @('InterceptorKeyboard.inf','InterceptorMouse.inf','InterceptorControl.inf')){
        $inf=Get-Content -LiteralPath (Join-Path $Payload $name) -Raw
        if($inf -notmatch '(?im)^\s*AddService\s*=\s*Interceptor\s*,'){throw "Unexpected service identity: $name"}
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
function Assert-InterceptorUsbPackage([string]$Payload) {
    # Call only after Assert-InterceptorPayload verified this exact embedded INF,
    # catalog, image and helper. Never apply the new backend requirement to an
    # older signed package merely because its service name matches.
    $inf=Get-Content -LiteralPath (Join-Path $Payload 'InterceptorControl.inf') -Raw
    $versions=[regex]::Matches($inf,'(?im)^\s*DriverVer\s*=\s*[^,\r\n]+,\s*(\d+\.\d+\.\d+\.\d+)\s*$')
    if($versions.Count -ne 1 -or [version]$versions[0].Groups[1].Value -lt [version]'1.4.17.0'){
        throw 'This installer requires the Microsoft-signed USB-backend driver package 1.4.17.0 or later. The older embedded package cannot complete this upgrade.'
    }
}
function Invoke-InterceptorSetupHelper([string]$Helper,[string[]]$Arguments,[int]$TimeoutMs=180000) {
    return [int](Invoke-InterceptorProcess $Helper $Arguments -TimeoutMs $TimeoutMs).ExitCode
}
function Initialize-InterceptorControllers([string]$Helper) {
    # The SDK intentionally refuses to migrate an older loaded upper backend.
    # Installation must preserve its pending state instead of treating skipped
    # XUSB associations as proof that the new package is operational.
    $backend=Invoke-InterceptorSetupHelper $Helper @('--require-usb-backend')
    if($backend -eq 3010){return 3010}
    if($backend -ne 0){throw ('Cannot verify the loaded USB XUSB backend: '+$backend)}
    $code=Invoke-InterceptorSetupHelper $Helper @('--init-all','--quiet')
    if($code -notin @(0,3010)){throw ('Controller initialization failed: '+$code)}
    return $code
}
. (Join-Path $PSScriptRoot 'Lifecycle.ps1')

. (Join-Path $PSScriptRoot 'Maintenance.ps1')

. (Join-Path $PSScriptRoot 'Common.ps1')
$code=1;$logged=$false;$bindingStarted=$false;$stagingStarted=$false;$existingPackages=@()
try {
    Assert-Administrator
    $payload=Join-Path $PSScriptRoot 'Payload'
    [void](Assert-InterceptorPayload $payload)
    Assert-InterceptorUsbPackage $payload
    Enter-InterceptorOperation;Start-InterceptorLog 'install';$logged=$true
    $baseline=Save-InterceptorBaseline
    $baseline.PendingUninstall=$false;$baseline.FinalizeAttempts=0;Write-InterceptorState $baseline
    Remove-InterceptorFinalCleanup
    Install-InterceptorRecovery
    Write-Host 'This product driver and catalogs are Microsoft signed.'
    Get-PnpDevice -PresentOnly | Where-Object Class -in @('Keyboard','Mouse','HIDClass','XnaComposite','XboxComposite') | Select-Object Class,FriendlyName,InstanceId,Status | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $env:ProgramData 'Interceptor\devices-before.json') -Encoding UTF8
    $existingPackages=@(Get-InterceptorPackages | ForEach-Object Driver)
    $stagingStarted=$true
    foreach($name in @('InterceptorKeyboard.inf','InterceptorMouse.inf','InterceptorControl.inf')) {
        Add-InterceptorPackage (Join-Path $payload $name)
    }
    $bindingStarted=$true
    & (Join-Path $payload 'InterceptorExample.exe') --install $payload
    $code=$LASTEXITCODE
    if($code -notin @(0,3010)) { throw "Association aux peripheriques en echec: $code" }
    if($code -eq 0) {
        & (Join-Path $payload 'InterceptorExample.exe') --status
        if($LASTEXITCODE -ne 0) { throw 'Le pilote ne repond pas apres installation.' }
        $code=Initialize-InterceptorControllers (Join-Path $payload 'InterceptorExample.exe')
    }
    Set-InterceptorPendingControllers $code (Get-FileHash -LiteralPath (Join-Path $payload 'Interceptor.sys') -Algorithm SHA256).Hash
    Get-InterceptorPackages | Select-Object Driver,OriginalFileName | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $env:ProgramData 'Interceptor\packages.json') -Encoding UTF8
    Write-Host 'Interceptor installed. Les entrees passent normalement tant qu aucun contexte ne capture.'
} catch {
    Write-Host $_.Exception.Message
    $restorationComplete=$true
    if($bindingStarted) {
        Write-Host 'Tentative de retour aux pilotes HID Windows...'
        & (Join-Path $PSScriptRoot 'Payload\InterceptorExample.exe') --restore
        $restorationComplete=$LASTEXITCODE -in @(0,3010)
        if(!$restorationComplete) {
            Write-Host ('Input restoration failed ('+$LASTEXITCODE+'). Driver packages, control endpoint and recovery files are retained because input stacks may still reference the driver. Retry Install / reinstall with the corrected installer or export diagnostics.')
        }
    }
    if($stagingStarted -and $restorationComplete) {
        try {
            foreach($package in (Get-InterceptorPackages | Where-Object { $_.Driver -notin $existingPackages })) {
                Remove-InterceptorPackage $package.Driver
            }
        } catch { Write-Host "Nettoyage des nouveaux paquets incomplet: $($_.Exception.Message)" }
    }
    $code=1
} finally { if($logged) { Stop-Transcript | Out-Null };Exit-InterceptorOperation }
exit $code

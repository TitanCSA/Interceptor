. (Join-Path $PSScriptRoot 'Common.ps1')
$code=1;$logged=$false;$bindingStarted=$false;$stagingStarted=$false;$existingPackages=@()
try {
    Assert-Administrator
    $payload=Join-Path $PSScriptRoot 'Payload'
    Assert-InterceptorPayload $payload|Out-Null
    Assert-InterceptorUsbPackage $payload
    Get-InterceptorWindowsTool 'pnputil.exe'|Out-Null
    Get-InterceptorWindowsTool 'sc.exe'|Out-Null
    Enter-InterceptorOperation
    Assert-InterceptorInstallationBoundary
    Start-InterceptorLog 'install';$logged=$true
    Set-InterceptorPhase 'Snapshot original Windows device bindings'
    $baseline=Save-InterceptorBaseline
    $baseline.PendingUninstall=$false;$baseline.FinalizeAttempts=0;Write-InterceptorState $baseline
    $helper=Join-Path $payload 'InterceptorExample.exe'
    $script:InterceptorBeforeSetup=Get-InterceptorSetupState $helper
    $existingPackages=@(Get-InterceptorPackages|ForEach-Object Driver)
    Set-InterceptorPhase 'Prepare protected recovery'
    Remove-InterceptorFinalCleanup
    Install-InterceptorRecovery
    Set-InterceptorPhase 'Stage Microsoft-signed driver packages'
    $stagingStarted=$true
    foreach($name in @('InterceptorKeyboard.inf','InterceptorMouse.inf','InterceptorControl.inf')){Add-InterceptorPackage (Join-Path $payload $name)}
    Set-InterceptorPhase 'Associate supported device instances'
    $bindingStarted=$true
    $code=Invoke-InterceptorSetupHelper $helper @('--install',$payload)
    if($code -notin @(0,3010)){throw ('Device association failed: '+$code)}
    if($code -eq 0){
        if((Invoke-InterceptorSetupHelper $helper @('--status') 20000) -ne 0){throw 'Driver API did not respond after installation.'}
        Set-InterceptorPhase 'Initialize supported input interfaces'
        $code=Initialize-InterceptorControllers $helper
        if($code -notin @(0,3010)){throw ('Input interface initialization failed: '+$code)}
    }
    if($script:InterceptorNeedsReboot){$code=3010}
    Set-InterceptorPhase 'Verify Windows device startup'
    $code=Wait-InterceptorSetupDevices $helper $script:InterceptorBeforeSetup -RestartRequested:($code -eq 3010)
    Set-InterceptorPendingControllers $code (Get-FileHash -LiteralPath (Join-Path $payload 'Interceptor.sys') -Algorithm SHA256).Hash
    Get-InterceptorPackages|Select-Object Driver,OriginalFileName|ConvertTo-Json|Set-Content -LiteralPath (Join-Path (Get-InterceptorDataRoot) 'packages.json') -Encoding UTF8
    Complete-InterceptorOperation $code
    Write-Host 'Installation verified. Physical input passes through until an application opens capture.'
}catch{
    $code=if($logged){Write-InterceptorFailure $_}else{Write-Host $_.Exception.ToString();if($script:InterceptorLastExitCode -notin @(0,3010)){$script:InterceptorLastExitCode}else{1}}
    $safeToRemove=$false
    if($script:InterceptorProcessTimedOut){
        Write-Host 'A setup command exceeded its deadline. Binding rollback and package deletion are deferred; recovery and operation evidence are retained. Restart Windows normally, then retry.'
    }elseif($bindingStarted){
        try{
            Write-Host 'Restoring Windows input bindings after the failed installation...'
            $restore=Invoke-InterceptorSetupHelper $helper @('--restore')
            if($restore -eq 0){
                $remaining=Get-InterceptorSetupState $helper
                $safeToRemove=@($remaining.devices).Count -eq 0
            }
            if(!$safeToRemove){Write-Host ('Restoration is incomplete or needs a restart (code '+$restore+'). Driver packages and recovery files are retained.')}
        }catch{Write-Host ('Restoration failed; driver packages and recovery are retained: '+$_.Exception.Message)}
    }else{$safeToRemove=$true}
    if($stagingStarted -and $safeToRemove){
        try{foreach($package in (Get-InterceptorPackages|Where-Object {$_.Driver -notin $existingPackages})){Remove-InterceptorPackage $package.Driver}}
        catch{Write-Host ('Owned package cleanup is pending: '+$_.Exception.Message)}
    }
}finally{
    try{if($logged){Stop-Transcript|Out-Null}}finally{Exit-InterceptorOperation}
}
exit $code

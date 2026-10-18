param([switch]$Emergency,[switch]$Finalize)
. (Join-Path $PSScriptRoot 'Common.ps1')
$code=1;$logged=$false
try {
    Assert-Administrator;Enter-InterceptorOperation
    if($Finalize){$pending=Read-InterceptorState;if(!$pending -or !$pending.PendingUninstall){Write-Host 'No pending Interceptor cleanup; newer installation preserved.';$code=0;exit 0}}
    Start-InterceptorLog 'uninstall';$logged=$true
    $code=Invoke-InterceptorCleanUninstall -Finalize:$Finalize
    Write-Host 'Interceptor bindings, packages and services removed. Boot settings and pre-existing certificates preserved.'
    if($code -eq 0){
        Stop-Transcript|Out-Null;$logged=$false
        Remove-InterceptorOwnedTree (Get-InterceptorRecoveryRoot) (Join-Path $env:ProgramFiles 'Interceptor\Recovery')
        $parent=Join-Path $env:ProgramFiles 'Interceptor'
        if((Test-Path -LiteralPath $parent) -and @(Get-ChildItem -LiteralPath $parent -Force).Count -eq 0){Remove-Item -LiteralPath $parent}
        Remove-InterceptorOwnedTree (Get-InterceptorDataRoot) (Join-Path $env:ProgramData 'Interceptor')
    } else {Write-Host 'Windows requires a restart. Remaining owned files will be removed automatically after that restart.'}
} catch {Write-Host $_.Exception.ToString();$code=1}
finally {if($logged){Stop-Transcript|Out-Null};Exit-InterceptorOperation}
if($Emergency -and $code -eq 3010){
    & (Join-Path $env:WINDIR 'System32\shutdown.exe') /r /t 30 /d p:2:4 /c 'Interceptor emergency recovery: restart required to finish restoring Windows input drivers.'
}
exit $code

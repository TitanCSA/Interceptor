# Loaded by Common.ps1. No machine mutation occurs just by loading this file.
function Get-InterceptorDataRoot {Join-Path $env:ProgramData 'Interceptor'}
function Get-InterceptorRecoveryRoot {Join-Path $env:ProgramFiles 'Interceptor\Recovery'}
function Assert-InterceptorDirectory([string]$Path,[string]$Expected) {
    $full=[IO.Path]::GetFullPath($Path).TrimEnd('\')
    if($full -ine [IO.Path]::GetFullPath($Expected).TrimEnd('\')){throw 'Unexpected Interceptor directory.'}
    $ancestor=$full
    while($ancestor){if(Test-Path -LiteralPath $ancestor){if((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Interceptor directory or ancestor is a reparse point.'}};$ancestor=Split-Path -Parent $ancestor}
    return $full
}
function Initialize-InterceptorStorage {
    $path=Assert-InterceptorDirectory (Get-InterceptorDataRoot) (Join-Path $env:ProgramData 'Interceptor')
    if(Test-Path -LiteralPath $path){$owner=(Get-Acl -LiteralPath $path).GetOwner([Security.Principal.SecurityIdentifier]).Value;if($owner -notin @('S-1-5-18','S-1-5-32-544')){throw 'Existing Interceptor data folder has an untrusted owner.'}}
    $security=New-Object Security.AccessControl.DirectorySecurity
    $security.SetAccessRuleProtection($true,$false)
    foreach($id in @('S-1-5-18','S-1-5-32-544')){
        $sid=New-Object Security.Principal.SecurityIdentifier($id)
        $security.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow')))
    }
    if(!(Test-Path -LiteralPath $path)){[IO.Directory]::CreateDirectory($path,$security)|Out-Null}else{Set-Acl -LiteralPath $path -AclObject $security}
}
function Enter-InterceptorOperation {
    $script:InterceptorOperationOwned=$false
    $security=New-Object Security.AccessControl.MutexSecurity
    foreach($id in @('S-1-5-18','S-1-5-32-544')){$security.AddAccessRule((New-Object Security.AccessControl.MutexAccessRule((New-Object Security.Principal.SecurityIdentifier($id)),'FullControl','Allow')))}
    $created=$false
    $script:InterceptorOperation=New-Object Threading.Mutex($false,'Global\InterceptorSetup-Installation',[ref]$created,$security)
    try{$acquired=$script:InterceptorOperation.WaitOne(120000)}catch [Threading.AbandonedMutexException]{$acquired=$true}
    if(!$acquired){$script:InterceptorOperation.Dispose();$script:InterceptorOperation=$null;throw 'Another Interceptor operation is still active.'}
    $script:InterceptorOperationOwned=$true
}
function Exit-InterceptorOperation {if($script:InterceptorOperation){if($script:InterceptorOperationOwned){$script:InterceptorOperation.ReleaseMutex()};$script:InterceptorOperation.Dispose();$script:InterceptorOperation=$null;$script:InterceptorOperationOwned=$false}}
function Import-InterceptorRecoveryAssembly {
    if(!('InterceptorRecovery.Devices' -as [type])){[Reflection.Assembly]::Load([IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'Recovery\InterceptorRecovery.exe')))|Out-Null}
}
function Write-InterceptorState($State) {
    $path=Join-Path (Get-InterceptorDataRoot) 'state.json'
    [InterceptorRecovery.Devices]::WriteState($path,($State|ConvertTo-Json -Depth 12))
}
function Read-InterceptorState {
    $path=Join-Path (Get-InterceptorDataRoot) 'state.json'
    if(!(Test-Path -LiteralPath $path)){return $null}
    if((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Restoration journal is a reparse point.'}
    $owner=(Get-Acl -LiteralPath $path).GetOwner([Security.Principal.SecurityIdentifier]).Value
    if($owner -notin @('S-1-5-18','S-1-5-32-544')){throw 'Restoration journal has an untrusted owner.'}
    $state=Get-Content -LiteralPath $path -Raw -Encoding UTF8|ConvertFrom-Json
    if($state.Schema -ne 1){throw 'Unsupported Interceptor restoration journal.'}
    return $state
}
function Save-InterceptorBaseline {
    Import-InterceptorRecoveryAssembly
    $state=Read-InterceptorState
    if(!$state){$state=[pscustomobject]@{Schema=1;CreatedUtc=[DateTime]::UtcNow.ToString('o');Devices=@();CertificatesAdded=@();PendingUninstall=$false;FinalizeAttempts=0}}
    $known=@{};foreach($device in $state.Devices){$known[$device.Instance]=$true}
    foreach($device in [InterceptorRecovery.Devices]::Snapshot()){
        if(!$known.ContainsKey($device.Instance)){$state.Devices+= $device;$known[$device.Instance]=$true}
    }
    Write-InterceptorState $state
    return $state
}
function Register-InterceptorCertificateAdds($State,[string]$Thumbprint) {
    foreach($store in @('Root','TrustedPublisher')){
        if(!(Test-Path -LiteralPath ('Cert:\LocalMachine\'+$store+'\'+$Thumbprint))){
            if(!@($State.CertificatesAdded|Where-Object {$_.Store -eq $store -and $_.Thumbprint -eq $Thumbprint}).Count){$State.CertificatesAdded+= [pscustomobject]@{Store=$store;Thumbprint=$Thumbprint}}
        }
    }
    Write-InterceptorState $State
}
function Set-InterceptorPendingControllers([int]$Code,[string]$DriverSHA256) {
    $state=Read-InterceptorState
    if(!$state){throw 'Installation journal missing for controller association.'}
    $pending=[pscustomobject]@{Pending=($Code -eq 3010);DriverSHA256=$DriverSHA256;Boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o');LastError=0}
    $state|Add-Member -NotePropertyName ControllerAssociation -NotePropertyValue $pending -Force
    Write-InterceptorState $state
    if($pending.Pending){Write-Host 'Association des manettes en attente : apres redemarrage, rouvrir cet installeur pour la terminer.'}
}
function Install-InterceptorRecovery {
    $root=Assert-InterceptorDirectory (Get-InterceptorRecoveryRoot) (Join-Path $env:ProgramFiles 'Interceptor\Recovery')
    $existing=Get-Service -Name InterceptorRecovery -ErrorAction SilentlyContinue
    if($existing){
        $image=[string](Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\InterceptorRecovery' -Name ImagePath).ImagePath
        if($image.Trim('"') -ine (Join-Path $root 'Recovery\InterceptorRecovery.exe')){throw 'A different recovery service already uses this name.'}
        if($existing.Status -ne 'Stopped'){$existing.Stop();$existing.WaitForStatus('Stopped',[TimeSpan]::FromSeconds(35))};$existing.Dispose()
    }
    $security=Get-Acl -LiteralPath (Get-InterceptorDataRoot)
    if(!(Test-Path -LiteralPath $root)){[IO.Directory]::CreateDirectory($root,$security)|Out-Null}else{Set-Acl -LiteralPath $root -AclObject $security}
    foreach($relative in @('Common.ps1','Lifecycle.ps1','Uninstall.ps1','EmergencyRecover.ps1','FinalizeUninstall.ps1','Recovery\InterceptorRecovery.exe','Recovery\RecoveryBinding.dll','Payload\InterceptorExample.exe')){
        $source=Join-Path $PSScriptRoot $relative;$target=Join-Path $root $relative
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))|Out-Null
        if([IO.Path]::GetFullPath($source) -ine [IO.Path]::GetFullPath($target)){Copy-Item -LiteralPath $source -Destination $target -Force}
    }
    if(!$existing){New-Service -Name InterceptorRecovery -DisplayName 'Interceptor emergency recovery' -BinaryPathName ('"'+(Join-Path $root 'Recovery\InterceptorRecovery.exe')+'"') -StartupType Automatic -DependsOn Winmgmt|Out-Null}
    # Automatic start at next boot, never during an installation's mutex hold.
}
function Stop-InterceptorRecovery {
    $service=Get-Service -Name InterceptorRecovery -ErrorAction SilentlyContinue
    if($service){try{if($service.Status -ne 'Stopped'){$service.Stop();$service.WaitForStatus('Stopped',[TimeSpan]::FromSeconds(35))}}finally{$service.Dispose()}}
}
function Remove-InterceptorRecoveryService {
    Stop-InterceptorRecovery
    if(Test-Path -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\InterceptorRecovery'){
        & sc.exe delete InterceptorRecovery|Write-Host
        if($LASTEXITCODE -notin @(0,1060,1072)){throw 'Recovery service deletion failed.'}
    }
}
function Remove-InterceptorOwnedTree([string]$Path,[string]$Expected) {
    $target=Assert-InterceptorDirectory $Path $Expected
    if(!(Test-Path -LiteralPath $target)){return}
    $pending=New-Object Collections.Generic.Stack[string];$pending.Push($target)
    while($pending.Count){foreach($item in Get-ChildItem -LiteralPath $pending.Pop() -Force){
        if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw ('Refusing cleanup through reparse point: '+$item.FullName)}
        if($item.PSIsContainer){$pending.Push($item.FullName)}
    }}
    Remove-Item -LiteralPath $target -Recurse -Force
}
function Register-InterceptorFinalCleanup {
    Get-InterceptorFinalCleanupTask|Out-Null
    $root=Get-InterceptorRecoveryRoot
    if(!(Test-Path -LiteralPath (Join-Path $root 'FinalizeUninstall.ps1'))){Install-InterceptorRecovery}
    $action=New-ScheduledTaskAction -Execute (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe') -Argument ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "'+(Join-Path $root 'FinalizeUninstall.ps1')+'"')
    $trigger=New-ScheduledTaskTrigger -AtStartup
    $settings=New-ScheduledTaskSettingsSet -StartWhenAvailable -ExecutionTimeLimit ([TimeSpan]::FromMinutes(10)) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName 'InterceptorCompleteUninstall' -Action $action -Trigger $trigger -Settings $settings -User 'S-1-5-18' -RunLevel Highest -Force|Out-Null
}
function Remove-InterceptorFinalCleanup {
    if(Get-InterceptorFinalCleanupTask){Unregister-ScheduledTask -TaskName InterceptorCompleteUninstall -Confirm:$false}
}
function Get-InterceptorFinalCleanupTask {
    $task=Get-ScheduledTask -TaskName InterceptorCompleteUninstall -ErrorAction SilentlyContinue
    if(!$task){return $null}
    $expectedExecute=Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $expectedArguments='-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "'+(Join-Path (Get-InterceptorRecoveryRoot) 'FinalizeUninstall.ps1')+'"'
    $isSystem=$false
    try{
        $id=[string]$task.Principal.UserId
        $sid=if($id -match '^S-1-') {New-Object Security.Principal.SecurityIdentifier($id)}else{(New-Object Security.Principal.NTAccount($id)).Translate([Security.Principal.SecurityIdentifier])}
        $isSystem=$sid.Value -eq 'S-1-5-18'
    }catch{$isSystem=$false}
    if(@($task.Actions).Count -ne 1 -or $task.Actions[0].Execute -ine $expectedExecute -or $task.Actions[0].Arguments -cne $expectedArguments -or !$isSystem){throw 'An unrelated scheduled task uses the Interceptor cleanup name; it will not be changed.'}
    return $task
}
function Invoke-InterceptorCleanUninstall([switch]$Finalize) {
    Import-InterceptorRecoveryAssembly
    Stop-InterceptorRecovery
    $state=Read-InterceptorState
    $reboot=$false
    if($state){
        $saved=[InterceptorRecovery.Devices]::Read((ConvertTo-Json -InputObject @($state.Devices) -Depth 10))
        $owned=@{};foreach($current in [InterceptorRecovery.Devices]::Snapshot()){if($current.Service -eq 'Interceptor'){$owned[$current.Instance]=$true}}
        foreach($device in $saved){if($owned.ContainsKey($device.Instance) -and $device.Service -in @('kbdhid','mouhid')){
            try{$reboot=[InterceptorRecovery.Devices]::RestoreMapper($device,(Join-Path $PSScriptRoot 'Recovery\RecoveryBinding.dll')) -or $reboot}
            catch{Write-Host ('Original binding unavailable; restoring the Windows mapper: '+$device.Instance+'; '+$_.Exception.Message)}
        }}
    }
    & (Join-Path $PSScriptRoot 'Payload\InterceptorExample.exe') --restore|ForEach-Object {Write-Host $_}
    if($LASTEXITCODE -notin @(0,3010)){throw "Windows input restoration failed ($LASTEXITCODE). Recovery files retained."}
    $reboot=$reboot -or $LASTEXITCODE -eq 3010
    if($state){foreach($device in $saved){[InterceptorRecovery.Devices]::RestoreFilter($device.Instance,$device.Upper);[InterceptorRecovery.Devices]::RestoreFilter($device.Instance,$device.Lower)}}
    $reboot=[InterceptorRecovery.Devices]::CleanClassFilters() -or $reboot
    foreach($package in Get-InterceptorPackages){Remove-InterceptorPackage $package.Driver}
    $reboot=$reboot -or $script:InterceptorNeedsReboot
    $remaining=@(Get-InterceptorPackages)
    $driver=Get-Service -Name Interceptor -ErrorAction SilentlyContinue
    if($driver){$running=$driver.Status -ne 'Stopped';$driver.Dispose();& sc.exe delete Interceptor|Write-Host;if($LASTEXITCODE -notin @(0,1060,1072)){throw 'Interceptor service removal failed.'};$reboot=$reboot -or $running}
    if($remaining.Count){$reboot=$true}
    if($state){foreach($cert in $state.CertificatesAdded){
        if($cert.Store -notin @('Root','TrustedPublisher') -or $cert.Thumbprint -notmatch '^[A-Fa-f0-9]{40}$'){throw 'Invalid certificate restoration entry.'}
        $path='Cert:\LocalMachine\'+$cert.Store+'\'+$cert.Thumbprint
        if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path}
    }}
    Remove-InterceptorRecoveryService
    if($reboot){
        if(!$state){$state=[pscustomobject]@{Schema=1;Devices=@();CertificatesAdded=@();PendingUninstall=$true;FinalizeAttempts=0}}
        $state.PendingUninstall=$true
        if($Finalize){$state.FinalizeAttempts++;if($state.FinalizeAttempts -gt 3){Write-InterceptorState $state;throw 'Cleanup still pending after three boots; state retained for diagnosis.'}}
        Write-InterceptorState $state;Register-InterceptorFinalCleanup
        return 3010
    }
    Remove-InterceptorFinalCleanup
    return 0
}

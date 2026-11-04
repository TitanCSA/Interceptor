# Installer-only process and PnP checks. Loading this file performs no changes.
$script:InterceptorNeedsReboot=$false
$script:InterceptorProcessTimedOut=$false
$script:InterceptorLastExitCode=0
$script:InterceptorLastPhase='preflight'
$script:InterceptorBeforeSetup=$null
function Get-InterceptorWindowsTool {
    param([ValidateSet('pnputil.exe','sc.exe')][string]$Name)
    $path=Join-Path ([Environment]::SystemDirectory) $Name
    if(!(Test-Path -LiteralPath $path -PathType Leaf)){throw ('Required Windows component is missing: '+$path+'. Repair Windows, then retry setup.')}
    return $path
}
function ConvertTo-InterceptorArgument([AllowEmptyString()][string]$Value) {
    if($Value.IndexOf([char]0) -ge 0){throw 'A process argument contains a NUL character.'}
    # CommandLineToArgvW / C runtime quoting, including trailing backslashes.
    $quoted=[regex]::Replace($Value,'(\\*)"','$1$1\"')
    $quoted=[regex]::Replace($quoted,'(\\+)$','$1$1')
    return '"'+$quoted+'"'
}
function Invoke-InterceptorProcess {
    param([string]$File,[string[]]$Arguments=@(),[int]$TimeoutMs=180000,[switch]$Capture)
    if(![IO.Path]::IsPathRooted($File) -or !(Test-Path -LiteralPath $File -PathType Leaf)){throw ('Executable is missing or not absolute: '+$File)}
    if($TimeoutMs -lt 1){throw 'Invalid process deadline.'}
    $process=New-Object Diagnostics.Process
    $process.StartInfo.FileName=$File
    $process.StartInfo.Arguments=(@($Arguments|ForEach-Object {ConvertTo-InterceptorArgument $_}) -join ' ')
    $process.StartInfo.WorkingDirectory=[IO.Path]::GetDirectoryName($File)
    $process.StartInfo.UseShellExecute=$false;$process.StartInfo.CreateNoWindow=$true
    $process.StartInfo.RedirectStandardOutput=$true;$process.StartInfo.RedirectStandardError=$true
    $encoding=if([IO.Path]::GetFileName($File) -in @('pnputil.exe','sc.exe')){[Text.Encoding]::Default}else{[Text.UTF8Encoding]::new($false)}
    $process.StartInfo.StandardOutputEncoding=$encoding
    $process.StartInfo.StandardErrorEncoding=$encoding
    try {
        if(!$process.Start()){throw 'Could not start the setup command.'}
        $held=$process.Handle # Preserve the process identity even for immediate exits.
        $output=$process.StandardOutput.ReadToEndAsync()
        $errors=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit($TimeoutMs)){
            $script:InterceptorProcessTimedOut=$true;$script:InterceptorLastExitCode=1460
            try{$process.Kill();[void]$process.WaitForExit(5000)}catch{}
            throw ('Setup command timed out [Win32 1460]: '+$File+' '+$process.StartInfo.Arguments+
                '. Recovery files and driver packages are retained; do not start a competing device operation. Restart Windows normally before retrying.')
        }
        $code=$process.ExitCode
        # A descendant must not keep setup blocked by inheriting output pipes.
        if(![Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($output,$errors),5000)){
            $script:InterceptorProcessTimedOut=$true;$script:InterceptorLastExitCode=1460
            throw 'Setup output pipes did not close. Operation evidence is retained; restart Windows before retrying.'
        }
        $text=$output.GetAwaiter().GetResult();$errorText=$errors.GetAwaiter().GetResult()
        $script:InterceptorLastExitCode=[int]$code
        if(!$Capture){if($text){Write-Host $text.TrimEnd()};if($errorText){Write-Host $errorText.TrimEnd()}}
        return [pscustomobject]@{ExitCode=[int]$code;Output=$text;Error=$errorText}
    }finally{$process.Dispose()}
}
function Get-InterceptorSetupState([string]$Helper) {
    $result=Invoke-InterceptorProcess $Helper @('--setup-state') -TimeoutMs 20000 -Capture
    if($result.ExitCode -ne 0){throw ('PnP inventory failed: '+$result.ExitCode+' '+$result.Error)}
    $state=$result.Output|ConvertFrom-Json
    if($state.schema -ne 1 -or $state.service -cne $script:InterceptorService){throw 'PnP inventory belongs to a different driver product or schema.'}
    return $state
}
function Get-InterceptorSetupAssessment {
    param($State,$Before,[switch]$RestartRequested)
    $rows=@();$roots=0
    foreach($device in $State.devices){
        if($device.root){$roots++}
        $prior=@($Before.devices|Where-Object instance -CEQ $device.instance|Select-Object -First 1)
        $status='Pending';$reason='Waiting for authoritative Windows device properties.'
        if(!$device.root -and $prior.Count -and $prior[0].status_error -eq 0 -and $prior[0].problem -eq 22 -and $device.problem -eq 22){
            $status='Unverified';$reason='Device was disabled before setup; its disabled state is preserved.'
        }elseif($device.status_error -eq 0){
            $problem=[uint32]$device.problem;$flags=[uint32]$device.status
            if($problem -ne 0 -and $problem -ne 14){$status='Failed';$reason='Windows device problem '+$problem+'.'}
            elseif($problem -eq 14 -or ($flags -band 0x100)){$status='Restart';$reason='Windows requires a restart.'}
            elseif($device.owned -and !($flags -band 0x400) -and ($flags -band 8)){$status='Ready';$reason='Device is started and associated with this product.'}
            elseif($device.owned -and $RestartRequested -and !($flags -band 0x400)){$status='Restart';$reason='Setup requested a restart before this device can start.'}
        }
        $rows+=[pscustomobject]@{Instance=$device.instance;State=$status;Reason=$reason;Problem=$device.problem;Flags=$device.status;QueryError=$device.status_error;Root=$device.root}
    }
    if(!$roots){$rows+=[pscustomobject]@{Instance='ROOT\'+$script:InterceptorService+'Control\(missing)';State='Pending';Reason='The product control endpoint is not present.';Root=$true}}
    $disconnected=@($Before.devices|Where-Object {!$_.root -and $_.instance -cnotin @($State.devices.instance)}|ForEach-Object instance)
    $unresolved=@($rows|Where-Object {$_.State -in @('Pending','Failed')})
    return [pscustomobject]@{Ready=($unresolved.Count -eq 0 -and !@($rows|Where-Object State -EQ 'Restart').Count);Settled=($unresolved.Count -eq 0);RestartRequired=([bool]$RestartRequested -or @($rows|Where-Object State -EQ 'Restart').Count -gt 0);Devices=$rows;Disconnected=$disconnected}
}
function Wait-InterceptorSetupDevices {
    param([string]$Helper,$Before,[switch]$RestartRequested,[int]$TimeoutMs=20000,[int]$PollMs=250,[switch]$NoReport)
    $watch=[Diagnostics.Stopwatch]::StartNew();$history=New-Object Collections.Generic.List[object]
    $previous='';$stable=0;$assessment=$null
    do {
        try {
            $state=Get-InterceptorSetupState $Helper
            $assessment=Get-InterceptorSetupAssessment $state $Before -RestartRequested:$RestartRequested
            $signature=(@($assessment.Devices|Sort-Object Instance|ForEach-Object {$_.Instance+'='+$_.State})+@($assessment.Disconnected|Sort-Object)) -join '|'
            if($assessment.Settled -and $signature -eq $previous){$stable++}else{$stable=0}
            $previous=$signature
            $history.Add([pscustomobject]@{ElapsedMs=$watch.ElapsedMilliseconds;Assessment=$assessment})
            if($stable -ge 1 -and (!$assessment.Disconnected.Count -or $watch.ElapsedMilliseconds -ge 2000)){break}
        }catch{
            if($script:InterceptorProcessTimedOut){throw}
            $stable=0;$assessment=$null;$previous=''
            $history.Add([pscustomobject]@{ElapsedMs=$watch.ElapsedMilliseconds;QueryError=$_.Exception.Message})
        }
        if($history.Count -gt 20){$history.RemoveAt(1)}
        if($watch.ElapsedMilliseconds -ge $TimeoutMs){break}
        Start-Sleep -Milliseconds ([Math]::Min($PollMs,[Math]::Max(1,$TimeoutMs-$watch.ElapsedMilliseconds)))
    }while($watch.ElapsedMilliseconds -lt $TimeoutMs)
    while($history.Count -gt 20){$history.RemoveAt(1)}
    if(!$NoReport){
        $report=Join-Path (Get-InterceptorDataRoot) 'setup-devices.json'
        [InterceptorRecovery.Devices]::WriteState($report,([pscustomobject]@{Schema=1;ElapsedMs=$watch.ElapsedMilliseconds;History=$history.ToArray()}|ConvertTo-Json -Depth 15))
        Write-Host ('Windows device verification report: '+$report)
    }
    if(!$assessment -or !$assessment.Settled -or $stable -lt 1 -or ($assessment.Disconnected.Count -and $watch.ElapsedMilliseconds -lt 2000)){
        $details=if($assessment){@($assessment.Devices|Where-Object {$_.State -in @('Failed','Pending')}|ForEach-Object {$_.Instance+': '+$_.Reason+' Flags='+$_.Flags+' Query='+$_.QueryError}) -join '; '}else{'Windows device queries did not complete successfully.'}
        if(!$details){$details='Windows startup did not remain stable across consecutive samples before the deadline.'}
        throw ('PnP verification failed: '+$details)
    }
    foreach($row in $assessment.Devices|Where-Object State -EQ 'Unverified'){Write-Host ($row.Instance+': '+$row.Reason)}
    return $(if($assessment.RestartRequired){3010}else{0})
}
function Set-InterceptorPhase {
    param([string]$Phase,[string]$Status='Running',[int]$Result=0)
    $script:InterceptorLastPhase=$Phase
    Write-Host ('SETUP_STAGE: '+$Phase)
    Import-InterceptorRecoveryAssembly
    $state=Read-InterceptorState
    if(!$state){$state=[pscustomobject]@{Schema=1;Devices=@();CertificatesAdded=@();PendingUninstall=$false;FinalizeAttempts=0}}
    $operation=[pscustomobject]@{Action=$script:InterceptorOperationLabel;Phase=$Phase;Status=$Status;Result=$Result;UpdatedUtc=[DateTime]::UtcNow.ToString('o');ProcessId=$PID}
    $state|Add-Member -NotePropertyName Operation -NotePropertyValue $operation -Force
    Write-InterceptorState $state
}
function Complete-InterceptorOperation([int]$Code) {
    if($Code -notin @(0,3010)){throw ('Unexpected setup completion code: '+$Code)}
    Set-InterceptorPhase 'complete' $(if($Code -eq 3010){'RestartRequired'}else{'Completed'}) $Code
}
function Write-InterceptorFailure($ErrorRecord) {
    $code=if($script:InterceptorLastExitCode -notin @(0,3010)){$script:InterceptorLastExitCode}else{1}
    Write-Host ('FAILED STAGE: '+$script:InterceptorLastPhase+'; exit code '+$code)
    Write-Host $ErrorRecord.Exception.ToString()
    if($ErrorRecord.InvocationInfo){Write-Host $ErrorRecord.InvocationInfo.PositionMessage}
    try{Set-InterceptorPhase $script:InterceptorLastPhase 'Failed' $code}catch{Write-Host ('Failure journal could not be updated: '+$_.Exception.Message)}
    return [int]$code
}


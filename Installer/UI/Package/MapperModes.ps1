# Loaded by Lifecycle.ps1. The caller holds the installation mutex for mutations.
function Get-InterceptorMapperModeKey {
    'HKLM:\SOFTWARE\'+$script:InterceptorService+'\MapperModes'
}
function Test-InterceptorMapperPaused([string]$Instance) {
    $value=Get-ItemPropertyValue -LiteralPath (Get-InterceptorMapperModeKey) -Name $Instance -ErrorAction SilentlyContinue
    return $value -eq 'Original'
}
function Set-InterceptorMapperPreference([string]$Instance,[bool]$Original) {
    $key=Get-InterceptorMapperModeKey
    if($Original){
        New-Item -Path $key -Force|Out-Null
        New-ItemProperty -LiteralPath $key -Name $Instance -Value 'Original' -PropertyType String -Force|Out-Null
    }elseif(Test-Path -LiteralPath $key){
        $value=Get-ItemPropertyValue -LiteralPath $key -Name $Instance -ErrorAction SilentlyContinue
        if($null -ne $value){Remove-ItemProperty -LiteralPath $key -Name $Instance}
    }
}
function Get-InterceptorInputMapper([string]$Instance) {
    $device=@([InterceptorRecovery.Devices]::Snapshot()|Where-Object Instance -IEQ $Instance)
    if($device.Count -ne 1){throw 'The selected input instance is unavailable.'}
    return $device[0]
}
function Get-InterceptorMapperInfPath([string]$Name) {
    if($Name -notmatch '\A(?:oem[0-9]+|keyboard|msmouse)\.inf\z'){throw 'Unexpected original input INF name.'}
    Join-Path ([Environment]::GetFolderPath('Windows')) ('INF\'+$Name)
}
function Save-InterceptorMapperOriginal($State,$Device) {
    if($Device.Service -notin @('mouhid','kbdhid')){throw 'The selected instance does not use a Windows HID input mapper.'}
    $path=Get-InterceptorMapperInfPath $Device.Inf
    $item=Get-Item -LiteralPath $path -Force
    if($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Unexpected original INF file.'}
    $hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    $record=[pscustomobject]@{Instance=$Device.Instance;ClassGuid=$Device.ClassGuid;Service=$Device.Service;Inf=$Device.Inf;Sha256=$hash;
        Upper=@($Device.Upper.Values|Where-Object {$_});Lower=@($Device.Lower.Values|Where-Object {$_})}
    $others=@($State.MapperOriginals|Where-Object {$_ -and $_.Instance -ine $Device.Instance})
    $State|Add-Member -NotePropertyName MapperOriginals -NotePropertyValue @($others+@($record)) -Force
    Write-InterceptorState $State
    return $record
}
function Get-InterceptorCurrentMapperPackage([bool]$Mouse) {
    $kind=if($Mouse){'mouse'}else{'keyboard'}
    $packages=@(Get-InterceptorPackages | Where-Object { [IO.Path]::GetFileNameWithoutExtension($_.OriginalFileName).EndsWith($kind,[StringComparison]::OrdinalIgnoreCase) } |
        Sort-Object {[version]$_.Version} -Descending)
    if(!$packages.Count -or $packages[0].Driver -notmatch '\Aoem[0-9]+\.inf\z'){throw 'Install the signed Interceptor input package first.'}
    $inf=Get-InterceptorMapperInfPath $packages[0].Driver
    if(!(Test-Path -LiteralPath $inf -PathType Leaf)){throw 'The staged Interceptor input package is missing.'}
    return [pscustomobject]@{Inf=$packages[0].Driver;Path=$inf}
}
function Invoke-InterceptorMapperBind([string]$Helper,$Current,[string]$Destination,[string]$DesiredService,[string]$DesiredInf,$ExpectedBinding) {
    $code=Invoke-InterceptorSetupHelper $Helper @('--bind-mapper',$Current.Instance,$Current.Service,$Current.Inf,$Destination,$DesiredService)
    if($code -notin @(0,3010)){throw [ComponentModel.Win32Exception]::new($code,'Windows did not bind the requested input package (Win32 '+$code+').')}
    if($code -eq 3010){return 3010}
    $timer=[Diagnostics.Stopwatch]::StartNew()
    do {
        $node=Get-PnpDevice -InstanceId $Current.Instance -PresentOnly -ErrorAction SilentlyContinue
        if($node){
            $problem=(Get-PnpDeviceProperty -InstanceId $Current.Instance -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue).Data
            if($problem -eq 14){return 3010}
            if($node.Status -eq 'OK'){
                $bound=Get-InterceptorInputMapper $Current.Instance
                if($bound.Service -ieq $DesiredService -and $bound.Inf -ieq $DesiredInf){
                    if($ExpectedBinding){
                        $missingUpper=@($ExpectedBinding.Upper|Where-Object {$_ -and $_ -notin @($bound.Upper.Values)})
                        $missingLower=@($ExpectedBinding.Lower|Where-Object {$_ -and $_ -notin @($bound.Lower.Values)})
                        if($missingUpper.Count -or $missingLower.Count){throw 'The original input package is active, but a recorded manufacturer filter is missing.'}
                    }
                    return 0
                }
            }
        }
        Start-Sleep -Milliseconds 150
    }while($timer.ElapsedMilliseconds -lt 3000)
    throw 'Windows did not start the input instance with the requested package.'
}
function Set-InterceptorMapperMode($State,[string]$Instance,[ValidateSet('Original','Interceptor')][string]$Mode,[string]$Helper) {
    if($Instance -notmatch '\AHID\\[A-Za-z0-9_\\&.{}\-\(\)#+!]{3,195}\z'){throw 'Expected a HID device instance ID.'}
    if($State.PendingUninstall){throw 'This Interceptor edition is pending removal.'}
    Get-PnpDevice -InstanceId $Instance -PresentOnly -ErrorAction Stop|Out-Null
    $current=Get-InterceptorInputMapper $Instance
    $saved=@($State.MapperOriginals|Where-Object {$_ -and $_.Instance -ieq $Instance})
    if($saved.Count -gt 1){throw 'Ambiguous original input package in the recovery journal.'}
    $expectedOriginal=if($current.ClassGuid -eq '{4d36e96f-e325-11ce-bfc1-08002be10318}'){'mouhid'}else{'kbdhid'}
    if($current.Service -notin @($expectedOriginal,$script:InterceptorService)){throw 'The input device changed ownership; association was not changed.'}
    if($Mode -eq 'Original'){
        if($current.Service -eq $expectedOriginal){
            Save-InterceptorMapperOriginal $State $current|Out-Null
            Set-InterceptorMapperPreference $Instance $true
            return 0
        }
        if(!$saved.Count -or $saved[0].ClassGuid -ine $current.ClassGuid -or $saved[0].Service -ine $expectedOriginal){
            throw 'The original input INF could not be identified. Reinstall the manufacturer package, then associate the device again.'
        }
        $original=Get-InterceptorMapperInfPath $saved[0].Inf
        if($saved[0].Inf -like 'oem*.inf' -and (Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash -ne $saved[0].Sha256){
            throw 'The original input package changed. Reinstall the current manufacturer package before switching.'
        }
        $wasPaused=Test-InterceptorMapperPaused $Instance
        Set-InterceptorMapperPreference $Instance $true
        try{return Invoke-InterceptorMapperBind $Helper $current $original $expectedOriginal $saved[0].Inf $saved[0]}
        catch{
            if(!$wasPaused -and !$script:InterceptorProcessTimedOut){
                $snapshot=$null
                try{$snapshot=Get-InterceptorInputMapper $Instance}catch{}
                if($snapshot -and $snapshot.Service -eq $script:InterceptorService){Set-InterceptorMapperPreference $Instance $false}
            }
            throw
        }
    }
    if($current.Service -eq $script:InterceptorService){
        Set-InterceptorMapperPreference $Instance $false
        return 0
    }
    $original=Save-InterceptorMapperOriginal $State $current
    $new=Get-InterceptorCurrentMapperPackage ($expectedOriginal -eq 'mouhid')
    try{
        $code=Invoke-InterceptorMapperBind $Helper $current $new.Path $script:InterceptorService $new.Inf
        Set-InterceptorMapperPreference $Instance $false
        return $code
    }catch{
        if(!$script:InterceptorProcessTimedOut){
            $restore=Get-InterceptorInputMapper $Instance
            if($restore.Service -eq $script:InterceptorService){
                Set-InterceptorMapperPreference $Instance $true
                $rollback=Invoke-InterceptorMapperBind $Helper $restore (Get-InterceptorMapperInfPath $original.Inf) $original.Service $original.Inf $original
                if($rollback -eq 3010){throw 'Changing the input driver failed. Restart Windows to restore the original driver.'}
            }
        }
        throw
    }
}
function Enable-InterceptorManufacturerMappers($State,[string]$Helper) {
    $code=0
    foreach($device in [InterceptorRecovery.Devices]::Snapshot()){
        if($device.Instance -notlike 'HID\*' -or $device.Service -notin @('kbdhid','mouhid') -or
           $device.Inf -notmatch '\Aoem[0-9]+\.inf\z' -or (Test-InterceptorMapperPaused $device.Instance)){continue}
        $present=Get-PnpDevice -InstanceId $device.Instance -PresentOnly -ErrorAction SilentlyContinue
        if(!$present -or $present.Status -ne 'OK'){continue}
        try{$result=Set-InterceptorMapperMode $State $device.Instance Interceptor $Helper}
        catch [ComponentModel.Win32Exception]{
            if($_.Exception.NativeErrorCode -ne 50){throw}
            $script:InterceptorLastExitCode=0
            Write-Warning ('Unsupported HID input profile; original driver retained: '+$device.Instance)
            continue
        }
        if($result -eq 3010){$code=3010}
    }
    return $code
}

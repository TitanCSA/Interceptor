. (Join-Path $PSScriptRoot 'Common.ps1')
$code=1;$logged=$false
try {
    Assert-Administrator;Enter-InterceptorOperation
    Import-InterceptorRecoveryAssembly
    $state=Read-InterceptorState
    if(!$state -or $state.PendingUninstall -or !$state.ControllerAssociation.Pending){$code=0}
    else {
        Start-InterceptorLog 'complete-association';$logged=$true
        $payload=Join-Path $PSScriptRoot 'Payload'
        Assert-InterceptorPayload $payload|Out-Null
        $expected=(Get-FileHash -LiteralPath (Join-Path $payload 'Interceptor.sys') -Algorithm SHA256).Hash
        if($state.ControllerAssociation.DriverSHA256 -cne $expected){throw 'Un autre paquet a demande cette association. Reouvrir son installeur ; aucune association modifiee.'}
        $boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
        if($state.ControllerAssociation.Boot -eq $boot){
            Write-Host 'Windows doit redemarrer avant la suite des associations. Aucun peripherique redemarre par ce script.';$code=3010
        } else {
            $driver=Get-CimInstance Win32_SystemDriver -Filter "Name='Interceptor'"
            $driverPath=[string]$driver.PathName
            if($driverPath.StartsWith('\SystemRoot\',[StringComparison]::OrdinalIgnoreCase)){$driverPath=Join-Path $env:WINDIR $driverPath.Substring(12)}
            $driverPath=$driverPath -replace '^\\\?\?\\',''
            if(!$driver -or $driver.State -ne 'Running' -or (Get-FileHash -LiteralPath $driverPath -Algorithm SHA256).Hash -cne $expected){throw 'Le pilote attendu ne fonctionne pas apres redemarrage. Association differee conservee.'}
            $helper=Join-Path $payload 'InterceptorExample.exe'
            & $helper --status
            if($LASTEXITCODE){throw 'API du pilote indisponible apres redemarrage.'}
            # Include devices first connected after the initial installation
            # in the same protected restoration journal before editing filters.
            $state=Save-InterceptorBaseline
            # Finish only the deferred input associations. Do not repeat mapper
            # installation or rebind the root control endpoint on every launch.
            & $helper --init-all --quiet
            $code=$LASTEXITCODE
            $state.ControllerAssociation.Pending=($code -ne 0)
            $state.ControllerAssociation.Boot=$boot
            $state.ControllerAssociation.LastError=if($code -in @(0,3010)){0}else{$code}
            Write-InterceptorState $state
            if($code -notin @(0,3010)){throw ('Association des manettes en echec : '+$code)}
            Write-Host $(if($code -eq 0){'Associations terminees. La capture reste a verifier dans le diagnostic.'}else{'Manettes associees ; Windows demande un dernier redemarrage pour charger leurs filtres.'})
        }
    }
}catch{Write-Host $_.Exception.Message;$code=1}
finally{if($logged){Stop-Transcript|Out-Null};Exit-InterceptorOperation}
exit $code

. (Join-Path $PSScriptRoot 'Common.ps1')
$code=1;$logged=$false;$bindingStarted=$false;$stagingStarted=$false;$existingPackages=@()
try {
    Assert-Administrator;Enter-InterceptorOperation;Start-InterceptorLog 'install';$logged=$true
    $payload=Join-Path $PSScriptRoot 'Payload'
    $mode=Assert-InterceptorPayload $payload
    if($mode -eq 'Test' -and !(Test-RunningTestMode)){throw 'Le mode test doit etre actif apres redemarrage pour ce paquet de laboratoire.'}
    $baseline=Save-InterceptorBaseline
    $baseline.PendingUninstall=$false;$baseline.FinalizeAttempts=0;Write-InterceptorState $baseline
    Remove-InterceptorFinalCleanup
    Install-InterceptorRecovery
    if($mode -eq 'Test') {
        $cert=New-Object Security.Cryptography.X509Certificates.X509Certificate2((Join-Path $payload 'InterceptorTest.cer'))
        Register-InterceptorCertificateAdds $baseline $cert.Thumbprint
        foreach($store in @('Root','TrustedPublisher')) { Import-Certificate -FilePath (Join-Path $payload 'InterceptorTest.cer') -CertStoreLocation ("Cert:\LocalMachine\"+$store) | Out-Null }
        foreach($name in @('Interceptor.sys','InterceptorExample.exe','InterceptorKeyboard.cat','InterceptorMouse.cat','InterceptorControl.cat')) {
            $signature=Get-AuthenticodeSignature -LiteralPath (Join-Path $payload $name)
            if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw "Signature invalide: $name" }
        }
    } else {
        Write-Host 'Pilote et catalogues signes Microsoft. Mode test et certificat de developpement non requis.'
    }
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
        & (Join-Path $payload 'InterceptorExample.exe') --init-all --quiet
        $code=$LASTEXITCODE
        if($code -notin @(0,3010)) { throw "Initialisation des peripheriques en echec: $code" }
    }
    Set-InterceptorPendingControllers $code (Get-FileHash -LiteralPath (Join-Path $payload 'Interceptor.sys') -Algorithm SHA256).Hash
    Get-InterceptorPackages | Select-Object Driver,OriginalFileName | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $env:ProgramData 'Interceptor\packages.json') -Encoding UTF8
    Write-Host 'Interceptor installe. Les entrees passent normalement tant qu aucun contexte ne capture.'
} catch {
    Write-Host $_.Exception.Message
    if($bindingStarted) {
        Write-Host 'Tentative de retour aux pilotes HID Windows...'
        & (Join-Path $PSScriptRoot 'Payload\InterceptorExample.exe') --restore
        if($LASTEXITCODE -notin @(0,3010)) { Write-Host 'Restauration incomplete: utiliser Desinstaller.bat en mode sans echec.' }
    }
    if($stagingStarted) {
        try {
            foreach($package in (Get-InterceptorPackages | Where-Object { $_.Driver -notin $existingPackages })) {
                Remove-InterceptorPackage $package.Driver
            }
        } catch { Write-Host "Nettoyage des nouveaux paquets incomplet: $($_.Exception.Message)" }
    }
    $code=1
} finally { if($logged) { Stop-Transcript | Out-Null };Exit-InterceptorOperation }
exit $code

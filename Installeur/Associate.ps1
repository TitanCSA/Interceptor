. (Join-Path $PSScriptRoot 'Common.ps1')
$code=1;$logged=$false
try {
    Assert-Administrator;Enter-InterceptorOperation;Start-InterceptorLog 'association';$logged=$true
    $payload=Join-Path $PSScriptRoot 'Payload'
    $helper=Join-Path $payload 'InterceptorExample.exe'
    $mode=Assert-InterceptorPayload $payload
    Save-InterceptorBaseline|Out-Null
    if($mode -eq 'Test') {
        if(!(Test-RunningTestMode)) { throw 'Le mode test doit etre actif. Voir Activer-Mode-Test.bat et redemarrer si necessaire.' }
        $cert=New-Object Security.Cryptography.X509Certificates.X509Certificate2((Join-Path $payload 'InterceptorTest.cer'))
        $signature=Get-AuthenticodeSignature -LiteralPath $helper
        if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw 'Helper non approuve. Executer Installer.bat une premiere fois avec ce paquet.' }
    }
    # Bind using INF files already present in the Driver Store; do not stage or
    # upgrade the bundled driver and do not import certificates in this command.
    $packages=@(Get-InterceptorPackages)
    $paths=@()
    foreach($name in @('interceptorkeyboard.inf','interceptormouse.inf','interceptorcontrol.inf')) {
        $package=$packages | Where-Object { [IO.Path]::GetFileName($_.OriginalFileName) -eq $name } |
            Sort-Object { [version]$_.Version } -Descending | Select-Object -First 1
        if(!$package -or !(Test-Path -LiteralPath $package.OriginalFileName)) { throw 'Pilote absent du magasin Windows. Executer Installer.bat une premiere fois.' }
        $paths+= $package.OriginalFileName
    }
    & $helper --associate $paths[0] $paths[1] $paths[2]
    $code=$LASTEXITCODE
    if($code -notin @(0,3010)) { throw "Association interrompue ($code). Certaines associations peuvent avoir reussi ; consulter ce journal puis relancer le script." }
    if($code -eq 0) {
        & $helper --init-all --quiet
        $code=$LASTEXITCODE
        if($code -notin @(0,3010)) { throw "Initialisation des manettes en echec: $code" }
    }
    Set-InterceptorPendingControllers $code (Get-FileHash -LiteralPath (Join-Path $payload 'Interceptor.sys') -Algorithm SHA256).Hash
    Write-Host 'Association terminee. Les pilotes tiers sont laisses en place.'
    if($code -eq 3010) { Write-Host 'Windows demande un redemarrage pour terminer.' }
} catch { Write-Host $_.Exception.Message;$code=1 }
finally { if($logged) { Stop-Transcript | Out-Null };Exit-InterceptorOperation }
exit $code

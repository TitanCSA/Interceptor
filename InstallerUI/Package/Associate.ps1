. (Join-Path $PSScriptRoot 'Common.ps1')
$code=1;$logged=$false
try {
    Assert-Administrator
    $payload=Join-Path $PSScriptRoot 'Payload'
    $mode=Assert-InterceptorPayload $payload
    Assert-InterceptorUsbPackage $payload
    Enter-InterceptorOperation;Start-InterceptorLog 'association';$logged=$true
    $payload=Join-Path $PSScriptRoot 'Payload'
    $helper=Join-Path $payload 'InterceptorExample.exe'
    $mode=Assert-InterceptorPayload $payload
    Save-InterceptorBaseline|Out-Null
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
    $code=Initialize-InterceptorControllers $helper
    Set-InterceptorPendingControllers $code (Get-FileHash -LiteralPath (Join-Path $payload 'Interceptor.sys') -Algorithm SHA256).Hash
    Write-Host 'Association terminee. Les pilotes tiers sont laisses en place.'
    if($code -eq 3010) { Write-Host 'Windows demande un redemarrage pour terminer.' }
} catch { Write-Host $_.Exception.Message;$code=1 }
finally { if($logged) { Stop-Transcript | Out-Null };Exit-InterceptorOperation }
exit $code

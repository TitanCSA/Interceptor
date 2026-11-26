param(
    [Parameter(Mandatory=$true)][ValidateSet('Original','Interceptor')][string]$Mode,
    [Parameter(Mandatory=$true)][string]$Instance
)
. (Join-Path $PSScriptRoot 'Common.ps1')
$code=1
try {
    Assert-Administrator
    Enter-InterceptorOperation
    Assert-InterceptorInstallationBoundary
    Import-InterceptorRecoveryAssembly
    $state=Read-InterceptorState
    if(!$state){throw 'Install this Interceptor edition before changing an input binding.'}
    $helper=Join-Path $PSScriptRoot 'Payload\InterceptorExample.exe'
    $code=Set-InterceptorMapperMode $state $Instance $Mode $helper
}catch {
    Write-Host $_.Exception.Message
    if($_.Exception -is [ComponentModel.Win32Exception]){$code=$_.Exception.NativeErrorCode}
    elseif($script:InterceptorLastExitCode -and $script:InterceptorLastExitCode -notin @(0,3010)){$code=$script:InterceptorLastExitCode}
}finally {
    Exit-InterceptorOperation
}
exit $code

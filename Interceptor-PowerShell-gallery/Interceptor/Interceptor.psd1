@{
RootModule = 'Interceptor.psm1'
ModuleVersion = '1.0.0'
GUID = 'cf0650be-a9ac-5d6f-92bd-e55c82ac93e5'
Author = 'TitanCSA'
Description = 'Windows x64 Interceptor input SDK for PowerShell 7.4+. Driver installed separately.'
PowerShellVersion = '7.4'
CompatiblePSEditions = @('Core')
FunctionsToExport = @('Connect-InterceptorSDK','Initialize-InterceptorSDK','Get-InterceptorSDKInfo','Get-InterceptorSDKDevice','Receive-InterceptorSDKInput','Send-InterceptorSDKBatch','Remove-InterceptorSDKBatch','Stop-InterceptorSDKWait','Get-InterceptorSDKDescriptor','Send-InterceptorSDKInput','Disconnect-InterceptorSDK')
CmdletsToExport = @()
VariablesToExport = @()
AliasesToExport = @()
PrivateData = @{ PSData = @{ Tags = @('Windows','SDK','Keyboard','Mouse','Gamepad'); LicenseUri = 'https://github.com/TitanCSA/Interceptor/blob/main/LICENSE'; ProjectUri = 'https://github.com/TitanCSA/Interceptor'; ReleaseNotes = 'First PowerShell SDK; docs and native bridge included.' } }
}

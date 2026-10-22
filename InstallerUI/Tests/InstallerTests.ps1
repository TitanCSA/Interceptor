param([Parameter(Mandatory=$true)][string]$ReportDirectory)
$ErrorActionPreference='Stop'
$source=Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Path $ReportDirectory -Force|Out-Null
. (Join-Path $source 'Package\Common.ps1')
$checks=0
function Expect($Condition,[string]$Message){if(!$Condition){throw $Message};$script:checks++}
function CheckBoundary([string[]]$Present,[bool]$ExpectedBlocked){
 function Test-Path {param([string]$LiteralPath) return (@($Present|Where-Object {$LiteralPath -eq ('HKLM:\SYSTEM\CurrentControlSet\Services\'+$_)}).Count -gt 0)}
 $blocked=$false;$script:InterceptorLastExitCode=0
 try{Assert-InterceptorInstallationBoundary}catch{if($script:InterceptorLastExitCode -ne 1638){throw};$blocked=$true}
 Expect ($blocked -eq $ExpectedBlocked) 'Edition boundary rejected a compatible product or accepted a conflicting product'
}
CheckBoundary @() $false
CheckBoundary @($script:InterceptorService) $false
CheckBoundary @('Interceptor') ($script:InterceptorService -ne 'Interceptor')
CheckBoundary @('InterceptorKeyboardMouse','InterceptorGamepadController') ($script:InterceptorService -eq 'Interceptor')

$fixture=Join-Path $ReportDirectory ('SetupFixture-'+[guid]::NewGuid().ToString('N')+'.exe')
$code=@'
using System;using System.Linq;using System.Threading;using System.Web.Script.Serialization;
class SetupFixture {
 static int Main(string[] args){
  Console.OutputEncoding=new System.Text.UTF8Encoding(false);
  if(args[0]=="sleep"){Thread.Sleep(30000);return 0;}
  if(args[0]=="echo"){Console.Write(new JavaScriptSerializer().Serialize(args.Skip(1).ToArray()));return 0;}
  if(args[0]=="flood"){Console.Write(new string('O',131072));Console.Error.Write(new string('E',131072));return 7;}
  Console.Write("stdout");Console.Error.Write("stderr");return int.Parse(args[0]);
 }
}
'@
Add-Type -TypeDefinition $code -OutputAssembly $fixture -OutputType ConsoleApplication -ReferencedAssemblies 'System.Web.Extensions.dll'
foreach($exitCode in @(0,7,3010,259,-1)){
 $r=Invoke-InterceptorProcess $fixture @([string]$exitCode) -Capture
 Expect ($r.ExitCode -eq $exitCode) ('Exit code lost: '+$exitCode)
 Expect ($r.Output -eq 'stdout' -and $r.Error -eq 'stderr') 'Process output lost'
}
for($i=0;$i -lt 75;$i++){Expect ((Invoke-InterceptorProcess $fixture @('0') -Capture).ExitCode -eq 0) 'Fast exit race'}
$arguments=@('','spaces in argument','embedded"quote','ending\','\\server\share\','a\"b',[string][char]0x00E9)
$r=Invoke-InterceptorProcess $fixture (@('echo')+$arguments) -Capture
$actual=$r.Output|ConvertFrom-Json
Expect (@(Compare-Object $arguments @($actual) -CaseSensitive).Count -eq 0) 'Windows argv quoting/Unicode changed'
$r=Invoke-InterceptorProcess $fixture @('flood') -Capture
Expect ($r.ExitCode -eq 7 -and $r.Output.Length -eq 131072 -and $r.Error.Length -eq 131072) 'Concurrent large stdout/stderr failed'
$r=Invoke-InterceptorProcess (Get-InterceptorWindowsTool 'pnputil.exe') @('/?') -Capture
Expect ($r.ExitCode -eq 0 -and !$r.Output.Contains([string][char]0xFFFD)) 'Windows utility output encoding/exit status lost'
$clock=[Diagnostics.Stopwatch]::StartNew();$timedOut=$false
try{Invoke-InterceptorProcess $fixture @('sleep') -TimeoutMs 150 -Capture|Out-Null}catch [TimeoutException]{$timedOut=$true}catch{if($_.Exception.Message -like '*timed out*'){$timedOut=$true}else{throw}}
Expect ($timedOut -and $script:InterceptorProcessTimedOut -and $script:InterceptorLastExitCode -eq 1460 -and $clock.ElapsedMilliseconds -lt 10000) 'Timeout not bounded or evidence not marked'
$script:InterceptorProcessTimedOut=$false
function Row([string]$Id='ROOT\fixture\0000',[int]$Problem=0,[uint32]$Flags=8,[bool]$Root=$true,[int]$Query=0,[bool]$Owned=$true){
 [pscustomobject]@{instance=$Id;root=$Root;owned=$Owned;status_error=$Query;status=$Flags;problem=$Problem}
}
function State([object[]]$Devices){[pscustomobject]@{schema=1;service=$script:InterceptorService;devices=$Devices}}
$root=Row;$before=State @($root)
Expect (Get-InterceptorSetupAssessment (State @($root)) $before).Ready 'Healthy endpoint rejected'
Expect (!(Get-InterceptorSetupAssessment (State @()) $before).Settled) 'Missing endpoint accepted'
for($i=1;$i -le 57;$i++){
 $a=Get-InterceptorSetupAssessment (State @((Row -Problem $i -Flags 1024))) $before -RestartRequested
 if($i -eq 14){Expect ($a.Settled -and $a.RestartRequired -and !$a.Ready) 'Problem 14 did not require restart'}
 else{Expect (!$a.Ready -and !$a.Settled) ('Problem '+$i+' hidden by restart flag')}
}
Expect (Get-InterceptorSetupAssessment (State @((Row -Flags 256))) $before).RestartRequired 'DN_NEED_RESTART ignored'
Expect (!(Get-InterceptorSetupAssessment (State @((Row -Flags 1032))) $before).Ready) 'DN_HAS_PROBLEM ignored'
Expect (!(Get-InterceptorSetupAssessment (State @((Row -Query 13))) $before).Ready) 'Unknown CM status accepted'
Expect (!(Get-InterceptorSetupAssessment (State @((Row -Owned $false))) $before).Ready) 'Unbound endpoint accepted'
$disabled=Row 'HID\fixture\disabled' 22 1024 $false
$a=Get-InterceptorSetupAssessment (State @($root,$disabled)) (State @($root,$disabled))
Expect ($a.Ready -and @($a.Devices|Where-Object State -EQ Unverified).Count -eq 1) 'Pre-existing disabled device was modified/claimed'
Expect (!(Get-InterceptorSetupAssessment (State @($root,$disabled)) $before).Ready) 'Newly disabled target accepted'
function WaitCase([object[]]$Snapshots,[int]$Deadline=150) {
 $script:snapshots=$Snapshots;$script:reads=0
 function Get-InterceptorSetupState($Helper){
  $index=[Math]::Min($script:reads,$script:snapshots.Count-1);$script:reads++
  if($script:snapshots[$index] -is [Exception]){throw $script:snapshots[$index]}
  return $script:snapshots[$index]
 }
 Wait-InterceptorSetupDevices 'unused' $before -TimeoutMs $Deadline -PollMs 1 -NoReport
}
$waitResult=WaitCase @((State @((Row -Query 13))),$before)
Expect ($waitResult -eq 0) 'Delayed properties did not settle'
Expect ($script:reads -ge 3) 'Verification accepted only one healthy sample'
$waitResult=WaitCase @([Exception]::new('transient query error'),$before)
Expect ($waitResult -eq 0) 'Transient PnP error not retried'
$bad=$false
try{WaitCase @((State @((Row -Problem 10 -Flags 1024)))) -Deadline 35|Out-Null}catch{$bad=$_.Exception.Message -like '*problem 10*'}
Expect $bad 'Permanent PnP failure hidden'
# Root identity must match the actual binding backend, including split editions.
$backend=[IO.File]::ReadAllText((Join-Path $source 'Native\Setup.cpp'))
$entry=[IO.File]::ReadAllText((Join-Path $source 'Native\Main.cpp'))
$hardware=[regex]::Match($backend,'ControlHardware\[\]=L"([^"]+)"').Groups[1].Value
Expect ($hardware -and $entry.Contains('SetupHealth::Print(L"'+$hardware+'")')) 'PnP verification uses a different root identity than installation'
function SingleSampleDeadline {
 function Get-InterceptorSetupState($Helper){Start-Sleep -Milliseconds 35;return $before}
 $rejected=$false
 try{Wait-InterceptorSetupDevices 'unused' $before -TimeoutMs 15 -PollMs 1 -NoReport|Out-Null}catch{$rejected=$_.Exception.Message -like '*consecutive samples*'}
 Expect $rejected 'One late healthy sample was accepted without a stability confirmation'
}
SingleSampleDeadline
$waitResult=WaitCase @($before,[Exception]::new('interrupted sampling'),$before)
Expect ($waitResult -eq 0 -and $script:reads -ge 4) 'Transient error did not reset consecutive-state confirmation'

# Execute the real installation script in a child with a fake Common.ps1.
# Only private files and fixture functions are available; no PnP API is invoked.
$install=[IO.File]::ReadAllText((Join-Path $source 'Package\Install.ps1'))
$sys=[regex]::Match($install,"Join-Path "+'\$payload'+" '([^']+\.sys)'").Groups[1].Value
if(!$sys){throw 'Expected product image in installation source'}
$caseDirectory=Join-Path $ReportDirectory 'Transactions'
New-Item -ItemType Directory -Path (Join-Path $caseDirectory 'Payload') -Force|Out-Null
[IO.File]::WriteAllText((Join-Path $caseDirectory 'Install.ps1'),$install)
[IO.File]::WriteAllText((Join-Path $caseDirectory ('Payload\'+$sys)),'fixture')
$mock=@'
$ErrorActionPreference='Stop'
$case=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'case.json') -Raw|ConvertFrom-Json
$events=New-Object Collections.Generic.List[string]
$script:InterceptorLastExitCode=0;$script:InterceptorProcessTimedOut=$false;$script:InterceptorNeedsReboot=$false
function Assert-Administrator {}
function Assert-InterceptorPayload($p) {}
function Assert-InterceptorUsbPackage($p) {}
function Get-InterceptorWindowsTool($n){return 'unused'}
function Enter-InterceptorOperation {}
function Assert-InterceptorInstallationBoundary {}
function Start-InterceptorLog($n){$events.Add('log')}
function Stop-Transcript {}
function Get-InterceptorDataRoot {return $PSScriptRoot}
function Set-InterceptorPhase($p){$events.Add($p)}
function Save-InterceptorBaseline {return [pscustomobject]@{PendingUninstall=$false;FinalizeAttempts=0}}
function Write-InterceptorState($s) {}
function Get-InterceptorSetupState($h){return [pscustomobject]@{devices=@()}}
function Get-InterceptorPackages {
 [pscustomobject]@{Driver='oem1.inf';OriginalFileName='existing'}
 if($script:staged){[pscustomobject]@{Driver='oem2.inf';OriginalFileName='new'}}
}
function Remove-InterceptorFinalCleanup {}
function Install-InterceptorRecovery {$events.Add('recovery')}
function Add-InterceptorPackage($p){$script:staged=$true;if($case.StageRestart){$script:InterceptorNeedsReboot=$true}}
function Invoke-InterceptorSetupHelper($h,$arguments,$timeout=0){
 $mode=$arguments[0];$events.Add($mode)
 if($mode -eq '--install' -and $case.Fail){
  $script:InterceptorLastExitCode=if($case.Timeout){1460}else{7}
  if($case.Timeout){$script:InterceptorProcessTimedOut=$true;throw 'fixture timeout'}
  return 7
 }
 if($mode -eq '--restore'){return [int]$case.Restore}
 return 0
}
function Initialize-InterceptorControllers($h){return 0}
function Wait-InterceptorSetupDevices {param($h,$before,[switch]$RestartRequested);$events.Add('verify');return $(if($RestartRequested){3010}else{0})}
function Set-InterceptorPendingControllers($c,$h){$events.Add('pending='+$c)}
function Complete-InterceptorOperation($c){$events.Add('complete='+$c)}
function Write-InterceptorFailure($e){return $script:InterceptorLastExitCode}
function Remove-InterceptorPackage($n){$events.Add('remove='+$n)}
function Exit-InterceptorOperation {[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'events.json'),(ConvertTo-Json -InputObject @($events.ToArray())))}
'@
[IO.File]::WriteAllText((Join-Path $caseDirectory 'Common.ps1'),$mock)
$pwsh=Join-Path ([Environment]::SystemDirectory) 'WindowsPowerShell\v1.0\powershell.exe'
foreach($c in @(
 @{Fail=$false;Restore=0;Timeout=$false;StageRestart=$false},
 @{Fail=$false;Restore=0;Timeout=$false;StageRestart=$true},
 @{Fail=$true;Restore=3010;Timeout=$false;StageRestart=$false},
 @{Fail=$true;Restore=7;Timeout=$false;StageRestart=$false},
 @{Fail=$true;Restore=0;Timeout=$false;StageRestart=$false},
 @{Fail=$true;Restore=0;Timeout=$true;StageRestart=$false}
)){
 [IO.File]::WriteAllText((Join-Path $caseDirectory 'case.json'),($c|ConvertTo-Json))
 $r=Invoke-InterceptorProcess $pwsh @('-NoProfile','-NonInteractive','-File',(Join-Path $caseDirectory 'Install.ps1')) -Capture
 $events=Get-Content -LiteralPath (Join-Path $caseDirectory 'events.json') -Raw|ConvertFrom-Json
 if(!$c.Fail){Expect ($r.ExitCode -eq $(if($c.StageRestart){3010}else{0})) 'Staging reboot code lost';Expect ('verify' -in $events) 'Success without PnP verification'}
 elseif($c.Timeout){Expect ($r.ExitCode -eq 1460 -and '--restore' -notin $events -and 'remove=oem2.inf' -notin $events) 'Timeout caused competing rollback/deletion'}
 else{Expect ($r.ExitCode -eq 7) 'Original failure code lost';Expect (('remove=oem2.inf' -in $events) -eq ($c.Restore -eq 0)) 'Unsafe package removal before restoration completed'}
 Expect ('remove=oem1.inf' -notin $events) 'Existing package removed by failed installation'
}

# Do not hide a failed deferred association behind a successful PnP query.
$completionDirectory=Join-Path $ReportDirectory 'DeferredAssociation'
New-Item -ItemType Directory -Path (Join-Path $completionDirectory 'Payload') -Force|Out-Null
[IO.File]::WriteAllText((Join-Path $completionDirectory ('Payload\'+$sys)),'fixture')
Copy-Item -LiteralPath (Join-Path $source 'Package\CompleteAssociation.ps1') -Destination $completionDirectory
$completionMock=@'
$ErrorActionPreference='Stop'
$script:fixtureSys=Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'Payload') -Filter '*.sys'|Select-Object -First 1 -ExpandProperty FullName
$script:InterceptorLastExitCode=0;$script:healthReached=$false
$script:state=[pscustomobject]@{Schema=1;PendingUninstall=$false;Devices=@();ControllerAssociation=[pscustomobject]@{Pending=$true;DriverSHA256=(Get-FileHash -LiteralPath $script:fixtureSys).Hash;Boot='earlier';LastError=0;UsbMigrationVerified=$false}}
function Assert-Administrator {}
function Enter-InterceptorOperation {}
function Exit-InterceptorOperation {[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'result.json'),([pscustomobject]@{HealthReached=$script:healthReached}|ConvertTo-Json))}
function Import-InterceptorRecoveryAssembly {}
function Read-InterceptorState {return $script:state}
function Write-InterceptorState($s) {$script:state=$s}
function Start-InterceptorLog($s) {}
function Stop-Transcript {}
function Assert-InterceptorPayload($p) {}
function Assert-InterceptorUsbPackage($p) {}
function Get-CimInstance($ClassName,$Filter){if($ClassName -eq 'Win32_OperatingSystem'){return [pscustomobject]@{LastBootUpTime=[DateTime]::UtcNow}};return [pscustomobject]@{State='Running';PathName=$script:fixtureSys}}
function Invoke-InterceptorSetupHelper($h,$a){if($a[0] -eq '--status'){return 0};$script:InterceptorLastExitCode=7;return 7}
function Set-InterceptorPhase($p) {}
function Save-InterceptorBaseline {return $script:state}
function Get-InterceptorSetupState($h){return [pscustomobject]@{devices=@()}}
function Initialize-InterceptorControllers($h){$script:InterceptorLastExitCode=7;throw 'Fixture initialization failed'}
function Wait-InterceptorSetupDevices {$script:healthReached=$true;return 0}
function Complete-InterceptorOperation($c) {}
function Write-InterceptorFailure($e){return $script:InterceptorLastExitCode}
'@
[IO.File]::WriteAllText((Join-Path $completionDirectory 'Common.ps1'),$completionMock)
$r=Invoke-InterceptorProcess $pwsh @('-NoProfile','-NonInteractive','-File',(Join-Path $completionDirectory 'CompleteAssociation.ps1')) -Capture
$completion=Get-Content -LiteralPath (Join-Path $completionDirectory 'result.json') -Raw|ConvertFrom-Json
Expect ($r.ExitCode -eq 7 -and !$completion.HealthReached) 'Deferred association failure was hidden by PnP verification'

$managed=Join-Path $ReportDirectory ('ManagedTests-'+[guid]::NewGuid().ToString('N')+'.exe')
Add-Type -Path @((Join-Path $source 'Package.cs'),(Join-Path $PSScriptRoot 'ManagedInstallerTests.cs')) -OutputAssembly $managed -OutputType ConsoleApplication -ReferencedAssemblies 'System.dll','System.Core.dll','System.Web.Extensions.dll','System.IO.Compression.dll'
$managedResult=Invoke-InterceptorProcess $managed @($fixture) -Capture
Expect ($managedResult.ExitCode -eq 0) ('Managed installer regressions: '+$managedResult.Error)
Write-Host $managedResult.Output.TrimEnd()

# Test the production uninstall policy with simulated Windows/Driver Store APIs.
Add-Type -TypeDefinition 'namespace InterceptorRecovery { public static class Devices { public static bool CleanClassFilters(){return false;} } }'
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $source 'Package\Lifecycle.ps1'),[ref]$tokens,[ref]$errors)
$functions=@($ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -in @('Invoke-InterceptorCleanUninstall','Set-InterceptorUninstallPending')},$true))
Expect ($functions.Count -eq 2) 'Uninstall policy functions missing'
$literalRoot="'"+(Join-Path $source 'Package').Replace("'","''")+"'"
foreach($definition in $functions){. ([scriptblock]::Create($definition.Extent.Text.Replace('$PSScriptRoot',$literalRoot)))}
function UninstallCase([int]$Restore,[bool]$Owned,[bool]$PackageRestart=$false) {
 $script:uninstallEvents=New-Object Collections.Generic.List[string]
 $script:restoreCode=$Restore;$script:ownedAfter=$Owned;$script:packageRestart=$PackageRestart;$script:removedPackages=$false
 $script:InterceptorNeedsReboot=$false
 function Import-InterceptorRecoveryAssembly {}
 function Stop-InterceptorRecovery {$script:uninstallEvents.Add('stop-recovery')}
 function Read-InterceptorState {return $null}
 function Set-InterceptorPhase($p) {}
 function Invoke-InterceptorSetupHelper {return $script:restoreCode}
 function Get-InterceptorSetupState {return [pscustomobject]@{devices=$(if($script:ownedAfter){@([pscustomobject]@{owned=$true})}else{@()})}}
 function Get-InterceptorPackages {if(!$script:removedPackages){[pscustomobject]@{Driver='oem1.inf'}}}
 function Remove-InterceptorPackage($p){$script:uninstallEvents.Add('remove-package');$script:removedPackages=$true;$script:InterceptorNeedsReboot=$script:packageRestart}
 function Get-Service {}
 function Remove-InterceptorRecoveryService {$script:uninstallEvents.Add('remove-recovery')}
 function Remove-InterceptorFinalCleanup {$script:uninstallEvents.Add('remove-task')}
 function Register-InterceptorFinalCleanup {$script:uninstallEvents.Add('schedule-cleanup')}
 function Write-InterceptorState($s){$script:uninstallEvents.Add('pending='+$s.PendingUninstall)}
 Invoke-InterceptorCleanUninstall
}
$u=UninstallCase 3010 $false
Expect ($u -eq 3010 -and 'remove-package' -notin $script:uninstallEvents -and 'remove-recovery' -notin $script:uninstallEvents -and 'schedule-cleanup' -in $script:uninstallEvents) 'Pending input restart removed necessary packages/recovery'
$u=UninstallCase 0 $true
Expect ($u -eq 3010 -and 'remove-package' -notin $script:uninstallEvents) 'Still-associated device lost its package'
$u=UninstallCase 0 $false
Expect ($u -eq 0 -and 'remove-package' -in $script:uninstallEvents -and 'remove-recovery' -in $script:uninstallEvents) 'Clean uninstallation did not finish'
$u=UninstallCase 0 $false $true
Expect ($u -eq 3010 -and 'schedule-cleanup' -in $script:uninstallEvents) 'Package deletion restart was not retained'
$failed=$false
try{UninstallCase 7 $false|Out-Null}catch{$failed=$true}
Expect ($failed -and 'remove-package' -notin $script:uninstallEvents) 'Failed input restoration removed driver packages'

[ordered]@{Passed=$true;Checks=$checks;FastProcessExits=75;WindowsProblemCodes=57;TransactionScenarios=6;NoHostDriverChanges=$true;MaintenanceSHA256=(Get-FileHash -LiteralPath (Join-Path $source 'Package\Maintenance.ps1')).Hash}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $ReportDirectory 'installer-tests.json') -Encoding UTF8
Write-Host ("PASS installer robustness: "+$checks+" checks; real process/quoting/timeout fixtures, PnP assessment and installation rollback scenarios. No host driver changed.")


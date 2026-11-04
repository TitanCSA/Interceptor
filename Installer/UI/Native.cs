using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace InterceptorSetup {
 public sealed class DeviceRow {
  public string Kind {get;set;} public string Name {get;set;} public string VidPid {get;set;}
  public string Service {get;set;} public string Association {get;set;} public bool Associated {get;set;} public bool InterfaceActive {get;set;} public string Instance {get;set;} public string InstalledInf {get;set;}
  public string ServiceDisplay {get{return string.IsNullOrEmpty(Service)?"Uses parent service":Service;}}
  public string Explanation {get{
   if(Kind=="Control")return "Software access point for the Interceptor API, with no physical device or input to capture.";
   if(Kind=="XUSB")return "Wired Xbox USB transport. The current filter belongs below xusb22, so Windows keeps its native XInput, browser and HID views. No separate HID mirror association is required.";
   if(Kind=="Xbox HID view")return "Secondary Windows view of an XUSB/GIP controller: capture occurs on the parent. It does not need a separate association.";
   if(Kind=="XInputHID")return "XInputHID collection. Use its XUSB/GIP parent when present; direct profiles are checked separately.";
   return "Windows input node. Interceptor must be associated at the correct level for this profile; a published interface does not prove the reader is ready.";
  }}
  public string HardwareIds {get;set;} public string WindowsState {get;set;} public string Filters {get;set;} public string ProfileSupport {get;set;}
  public string CompatibleIds {get;set;} public string UpperFilters {get;set;} public string LowerFilters {get;set;} public string ClassFilters {get;set;}
  public string ClassUpperFilters {get;set;} public string ClassLowerFilters {get;set;}
  public string ParentInstance {get;set;} public string DriverVersion {get;set;} public string DriverProvider {get;set;}
  public uint ProblemCode {get;set;} public uint PnpStatus {get;set;} public bool PnpQuerySucceeded {get;set;} public int? PropertyFlags {get;set;}
 }
 public sealed class Snapshot {
  public string Timestamp {get;set;} public string Os {get;set;} public string DriverState {get;set;} public bool DriverRunning {get;set;} public bool ApiAvailable {get;set;}
  public string DriverPath {get;set;} public string DriverSha256 {get;set;} public string ApiState {get;set;} public string ApiDetail {get;set;}
  public string EmbeddedDriverSha256 {get;set;} public bool? DriverFileMatchesPayload {get;set;} public string DriverHashScope {get;set;}
  public List<string> StagedPackages {get;set;}
  public bool CapabilitiesAvailable {get;set;} public uint ControllerMask {get;set;} public string ControllerProfiles {get;set;}
  public string TestMode {get;set;} public string PackageState {get;set;} public string Error {get;set;}
  public string DriverSigningMode {get;set;}
  public string RecoveryState {get;set;}
  public string TrialState {get;set;} public uint? TrialCode {get;set;} public ulong TrialRemaining100ns {get;set;}
  public bool? DeviceRestartPending {get;set;}
  public List<DeviceRow> Devices {get;set;} public uint KeyboardQueued {get;set;} public uint MouseQueued {get;set;} public uint HidQueued {get;set;} public uint LastResetReason {get;set;} public ulong Overflows {get;set;}
 }
 [StructLayout(LayoutKind.Sequential)] struct DevInfo {public uint Size;public Guid Class;public uint Instance;public IntPtr Reserved;}
 [StructLayout(LayoutKind.Sequential)] struct InterfaceInfo {public uint Size;public Guid Class;public uint Flags;public IntPtr Reserved;}
 [StructLayout(LayoutKind.Sequential)] struct IcStatus {public uint Version,Mask,KeyboardCount,MouseCount,Keyboards,MouseDevices,Reset,Capacity;public ulong Generation,Overflows,Frequency;public uint HidCount,Hids;}
 [StructLayout(LayoutKind.Sequential)] struct IcCapabilities {public uint Size,Version,Abi,Inputs,Controllers,Flags,MaxDevices,QueueCapacity,MaxSend,HidReportMax,XusbProfiles,XihProfiles,GipProfiles,Reserved0,Reserved1,Reserved2;}
 [StructLayout(LayoutKind.Sequential)] struct IcTrial {public uint Size,Version,State,Error;public ulong Duration,FirstContext,Elapsed,Remaining;public uint ActiveMask,Reserved0,Reserved1,Reserved2;}
 [StructLayout(LayoutKind.Sequential)] struct CiInfo {public uint Length,Options;}
 [StructLayout(LayoutKind.Sequential)] struct ServiceStatus {public uint Type,State,Controls,Win32Exit,ServiceExit,Checkpoint,WaitHint;}
 [StructLayout(LayoutKind.Sequential)] public struct RawRegistration {public ushort Page,Usage;public uint Flags;public IntPtr Target;}
 public static class Native {
  const uint StatusIoctl=(0x8349u<<16)|(3u<<14)|(0xA05u<<2);
  const uint CapabilitiesIoctl=(0x8349u<<16)|(3u<<14)|(0xA26u<<2);
  const uint TrialIoctl=(0x8349u<<16)|(3u<<14)|(0xA2Du<<2);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr SetupDiGetClassDevs(IntPtr guid,string enumerator,IntPtr hwnd,uint flags);
  [DllImport("setupapi.dll",SetLastError=true)] static extern bool SetupDiEnumDeviceInfo(IntPtr set,uint index,ref DevInfo info);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool SetupDiGetDeviceRegistryProperty(IntPtr set,ref DevInfo info,uint property,out uint type,byte[] data,uint size,out uint required);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool SetupDiGetDeviceInstanceId(IntPtr set,ref DevInfo info,StringBuilder data,uint size,out uint required);
  [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,EntryPoint="SetupDiGetClassDevsW",SetLastError=true)] static extern IntPtr GetInterfaces(ref Guid guid,string enumerator,IntPtr hwnd,uint flags);
  [DllImport("setupapi.dll",SetLastError=true)] static extern bool SetupDiEnumDeviceInterfaces(IntPtr set,IntPtr dev,ref Guid guid,uint index,ref InterfaceInfo info);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr set,ref InterfaceInfo item,IntPtr detail,uint size,out uint required,ref DevInfo info);
  [DllImport("cfgmgr32.dll")] static extern uint CM_Get_DevNode_Status(out uint status,out uint problem,uint instance,uint flags);
  [DllImport("cfgmgr32.dll")] static extern uint CM_Get_Parent(out uint parent,uint instance,uint flags);
  [DllImport("cfgmgr32.dll",CharSet=CharSet.Unicode)] static extern uint CM_Get_Device_ID(uint instance,StringBuilder id,uint size,uint flags);
  [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int kind,ref CiInfo info,uint size,out uint returned);
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern SafeFileHandle CreateFile(string path,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool DeviceIoControl(SafeFileHandle file,uint code,IntPtr input,uint inputSize,out IcStatus output,uint outputSize,out uint returned,IntPtr overlapped);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool DeviceIoControl(SafeFileHandle file,uint code,IntPtr input,uint inputSize,out IcCapabilities output,uint outputSize,out uint returned,IntPtr overlapped);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool DeviceIoControl(SafeFileHandle file,uint code,IntPtr input,uint inputSize,out IcTrial output,uint outputSize,out uint returned,IntPtr overlapped);
  [DllImport("advapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr OpenSCManager(string machine,string database,uint access);
  [DllImport("advapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr OpenService(IntPtr manager,string name,uint access);
  [DllImport("advapi32.dll",SetLastError=true)] static extern bool QueryServiceStatus(IntPtr service,out ServiceStatus status);
  [DllImport("advapi32.dll")] static extern bool CloseServiceHandle(IntPtr service);
  [DllImport("user32.dll",SetLastError=true)] public static extern bool RegisterRawInputDevices(RawRegistration[] devices,uint count,uint size);
  [DllImport("user32.dll",SetLastError=true)] public static extern uint GetRawInputData(IntPtr raw,uint command,IntPtr data,ref uint size,uint header);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern uint GetRawInputDeviceInfo(IntPtr device,uint command,StringBuilder data,ref uint size);

  static string Property(IntPtr set,ref DevInfo info,uint prop) {uint type,size;var data=new byte[8192];if(!SetupDiGetDeviceRegistryProperty(set,ref info,prop,out type,data,(uint)data.Length,out size))return "";return Encoding.Unicode.GetString(data,0,(int)Math.Min(size,(uint)data.Length)).TrimEnd('\0').Replace('\0',';');}
  static string Id(IntPtr set,ref DevInfo info) {uint size;var b=new StringBuilder(4096);return SetupDiGetDeviceInstanceId(set,ref info,b,(uint)b.Capacity,out size)?b.ToString():"";}
  static HashSet<string> ActiveHid() {
   var result=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
   foreach(var value in new[]{"2de3334d-67cd-4a3a-a22c-048f77ba323e","34c1a1e7-9f5e-4b6c-a713-55b951eacf35","7213ba9d-e7f2-43e6-b374-a5c477312c13","c2543732-f736-49ff-b84d-6eaebdaa46df","eed47f20-11e1-443d-bde7-5db5c8c83f1c"}) {
    Guid guid=new Guid(value);IntPtr set=GetInterfaces(ref guid,null,IntPtr.Zero,18);if(set==new IntPtr(-1))continue;
    try {for(uint i=0;;i++) {var item=new InterfaceInfo{Size=(uint)Marshal.SizeOf(typeof(InterfaceInfo))};if(!SetupDiEnumDeviceInterfaces(set,IntPtr.Zero,ref guid,i,ref item))break;if((item.Flags&1)==0)continue;uint size;var dev=new DevInfo{Size=(uint)Marshal.SizeOf(typeof(DevInfo))};SetupDiGetDeviceInterfaceDetail(set,ref item,IntPtr.Zero,0,out size,ref dev);if(size==0 || size>65536)continue;IntPtr detail=Marshal.AllocHGlobal((int)size);try {Marshal.WriteInt32(detail,IntPtr.Size==8?8:6);if(SetupDiGetDeviceInterfaceDetail(set,ref item,detail,size,out size,ref dev))result.Add(Id(set,ref dev));}finally{Marshal.FreeHGlobal(detail);}}}
    finally{SetupDiDestroyDeviceInfoList(set);}
   }return result;
  }
  static bool Contains(string value,string needle) {return value.IndexOf(needle,StringComparison.OrdinalIgnoreCase)>=0;}
  internal static string KindOf(string cls,string service,string hardware,string compatible,string upper="") {
   string identifiers=hardware+";"+compatible;
   return cls=="Keyboard"?"Keyboard":cls=="Mouse"?"Mouse":Has(hardware,@"ROOT\InterceptorControl")?"Control":Has(upper,"xinputhid") || service.Equals("xinputhid",StringComparison.OrdinalIgnoreCase)?"XInputHID":service.StartsWith("xusb",StringComparison.OrdinalIgnoreCase)?"XUSB":service.Equals("dc1-controller",StringComparison.OrdinalIgnoreCase) || Contains(service,"gip")?"GIP":Contains(hardware,"&IG_")?"Xbox HID view":Has(identifiers,"HID_DEVICE_SYSTEM_GAME") || identifiers.Split(';').Any(GameUsage)?"HID controller":"";
  }
  internal static bool GameUsage(string id){
   if(id.Length!=25||!id.StartsWith("HID_DEVICE_UP:",StringComparison.OrdinalIgnoreCase)||!id.Substring(18,3).Equals("_U:",StringComparison.OrdinalIgnoreCase))return false;
   ushort page,usage;if(!ushort.TryParse(id.Substring(14,4),System.Globalization.NumberStyles.HexNumber,System.Globalization.CultureInfo.InvariantCulture,out page)||!ushort.TryParse(id.Substring(21,4),System.Globalization.NumberStyles.HexNumber,System.Globalization.CultureInfo.InvariantCulture,out usage))return false;
   return page==1&&(usage==4||usage==5||usage==8||usage==15)||page==2&&(usage>=1&&usage<=12||usage==32||usage==33||usage==36)||page==4&&usage>=1&&usage<=4||page==5&&usage>=1&&usage<=3;
  }
  static bool Has(string values,string item){return values.Split(';').Any(x=>x.Equals(item,StringComparison.OrdinalIgnoreCase));}
  static string HexId(string ids,string prefix) {int i=ids.IndexOf(prefix,StringComparison.OrdinalIgnoreCase);return i>=0 && ids.Length>=i+prefix.Length+4?ids.Substring(i+prefix.Length,4).ToUpperInvariant():"----";}
  public static List<DeviceRow> EnumerateDevices() {
   var rows=new List<DeviceRow>();var active=ActiveHid();IntPtr set=SetupDiGetClassDevs(IntPtr.Zero,null,IntPtr.Zero,6);if(set==new IntPtr(-1))throw new Win32Exception();
   try {for(uint i=0;;i++) {var info=new DevInfo{Size=(uint)Marshal.SizeOf(typeof(DevInfo))};if(!SetupDiEnumDeviceInfo(set,i,ref info)) {if(Marshal.GetLastWin32Error()!=259)throw new Win32Exception();break;}
    string cls=Property(set,ref info,7),service=Property(set,ref info,4),ids=Property(set,ref info,1),compatible=Property(set,ref info,2),name=Property(set,ref info,12);if(name.Length==0)name=Property(set,ref info,0);
    string upper=Property(set,ref info,17),lower=Property(set,ref info,18),kind=KindOf(cls,service,ids,compatible,upper);
    if(kind.Length==0)continue;string instance=Id(set,ref info),filters=upper+";"+lower;uint status,problem;bool queried=CM_Get_DevNode_Status(out status,out problem,info.Instance,0)==0;bool started=queried && (status&8)!=0 && (status&0x400)==0;
    string classUpper="",classLower="";using(var classKey=Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\"+info.Class.ToString("B"))){if(classKey!=null){classUpper=String.Join(";",classKey.GetValue("UpperFilters",new string[0]) as string[]??new string[0]);classLower=String.Join(";",classKey.GetValue("LowerFilters",new string[0]) as string[]??new string[0]);}}string classFilters=classUpper+";"+classLower;
    bool associated=service.Equals("Interceptor",StringComparison.OrdinalIgnoreCase) || filters.Split(';').Any(x=>x.Equals("Interceptor",StringComparison.OrdinalIgnoreCase));
    if(kind=="XUSB"&&Has(classFilters,"Interceptor"))associated=true;
    // A published, present interface proves attachment only, never exclusive capture.
    string association=(service.Equals("Interceptor",StringComparison.OrdinalIgnoreCase)||Has(filters,"Interceptor"))?"Original Interceptor also owns this device · uninstall it before migration":active.Contains(instance)?"Interface · capture unverified":associated?"Associated · unverified":"Not associated";
    string inf="",version="",provider="";int? propertyFlags=null;using(var driver=Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\"+Property(set,ref info,9))){if(driver!=null){inf=Convert.ToString(driver.GetValue("InfPath",""));version=Convert.ToString(driver.GetValue("DriverVersion",""));provider=Convert.ToString(driver.GetValue("ProviderName",""));var flagsValue=driver.GetValue("DevicePropertyFlags",null);if(flagsValue is int)propertyFlags=(int)flagsValue;}}
    uint parent;string parentId="";if(CM_Get_Parent(out parent,info.Instance,0)==0){var parentText=new StringBuilder(4096);if(CM_Get_Device_ID(parent,parentText,(uint)parentText.Capacity,0)==0)parentId=parentText.ToString();}
    bool originalXusb=instance.StartsWith(@"USB\VID_045E&PID_028E\",StringComparison.OrdinalIgnoreCase)||instance.StartsWith(@"USB\VID_045E&PID_028E&",StringComparison.OrdinalIgnoreCase);
    bool wiredXusb=Has(compatible,@"USB\MS_COMP_XUSB10")&&Has(compatible,@"USB\Class_ff&SubClass_5d&Prot_01");
    string profile=kind=="XUSB"?(service.Equals("xusb22",StringComparison.OrdinalIgnoreCase)&&cls=="XnaComposite"&&(originalXusb||wiredXusb)?"Wired XUSB · USB lower filter; verify loaded driver capabilities":"XUSB profile not associated automatically"):kind=="GIP"?(service.Equals("dc1-controller",StringComparison.OrdinalIgnoreCase)&&cls=="XboxComposite"&&Has(compatible,@"USB\MS_COMP_XGIP10")?"USB GIP · 14-byte gamepad prefix + Guide · metadata and actual format must be validated":"GIP transport not associated automatically"):kind=="XInputHID"?(propertyFlags==6?"XInputHID flags 6 · ancestry and descriptor must be validated":"XInputHID not associated: flags "+(propertyFlags.HasValue?propertyFlags.Value.ToString():"unknown")+" (6 required)"):kind=="Xbox HID view"?"Secondary Xbox view · requires a verified native link to its parent":kind=="Control"?"PnP control endpoint · no input":"";
    rows.Add(new DeviceRow{Kind=kind,Name=name,Service=service,Instance=instance,HardwareIds=ids,CompatibleIds=compatible,UpperFilters=upper,LowerFilters=lower,ClassFilters=classFilters,ClassUpperFilters=classUpper,ClassLowerFilters=classLower,ParentInstance=parentId,DriverVersion=version,DriverProvider=provider,ProblemCode=problem,PnpStatus=status,PnpQuerySucceeded=queried,PropertyFlags=propertyFlags,VidPid=HexId(ids,"VID_")+" : "+HexId(ids,"PID_"),Filters=filters.Trim(';'),WindowsState=started?"Started":"Not started / code "+problem,Association=association,Associated=associated,InterfaceActive=active.Contains(instance),InstalledInf=inf,ProfileSupport=profile});
   }}finally{SetupDiDestroyDeviceInfoList(set);}return rows.OrderBy(x=>x.Kind).ThenBy(x=>x.Name).ToList();
  }
  static string DriverState() {IntPtr scm=OpenSCManager(null,null,1);if(scm==IntPtr.Zero)return "Access denied";try{IntPtr service=OpenService(scm,"Interceptor",4);if(service==IntPtr.Zero)return Marshal.GetLastWin32Error()==1060?"Not installed":"Access denied";try{ServiceStatus state;if(!QueryServiceStatus(service,out state))return "Unknown status";return state.State==4?"Running":state.State==1?"Stopped":"Transition ("+state.State+")";}finally{CloseServiceHandle(service);}}finally{CloseServiceHandle(scm);}}
  static SafeFileHandle OpenDriver() {var handle=CreateFile("\\\\.\\Interceptor",0xC0000000,3,IntPtr.Zero,3,0,IntPtr.Zero);if(handle.IsInvalid){handle.Dispose();throw new Win32Exception();}return handle;}
  static IcStatus Status(SafeFileHandle handle) {IcStatus state;uint returned;if(!DeviceIoControl(handle,StatusIoctl,IntPtr.Zero,0,out state,64,out returned,IntPtr.Zero))throw new Win32Exception();if(returned!=64 || state.Version!=4)throw new InvalidDataException("The driver ABI is incompatible with this installer.");return state;}
  internal static bool ValidCapabilities(IcCapabilities c,uint bytes){return bytes==64&&c.Size==64&&c.Version==1&&c.Abi==4&&c.Inputs==3&&c.Controllers==15&&c.Flags==0&&c.Reserved0==0&&c.Reserved1==0&&c.Reserved2==0&&c.MaxDevices>0&&c.MaxDevices<=256&&c.QueueCapacity==8192&&c.MaxSend==256&&c.HidReportMax==1024;}
  static void ReadTrial(SafeFileHandle handle,Snapshot s){IcTrial t;uint bytes;if(!DeviceIoControl(handle,TrialIoctl,IntPtr.Zero,0,out t,64,out bytes,IntPtr.Zero)){int error=Marshal.GetLastWin32Error();if(error==1||error==50)return;throw new Win32Exception(error);}if(bytes!=64||t.Size!=64||t.Version!=1||t.State>3||t.Duration==0||t.Remaining>t.Duration||t.Reserved0!=0||t.Reserved1!=0||t.Reserved2!=0)throw new InvalidDataException("Invalid trial status.");s.TrialCode=t.State;s.TrialRemaining100ns=t.Remaining;s.TrialState=t.State==0?"Demo: 1 hour from the first capture context, including diagnostics. Injection requires this activation.":t.State==1?"Demo active: "+TimeSpan.FromTicks((long)t.Remaining).ToString(@"hh\:mm\:ss")+" remaining · shared across all applications.":t.State==2?"Demo expired: passthrough active. Restart Windows for another 2-hour session.":"Demo unavailable: capture disabled, passthrough preserved · Windows "+t.Error;s.ApiDetail+=" · "+s.TrialState;}
  static void ReadCapabilities(SafeFileHandle handle,Snapshot s){IcCapabilities c;uint bytes;if(!DeviceIoControl(handle,CapabilitiesIoctl,IntPtr.Zero,0,out c,64,out bytes,IntPtr.Zero)){int error=Marshal.GetLastWin32Error();if(error==1||error==50){s.ControllerProfiles="Older driver: native capabilities are not advertised";return;}throw new Win32Exception(error);}if(!ValidCapabilities(c,bytes))throw new InvalidDataException("Invalid capabilities response.");s.CapabilitiesAvailable=true;s.ControllerMask=c.Controllers;var profiles=new List<string>();if((c.Controllers&1)!=0)profiles.Add("HID");if((c.Controllers&2)!=0){if((c.XusbProfiles&4)!=0)profiles.Add("USB wired XUSB · native Windows views");else if((c.XusbProfiles&2)!=0)profiles.Add("Legacy wired XUSB upper backend");else if((c.XusbProfiles&1)!=0)profiles.Add("Legacy XUSB 028E upper backend");}if((c.Controllers&4)!=0&&(c.XihProfiles&1)!=0)profiles.Add("Generic XInputHID");if((c.Controllers&8)!=0&&(c.GipProfiles&1)!=0)profiles.Add((c.GipProfiles&2)!=0?"GIP USB 18 · 64 bytes, raw extension":"GIP USB 14-byte prefix, legacy decoder");s.ControllerProfiles="Controller profiles: "+String.Join(", ",profiles)+" · hardware availability must be verified";}
  public static Snapshot Inspect() {
   var s=new Snapshot{Timestamp=DateTimeOffset.Now.ToString("o"),Os=Environment.OSVersion.VersionString,DriverState=DriverState(),Devices=EnumerateDevices(),DriverPath="Absent",StagedPackages=new List<string>()};
   s.DriverRunning=s.DriverState=="Running";
   try{using(var key=Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Interceptor\PendingDeviceRestart"))s.DeviceRestartPending=key!=null&&Convert.ToInt32(key.GetValue("Required",0))!=0;}catch(Exception){s.DeviceRestartPending=null;}
   string repository=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),@"DriverStore\FileRepository");
   foreach(string inf in new[]{"interceptorkeyboard.inf","interceptormouse.inf","interceptorcontrol.inf"})foreach(string directory in Directory.GetDirectories(repository,inf+"_amd64_*"))if(File.Exists(Path.Combine(directory,inf)))s.StagedPackages.Add(Path.Combine(directory,inf));
   s.DriverSigningMode=Package.SigningMode();
   s.PackageState=(s.DriverSigningMode=="Microsoft"?"Embedded Microsoft-signed driver · test mode is not required\n":"Awaiting Microsoft signature; installation disabled.\n")+"Windows Driver Store: "+s.StagedPackages.Count+" Interceptor package(s)";
   using(var recovery=Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\InterceptorRecovery"))s.RecoveryState=recovery!=null&&Convert.ToInt32(recovery.GetValue("Start",4))==2?"USB recovery armed at startup · 30-second window":"USB recovery is not installed";
   s.PackageState+="\n"+s.RecoveryState;
   using(var key=Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\Interceptor")){if(key!=null)s.DriverPath=Convert.ToString(key.GetValue("ImagePath","Not specified"));}
   string binary=Environment.ExpandEnvironmentVariables(s.DriverPath).Trim('"');if(binary.StartsWith(@"\??\"))binary=binary.Substring(4);if(binary.StartsWith(@"\SystemRoot\",StringComparison.OrdinalIgnoreCase))binary=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows),binary.Substring(12));
   try{if(File.Exists(binary))using(var file=File.OpenRead(binary))s.DriverSha256=Package.Digest(file);}catch(IOException e){s.Error="Reading the driver hash: "+e.Message;}
   s.EmbeddedDriverSha256=Package.Manifest()["Payload/Interceptor.sys"];s.DriverFileMatchesPayload=String.IsNullOrEmpty(s.DriverSha256)?(bool?)null:s.DriverSha256.Equals(s.EmbeddedDriverSha256,StringComparison.OrdinalIgnoreCase);s.DriverHashScope="File configured for the service; code already loaded in memory is not measured.";
   var ci=new CiInfo{Length=8};uint returned;s.TestMode=NtQuerySystemInformation(103,ref ci,8,out returned)>=0?((ci.Options&2)!=0?"Test mode enabled":"Test mode disabled"):"Test mode unknown";
   try{using(var handle=OpenDriver()){var state=Status(handle);s.ApiAvailable=true;s.ApiState="Responding · ABI "+state.Version;s.KeyboardQueued=state.KeyboardCount;s.MouseQueued=state.MouseCount;s.HidQueued=state.HidCount;s.LastResetReason=state.Reset;s.Overflows=state.Overflows;s.ApiDetail=state.Keyboards+" keyboard(s), "+state.MouseDevices+" mouse device(s), "+state.Hids+" HID reader(s); queued HID reports: "+state.HidCount;}}
   catch(Exception e){s.ApiState="Unavailable";s.ApiDetail=e.Message;}
   if(s.ApiAvailable)try{using(var handle=OpenDriver()){ReadCapabilities(handle,s);s.ApiDetail+=" · "+s.ControllerProfiles;ReadTrial(handle,s);}}catch(Exception e){s.ControllerProfiles="Capabilities unverified: "+e.Message;s.Error=s.ControllerProfiles;}
   return s;
  }
  public static string Benchmark() {
   using(var handle=OpenDriver()){for(int i=0;i<20;i++)Status(handle);var values=new List<double>();var cpu=Process.GetCurrentProcess().TotalProcessorTime;var total=Stopwatch.StartNew();for(int i=0;i<1000 && total.ElapsedMilliseconds<1500;i++){long t=Stopwatch.GetTimestamp();Status(handle);values.Add((Stopwatch.GetTimestamp()-t)*1000000.0/Stopwatch.Frequency);}total.Stop();values.Sort();return String.Format(System.Globalization.CultureInfo.InvariantCulture,"{0:F1}|median {0:F1} µs · p95 {1:F1} µs · max {2:F1} µs\n{3} requests in {4:F1} ms · process CPU {5:F1} ms",values[values.Count/2],values[(int)((values.Count-1)*.95)],values[values.Count-1],values.Count,total.Elapsed.TotalMilliseconds,(Process.GetCurrentProcess().TotalProcessorTime-cpu).TotalMilliseconds);}
  }
 }
}

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

namespace InterceptorRecovery {
 public sealed class SavedValue { public string Name;public bool Exists;public string[] Values; }
 public sealed class DeviceState {public string Instance,ClassGuid,Service,Inf;public SavedValue Upper,Lower;}
 public static class Devices {
  public static DeviceState[] Read(string json){return new System.Web.Script.Serialization.JavaScriptSerializer{MaxJsonLength=16*1024*1024}.Deserialize<DeviceState[]>(json);}
  public static void WriteState(string path,string json){
   string temp=path+"."+Guid.NewGuid().ToString("N")+".tmp";
   byte[] bytes=new UTF8Encoding(false).GetBytes(json);
   using(var file=new FileStream(temp,FileMode.CreateNew,FileAccess.Write,FileShare.None)){file.Write(bytes,0,bytes.Length);file.Flush(true);}
   if(File.Exists(path))File.Replace(temp,path,null);else File.Move(temp,path);
  }
  [StructLayout(LayoutKind.Sequential)] struct Info {public uint Size;public Guid Class;public uint Instance;public IntPtr Reserved;}
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern IntPtr SetupDiGetClassDevs(IntPtr cls,string enumerator,IntPtr parent,uint flags);
  [DllImport("setupapi.dll",SetLastError=true)]static extern bool SetupDiEnumDeviceInfo(IntPtr set,uint index,ref Info data);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern bool SetupDiGetDeviceInstanceId(IntPtr set,ref Info info,StringBuilder id,uint size,out uint needed);
  [DllImport("setupapi.dll")]static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern IntPtr SetupDiCreateDeviceInfoList(IntPtr cls,IntPtr parent);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern bool SetupDiOpenDeviceInfo(IntPtr set,string id,IntPtr parent,uint flags,ref Info info);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern bool SetupDiSetDeviceRegistryProperty(IntPtr set,ref Info info,uint property,byte[] data,uint bytes);
  public static SavedValue Value(RegistryKey key,string name) {
   bool exists=Array.IndexOf(key.GetValueNames(),name)>=0;
   if(exists&&key.GetValueKind(name)!=RegistryValueKind.MultiString)throw new InvalidDataException("Unexpected filter type: "+name);
   return new SavedValue{Name=name,Exists=exists,Values=exists?(string[])key.GetValue(name):new string[0]};
  }
  public static DeviceState[] Snapshot() {
   var result=new List<DeviceState>();IntPtr set=SetupDiGetClassDevs(IntPtr.Zero,null,IntPtr.Zero,4);
   if(set==new IntPtr(-1))throw new Win32Exception();
   try{for(uint n=0;;n++){
    var info=new Info{Size=(uint)Marshal.SizeOf(typeof(Info))};if(!SetupDiEnumDeviceInfo(set,n,ref info)){if(Marshal.GetLastWin32Error()==259)break;throw new Win32Exception();}
    string cls=info.Class.ToString("B");
    if(!cls.Equals("{4d36e96b-e325-11ce-bfc1-08002be10318}",StringComparison.OrdinalIgnoreCase)&&!cls.Equals("{4d36e96f-e325-11ce-bfc1-08002be10318}",StringComparison.OrdinalIgnoreCase)&&!cls.Equals("{745a17a0-74d3-11d0-b6fe-00a0c90f57da}",StringComparison.OrdinalIgnoreCase)&&!cls.Equals("{d61ca365-5af4-4486-998b-9db4734c6ca3}",StringComparison.OrdinalIgnoreCase)&&!cls.Equals("{05f5cfe2-4733-4950-a6bb-07aad01a3a84}",StringComparison.OrdinalIgnoreCase))continue;
    var id=new StringBuilder(512);uint needed;if(!SetupDiGetDeviceInstanceId(set,ref info,id,512,out needed))throw new Win32Exception();
    using(var key=Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\"+id)){
     if(key==null)continue;string driver=Convert.ToString(key.GetValue("Driver","")),inf="";
     if(driver.Length>0)using(var software=Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\"+driver))if(software!=null)inf=Convert.ToString(software.GetValue("InfPath",""));
     result.Add(new DeviceState{Instance=id.ToString(),ClassGuid=cls,Service=Convert.ToString(key.GetValue("Service","")),Inf=inf,Upper=Value(key,"UpperFilters"),Lower=Value(key,"LowerFilters")});
    }
   }}finally{SetupDiDestroyDeviceInfoList(set);}return result.ToArray();
  }
  public static string[] WithoutOurs(string[] values){var r=new List<string>();foreach(string v in values??new string[0])if(!v.Equals("Interceptor",StringComparison.OrdinalIgnoreCase))r.Add(v);return r.ToArray();}
  public static bool Same(string[] a,string[] b){a=a??new string[0];b=b??new string[0];if(a.Length!=b.Length)return false;for(int n=0;n<a.Length;n++)if(a[n]!=b[n])return false;return true;}
  public static void RestoreFilter(string instance,SavedValue saved){
   if(saved.Name!="UpperFilters"&&saved.Name!="LowerFilters")throw new InvalidDataException("Unexpected filter property.");
   string[] values;bool exists;
   using(var key=Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\"+instance)){
    if(key==null)return;var current=Value(key,saved.Name);string[] original=WithoutOurs(saved.Values),remaining=WithoutOurs(current.Values);
    // Preserve other software's later changes. Only remove our exact entry.
    if(Same(remaining,original)){
     values=original;exists=saved.Exists;
    }else if(!Same(remaining,current.Values)){values=remaining;exists=remaining.Length!=0;}else return;
    if(current.Exists==exists&&Same(current.Values,values))return;
   }
   byte[] data=exists?Encoding.Unicode.GetBytes(String.Join("\0",values)+"\0\0"):null;
   IntPtr set=SetupDiCreateDeviceInfoList(IntPtr.Zero,IntPtr.Zero);if(set==new IntPtr(-1))throw new Win32Exception();
   try{var info=new Info{Size=(uint)Marshal.SizeOf(typeof(Info))};if(!SetupDiOpenDeviceInfo(set,instance,IntPtr.Zero,0,ref info))throw new Win32Exception();if(!SetupDiSetDeviceRegistryProperty(set,ref info,saved.Name=="UpperFilters"?17u:18u,data,data==null?0u:(uint)data.Length))throw new Win32Exception();}finally{SetupDiDestroyDeviceInfoList(set);}
  }
  public static bool CleanClassFilters(){bool changed=false;using(var classes=Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class"))foreach(string name in classes.GetSubKeyNames())using(var key=classes.OpenSubKey(name)){
   foreach(string property in new[]{"UpperFilters","LowerFilters"}){if(Array.IndexOf(key.GetValueNames(),property)<0)continue;var current=Value(key,property);var remaining=WithoutOurs(current.Values);if(!Same(current.Values,remaining)){using(var write=classes.OpenSubKey(name,true)){if(remaining.Length==0)write.DeleteValue(property,false);else write.SetValue(property,remaining,RegistryValueKind.MultiString);}changed=true;}}
  }return changed;}
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern IntPtr LoadLibraryEx(string name,IntPtr reserved,uint flags);
  [DllImport("kernel32.dll",SetLastError=true)]static extern IntPtr GetProcAddress(IntPtr module,string name);
  [DllImport("kernel32.dll")]static extern bool FreeLibrary(IntPtr module);
  [UnmanagedFunctionPointer(CallingConvention.StdCall,CharSet=CharSet.Unicode)]delegate uint Bind(string instance,string inf,[MarshalAs(UnmanagedType.Bool)]bool mouse,[MarshalAs(UnmanagedType.Bool)]out bool reboot);
  public static bool RestoreMapper(DeviceState saved,string nativeDll) {
   if(!String.Equals(saved.Service,"kbdhid",StringComparison.OrdinalIgnoreCase)&&!String.Equals(saved.Service,"mouhid",StringComparison.OrdinalIgnoreCase))return false;
   if(String.IsNullOrEmpty(saved.Inf)||Path.GetFileName(saved.Inf)!=saved.Inf)throw new InvalidDataException("Invalid saved INF name.");
   string inf=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows),"INF",saved.Inf);if(!File.Exists(inf))throw new FileNotFoundException("Original INF no longer available",inf);
   IntPtr module=LoadLibraryEx(Path.GetFullPath(nativeDll),IntPtr.Zero,8);if(module==IntPtr.Zero)throw new Win32Exception();
   try{IntPtr proc=GetProcAddress(module,"RestoreInterceptorMapper");if(proc==IntPtr.Zero)throw new Win32Exception();bool reboot;uint error=((Bind)Marshal.GetDelegateForFunctionPointer(proc,typeof(Bind)))(saved.Instance,inf,String.Equals(saved.Service,"mouhid",StringComparison.OrdinalIgnoreCase),out reboot);if(error==433||error==1167)return false;if(error!=0)throw new Win32Exception((int)error);return reboot;}finally{FreeLibrary(module);}
  }
 }
}

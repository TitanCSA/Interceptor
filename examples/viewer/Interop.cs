using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Security.Cryptography;
using System.Threading;

namespace InterceptorViewer {
 [StructLayout(LayoutKind.Sequential, Pack=8)]
 public struct Snapshot {
  public uint Size,Version,Status,Error,ResetReason,DeviceCount,Mask,Controllers;
  public ulong Revision,Generation,Received,Forwarded;
  public uint Associated,Existing;
  public ulong Reserved;
 }
 [StructLayout(LayoutKind.Sequential, Pack=8, CharSet=CharSet.Unicode)]
 public struct DeviceInfo {
  public uint Id,Type,Token,Flags;
  public ushort Vid,Pid,Revision,UsagePage,Usage,InputBytes,OutputBytes,FeatureBytes;
  [MarshalAs(UnmanagedType.ByValTStr,SizeConst=128)] public string Name;
  [MarshalAs(UnmanagedType.ByValTStr,SizeConst=256)] public string HardwareId;
 }
 [StructLayout(LayoutKind.Sequential, Pack=8)]
 public struct DeviceState {
  public DeviceInfo Info;
  public ulong Sequence,Timestamp,InputCount;
  public uint Flags,DecodeError;
  [MarshalAs(UnmanagedType.ByValArray,SizeConst=96)] public byte[] Keys;
  public uint MouseButtons;
  public int MouseX,MouseY,Wheel,HorizontalWheel;
  public uint MouseFlags;
  public ulong Buttons;
  [MarshalAs(UnmanagedType.ByValArray,SizeConst=8)] public float[] Axes;
  [MarshalAs(UnmanagedType.ByValArray,SizeConst=8)] public int[] RawAxes;
  [MarshalAs(UnmanagedType.ByValArray,SizeConst=8)] public ushort[] AxisUsages;
  public uint AxisMask;
  public int Hat;
  public float LeftTrigger,RightTrigger;
  public ulong ButtonMask;
  public static DeviceState Empty(){return new DeviceState{Keys=new byte[96],Axes=new float[8],RawAxes=new int[8],AxisUsages=new ushort[8],Hat=-1};}
  public bool Pressed(int code){return Keys!=null&&code>=0&&code<768&&(Keys[code/8]&(1<<(code%8)))!=0;}
 }
 // Only this C ABI crosses into WPF. The native layer owns the public SDK context.
 sealed class Bridge : IDisposable {
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate uint StartFn(byte mask,byte controllers,IntPtr changeEvent);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void StopFn();
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate uint ReadFn(ref Snapshot header,IntPtr devices,uint capacity,out uint required);
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr LoadLibraryEx(string path,IntPtr file,uint flags);
  [DllImport("kernel32.dll",CharSet=CharSet.Ansi,ExactSpelling=true,SetLastError=true)] static extern IntPtr GetProcAddress(IntPtr module,string name);
  [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
  readonly AutoResetEvent changed=new AutoResetEvent(false);
  readonly object gate=new object();bool disposed;
  IntPtr library,buffer;string directory;FileStream pinnedFile;StartFn start;StopFn stop;ReadFn read;
  public WaitHandle Changed{get{return changed;}}
  public Bridge(){
   try {
    if(Marshal.SizeOf(typeof(Snapshot))!=80||Marshal.SizeOf(typeof(DeviceState))!=1064)throw new InvalidDataException("Incompatible viewer ABI.");
    // Extract only the assembly resource into a fresh, private directory. Keep
    // the file open without write/delete sharing while the DLL is loaded.
    directory=Path.Combine(Path.GetTempPath(),"InterceptorViewer-"+Guid.NewGuid().ToString("N"));
    var security=new DirectorySecurity();security.SetAccessRuleProtection(true,false);
    security.AddAccessRule(new FileSystemAccessRule(WindowsIdentity.GetCurrent().User,FileSystemRights.FullControl,InheritanceFlags.ContainerInherit|InheritanceFlags.ObjectInherit,PropagationFlags.None,AccessControlType.Allow));
    Directory.CreateDirectory(directory,security);string path=Path.Combine(directory,"InterceptorViewerBridge.dll");
    byte[] expected;
    using(var resource=Assembly.GetExecutingAssembly().GetManifestResourceStream("InterceptorViewerBridge.dll")){
     if(resource==null)throw new InvalidDataException("Embedded bridge missing.");
     using(var hash=SHA256.Create())expected=hash.ComputeHash(resource);resource.Position=0;
     using(var output=new FileStream(path,FileMode.CreateNew,FileAccess.Write,FileShare.None)){resource.CopyTo(output);output.Flush(true);}
    }
    pinnedFile=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read);
    using(var hash=SHA256.Create()){var actual=hash.ComputeHash(pinnedFile);for(int i=0;i<expected.Length;i++)if(actual[i]!=expected[i])throw new InvalidDataException("Extracted bridge changed.");}
    library=LoadLibraryEx(path,IntPtr.Zero,0x100|0x800); // DLL directory + System32 dependencies only.
    if(library==IntPtr.Zero)throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    start=(StartFn)Function("Viewer_Start",typeof(StartFn));stop=(StopFn)Function("Viewer_Stop",typeof(StopFn));read=(ReadFn)Function("Viewer_ReadSnapshot",typeof(ReadFn));
    buffer=Marshal.AllocHGlobal(1064*256);
   }catch{Dispose();throw;}
  }
  Delegate Function(string name,Type type){var p=GetProcAddress(library,name);if(p==IntPtr.Zero)throw new InvalidDataException("Missing export: "+name);return Marshal.GetDelegateForFunctionPointer(p,type);}
  public uint Start(byte mask,byte controllers){lock(gate){if(disposed)throw new ObjectDisposedException("Bridge");return start(mask,controllers,changed.SafeWaitHandle.DangerousGetHandle());}}
  public void Stop(){lock(gate){if(!disposed&&stop!=null)stop();}}
  public DeviceState[] Read(out Snapshot snapshot){
   lock(gate){
   if(disposed)throw new ObjectDisposedException("Bridge");
   snapshot=new Snapshot{Size=80,Version=1};uint required;
   uint error=read(ref snapshot,buffer,256,out required);
   if(error!=0)throw new System.ComponentModel.Win32Exception((int)error);
   if(required>256||snapshot.DeviceCount!=required)throw new InvalidDataException("Invalid snapshot.");
   var result=new DeviceState[required];for(int i=0;i<result.Length;i++)result[i]=(DeviceState)Marshal.PtrToStructure(IntPtr.Add(buffer,i*1064),typeof(DeviceState));return result;
   }
  }
  public void Dispose(){
   // The native lifecycle mutex protects its worker, not concurrent callers
   // still returning through DLL code. Never unload while any call is active.
   lock(gate){
   if(disposed)return;disposed=true;
   if(stop!=null)stop();stop=null;start=null;read=null;
   if(buffer!=IntPtr.Zero){Marshal.FreeHGlobal(buffer);buffer=IntPtr.Zero;}
   if(library!=IntPtr.Zero){FreeLibrary(library);library=IntPtr.Zero;}
   if(pinnedFile!=null){pinnedFile.Dispose();pinnedFile=null;}
   if(directory!=null){try{File.Delete(Path.Combine(directory,"InterceptorViewerBridge.dll"));Directory.Delete(directory);}catch(IOException){}catch(UnauthorizedAccessException){}directory=null;}
   changed.Dispose();
   }
  }
 }
}

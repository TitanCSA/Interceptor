using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Management;
using System.Runtime.InteropServices;
using System.ServiceProcess;
using System.Threading;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace InterceptorRecovery {
 public static class RecoveryPolicy {
  public static bool External(int driveType,int bus,bool removable){return driveType==2||(driveType==3&&(bus==7||bus==4||removable));}
  public static bool InWindow(long now,long deadline){return now<deadline;}
 }
 public sealed class RecoveryService:ServiceBase {
  readonly ManualResetEvent stop=new ManualResetEvent(false);Thread worker;
  public RecoveryService(){ServiceName="InterceptorRecovery";CanStop=true;AutoLog=false;}
  protected override void OnStart(string[] args){worker=new Thread(()=>{try{Monitor(null);}catch(Exception e){Log("monitor-error "+e.Message);}finally{ThreadPool.QueueUserWorkItem(x=>{try{Stop();}catch(InvalidOperationException){}});}}){IsBackground=true};worker.Start();}
  protected override void OnStop(){stop.Set();if(worker!=null&&worker!=Thread.CurrentThread)worker.Join(5000);}
  static string RecoveryRoot {get{return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),"Interceptor","Recovery");}}
  static void Log(string value){try{string root=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),"Interceptor");if(Directory.Exists(root))File.AppendAllText(Path.Combine(root,"recovery.log"),DateTime.UtcNow.ToString("o")+" "+value+Environment.NewLine);}catch(IOException){}catch(UnauthorizedAccessException){}}
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern SafeFileHandle CreateFile(string name,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
  [DllImport("kernel32.dll",SetLastError=true)]static extern bool DeviceIoControl(SafeFileHandle file,uint code,byte[] input,uint inputBytes,byte[] output,uint outputBytes,out uint returned,IntPtr overlapped);
  static bool ExternalDrive(DriveInfo drive) {
   if(drive.DriveType==DriveType.Removable)return true;
   if(drive.DriveType!=DriveType.Fixed)return false;
   using(var handle=CreateFile(@"\\.\"+drive.Name.Substring(0,2),0,7,IntPtr.Zero,3,0,IntPtr.Zero)){
    if(handle.IsInvalid)return false;var query=new byte[12];var answer=new byte[4096];uint returned;
    if(!DeviceIoControl(handle,0x2D1400,query,(uint)query.Length,answer,(uint)answer.Length,out returned,IntPtr.Zero)||returned<36||BitConverter.ToUInt32(answer,4)<36)return false;
    return RecoveryPolicy.External((int)drive.DriveType,(int)BitConverter.ToUInt32(answer,28),answer[10]!=0);
   }
  }
  static bool Marker(string root) {
   try {
    string path=Path.Combine(root,"interceptor.txt");FileAttributes a=File.GetAttributes(path);
    return (a&(FileAttributes.Directory|FileAttributes.ReparsePoint))==0;
   }catch(IOException){return false;}catch(UnauthorizedAccessException){return false;}
  }
  bool Scan(string laboratoryRoot) {
   string system=Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.Windows));
   foreach(DriveInfo drive in DriveInfo.GetDrives())try{
    if(drive.Name.Equals(system,StringComparison.OrdinalIgnoreCase))continue;
    if(laboratoryRoot!=null?!drive.Name.Equals(laboratoryRoot,StringComparison.OrdinalIgnoreCase):!ExternalDrive(drive))continue;
    if(Marker(drive.Name)){Log("marker "+drive.Name+(laboratoryRoot==null?" external":" LAB-VIRTUAL-VOLUME"));return true;}
   }catch(IOException){}catch(UnauthorizedAccessException){}
   return false;
  }
  public int Monitor(string laboratoryRoot) {
   var clock=Stopwatch.StartNew();const int window=30000;Log("window-start 30s");
   using(var changed=new AutoResetEvent(false))using(var watcher=new ManagementEventWatcher("SELECT * FROM Win32_VolumeChangeEvent WHERE EventType = 2 OR EventType = 3")){
    watcher.EventArrived+=(s,e)=>{try{changed.Set();}catch(ObjectDisposedException){}};
    watcher.Start();
    try {
     bool scan=true;
     while(RecoveryPolicy.InWindow(clock.ElapsedMilliseconds,window)&&!stop.WaitOne(0)){
      if(scan&&Scan(laboratoryRoot)&&RecoveryPolicy.InWindow(clock.ElapsedMilliseconds,window)){
       string script=Path.Combine(RecoveryRoot,"EmergencyRecover.ps1");
       if(!File.Exists(script))throw new FileNotFoundException("Protected recovery script missing",script);
       // Only the locally installed script is executed. No bytes, commands or
       // paths are loaded from the external marker file.
       var start=new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),@"WindowsPowerShell\v1.0\powershell.exe"),"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""+script+"\""){UseShellExecute=false,CreateNoWindow=true,WorkingDirectory=Environment.GetFolderPath(Environment.SpecialFolder.System)};
       using(var process=Process.Start(start)){if(process==null)throw new IOException("Recovery launch failed");Log("uninstall-start pid="+process.Id);}
       return 0;
      }
      int remaining=(int)Math.Max(0,window-clock.ElapsedMilliseconds);
      int reason=WaitHandle.WaitAny(new WaitHandle[]{stop,changed},remaining);
      if(reason==0||reason==WaitHandle.WaitTimeout)break;scan=true;
     }
     Log("window-end no-marker");return 3;
    }finally{watcher.Stop();}
   }
  }
  static bool VirtualMachine(){using(var key=Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\BIOS"))return key!=null&&Convert.ToString(key.GetValue("SystemProductName","")).Equals("Virtual Machine",StringComparison.OrdinalIgnoreCase);}
  public static int Main(string[] args){
   if(args.Length==1&&args[0]=="--self-test"){
    int[,] samples={{3,7,0,1},{3,4,0,1},{3,11,0,0},{3,17,0,0},{3,15,0,0},{4,7,1,0},{5,7,1,0},{2,0,0,1},{3,12,1,1}};
    int cases=samples.GetLength(0);for(int n=0;n<cases;n++)if(RecoveryPolicy.External(samples[n,0],samples[n,1],samples[n,2]!=0)!=(samples[n,3]!=0))throw new Exception("External drive classification");
    if(!RecoveryPolicy.InWindow(29999,30000)||RecoveryPolicy.InWindow(30000,30000)||RecoveryPolicy.InWindow(30001,30000))throw new Exception("Window deadline");
    Console.WriteLine("PASS "+cases+" drive classes and exact window boundary");return 0;
   }
   if(args.Length==2&&args[0]=="--lab-monitor"){
    if(!VirtualMachine())throw new InvalidOperationException("Virtual volume substitution is restricted to Microsoft laboratory VMs.");
    string root=Path.GetFullPath(args[1]);if(root.Length!=3||root[1]!=':'||root[2]!='\\')throw new ArgumentException("A drive root is required");
    using(var service=new RecoveryService())return service.Monitor(root);
   }
   if(args.Length!=0)return 87;
   using(var service=new RecoveryService())Run(service);return 0;
  }
 }
}

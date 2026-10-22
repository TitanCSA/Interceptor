using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace InterceptorSetup {
 public static class Package {
  public static JavaScriptSerializer Json {get{return new JavaScriptSerializer{MaxJsonLength=16*1024*1024};}}
  static Stream Resource(string name){var stream=Assembly.GetExecutingAssembly().GetManifestResourceStream(name);if(stream==null)throw new InvalidDataException("Missing resource: "+name);return stream;}
  public static string TextResource(string name){using(var stream=Resource(name))using(var reader=new StreamReader(stream,Encoding.UTF8))return reader.ReadToEnd();}
  public static string Digest(Stream data){using(var hash=SHA256.Create())return BitConverter.ToString(hash.ComputeHash(data)).Replace("-","");}
  public static Dictionary<string,string> Manifest(){
   var text=TextResource("PayloadHashes.json");if(text.Length>65536)throw new InvalidDataException("Package manifest is too large.");
   var manifest=Json.Deserialize<Dictionary<string,string>>(text);
   if(manifest==null||manifest.Count==0||manifest.Count>256)throw new InvalidDataException("Invalid package manifest size.");
   foreach(var pair in manifest){ValidateName(pair.Key);if(pair.Value==null||pair.Value.Length!=64||pair.Value.Any(c=>!Uri.IsHexDigit(c)))throw new InvalidDataException("Invalid package digest.");}
   return manifest;
  }
  public static string DriverVersion(){using(var stream=Resource("Payload.zip"))using(var zip=new ZipArchive(stream,ZipArchiveMode.Read)){var entry=zip.GetEntry("Payload/InterceptorControl.inf");if(entry==null||entry.Length>65536)return "unknown version";using(var reader=new StreamReader(entry.Open())){string line;while((line=reader.ReadLine())!=null)if(line.TrimStart().StartsWith("DriverVer=",StringComparison.OrdinalIgnoreCase)){int comma=line.IndexOf(',');return comma>=0?line.Substring(comma+1).Trim():"unknown version";}}}return "unknown version";}
  internal static string ExtractDiagnosticWorker(){
   string root=CreatePrivateDirectory();try{using(var input=Resource("InterceptorDiagnosticWorker.exe")){string expected=TextResource("DiagnosticWorker.sha256").Trim();if(!Digest(input).Equals(expected,StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("The diagnostic worker hash is invalid.");input.Position=0;using(var output=new FileStream(Path.Combine(root,"InterceptorDiagnosticWorker.exe"),FileMode.CreateNew,FileAccess.Write,FileShare.None)){input.CopyTo(output);output.Flush(true);}}return root;}catch{Cleanup(root);throw;}
  }
  public static string SigningMode() {
   using(var stream=Resource("Payload.zip"))using(var zip=new ZipArchive(stream,ZipArchiveMode.Read)){
    var entry=zip.GetEntry("Payload/Signing.json");if(entry==null)throw new InvalidDataException("Package signing policy is missing.");
    if(entry.Length>4096)throw new InvalidDataException("The signing policy is too large.");
    using(var input=entry.Open())using(var reader=new StreamReader(input,Encoding.UTF8)){
     var policy=Json.Deserialize<Dictionary<string,object>>(reader.ReadToEnd());
     if(policy==null||!policy.ContainsKey("Schema")||Convert.ToInt32(policy["Schema"])!=1||!policy.ContainsKey("Mode"))throw new InvalidDataException("Invalid signing policy.");
     string mode=Convert.ToString(policy["Mode"]);if(mode!="Development"&&mode!="Microsoft")throw new InvalidDataException("Unknown signing mode.");return mode;
    }
   }
  }
  public static string Verify() {
   var manifest=Manifest();long total=0;var seen=new HashSet<string>(StringComparer.OrdinalIgnoreCase);using(var stream=Resource("Payload.zip"))using(var zip=new ZipArchive(stream,ZipArchiveMode.Read))foreach(var entry in zip.Entries){if(entry.FullName.EndsWith("/"))throw new InvalidDataException("Unexpected package directory entry.");ValidateName(entry.FullName);if(entry.Length>32*1024*1024||(total+=entry.Length)>128*1024*1024)throw new InvalidDataException("Embedded package exceeds size limits.");string hash;if(!manifest.TryGetValue(entry.FullName,out hash) || !seen.Add(entry.FullName))throw new InvalidDataException("Unknown or duplicate package entry.");using(var input=entry.Open())if(!Digest(input).Equals(hash,StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("Integrity check failed: "+entry.FullName);}
   if(seen.Count!=manifest.Count || !seen.Contains("Install.ps1") || !seen.Contains("Uninstall.ps1") || !seen.Contains("Maintenance.ps1") || !seen.Contains("Payload/Interceptor.sys") || !seen.Contains("Payload/InterceptorControl.inf") || !seen.Contains("Payload/InterceptorControl.cat"))throw new InvalidDataException("Incomplete embedded package (PnP control endpoint required).");return seen.Count+" embedded files · SHA-256 verified";
  }
  internal static void ValidateName(string name){
   if(String.IsNullOrWhiteSpace(name)||name.Length>240||name.Contains(":")||name.Contains("\\")||name.StartsWith("/"))throw new InvalidDataException("Unsafe package path.");
   var parts=name.Split('/');if(parts.Length>8)throw new InvalidDataException("Package path is too deep.");
   foreach(var part in parts){
    string device=part.Split('.')[0].ToUpperInvariant();
    bool reserved=device=="CON"||device=="PRN"||device=="AUX"||device=="NUL"||
     (device.Length==4&&(device.StartsWith("COM")||device.StartsWith("LPT"))&&device[3]>='1'&&device[3]<='9');
    if(String.IsNullOrWhiteSpace(part)||part!=part.Trim()||part.EndsWith(".")||part.IndexOfAny(Path.GetInvalidFileNameChars())>=0||part=="."||part==".."||reserved)throw new InvalidDataException("Unsafe package path component.");
   }
  }
  static string CreatePrivateDirectory() {
   // Administrators and SYSTEM only. An unpredictable child of the protected
   // Windows directory avoids executing extracted scripts from a user-writable tree.
   string parent=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows),"Temp");
   for(string ancestor=parent;!String.IsNullOrEmpty(ancestor);ancestor=Path.GetDirectoryName(ancestor))
    if((File.GetAttributes(ancestor)&FileAttributes.ReparsePoint)!=0)throw new IOException("The setup directory or an ancestor is redirected.");
   string path=Path.Combine(parent,"InterceptorSetup-"+Guid.NewGuid().ToString("N"));
   var security=new DirectorySecurity();security.SetAccessRuleProtection(true,false);
   foreach(var sid in new[]{WellKnownSidType.BuiltinAdministratorsSid,WellKnownSidType.LocalSystemSid})security.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(sid,null),FileSystemRights.FullControl,InheritanceFlags.ContainerInherit|InheritanceFlags.ObjectInherit,PropagationFlags.None,AccessControlType.Allow));
   Directory.CreateDirectory(path,security);return path;
  }
  public static string Extract() {
   Verify();string root=CreatePrivateDirectory();try{using(var stream=Resource("Payload.zip"))using(var zip=new ZipArchive(stream,ZipArchiveMode.Read))foreach(var entry in zip.Entries){if(entry.FullName.EndsWith("/"))continue;ValidateName(entry.FullName);string path=Path.GetFullPath(Path.Combine(root,entry.FullName.Replace('/',Path.DirectorySeparatorChar)));if(!path.StartsWith(root+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))throw new IOException("Extraction outside the private directory.");Directory.CreateDirectory(Path.GetDirectoryName(path));using(var input=entry.Open())using(var output=new FileStream(path,FileMode.CreateNew,FileAccess.Write,FileShare.None))input.CopyTo(output);}return root;}catch{Cleanup(root);throw;}
  }
  public static void Cleanup(string root) {
   if(String.IsNullOrEmpty(root))return;string parent=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows),"Temp")+Path.DirectorySeparatorChar;
   string path=Path.GetFullPath(root);if(!path.StartsWith(parent+"InterceptorSetup-",StringComparison.OrdinalIgnoreCase)||Path.GetDirectoryName(path)+Path.DirectorySeparatorChar!=parent)throw new IOException("Cleanup outside the expected temporary directory.");
   if(Directory.Exists(path)){
    var pending=new Stack<string>();pending.Push(path);
    while(pending.Count!=0){string current=pending.Pop();if((File.GetAttributes(current)&FileAttributes.ReparsePoint)!=0)throw new IOException("Setup cleanup refuses redirected directories.");
     foreach(string child in Directory.EnumerateFileSystemEntries(current)){var flags=File.GetAttributes(child);if((flags&FileAttributes.ReparsePoint)!=0)throw new IOException("Setup cleanup refuses redirected entries.");if((flags&FileAttributes.Directory)!=0)pending.Push(child);}}
    Directory.Delete(path,true);
   }
  }
  public static int Execute(string action,Action<string> log,Action<string> phase) {
   // Scripts own the shared SYSTEM/administrator mutex, including recovery.
   return ExecuteLocked(action,log,phase);
  }
  static int ExecuteLocked(string action,Action<string> log,Action<string> phase) {
   if(!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))throw new UnauthorizedAccessException("Administrator rights are required.");
   if(action!="Uninstall"&&SigningMode()!="Microsoft")throw new InvalidOperationException("Microsoft signing is pending for this driver. No Windows setting was changed.");
   if(action!="Install"&&action!="Uninstall"&&action!="Associate"&&action!="CompleteAssociation")throw new ArgumentException("Unknown action.");string root=null;
   try {phase("Checking and extracting the package…");root=Extract();log(Verify());phase(action=="Install"?"Installing and associating devices…":action=="Uninstall"?"Restoring Windows drivers and uninstalling…":"Associating connected devices…");
    var start=new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),@"WindowsPowerShell\v1.0\powershell.exe"),"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""+Path.Combine(root,action+".ps1")+"\""){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true,WorkingDirectory=root};
    // PowerShell emits UTF-8; the worker decodes Windows utility output separately.
    start.StandardOutputEncoding=start.StandardErrorEncoding=new UTF8Encoding(false);
    return RunSetupProcess(start,line=>{Report(log,line);if(line.StartsWith("SETUP_STAGE: ",StringComparison.Ordinal))Report(phase,line.Substring(13));});
   }finally{if(root!=null){try{Cleanup(root);}catch(Exception e){Report(log,"Cleanup deferred: "+e.Message);}}}
  }
  static void Report(Action<string> callback,string message){
   if(callback==null)return;
   try{callback(message);}catch(Exception error){Debug.WriteLine("Setup display callback failed: "+error);}
  }
  internal static int RunSetupProcess(ProcessStartInfo start,Action<string> output){
   object gate=new object();
   using(var process=new Process{StartInfo=start}){
    DataReceivedEventHandler report=(sender,eventArgs)=>{if(eventArgs.Data!=null)lock(gate)Report(output,eventArgs.Data);};
    process.OutputDataReceived+=report;process.ErrorDataReceived+=report;
    if(!process.Start())throw new IOException("Cannot start the setup worker.");
    var held=process.Handle;
    process.BeginOutputReadLine();process.BeginErrorReadLine();
    process.WaitForExit(); // Complete both asynchronous streams before reading the exit code.
    return process.ExitCode;
   }
  }
  public static bool PendingControllerAssociation() {
   string path=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),@"Interceptor\state.json");
   if(!File.Exists(path))return false;
   var state=Json.Deserialize<Dictionary<string,object>>(File.ReadAllText(path,Encoding.UTF8));
   return PendingControllerAssociation(state,Convert.ToString(Manifest()["Payload/Interceptor.sys"]));
  }
  internal static bool PendingControllerAssociation(Dictionary<string,object> state,string expectedHash) {
   object value;if(state==null||!state.TryGetValue("Schema",out value)||Convert.ToInt32(value)!=1)return false;
   if(state.TryGetValue("Operation",out value)){
    var operation=value as Dictionary<string,object>;object status;
    if(operation!=null&&operation.TryGetValue("Status",out status)&&(Convert.ToString(status)=="Running"||Convert.ToString(status)=="Failed"))return false;
   }
   if(state.TryGetValue("PendingUninstall",out value)&&Convert.ToBoolean(value))return false;
   if(!state.TryGetValue("ControllerAssociation",out value))return false;
   var pending=value as Dictionary<string,object>;object enabled,hash,verified;
   if(pending==null||!pending.TryGetValue("DriverSHA256",out hash)||!string.Equals(Convert.ToString(hash),expectedHash,StringComparison.OrdinalIgnoreCase))return false;
   return (pending.TryGetValue("Pending",out enabled)&&Convert.ToBoolean(enabled))||
     !pending.TryGetValue("UsbMigrationVerified",out verified)||!Convert.ToBoolean(verified);
  }
 }
}

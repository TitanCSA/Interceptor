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
  public static Dictionary<string,string> Manifest(){return Json.Deserialize<Dictionary<string,string>>(TextResource("PayloadHashes.json"));}
  public static string DriverVersion(){using(var stream=Resource("Payload.zip"))using(var zip=new ZipArchive(stream,ZipArchiveMode.Read)){var entry=zip.GetEntry("Payload/InterceptorControl.inf");if(entry==null||entry.Length>65536)return "unknown version";using(var reader=new StreamReader(entry.Open())){string line;while((line=reader.ReadLine())!=null)if(line.TrimStart().StartsWith("DriverVer=",StringComparison.OrdinalIgnoreCase)){int comma=line.IndexOf(',');return comma>=0?line.Substring(comma+1).Trim():"unknown version";}}}return "unknown version";}
  internal static string ExtractDiagnosticWorker(){
   string root=CreatePrivateDirectory();try{using(var input=Resource("InterceptorDiagnosticWorker.exe")){string expected=TextResource("DiagnosticWorker.sha256").Trim();if(!Digest(input).Equals(expected,StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("The diagnostic worker hash is invalid.");input.Position=0;using(var output=new FileStream(Path.Combine(root,"InterceptorDiagnosticWorker.exe"),FileMode.CreateNew,FileAccess.Write,FileShare.None)){input.CopyTo(output);output.Flush(true);}}return root;}catch{Cleanup(root);throw;}
  }
  public static string SigningMode() {
   using(var stream=Resource("Payload.zip"))using(var zip=new ZipArchive(stream,ZipArchiveMode.Read)){
    var entry=zip.GetEntry("Payload/Signing.json");if(entry==null)return "Test";
    if(entry.Length>4096)throw new InvalidDataException("The signing policy is too large.");
    using(var input=entry.Open())using(var reader=new StreamReader(input,Encoding.UTF8)){
     var policy=Json.Deserialize<Dictionary<string,object>>(reader.ReadToEnd());
     if(policy==null||!policy.ContainsKey("Schema")||Convert.ToInt32(policy["Schema"])!=1||!policy.ContainsKey("Mode"))throw new InvalidDataException("Invalid signing policy.");
     string mode=Convert.ToString(policy["Mode"]);if(mode!="Test"&&mode!="Microsoft")throw new InvalidDataException("Unknown signing mode.");return mode;
    }
   }
  }
  public static string Verify() {
   var manifest=Manifest();var seen=new HashSet<string>(StringComparer.OrdinalIgnoreCase);using(var stream=Resource("Payload.zip"))using(var zip=new ZipArchive(stream,ZipArchiveMode.Read))foreach(var entry in zip.Entries){if(entry.FullName.EndsWith("/"))continue;ValidateName(entry.FullName);string hash;if(!manifest.TryGetValue(entry.FullName,out hash) || !seen.Add(entry.FullName))throw new InvalidDataException("Unknown or duplicate package entry.");using(var input=entry.Open())if(!Digest(input).Equals(hash,StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("Integrity check failed: "+entry.FullName);}
   if(seen.Count!=manifest.Count || !seen.Contains("Install.ps1") || !seen.Contains("Uninstall.ps1") || !seen.Contains("Payload/Interceptor.sys") || !seen.Contains("Payload/InterceptorControl.inf") || !seen.Contains("Payload/InterceptorControl.cat"))throw new InvalidDataException("Incomplete embedded package (PnP control endpoint required).");return seen.Count+" embedded files · SHA-256 verified";
  }
  internal static void ValidateName(string name){if(String.IsNullOrWhiteSpace(name)||name.Contains(":")||name.Contains("\\")||name.StartsWith("/")||name.Split('/').Any(p=>p==".."||p=="."))throw new InvalidDataException("Unsafe package path.");}
  static string CreatePrivateDirectory() {
   // Administrators and SYSTEM only. An unpredictable child of the protected
   // Windows directory avoids executing extracted scripts from a user-writable tree.
   string parent=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows),"Temp");
   if((File.GetAttributes(parent)&FileAttributes.ReparsePoint)!=0)throw new IOException("The Windows temporary directory is a link.");
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
   if(Directory.Exists(path)){if((File.GetAttributes(path)&FileAttributes.ReparsePoint)!=0)throw new IOException("The temporary directory was replaced with a link.");Directory.Delete(path,true);}
  }
  public static int Execute(string action,Action<string> log,Action<string> phase) {
   // Scripts own the shared SYSTEM/administrator mutex, including recovery.
   return ExecuteLocked(action,log,phase);
  }
  static int ExecuteLocked(string action,Action<string> log,Action<string> phase) {
   if(!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))throw new UnauthorizedAccessException("Administrator rights are required.");
   if(action!="Install"&&action!="Uninstall"&&action!="Associate"&&action!="CompleteAssociation")throw new ArgumentException("Unknown action.");string root=null;
   try {phase("Checking and extracting the package…");root=Extract();log(Verify());phase(action=="Install"?"Installing and associating devices…":action=="Uninstall"?"Restoring Windows drivers and uninstalling…":"Associating connected devices…");
    var start=new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),@"WindowsPowerShell\v1.0\powershell.exe"),"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""+Path.Combine(root,action+".ps1")+"\""){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true,WorkingDirectory=root};
    // Native utilities inherit UTF-8 output from the wrapper command environment.
    using(var process=new Process{StartInfo=start}){process.OutputDataReceived+=(s,e)=>{if(e.Data!=null)log(e.Data);};process.ErrorDataReceived+=(s,e)=>{if(e.Data!=null)log(e.Data);};if(!process.Start())throw new IOException("Cannot start the operation.");process.BeginOutputReadLine();process.BeginErrorReadLine();process.WaitForExit();return process.ExitCode;}
   }finally{if(root!=null){try{Cleanup(root);}catch(Exception e){log("Cleanup deferred: "+e.Message);}}}
  }
  public static bool PendingControllerAssociation() {
   string path=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),@"Interceptor\state.json");
   if(!File.Exists(path))return false;
   var state=Json.Deserialize<Dictionary<string,object>>(File.ReadAllText(path,Encoding.UTF8));
   object value;if(state==null||!state.TryGetValue("Schema",out value)||Convert.ToInt32(value)!=1)return false;
   if(state.TryGetValue("PendingUninstall",out value)&&Convert.ToBoolean(value))return false;
   if(!state.TryGetValue("ControllerAssociation",out value))return false;
   var pending=value as Dictionary<string,object>;object enabled,hash;
   return pending!=null&&pending.TryGetValue("Pending",out enabled)&&Convert.ToBoolean(enabled)&&pending.TryGetValue("DriverSHA256",out hash)&&string.Equals(Convert.ToString(hash),Convert.ToString(Manifest()["Payload/Interceptor.sys"]),StringComparison.OrdinalIgnoreCase);
  }
 }
}

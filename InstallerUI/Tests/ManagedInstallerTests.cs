using System;
using System.Diagnostics;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using InterceptorSetup;
internal static class ManagedInstallerTests {
 static int checks;
 static void Check(bool condition,string message){++checks;if(!condition)throw new Exception(message);}
 static ProcessStartInfo Start(string fixture,string mode){return new ProcessStartInfo(fixture,mode){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true};}
 static int Main(string[] args) {
  try {
   if(args.Length!=1)throw new ArgumentException("Fixture path required.");
   for(int i=0;i<120;++i){
    var lines=new List<string>();int expected=i%3==0?3010:i%3==1?0:7;
    int result=Package.RunSetupProcess(Start(args[0],expected.ToString()),lines.Add);
    Check(result==expected,"Worker exit code changed");
    Check(lines.Contains("stdout")&&lines.Contains("stderr"),"Output was not drained");
   }
   Check(Package.RunSetupProcess(Start(args[0],"0"),line=>{throw new IOException("display closed");})==0,"Display failure changed the operation result");
   foreach(string name in new[]{"Payload/ok.sys","Recovery/RecoveryBinding.dll","Maintenance.ps1"}){Package.ValidateName(name);++checks;}
   foreach(string name in new[]{"../bad","/bad","a//b","a/./b","a/../b","C:/bad","a\\bad","a/CON.exe","a/NUL","a/LPT1.txt","a/trailing.","a/trailing ","a/\0bad"}){
    bool rejected=false;try{Package.ValidateName(name);}catch(InvalidDataException){rejected=true;}
    Check(rejected,"Windows path alias accepted");
   }
   var method=typeof(Package).GetMethods(System.Reflection.BindingFlags.Static|System.Reflection.BindingFlags.NonPublic).FirstOrDefault(m=>m.Name=="PendingControllerAssociation"&&m.GetParameters().Length==2);
   if(method!=null){
    const string hash="AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    foreach(string state in new[]{"Running","Failed"}){
     var pending=new Dictionary<string,object>{{"Schema",1},{"ControllerAssociation",new Dictionary<string,object>{{"Pending",true},{"DriverSHA256",hash}}},{"Operation",new Dictionary<string,object>{{"Status",state}}}};
     Check(!(bool)method.Invoke(null,new object[]{pending,hash}),"Interrupted operation resumed automatically");
    }
   }
   Console.WriteLine(checks+" managed checks passed; no extraction or driver access.");return 0;
  }catch(Exception error){Console.Error.WriteLine(error);return 1;}
 }
}


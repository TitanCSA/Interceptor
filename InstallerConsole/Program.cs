using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security.Principal;
using System.Text;

namespace InterceptorSetup {
 internal static class ConsoleProgram {
  static readonly object OutputLock = new object();

  internal static string Parse(string[] args) {
   if (args.Length == 0) return "Help";
   if (args.Length != 1) throw new ArgumentException("Use one argument: /install or /uninstall.");
   switch (args[0].ToLowerInvariant()) {
    case "/install": return "Install";
    case "/uninstall": return "Uninstall";
    case "/?": case "/help": case "--help": return "Help";
    case "--self-test": return "Verify";
    default: throw new ArgumentException("Unknown argument: " + args[0]);
   }
  }

  static void Help() {
   Console.WriteLine("InterceptorSetupConsole.exe /install");
   Console.WriteLine("  Installs or reinstalls the driver, like Install / reinstall in the UI.");
   Console.WriteLine("  Resumes pending associations after a requested Windows restart.");
   Console.WriteLine("InterceptorSetupConsole.exe /uninstall");
   Console.WriteLine("  Restores Windows drivers and uninstalls Interceptor.");
   Console.WriteLine("InterceptorSetupConsole.exe /? | --self-test");
   Console.WriteLine("  Shows help or checks the embedded package without installing anything.");
   Console.WriteLine();
   Console.WriteLine("Administrator rights are required to install or uninstall (automatic UAC prompt).");
   Console.WriteLine("In an elevated terminal, output stays in that terminal and can be redirected.");
   Console.WriteLine("Exit codes: 0 = completed; 3010 = restart required; 1223 = UAC cancelled;");
   Console.WriteLine("        87 = invalid arguments; 1618 = another setup is active; 1638 = conflicting Interceptor edition; 1460 = Windows command timed out; 1 = failure. Windows never restarts automatically.");
  }

  static void Log(string line) {
   lock (OutputLock) Console.WriteLine(DateTime.Now.ToString("HH:mm:ss") + "  " + line);
  }

  static bool Administrator {
   get { return new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator); }
  }

  // CompleteAssociation is the same continuation run when the UI is reopened.
  // Repeating Install while it is pending would rebind the keyboard/mouse mappers.
  internal static string SelectAction(string action, bool pending) {
   return action == "Install" && pending ? "CompleteAssociation" : action;
  }

  public static int Main(string[] args) {
   Console.OutputEncoding = new UTF8Encoding(false);
   string action;
   try { action = Parse(args); }
   catch (ArgumentException e) { Console.Error.WriteLine(e.Message); Help(); return 87; }
   if (action == "Help") { Help(); return 0; }
   try {
    if (action == "Verify") {
     // No device, service, registry write, extraction, or elevation in this mode.
     string verified = Package.Verify();
     foreach (var pair in new[] {new[] {"/install", "Install"}, new[] {"/INSTALL", "Install"}, new[] {"/uninstall", "Uninstall"}, new[] {"/UNINSTALL", "Uninstall"}, new[] {"/?", "Help"}})
      if (Parse(new[] {pair[0]}) != pair[1]) throw new InvalidDataException("Contrat des arguments incorrect.");
     if (SelectAction("Install", false) != "Install" || SelectAction("Install", true) != "CompleteAssociation" || SelectAction("Uninstall", true) != "Uninstall")
      throw new InvalidDataException("Contrat de reprise incorrect.");
     Console.WriteLine(Package.Json.Serialize(new {passed = true, package = verified, driverVersion = Package.DriverVersion(), driverSigningMode = Package.SigningMode(), installedAnything = false}));
     return 0;
    }

    ConsoleCancelEventHandler cancel = (sender, e) => {e.Cancel = true; Log("Operation in progress: wait for it to finish before closing the terminal.");};
    Console.CancelKeyPress += cancel;
    try {
     if (!Administrator) {
      Log("Administrator approval requested. Output will appear in the elevated console.");
      var start = new ProcessStartInfo(Assembly.GetExecutingAssembly().Location, action == "Install" ? "/install" : "/uninstall") {
       UseShellExecute = true, Verb = "runas", WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory
      };
      using (var child = Process.Start(start)) {
       if (child == null) throw new IOException("Cannot start the elevated console.");
       child.WaitForExit();
       Log("Code de sortie : " + child.ExitCode);
       return child.ExitCode;
      }
     }

     Log("Interceptor · driver " + Package.DriverVersion() + " · signature " + Package.SigningMode());
     string selected = SelectAction(action, action == "Install" && Package.PendingControllerAssociation());
     if (selected == "CompleteAssociation") Log("Resuming associations requested by the previous installation.");
     // Exactly the engine called by Dashboard.Operation in InstallerUI/App.cs.
     int result = Package.Execute(selected, Log, Log);
     if (result == 0) Log("Operation completed.");
     else if (result == 3010) Log(action == "Install"
      ? "Operation completed. Restart Windows, then run /install again to finish device associations."
      : "Operation completed. Restart Windows to finish uninstalling.");
     else Console.Error.WriteLine("Operation failed · code " + result + ". See the log above.");
     return result;
    } finally { Console.CancelKeyPress -= cancel; }
   } catch (Win32Exception e) {
    if (e.NativeErrorCode == 1223) { Console.Error.WriteLine("Administrator approval cancelled."); return 1223; }
    Console.Error.WriteLine(e.ToString()); return 1;
   } catch (Exception e) { Console.Error.WriteLine(e.ToString()); return 1; }
  }
 }
}

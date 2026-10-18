using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;

namespace InterceptorSetup {
 public sealed class DiagnosticFinding {
  public string Check {get;set;} public string Status {get;set;} public string Message {get;set;}
 }
 public sealed class DiagnosticDevice : INotifyPropertyChanged {
  public DeviceRow Device {get;set;} public List<DiagnosticFinding> Findings {get;set;}
  public DiagnosticDevice(){Findings=new List<DiagnosticFinding>();}
  public string Name {get{return Device.Name;}} public string Subtitle {get{return Device.Kind+" · "+Device.VidPid;}}
  string state="Waiting";public string State {get{return state;}set{state=value;Changed("State");Changed("Color");}}
  public Brush Color {get{return new SolidColorBrush(State=="Error"?Colors.Firebrick:State=="Checks passed"?ColorFrom("#168264"):State=="Running"?ColorFrom("#1464E8"):ColorFrom("#61758E"));}}
  static Color ColorFrom(string value){return (Color)ColorConverter.ConvertFromString(value);}
  public event PropertyChangedEventHandler PropertyChanged;void Changed(string name){var handler=PropertyChanged;if(handler!=null)handler(this,new PropertyChangedEventArgs(name));}
  public void Add(string check,string status,string message){Findings.Add(new DiagnosticFinding{Check=check,Status=status,Message=message});}
  public void Complete(){State=Findings.Any(x=>x.Status=="error")?"Error":Findings.Any(x=>x.Status=="unverified"||x.Status=="warning")?"Partially verified":"Checks passed";}
 }
 public sealed class DiagnosticReport {
  public string StartedUtc {get;set;}public string CompletedUtc {get;set;}
  public string Mode {get;set;} public bool Cancelled {get;set;} public Snapshot Snapshot {get;set;}
  public List<DiagnosticFinding> Global {get;set;}
  public List<DiagnosticDevice> Devices {get;set;}
  public DiagnosticReport(){StartedUtc=DateTime.UtcNow.ToString("o");Global=new List<DiagnosticFinding>();Devices=new List<DiagnosticDevice>();}
  public int Errors {get{return Global.Concat(Devices.SelectMany(x=>x.Findings)).Count(x=>x.Status=="error");}}
  public int Unverified {get{return Global.Concat(Devices.SelectMany(x=>x.Findings)).Count(x=>x.Status=="unverified");}}
  public int Warnings {get{return Global.Concat(Devices.SelectMany(x=>x.Findings)).Count(x=>x.Status=="warning");}}
  public string Text(DiagnosticDevice selected=null){
   var s=new StringBuilder();s.AppendLine("INTERCEPTOR · QUICK AUTOMATIC DIAGNOSTICS");s.AppendLine("Started UTC: "+StartedUtc);s.AppendLine("Completed UTC: "+(CompletedUtc??"running"));s.AppendLine("Mode: "+Mode);s.AppendLine("Status: "+(Cancelled?"cancelled":CompletedUtc==null?"running":"completed"));
   s.AppendLine(Errors+" error(s), "+Warnings+" warning(s), "+Unverified+" unverified check(s).");
   s.AppendLine("A quick check does not validate every key, button, axis or proprietary feature. Diagnostics install no drivers and change no device associations.");
   if(Snapshot!=null){s.AppendLine("Windows : "+Snapshot.Os);s.AppendLine("Driver: "+Snapshot.DriverState+" · "+Snapshot.DriverPath);s.AppendLine("Configured SHA-256: "+Snapshot.DriverSha256);s.AppendLine("API: "+Snapshot.ApiState+" · "+Snapshot.ApiDetail);s.AppendLine("Profiles: "+Snapshot.ControllerProfiles);s.AppendLine("Recovery: "+Snapshot.RecoveryState);}
   foreach(var f in Global)s.AppendLine("["+Label(f.Status)+"] "+f.Check+" — "+f.Message);
   foreach(var d in Devices.Where(x=>selected==null||x==selected)){s.AppendLine();s.AppendLine("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");s.AppendLine(d.Name+" · "+d.Subtitle+" · "+d.State);s.AppendLine("Instance: "+d.Device.Instance);s.AppendLine("Parent: "+d.Device.ParentInstance);s.AppendLine("Windows driver: "+d.Device.InstalledInf+" · "+d.Device.DriverVersion+" · "+d.Device.DriverProvider);s.AppendLine("Service: "+d.Device.Service+" · PnP : "+d.Device.WindowsState+" · code "+d.Device.ProblemCode);s.AppendLine("Hardware IDs: "+d.Device.HardwareIds);s.AppendLine("Compatible IDs: "+d.Device.CompatibleIds);s.AppendLine("UpperFilters : "+d.Device.UpperFilters);s.AppendLine("LowerFilters : "+d.Device.LowerFilters);s.AppendLine("Class filters: "+d.Device.ClassFilters);foreach(var f in d.Findings)s.AppendLine("["+Label(f.Status)+"] "+f.Check+" — "+f.Message);}
   return s.ToString();
  }
  public static string Label(string status){switch(status){case "pass":return "OK";case "error":return "ERROR";case "warning":return "WARNING";case "unverified":return "UNVERIFIED";default:return "INFO";}}
 }
 public sealed class DiagnosticEvent {public string check {get;set;}public string status {get;set;}public string message {get;set;}public int progress {get;set;}}
 internal sealed class DiagnosticWorkerHost : IDisposable {
  string folder;public string Executable {get;private set;}
  public DiagnosticWorkerHost(){folder=Package.ExtractDiagnosticWorker();Executable=Path.Combine(folder,"InterceptorDiagnosticWorker.exe");}
  static string Quote(string value){var s=new StringBuilder("\"");int slashes=0;foreach(char c in value){if(c=='\\'){slashes++;continue;}if(c=='"'){s.Append('\\',slashes*2+1);s.Append(c);}else{s.Append('\\',slashes);s.Append(c);}slashes=0;}s.Append('\\',slashes*2);s.Append('"');return s.ToString();}
  public async Task Run(DeviceRow device,int type,IntPtr owner,IProgress<DiagnosticEvent> progress,CancellationToken cancel){
   var start=new ProcessStartInfo(Executable,"--device "+Quote(device.Instance)+" --type "+type+" --window "+owner.ToInt64().ToString(System.Globalization.CultureInfo.InvariantCulture)){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true,StandardOutputEncoding=Encoding.UTF8,StandardErrorEncoding=Encoding.UTF8,WorkingDirectory=folder};
   using(var process=new Process{StartInfo=start}){
    var malformed=new List<string>();object gate=new object();
    process.OutputDataReceived+=(s,e)=>{if(e.Data==null)return;try{var item=Package.Json.Deserialize<DiagnosticEvent>(e.Data);if(item==null||item.check==null||!new[]{"pass","error","warning","info","unverified"}.Contains(item.status)||item.progress<0||item.progress>100)throw new InvalidDataException("Invalid event");progress.Report(item);}catch(Exception ex){lock(gate)malformed.Add("Invalid worker output: "+ex.Message);}};
    process.ErrorDataReceived+=(s,e)=>{if(e.Data!=null)lock(gate)malformed.Add(e.Data);};
    cancel.ThrowIfCancellationRequested();if(!process.Start())throw new IOException("Cannot start the native test.");process.BeginOutputReadLine();process.BeginErrorReadLine();
    using(cancel.Register(()=>{try{if(!process.HasExited)process.Kill();}catch(InvalidOperationException){}catch(Win32Exception){}})){
     bool ended=await Task.Run(()=>process.WaitForExit(5000));if(!ended){process.Kill();await Task.Run(()=>process.WaitForExit(5000));throw new TimeoutException("The native test exceeded five seconds. Its process was stopped to release capture.");}
     process.WaitForExit();cancel.ThrowIfCancellationRequested();
     string[] errors;lock(gate)errors=malformed.ToArray();if(errors.Length>0)throw new InvalidDataException(String.Join(Environment.NewLine,errors));
     if(process.ExitCode!=0&&process.ExitCode!=2)throw new InvalidOperationException("Worker exited unexpectedly: code "+process.ExitCode+". Windows closes the process capture handles.");
    }
   }
  }
  public void Dispose(){if(folder!=null){Package.Cleanup(folder);folder=null;}}
 }
 public sealed class DiagnosticWindow {
  public readonly Window Window;readonly ObservableCollection<DiagnosticDevice> rows=new ObservableCollection<DiagnosticDevice>();
  readonly ProgressBar progress=new ProgressBar{Minimum=0,Maximum=100,Height=9,Foreground=Brush("#1464E8"),Background=Brush("#DCE6F5"),BorderThickness=new Thickness(0)};
  readonly TextBlock title=new TextBlock(),phase=new TextBlock(),percent=new TextBlock(),summary=new TextBlock();readonly TextBox reportBox=new TextBox();
  readonly ListBox list=new ListBox();readonly Button cancel=new Button(),copy=new Button(),save=new Button();readonly CancellationTokenSource cancellation=new CancellationTokenSource();
  readonly bool preview;readonly string screenshot;bool running,closed,closeRequested;double highest;DiagnosticWorkerHost worker;
  public DiagnosticReport Report {get;private set;}
  public event Action Finished;
  static Brush Brush(string hex){return new SolidColorBrush((Color)ColorConverter.ConvertFromString(hex));}
  static TextBlock Label(string text,double size,Brush color){return new TextBlock{Text=text,FontSize=size,Foreground=color,TextWrapping=TextWrapping.Wrap};}
  static Button Button(string text){return new Button{Content=text,Padding=new Thickness(17,10,17,10),Margin=new Thickness(0,0,9,0),Background=Brushes.White,BorderBrush=Brush("#D5DFEC"),Foreground=Brush("#173556"),Cursor=System.Windows.Input.Cursors.Hand};}
  static Border Card(UIElement child){return new Border{Child=child,Background=Brushes.White,CornerRadius=new CornerRadius(14),BorderBrush=Brush("#E4EAF3"),BorderThickness=new Thickness(1),Padding=new Thickness(20)};}
  public DiagnosticWindow(Window owner,bool preview=false,string screenshot=null){
   this.preview=preview;this.screenshot=screenshot;Report=new DiagnosticReport();
   Window=new Window{Owner=owner,Title="Interceptor · Automatic diagnostics",Width=1160,Height=810,MinWidth=900,MinHeight=650,WindowStartupLocation=owner==null?WindowStartupLocation.CenterScreen:WindowStartupLocation.CenterOwner,Background=Brush("#F3F6FB"),Foreground=Brush("#152B49"),FontFamily=new FontFamily("Segoe UI")};
   var root=new Grid{Margin=new Thickness(26)};root.RowDefinitions.Add(new RowDefinition{Height=GridLength.Auto});root.RowDefinitions.Add(new RowDefinition{Height=GridLength.Auto});root.RowDefinitions.Add(new RowDefinition{Height=new GridLength(1,GridUnitType.Star)});root.RowDefinitions.Add(new RowDefinition{Height=GridLength.Auto});Window.Content=root;
   var heading=new StackPanel{Margin=new Thickness(0,0,0,18)};heading.Children.Add(Label("QUICK AUTOMATIC DIAGNOSTICS",11,Brush("#1464E8")));title.Text="Check each device";title.FontSize=28;title.FontWeight=FontWeights.SemiBold;title.Margin=new Thickness(0,5,0,7);heading.Children.Add(title);heading.Children.Add(Label("Installation, identity, capture and Windows reception. These diagnostics install no drivers.",13,Brush("#61758E")));root.Children.Add(heading);
   var status=new Grid();status.RowDefinitions.Add(new RowDefinition{Height=GridLength.Auto});status.RowDefinitions.Add(new RowDefinition{Height=GridLength.Auto});status.RowDefinitions.Add(new RowDefinition{Height=GridLength.Auto});var line=new DockPanel();percent.Text="0 %";percent.FontSize=18;percent.FontWeight=FontWeights.SemiBold;percent.Foreground=Brush("#1464E8");percent.MinWidth=65;DockPanel.SetDock(percent,Dock.Right);line.Children.Add(percent);phase.Text="Preparing diagnostics…";phase.FontSize=15;phase.FontWeight=FontWeights.SemiBold;line.Children.Add(phase);status.Children.Add(line);progress.Margin=new Thickness(0,15,0,12);Grid.SetRow(progress,1);status.Children.Add(progress);summary.FontSize=12;summary.Foreground=Brush("#61758E");summary.TextWrapping=TextWrapping.Wrap;summary.Text="Features that cannot be tested automatically are marked unverified.";Grid.SetRow(summary,2);status.Children.Add(summary);var statusCard=Card(status);statusCard.Margin=new Thickness(0,0,0,17);Grid.SetRow(statusCard,1);root.Children.Add(statusCard);
   var body=new Grid();body.ColumnDefinitions.Add(new ColumnDefinition{Width=new GridLength(310)});body.ColumnDefinitions.Add(new ColumnDefinition{Width=new GridLength(1,GridUnitType.Star)});Grid.SetRow(body,2);root.Children.Add(body);
   list.ItemsSource=rows;list.BorderThickness=new Thickness(0);list.Background=Brushes.White;list.HorizontalContentAlignment=HorizontalAlignment.Stretch;list.SetValue(ScrollViewer.HorizontalScrollBarVisibilityProperty,ScrollBarVisibility.Disabled);list.SelectionChanged+=(s,e)=>UpdateReport();
   var template=new DataTemplate(typeof(DiagnosticDevice));var stack=new FrameworkElementFactory(typeof(StackPanel));stack.SetValue(StackPanel.MarginProperty,new Thickness(2,8,2,10));var name=new FrameworkElementFactory(typeof(TextBlock));name.SetBinding(TextBlock.TextProperty,new System.Windows.Data.Binding("Name"));name.SetValue(TextBlock.FontWeightProperty,FontWeights.SemiBold);name.SetValue(TextBlock.TextWrappingProperty,TextWrapping.Wrap);stack.AppendChild(name);var sub=new FrameworkElementFactory(typeof(TextBlock));sub.SetBinding(TextBlock.TextProperty,new System.Windows.Data.Binding("Subtitle"));sub.SetValue(TextBlock.FontSizeProperty,11.0);sub.SetValue(TextBlock.ForegroundProperty,Brush("#61758E"));sub.SetValue(TextBlock.MarginProperty,new Thickness(0,3,0,4));stack.AppendChild(sub);var state=new FrameworkElementFactory(typeof(TextBlock));state.SetBinding(TextBlock.TextProperty,new System.Windows.Data.Binding("State"));state.SetBinding(TextBlock.ForegroundProperty,new System.Windows.Data.Binding("Color"));state.SetValue(TextBlock.FontSizeProperty,12.0);stack.AppendChild(state);template.VisualTree=stack;list.ItemTemplate=template;var devicesCard=Card(list);devicesCard.Padding=new Thickness(10);devicesCard.Margin=new Thickness(0,0,15,0);body.Children.Add(devicesCard);
   reportBox.IsReadOnly=true;reportBox.FontFamily=new FontFamily("Consolas");reportBox.FontSize=12;reportBox.BorderThickness=new Thickness(0);reportBox.Background=Brushes.White;reportBox.Foreground=Brush("#263D58");reportBox.TextWrapping=TextWrapping.Wrap;reportBox.VerticalScrollBarVisibility=ScrollBarVisibility.Auto;reportBox.HorizontalScrollBarVisibility=ScrollBarVisibility.Disabled;var reportCard=Card(reportBox);Grid.SetColumn(reportCard,1);body.Children.Add(reportCard);
   var footer=new DockPanel{Margin=new Thickness(0,17,0,0)};Grid.SetRow(footer,3);root.Children.Add(footer);var actions=new StackPanel{Orientation=Orientation.Horizontal,HorizontalAlignment=HorizontalAlignment.Right};DockPanel.SetDock(actions,Dock.Right);copy.Content="Copy report";copy.Padding=new Thickness(17,10,17,10);copy.Margin=new Thickness(0,0,9,0);save.Content="Save";save.Padding=new Thickness(17,10,17,10);save.Margin=new Thickness(0,0,9,0);cancel.Content="Cancel";cancel.Padding=new Thickness(17,10,17,10);foreach(var button in new[]{copy,save,cancel}){button.Background=Brushes.White;button.BorderBrush=Brush("#D5DFEC");actions.Children.Add(button);}footer.Children.Add(actions);footer.Children.Add(Label("An idle device does not indicate a fault.",12,Brush("#61758E")));
   copy.Click+=async(s,e)=>{try{Clipboard.SetText(Report.Text());copy.Content="Copied";await Task.Delay(1500);if(!closed)copy.Content="Copy report";}catch(ExternalException error){summary.Text="Clipboard unavailable: "+error.Message+". You can save the report instead.";}};
   save.Click+=(s,e)=>{var dialog=new Microsoft.Win32.SaveFileDialog{FileName="Interceptor-diagnostic-"+DateTime.Now.ToString("yyyyMMdd-HHmmss")+".txt",Filter="Text report (*.txt)|*.txt"};if(dialog.ShowDialog(Window)==true)try{File.WriteAllText(dialog.FileName,Report.Text(),Encoding.UTF8);}catch(Exception error){summary.Text="Cannot save: "+error.Message;}};
   cancel.Click+=(s,e)=>{if(running){cancellation.Cancel();phase.Text="Stopping the test and releasing devices…";cancel.IsEnabled=false;}else Window.Close();};
   Window.Closing+=(s,e)=>{if(running){e.Cancel=true;closeRequested=true;cancellation.Cancel();phase.Text="Stopping the test and releasing devices…";cancel.IsEnabled=false;}};
   Window.Closed+=(s,e)=>{closed=true;cancellation.Dispose();};Window.Loaded+=async(s,e)=>{if(preview){Preview();if(screenshot!=null){await Task.Delay(350);SavePreview(screenshot);Window.Close();}}else await Run();};
  }
  void Progress(double value,string text){if(closed)return;highest=Math.Max(highest,Math.Min(100,value));double from=progress.Value;progress.BeginAnimation(RangeBase.ValueProperty,null);progress.Value=highest;progress.BeginAnimation(RangeBase.ValueProperty,new DoubleAnimation(from,highest,TimeSpan.FromMilliseconds(180)){EasingFunction=new CubicEase{EasingMode=EasingMode.EaseOut},FillBehavior=FillBehavior.Stop});percent.Text=((int)highest)+" %";phase.Text=text;}
  void UpdateReport(){if(closed)return;reportBox.Text=Report.Text(list.SelectedItem as DiagnosticDevice);if(running)reportBox.ScrollToEnd();else reportBox.ScrollToHome();}
  static int Type(DeviceRow row){switch(row.Kind){case "Keyboard":return 1;case "Mouse":return 2;case "HID controller":return 3;case "XUSB":return 4;case "XInputHID":return 5;case "GIP":return 6;default:return 0;}}
  static int FilterCount(string text){return (text??"").Split(';').Count(x=>x.Equals("Interceptor",StringComparison.OrdinalIgnoreCase));}
  internal static void CheckDevice(DiagnosticDevice d,Snapshot snapshot){
   var row=d.Device;
   d.Add("Windows / PnP",row.PnpQuerySucceeded&&row.ProblemCode==0&&(row.PnpStatus&8)!=0?"pass":"error","Status="+row.WindowsState+", problem code="+row.ProblemCode+", flags="+row.PnpStatus+". "+(!row.PnpQuerySucceeded?"Incomplete PnP query: reconnect the device and run diagnostics again.":row.ProblemCode==14?"Windows requires a restart.":row.ProblemCode!=0?"Look up this Windows code in Device Manager.":""));
   d.Add("Windows driver",String.IsNullOrEmpty(row.InstalledInf)?"warning":"pass","INF="+row.InstalledInf+", version="+row.DriverVersion+", provider="+row.DriverProvider+", service="+row.Service+".");
   if(row.Kind=="Control"){d.Add("Interceptor control",snapshot.ApiAvailable?"pass":"error",snapshot.ApiDetail);return;}
   if(row.Kind=="Xbox HID view"||row.Kind=="XInputHID"&&row.PropertyFlags==14){d.Add("Secondary view","unverified","This collection is the Windows view of a native controller. Capture is tested on its XUSB/GIP parent; this row is not tested separately. See the parent results.");return;}
   d.Add("Association",row.Associated?"pass":"warning",row.Associated?"An Interceptor reference is present; actual availability is checked separately.":"Interceptor is not associated with this instance. Use Associate new devices if its profile is supported, then restart if Windows requests it.");
   if(row.Associated&&Type(row)>=3){
    bool lower=row.Kind=="GIP";int correct=lower?FilterCount(row.LowerFilters):FilterCount(row.UpperFilters)+FilterCount(row.ClassUpperFilters);int wrong=lower?FilterCount(row.UpperFilters)+FilterCount(row.ClassUpperFilters)+FilterCount(row.ClassLowerFilters):FilterCount(row.LowerFilters)+FilterCount(row.ClassLowerFilters);
    d.Add("Filter position",correct==1&&wrong==0?"pass":"error","Expected: "+(lower?"GIP instance LowerFilters":"Collection or class UpperFilters")+"; correct references="+correct+", references elsewhere="+wrong+". "+(correct==1&&wrong==0?"Filter placement is consistent. Presence alone does not prove filter ordering or exclude third-party conflicts.":"Inconsistent or duplicate association: restore and associate using the installer. Do not manually remove third-party filters."));
   }
   if(Type(row)>=3)d.Add("Interface",row.InterfaceActive?"pass":"warning",row.InterfaceActive?"A private interface is published. This does not yet prove its reader is ready.":"No private interface: association may be missing, the stack may need restarting, the profile may be unsupported, or the reader may still be preparing.");
   if(row.Kind=="XInputHID")d.Add("XInputHID profile",row.PropertyFlags==6?"pass":"unverified","DevicePropertyFlags="+(row.PropertyFlags.HasValue?row.PropertyFlags.Value.ToString():"unknown")+"; the generic source profile requires 6. Diagnostics never change this value.");
   d.Add("Hardware features","unverified","An automatic quick check does not individually test every key, button, axis, vibration, audio, Bluetooth/radio or proprietary feature. A valid descriptor confirms what is declared, not physical operation.");
  }
  async Task Run(){
   running=true;Report.Mode="Quick automatic diagnostics · bounded native tests on installed devices";
   try{
    Progress(2,"Reading the package and Windows…");string integrity=await Task.Run(()=>Package.Verify());Report.Global.Add(new DiagnosticFinding{Check="Embedded package",Status="pass",Message=integrity});
    Report.Snapshot=await Task.Run(()=>Native.Inspect());cancellation.Token.ThrowIfCancellationRequested();
    Report.Global.Add(new DiagnosticFinding{Check="API",Status=Report.Snapshot.ApiAvailable?"pass":"warning",Message=Report.Snapshot.ApiState+" — "+Report.Snapshot.ApiDetail});
    Report.Global.Add(new DiagnosticFinding{Check="Device restart",Status=Report.Snapshot.DeviceRestartPending==false?"pass":"warning",Message=Report.Snapshot.DeviceRestartPending==true?"An Interceptor association still requires a Windows restart. Tests describe the currently loaded stack; restart before evaluating the new association.":Report.Snapshot.DeviceRestartPending==false?"No Interceptor restart request is recorded for this Windows session.":"The restart request status could not be read."});
    Report.Global.Add(new DiagnosticFinding{Check="Driver file",Status=Report.Snapshot.DriverFileMatchesPayload==true?"pass":"warning",Message=Report.Snapshot.DriverFileMatchesPayload==true?"The configured file matches the packaged driver. Code already loaded in memory is not measured.":"The configured file is missing or differs from the packaged driver. Compare versions, signatures and pending restarts; a different version alone does not indicate a fault."});
    foreach(var device in Report.Snapshot.Devices){var row=new DiagnosticDevice{Device=device};rows.Add(row);Report.Devices.Add(row);}Progress(10,rows.Count+" device(s) to examine");UpdateReport();
    if(Report.Snapshot.ApiAvailable&&App.Administrator)worker=await Task.Run(()=>new DiagnosticWorkerHost());
    for(int i=0;i<rows.Count;++i){
     cancellation.Token.ThrowIfCancellationRequested();var d=rows[i];d.State="Running";list.SelectedItem=d;list.ScrollIntoView(d);CheckDevice(d,Report.Snapshot);int index=i;Progress(10+85.0*i/Math.Max(1,rows.Count),(i+1)+" / "+rows.Count+" · "+d.Name);UpdateReport();
     bool secondary=d.Device.Kind=="Xbox HID view"||d.Device.Kind=="XInputHID"&&d.Device.PropertyFlags==14;int type=Type(d.Device);
     if(type>0&&!secondary){
      if(worker==null)d.Add("Capture and modification","unverified",!App.Administrator?"Administrator rights are required to test the driver. Read-only inventory remains available.":"API unavailable: no capture context or injection was attempted. Install or start the correct driver, then retry.");
      else if(!d.Device.Associated)d.Add("Capture and modification","unverified","Device not associated: no active test or installation change was attempted.");
      else if(d.Device.ProblemCode!=0)d.Add("Capture and modification","unverified","Windows reports a fault or pending restart; the active test was skipped.");
      else {
       var events=new Progress<DiagnosticEvent>(item=>{if(closed)return;if(item.check!="sample")d.Add(item.check,item.status,item.message);Progress(10+85.0*(index+item.progress/100.0)/Math.Max(1,rows.Count),(index+1)+" / "+rows.Count+" · "+d.Name);UpdateReport();});
       try{await worker.Run(d.Device,type,new WindowInteropHelper(Window).Handle,events,cancellation.Token);await Window.Dispatcher.InvokeAsync(()=>{},DispatcherPriority.Background);}
       catch(OperationCanceledException){throw;}catch(Exception error){d.Add("Test natif","error",error.ToString());}
      }
     }
     d.Complete();Progress(10+85.0*(i+1)/Math.Max(1,rows.Count),(i+1)+" / "+rows.Count+" checked");UpdateReport();
    }
    if(rows.Count==0)Report.Global.Add(new DiagnosticFinding{Check="Inventory",Status="unverified",Message="No recognized input devices in this inventory. No hardware test was performed."});
    Progress(100,"Diagnostics completed");
   }catch(OperationCanceledException){Report.Cancelled=true;foreach(var row in rows.Where(x=>x.State=="Running"||x.State=="Waiting")){row.Add("Cancellation","unverified","Test cancelled or skipped at the user's request.");row.State="Cancelled";}phase.Text="Diagnostics cancelled · captures released";}
   catch(Exception error){Report.Global.Add(new DiagnosticFinding{Check="Diagnostic",Status="error",Message=error.ToString()});phase.Text="Diagnostics interrupted · see the report";}
   finally{
    if(worker!=null)try{worker.Dispose();}catch(Exception error){Report.Global.Add(new DiagnosticFinding{Check="Worker cleanup",Status="warning",Message=error.Message});}worker=null;running=false;Report.CompletedUtc=DateTime.UtcNow.ToString("o");summary.Text=Report.Errors+" error(s) · "+Report.Warnings+" warning(s) · "+Report.Unverified+" unverified check(s).";cancel.Content="Close";cancel.IsEnabled=true;UpdateReport();if(closeRequested)Window.Close();var finished=Finished;if(finished!=null)finished();
   }
  }
  void Preview(){Report.Mode="SIMULATED PREVIEW · no hardware tests";foreach(var device in new[]{new DeviceRow{Name="HID keyboard",Kind="Keyboard",VidPid="1234 : 1001",Instance=@"HID\DEMO\keyboard"},new DeviceRow{Name="Simulation controller",Kind="HID controller",VidPid="1234 : 1002",Instance=@"HID\DEMO\simulation"},new DeviceRow{Name="Xbox GIP",Kind="GIP",VidPid="0E6F : 0302",Instance=@"USB\DEMO\gip"}}){var row=new DiagnosticDevice{Device=device};row.Add("Identity","pass","The device instance was identified unambiguously.");row.Add("Interception","unverified","No physical input was received during the quick check. The device may be idle.");row.Complete();rows.Add(row);Report.Devices.Add(row);}Report.Global.Add(new DiagnosticFinding{Check="Preview",Status="info",Message="Simulated window preview. No capture or installation."});Report.CompletedUtc=DateTime.UtcNow.ToString("o");Progress(100,"Diagnostics completed");summary.Text="3 devices · 0 errors · 3 unverified checks";cancel.Content="Close";UpdateReport();}
  void SavePreview(string path){Window.UpdateLayout();var bmp=new System.Windows.Media.Imaging.RenderTargetBitmap((int)Window.ActualWidth,(int)Window.ActualHeight,96,96,PixelFormats.Pbgra32);bmp.Render(Window);var png=new System.Windows.Media.Imaging.PngBitmapEncoder();png.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bmp));using(var file=File.Create(Path.GetFullPath(path)))png.Save(file);}
  internal static void SelfTest(){var report=new DiagnosticReport{Mode="self-test"};var row=new DiagnosticDevice{Device=new DeviceRow{Name="Test",Instance=@"HID\TEST"}};row.Add("capture","unverified","No physical input.");row.Complete();if(row.State!="Partially verified")throw new Exception("Unobserved input must not pass");report.Devices.Add(row);if(report.Unverified!=1||!report.Text().Contains("UNVERIFIED"))throw new Exception("Diagnostic report completeness");row.Add("Windows","error","PnP 10");row.Complete();if(row.State!="Error"||report.Errors!=1)throw new Exception("Diagnostic errors must dominate verdict");}
 }
}

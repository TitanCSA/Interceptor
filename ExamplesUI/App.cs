using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace InterceptorViewer {
 public static class App {
  internal static readonly int[] SupportedKinds=new[]{0,1,2,3,4,5};
  internal static bool Administrator{get{return new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator);}}
  internal static void Json(string path,object value){File.WriteAllText(Path.GetFullPath(path),new JavaScriptSerializer().Serialize(value));}
  [STAThread] public static int Main(string[] args){
   try {
    if(args.Length==2&&args[0]=="--self-test"){SelfTest(args[1]);return 0;}
    if(args.Length==2&&args[0]=="--render-tests"){RenderTests(args[1]);return 0;}
    if(args.Length==2&&args[0]=="--lifecycle-test")return LifecycleTests(args[1]);
    if(args.Length==2&&args[0]=="--native-lifecycle-test")return LifecycleTests(args[1],true);
    int observe=-1;if(args.Length==2&&args[0]=="--observe"){observe=int.Parse(args[1],CultureInfo.InvariantCulture);if(!SupportedKinds.Contains(observe))throw new ArgumentException("Invalid device type.");}else if(args.Length!=0)throw new ArgumentException("Arguments: --self-test JSON, --render-tests directory, --observe type(0..5).");
    DriverInstallation.RequireInstalled();
    var application=new Application();using(var dashboard=new Dashboard(false)){if(observe>=0)dashboard.ObserveOnLoad(observe);application.Run(dashboard.Window);}return 0;
   }catch(Exception e){
    try{if(args.Length==2&&(args[0]=="--self-test"||args[0]=="--lifecycle-test"||args[0]=="--native-lifecycle-test"))Json(args[1],new{passed=false,error=e.ToString()});else if(args.Length==2&&args[0]=="--render-tests"){Directory.CreateDirectory(args[1]);Json(Path.Combine(args[1],"render-results.json"),new{passed=false,error=e.ToString()});}else MessageBox.Show(e.Message,"Interceptor · graphical example",MessageBoxButton.OK,MessageBoxImage.Error);}catch{}return 1;
   }
  }
  static void Check(bool condition,string message){if(!condition)throw new InvalidDataException(message);}
  static void SelfTest(string output){
   DriverInstallation.RequireInstalled(0);
   bool missing=false;try{DriverInstallation.RequireInstalled(1060);}catch(InvalidOperationException e){missing=e.Message.Contains("Interceptor is not installed")&&e.Message.Contains("InterceptorSetupConsole.exe /install");}
   Check(missing,"Missing-driver installation instructions");
   bool denied=false;try{DriverInstallation.RequireInstalled(5);}catch(Win32Exception e){denied=e.NativeErrorCode==5&&!e.Message.Contains("is not installed");}
   Check(denied,"Access denied must not be reported as a missing driver");
   Check(Marshal.SizeOf(typeof(DeviceInfo))==800&&Marshal.SizeOf(typeof(DeviceState))==1064&&Marshal.SizeOf(typeof(Snapshot))==80&&Marshal.SizeOf(typeof(Diagnostic))==1096,"ABI sizes");
   Check(Marshal.OffsetOf(typeof(Diagnostic),"Message").ToInt32()==72,"Diagnostic ABI offset");
   Check(Marshal.OffsetOf(typeof(DeviceState),"Keys").ToInt32()==832&&Marshal.OffsetOf(typeof(DeviceState),"Buttons").ToInt32()==952,"ABI offsets");Check(InputDrawing.KeyCount==105,"Incomplete ISO keyboard");
   int[] gipBits={8,9,10,11,2,3,14,15,12,13,-1,-1,4,5,6,7};
   for(int bit=0;bit<gipBits.Length;++bit){Check(InputDrawing.NativeButtonBit(bit,true)==gipBits[bit],"GIP layout: bit "+bit);Check(InputDrawing.NativeButtonBit(bit,false)==bit,"XUSB layout: bit "+bit);}
   Check(InputDrawing.NativeButtonBit(16,true)==16,"GIP Guide preserved");
   var a=DeviceState.Empty();a.Info=new DeviceInfo{Type=1,Id=1};a.Keys[0x23/8]|=(byte)(1<<(0x23%8));
   var b=DeviceState.Empty();b.Info=new DeviceInfo{Type=1,Id=2};b.Keys[0x23/8]|=(byte)(1<<(0x23%8));b.Keys[0x24/8]|=(byte)(1<<(0x24%8));
   Check(Dashboard.Aggregate(new[]{a,b},0).Pressed(0x23),"H aggregation");a.Keys=new byte[96];var combined=Dashboard.Aggregate(new[]{a,b},0);Check(combined.Pressed(0x23)&&combined.Pressed(0x24),"Release on another keyboard");b.Keys=new byte[96];Check(!Dashboard.Aggregate(new[]{a,b},0).Pressed(0x23),"Final release");
   var m=DeviceState.Empty();m.MouseButtons=1;m.Sequence=1;m.MouseX=17;var n=DeviceState.Empty();n.MouseButtons=8;n.Sequence=2;n.MouseX=-23;var merged=Dashboard.Aggregate(new[]{m,n},1);Check(merged.MouseButtons==9&&merged.MouseX==-23,"Mouse aggregation");
   Snapshot header;using(var bridge=new Bridge()){var states=bridge.Read(out header);Check(header.Status==0&&states.Length==0,"Idle bridge without capture");var diagnostic=bridge.ReadDiagnostic();Check(diagnostic.Stage==0&&diagnostic.Error==0,"Idle diagnostic without capture");}
   BridgeConcurrencyTest();
   int frameChecks=FrameRefreshTests.Run();
   using(var dashboard=new Dashboard(true)){dashboard.SetPreview(SupportedKinds[0]);dashboard.LayoutForRender();dashboard.TestDiagnosticPresentation();}
   Json(output,new{passed=true,installationCheck="installed, missing with setup instructions, and access denied",snapshotBytes=80,deviceBytes=1064,diagnosticBytes=1096,diagnostics="automatic stops: exact operation, family, device, sequence, Win32/reset codes, partial sends, copyable complete cause; offline status and ABI tests",keyCount=InputDrawing.KeyCount,aggregation="multi-keyboard release and mouse last packet",bridge="embedded DLL loaded, stopped snapshot read; four concurrent Read/Stop callers racing Dispose, repeated four times",display="event-driven native publication and one pending WPF frame; no application FPS cap, display timer or idle rendering subscription",frameChecks=frameChecks,captureStarted=false,installedAnything=false});
  }
  static void BridgeConcurrencyTest(){
   for(int cycle=0;cycle<4;++cycle)using(var bridge=new Bridge())using(var ready=new CountdownEvent(4))using(var go=new ManualResetEvent(false)){
    var calls=new List<Task>();
    for(int caller=0;caller<4;++caller)calls.Add(Task.Run(()=>{
     Snapshot snapshot;try{Check(bridge.Read(out snapshot).Length==0,"Initial read before concurrent calls");}finally{ready.Signal();}
     go.WaitOne();for(int i=0;i<128;++i){bridge.Stop();try{bridge.Read(out snapshot);bridge.ReadDiagnostic();}catch(ObjectDisposedException){break;}}
    }));
    Check(ready.Wait(10000),"Bridge test threads did not start");calls.Add(Task.Run(()=>{go.WaitOne();bridge.Dispose();}));go.Set();Task.WaitAll(calls.ToArray());
    bool rejected=false;try{Snapshot snapshot;bridge.Read(out snapshot);}catch(ObjectDisposedException){rejected=true;}
    Check(rejected,"Read after unload must be rejected");rejected=false;try{bridge.ReadDiagnostic();}catch(ObjectDisposedException){rejected=true;}Check(rejected,"Diagnostic read after unload must be rejected");bridge.Stop();bridge.Dispose();
   }
  }
  static void RenderTests(string directory){
   directory=Path.GetFullPath(directory);Directory.CreateDirectory(directory);string[] names={"keyboard.png","mouse.png","hid.png","xusb.png","xinputhid.png","gip.png"};
   using(var dashboard=new Dashboard(true))foreach(int i in SupportedKinds){
    dashboard.SetPreview(i);dashboard.LayoutForRender();var bitmap=new RenderTargetBitmap(1200,800,96,96,PixelFormats.Pbgra32);bitmap.Render(dashboard.Root);var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(bitmap));using(var stream=File.Create(Path.Combine(directory,names[i])))encoder.Save(stream);
   }
   Json(Path.Combine(directory,"render-results.json"),new{passed=true,simulated=true,files=SupportedKinds.Select(i=>names[i]).ToArray(),width=1200,height=800,captureStarted=false});
  }
  static int LifecycleTests(string output,bool native=false){
   // This mode captures actual fixture devices. It is never part of host build
   // self-tests and requires an explicit opt-in inside a Hyper-V guest.
   Check(Administrator,"Lifecycle tests require an administrator account in the VM.");
   Check(Environment.GetEnvironmentVariable("INTERCEPTOR_VIEWER_LAB")=="1","Set INTERCEPTOR_VIEWER_LAB=1 only inside the dedicated VM.");
   using(var bios=Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\BIOS"))
    Check(bios!=null&&string.Equals(Convert.ToString(bios.GetValue("SystemProductName")),"Virtual Machine",StringComparison.OrdinalIgnoreCase),"Lifecycle tests are restricted to the Hyper-V VM.");
   var application=new Application{ShutdownMode=ShutdownMode.OnExplicitShutdown};var phases=new List<string>();int code=1;
   application.Dispatcher.BeginInvoke(new Action(async()=>{
    try{await Dashboard.TestLifecycle(phases,native);code=0;Json(output,new{passed=true,phases=phases,captureStarted=true,installedDriver=false,rebootRequested=false,nativeFamilies=native,scope="Real WPF Dispatcher, tab selection and native bridge; fixture capture, tracked stop, owner reacquisition and close during pending transition. Native mode additionally requires input and forwarding in HID, XUSB, XInputHID and GIP tabs. Read failure is deliberately injected at the managed error handler."});}
    catch(Exception error){try{Json(output,new{passed=false,phases=phases,error=error.ToString()});}catch{}}
    finally{application.Shutdown(code);}
   }));
   return application.Run();
  }
 }
 public sealed class Dashboard : IDisposable {
  public readonly Window Window;public readonly Grid Root;
  readonly TabControl tabs=new TabControl(),controllerTabs=new TabControl();readonly ComboBox devices=new ComboBox(),layout=new ComboBox();
  readonly Button start=new Button(),stop=new Button(),copyDiagnostic=new Button();readonly TextBlock state=new TextBlock(),detail=new TextBlock(),count=new TextBlock(),deviceDetail=new TextBlock(),notice=new TextBlock();
  readonly InputDrawing drawing=new InputDrawing();readonly FrameRefresh refresh;readonly bool preview,suppressRestartPrompt;
  Bridge bridge;RegisteredWaitHandle registration;DeviceState[] snapshotDevices=new DeviceState[0];Snapshot header;string deviceSignature="";bool changing,busy,active,closeAllowed,closingRequested;volatile bool closed;bool disposed;
  Task lifecycle=Task.FromResult(0);bool restartPromptShown;string diagnosticText="";
  sealed class Choice {public uint Type,Id,Token;public string Label;public override string ToString(){return Label;}}
  public Dashboard(bool preview,bool suppressRestartPrompt=false){
   this.preview=preview;this.suppressRestartPrompt=suppressRestartPrompt;Window=new Window{Title="Interceptor · Input viewer",Width=1220,Height=850,MinWidth=900,MinHeight=690,Background=Palette.Soft,FontFamily=new FontFamily("Segoe UI"),WindowStartupLocation=WindowStartupLocation.CenterScreen};
   refresh=new FrameRefresh(Window.Dispatcher,ReadSnapshot);
   Window.Resources.Add(typeof(Button),ButtonStyle());Window.Resources.Add(typeof(TabItem),TabStyle());
   Root=new Grid{Background=Palette.Soft,Margin=new Thickness(0)};Root.RowDefinitions.Add(new RowDefinition{Height=GridLength.Auto});Root.RowDefinitions.Add(new RowDefinition{Height=GridLength.Auto});Root.RowDefinitions.Add(new RowDefinition{Height=new GridLength(1,GridUnitType.Star)});Root.RowDefinitions.Add(new RowDefinition{Height=GridLength.Auto});Window.Content=Root;
   var heading=new Grid{Margin=new Thickness(28,24,28,17)};heading.ColumnDefinitions.Add(new ColumnDefinition{Width=GridLength.Auto});heading.ColumnDefinitions.Add(new ColumnDefinition());heading.ColumnDefinitions.Add(new ColumnDefinition{Width=GridLength.Auto});
   var logo=new Border{Width=46,Height=46,CornerRadius=new CornerRadius(14),Background=Palette.Blue,Child=new TextBlock{Text="I",Foreground=Palette.Yellow,FontSize=32,FontWeight=FontWeights.Bold,HorizontalAlignment=HorizontalAlignment.Center,VerticalAlignment=VerticalAlignment.Center}};heading.Children.Add(logo);
   var titles=new StackPanel{Margin=new Thickness(15,0,0,0)};titles.Children.Add(Label("Interceptor",27,Palette.Ink,FontWeights.SemiBold));titles.Children.Add(Label("Input viewer · public API example",13,Palette.Muted));Grid.SetColumn(titles,1);heading.Children.Add(titles);
   var badge=new Border{CornerRadius=new CornerRadius(8),Background=Palette.Yellow,Padding=new Thickness(13,9,13,9),VerticalAlignment=VerticalAlignment.Center,Child=Label(preview?"SIMULATED PREVIEW":"INPUT FORWARDED",11,Palette.Ink,FontWeights.SemiBold)};Grid.SetColumn(badge,2);heading.Children.Add(badge);Root.Children.Add(heading);
   var top=new Grid{Margin=new Thickness(28,0,28,16)};top.ColumnDefinitions.Add(new ColumnDefinition());top.ColumnDefinitions.Add(new ColumnDefinition{Width=GridLength.Auto});top.ColumnDefinitions.Add(new ColumnDefinition{Width=GridLength.Auto});
   var status=new StackPanel();state.Text="Ready to observe";state.FontSize=18;state.FontWeight=FontWeights.SemiBold;state.Foreground=Palette.Ink;status.Children.Add(state);detail.Text="Choose a tab, then start observing.";detail.Foreground=Palette.Muted;detail.Margin=new Thickness(0,5,12,0);detail.TextWrapping=TextWrapping.Wrap;status.Children.Add(detail);top.Children.Add(status);
   start.Content="▶  Observe";start.Background=Palette.Blue;start.Foreground=Palette.Paper;start.MinWidth=132;start.VerticalAlignment=VerticalAlignment.Top;start.Margin=new Thickness(12,2,8,2);start.Click+=async(s,e)=>await Start();Grid.SetColumn(start,1);top.Children.Add(start);
   stop.Content="■  Stop";stop.MinWidth=112;stop.VerticalAlignment=VerticalAlignment.Top;stop.IsEnabled=false;stop.Margin=new Thickness(0,2,0,2);stop.Click+=async(s,e)=>await Stop();Grid.SetColumn(stop,2);top.Children.Add(stop);Grid.SetRow(top,1);Root.Children.Add(top);
   var card=new Border{Margin=new Thickness(28,0,28,0),Padding=new Thickness(22,18,22,16),Background=Palette.Paper,CornerRadius=new CornerRadius(18),BorderBrush=Palette.Line,BorderThickness=new Thickness(1)};Grid.SetRow(card,2);Root.Children.Add(card);
   var body=new Grid();for(int i=0;i<3;i++)body.RowDefinitions.Add(new RowDefinition{Height=GridLength.Auto});body.RowDefinitions.Add(new RowDefinition{Height=new GridLength(1,GridUnitType.Star)});body.RowDefinitions.Add(new RowDefinition{Height=GridLength.Auto});card.Child=body;
   tabs.Items.Add(new TabItem{Header="Keyboard"});tabs.Items.Add(new TabItem{Header="Mouse"});tabs.Items.Add(new TabItem{Header="Controllers"});tabs.SelectedIndex=0;tabs.BorderThickness=new Thickness(0);tabs.Background=Palette.Paper;tabs.Margin=new Thickness(0,0,0,12);body.Children.Add(tabs);
   string[] types={"HID","XInput / XUSB","XInputHID","GIP"};foreach(string type in types)controllerTabs.Items.Add(new TabItem{Header=type});controllerTabs.SelectedIndex=0;controllerTabs.BorderThickness=new Thickness(0);controllerTabs.Background=Palette.Paper;controllerTabs.Margin=new Thickness(0,0,0,10);controllerTabs.Visibility=Visibility.Visible;Grid.SetRow(controllerTabs,1);body.Children.Add(controllerTabs);
   var chooser=new Grid{Margin=new Thickness(0,0,0,5)};chooser.ColumnDefinitions.Add(new ColumnDefinition());chooser.ColumnDefinitions.Add(new ColumnDefinition{Width=new GridLength(170)});devices.MinHeight=34;devices.FontSize=13;devices.VerticalContentAlignment=VerticalAlignment.Center;devices.SelectionChanged+=(s,e)=>{if(!changing)Paint();};chooser.Children.Add(devices);
   layout.Items.Add("AZERTY · ISO");layout.Items.Add("QWERTY · ISO");layout.SelectedIndex=0;layout.Margin=new Thickness(12,0,0,0);layout.SelectionChanged+=(s,e)=>{drawing.Azerty=layout.SelectedIndex==0;drawing.InvalidateVisual();};Grid.SetColumn(layout,1);chooser.Children.Add(layout);Grid.SetRow(chooser,2);body.Children.Add(chooser);
   drawing.MinHeight=300;Grid.SetRow(drawing,3);body.Children.Add(drawing);
   var info=new Grid{Margin=new Thickness(0,8,0,0)};info.ColumnDefinitions.Add(new ColumnDefinition());info.ColumnDefinitions.Add(new ColumnDefinition{Width=GridLength.Auto});deviceDetail.TextWrapping=TextWrapping.Wrap;deviceDetail.Foreground=Palette.Muted;deviceDetail.FontSize=12;deviceDetail.Margin=new Thickness(0,0,14,0);info.Children.Add(deviceDetail);count.Text="0 inputs";count.FontWeight=FontWeights.SemiBold;count.FontSize=14;count.Foreground=Palette.Blue;Grid.SetColumn(count,1);info.Children.Add(count);Grid.SetRow(info,4);body.Children.Add(info);
   notice.Text="No remapping. Inputs are forwarded unchanged. Stop or close this application to release capture.";notice.Foreground=Palette.Muted;notice.FontSize=12;notice.TextWrapping=TextWrapping.Wrap;
   var footer=new Grid{Margin=new Thickness(30,14,30,19)};footer.ColumnDefinitions.Add(new ColumnDefinition());footer.ColumnDefinitions.Add(new ColumnDefinition{Width=GridLength.Auto});footer.Children.Add(notice);
   copyDiagnostic.Content="Copy diagnostic";copyDiagnostic.MinWidth=145;copyDiagnostic.Margin=new Thickness(16,0,0,0);copyDiagnostic.Visibility=Visibility.Collapsed;copyDiagnostic.VerticalAlignment=VerticalAlignment.Center;
   copyDiagnostic.Click+=(s,e)=>{try{Clipboard.SetText(diagnosticText);copyDiagnostic.Content="Diagnostic copied";}catch(Exception error){copyDiagnostic.Content="Copy diagnostic";detail.Text="Could not copy the diagnostic: "+error.Message;}};
   Grid.SetColumn(copyDiagnostic,1);footer.Children.Add(copyDiagnostic);Grid.SetRow(footer,3);Root.Children.Add(footer);
   tabs.SelectionChanged+=async(s,e)=>{if(e.Source==tabs&&!changing)await ChangeTab();};controllerTabs.SelectionChanged+=async(s,e)=>{if(e.Source==controllerTabs&&!changing)await ChangeTab();};
   Window.Closing+=async(s,e)=>{
    if(closeAllowed||disposed)return;e.Cancel=true;if(closingRequested)return;
    closingRequested=true;UpdateControls();
    try{await lifecycle;if(!closed)await Stop();closeAllowed=true;if(!closed)Window.Close();}
    catch(Exception error){closingRequested=false;if(!closed){state.Text="Close interrupted";detail.Text=error.Message;UpdateControls();}}
   };
   Window.Closed+=(s,e)=>{closed=true;refresh.Dispose();};SetView();
  }
  static TextBlock Label(string text,double size,Brush brush,FontWeight? weight=null){return new TextBlock{Text=text,FontSize=size,Foreground=brush,FontWeight=weight??FontWeights.Normal};}
  static Style ButtonStyle(){return (Style)XamlReader.Parse("<Style xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' TargetType='{x:Type Button}' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'><Setter Property='Background' Value='#EFF4FA'/><Setter Property='Foreground' Value='#152744'/><Setter Property='Padding' Value='17,11'/><Setter Property='FontSize' Value='14'/><Setter Property='FontWeight' Value='SemiBold'/><Setter Property='Cursor' Value='Hand'/><Setter Property='Template'><Setter.Value><ControlTemplate TargetType='{x:Type Button}'><Border Background='{TemplateBinding Background}' CornerRadius='9' Padding='{TemplateBinding Padding}'><ContentPresenter HorizontalAlignment='Center' VerticalAlignment='Center'/></Border><ControlTemplate.Triggers><Trigger Property='IsMouseOver' Value='True'><Setter Property='Opacity' Value='0.85'/></Trigger><Trigger Property='IsPressed' Value='True'><Setter Property='Opacity' Value='0.65'/></Trigger><Trigger Property='IsEnabled' Value='False'><Setter Property='Opacity' Value='0.4'/></Trigger></ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter></Style>");}
  static Style TabStyle(){return (Style)XamlReader.Parse("<Style xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' TargetType='{x:Type TabItem}' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'><Setter Property='Foreground' Value='#61738D'/><Setter Property='FontSize' Value='14'/><Setter Property='FontWeight' Value='SemiBold'/><Setter Property='Background' Value='White'/><Setter Property='Padding' Value='21,10'/><Setter Property='Cursor' Value='Hand'/><Setter Property='Template'><Setter.Value><ControlTemplate TargetType='{x:Type TabItem}'><Border Background='{TemplateBinding Background}' CornerRadius='8' Margin='0,0,7,0' Padding='{TemplateBinding Padding}'><ContentPresenter ContentSource='Header' HorizontalAlignment='Center'/></Border></ControlTemplate></Setter.Value></Setter><Style.Triggers><Trigger Property='IsSelected' Value='True'><Setter Property='Background' Value='#1464E8'/><Setter Property='Foreground' Value='White'/></Trigger></Style.Triggers></Style>");}
  int Kind{get{return tabs.SelectedIndex<2?tabs.SelectedIndex:2+controllerTabs.SelectedIndex;}}
  void SelectKind(int kind){changing=true;tabs.SelectedIndex=kind<2?kind:2;if(kind>=2)controllerTabs.SelectedIndex=kind-2;changing=false;SetView();}
  void SetView(){drawing.Kind=Kind<2?Kind:2;drawing.NativeLayout=Kind==3||Kind==5;controllerTabs.Visibility=Kind>=2?Visibility.Visible:Visibility.Collapsed;layout.Visibility=Kind==0?Visibility.Visible:Visibility.Collapsed;ResetChoices();Paint();}
  Task ChangeTab(){
   if(busy||closed||closingRequested)return lifecycle;bool resume=active;
   return BeginLifecycle(async()=>{await StopBridge();if(closed||closingRequested)return;ClearSnapshot();SetView();if(resume)StartBridge();},"Tab change interrupted",true);
  }
  void UpdateControls(){bool enabled=!busy&&!closingRequested&&!closed;tabs.IsEnabled=controllerTabs.IsEnabled=enabled;start.IsEnabled=enabled&&!active&&!preview;stop.IsEnabled=enabled&&active&&!preview;}
  void Busy(bool value){busy=value;if(!closed)UpdateControls();}
  void ClearSnapshot(){SetDiagnostic("");active=false;snapshotDevices=new DeviceState[0];header=new Snapshot();deviceSignature="";ResetChoices();Paint();}
  Task StopBridge(){var target=bridge;return target==null?Task.FromResult(0):Task.Run(()=>target.Stop());}
  // All transitions are tracked by this one Task. Snapshot status never clears
  // busy, and closing waits for the transition before requesting the final stop.
  Task BeginLifecycle(Func<Task> operation,string failureTitle,bool readAfter){
   if(busy||closed)return lifecycle;Busy(true);lifecycle=RunLifecycle(operation,failureTitle,readAfter);return lifecycle;
  }
  async Task RunLifecycle(Func<Task> operation,string failureTitle,bool readAfter){
   // Publish lifecycle before running any operation that could fail synchronously.
   await Task.Yield();if(closed){busy=false;return;}bool success=false;
   try{
    Exception failure=null;try{await operation();success=true;}catch(Exception error){failure=error;}
    if(failure!=null){
     string message=failure.Message;var win32=failure as Win32Exception;if(win32!=null)message="Windows error "+win32.NativeErrorCode+" (0x"+unchecked((uint)win32.NativeErrorCode).ToString("X8",CultureInfo.InvariantCulture)+"): "+message;
     try{await StopBridge();}catch(Exception stopError){message+="\nStop: "+stopError.Message;}
     if(!closed){ClearSnapshot();state.Text=failureTitle;detail.Text=message;SetDiagnostic(failureTitle+" for "+FamilyName((uint)(Kind+1))+"\n"+failure+"\n"+message);}
    }
   }
   finally{Busy(false);}
   if(success&&readAfter&&!closed&&!closingRequested)ReadSnapshot();
  }
  void EnsureBridge(){if(bridge!=null)return;var created=new Bridge();try{registration=ThreadPool.RegisterWaitForSingleObject(created.Changed,(s,t)=>{
    if(!closed)refresh.Request();
   },null,Timeout.Infinite,false);
    bridge=created;
   }catch{created.Dispose();throw;}
  }
  void StartBridge(){
   EnsureBridge();ClearSnapshot();restartPromptShown=false;byte mask=(byte)(Kind==0?1:Kind==1?2:0),controllers=(byte)(Kind>=2?1<<(Kind-2):0);
   uint error=bridge.Start(mask,controllers);if(error!=0)throw new Win32Exception((int)error);active=true;
  }
  Task Start(){
   if(busy||preview||closed||closingRequested)return lifecycle;
   if(!App.Administrator){try{Process.Start(new ProcessStartInfo(Assembly.GetExecutingAssembly().Location,"--observe "+Kind){UseShellExecute=true,Verb="runas"});closeAllowed=true;Window.Close();}catch(Exception e){var win32=e as Win32Exception;detail.Text=win32!=null&&win32.NativeErrorCode==1223?"Permission cancelled. No capture started.":e.Message;}return Task.FromResult(0);}
   return BeginLifecycle(async()=>{if(closed||closingRequested)return;state.Text="Initializing devices…";detail.Text="Associating through the public API, then waiting for input.";EnsureBridge();await StopBridge();if(!closed&&!closingRequested)StartBridge();},"Observation unavailable",true);
  }
  Task Stop(){
   if(busy||closed)return lifecycle;
   return BeginLifecycle(async()=>{state.Text="Stopping observation…";await StopBridge();if(closed)return;ClearSnapshot();state.Text="Observation stopped";detail.Text="Devices work normally. You can start observing again.";},"Stop interrupted",false);
  }
  internal static bool TerminalStatus(uint status){return status!=0&&status!=1&&status!=2&&status!=8;}
  internal static string FamilyName(uint type){switch(type){case 3:return "HID";case 4:return "XInput / XUSB";case 5:return "XInputHID";case 6:return "GIP";default:return "type "+type;}}
  internal static string StageName(uint stage){switch(stage){case 0:return "not recorded";case 1:return "Init_AllDevices";case 2:return "Get_Capabilities";case 3:return "Create_Context";case 4:return "Get_Devices";case 5:return "Wait_For_Input";case 6:return "Get_Input (receive)";case 7:return "Send_Input (forward unchanged)";case 8:return "display decoding";default:return "stage "+stage;}}
  internal static string ResetDescription(uint reason){switch(reason){case 0:return "not reported";case 1:return "emergency passthrough shortcut";case 2:return "input queue overflow; capture released";case 3:return "capture/context closed";case 4:return "device removed or device topology changed";case 5:return "two-hour demo expired";default:return "unknown reset reason";}}
  internal static string StopDiagnostic(Snapshot snapshot,Diagnostic diagnostic,int kind,DeviceState[] states,bool concise){
   uint error=diagnostic.Error!=0?diagnostic.Error:snapshot.Error;
   uint reason=diagnostic.ResetReason!=0?diagnostic.ResetReason:snapshot.ResetReason;
   uint type=diagnostic.Type!=0?diagnostic.Type:(uint)(kind+1);
   string device=diagnostic.Device!=0?"#"+diagnostic.Device:"not recorded";
   string sequence=diagnostic.Sequence!=0?diagnostic.Sequence.ToString(CultureInfo.InvariantCulture):"not recorded";
   ulong received=diagnostic.Received!=0?diagnostic.Received:snapshot.Received,forwarded=diagnostic.Forwarded!=0?diagnostic.Forwarded:snapshot.Forwarded;
   var lines=new List<string>();
   lines.Add("Stage: "+StageName(diagnostic.Stage)+". "+(diagnostic.Type!=0?"Family: ":"Selected family: ")+FamilyName(type)+". Device: "+device+". Sequence: "+sequence+".");
   lines.Add(error!=0?"Windows error "+error+" (0x"+error.ToString("X8",CultureInfo.InvariantCulture)+"): "+new Win32Exception((int)error).Message:"Windows error: no error code reported.");
   if(diagnostic.Stage==7)lines.Add("Batch: "+diagnostic.Requested+" received, "+diagnostic.Accepted+" accepted, "+(diagnostic.Requested>diagnostic.Accepted?diagnostic.Requested-diagnostic.Accepted:0)+" not forwarded.");
   else if(diagnostic.Stage==6)lines.Add("Read capacity: "+diagnostic.Requested+" inputs. The read failed before a complete batch was returned.");
   lines.Add("Last driver reset "+reason+": "+ResetDescription(reason)+". Total: "+received+" received / "+forwarded+" forwarded.");
   string message=diagnostic.Message??"";
   if(message.Length!=0){if(concise&&message.Length>180)message=message.Substring(0,180)+"... (copy diagnostic for the full message)";lines.Add("Cause: "+message);}
   if(!concise){
    var source=(states??new DeviceState[0]).FirstOrDefault(x=>x.Info.Type==type&&x.Info.Id==diagnostic.Device);
    if(source.Info.Id!=0)lines.Add("Last source: "+(source.Info.Name??"")+"; VID:PID "+source.Info.Vid.ToString("X4")+":"+source.Info.Pid.ToString("X4")+"; token "+source.Info.Token+".");
    lines.Add("Snapshot status "+snapshot.Status+"; generation "+(diagnostic.Generation!=0?diagnostic.Generation:snapshot.Generation)+"; ready mask 0x"+diagnostic.ReadyMask.ToString("X8",CultureInfo.InvariantCulture)+".");
    lines.Add(TerminalStatus(snapshot.Status)?"This identifies the failed client operation; it does not by itself establish a driver defect. The last driver reset reason may refer to an earlier context, and a wait failure does not identify which physical device caused it.":"Observation remains active. Inputs pass through until a source is ready; use Stop to release the context.");
   }
   return string.Join("\n",lines.ToArray());
  }
  void SetDiagnostic(string text){diagnosticText=string.IsNullOrEmpty(text)?"":"Interceptor ExampleUI "+Assembly.GetExecutingAssembly().GetName().Version+"\n"+text;copyDiagnostic.Content="Copy diagnostic";copyDiagnostic.Visibility=diagnosticText.Length==0?Visibility.Collapsed:Visibility.Visible;detail.ToolTip=diagnosticText.Length==0?null:diagnosticText;}
  void PresentSnapshot(Snapshot value,DeviceState[] values,Diagnostic diagnostic){
   header=value;snapshotDevices=values??new DeviceState[0];string signature=header.Generation+":"+string.Join(",",snapshotDevices.Select(x=>x.Info.Type+"/"+x.Info.Id+"/"+x.Info.Token).ToArray());if(signature!=deviceSignature){deviceSignature=signature;UpdateChoices();}
   active=header.Status==1||header.Status==2||header.Status==8;UpdateControls();
   switch(header.Status){
    case 0:state.Text="Observation stopped";detail.Text="Devices work normally.";break;
    case 1:state.Text="Initializing devices...";detail.Text="Windows is preparing associations. Input will be forwarded.";break;
    case 2:state.Text="Live observation";detail.Text="Move a controller stick or press a controller button.";break;
    case 3:state.Text="No source registered for this family";detail.Text=Kind==4?"This tab covers direct XInputHID collections. For a controller with a GIP or XUSB parent, select the parent tab.":"No Interceptor reader was found for the selected family. Check the connection and installer associations.";break;
    case 4:state.Text="Restart required";detail.Text="New devices were associated. Save your work and restart Windows before trying again.";break;
    case 5:state.Text="Capture operation not supported";detail.Text="The capture operation was rejected. The stage and error below identify where it stopped.";break;
    case 7:state.Text="Observation stopped automatically";detail.Text="The capture was released; this viewer is no longer intercepting inputs.";break;
    case 8:state.Text="Waiting for a compatible source";detail.Text="Waiting for the selected family to become capturable. Inputs pass through until ready. Observe remains active; Stop releases it.";break;
    case 9:state.Text="Capture operation timed out";detail.Text="The operation timed out before capture became ready. The exact stage and error are shown below.";break;
    case 10:state.Text="Two-hour demo expired";detail.Text="All captures have been released. Restart Windows to begin another two-hour session.";break;
    default:state.Text="Observation stopped automatically";detail.Text="The capture operation failed. See its exact stage and error below.";break;
   }
   if(TerminalStatus(header.Status)){
    if(header.Status!=10){if(diagnostic.Stage==7)state.Text="Observation stopped: forwarding failed";else if(diagnostic.Stage==6)state.Text="Observation stopped: input read failed";else if(diagnostic.Stage==5)state.Text="Observation stopped: input wait failed";}
    SetDiagnostic(StopDiagnostic(header,diagnostic,Kind,snapshotDevices,false));detail.Text+="\n"+StopDiagnostic(header,diagnostic,Kind,snapshotDevices,true);
   }else if(header.Status==8&&diagnostic.Stage!=0){
    SetDiagnostic("Capture preparation (still active)\n"+StopDiagnostic(header,diagnostic,Kind,snapshotDevices,false));
   }else if(active)SetDiagnostic("");
   if(!active){snapshotDevices=new DeviceState[0];ResetChoices();}Paint();
  }
  void ReadSnapshot(){
   if(bridge==null||closed||busy||closingRequested)return;
   try{
    Snapshot value;var values=bridge.Read(out value);var diagnostic=TerminalStatus(value.Status)||value.Status==8?bridge.ReadDiagnostic():new Diagnostic();
    PresentSnapshot(value,values,diagnostic);
    if(header.Status==4&&!restartPromptShown&&!preview&&!suppressRestartPrompt&&Window.IsVisible){restartPromptShown=true;PromptRestart();}
   }catch(Exception e){ReadFailure(e);}
  }
  void ReadFailure(Exception error){
   var win32=error as Win32Exception;string message="Reading the viewer snapshot failed for "+FamilyName((uint)(Kind+1))+". "+error.Message;
   if(win32!=null)message+=" (Windows error "+win32.NativeErrorCode+").";
   string full=message+"\n"+error.GetType().FullName+"\nLast totals: "+header.Received+" received / "+header.Forwarded+" forwarded.";
   BeginLifecycle(async()=>{await StopBridge();if(closed)return;ClearSnapshot();state.Text="Read interrupted";detail.Text=message;SetDiagnostic(full);},"Stop after error interrupted",false);
  }
  void PromptRestart(){
   if(closed||closingRequested||suppressRestartPrompt)return;
   var dialog=new Window{Title="Interceptor - Restart required",Owner=Window,WindowStartupLocation=WindowStartupLocation.CenterOwner,SizeToContent=SizeToContent.Height,Width=505,ResizeMode=ResizeMode.NoResize,ShowInTaskbar=false,Background=Palette.Paper,FontFamily=Window.FontFamily};
   dialog.Resources.Add(typeof(Button),ButtonStyle());var content=new StackPanel{Margin=new Thickness(25)};dialog.Content=content;
   content.Children.Add(Label("New devices detected",22,Palette.Ink,FontWeights.SemiBold));
   var message=Label("New controller devices have been detected. Restart Windows to use them with Interceptor.\n\nSave your work before restarting.",14,Palette.Muted);message.TextWrapping=TextWrapping.Wrap;message.Margin=new Thickness(0,13,0,22);content.Children.Add(message);
   var buttons=new StackPanel{Orientation=Orientation.Horizontal,HorizontalAlignment=HorizontalAlignment.Right};content.Children.Add(buttons);
   var now=new Button{Content="Restart now",Background=Palette.Blue,Foreground=Palette.Paper,Margin=new Thickness(0,0,9,0)};
   var later=new Button{Content="Restart later",IsDefault=true,IsCancel=true};buttons.Children.Add(now);buttons.Children.Add(later);
   bool requesting=false;dialog.Closing+=(s,e)=>{if(requesting)e.Cancel=true;};
   now.Click+=async(s,e)=>{
    if(requesting||suppressRestartPrompt)return;requesting=true;now.IsEnabled=later.IsEnabled=false;
    try{
     // Only an explicit click reaches this command. /t 0 and no /f preserve
     // Windows' application shutdown blocking; no forced loss of unsaved work.
     int exit=await Task.Run(()=>{using(var process=Process.Start(new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),"shutdown.exe"),"/r /t 0 /d p:2:4"){UseShellExecute=false,CreateNoWindow=true,WindowStyle=ProcessWindowStyle.Hidden})){if(process==null)throw new InvalidOperationException("Windows did not start the restart request.");if(!process.WaitForExit(10000))throw new TimeoutException("Windows has not confirmed the restart request within 10 seconds.");return process.ExitCode;}});
     if(exit!=0)throw new Win32Exception(exit);requesting=false;dialog.DialogResult=true;
    }catch(Exception error){requesting=false;now.IsEnabled=later.IsEnabled=true;message.Text="Windows could not restart automatically. Please restart your computer manually.\n\n"+error.Message;}
   };
   dialog.ShowDialog();
  }
  void ResetChoices(){changing=true;devices.Items.Clear();devices.Items.Add(new Choice{Label=Kind==0?"All keyboards":Kind==1?"All mouse devices":"Last active controller"});devices.SelectedIndex=0;changing=false;}
  void UpdateChoices(){var old=devices.SelectedItem as Choice;ResetChoices();changing=true;foreach(var item in snapshotDevices){string name=string.IsNullOrWhiteSpace(item.Info.Name)?"Device":item.Info.Name;var choice=new Choice{Type=item.Info.Type,Id=item.Info.Id,Token=item.Info.Token,Label="#"+item.Info.Id+" · "+name+" · "+item.Info.Vid.ToString("X4")+":"+item.Info.Pid.ToString("X4")};devices.Items.Add(choice);if(old!=null&&old.Id==choice.Id&&old.Type==choice.Type&&old.Token==choice.Token)devices.SelectedItem=choice;}changing=false;}
  public static DeviceState Aggregate(DeviceState[] states,int kind){
   var result=DeviceState.Empty();if(states.Length==0)return result;
   var latest=states.OrderByDescending(x=>x.Sequence).First();if(kind>=2)return latest;
   result=latest;result.Keys=new byte[96];result.MouseButtons=0;
   foreach(var item in states){if(item.Keys!=null)for(int i=0;i<96;i++)result.Keys[i]|=item.Keys[i];result.MouseButtons|=item.MouseButtons;}return result;
  }
  void Paint(){
   var selected=devices.SelectedItem as Choice;var available=snapshotDevices;
   if(selected!=null&&selected.Id!=0)available=snapshotDevices.Where(x=>x.Info.Id==selected.Id&&x.Info.Type==selected.Type&&x.Info.Token==selected.Token).ToArray();
   var value=Aggregate(available,Kind);drawing.Update(value);count.Text=header.Received.ToString("N0",CultureInfo.CurrentCulture)+" inputs · "+header.Forwarded.ToString("N0",CultureInfo.CurrentCulture)+" forwarded";
   deviceDetail.Text=available.Length==0?"No state received yet.":(selected!=null&&selected.Id!=0?"ID "+value.Info.Id:"Devices: "+available.Length)+" · last ID "+value.Info.Id+" · VID "+value.Info.Vid.ToString("X4")+" / PID "+value.Info.Pid.ToString("X4")+" · sequence "+value.Sequence;
   if((value.Flags&6)!=0)deviceDetail.Text+="\nThe report is forwarded, but its format cannot be fully decoded ("+value.DecodeError+").";else if((value.Flags&64)!=0)deviceDetail.Text+="\nPartial display: some HID fields do not map to standard controls.";
   notice.Text=Kind>=3?"Native transport: profile verified by the loaded driver. Unrecognized devices or extensions remain unsupported.":Kind==2?"HID: numbered buttons and axes declared by the device. Physical layouts may vary.":"No remapping. Inputs are forwarded unchanged. Stop or close this application to release capture.";
  }
  public void ObserveOnLoad(int kind){SelectKind(kind);Window.Loaded+=async(s,e)=>await Start();}
  public void SetPreview(int kind){
   SelectKind(kind);var sample=DeviceState.Empty();sample.Info=new DeviceInfo{Type=(uint)(kind+1),Id=1,Vid=0x1209,Pid=0xCA53,Name="Simulated preview source"};sample.Sequence=1324;sample.InputCount=1324;sample.Flags=1;sample.Keys[0x23/8]|=(byte)(1<<(0x23%8));sample.Keys[0x24/8]|=(byte)(1<<(0x24%8));sample.MouseButtons=9;sample.MouseX=17;sample.MouseY=-23;sample.Wheel=120;sample.Buttons=9;sample.AxisMask=0x1B;sample.Axes[0]=-.55f;sample.Axes[1]=-.35f;sample.Axes[3]=.6f;sample.Axes[4]=.15f;sample.RawAxes[0]=-18022;sample.RawAxes[1]=-11468;sample.RawAxes[3]=19660;sample.RawAxes[4]=4915;sample.Hat=1;sample.Flags|=8;
   if(kind==3||kind==5){sample.Flags|=32|16;sample.Buttons=kind==5?(1UL<<4)|(1UL<<6)|(1UL<<8):(1UL<<12)|(1UL<<14)|1;sample.LeftTrigger=.35f;sample.RightTrigger=.7f;}
   snapshotDevices=new[]{sample};header=new Snapshot{Received=1324,Forwarded=1324,DeviceCount=1};UpdateChoices();state.Text="Simulated preview · no device captured";detail.Text="Fixed sample data used only to verify UI rendering.";start.IsEnabled=stop.IsEnabled=false;Paint();
  }
  public void LayoutForRender(){Root.Measure(new Size(1200,800));Root.Arrange(new Rect(0,0,1200,800));Root.UpdateLayout();}
  static void TestCheck(bool condition,string description){if(!condition)throw new InvalidDataException(description);}
  internal void TestDiagnosticPresentation(){
   var failedRead=StopDiagnostic(new Snapshot{Status=6,Error=13},new Diagnostic{Stage=6,Type=4,Requested=128,Error=13},3,new DeviceState[0],false);
   TestCheck(failedRead.Contains("Read capacity: 128")&&!failedRead.Contains("not forwarded"),"Read buffer capacity must never be presented as a received or lost input batch");
   SelectKind(3);var input=DeviceState.Empty();input.Info=new DeviceInfo{Type=4,Id=2,Token=7,Vid=0x1209,Pid=0xCA54,Name="Offline diagnostic fixture"};input.Sequence=982;input.InputCount=3;
   var diagnostic=new Diagnostic{Size=1096,Version=1,Stage=7,Type=4,Device=2,Requested=3,Accepted=1,Error=50,Sequence=982,Generation=44,Received=3,Forwarded=1,ReadyMask=16,Message="Client forwarding failure; no driver-cause assumption. "+new string('x',220)};
   foreach(uint status in new uint[]{3,4,5,6,7,9,10,99}){
    PresentSnapshot(new Snapshot{Status=status,Error=50,Generation=44,Received=3,Forwarded=1},new[]{input},diagnostic);
    TestCheck(!active&&snapshotDevices.Length==0&&!stop.IsEnabled,"Terminal status must release the visual capture state: "+status);
    TestCheck(detail.Text.Contains("Send_Input")&&detail.Text.Contains("XInput / XUSB")&&detail.Text.Contains("#2")&&detail.Text.Contains("982")&&detail.Text.Contains("50 (0x00000032)"),"Automatic stop must identify the exact operation, family, device, sequence and Windows error: "+status);
    TestCheck(detail.Text.Contains("Batch: 3 received, 1 accepted, 2 not forwarded.")&&copyDiagnostic.Visibility==Visibility.Visible,"Partial forwarding and copy control must remain visible: "+status);
    TestCheck(diagnosticText.Contains(diagnostic.Message)&&diagnosticText.Contains("1209:CA54")&&diagnosticText.Contains("token 7"),"Copy diagnostic must retain the full message and last source identity: "+status);
   }
   for(uint reason=0;reason<=6;reason++){
    var value=diagnostic;value.Stage=5;value.Error=995;value.ResetReason=reason;value.Requested=value.Accepted=0;
    PresentSnapshot(new Snapshot{Status=7,Error=995,ResetReason=reason},new[]{input},value);
    TestCheck(detail.Text.Contains("Wait_For_Input")&&detail.Text.Contains("Last driver reset "+reason+": "+ResetDescription(reason)),"Context reset reason must be explicit: "+reason);
   }
   var noError=StopDiagnostic(new Snapshot{Status=7},new Diagnostic(),3,new DeviceState[0],false);
   TestCheck(noError.Contains("no error code reported")&&!noError.Contains("The operation completed successfully"),"Missing error codes must not be shown as a successful operation");
   PresentSnapshot(new Snapshot{Status=8},new DeviceState[0],new Diagnostic{Stage=4,Message="Selected source still preparing"});
   TestCheck(active&&copyDiagnostic.Visibility==Visibility.Visible&&!detail.Text.Contains("30 seconds")&&diagnosticText.Contains("still active"),"Preparing capture stays active without an invented thirty-second deadline");
   PresentSnapshot(new Snapshot{Status=2},new[]{input},new Diagnostic());
   TestCheck(active&&snapshotDevices.Length==1&&copyDiagnostic.Visibility==Visibility.Collapsed,"A controller input must keep live observation active and clear stale preparation diagnostics");
   ClearSnapshot();TestCheck(copyDiagnostic.Visibility==Visibility.Collapsed&&diagnosticText.Length==0,"Starting a fresh capture clears stale error diagnostics");
  }
  async Task TestRunning(){
   var deadline=Stopwatch.StartNew();
   while((header.Status==1||header.Status==8)&&deadline.ElapsedMilliseconds<35000){await Task.Delay(10);ReadSnapshot();}
   TestCheck(header.Status==2&&snapshotDevices.Length>0,"Live capture unavailable: status="+header.Status+", error="+header.Error+". Check fixtures and associations; no restart should be pending.");
  }
  void TestStopped(){Snapshot value;var states=bridge.Read(out value);TestCheck(value.Status==0&&states.Length==0,"Native worker was not stopped and joined.");}
  async Task TestClosed(){
   var deadline=Stopwatch.StartNew();while(!closed&&deadline.ElapsedMilliseconds<30000)await Task.Delay(10);
   TestCheck(closed,"Close request during a transition was lost.");await lifecycle;if(bridge!=null)TestStopped();
  }
  async Task TestControllerInput(int kind){
   var deadline=Stopwatch.StartNew();
   while(header.Received==0&&deadline.ElapsedMilliseconds<10000){await Task.Delay(10);ReadSnapshot();}
   TestCheck(snapshotDevices.Length>0&&snapshotDevices.All(x=>x.Info.Type==(uint)(kind+1)),"Wrong family in controller selector.");
   TestCheck(header.Received>0&&header.Forwarded==header.Received&&snapshotDevices.Any(x=>x.InputCount>0),"No controller input received and forwarded in the tab.");
  }
  internal static async Task TestLifecycle(List<string> phases,bool native=false){
   using(var dashboard=new Dashboard(false,true)){
    foreach(int kind in App.SupportedKinds)for(int cycle=0;cycle<2;++cycle){
     dashboard.SelectKind(kind);await dashboard.Start();await dashboard.TestRunning();if(native&&kind>=2)await dashboard.TestControllerInput(kind);await dashboard.Stop();dashboard.TestStopped();
     phases.Add("Real Start/Stop and joined worker: type="+kind+", cycle="+cycle);
    }
    dashboard.SelectKind(App.SupportedKinds[0]);await dashboard.Start();await dashboard.TestRunning();
    dashboard.ReadFailure(new InvalidDataException("Read error injected by lifecycle test."));
    TestCheck(dashboard.busy&&!dashboard.lifecycle.IsCompleted,"Stop after error was not tracked.");
    dashboard.Window.Close();await dashboard.TestClosed();phases.Add("Injected read error, immediate close, tracked stop before destruction");
   }
   using(var dashboard=new Dashboard(false,true)){
    dashboard.SelectKind(App.SupportedKinds[0]);var pending=dashboard.Start();
    TestCheck(dashboard.busy&&!pending.IsCompleted,"Start must be a tracked transition.");
    dashboard.Window.Close();await pending;await dashboard.TestClosed();phases.Add("Close during pending start: request preserved; no abandoned capture");
   }
   using(var dashboard=new Dashboard(false,true)){
    dashboard.SelectKind(App.SupportedKinds[0]);await dashboard.Start();await dashboard.TestRunning();
    dashboard.Window.Close();await dashboard.TestClosed();phases.Add("New capture after close, then close with active capture");
   }
  }
  public void Dispose(){if(disposed)return;disposed=true;closed=true;refresh.Dispose();if(registration!=null){using(var complete=new ManualResetEvent(false)){if(registration.Unregister(complete))complete.WaitOne();}registration=null;}if(bridge!=null){bridge.Dispose();bridge=null;}}
 }
}

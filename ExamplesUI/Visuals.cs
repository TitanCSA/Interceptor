using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Media;

namespace InterceptorViewer {
 static class Palette {
  public static readonly Brush Blue=Brush("#1464E8"),Ink=Brush("#152744"),Muted=Brush("#61738D"),Line=Brush("#D8E2F0"),Paper=Brush("#FFFFFF"),Soft=Brush("#EFF4FA"),Yellow=Brush("#FFE365"),PaleBlue=Brush("#E8F1FF");
  public static Brush Brush(string color){var b=new SolidColorBrush((Color)ColorConverter.ConvertFromString(color));b.Freeze();return b;}
 }
 public sealed class InputDrawing : FrameworkElement {
  public int Kind;public bool Azerty=true,NativeLayout;public DeviceState State=DeviceState.Empty();
  static readonly Typeface Font=new Typeface("Segoe UI"),Mono=new Typeface("Consolas");
  static readonly Dictionary<Brush,Dictionary<double,Pen>> pens=new Dictionary<Brush,Dictionary<double,Pen>>();
  static Geometry Shape(string path){var value=Geometry.Parse(path);value.Freeze();return value;}
  static readonly Geometry mouseOutline=Shape("M 260,45 C 170,45 145,102 145,170 L 145,282 C 145,375 190,405 260,405 C 330,405 375,375 375,282 L 375,170 C 375,102 350,45 260,45 Z"),mouseLeft=Shape("M 251,58 C 183,58 158,102 158,166 L 158,202 L 251,202 Z"),mouseRight=Shape("M 269,58 C 337,58 362,102 362,166 L 362,202 L 269,202 Z"),controllerBody=Shape("M 160,106 C 115,115 88,220 70,330 C 63,375 89,402 120,375 L 219,298 L 460,298 L 559,375 C 590,402 617,375 610,330 C 592,220 565,115 520,106 C 464,79 216,79 160,106 Z");
  sealed class KeyShape{public string Label;public int Code;public double X,Y,W,H;}
  static readonly List<KeyShape> keys=MakeKeys();
  public static int KeyCount{get{return keys.Count;}}
  public void Update(DeviceState state){State=state;InvalidateVisual();}
  protected override void OnRender(DrawingContext d){
   base.OnRender(d);double scale=Math.Min(ActualWidth/1100,ActualHeight/440);if(scale<=0)return;
   d.PushTransform(new TranslateTransform((ActualWidth-1100*scale)/2,(ActualHeight-440*scale)/2));d.PushTransform(new ScaleTransform(scale,scale));
   if(Kind==0)Keyboard(d);else if(Kind==1)Mouse(d);else Controller(d);
   d.Pop();d.Pop();
  }
  static void Text(DrawingContext d,string value,double x,double y,double size,Brush brush,bool mono=false){var t=new FormattedText(value??"",CultureInfo.CurrentUICulture,FlowDirection.LeftToRight,mono?Mono:Font,size,brush);d.DrawText(t,new Point(x,y));}
  static void Center(DrawingContext d,string value,Rect rect,double size,Brush brush){var t=new FormattedText(value,CultureInfo.CurrentUICulture,FlowDirection.LeftToRight,Font,size,brush);d.DrawText(t,new Point(rect.X+(rect.Width-t.Width)/2,rect.Y+(rect.Height-t.Height)/2));}
  // Only the UI thread renders; the finite palette/width set bounds this cache.
  static Pen Pen(Brush brush,double size=1){Dictionary<double,Pen> widths;if(!pens.TryGetValue(brush,out widths)){widths=new Dictionary<double,Pen>();pens.Add(brush,widths);}Pen value;if(!widths.TryGetValue(size,out value)){value=new Pen(brush,size);value.Freeze();widths.Add(size,value);}return value;}
  static void Round(DrawingContext d,Rect r,Brush fill,double radius=8,Brush line=null){d.DrawRoundedRectangle(fill,line==null?null:Pen(line),r,radius,radius);}
  static void Add(List<KeyShape> list,string label,int code,double x,double y,double w=1,double h=1){list.Add(new KeyShape{Label=label,Code=code,X=x,Y=y,W=w,H=h});}
  static List<KeyShape> MakeKeys(){
   var list=new List<KeyShape>();Add(list,"Esc",1,0,0);
   for(int i=0;i<12;i++)Add(list,"F"+(i+1),i<10?0x3B+i:0x57+i-10,2+i+(i>=4?.5:0)+(i>=8?.5:0),0);
   Add(list,"PrtSc",0x137,15.6,0);Add(list,"ScrLk",0x46,16.6,0);Add(list,"Pause",0x245,17.6,0);
   string[] numbers={"²","1","2","3","4","5","6","7","8","9","0","−","="};for(int i=0;i<numbers.Length;i++)Add(list,numbers[i],i==0?0x29:i+1,i,1.3);
   Add(list,"Back",0x0E,13,1.3,2);Add(list,"Tab",0x0F,0,2.3,1.5);
   string[] top={"Q","W","E","R","T","Y","U","I","O","P","[","]"};for(int i=0;i<12;i++)Add(list,top[i],0x10+i,1.5+i,2.3);
   Add(list,"Enter",0x1C,13.5,2.3,1.5,2);Add(list,"Lock",0x3A,0,3.3,1.75);
   string[] middle={"A","S","D","F","G","H","J","K","L",";","'"};for(int i=0;i<middle.Length;i++)Add(list,middle[i],0x1E+i,1.75+i,3.3);
   Add(list,"\\",0x2B,12.75,3.3,.75);Add(list,"Shift",0x2A,0,4.3,1.25);Add(list,"< >",0x56,1.25,4.3);
   string[] bottom={"Z","X","C","V","B","N","M",",",".","/"};for(int i=0;i<bottom.Length;i++)Add(list,bottom[i],0x2C+i,2.25+i,4.3);
   Add(list,"Shift",0x36,12.25,4.3,2.75);Add(list,"Ctrl",0x1D,0,5.3,1.25);Add(list,"Win",0x15B,1.25,5.3,1.25);Add(list,"Alt",0x38,2.5,5.3,1.25);Add(list,"Space",0x39,3.75,5.3,6.25);Add(list,"Alt Gr",0x138,10,5.3,1.25);Add(list,"Win",0x15C,11.25,5.3,1.25);Add(list,"Menu",0x15D,12.5,5.3,1.25);Add(list,"Ctrl",0x11D,13.75,5.3,1.25);
   string[] nav={"Ins","Home","Page ↑","Del","End","Page ↓"};int[] scans={0x152,0x147,0x149,0x153,0x14F,0x151};for(int i=0;i<6;i++)Add(list,nav[i],scans[i],15.6+i%3,1.3+i/3);
   Add(list,"↑",0x148,16.6,4.3);Add(list,"←",0x14B,15.6,5.3);Add(list,"↓",0x150,16.6,5.3);Add(list,"→",0x14D,17.6,5.3);
   string[] pad={"Lock","/","×","−","7","8","9","+","4","5","6","1","2","3","Enter","0","."};int[] padScans={0x45,0x135,0x37,0x4A,0x47,0x48,0x49,0x4E,0x4B,0x4C,0x4D,0x4F,0x50,0x51,0x11C,0x52,0x53};
   double[] px={0,1,2,3,0,1,2,3,0,1,2,0,1,2,3,0,2},py={0,0,0,0,1,1,1,1,2,2,2,3,3,3,3,4,4};
   for(int i=0;i<pad.Length;i++)Add(list,pad[i],padScans[i],19.2+px[i],1.3+py[i],i==15?2:1,i==7||i==14?2:1);
   return list;
  }
  string Label(KeyShape key){
   if(!Azerty)return key.Label=="²"?"`":key.Label;
   switch(key.Code){case 0x10:return "A";case 0x11:return "Z";case 0x1E:return "Q";case 0x2C:return "W";case 0x27:return "M";case 0x28:return "ù";case 0x32:return ",";case 0x33:return ";";case 0x34:return ":";case 0x35:return "!";case 0x1A:return "^";case 0x1B:return "$";case 0x2B:return "*";default:return key.Label;}
  }
  void Keyboard(DrawingContext d){
   Round(d,new Rect(4,46,1092,330),Palette.Soft,20,Palette.Line);
   foreach(var key in keys){var r=new Rect(21+key.X*46,65+key.Y*46,key.W*46-5,key.H*46-5);bool pressed=State.Pressed(key.Code);Round(d,new Rect(r.X,r.Y+3,r.Width,r.Height),pressed?Palette.Blue:Palette.Line,6);Round(d,r,pressed?Palette.Blue:Palette.Paper,6,pressed?null:Palette.Line);Center(d,Label(key),r,key.W<1?11:key.Label.Length>5?10:12,pressed?Palette.Paper:Palette.Ink);}
   int count=0;if(State.Keys!=null)foreach(byte value in State.Keys)for(int i=0;i<8;i++)if((value&(1<<i))!=0)count++;
   Text(d,"FULL KEYBOARD",8,4,13,Palette.Muted);Text(d,Azerty?"ISO · AZERTY · 105 keys":"ISO · QWERTY · 105 keys",810,4,13,Palette.Muted);
   d.DrawEllipse(Palette.Blue,null,new Point(16,407),5,5);Text(d,count+" key(s) held",31,396,15,Palette.Ink);Text(d,"Blue while held · state clears on release",510,396,14,Palette.Muted);
  }
  void Mouse(DrawingContext d){
   Text(d,"5-BUTTON MOUSE",20,6,13,Palette.Muted);
   d.DrawGeometry(Palette.Soft,Pen(Palette.Line,2),mouseOutline);
   d.DrawGeometry((State.MouseButtons&1)!=0?Palette.Blue:Palette.Paper,Pen(Palette.Line),mouseLeft);d.DrawGeometry((State.MouseButtons&2)!=0?Palette.Blue:Palette.Paper,Pen(Palette.Line),mouseRight);
   Round(d,new Rect(245,115,30,65),(State.MouseButtons&4)!=0?Palette.Blue:Palette.Ink,12);for(int i=0;i<5;i++)d.DrawLine(Pen(Palette.Paper,.8),new Point(252,127+i*9),new Point(268,127+i*9));
   Round(d,new Rect(128,211,18,56),(State.MouseButtons&8)!=0?Palette.Blue:Palette.Paper,7,Palette.Line);Round(d,new Rect(128,276,18,56),(State.MouseButtons&16)!=0?Palette.Blue:Palette.Paper,7,Palette.Line);
   Text(d,"X1",88,226,13,Palette.Muted);Text(d,"X2",88,292,13,Palette.Muted);Center(d,"I",new Rect(229,303,62,52),34,Palette.Blue);
   Metric(d,"X",State.MouseX.ToString(CultureInfo.InvariantCulture),480,48);Metric(d,"Y",State.MouseY.ToString(CultureInfo.InvariantCulture),760,48);
   Text(d,(State.MouseFlags&1)!=0?"Absolute coordinates of the last report":"Relative movement of the last report",483,154,15,Palette.Muted);
   Metric(d,"VERTICAL WHEEL",State.Wheel.ToString(CultureInfo.InvariantCulture),480,204);Metric(d,"HORIZONTAL WHEEL",State.HorizontalWheel.ToString(CultureInfo.InvariantCulture),760,204);
   Text(d,"Pressed: "+MouseNames(State.MouseButtons),483,348,16,Palette.Ink);Text(d,"Movements are forwarded to Windows unchanged.",483,383,13,Palette.Muted);
  }
  static string MouseNames(uint buttons){string[] names={"left","right","wheel","X1","X2"};var active=new List<string>();for(int i=0;i<5;i++)if((buttons&(1u<<i))!=0)active.Add(names[i]);return active.Count==0?"none":string.Join(" · ",active.ToArray());}
  static void Metric(DrawingContext d,string label,string value,double x,double y){Round(d,new Rect(x,y,258,97),Palette.Soft,12);Text(d,label,x+18,y+13,12,Palette.Muted);Text(d,value,x+18,y+35,34,Palette.Ink,true);}
  bool Native{get{return NativeLayout||(State.Flags&32)!=0;}}
  static readonly int[] gipBits={8,9,10,11,2,3,14,15,12,13,-1,-1,4,5,6,7};
  public static int NativeButtonBit(int xboxBit,bool gip){if(xboxBit<0||xboxBit>16)return -1;return gip&&xboxBit<16?gipBits[xboxBit]:xboxBit;}
  bool Button(int hid,int native){int bit=Native?NativeButtonBit(native,State.Info.Type==6):hid-1;return bit>=0&&bit<64&&(State.Buttons&(1UL<<bit))!=0;}
  void Controller(DrawingContext d){
   Text(d,Native?"CONTROLLER · NATIVE BUTTONS":"HID CONTROLLER · DECLARED BUTTON NUMBERS",20,6,13,Palette.Muted);
   d.DrawGeometry(Palette.Soft,Pen(Palette.Line,2),controllerBody);
   Round(d,new Rect(142,64,126,29),Button(5,8)?Palette.Blue:Palette.Ink,9);Round(d,new Rect(414,64,126,29),Button(6,9)?Palette.Blue:Palette.Ink,9);Center(d,Native?"LB":"5",new Rect(142,64,126,29),12,Palette.Paper);Center(d,Native?"RB":"6",new Rect(414,64,126,29),12,Palette.Paper);
   if(Native){Trigger(d,142,33,State.LeftTrigger,"LT",(State.Flags&16)!=0);Trigger(d,414,33,State.RightTrigger,"RT",(State.Flags&16)!=0);}else{Small(d,205,42,"9",Button(9,0));Small(d,477,42,"10",Button(10,0));}
   Stick(d,220,181,0,1,Button(11,6),Native?"X / Y":"X / Y · 11");
   int rx=(State.AxisMask&0x18)==0x18?3:2,ry=rx==3?4:5;Stick(d,420,270,rx,ry,Button(12,7),(rx==3?"Rx / Ry":"Z / Rz")+(Native?"":" · 12"));
   int hat=State.Hat;bool up=Native?Button(0,0):hat==0||hat==1||hat==7;bool down=Native?Button(0,1):hat>=3&&hat<=5;bool left=Native?Button(0,2):hat>=5&&hat<=7;bool right=Native?Button(0,3):hat>=1&&hat<=3;
   Round(d,new Rect(240,260,34,86),Palette.Ink,5);Round(d,new Rect(214,286,86,34),Palette.Ink,5);Round(d,new Rect(240,260,34,26),up?Palette.Blue:Palette.Ink,4);Round(d,new Rect(240,320,34,26),down?Palette.Blue:Palette.Ink,4);Round(d,new Rect(214,286,26,34),left?Palette.Blue:Palette.Ink,4);Round(d,new Rect(274,286,26,34),right?Palette.Blue:Palette.Ink,4);
   Face(d,515,171,Native?"B":"2",Button(2,13));Face(d,477,209,Native?"A":"1",Button(1,12));Face(d,439,171,Native?"X":"3",Button(3,14));Face(d,477,133,Native?"Y":"4",Button(4,15));
   Small(d,304,179,Native?"View":"7",Button(7,5));Small(d,369,179,Native?"Menu":"8",Button(8,4));
   d.DrawEllipse(Native&&(State.Buttons&(1UL<<16))!=0?Palette.Blue:Palette.Yellow,null,new Point(338,133),17,17);Center(d,"I",new Rect(321,116,34,34),18,Palette.Ink);
   Text(d,"DECLARED AXES",710,38,13,Palette.Muted);string[] labels={"X","Y","Z","Rx","Ry","Rz","Slider","Dial"};
   for(int i=0;i<8;i++){
    double y=68+i*34;bool valid=(State.AxisMask&(1u<<i))!=0;Text(d,labels[i],710,y,13,Palette.Ink);
    Round(d,new Rect(771,y+6,180,7),Palette.Line,3);d.DrawLine(Pen(Palette.Muted),new Point(861,y+3),new Point(861,y+16));
    if(valid){double v=Clamp(State.Axes[i]);Round(d,new Rect(Math.Min(861,861+v*90),y+6,Math.Max(2,Math.Abs(v)*90),7),Palette.Blue,3);Text(d,State.RawAxes[i].ToString(CultureInfo.InvariantCulture),970,y-1,14,Palette.Ink,true);}else Text(d,"—",970,y-1,14,Palette.Muted);
   }
   var pressed=new List<string>();for(int i=0;i<64;i++)if((State.Buttons&(1UL<<i))!=0)pressed.Add(Native?"bit "+i:(i+1).ToString());
   if(Native){string value=pressed.Count==0?"none":string.Join(", ",pressed.Take(12).ToArray())+(pressed.Count>12?"…":"");Text(d,"Active buttons: "+value,710,360,14,Palette.Ink);}
   else{Text(d,"HID BUTTONS · 1 TO 64",710,347,11,Palette.Muted);for(int i=0;i<64;i++){bool on=(State.Buttons&(1UL<<i))!=0;var r=new Rect(710+(i%16)*22,369+(i/16)*16,20,14);Round(d,r,on?Palette.Blue:Palette.Soft,3);Center(d,(i+1).ToString(),r,9,on?Palette.Paper:Palette.Muted);}}
   Text(d,Native?"Xbox layout · states received from the transport":"Generic layout · HID usages vary by controller",20,413,13,Palette.Muted);
  }
  static double Clamp(double value){return double.IsNaN(value)||double.IsInfinity(value)?0:Math.Max(-1,Math.Min(1,value));}
  void Stick(DrawingContext d,double x,double y,int ax,int ay,bool pressed,string label){
   d.DrawEllipse(Palette.Paper,Pen(Palette.Line,2),new Point(x,y),42,42);d.DrawLine(Pen(Palette.Line),new Point(x-30,y),new Point(x+30,y));d.DrawLine(Pen(Palette.Line),new Point(x,y-30),new Point(x,y+30));
   double dx=(State.AxisMask&(1u<<ax))!=0?Clamp(State.Axes[ax])*22:0,dy=(State.AxisMask&(1u<<ay))!=0?Clamp(State.Axes[ay])*22:0;
   // Xbox native Y is positive upwards; generic HID keeps descriptor values.
   if(Native)dy=-dy;
   d.DrawEllipse(pressed?Palette.Blue:Palette.Ink,null,new Point(x+dx,y+dy),25,25);d.DrawEllipse(null,Pen(pressed?Palette.Paper:Palette.Muted,1),new Point(x+dx,y+dy),17,17);Center(d,label,new Rect(x-45,y+46,90,19),11,Palette.Muted);
  }
  static void Face(DrawingContext d,double x,double y,string label,bool pressed){d.DrawEllipse(pressed?Palette.Blue:Palette.Paper,Pen(Palette.Line,2),new Point(x,y),22,22);Center(d,label,new Rect(x-22,y-22,44,44),17,pressed?Palette.Paper:Palette.Ink);}
  static void Small(DrawingContext d,double x,double y,string label,bool pressed){Round(d,new Rect(x-20,y-11,40,22),pressed?Palette.Blue:Palette.Paper,11,Palette.Line);Center(d,label,new Rect(x-20,y-11,40,22),10,pressed?Palette.Paper:Palette.Ink);}
  static void Trigger(DrawingContext d,double x,double y,float value,string label,bool valid){Round(d,new Rect(x,y,126,21),Palette.Line,6);if(valid)Round(d,new Rect(x,y,126*Math.Max(0,Math.Min(1,value)),21),Palette.Blue,6);Center(d,valid?label+" "+(value*100).ToString("F0")+" %":label+" —",new Rect(x,y,126,21),10,valid&&value>.6?Palette.Paper:Palette.Ink);}
 }
}

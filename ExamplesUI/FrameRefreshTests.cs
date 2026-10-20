using System;
using System.Collections.Concurrent;
using System.IO;
using System.Threading.Tasks;

namespace InterceptorViewer {
 // Exercise the production scheduler with controlled dispatcher/frame events.
 // No window, capture, driver or input source is opened by these fixtures.
 internal static class FrameRefreshTests {
  public static int Run(){
   int checks=0,reads=0,subscriptions=0,latest=0,presented=-1;
   Action<bool,string> check=(ok,message)=>{++checks;if(!ok)throw new InvalidDataException(message);};
   var queue=new ConcurrentQueue<Action>();EventHandler frame=null;bool duringRead=false;
   FrameRefresh refresh=null;
   refresh=new FrameRefresh(action=>queue.Enqueue(action),
    handler=>{check(frame==null,"Only one frame subscription");frame=handler;++subscriptions;},
    handler=>{check(frame==handler,"Remove the matching frame callback");frame=null;--subscriptions;},
    ()=>{++reads;presented=latest;if(duringRead){duringRead=false;++latest;refresh.Request();}});
   Action pump=()=>{Action action;check(queue.TryDequeue(out action),"A requested frame must reach the dispatcher");action();};
   Action draw=()=>{var handler=frame;check(handler!=null,"The next available frame is armed");handler(null,EventArgs.Empty);};
   try{
    var producers=new Task[4];for(int i=0;i<producers.Length;++i)producers[i]=Task.Run(()=>{for(int n=0;n<25000;++n)refresh.Request();});
    Task.WaitAll(producers);check(queue.Count==1&&subscriptions==0&&reads==0,"100000 notifications coalesce before touching the UI");
    pump();for(int n=0;n<1000;++n)refresh.Request();latest=12345;
    check(queue.IsEmpty&&subscriptions==1,"Notifications while awaiting a frame do not flood the dispatcher");
    duringRead=true;draw();check(reads==1&&presented==12345&&subscriptions==0&&queue.Count==1,"A final change during snapshot acknowledgement schedules its next frame");
    pump();draw();check(reads==2&&presented==12346&&queue.IsEmpty&&frame==null,"The quiet final change is shown without another input");
    for(int cycle=0;cycle<10000;++cycle){
     for(int n=0;n<64;++n){latest=cycle*64+n;refresh.Request();}
     check(queue.Count==1,"A burst occupies one dispatcher operation");pump();draw();
     check(presented==latest&&queue.IsEmpty&&subscriptions==0,"Show the latest actual state and return to idle without a frame subscription");
    }
    int before=reads;refresh.Request();refresh.Dispose();pump();
    check(reads==before&&subscriptions==0&&frame==null,"Closing before dispatcher delivery prevents frame subscription and reads");
    refresh.Request();check(queue.IsEmpty,"Disposed viewers cannot enqueue more updates");
   }finally{refresh.Dispose();}
   EventHandler stale=null;int closedReads=0;
   using(var closing=new FrameRefresh(action=>action(),handler=>stale=handler,handler=>frame=null,()=>++closedReads)){
    closing.Request();closing.Dispose();stale(null,EventArgs.Empty);
    check(closedReads==0,"A stale compositor callback after close cannot read the unloaded bridge");
   }
   bool reject=true;
   using(var stopping=new FrameRefresh(action=>{if(reject)throw new InvalidOperationException("Dispatcher shutting down");queue.Enqueue(action);},handler=>frame=handler,handler=>frame=null,()=>{})){
    stopping.Request();reject=false;stopping.Request();
    check(queue.Count==1,"A rejected dispatcher operation does not permanently occupy the notification slot");
    pump();stopping.Dispose();check(frame==null,"Disposal removes the compositor callback");
   }
   return checks;
  }
 }
}

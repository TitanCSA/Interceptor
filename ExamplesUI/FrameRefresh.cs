using System;
using System.Threading;
using System.Windows.Media;
using System.Windows.Threading;

namespace InterceptorViewer {
 // Request may run on a notification thread; arming, rendering and disposal
 // run on the UI thread. Only one frame is subscribed, and only while dirty.
 internal sealed class FrameRefresh : IDisposable {
  readonly Action<Action> post;
  readonly Action<EventHandler> subscribe,unsubscribe;
  readonly Action read,arm;
  readonly EventHandler onFrame;
  int scheduled;
  bool armed;
  volatile bool disposed;
  public FrameRefresh(Dispatcher dispatcher,Action read)
   : this(action=>{dispatcher.BeginInvoke(action,DispatcherPriority.Render);},
          handler=>{CompositionTarget.Rendering+=handler;},
          handler=>{CompositionTarget.Rendering-=handler;},read){}
  internal FrameRefresh(Action<Action> post,Action<EventHandler> subscribe,Action<EventHandler> unsubscribe,Action read){
   this.post=post;this.subscribe=subscribe;this.unsubscribe=unsubscribe;this.read=read;
   arm=Arm;onFrame=Render;
  }
  public void Request(){
   if(disposed||Interlocked.Exchange(ref scheduled,1)!=0)return;
   try{post(arm);}catch(InvalidOperationException){Interlocked.Exchange(ref scheduled,0);}
  }
  void Arm(){
   if(disposed){Interlocked.Exchange(ref scheduled,0);return;}
   armed=true;subscribe(onFrame);
  }
  void Detach(){if(armed){unsubscribe(onFrame);armed=false;}}
  void Render(object sender,EventArgs args){
   Detach();
   // Release before acknowledging the native snapshot. A notification arriving
   // during/after that read must be able to schedule the final next frame.
   Interlocked.Exchange(ref scheduled,0);
   if(!disposed)read();
  }
  public void Dispose(){disposed=true;Detach();Interlocked.Exchange(ref scheduled,0);}
 }
}

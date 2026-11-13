import titancsa.products.interceptor.Interceptor;
import java.nio.file.Path;
import java.util.*;
import java.util.concurrent.atomic.AtomicReference;
import java.lang.foreign.*;
import static java.lang.foreign.ValueLayout.*;
public class ProductContract {
 static void check(boolean b){if(!b)throw new AssertionError("Product Java SDK contract failed");}
 public static void main(String[] args)throws Throwable {
  var fixture=Path.of(args[0]);
  var lookup=SymbolLookup.libraryLookup(fixture,Arena.global());
  var mode=Linker.nativeLinker().downcallHandle(lookup.find("interceptor_test_mode").orElseThrow(),FunctionDescriptor.ofVoid(JAVA_INT));
  var allowed=Set.of("keyboard","mouse","hid","xinput","xinputhid","gip");
  try(var client=new Interceptor(fixture)) {
   client.initialize();var info=client.info();check(info.inputMask()==3 && info.controllerMask()==15);
   check(client.devices().stream().map(Interceptor.Device::kind).collect(java.util.stream.Collectors.toSet()).equals(allowed));
   var batch=client.receive(0);check(batch.size()==allowed.size());
   for(var event:batch){check(allowed.contains(event.kind));check(Long.compareUnsigned(event.sequence,Long.MIN_VALUE)>0);
    if(event.kind.equals("keyboard"))event.state.scalars.put("scan_code",0x30L);
    if(event.kind.equals("mouse"))event.state.scalars.put("y",-7L);
   }
   check(client.send(batch)==allowed.size());check(client.send(batch)==0);
   var current=client.receive(0);try{client.send(batch);throw new AssertionError("Stale batch accepted");}catch(IllegalArgumentException expected){}
   current.get(0).blocked=true;check(client.send(current)==allowed.size()-1);
   var retry=client.receive(0);mode.invokeExact(2);
   try{client.send(retry);throw new AssertionError("Partial send missing");}catch(Interceptor.Error error){check(error.code==170 && error.accepted==1);}
   check(client.send(retry)==allowed.size()-1);mode.invokeExact(0);
   client.discard(client.receive(0));
   for(var kind:List.of("keyboard","mouse","hid","xinput","xinputhid","gip"))if(!allowed.contains(kind)) {
    try{client.inject(kind,Map.of());throw new AssertionError("Unsupported family accepted");}catch(Interceptor.Error error){check(error.code==50);}
   }
   mode.invokeExact(1);var failure=new AtomicReference<Throwable>();
   var reader=new Thread(()->{try{check(client.receive(2000).isEmpty());}catch(Throwable e){failure.set(e);}});
   reader.start();Thread.sleep(30);client.cancel();reader.join(3000);check(!reader.isAlive() && failure.get()==null);mode.invokeExact(0);
  }
  try(var production=new Interceptor()){}
  System.out.println("Interceptor Java SDK: masks, edits, blocking, retries, uint64, cancellation and production DLL passed");
 }
}

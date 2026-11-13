import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';
import 'package:interceptor/interceptor.dart';

void check(bool value) {
  if (!value) throw StateError('Product Dart SDK contract failed');
}

Future<void> cancellationWorker((CancellationToken, SendPort) data) async {
  data.$2.send('ready');
  await Future<void>.delayed(const Duration(milliseconds: 50));
  try {
    data.$1.cancel();
    data.$2.send('cancelled');
  } catch (error) {
    data.$2.send(error.toString());
  }
}

Future<void> main(List<String> args) async {
  final fixture = args.single;
  final mode = DynamicLibrary.open(fixture)
      .lookupFunction<Void Function(Uint32), void Function(int)>(
          'interceptor_test_mode');
  final allowed = <String>[
    "keyboard",
    "mouse",
    "hid",
    "xinput",
    "xinputhid",
    "gip"
  ];
  final client = await Interceptor.open(libraryPath: fixture);
  try {
    client.initialize();
    final info = client.info();
    check(info['input_mask'] == 3 && info['controller_mask'] == 15);
    check(client.devices().map((d) => d['kind']).toSet().containsAll(allowed));
    final batch = client.receive(timeout: 0);
    check(batch.length == allowed.length);
    check(batch.events.every((e) =>
        allowed.contains(e.kind) && e.sequence > (BigInt.from(1) << 63)));
    for (final event in batch.events) {
      if (event.kind == 'keyboard') event.field('scan_code').value = 0x30;
      if (event.kind == 'mouse') event.field('y').value = -7;
    }
    check(client.send(batch) == allowed.length);
    check(client.send(batch) == 0);
    final current = client.receive(timeout: 0);
    try {
      client.send(batch);
      throw StateError('Stale batch accepted');
    } on ArgumentError catch (e) {
      check(e.toString().contains('Stale'));
    }
    current[0].blocked = true;
    check(client.send(current) == allowed.length - 1);
    final retry = client.receive(timeout: 0);
    mode(2);
    try {
      client.send(retry);
      throw StateError('Partial send missing');
    } on InterceptorException catch (e) {
      check(e.code == 170 && e.accepted == 1);
    }
    check(client.send(retry) == allowed.length - 1);
    mode(0);
    client.discard(client.receive(timeout: 0));
    for (final kind in [
      'keyboard',
      'mouse',
      'hid',
      'xinput',
      'xinputhid',
      'gip'
    ].where((k) => !allowed.contains(k))) {
      try {
        client.inject(kind, {});
        throw StateError('Unsupported family accepted');
      } on InterceptorException catch (e) {
        check(e.code == 50);
      }
    }
    mode(1);
    final ready = Completer<void>(), done = Completer<void>();
    final port = ReceivePort();
    port.listen((message) {
      if (message == 'ready') {
        ready.complete();
      } else if (message == 'cancelled') {
        done.complete();
      } else {
        done.completeError(StateError('$message'));
      }
    });
    await Isolate.spawn(
        cancellationWorker, (client.cancellationToken, port.sendPort));
    await ready.future;
    check(client.receive(timeout: 2000).length == 0);
    await done.future;
    port.close();
    mode(0);
    final staleToken = client.cancellationToken;
    client.close();
    try {
      staleToken.cancel();
      throw StateError('Stale cancellation accepted');
    } on InterceptorException catch (e) {
      check(e.code == 6);
    }
  } finally {
    client.close();
  }
  final production = await Interceptor.open();
  production.close();
  print(
      'Interceptor Dart SDK: product masks, edits, blocking, retries, uint64, unsupported families, isolate cancellation and production DLL passed');
}

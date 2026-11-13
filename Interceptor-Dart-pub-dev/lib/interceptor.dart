/// Direct Windows x64 FFI bindings. Capture calls are synchronous: use a worker
/// isolate for a Flutter UI. Installing this package does not install a driver.
library;

import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';

final class InterceptorException implements Exception {
  final int code, accepted;
  final String message;
  InterceptorException(this.code, this.message, this.accepted);
  @override
  String toString() =>
      'InterceptorException($code, accepted=$accepted): $message';
}

final class Field {
  int value;
  double normalized;
  Uint8List data;
  final Map<String, Object> metadata;
  Field(this.value, this.normalized, this.data, Map<String, Object> metadata)
      : metadata = Map.unmodifiable(metadata);
}

final class Input {
  final String kind, token;
  final BigInt sequence, timestamp;
  final List<Field> fields;
  final Map<String, int> metadata;
  bool blocked;
  Input(this.kind, this.token, this.sequence, this.timestamp,
      List<Field> fields, Map<String, int> metadata, this.blocked)
      : fields = List.unmodifiable(fields),
        metadata = Map.unmodifiable(metadata);
  Field field(String name) {
    final matches = fields.where((f) => f.metadata['name'] == name).toList();
    if (matches.length != 1) {
      throw ArgumentError(
          'Field is absent or ambiguous; use descriptor metadata');
    }
    return matches.single;
  }
}

final class Batch {
  final List<Input> events;
  Batch._(List<Input> inputs) : events = List.unmodifiable(inputs);
  int get length => events.length;
  Input operator [](int index) => events[index];
}

final class _Heap {
  static final _library = DynamicLibrary.open('kernel32.dll');
  static final _get = _library.lookupFunction<Pointer<Void> Function(),
      Pointer<Void> Function()>('GetProcessHeap');
  static final _allocate = _library.lookupFunction<
      Pointer<Void> Function(Pointer<Void>, Uint32, IntPtr),
      Pointer<Void> Function(Pointer<Void>, int, int)>('HeapAlloc');
  static final _release = _library.lookupFunction<
      Int32 Function(Pointer<Void>, Uint32, Pointer<Void>),
      int Function(Pointer<Void>, int, Pointer<Void>)>('HeapFree');
  static Pointer<Void> allocate(int size) {
    final pointer = _allocate(_get(), 8, size == 0 ? 1 : size);
    if (pointer == nullptr) throw StateError('Native allocation failed');
    return pointer;
  }

  static void free(Pointer<Void> pointer) {
    _release(_get(), 0, pointer);
  }
}

ByteData _view(Pointer<Void> p, int size) => p
    .cast<Uint8>()
    .asTypedList(size)
    .buffer
    .asByteData(p.cast<Uint8>().asTypedList(size).offsetInBytes, size);
int _u32(Pointer<Void> p, int offset) =>
    _view(p, offset + 4).getUint32(offset, Endian.little);
int _i64(Pointer<Void> p, int offset) =>
    _view(p, offset + 8).getInt64(offset, Endian.little);
int _raw64(Pointer<Void> p, int offset) =>
    _view(p, offset + 8).getUint64(offset, Endian.little);
BigInt _u64(Pointer<Void> p, int offset) =>
    (BigInt.from(_u32(p, offset + 4)) << 32) | BigInt.from(_u32(p, offset));
Pointer<Void> _pointer(Pointer<Void> p, int offset) =>
    Pointer<Void>.fromAddress(_raw64(p, offset));
Pointer<Void> _at(Pointer<Void> p, int offset) =>
    Pointer<Void>.fromAddress(p.address + offset);
void _put32(Pointer<Void> p, int offset, int value) =>
    _view(p, offset + 4).setUint32(offset, value, Endian.little);
void _put64(Pointer<Void> p, int offset, int value) =>
    _view(p, offset + 8).setUint64(offset, value, Endian.little);
String _text(Pointer<Void> p) {
  if (p == nullptr) return '';
  final bytes = <int>[];
  final pointer = p.cast<Uint8>();
  for (int i = 0; i < 1048576; i++) {
    final byte = pointer[i];
    if (byte == 0) return utf8.decode(bytes);
    bytes.add(byte);
  }
  throw StateError('Native string exceeds the SDK limit');
}

Pointer<Void> _string(String value) {
  if (value.contains('\u0000')) throw ArgumentError('String contains NUL');
  final bytes = utf8.encode(value);
  final p = _Heap.allocate(bytes.length + 1);
  p.cast<Uint8>().asTypedList(bytes.length).setAll(0, bytes);
  return p;
}

final class Interceptor {
  static const _kinds = [
    '',
    'keyboard',
    'mouse',
    'hid',
    'xinput',
    'xinputhid',
    'gip'
  ];
  final DynamicLibrary _library;
  final String _libraryPath;
  int _session = 0, _ticket = 0;
  Batch? _current;
  Pointer<Void> _header = nullptr, _events = nullptr;
  late final _version = _library
      .lookupFunction<Uint32 Function(), int Function()>('interceptor_version');
  late final _size =
      _library.lookupFunction<Uint32 Function(Uint32), int Function(int)>(
          'interceptor_sizeof');
  late final _open = _library.lookupFunction<
      Uint32 Function(Pointer<Void>, Pointer<Void>),
      int Function(Pointer<Void>, Pointer<Void>)>('interceptor_open');
  late final _close = _library.lookupFunction<
      Uint32 Function(Uint64, Pointer<Void>),
      int Function(int, Pointer<Void>)>('interceptor_close');
  late final _initialize = _library.lookupFunction<
      Uint32 Function(Uint64, Pointer<Void>, Pointer<Void>),
      int Function(
          int, Pointer<Void>, Pointer<Void>)>('interceptor_initialize');
  late final _info = _library.lookupFunction<
      Uint32 Function(Uint64, Pointer<Void>, Pointer<Void>),
      int Function(int, Pointer<Void>, Pointer<Void>)>('interceptor_info_get');
  late final _devices = _library.lookupFunction<
      Uint32 Function(Uint64, Pointer<Void>, Pointer<Void>),
      int Function(
          int, Pointer<Void>, Pointer<Void>)>('interceptor_devices_get');
  late final _receive = _library.lookupFunction<
      Uint32 Function(Uint64, Uint32, Pointer<Void>, Pointer<Void>),
      int Function(
          int, int, Pointer<Void>, Pointer<Void>)>('interceptor_receive');
  late final _send = _library.lookupFunction<
      Uint32 Function(Uint64, Uint64, Pointer<Void>),
      int Function(int, int, Pointer<Void>)>('interceptor_send');
  late final _discard = _library.lookupFunction<
      Uint32 Function(Uint64, Uint64, Pointer<Void>),
      int Function(int, int, Pointer<Void>)>('interceptor_discard');
  late final _cancel = _library.lookupFunction<
      Uint32 Function(Uint64, Pointer<Void>),
      int Function(int, Pointer<Void>)>('interceptor_cancel');
  late final _descriptor = _library.lookupFunction<
      Uint32 Function(
          Uint64, Pointer<Void>, Uint32, Pointer<Void>, Pointer<Void>),
      int Function(int, Pointer<Void>, int, Pointer<Void>,
          Pointer<Void>)>('interceptor_descriptor');
  late final _inject = _library.lookupFunction<
      Uint32 Function(Uint64, Pointer<Void>, Uint32, Pointer<Void>),
      int Function(
          int, Pointer<Void>, int, Pointer<Void>)>('interceptor_inject');

  /// Resolve the bundled DLL from the Dart package cache, including Flutter desktop.
  static Future<Interceptor> open({String? libraryPath}) async {
    if (!Platform.isWindows || sizeOf<IntPtr>() != 8)
      throw UnsupportedError('Interceptor requires Windows x64');
    libraryPath ??= Platform.environment['INTERCEPTOR_SDK_NATIVE_PATH'];
    if (libraryPath == null) {
      final bundled = File(
          '${File(Platform.resolvedExecutable).parent.path}${Platform.pathSeparator}InterceptorSDKBridge.dll');
      if (bundled.existsSync()) libraryPath = bundled.path;
    }

    if (libraryPath == null) {
      final uri = await Isolate.resolvePackageUri(
          Uri.parse('package:interceptor/native/InterceptorSDKBridge.dll'));
      if (uri == null)
        throw StateError('Bundled native library could not be resolved');
      libraryPath = uri.toFilePath();
    }
    return Interceptor._(DynamicLibrary.open(libraryPath), libraryPath);
  }

  Interceptor._(this._library, this._libraryPath) {
    if (_version() != 1 ||
        [120, 64, 24, 40, 776, 56]
            .asMap()
            .entries
            .any((e) => _size(e.key + 1) != e.value)) {
      throw StateError('Incompatible native ABI');
    }
    final out = _Heap.allocate(8);
    try {
      _checked((e) => _open(out, e));
      _session = _raw64(out, 0);
    } finally {
      _Heap.free(out);
    }
  }

  /// A capability that may be sent to another isolate to cancel a blocked wait.
  CancellationToken get cancellationToken =>
      CancellationToken._(_libraryPath, _active);

  int get _active {
    if (_session == 0) throw StateError('Interceptor is closed');
    return _session;
  }

  int _checked(int Function(Pointer<Void>) invoke) {
    final error = _Heap.allocate(776);
    try {
      final code = invoke(error);
      final accepted = _u32(error, 4);
      if (code != 0)
        throw InterceptorException(code, _text(_at(error, 8)), accepted);
      return accepted;
    } finally {
      _Heap.free(error);
    }
  }

  Map<String, int> initialize() {
    final out = _Heap.allocate(24);
    try {
      _checked((e) => _initialize(_active, out, e));
      return {
        for (final item in [
          'ready',
          'reboot_required',
          'associated',
          'already_associated',
          'skipped'
        ].asMap().entries)
          item.value: _u32(out, item.key * 4)
      };
    } finally {
      _Heap.free(out);
    }
  }

  Map<String, Object> info() {
    final out = _Heap.allocate(56);
    try {
      _checked((e) => _info(_active, out, e));
      return {
        for (final item in [
          'installed',
          'active',
          'trial_state',
          'input_mask',
          'controller_mask',
          'device_count',
          'queued',
          'last_reset'
        ].asMap().entries)
          item.value: _u32(out, item.key * 4),
        for (final item in ['remaining_ms', 'timestamp_frequency', 'generation']
            .asMap()
            .entries)
          item.value: _u64(out, 32 + item.key * 8)
      };
    } finally {
      _Heap.free(out);
    }
  }

  List<Map<String, Object>> devices() {
    final out = _Heap.allocate(8);
    try {
      _checked((e) => _devices(_active, out, e));
      final list = _pointer(out, 0), items = _pointer(_pointer(out, 0), 8);
      return List.generate(_u32(list, 0), (i) {
        final d = _at(items, i * 40);
        return {
          'kind': _kinds[_u32(d, 0)],
          'id': _u32(d, 4),
          for (final item
              in ['vid', 'pid', 'usage_page', 'usage'].asMap().entries)
            item.value: _view(d, 16).getUint16(8 + item.key * 2, Endian.little),
          for (final item in ['token', 'name', 'hardware_id'].asMap().entries)
            item.value: _text(_pointer(d, 16 + item.key * 8))
        };
      });
    } finally {
      _Heap.free(out);
    }
  }

  /// Blocking receive; use timeout milliseconds or null for an infinite wait.
  Batch receive({int? timeout}) {
    if (timeout != null && (timeout < 0 || timeout > 0xfffffffe))
      throw ArgumentError('Invalid timeout');
    _current = null;
    _header = nullptr;
    final out = _Heap.allocate(8);
    try {
      _checked((e) => _receive(_active, timeout ?? 0xffffffff, out, e));
      _header = _pointer(out, 0);
      _ticket = _raw64(_header, 0);
      _events = _pointer(_header, 16);
      final inputs = List.generate(_u32(_header, 8), (i) {
        final e = _at(_events, i * 64), fs = _pointer(_at(_events, i * 64), 32);
        final fields = List.generate(_u32(e, 40), (j) {
          final f = _at(fs, j * 120);
          final n = _u32(f, 112);
          return Field(
              _i64(f, 8),
              _view(f, 56).getFloat64(48, Endian.little),
              n == 0
                  ? Uint8List(0)
                  : Uint8List.fromList(
                      _pointer(f, 104).cast<Uint8>().asTypedList(n)),
              {
                'name': _text(_pointer(f, 0)),
                'minimum': _i64(f, 16),
                'maximum': _i64(f, 24),
                'physical_minimum': _i64(f, 32),
                'physical_maximum': _i64(f, 40),
                for (final item in [
                  'group',
                  'index',
                  'usage_page',
                  'usage',
                  'collection',
                  'report_id',
                  'data_index',
                  'element',
                  'flags',
                  'units',
                  'units_exponent',
                  'bit_size'
                ].asMap().entries)
                  item.value: _u32(f, 56 + item.key * 4)
              });
        });
        return Input(
            _kinds[_u32(e, 0)],
            _text(_pointer(e, 24)),
            _u64(e, 8),
            _u64(e, 16),
            fields,
            {
              for (final item in [
                'decode_error',
                'decode_complete',
                'parser_status',
                'device'
              ].asMap().entries)
                item.value: _u32(e, 44 + item.key * 4)
            },
            _u32(e, 4) != 0);
      });
      return _current = Batch._(inputs);
    } finally {
      _Heap.free(out);
    }
  }

  void _validate(Batch batch) {
    _active;
    if (!identical(batch, _current) ||
        _header == nullptr ||
        _raw64(_header, 0) != _ticket ||
        batch.length != _u32(_header, 8))
      throw ArgumentError('Stale or foreign batch');
  }

  int send(Batch batch) {
    _validate(batch);
    for (var i = 0; i < batch.length; i++) {
      final fs = _pointer(_at(_events, i * 64), 32);
      for (var j = 0; j < batch[i].fields.length; j++) {
        final f = batch[i].fields[j];
        if (!f.normalized.isFinite ||
            f.data.length != _u32(_at(fs, j * 120), 112))
          throw ArgumentError(
              'Non-finite normalized value or resized binary field');
      }
    }
    for (var i = 0; i < batch.length; i++) {
      final event = _at(_events, i * 64),
          fs = _pointer(_at(_events, i * 64), 32);
      _put32(event, 4, batch[i].blocked ? 1 : 0);
      for (var j = 0; j < batch[i].fields.length; j++) {
        final f = _at(fs, j * 120), edit = batch[i].fields[j];
        _view(f, 16).setInt64(8, edit.value, Endian.little);
        _view(f, 56).setFloat64(48, edit.normalized, Endian.little);
        if (edit.data.isNotEmpty)
          _pointer(f, 104)
              .cast<Uint8>()
              .asTypedList(edit.data.length)
              .setAll(0, edit.data);
      }
    }
    return _checked((e) => _send(_active, _ticket, e));
  }

  void discard(Batch batch) {
    _validate(batch);
    _checked((e) => _discard(_active, _ticket, e));
  }

  void cancel() {
    _checked((e) => _cancel(_active, e));
  }

  Map<String, Object> descriptor(String token, {bool report = false}) {
    final t = _string(token), out = _Heap.allocate(24);
    try {
      _checked((e) => _descriptor(_active, t, report ? 1 : 0, out, e));
      final n = _u32(out, 8);
      return {
        'applicable': _u32(out, 0) != 0,
        'format': _u32(out, 4),
        'data': n == 0
            ? Uint8List(0)
            : Uint8List.fromList(_pointer(out, 16).cast<Uint8>().asTypedList(n))
      };
    } finally {
      _Heap.free(t);
      _Heap.free(out);
    }
  }

  int inject(String kind, Map<String, int> state,
      {String? token, Uint8List? report}) {
    final k = _kinds.indexOf(kind);
    if (k < 1) throw ArgumentError('Unknown input kind');
    final count = state.length + (report == null ? 0 : 1),
        allocations = <Pointer<Void>>[];
    Pointer<Void> allocate(int n) {
      final p = _Heap.allocate(n);
      allocations.add(p);
      return p;
    }

    Pointer<Void> text(String s) {
      final p = _string(s);
      allocations.add(p);
      return p;
    }

    try {
      final fields = allocate(count * 120), event = allocate(64);
      var i = 0;
      for (final entry in state.entries) {
        final f = _at(fields, i++ * 120);
        _put64(f, 0, text(entry.key).address);
        _view(f, 16).setInt64(8, entry.value, Endian.little);
      }
      if (report != null) {
        final f = _at(fields, i * 120), data = allocate(report.length);
        data.cast<Uint8>().asTypedList(report.length).setAll(0, report);
        _put64(f, 0, text('report').address);
        _put32(f, 56, 7);
        _put64(f, 104, data.address);
        _put32(f, 112, report.length);
      }
      _put32(event, 0, k);
      _put64(event, 32, fields.address);
      _put32(event, 40, count);
      if (token != null) _put64(event, 24, text(token).address);
      return _checked((e) => _inject(_active, event, 1, e));
    } finally {
      for (final p in allocations.reversed) {
        _Heap.free(p);
      }
    }
  }

  void close() {
    if (_session != 0) {
      final id = _session;
      _session = 0;
      _current = null;
      _header = nullptr;
      _checked((e) => _close(id, e));
    }
  }
}

/// Sendable cancellation capability for a worker isolate. A stale token cannot
/// cancel a later session. The object stores only an immutable path and integer.
final class CancellationToken {
  final String _libraryPath;
  final int _session;
  const CancellationToken._(this._libraryPath, this._session);
  void cancel() {
    final library = DynamicLibrary.open(_libraryPath);
    final invoke = library.lookupFunction<
        Uint32 Function(Uint64, Pointer<Void>),
        int Function(int, Pointer<Void>)>('interceptor_cancel');
    final error = _Heap.allocate(776);
    try {
      final code = invoke(_session, error);
      if (code != 0)
        throw InterceptorException(code, _text(_at(error, 8)), _u32(error, 4));
    } finally {
      _Heap.free(error);
    }
  }
}

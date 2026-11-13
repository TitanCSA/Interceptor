using System.Collections;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace TitanCSA.Products.Interceptor.PowerShell;

public sealed class InterceptorException(uint code, string message, uint accepted = 0) : Exception
(message) {
    public uint Code { get; } = code;
    public uint Accepted { get; } = accepted;
}
public sealed class Field {
    public long Value { get; set; }
    public double Normalized { get; set; }
    public bool Pressed { get; set; }
    public long Minimum { get; internal set; }
    public long Maximum { get; internal set; }
    public long PhysicalMinimum { get; internal set; }
    public long PhysicalMaximum { get; internal set; }
    public bool NormalizedValid { get; internal set; }
    public uint UsagePage { get; internal set; }
    public uint Usage { get; internal set; }
    public uint Collection { get; internal set; }
    public uint ReportId { get; internal set; }
    public uint DataIndex { get; internal set; }
    public uint Element { get; internal set; }
    public uint Flags { get; internal set; }
    public uint BitSize { get; internal set; }
    public uint Units { get; internal set; }
    public uint UnitsExponent { get; internal set; }
    public byte[] Data { get; set; } = [];
}
public sealed class State : Dictionary<string, long> {
    public List<Field> Axes { get; } = [];
    public List<Field> Triggers { get; } = [];
    public List<Field> Hats { get; } = [];
    public new List<Field> Values { get;
}
= [];
public List<Field> Buttons { get; } = [];
public List<Field> BinaryValues { get; } = [];
internal List<Field> Group(uint group) => group switch {
    1 => Axes,
    2 => Triggers,
    3 => Hats,
    4 => Values,
    5 => Buttons,
    6 => BinaryValues,
    _ => throw new ArgumentOutOfRangeException(nameof(group))
};
}
public sealed class Input {
    public string Kind { get; internal set; } = "";
    public string Token { get; internal set; } = "";
    public ulong Sequence { get; internal set; }
    public ulong Timestamp { get; internal set; }
    public uint DecodeError { get; internal set; }
    public bool DecodeComplete { get; internal set; }
    public uint ParserStatus { get; internal set; }
    public bool Blocked { get; set; }
    public State State { get; } = new();
    public byte[] Report { get; set; } = [];
    internal readonly List<(nint Pointer, string? Name, Field? Field)> Edits = [];
}
public sealed class Batch : IReadOnlyList<Input> {
    internal readonly Interceptor Owner;
    internal readonly ulong Ticket;
    internal readonly nint Events, Header;
    private readonly Input[] items;
    internal Batch(Interceptor owner, ulong ticket, nint events, nint header, Input[] inputs) {
        Owner = owner;
        Ticket = ticket;
        Events = events;
        Header = header;
        items = inputs;
    }
    public int Count => items.Length;
    public Input this[int index] => items[index];
    public IEnumerator<Input> GetEnumerator() => ((IEnumerable<Input>)items).GetEnumerator();
    IEnumerator IEnumerable.GetEnumerator() => items.GetEnumerator();
}
public sealed record Device(string Kind, uint Id, ushort Vid, ushort Pid, ushort UsagePage,
                            ushort Usage, string Token, string Name, string HardwareId);
public sealed record Initialization(bool Ready, bool RebootRequired, uint Associated,
                                    uint AlreadyAssociated, uint Skipped);
public sealed record Info(bool Installed, bool Active, uint TrialState, uint InputMask,
                          uint ControllerMask, uint DeviceCount, uint Queued, uint LastReset,
                          ulong RemainingMs, ulong TimestampFrequency, ulong Generation);
public sealed record Descriptor(bool Applicable, uint Format, byte[] Data);

public sealed unsafe class Interceptor : IDisposable {
    private ulong handle, ticket;
    private readonly object sync = new();
    private readonly struct Lease(object sync) : IDisposable {
        public void Dispose() => Monitor.Exit(sync);
    }
    private Lease Enter() {
        if (!Monitor.TryEnter(sync))
            throw new InterceptorException(170, "Interceptor is busy in another operation");
        try {
            Active();
            return new Lease(sync);
        } catch {
            Monitor.Exit(sync);
            throw;
        }
    }
    internal static readonly string[] Kinds =
        ["", "keyboard", "mouse", "hid", "xinput", "xinputhid", "gip"];
    static string Text(nint p) => Marshal.PtrToStringUTF8(p) ?? "";
    static byte[] Bytes(nint p, uint length) {
        var b = new byte[length];
        if (length > 0)
            Marshal.Copy(p, b, 0, b.Length);
        return b;
    }
    static void Check(uint code, ref Native.Error error) {
        if (code != 0) {
            fixed(byte *p = error.Message) throw new InterceptorException(
                code,
                Encoding.UTF8.GetString(MemoryMarshal.CreateReadOnlySpanFromNullTerminated(p)),
                error.Accepted);
        }
    }
    ulong Active() => Volatile.Read(ref handle) != 0
                          ? Volatile.Read(ref handle)
                          : throw new ObjectDisposedException(nameof(Interceptor));
    public Interceptor() {
        if (!OperatingSystem.IsWindows() || !Environment.Is64BitProcess)
            throw new PlatformNotSupportedException("Interceptor requires Windows x64");
        if (Native.interceptor_version() != 1 || Native.interceptor_sizeof(1) != sizeof(Native.Field) ||
            Native.interceptor_sizeof(2) != sizeof(Native.Event) ||
            Native.interceptor_sizeof(5) != sizeof(Native.Error))
            throw new InvalidOperationException("Incompatible bridge ABI");
        var error = new Native.Error();
        Check(Native.interceptor_open(out handle, ref error), ref error);
    }
    public Initialization Initialize() {
        using var lease = Enter();
        var e = new Native.Error();
        Check(Native.interceptor_initialize(Active(), out var r, ref e), ref e);
        return new(r.Ready != 0, r.Reboot != 0, r.Associated, r.Already, r.Skipped);
    }
    public Info GetInfo() {
        using var lease = Enter();
        var e = new Native.Error();
        Check(Native.interceptor_info_get(Active(), out var r, ref e), ref e);
        return new(r.Installed != 0, r.Active != 0, r.Trial, r.InputMask, r.ControllerMask,
                   r.DeviceCount, r.Queued, r.Reset, r.Remaining, r.Frequency, r.Generation);
    }
    public IReadOnlyList<Device> Devices() {
        using var lease = Enter();
        var e = new Native.Error();
        Check(Native.interceptor_devices_get(Active(), out var ptr, ref e), ref e);
        var view = *(Native.Devices *)ptr;
        var result = new Device[view.Count];
        for (uint i = 0; i < view.Count; ++i) {
            var d = ((Native.Device *)view.Items)[i];
            result[i] = new(Kinds[d.Kind], d.Id, d.Vid, d.Pid, d.UsagePage, d.Usage, Text(d.Token),
                            Text(d.Name), Text(d.Hardware));
        }
        return result;
    }
    public Batch Receive(int timeout = -1) {
        using var lease = Enter();
        if (timeout < -1)
            throw new ArgumentOutOfRangeException(nameof(timeout));
        var e = new Native.Error();
        Check(Native.interceptor_receive(Active(), unchecked((uint)timeout), out var pointer, ref e),
              ref e);
        var b = *(Native.Batch *)pointer;
        ticket = b.Id;
        var result = new Input[b.Count];
        for (uint i = 0; i < b.Count; ++i) {
            var n = ((Native.Event *)b.Events)[i];
            var input =
                new Input { Kind = Kinds[n.Kind],         Token = Text(n.Token),
                            Sequence = n.Sequence,        Timestamp = n.Timestamp,
                            DecodeError = n.DecodeError,  DecodeComplete = n.DecodeComplete != 0,
                            ParserStatus = n.ParserStatus };
            result[i] = input;
            for (uint f = 0; f < n.Count; ++f) {
                var p = (Native.Field *)n.Fields + f;
                var cell = *p;
                if (cell.Group == 0) {
                    var name = Text(cell.Name);
                    input.State[name] = cell.Value;
                    input.Edits.Add(((nint)p, name, null));
                } else if (cell.Group == 7) {
                    input.Report = Bytes(cell.Data, cell.Length);
                    input.Edits.Add(((nint)p, "report", null));
                } else {
                    var field = new Field { Value = cell.Value,
                                            Minimum = cell.Minimum,
                                            Maximum = cell.Maximum,
                                            PhysicalMinimum = cell.PhysicalMin,
                                            PhysicalMaximum = cell.PhysicalMax,
                                            Normalized = cell.Normalized,
                                            NormalizedValid = (cell.Flags & 1) != 0,
                                            Pressed = cell.Value != 0,
                                            UsagePage = cell.UsagePage,
                                            Usage = cell.Usage,
                                            Collection = cell.Collection,
                                            ReportId = cell.Report,
                                            DataIndex = cell.DataIndex,
                                            Element = cell.Element,
                                            Flags = cell.Flags,
                                            BitSize = cell.BitSize,
                                            Units = cell.Units,
                                            UnitsExponent = cell.UnitsExponent,
                                            Data = Bytes(cell.Data, cell.Length) };
                    input.State.Group(cell.Group).Add(field);
                    input.Edits.Add(((nint)p, null, field));
                }
            }
        }
        return new(this, b.Id, b.Events, pointer, result);
    }
    void Validate(Batch inputs) {
        Active();
        if (inputs.Owner != this || inputs.Ticket != ticket ||
            ((Native.Batch *)inputs.Header)->Id != inputs.Ticket)
            throw new ArgumentException("Stale or foreign batch");
    }
    public uint Send(Batch inputs) {
        using var lease = Enter();
        Validate(inputs);
        for (int i = 0; i < inputs.Count; ++i) {
            var input = inputs[i];
            ((Native.Event *)inputs.Events)[i].Blocked = input.Blocked ? 1u : 0u;
            if (input.Blocked)
                continue;
            foreach (var edit in input.Edits) {
                var f = (Native.Field *)edit.Pointer;
                byte[]? data = null;
                if (edit.Name == "report")
                    data = input.Report;
                else if (edit.Name != null)
                    f->Value = input.State[edit.Name];
                else {
                    var v = edit.Field!;
                    f->Value = f->Group == 5 ? (v.Pressed ? 1 : 0) : v.Value;
                    f->Normalized = v.Normalized;
                    data = v.Data;
                }
                if (data != null) {
                    if (data.Length != f->Length)
                        throw new ArgumentException("Input data length is immutable");
                    if (data.Length > 0)
                        Marshal.Copy(data, 0, f->Data, data.Length);
                }
            }
        }
        var e = new Native.Error();
        Check(Native.interceptor_send(handle, inputs.Ticket, ref e), ref e);
        return e.Accepted;
    }
    public void Discard(Batch inputs) {
        using var lease = Enter();
        Validate(inputs);
        var e = new Native.Error();
        Check(Native.interceptor_discard(handle, inputs.Ticket, ref e), ref e);
    }
    public void Cancel() {
        var e = new Native.Error();
        Check(Native.interceptor_cancel(Active(), ref e), ref e);
    }
    public Descriptor GetDescriptor(string token, bool report = false) {
        using var lease = Enter();
        var e = new Native.Error();
        Check(Native.interceptor_descriptor(Active(), token, report ? 1u : 0u, out var b, ref e), ref e);
        return new(b.Applicable != 0, b.Format, Bytes(b.Data, b.Length));
    }
    public uint Inject(string kind, IReadOnlyDictionary<string, long> state, string? token = null,
                       byte[]? report = null) {
        using var lease = Enter();
        int k = Array.IndexOf(Kinds, kind);
        if (k < 1)
            throw new ArgumentException("Unknown input kind");
        var allocations = new List<nint>();
        var fields = new Native.Field[state.Count + (report == null ? 0 : 1)];
        GCHandle pin = default;
        try {
            int n = 0;
            foreach (var pair in state) {
                var p = Marshal.StringToCoTaskMemUTF8(pair.Key);
                allocations.Add(p);
                fields[n++] = new Native.Field { Name = p, Value = pair.Value };
            }
            if (report != null) {
                var name = Marshal.StringToCoTaskMemUTF8("report");
                allocations.Add(name);
                pin = GCHandle.Alloc(report, GCHandleType.Pinned);
                fields[n] =
                    new Native.Field { Name = name, Group = 7, Data = pin.AddrOfPinnedObject(),
                                       Length = (uint)report.Length };
            }
            var tp = token == null ? 0 : Marshal.StringToCoTaskMemUTF8(token);
            if (tp != 0)
                allocations.Add(tp);
            fixed(Native.Field *f = fields) {
                var input = new Native.Event { Kind = (uint)k, Token = tp, Fields = (nint)f,
                                               Count = (uint)fields.Length };
                var e = new Native.Error();
                Check(Native.interceptor_inject(Active(), ref input, 1, ref e), ref e);
                return e.Accepted;
            }
        } finally {
            if (pin.IsAllocated)
                pin.Free();
            foreach (var p in allocations)
                Marshal.FreeCoTaskMem(p);
        }
    }
    public void Dispose() {
        var id = Volatile.Read(ref handle);
        if (id != 0) {
            var error = new Native.Error();
            Native.interceptor_cancel(id, ref error);
            lock (sync) {
                id = Interlocked.Exchange(ref handle, 0);
                ticket = 0;
                if (id != 0)
                    Check(Native.interceptor_close(id, ref error), ref error);
            }
        }
        GC.SuppressFinalize(this);
    }
    ~Interceptor() {
        try {
            Dispose();
        } catch {
        }
    }
}
internal static unsafe class Native {
    const string Library = "InterceptorSDKBridge";
    static Native() {
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, Resolve);
    }
    static nint Resolve(string name, Assembly assembly, DllImportSearchPath? search) {
        if (name != Library)
            return 0;
        var explicitPath = Environment.GetEnvironmentVariable("INTERCEPTOR_SDK_NATIVE_PATH");
        if (explicitPath != null)
            return NativeLibrary.Load(Path.GetFullPath(explicitPath));
        var bundled = Path.Combine(Path.GetDirectoryName(assembly.Location)!, "InterceptorSDKBridge.dll");
        if (File.Exists(bundled)) return NativeLibrary.Load(bundled);
        var p = Path.Combine(AppContext.BaseDirectory, "InterceptorSDKBridge.dll");
        if (File.Exists(p))
            return NativeLibrary.Load(p);
        p = Path.Combine(AppContext.BaseDirectory, "runtimes", "win-x64", "native", "InterceptorSDKBridge.dll");
        if (File.Exists(p))
            return NativeLibrary.Load(p);
        throw new DllNotFoundException(
            "Place InterceptorSDKBridge.dll beside your EXE, or use the complete release package");
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Error {
        public uint Code, Accepted;
        public fixed byte Message[768];
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Field {
        public nint Name;
        public long Value, Minimum, Maximum, PhysicalMin, PhysicalMax;
        public double Normalized;
        public uint Group, Index, UsagePage, Usage, Collection, Report, DataIndex, Element, Flags,
            Units, UnitsExponent, BitSize;
        public nint Data;
        public uint Length, Reserved;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Event {
        public uint Kind, Blocked;
        public ulong Sequence, Timestamp;
        public nint Token, Fields;
        public uint Count, DecodeError, DecodeComplete, ParserStatus, Device, Reserved;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Batch {
        public ulong Id;
        public uint Count, Reserved;
        public nint Events;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Device {
        public uint Kind, Id;
        public ushort Vid, Pid, UsagePage, Usage;
        public nint Token, Name, Hardware;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Devices {
        public uint Count, Reserved;
        public nint Items;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Blob {
        public uint Applicable, Format, Length, Reserved;
        public nint Data;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Init {
        public uint Ready, Reboot, Associated, Already, Skipped, Reserved;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Info {
        public uint Installed, Active, Trial, InputMask, ControllerMask, DeviceCount, Queued, Reset;
        public ulong Remaining, Frequency, Generation;
    }
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint interceptor_version();
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint interceptor_sizeof(uint kind);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint interceptor_open(out ulong handle, ref Error e);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint interceptor_close(ulong handle, ref Error e);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint interceptor_cancel(ulong handle, ref Error e);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint interceptor_initialize(ulong handle, out Init result, ref Error e);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint interceptor_info_get(ulong handle, out Info result, ref Error e);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint interceptor_devices_get(ulong handle, out nint result, ref Error e);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint interceptor_receive(ulong handle, uint timeout, out nint result,
                                            ref Error e);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint interceptor_send(ulong handle, ulong ticket, ref Error e);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint interceptor_discard(ulong handle, ulong ticket, ref Error e);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint interceptor_descriptor(ulong handle,
                                               [MarshalAs(UnmanagedType.LPUTF8Str)] string token,
                                               uint report, out Blob result, ref Error e);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint interceptor_inject(ulong handle, ref Event inputs, uint count, ref Error e);
}

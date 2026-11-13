package titancsa.products.interceptor;

import static java.lang.foreign.ValueLayout.*;

import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicLong;

/** Java 22+ FFM binding. No JNI, Maven dependency or polling thread. */
public final class Interceptor implements AutoCloseable {
    public static final String[] KINDS = {"",       "keyboard",  "mouse", "hid",
                                          "xinput", "xinputhid", "gip"};
    private static final String[] GROUPS = {"state",  "axes",    "triggers",      "hats",
                                            "values", "buttons", "binary_values", "report"};
    public static final class Error extends RuntimeException {
        public final int code;
        public final long accepted;
        Error(int code, long accepted, String message) {
            super(message);
            this.code = code;
            this.accepted = accepted;
        }
    }
    public static final class Field {
        public long value;
        public double normalized;
        public boolean pressed;
        public final long minimum, maximum, physicalMinimum, physicalMaximum;
        public final int usagePage, usage, collection, reportId, dataIndex, element, flags, units,
            unitsExponent, bitSize;
        public final boolean normalizedValid;
        public byte[] data;
        private final MemorySegment nativeField;
        Field(MemorySegment p) {
            nativeField = p;
            value = p.get(JAVA_LONG, 8);
            minimum = p.get(JAVA_LONG, 16);
            maximum = p.get(JAVA_LONG, 24);
            physicalMinimum = p.get(JAVA_LONG, 32);
            physicalMaximum = p.get(JAVA_LONG, 40);
            normalized = p.get(JAVA_DOUBLE, 48);
            usagePage = p.get(JAVA_INT, 64);
            usage = p.get(JAVA_INT, 68);
            collection = p.get(JAVA_INT, 72);
            reportId = p.get(JAVA_INT, 76);
            dataIndex = p.get(JAVA_INT, 80);
            element = p.get(JAVA_INT, 84);
            flags = p.get(JAVA_INT, 88);
            units = p.get(JAVA_INT, 92);
            unitsExponent = p.get(JAVA_INT, 96);
            bitSize = p.get(JAVA_INT, 100);
            normalizedValid = (flags & 1) != 0;
            pressed = value != 0;
            data = bytes(p.get(ADDRESS, 104), p.get(JAVA_INT, 112));
        }
    }
    public static final class State {
        public final Map<String, Long> scalars = new LinkedHashMap<>();
        public final List<Field> axes = new ArrayList<>(), triggers = new ArrayList<>(),
                                 hats = new ArrayList<>(), values = new ArrayList<>(),
                                 buttons = new ArrayList<>(), binaryValues = new ArrayList<>();
        List<Field> group(int n) {
            return switch (n) {
                case 1 -> axes;
                case 2 -> triggers;
                case 3 -> hats;
                case 4 -> values;
                case 5 -> buttons;
                case 6 -> binaryValues;
                default -> throw new IllegalArgumentException("Invalid field group");
            };
        }
    }
    private record Edit(MemorySegment pointer, String name, Field field) {}
    public static final class Input {
        public final String kind, token;
        public final long sequence, timestamp;
        public final int decodeError, parserStatus;
        public final boolean decodeComplete;
        public final State state = new State();
        public byte[] report = new byte[0];
        public boolean blocked;
        private final MemorySegment nativeEvent;
        private final List<Edit> edits = new ArrayList<>();
        Input(MemorySegment p) {
            nativeEvent = p;
            kind = KINDS[p.get(JAVA_INT, 0)];
            token = text(p.get(ADDRESS, 24));
            sequence = p.get(JAVA_LONG, 8);
            timestamp = p.get(JAVA_LONG, 16);
            decodeError = p.get(JAVA_INT, 44);
            decodeComplete = p.get(JAVA_INT, 48) != 0;
            parserStatus = p.get(JAVA_INT, 52);
            int count = p.get(JAVA_INT, 40);
            MemorySegment fields = p.get(ADDRESS, 32).reinterpret((long)count * 120);
            for (int n = 0; n < count; ++n) {
                MemorySegment f = fields.asSlice((long)n * 120, 120);
                int group = f.get(JAVA_INT, 56);
                if (group == 0) {
                    String name = text(f.get(ADDRESS, 0));
                    state.scalars.put(name, f.get(JAVA_LONG, 8));
                    edits.add(new Edit(f, name, null));
                } else if (group == 7) {
                    report = bytes(f.get(ADDRESS, 104), f.get(JAVA_INT, 112));
                    edits.add(new Edit(f, "report", null));
                } else {
                    Field field = new Field(f);
                    state.group(group).add(field);
                    edits.add(new Edit(f, null, field));
                }
            }
        }
        void commit() {
            nativeEvent.set(JAVA_INT, 4, blocked ? 1 : 0);
            if (blocked)
                return;
            for (Edit edit : edits) {
                MemorySegment p = edit.pointer;
                byte[] data = null;
                if ("report".equals(edit.name))
                    data = report;
                else if (edit.name != null) {
                    Long value = state.scalars.get(edit.name);
                    if (value == null)
                        throw new IllegalArgumentException("Missing state field " + edit.name);
                    p.set(JAVA_LONG, 8, value);
                } else {
                    Field f = edit.field;
                    p.set(JAVA_LONG, 8, p.get(JAVA_INT, 56) == 5 ? (f.pressed ? 1L : 0L) : f.value);
                    p.set(JAVA_DOUBLE, 48, f.normalized);
                    data = f.data;
                }
                if (data != null) {
                    int length = p.get(JAVA_INT, 112);
                    if (data.length != length)
                        throw new IllegalArgumentException("Input data length is immutable");
                    if (length > 0)
                        MemorySegment.copy(MemorySegment.ofArray(data), 0,
                                           p.get(ADDRESS, 104).reinterpret(length), 0, length);
                }
            }
        }
    }
    public static final class Batch extends AbstractList<Input> {
        private final Interceptor owner;
        private final long ticket;
        private final List<Input> items;
        private final MemorySegment header;
        Batch(Interceptor owner, MemorySegment pointer) {
            this.owner = owner;
            MemorySegment b = pointer.reinterpret(24);
            header = b;
            ticket = b.get(JAVA_LONG, 0);
            int n = b.get(JAVA_INT, 8);
            MemorySegment events = b.get(ADDRESS, 16).reinterpret((long)n * 64);
            var result = new ArrayList<Input>();
            for (int i = 0; i < n; ++i)
                result.add(new Input(events.asSlice((long)i * 64, 64)));
            items = List.copyOf(result);
        }
        public Input get(int n) {
            return items.get(n);
        }
        public int size() {
            return items.size();
        }
    }
    public record Device(String kind, int id, int vid, int pid, int usagePage, int usage,
                         String token, String name, String hardwareId) {}
    public record Initialization(boolean ready, boolean rebootRequired, long associated,
                                 long alreadyAssociated, long skipped) {}
    public record Info(boolean installed, boolean active, int trialState, int inputMask,
                       int controllerMask, int deviceCount, int queued, int lastReset,
                       long remainingMs, long timestampFrequency, long generation) {}
    public record Descriptor(boolean applicable, int format, byte[] data) {}
    private static String text(MemorySegment p) {
        return p.address() == 0 ? "" : p.reinterpret(8192).getString(0);
    }
    private static byte[] bytes(MemorySegment p, int n) {
        return n == 0 ? new byte[0] : p.reinterpret(n).toArray(JAVA_BYTE);
    }
    private static final Linker LINKER = Linker.nativeLinker();
    private final Map<String, MethodHandle> functions = new HashMap<>();
    private final AtomicLong session = new AtomicLong();
    private long ticket;
    public Interceptor() {
        this(null);
    }
    public Interceptor(Path library) {
        if (!System.getProperty("os.name").startsWith("Windows") || ADDRESS.byteSize() != 8)
            throw new UnsupportedOperationException("Interceptor requires Windows x64");
        if (library == null) {
            String env = System.getenv("INTERCEPTOR_SDK_NATIVE_PATH");
            if (env != null)
                library = Path.of(env);
        }
        if (library == null) {
            try {
                Path location = Path.of(
                    Interceptor.class.getProtectionDomain().getCodeSource().getLocation().toURI());
                Path dir = Files.isDirectory(location) ? location : location.getParent();
                for (Path p :
                     List.of(dir.resolve("InterceptorSDKBridge.dll"),
                             dir.resolve("../../runtimes/win-x64/InterceptorSDKBridge.dll"))) {
                    if (Files.isRegularFile(p)) {
                        library = p;
                        break;
                    }
                }
            } catch (Exception e) {
                throw new IllegalStateException(e);
            }
        }
        if (library == null) {
            try (var resource = Interceptor.class.getResourceAsStream("/native/interceptor/win-x64/InterceptorSDKBridge.dll")) {
                if (resource != null) {
                    Path directory = Files.createTempDirectory("titancsa-interceptor-");
                    directory.toFile().deleteOnExit();
                    library = directory.resolve("InterceptorSDKBridge.dll");
                    Files.copy(resource, library);
                    library.toFile().deleteOnExit();
                }
            } catch (java.io.IOException e) { throw new IllegalStateException("Cannot extract packaged native library", e); }
        }
        if (library == null)
            throw new IllegalStateException("InterceptorSDKBridge.dll not found; use the complete " +
                                            "release or pass its absolute path");
        SymbolLookup lookup = SymbolLookup.libraryLookup(library.toAbsolutePath(), Arena.global());
        bind(lookup, "version");
        bind(lookup, "sizeof", JAVA_INT);
        bind(lookup, "open", ADDRESS, ADDRESS);
        bind(lookup, "close", JAVA_LONG, ADDRESS);
        bind(lookup, "cancel", JAVA_LONG, ADDRESS);
        bind(lookup, "initialize", JAVA_LONG, ADDRESS, ADDRESS);
        bind(lookup, "info_get", JAVA_LONG, ADDRESS, ADDRESS);
        bind(lookup, "devices_get", JAVA_LONG, ADDRESS, ADDRESS);
        bind(lookup, "receive", JAVA_LONG, JAVA_INT, ADDRESS, ADDRESS);
        bind(lookup, "send", JAVA_LONG, JAVA_LONG, ADDRESS);
        bind(lookup, "discard", JAVA_LONG, JAVA_LONG, ADDRESS);
        bind(lookup, "descriptor", JAVA_LONG, ADDRESS, JAVA_INT, ADDRESS, ADDRESS);
        bind(lookup, "inject", JAVA_LONG, ADDRESS, JAVA_INT, ADDRESS);
        if (raw("version") != 1 || raw("sizeof", 1) != 120 || raw("sizeof", 2) != 64 ||
            raw("sizeof", 5) != 776)
            throw new IllegalStateException("Incompatible native ABI");
        try (Arena a = Arena.ofConfined()) {
            MemorySegment out = a.allocate(JAVA_LONG);
            call(a, "open", out);
            session.set(out.get(JAVA_LONG, 0));
        }
    }
    private void bind(SymbolLookup lookup, String name, MemoryLayout... args) {
        functions.put(name, LINKER.downcallHandle(lookup.find("interceptor_" + name).orElseThrow(),
                                                  FunctionDescriptor.of(JAVA_INT, args)));
    }
    private int raw(String name, Object... args) {
        try {
            return (int)functions.get(name).invokeWithArguments(args);
        } catch (Throwable e) {
            throw new IllegalStateException("Native call failed: " + name, e);
        }
    }
    private long call(Arena arena, String name, Object... args) {
        MemorySegment error = arena.allocate(776, 8);
        Object[] all = Arrays.copyOf(args, args.length + 1);
        all[args.length] = error;
        int code = raw(name, all);
        if (code != 0)
            throw new Error(code, Integer.toUnsignedLong(error.get(JAVA_INT, 4)),
                            error.asSlice(8, 768).getString(0));
        return Integer.toUnsignedLong(error.get(JAVA_INT, 4));
    }
    private long active() {
        long id = session.get();
        if (id == 0)
            throw new IllegalStateException("Interceptor is closed");
        return id;
    }
    public synchronized Initialization initialize() {
        try (Arena a = Arena.ofConfined()) {
            MemorySegment out = a.allocate(24, 4);
            call(a, "initialize", active(), out);
            return new Initialization(out.get(JAVA_INT, 0) != 0, out.get(JAVA_INT, 4) != 0,
                                      Integer.toUnsignedLong(out.get(JAVA_INT, 8)),
                                      Integer.toUnsignedLong(out.get(JAVA_INT, 12)),
                                      Integer.toUnsignedLong(out.get(JAVA_INT, 16)));
        }
    }
    public synchronized Info info() {
        try (Arena a = Arena.ofConfined()) {
            MemorySegment p = a.allocate(56, 8);
            call(a, "info_get", active(), p);
            return new Info(p.get(JAVA_INT, 0) != 0, p.get(JAVA_INT, 4) != 0, p.get(JAVA_INT, 8),
                            p.get(JAVA_INT, 12), p.get(JAVA_INT, 16), p.get(JAVA_INT, 20),
                            p.get(JAVA_INT, 24), p.get(JAVA_INT, 28), p.get(JAVA_LONG, 32),
                            p.get(JAVA_LONG, 40), p.get(JAVA_LONG, 48));
        }
    }
    public synchronized List<Device> devices() {
        try (Arena a = Arena.ofConfined()) {
            MemorySegment out = a.allocate(ADDRESS);
            call(a, "devices_get", active(), out);
            MemorySegment view = out.get(ADDRESS, 0).reinterpret(16);
            int count = view.get(JAVA_INT, 0);
            MemorySegment items = view.get(ADDRESS, 8).reinterpret((long)count * 40);
            var result = new ArrayList<Device>();
            for (int n = 0; n < count; ++n) {
                MemorySegment d = items.asSlice((long)n * 40, 40);
                result.add(new Device(KINDS[d.get(JAVA_INT, 0)], d.get(JAVA_INT, 4),
                                      Short.toUnsignedInt(d.get(JAVA_SHORT, 8)),
                                      Short.toUnsignedInt(d.get(JAVA_SHORT, 10)),
                                      Short.toUnsignedInt(d.get(JAVA_SHORT, 12)),
                                      Short.toUnsignedInt(d.get(JAVA_SHORT, 14)),
                                      text(d.get(ADDRESS, 16)), text(d.get(ADDRESS, 24)),
                                      text(d.get(ADDRESS, 32))));
            }
            return List.copyOf(result);
        }
    }
    public synchronized Batch receive() {
        return receive(-1);
    }
    public synchronized Batch receive(int timeout) {
        if (timeout < -1)
            throw new IllegalArgumentException("Invalid timeout");
        try (Arena a = Arena.ofConfined()) {
            MemorySegment out = a.allocate(ADDRESS);
            call(a, "receive", active(), timeout, out);
            Batch result = new Batch(this, out.get(ADDRESS, 0));
            ticket = result.ticket;
            return result;
        }
    }
    private void validate(Batch b) {
        active();
        if (b.owner != this || b.ticket != ticket || b.header.get(JAVA_LONG, 0) != b.ticket)
            throw new IllegalArgumentException("Stale or foreign input batch");
    }
    public synchronized long send(Batch batch) {
        validate(batch);
        for (Input input : batch)
            input.commit();
        try (Arena a = Arena.ofConfined()) {
            return call(a, "send", active(), batch.ticket);
        }
    }
    public synchronized void discard(Batch batch) {
        validate(batch);
        try (Arena a = Arena.ofConfined()) {
            call(a, "discard", active(), batch.ticket);
        }
    }
    public synchronized Descriptor descriptor(String token, boolean report) {
        try (Arena a = Arena.ofConfined()) {
            MemorySegment out = a.allocate(24, 8);
            call(a, "descriptor", active(), a.allocateFrom(token), report ? 1 : 0, out);
            return new Descriptor(out.get(JAVA_INT, 0) != 0, out.get(JAVA_INT, 4),
                                  bytes(out.get(ADDRESS, 16), out.get(JAVA_INT, 8)));
        }
    }
    public synchronized long inject(String kind, Map<String, Long> state) {
        return inject(kind, state, null, null);
    }
    public synchronized long inject(String kind, Map<String, Long> state, String token,
                                    byte[] report) {
        int k = Arrays.asList(KINDS).indexOf(kind);
        if (k < 1)
            throw new IllegalArgumentException("Unknown input kind");
        try (Arena a = Arena.ofConfined()) {
            MemorySegment event = a.allocate(64, 8);
            event.set(JAVA_INT, 0, k);
            if (token != null)
                event.set(ADDRESS, 24, a.allocateFrom(token));
            int count = state.size() + (report == null ? 0 : 1);
            MemorySegment fields = a.allocate((long)Math.max(count, 1) * 120, 8);
            int n = 0;
            for (var entry : state.entrySet()) {
                long offset = (long)n++ * 120;
                fields.set(ADDRESS, offset, a.allocateFrom(entry.getKey()));
                fields.set(JAVA_LONG, offset + 8, entry.getValue());
            }
            if (report != null) {
                long offset = (long)n * 120;
                fields.set(ADDRESS, offset, a.allocateFrom("report"));
                fields.set(JAVA_INT, offset + 56, 7);
                MemorySegment data = a.allocateFrom(JAVA_BYTE, report);
                fields.set(ADDRESS, offset + 104, data);
                fields.set(JAVA_INT, offset + 112, report.length);
            }
            event.set(ADDRESS, 32, fields);
            event.set(JAVA_INT, 40, count);
            return call(a, "inject", active(), event, 1);
        }
    }
    public void cancel() {
        try (Arena a = Arena.ofConfined()) {
            call(a, "cancel", active());
        }
    }
    @Override
    public void close() {
        long id = session.get();
        if (id == 0)
            return;
        try {
            cancel();
        } catch (Error ignored) {
        }
        synchronized (this) {
            id = session.getAndSet(0);
            ticket = 0;
            if (id != 0)
                try (Arena a = Arena.ofConfined()) {
                    call(a, "close", id);
                }
        }
    }
}

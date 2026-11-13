//go:build windows && amd64

// Package interceptor wraps the Windows x64 Interceptor driver without cgo.
package interceptor_sdk

import (
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"sync"
	"sync/atomic"
	"syscall"
	"unsafe"
)

type Error struct {
	Code, Accepted uint32
	Message        string
}

func (e *Error) Error() string { return e.Message }

type nativeError struct {
	Code, Accepted uint32
	Message        [768]byte
}
type nativeField struct {
	Name                                                                                                           uintptr
	Value, Minimum, Maximum, PhysicalMinimum, PhysicalMaximum                                                      int64
	Normalized                                                                                                     float64
	Group, Index, UsagePage, Usage, Collection, ReportID, DataIndex, Element, Flags, Units, UnitsExponent, BitSize uint32
	Data                                                                                                           uintptr
	Length, Reserved                                                                                               uint32
}
type nativeEvent struct {
	Kind, Blocked                                                      uint32
	Sequence, Timestamp                                                uint64
	Token, Fields                                                      uintptr
	Count, DecodeError, DecodeComplete, ParserStatus, Device, Reserved uint32
}
type nativeBatch struct {
	ID              uint64
	Count, Reserved uint32
	Events          uintptr
}
type nativeDevice struct {
	Kind, ID                   uint32
	VID, PID, UsagePage, Usage uint16
	Token, Name, Hardware      uintptr
}
type nativeDevices struct {
	Count, Reserved uint32
	Items           uintptr
}
type nativeBlob struct {
	Applicable, Format, Length, Reserved uint32
	Data                                 uintptr
}
type Initialization struct{ Ready, RebootRequired, Associated, AlreadyAssociated, Skipped, Reserved uint32 }
type Info struct {
	Installed, Active, TrialState, InputMask, ControllerMask, DeviceCount, Queued, LastReset uint32
	RemainingMS, TimestampFrequency, Generation                                              uint64
}
type Device struct {
	Kind, Token, Name, HardwareID string
	ID                            uint32
	VID, PID, UsagePage, Usage    uint16
}
type Injection struct {
	Token  string
	Report []byte
}
type Descriptor struct {
	Applicable bool
	Format     uint32
	Data       []byte
}
type Field struct {
	Value, Minimum, Maximum, PhysicalMinimum, PhysicalMaximum                                        int64
	Normalized                                                                                       float64
	NormalizedValid, Pressed                                                                         bool
	UsagePage, Usage, Collection, ReportID, DataIndex, Element, Flags, Units, UnitsExponent, BitSize uint32
	Data                                                                                             []byte
}
type HIDState struct{ Axes, Triggers, Hats, Values, Buttons, BinaryValues []*Field }

func (h *HIDState) group(n uint32) *[]*Field {
	switch n {
	case 1:
		return &h.Axes
	case 2:
		return &h.Triggers
	case 3:
		return &h.Hats
	case 4:
		return &h.Values
	case 5:
		return &h.Buttons
	case 6:
		return &h.BinaryValues
	}
	panic("Invalid field group")
}

type edit struct {
	pointer uintptr
	name    string
	field   *Field
	report  bool
}
type Input struct {
	Kind, Token               string
	Sequence, Timestamp       uint64
	State                     map[string]int64
	HID                       HIDState
	Report                    []byte
	Blocked                   bool
	DecodeError, ParserStatus uint32
	DecodeComplete            bool
	edits                     []edit
}
type Batch struct {
	Inputs         []*Input
	owner          *Interceptor
	ticket         uint64
	events, header uintptr
	original       []*Input
}
type Interceptor struct {
	mu     sync.Mutex
	handle atomic.Uint64
	dll    *syscall.DLL
	procs  map[string]*syscall.Proc
	ticket uint64
}

var kinds = []string{"", "keyboard", "mouse", "hid", "xinput", "xinputhid", "gip"}
var dlls = struct {
	sync.Mutex
	items map[string]*syscall.DLL
}{items: map[string]*syscall.DLL{}}

func text(pointer uintptr) string {
	if pointer == 0 {
		return ""
	}
	var b []byte
	for n := uintptr(0); n < 8192; n++ {
		c := *(*byte)(unsafe.Pointer(pointer + n))
		if c == 0 {
			return string(b)
		}
		b = append(b, c)
	}
	panic("Invalid native string")
}
func data(pointer uintptr, n uint32) []byte {
	if n == 0 {
		return []byte{}
	}
	return append([]byte(nil), unsafe.Slice((*byte)(unsafe.Pointer(pointer)), n)...)
}
func New(library ...string) (*Interceptor, error) {
	if runtime.GOOS != "windows" || unsafe.Sizeof(uintptr(0)) != 8 {
		return nil, errors.New("Interceptor requires Windows x64")
	}
	var selected string
	if len(library) > 0 {
		selected = library[0]
	} else {
		selected = os.Getenv("INTERCEPTOR_SDK_NATIVE_PATH")
	}
	if selected == "" {
		var err error
		selected, err = bundledLibrary()
		if err != nil {
			return nil, err
		}
	}
	path, e := filepath.Abs(selected)
	if e != nil {
		return nil, e
	}
	dlls.Lock()
	dll := dlls.items[path]
	if dll == nil {
		dll, e = syscall.LoadDLL(path)
		if e == nil {
			dlls.items[path] = dll
		}
	}
	dlls.Unlock()
	if e != nil {
		return nil, e
	}
	io := &Interceptor{dll: dll, procs: map[string]*syscall.Proc{}}
	for _, name := range []string{"version", "sizeof", "open", "close", "cancel", "initialize", "info_get", "devices_get", "receive", "send", "discard", "descriptor", "inject"} {
		p, e := dll.FindProc("interceptor_" + name)
		if e != nil {
			return nil, e
		}
		io.procs[name] = p
	}
	version, _, _ := io.procs["version"].Call()
	if version != 1 {
		return nil, errors.New("Incompatible bridge ABI")
	}
	for i, size := range []uintptr{unsafe.Sizeof(nativeField{}), unsafe.Sizeof(nativeEvent{}), unsafe.Sizeof(nativeBatch{}), unsafe.Sizeof(nativeDevice{}), unsafe.Sizeof(nativeError{}), unsafe.Sizeof(Info{})} {
		n, _, _ := io.procs["sizeof"].Call(uintptr(i + 1))
		if n != size {
			return nil, errors.New("Incompatible native layout")
		}
	}
	var id uint64
	_, e = io.call("open", []any{&id}, uintptr(unsafe.Pointer(&id)))
	if e != nil {
		return nil, e
	}
	io.handle.Store(id)
	runtime.SetFinalizer(io, func(p *Interceptor) { _ = p.Close() })
	return io, nil
}
func (io *Interceptor) call(name string, keep []any, args ...uintptr) (uint32, error) {
	var result nativeError
	var pins runtime.Pinner
	pins.Pin(&result)
	for _, p := range keep {
		pins.Pin(p)
	}
	defer pins.Unpin()
	args = append(args, uintptr(unsafe.Pointer(&result)))
	code, _, _ := io.procs[name].Call(args...)
	runtime.KeepAlive(keep)
	runtime.KeepAlive(io)
	if code != 0 {
		n := 0
		for n < len(result.Message) && result.Message[n] != 0 {
			n++
		}
		return result.Accepted, &Error{uint32(code), result.Accepted, string(result.Message[:n])}
	}
	return result.Accepted, nil
}
func (io *Interceptor) enter() error {
	if !io.mu.TryLock() {
		return &Error{Code: 170, Message: "Interceptor is busy in another operation"}
	}
	if io.handle.Load() == 0 {
		io.mu.Unlock()
		return errors.New("Interceptor is closed")
	}
	return nil
}
func (io *Interceptor) Initialize() (Initialization, error) {
	var out Initialization
	if e := io.enter(); e != nil {
		return out, e
	}
	defer io.mu.Unlock()
	_, e := io.call("initialize", []any{&out}, uintptr(io.handle.Load()), uintptr(unsafe.Pointer(&out)))
	return out, e
}
func (io *Interceptor) Info() (Info, error) {
	var out Info
	if e := io.enter(); e != nil {
		return out, e
	}
	defer io.mu.Unlock()
	_, e := io.call("info_get", []any{&out}, uintptr(io.handle.Load()), uintptr(unsafe.Pointer(&out)))
	return out, e
}
func (io *Interceptor) Devices() ([]Device, error) {
	if e := io.enter(); e != nil {
		return nil, e
	}
	defer io.mu.Unlock()
	var ptr uintptr
	_, e := io.call("devices_get", []any{&ptr}, uintptr(io.handle.Load()), uintptr(unsafe.Pointer(&ptr)))
	if e != nil {
		return nil, e
	}
	view := *(*nativeDevices)(unsafe.Pointer(ptr))
	result := make([]Device, view.Count)
	for n, d := range unsafe.Slice((*nativeDevice)(unsafe.Pointer(view.Items)), view.Count) {
		result[n] = Device{kinds[d.Kind], text(d.Token), text(d.Name), text(d.Hardware), d.ID, d.VID, d.PID, d.UsagePage, d.Usage}
	}
	return result, nil
}

// Receive waits in milliseconds. -1 waits indefinitely; 0 checks immediately.
func (io *Interceptor) Receive(timeout int) (*Batch, error) {
	if timeout < -1 || int64(timeout) > math.MaxUint32-1 {
		return nil, errors.New("Invalid timeout")
	}
	if e := io.enter(); e != nil {
		return nil, e
	}
	defer io.mu.Unlock()
	var ptr uintptr
	_, err := io.call("receive", []any{&ptr}, uintptr(io.handle.Load()), uintptr(uint32(timeout)), uintptr(unsafe.Pointer(&ptr)))
	if err != nil {
		return nil, err
	}
	view := *(*nativeBatch)(unsafe.Pointer(ptr))
	io.ticket = view.ID
	batch := &Batch{Inputs: make([]*Input, view.Count), owner: io, ticket: view.ID, events: view.Events, header: ptr}
	for n, event := range unsafe.Slice((*nativeEvent)(unsafe.Pointer(view.Events)), view.Count) {
		input := &Input{Kind: kinds[event.Kind], Token: text(event.Token), Sequence: event.Sequence, Timestamp: event.Timestamp, State: map[string]int64{}, DecodeError: event.DecodeError, DecodeComplete: event.DecodeComplete != 0, ParserStatus: event.ParserStatus}
		batch.Inputs[n] = input
		for f := uint32(0); f < event.Count; f++ {
			p := event.Fields + uintptr(f)*unsafe.Sizeof(nativeField{})
			cell := *(*nativeField)(unsafe.Pointer(p))
			if cell.Group == 0 {
				name := text(cell.Name)
				input.State[name] = cell.Value
				input.edits = append(input.edits, edit{pointer: p, name: name})
			} else if cell.Group == 7 {
				input.Report = data(cell.Data, cell.Length)
				input.edits = append(input.edits, edit{pointer: p, report: true})
			} else {
				item := &Field{Value: cell.Value, Minimum: cell.Minimum, Maximum: cell.Maximum, PhysicalMinimum: cell.PhysicalMinimum, PhysicalMaximum: cell.PhysicalMaximum, Normalized: cell.Normalized, NormalizedValid: cell.Flags&1 != 0, Pressed: cell.Value != 0, UsagePage: cell.UsagePage, Usage: cell.Usage, Collection: cell.Collection, ReportID: cell.ReportID, DataIndex: cell.DataIndex, Element: cell.Element, Flags: cell.Flags, Units: cell.Units, UnitsExponent: cell.UnitsExponent, BitSize: cell.BitSize, Data: data(cell.Data, cell.Length)}
				list := input.HID.group(cell.Group)
				*list = append(*list, item)
				input.edits = append(input.edits, edit{pointer: p, field: item})
			}
		}
	}
	batch.original = append([]*Input(nil), batch.Inputs...)
	return batch, nil
}
func (io *Interceptor) valid(batch *Batch) error {
	if batch == nil || batch.owner != io || batch.ticket != io.ticket || (*nativeBatch)(unsafe.Pointer(batch.header)).ID != batch.ticket {
		return errors.New("Stale or foreign batch")
	}
	return nil
}
func (io *Interceptor) Send(batch *Batch) (uint32, error) {
	if e := io.enter(); e != nil {
		return 0, e
	}
	defer io.mu.Unlock()
	if e := io.valid(batch); e != nil {
		return 0, e
	}
	if len(batch.Inputs) != len(batch.original) {
		return 0, errors.New("Do not resize an input batch")
	}
	for n, input := range batch.Inputs {
		if input != batch.original[n] {
			return 0, errors.New("Do not reorder an input batch")
		}
	}
	for n, input := range batch.Inputs {
		event := (*nativeEvent)(unsafe.Pointer(batch.events + uintptr(n)*unsafe.Sizeof(nativeEvent{})))
		if input.Blocked {
			event.Blocked = 1
		} else {
			event.Blocked = 0
		}
		if input.Blocked {
			continue
		}
		for _, edit := range input.edits {
			cell := (*nativeField)(unsafe.Pointer(edit.pointer))
			var bytes []byte
			if edit.report {
				bytes = input.Report
			} else if edit.field == nil {
				value, ok := input.State[edit.name]
				if !ok {
					return 0, fmt.Errorf("Missing state field %s", edit.name)
				}
				cell.Value = value
			} else {
				cell.Value = edit.field.Value
				if cell.Group == 5 {
					cell.Value = 0
					if edit.field.Pressed {
						cell.Value = 1
					}
				}
				cell.Normalized = edit.field.Normalized
				bytes = edit.field.Data
			}
			if edit.report || edit.field != nil {
				if len(bytes) != int(cell.Length) {
					return 0, errors.New("Input data length is immutable")
				}
				copy(unsafe.Slice((*byte)(unsafe.Pointer(cell.Data)), cell.Length), bytes)
			}
		}
	}
	return io.call("send", nil, uintptr(io.handle.Load()), uintptr(batch.ticket))
}
func (io *Interceptor) Discard(batch *Batch) error {
	if e := io.enter(); e != nil {
		return e
	}
	defer io.mu.Unlock()
	if e := io.valid(batch); e != nil {
		return e
	}
	_, e := io.call("discard", nil, uintptr(io.handle.Load()), uintptr(batch.ticket))
	return e
}
func (io *Interceptor) Descriptor(token string, report bool) (Descriptor, error) {
	var result Descriptor
	if e := io.enter(); e != nil {
		return result, e
	}
	defer io.mu.Unlock()
	name, e := syscall.BytePtrFromString(token)
	if e != nil {
		return result, e
	}
	var out nativeBlob
	raw := uintptr(0)
	if report {
		raw = 1
	}
	_, e = io.call("descriptor", []any{name, &out}, uintptr(io.handle.Load()), uintptr(unsafe.Pointer(name)), raw, uintptr(unsafe.Pointer(&out)))
	if e != nil {
		return result, e
	}
	return Descriptor{out.Applicable != 0, out.Format, data(out.Data, out.Length)}, nil
}
func (io *Interceptor) Inject(kind string, state map[string]int64, options ...Injection) (uint32, error) {
	if e := io.enter(); e != nil {
		return 0, e
	}
	defer io.mu.Unlock()
	k := 0
	for n, v := range kinds {
		if v == kind {
			k = n
		}
	}
	if k == 0 {
		return 0, errors.New("Unknown input kind")
	}
	fields := make([]nativeField, len(state))
	keep := []any{}
	names := [][]byte{}
	n := 0
	for name, value := range state {
		b := append([]byte(name), 0)
		names = append(names, b)
		keep = append(keep, &b[0])
		fields[n] = nativeField{Name: uintptr(unsafe.Pointer(&b[0])), Value: value}
		n++
	}
	var input nativeEvent
	input.Kind = uint32(k)
	input.Count = uint32(len(fields))
	if len(fields) > 0 {
		input.Fields = uintptr(unsafe.Pointer(&fields[0]))
		keep = append(keep, &fields[0])
	}
	if len(options) > 0 && options[0].Report != nil {
		b := options[0].Report
		if len(b) == 0 {
			return 0, errors.New("A raw report cannot be empty")
		}
		keep = append(keep, &b[0])
		name := append([]byte("report"), 0)
		names = append(names, name)
		keep = append(keep, &name[0])
		fields = append(fields, nativeField{Name: uintptr(unsafe.Pointer(&name[0])), Group: 7, Data: uintptr(unsafe.Pointer(&b[0])), Length: uint32(len(b))})
		input.Fields = uintptr(unsafe.Pointer(&fields[0]))
		input.Count = uint32(len(fields))
		keep = append(keep, &fields[0])
	}
	if len(options) > 0 && options[0].Token != "" {
		b := append([]byte(options[0].Token), 0)
		names = append(names, b)
		keep = append(keep, &b[0])
		input.Token = uintptr(unsafe.Pointer(&b[0]))
	}
	keep = append(keep, &input)
	accepted, e := io.call("inject", keep, uintptr(io.handle.Load()), uintptr(unsafe.Pointer(&input)), 1)
	runtime.KeepAlive(names)
	return accepted, e
}
func (io *Interceptor) Cancel() error {
	id := io.handle.Load()
	if id == 0 {
		return errors.New("Interceptor is closed")
	}
	_, e := io.call("cancel", nil, uintptr(id))
	return e
}
func (io *Interceptor) Close() error {
	id := io.handle.Load()
	if id == 0 {
		return nil
	}
	_, _ = io.call("cancel", nil, uintptr(id))
	io.mu.Lock()
	defer io.mu.Unlock()
	id = io.handle.Swap(0)
	if id == 0 {
		return nil
	}
	_, e := io.call("close", nil, uintptr(id))
	runtime.SetFinalizer(io, nil)
	return e
}

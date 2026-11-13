//! Windows x64 Interceptor binding. Receive, edit state, send; no polling thread.
#[cfg(not(all(windows, target_arch = "x86_64")))]
compile_error!("Interceptor requires Windows x64");
use std::{
    collections::BTreeMap,
    ffi::{c_char, CStr, CString},
    fmt, mem, ptr, slice,
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc,
    },
};
#[repr(C)]
#[derive(Clone, Copy)]
struct RawField {
    name: *const c_char,
    value: i64,
    minimum: i64,
    maximum: i64,
    physical_minimum: i64,
    physical_maximum: i64,
    normalized: f64,
    group: u32,
    index: u32,
    usage_page: u32,
    usage: u32,
    collection: u32,
    report_id: u32,
    data_index: u32,
    element: u32,
    flags: u32,
    units: u32,
    units_exponent: u32,
    bit_size: u32,
    data: *mut u8,
    length: u32,
    reserved: u32,
}
#[repr(C)]
struct RawEvent {
    kind: u32,
    blocked: u32,
    sequence: u64,
    timestamp: u64,
    token: *const c_char,
    fields: *mut RawField,
    count: u32,
    decode_error: u32,
    decode_complete: u32,
    parser_status: u32,
    device: u32,
    reserved: u32,
}
#[repr(C)]
struct RawBatch {
    id: u64,
    count: u32,
    reserved: u32,
    events: *mut RawEvent,
}
#[repr(C)]
struct RawDevice {
    kind: u32,
    id: u32,
    vid: u16,
    pid: u16,
    usage_page: u16,
    usage: u16,
    token: *const c_char,
    name: *const c_char,
    hardware_id: *const c_char,
}
#[repr(C)]
struct RawDevices {
    count: u32,
    reserved: u32,
    items: *const RawDevice,
}
#[repr(C)]
struct RawBlob {
    applicable: u32,
    format: u32,
    length: u32,
    reserved: u32,
    data: *const u8,
}
#[repr(C)]
#[derive(Default)]
struct RawInit {
    ready: u32,
    reboot: u32,
    associated: u32,
    already: u32,
    skipped: u32,
    reserved: u32,
}
#[repr(C)]
struct RawError {
    code: u32,
    accepted: u32,
    message: [c_char; 768],
}
impl Default for RawError {
    fn default() -> Self {
        Self {
            code: 0,
            accepted: 0,
            message: [0; 768],
        }
    }
}
#[repr(C)]
#[derive(Default, Debug)]
pub struct Info {
    pub installed: u32,
    pub active: u32,
    pub trial_state: u32,
    pub input_mask: u32,
    pub controller_mask: u32,
    pub device_count: u32,
    pub queued: u32,
    pub last_reset: u32,
    pub remaining_ms: u64,
    pub timestamp_frequency: u64,
    pub generation: u64,
}
#[link(name = "InterceptorSDKBridge")]
unsafe extern "C" {
    fn interceptor_version() -> u32;
    fn interceptor_sizeof(kind: u32) -> u32;
    fn interceptor_open(out: *mut u64, e: *mut RawError) -> u32;
    fn interceptor_close(id: u64, e: *mut RawError) -> u32;
    fn interceptor_cancel(id: u64, e: *mut RawError) -> u32;
    fn interceptor_initialize(id: u64, out: *mut RawInit, e: *mut RawError) -> u32;
    fn interceptor_info_get(id: u64, out: *mut Info, e: *mut RawError) -> u32;
    fn interceptor_devices_get(id: u64, out: *mut *const RawDevices, e: *mut RawError) -> u32;
    fn interceptor_receive(id: u64, timeout: u32, out: *mut *const RawBatch, e: *mut RawError) -> u32;
    fn interceptor_send(id: u64, ticket: u64, e: *mut RawError) -> u32;
    fn interceptor_discard(id: u64, ticket: u64, e: *mut RawError) -> u32;
    fn interceptor_descriptor(
        id: u64,
        token: *const c_char,
        report: u32,
        out: *mut RawBlob,
        e: *mut RawError,
    ) -> u32;
    fn interceptor_inject(id: u64, events: *const RawEvent, count: u32, e: *mut RawError) -> u32;
}
#[derive(Debug)]
pub struct Error {
    pub code: u32,
    pub accepted: u32,
    pub message: String,
}
impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}
impl std::error::Error for Error {}
fn error(message: &str) -> Error {
    Error {
        code: 87,
        accepted: 0,
        message: message.into(),
    }
}
fn check(code: u32, e: &RawError) -> Result<u32, Error> {
    if code == 0 {
        Ok(e.accepted)
    } else {
        Err(Error {
            code,
            accepted: e.accepted,
            message: unsafe { CStr::from_ptr(e.message.as_ptr()) }
                .to_string_lossy()
                .into_owned(),
        })
    }
}
unsafe fn text(p: *const c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}
unsafe fn bytes(p: *const u8, n: u32) -> Vec<u8> {
    if n == 0 {
        Vec::new()
    } else {
        slice::from_raw_parts(p, n as usize).to_vec()
    }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum Kind {
    Keyboard = 1,
    Mouse,
    Hid,
    XInput,
    XInputHid,
    Gip,
}
impl Kind {
    fn from(n: u32) -> Self {
        match n {
            1 => Self::Keyboard,
            2 => Self::Mouse,
            3 => Self::Hid,
            4 => Self::XInput,
            5 => Self::XInputHid,
            6 => Self::Gip,
            _ => panic!("Invalid bridge input kind"),
        }
    }
}
#[derive(Debug)]
pub struct Device {
    pub kind: Kind,
    pub id: u32,
    pub vid: u16,
    pub pid: u16,
    pub usage_page: u16,
    pub usage: u16,
    pub token: String,
    pub name: String,
    pub hardware_id: String,
}
#[derive(Debug)]
pub struct Initialization {
    pub ready: bool,
    pub reboot_required: bool,
    pub associated: u32,
    pub already_associated: u32,
    pub skipped: u32,
}
#[derive(Debug)]
pub struct Descriptor {
    pub applicable: bool,
    pub format: u32,
    pub data: Vec<u8>,
}
#[derive(Debug)]
pub struct Field {
    pub value: i64,
    pub normalized: f64,
    pub pressed: bool,
    pub minimum: i64,
    pub maximum: i64,
    pub physical_minimum: i64,
    pub physical_maximum: i64,
    pub normalized_valid: bool,
    pub usage_page: u32,
    pub usage: u32,
    pub collection: u32,
    pub report_id: u32,
    pub data_index: u32,
    pub element: u32,
    pub flags: u32,
    pub units: u32,
    pub units_exponent: u32,
    pub bit_size: u32,
    pub data: Vec<u8>,
    native: *mut RawField,
}
#[derive(Debug, Default)]
pub struct State {
    pub scalars: BTreeMap<String, i64>,
    pub axes: Vec<Field>,
    pub triggers: Vec<Field>,
    pub hats: Vec<Field>,
    pub values: Vec<Field>,
    pub buttons: Vec<Field>,
    pub binary_values: Vec<Field>,
}
impl State {
    fn group(&mut self, n: u32) -> &mut Vec<Field> {
        match n {
            1 => &mut self.axes,
            2 => &mut self.triggers,
            3 => &mut self.hats,
            4 => &mut self.values,
            5 => &mut self.buttons,
            6 => &mut self.binary_values,
            _ => panic!("Invalid field group"),
        }
    }
}
pub struct Input {
    pub kind: Kind,
    pub token: String,
    pub sequence: u64,
    pub timestamp: u64,
    pub blocked: bool,
    pub decode_error: u32,
    pub decode_complete: bool,
    pub parser_status: u32,
    pub state: State,
    pub report: Vec<u8>,
    native: *mut RawEvent,
    scalars: Vec<(String, *mut RawField)>,
    raw_report: *mut RawField,
}
impl Input {
    unsafe fn copy(p: *mut RawEvent) -> Self {
        let e = &*p;
        let mut result = Self {
            kind: Kind::from(e.kind),
            token: text(e.token),
            sequence: e.sequence,
            timestamp: e.timestamp,
            blocked: false,
            decode_error: e.decode_error,
            decode_complete: e.decode_complete != 0,
            parser_status: e.parser_status,
            state: State::default(),
            report: Vec::new(),
            native: p,
            scalars: Vec::new(),
            raw_report: ptr::null_mut(),
        };
        for n in 0..e.count {
            let pointer = e.fields.add(n as usize);
            let f = &*pointer;
            if f.group == 0 {
                let name = text(f.name);
                result.state.scalars.insert(name.clone(), f.value);
                result.scalars.push((name, pointer));
            } else if f.group == 7 {
                result.report = bytes(f.data, f.length);
                result.raw_report = pointer;
            } else {
                result.state.group(f.group).push(Field {
                    value: f.value,
                    normalized: f.normalized,
                    pressed: f.value != 0,
                    minimum: f.minimum,
                    maximum: f.maximum,
                    physical_minimum: f.physical_minimum,
                    physical_maximum: f.physical_maximum,
                    normalized_valid: f.flags & 1 != 0,
                    usage_page: f.usage_page,
                    usage: f.usage,
                    collection: f.collection,
                    report_id: f.report_id,
                    data_index: f.data_index,
                    element: f.element,
                    flags: f.flags,
                    units: f.units,
                    units_exponent: f.units_exponent,
                    bit_size: f.bit_size,
                    data: bytes(f.data, f.length),
                    native: pointer,
                });
            }
        }
        result
    }
    unsafe fn commit(&mut self) -> Result<(), Error> {
        (*self.native).blocked = u32::from(self.blocked);
        if self.blocked {
            return Ok(());
        }
        for (name, p) in &self.scalars {
            (**p).value = *self
                .state
                .scalars
                .get(name)
                .ok_or_else(|| error("Missing state field"))?;
        }
        if !self.raw_report.is_null() {
            let f = &mut *self.raw_report;
            if self.report.len() != f.length as usize {
                return Err(error("Input data length is immutable"));
            }
            if f.length > 0 {
                ptr::copy_nonoverlapping(self.report.as_ptr(), f.data, self.report.len());
            }
        }
        for list in [
            &mut self.state.axes,
            &mut self.state.triggers,
            &mut self.state.hats,
            &mut self.state.values,
            &mut self.state.buttons,
            &mut self.state.binary_values,
        ] {
            for field in list {
                let f = &mut *field.native;
                f.value = if f.group == 5 {
                    i64::from(field.pressed)
                } else {
                    field.value
                };
                f.normalized = field.normalized;
                if field.data.len() != f.length as usize {
                    return Err(error("Input data length is immutable"));
                }
                if f.length > 0 {
                    ptr::copy_nonoverlapping(field.data.as_ptr(), f.data, field.data.len());
                }
            }
        }
        Ok(())
    }
}
pub struct Batch {
    pub inputs: Vec<Input>,
    session: u64,
    ticket: u64,
    header: *const RawBatch,
}
pub struct Interceptor {
    id: Arc<AtomicU64>,
    ticket: u64,
}
#[derive(Clone)]
pub struct CancelHandle {
    session: Arc<AtomicU64>,
}
impl CancelHandle {
    pub fn cancel(&self) -> Result<(), Error> {
        let id = self.session.load(Ordering::Acquire);
        if id == 0 {
            return Err(error("Interceptor is closed"));
        }
        let mut e = RawError::default();
        check(unsafe { interceptor_cancel(id, &mut e) }, &e).map(|_| ())
    }
}
impl Interceptor {
    pub fn new() -> Result<Self, Error> {
        if unsafe { interceptor_version() } != 1
            || unsafe { interceptor_sizeof(1) } != mem::size_of::<RawField>() as u32
            || unsafe { interceptor_sizeof(2) } != mem::size_of::<RawEvent>() as u32
            || unsafe { interceptor_sizeof(5) } != mem::size_of::<RawError>() as u32
        {
            return Err(error("Incompatible native ABI"));
        }
        let mut id = 0;
        let mut e = RawError::default();
        check(unsafe { interceptor_open(&mut id, &mut e) }, &e)?;
        Ok(Self {
            id: Arc::new(AtomicU64::new(id)),
            ticket: 0,
        })
    }
    fn active(&self) -> Result<u64, Error> {
        let id = self.id.load(Ordering::Acquire);
        if id == 0 {
            Err(error("Interceptor is closed"))
        } else {
            Ok(id)
        }
    }
    pub fn initialize(&mut self) -> Result<Initialization, Error> {
        let mut r = RawInit::default();
        let mut e = RawError::default();
        check(
            unsafe { interceptor_initialize(self.active()?, &mut r, &mut e) },
            &e,
        )?;
        Ok(Initialization {
            ready: r.ready != 0,
            reboot_required: r.reboot != 0,
            associated: r.associated,
            already_associated: r.already,
            skipped: r.skipped,
        })
    }
    pub fn info(&mut self) -> Result<Info, Error> {
        let mut r = Info::default();
        let mut e = RawError::default();
        check(unsafe { interceptor_info_get(self.active()?, &mut r, &mut e) }, &e)?;
        Ok(r)
    }
    pub fn devices(&mut self) -> Result<Vec<Device>, Error> {
        let mut p = ptr::null();
        let mut e = RawError::default();
        check(
            unsafe { interceptor_devices_get(self.active()?, &mut p, &mut e) },
            &e,
        )?;
        let view = unsafe { &*p };
        Ok((0..view.count)
            .map(|n| unsafe {
                let d = &*view.items.add(n as usize);
                Device {
                    kind: Kind::from(d.kind),
                    id: d.id,
                    vid: d.vid,
                    pid: d.pid,
                    usage_page: d.usage_page,
                    usage: d.usage,
                    token: text(d.token),
                    name: text(d.name),
                    hardware_id: text(d.hardware_id),
                }
            })
            .collect())
    }
    /// Milliseconds; None waits indefinitely. No input returns an empty batch.
    pub fn receive(&mut self, timeout: Option<u32>) -> Result<Batch, Error> {
        let id = self.active()?;
        let mut p = ptr::null();
        let mut e = RawError::default();
        check(
            unsafe { interceptor_receive(id, timeout.unwrap_or(u32::MAX), &mut p, &mut e) },
            &e,
        )?;
        let view = unsafe { &*p };
        self.ticket = view.id;
        Ok(Batch {
            inputs: (0..view.count)
                .map(|n| unsafe { Input::copy(view.events.add(n as usize)) })
                .collect(),
            session: id,
            ticket: view.id,
            header: p,
        })
    }
    fn validate(&self, b: &Batch) -> Result<u64, Error> {
        let id = self.active()?;
        if b.session != id || b.ticket != self.ticket || unsafe { (*b.header).id } != b.ticket {
            return Err(error("Stale or foreign batch"));
        }
        Ok(id)
    }
    pub fn send(&mut self, b: &mut Batch) -> Result<u32, Error> {
        let id = self.validate(b)?;
        for input in &mut b.inputs {
            unsafe {
                input.commit()?;
            }
        }
        let mut e = RawError::default();
        check(unsafe { interceptor_send(id, b.ticket, &mut e) }, &e)
    }
    pub fn discard(&mut self, b: &Batch) -> Result<(), Error> {
        let id = self.validate(b)?;
        let mut e = RawError::default();
        check(unsafe { interceptor_discard(id, b.ticket, &mut e) }, &e).map(|_| ())
    }
    pub fn descriptor(&mut self, token: &str, report: bool) -> Result<Descriptor, Error> {
        let token = CString::new(token).map_err(|_| error("Token contains NUL"))?;
        let mut r = RawBlob {
            applicable: 0,
            format: 0,
            length: 0,
            reserved: 0,
            data: ptr::null(),
        };
        let mut e = RawError::default();
        check(
            unsafe {
                interceptor_descriptor(
                    self.active()?,
                    token.as_ptr(),
                    u32::from(report),
                    &mut r,
                    &mut e,
                )
            },
            &e,
        )?;
        Ok(Descriptor {
            applicable: r.applicable != 0,
            format: r.format,
            data: unsafe { bytes(r.data, r.length) },
        })
    }
    pub fn inject(
        &mut self,
        kind: Kind,
        state: &[(&str, i64)],
        token: Option<&str>,
        report: Option<&[u8]>,
    ) -> Result<u32, Error> {
        let names = state
            .iter()
            .map(|(name, _)| CString::new(*name).map_err(|_| error("Field name contains NUL")))
            .collect::<Result<Vec<_>, _>>()?;
        let mut fields: Vec<RawField> = state
            .iter()
            .zip(names.iter())
            .map(|((_, value), name)| RawField {
                name: name.as_ptr(),
                value: *value,
                ..unsafe { mem::zeroed() }
            })
            .collect();
        let report_name = CString::new("report").unwrap();
        if let Some(data) = report {
            fields.push(RawField {
                name: report_name.as_ptr(),
                group: 7,
                data: data.as_ptr() as *mut u8,
                length: data
                    .len()
                    .try_into()
                    .map_err(|_| error("Report too large"))?,
                ..unsafe { mem::zeroed() }
            });
        }
        let token = token
            .map(CString::new)
            .transpose()
            .map_err(|_| error("Token contains NUL"))?;
        let event = RawEvent {
            kind: kind as u32,
            token: token.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
            fields: fields.as_mut_ptr(),
            count: fields.len() as u32,
            ..unsafe { mem::zeroed() }
        };
        let mut e = RawError::default();
        check(unsafe { interceptor_inject(self.active()?, &event, 1, &mut e) }, &e)
    }
    pub fn cancel_handle(&self) -> CancelHandle {
        CancelHandle {
            session: self.id.clone(),
        }
    }
    pub fn close(&mut self) -> Result<(), Error> {
        let id = self.id.swap(0, Ordering::AcqRel);
        if id == 0 {
            return Ok(());
        }
        let mut e = RawError::default();
        unsafe {
            interceptor_cancel(id, &mut e);
        }
        check(unsafe { interceptor_close(id, &mut e) }, &e).map(|_| ())
    }
}
impl Drop for Interceptor {
    fn drop(&mut self) {
        let _ = self.close();
    }
}

#[cfg(test)] mod tests {
use super::*;
extern "C" { fn interceptor_test_mode(mode:u32); }
#[test] #[ignore="Requires the product synthetic backend"]
fn product_contract(){
 let kinds=[Kind::Keyboard,Kind::Mouse,Kind::Hid,Kind::XInput,Kind::XInputHid,Kind::Gip];
 let allowed=|k:Kind| {let n=k as u32;if n<=2{3u32&(1<<(n-1))!=0}else{15u32&(1<<(n-3))!=0}};
 let count=kinds.iter().filter(|&&k|allowed(k)).count();
 let mut io=Interceptor::new().unwrap();assert!(io.initialize().unwrap().ready);
 let info=io.info().unwrap();assert_eq!(info.input_mask,3);assert_eq!(info.controller_mask,15);
 let devices=io.devices().unwrap();assert_eq!(devices.len(),count);
 let mut batch=io.receive(Some(0)).unwrap();assert_eq!(batch.inputs.len(),count);
 for input in &mut batch.inputs {assert!(allowed(input.kind));assert!(input.sequence>i64::MAX as u64);if input.kind==Kind::Keyboard{input.state.scalars.insert("scan_code".into(),48);}if input.kind==Kind::Mouse{input.state.scalars.insert("y".into(),-7);}}
 assert_eq!(io.send(&mut batch).unwrap(),count as u32);assert_eq!(io.send(&mut batch).unwrap(),0);
 let mut next=io.receive(Some(0)).unwrap();assert!(io.send(&mut batch).is_err());next.inputs[0].blocked=true;assert_eq!(io.send(&mut next).unwrap(),count as u32-1);
 let mut retry=io.receive(Some(0)).unwrap();unsafe{interceptor_test_mode(2)};
 let error=io.send(&mut retry).unwrap_err();assert_eq!(error.code,170);assert_eq!(error.accepted,1);assert_eq!(io.send(&mut retry).unwrap(),count as u32-1);unsafe{interceptor_test_mode(0)};
 let discard=io.receive(Some(0)).unwrap();io.discard(&discard).unwrap();
 for kind in kinds {if !allowed(kind){assert_eq!(io.inject(kind,&[],None,None).unwrap_err().code,50);}}
 unsafe{interceptor_test_mode(1)};let cancel=io.cancel_handle();let worker=std::thread::spawn(move||{std::thread::sleep(std::time::Duration::from_millis(30));cancel.cancel().unwrap();});
 assert!(io.receive(Some(2000)).unwrap().inputs.is_empty());worker.join().unwrap();unsafe{interceptor_test_mode(0)};
 io.close().unwrap();assert!(io.receive(Some(0)).is_err());
}
}

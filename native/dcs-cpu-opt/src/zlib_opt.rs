//! `ZlibInflateOpt`: faster, byte-identical inflate for edCore.dll's edvfs zip reader.
//!
//! `edCore.dll+0x65730` (DCS 2.9.31.29434) decodes each method-8 zip entry as `inflateInit2_(strm, -15)` ->
//! one `inflate(strm, Z_FINISH)` with the whole entry in and the exact uncompressed size out -> `inflateEnd`, all
//! through `zlib1.dll` (zlib-ng 2.2.1, AVX2 `inflate_fast` at `+0xf300`). We redirect edCore's three zlib1 IAT slots
//! (data writes, no code patch). A stream that this thread just opened as raw deflate and now hands over with
//! `Z_FINISH` and no earlier `inflate` call is decoded in one go by the vendored, zlib-strict libdeflate 1.26 straight
//! into the caller's buffer, and the `z_stream` fields are set exactly as zlib-ng sets them. zlib-ng's own state for
//! that stream is never touched (it stays in HEAD and is only freed by the real `inflateEnd`), so the internal states
//! of the two implementations never mix. Everything else (streaming calls, zlib/gzip wrappers, other flush modes,
//! libdeflate rejects) goes to the real zlib1.dll. See `docs/investigations/zlib1-inflate.md`.
use std::cell::RefCell;
use std::ffi::{c_char, c_int, c_void};
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering::Relaxed, Ordering::SeqCst};

/// zlib `z_stream` on Win64 (`uLong` = 32 bits): 0x58 bytes, as edCore passes it (`stream_size = 0x58`).
#[repr(C)]
pub(crate) struct ZStream {
    /// Next input byte.
    pub next_in: *const u8,
    /// Bytes available at `next_in`.
    pub avail_in: u32,
    /// Total input bytes read so far.
    pub total_in: u32,
    /// Next output byte.
    pub next_out: *mut u8,
    /// Free space at `next_out`.
    pub avail_out: u32,
    /// Total output bytes written so far.
    pub total_out: u32,
    /// Last error message, or null.
    pub msg: *const c_char,
    /// zlib-ng internal state; only used here as an identity tag.
    pub state: *mut c_void,
    /// Allocator callback (unused here).
    pub zalloc: *mut c_void,
    /// Free callback (unused here).
    pub zfree: *mut c_void,
    /// Allocator cookie (unused here).
    pub opaque: *mut c_void,
    /// Decoder bit/mode summary written by inflate.
    pub data_type: c_int,
    /// Adler-32/CRC-32 of the data (not used by raw deflate).
    pub adler: u32,
    /// Reserved.
    pub reserved: u32,
}
const _: () = assert!(std::mem::size_of::<ZStream>() == 0x58);

/// `inflateInit2_` signature.
pub(crate) type InflateInit2Fn = unsafe extern "C" fn(*mut ZStream, c_int, *const c_char, c_int) -> c_int;

/// `inflate` signature.
pub(crate) type InflateFn = unsafe extern "C" fn(*mut ZStream, c_int) -> c_int;

/// `inflateEnd` signature.
pub(crate) type InflateEndFn = unsafe extern "C" fn(*mut ZStream) -> c_int;

/// zlib `Z_OK`.
pub(crate) const Z_OK: c_int = 0;

/// zlib `Z_STREAM_END`.
pub(crate) const Z_STREAM_END: c_int = 1;

/// zlib `Z_STREAM_ERROR`.
const Z_STREAM_ERROR: c_int = -2;

/// zlib `Z_FINISH`.
pub(crate) const Z_FINISH: c_int = 4;

/// Vendored libdeflate (built by `build.rs` with `LIBDEFLATE_ZLIB_STRICT`).
mod ld {
    use std::ffi::c_void;

    /// Opaque `struct libdeflate_decompressor`.
    pub enum Decompressor {}

    /// `LIBDEFLATE_SUCCESS`.
    pub const SUCCESS: u32 = 0;

    extern "C" {
        /// Allocates a decompressor (about 11 KB, malloc).
        pub fn libdeflate_alloc_decompressor() -> *mut Decompressor;

        /// Frees a decompressor.
        pub fn libdeflate_free_decompressor(d: *mut Decompressor);

        /// Raw DEFLATE decode that reports the bytes consumed and produced.
        pub fn libdeflate_deflate_decompress_ex(
            d: *mut Decompressor,
            input: *const c_void,
            in_nbytes: usize,
            out: *mut c_void,
            out_nbytes_avail: usize,
            actual_in_nbytes_ret: *mut usize,
            actual_out_nbytes_ret: *mut usize,
        ) -> u32;
    }
}

/// Original zlib1.dll `inflateInit2_` (the value in edCore's IAT before patching).
static REAL_INIT2: AtomicUsize = AtomicUsize::new(0);

/// Original zlib1.dll `inflate`.
static REAL_INFLATE: AtomicUsize = AtomicUsize::new(0);

/// Original zlib1.dll `inflateEnd`.
static REAL_END: AtomicUsize = AtomicUsize::new(0);

/// Streams decoded by libdeflate in the current log window.
pub(crate) static ZLIB_FAST: AtomicU64 = AtomicU64::new(0);

/// Output bytes produced by the fast path in the current log window.
pub(crate) static ZLIB_FAST_BYTES: AtomicU64 = AtomicU64::new(0);

/// QPC ticks spent in the fast path in the current log window.
pub(crate) static ZLIB_FAST_QPC: AtomicU64 = AtomicU64::new(0);

/// Fast-path candidates that libdeflate rejected and zlib-ng decided, in the current log window.
pub(crate) static ZLIB_FALLBACK: AtomicU64 = AtomicU64::new(0);

/// `inflate` calls passed straight to zlib-ng (not a fresh raw one-shot), in the current log window.
pub(crate) static ZLIB_PASS: AtomicU64 = AtomicU64::new(0);

/// Tracking state of one `z_stream` on this thread.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Track {
    /// Opened with windowBits -15, no `inflate` call yet.
    FreshRaw,
    /// Completed by the fast path; zlib-ng's state is still untouched.
    Done,
}

/// One tracked stream: (strm address, strm->state, track).
type Slot = (usize, usize, Option<Track>);

/// Per-thread decoder and stream table (edCore's zip reader holds one stream at a time per thread).
struct Tls {
    /// libdeflate decompressor, created on first use and freed when the thread exits.
    dec: *mut ld::Decompressor,
    /// A few recent streams.
    slots: [Slot; 4],
}

impl Drop for Tls {
    /// Frees the thread's decompressor at thread exit (TLS destructor).
    fn drop(&mut self) {
        if !self.dec.is_null() {
            unsafe { ld::libdeflate_free_decompressor(self.dec) };
            self.dec = std::ptr::null_mut();
        }
    }
}

thread_local! {
    /// This thread's decoder and stream table.
    static TLS: RefCell<Tls> = const { RefCell::new(Tls { dec: std::ptr::null_mut(), slots: [(0, 0, None); 4] }) };
}

/// Looks up the tracking entry of the stream identified by (`s`, `st`).
fn slot_get(t: &Tls, s: usize, st: usize) -> Option<Track> {
    t.slots.iter().find(|e| e.0 == s && e.1 == st).and_then(|e| e.2)
}

/// Sets, or clears with `None`, the tracking entry of the stream identified by (`s`, `st`).
fn slot_set(t: &mut Tls, s: usize, st: usize, k: Option<Track>) {
    if let Some(i) = t.slots.iter().position(|e| e.0 == s && e.1 == st) {
        t.slots[i] = (s, st, k);
    } else if k.is_some() {
        let i = t.slots.iter().position(|e| e.2.is_none()).unwrap_or(0);
        t.slots[i] = (s, st, k);
    }
}

/// Clears every entry for the stream address `s`, whatever its state pointer.
fn slot_forget(t: &mut Tls, s: usize) {
    for e in t.slots.iter_mut().filter(|e| e.0 == s) {
        *e = (0, 0, None);
    }
}

/// Sets the real zlib1.dll entry points the wrappers forward to.
pub(crate) fn set_real(init2: usize, inflate: usize, end: usize) {
    REAL_INIT2.store(init2, SeqCst);
    REAL_INFLATE.store(inflate, SeqCst);
    REAL_END.store(end, SeqCst);
}

/// Wrapper installed in edCore's `inflateInit2_` IAT slot: calls zlib-ng, then tracks fresh raw streams.
///
/// # Safety
/// Same contract as zlib's `inflateInit2_`.
pub(crate) unsafe extern "C" fn hook_inflate_init2(strm: *mut ZStream, wbits: c_int, ver: *const c_char, size: c_int) -> c_int {
    let real: InflateInit2Fn = std::mem::transmute(REAL_INIT2.load(Relaxed));
    let rc = real(strm, wbits, ver, size);
    if !strm.is_null() {
        let st = (*strm).state as usize;
        let ok = rc == Z_OK && wbits == -15 && size == 0x58;
        let _ = TLS.try_with(|t| {
            if let Ok(mut t) = t.try_borrow_mut() {
                slot_forget(&mut t, strm as usize);
                if ok {
                    slot_set(&mut t, strm as usize, st, Some(Track::FreshRaw));
                }
            }
        });
    }
    rc
}

/// Wrapper installed in edCore's `inflateEnd` IAT slot: forgets the stream, then calls zlib-ng.
///
/// # Safety
/// Same contract as zlib's `inflateEnd`.
pub(crate) unsafe extern "C" fn hook_inflate_end(strm: *mut ZStream) -> c_int {
    if !strm.is_null() {
        let _ = TLS.try_with(|t| {
            if let Ok(mut t) = t.try_borrow_mut() {
                slot_forget(&mut t, strm as usize);
            }
        });
    }
    let real: InflateEndFn = std::mem::transmute(REAL_END.load(Relaxed));
    real(strm)
}

/// Outcome of trying the fast path for one `inflate` call.
enum Fast {
    /// Not a candidate: forward to zlib-ng unchanged.
    Pass,
    /// Stream already completed by the fast path: return this code as zlib-ng in DONE mode would.
    Done(c_int),
    /// Decoded: return `Z_STREAM_END`.
    Decoded,
    /// Candidate that libdeflate rejected: let zlib-ng decide from its untouched state.
    Fallback,
}

/// Tries the libdeflate fast path for `inflate(strm, flush)`.
unsafe fn try_fast(strm: *mut ZStream, flush: c_int) -> Fast {
    let s = &mut *strm;
    let (sp, st) = (strm as usize, s.state as usize);
    let r = TLS.try_with(|t| {
        let Ok(mut t) = t.try_borrow_mut() else { return Fast::Pass };
        match slot_get(&t, sp, st) {
            None => Fast::Pass,
            Some(Track::Done) => {
                // zlib-ng in DONE mode: argument checks, then Z_STREAM_END with nothing consumed.
                if s.next_out.is_null() || (s.next_in.is_null() && s.avail_in != 0) {
                    Fast::Done(Z_STREAM_ERROR)
                } else {
                    Fast::Done(Z_STREAM_END)
                }
            }
            Some(Track::FreshRaw) => {
                // Whatever happens next, zlib-ng owns the stream unless libdeflate completes it.
                slot_set(&mut t, sp, st, None);
                if flush != Z_FINISH
                    || s.total_in != 0
                    || s.total_out != 0
                    || s.next_in.is_null()
                    || s.next_out.is_null()
                    || s.avail_out == 0
                {
                    return Fast::Pass;
                }
                if t.dec.is_null() {
                    t.dec = ld::libdeflate_alloc_decompressor();
                    if t.dec.is_null() {
                        return Fast::Pass;
                    }
                }
                let t0 = super::qpc_now();
                let (mut ain, mut aout) = (0usize, 0usize);
                let rc = ld::libdeflate_deflate_decompress_ex(
                    t.dec,
                    s.next_in as *const c_void,
                    s.avail_in as usize,
                    s.next_out as *mut c_void,
                    s.avail_out as usize,
                    &mut ain,
                    &mut aout,
                );
                if rc != ld::SUCCESS {
                    return Fast::Fallback;
                }
                s.next_in = s.next_in.add(ain);
                s.avail_in -= ain as u32;
                s.total_in = ain as u32;
                s.next_out = s.next_out.add(aout);
                s.avail_out -= aout as u32;
                s.total_out = aout as u32;
                // zlib-ng after the final block of a raw stream: last=1 (+64), no bits left, mode DONE.
                s.data_type = 64;
                slot_set(&mut t, sp, st, Some(Track::Done));
                ZLIB_FAST_BYTES.fetch_add(aout as u64, Relaxed);
                ZLIB_FAST_QPC.fetch_add((super::qpc_now() - t0).max(0) as u64, Relaxed);
                Fast::Decoded
            }
        }
    });
    r.unwrap_or(Fast::Pass)
}

/// Wrapper installed in edCore's `inflate` IAT slot.
///
/// # Safety
/// Same contract as zlib's `inflate`.
pub(crate) unsafe extern "C" fn hook_inflate(strm: *mut ZStream, flush: c_int) -> c_int {
    let real: InflateFn = std::mem::transmute(REAL_INFLATE.load(Relaxed));
    if strm.is_null() {
        return real(strm, flush);
    }
    match try_fast(strm, flush) {
        Fast::Decoded => {
            ZLIB_FAST.fetch_add(1, Relaxed);
            Z_STREAM_END
        }
        Fast::Done(rc) => rc,
        Fast::Fallback => {
            ZLIB_FALLBACK.fetch_add(1, Relaxed);
            real(strm, flush)
        }
        Fast::Pass => {
            ZLIB_PASS.fetch_add(1, Relaxed);
            real(strm, flush)
        }
    }
}

/// Locates the IAT slots a mapped PE image imports from `dll` (case-insensitive) for each name in `names`.
///
/// # Safety
/// `base` must be the base of a mapped PE32+ image (SEC_IMAGE layout).
pub(crate) unsafe fn find_iat_slots(base: *const u8, dll: &str, names: &[&str]) -> Vec<Option<*mut usize>> {
    let rd32 = |o: usize| (base.add(o) as *const u32).read_unaligned() as usize;
    let cstr = |o: usize| std::ffi::CStr::from_ptr(base.add(o) as *const c_char).to_bytes();
    let mut out = vec![None; names.len()];
    let pe = rd32(0x3c);
    if rd32(pe) != 0x4550 || (base.add(pe + 24) as *const u16).read_unaligned() != 0x20b {
        return out;
    }
    let imp_rva = rd32(pe + 24 + 112 + 8); // OptionalHeader64.DataDirectory[IMPORT]
    if imp_rva == 0 {
        return out;
    }
    let mut d = imp_rva;
    loop {
        let (oft, name, ft) = (rd32(d), rd32(d + 12), rd32(d + 16));
        if name == 0 {
            break;
        }
        if cstr(name).eq_ignore_ascii_case(dll.as_bytes()) {
            let lookup = if oft != 0 { oft } else { ft };
            let mut i = 0;
            loop {
                let thunk = (base.add(lookup + i * 8) as *const u64).read_unaligned();
                if thunk == 0 {
                    break;
                }
                if thunk >> 63 == 0 {
                    let n = cstr(thunk as usize + 2);
                    if let Some(k) = names.iter().position(|x| x.as_bytes() == n) {
                        out[k] = Some(base.add(ft + i * 8) as *mut usize);
                    }
                }
                i += 1;
            }
        }
        d += 20;
    }
    out
}

/// Atomically replaces one IAT slot's value if it still equals `expect`; returns true on success.
///
/// # Safety
/// `slot` must be a valid IAT slot of a loaded module.
unsafe fn swap_slot(slot: *mut usize, expect: usize, new: usize) -> bool {
    let mut old = 0u32;
    if super::VirtualProtect(slot as *mut c_void, 8, 0x04 /* PAGE_READWRITE */, &mut old) == 0 {
        return false;
    }
    let ok = (*(slot as *const AtomicUsize)).compare_exchange(expect, new, SeqCst, SeqCst).is_ok();
    let mut dummy = 0u32;
    super::VirtualProtect(slot as *mut c_void, 8, old, &mut dummy);
    ok
}

/// Name of libdeflate's decode path on this CPU (it dispatches by itself: BMI2 when CPUID.7:EBX[8] is set).
pub(crate) fn libdeflate_path() -> &'static str {
    let l0 = core::arch::x86_64::__cpuid(0);
    let bmi2 = l0.eax >= 7 && (core::arch::x86_64::__cpuid_count(7, 0).ebx >> 8) & 1 == 1;
    if bmi2 {
        "bmi2"
    } else {
        "generic"
    }
}

/// Installs on the loaded edCore.dll. `Ok(None)` when edCore.dll or zlib1.dll is not loaded yet (retry later);
/// `Err` when something does not match and the hook is skipped.
pub(crate) fn try_install() -> Result<Option<String>, String> {
    let hcore = unsafe { super::GetModuleHandleA(b"edCore.dll\0".as_ptr()) };
    let hz = unsafe { super::GetModuleHandleA(b"zlib1.dll\0".as_ptr()) };
    if hcore.is_null() || hz.is_null() {
        return Ok(None);
    }
    let zlo = hz as usize;
    let zhi = zlo + unsafe { super::pe_image_size(zlo) }.ok_or("bad_pe(zlib1)")?;
    let core = hcore as *const u8;
    let s = unsafe { find_iat_slots(core, "zlib1.dll", &["inflateInit2_", "inflate", "inflateEnd"]) };
    let [Some(a), Some(b), Some(c)] = s[..] else {
        return Err("edCore_imports_missing".into());
    };
    let orig = unsafe { [*a, *b, *c] };
    if orig.iter().any(|&p| p < zlo || p >= zhi) {
        return Err("iat_not_zlib1(already_hooked?)".into());
    }
    set_real(orig[0], orig[1], orig[2]);
    // End and inflate first, init last: a stream opened before the swap is never tracked.
    let ok_end = unsafe { swap_slot(c, orig[2], hook_inflate_end as *const () as usize) };
    let ok_inf = ok_end && unsafe { swap_slot(b, orig[1], hook_inflate as *const () as usize) };
    let ok_init = ok_inf && unsafe { swap_slot(a, orig[0], hook_inflate_init2 as *const () as usize) };
    if !ok_init {
        unsafe {
            swap_slot(b, hook_inflate as *const () as usize, orig[1]);
            swap_slot(c, hook_inflate_end as *const () as usize, orig[2]);
        }
        return Err("iat_write_failed".into());
    }
    let rva = |p: *mut usize| p as usize - core as usize;
    let msg = format!(
        "iat@edCore(init2=0x{:x},inflate=0x{:x},end=0x{:x}) libdeflate=1.26-strict/{}",
        rva(a),
        rva(b),
        rva(c),
        libdeflate_path()
    );
    Ok(Some(msg))
}

#[cfg(test)]
pub(crate) mod tests;

#![allow(non_snake_case)]

use std::ffi::c_void;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::PathBuf;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Mutex;

mod zlib_opt;

extern "C" {
    fn DcsCpuOpt_WriteGuardedPatch(target: *mut u8, bytes: *const u8, len: usize,
        expected_at: *const u8, expected: *const u8, expected_len: usize) -> i32;
    fn DcsCpuOpt_AllocNearPatch(target: *const u8, len: usize) -> *mut u8;
}

unsafe fn write_guarded_patch<const N: usize, const M: usize>(
    target: *mut u8, bytes: &[u8; N], expected_at: *const u8, expected: &[u8; M],
) -> bool {
    DLL_PINNED.load(Ordering::Acquire)
        && DcsCpuOpt_WriteGuardedPatch(target, bytes.as_ptr(), N, expected_at, expected.as_ptr(), M) != 0
}

// Expected first 15 bytes of ed_sound.dll!SND_process (DCS 2.9.31.29434, SHA-256 dc52bc85...)
// 0x180004980: MOV [RSP+0x08], RBX  (48 89 5c 24 08)
// 0x180004985: MOV [RSP+0x10], RBP  (48 89 6c 24 10)
// 0x18000498a: MOV [RSP+0x18], RSI  (48 89 74 24 18)
const EXPECTED_SND_PROCESS_PROLOGUE: [u8; 15] = [
    0x48, 0x89, 0x5c, 0x24, 0x08,
    0x48, 0x89, 0x6c, 0x24, 0x10,
    0x48, 0x89, 0x74, 0x24, 0x18,
];

// Expected first 15 bytes of ed_sound.dll!FUN_180008d40 (RVA 0x8d40)
// 0x180008d40: MOV [RSP+0x08], RBX       (48 89 5c 24 08)
// 0x180008d45: PUSH RDI                  (57)
// 0x180008d46: SUB RSP, 0x50             (48 83 ec 50)
// 0x180008d4a: MOVAPS [RSP+0x40], XMM6   (0f 29 74 24 40)
const EXPECTED_UPDATE_SRC_PROLOGUE: [u8; 15] = [
    0x48, 0x89, 0x5c, 0x24, 0x08,
    0x57,
    0x48, 0x83, 0xec, 0x50,
    0x0f, 0x29, 0x74, 0x24, 0x40,
];

// Context struct layout (verified from SND_process in DCS 2.9.31.29434)
const CTX_STRIDE: usize = 0x388;
const CTX_LISTENER_X: usize = 0x08;       // f64
const CTX_LISTENER_Z: usize = 0x18;       // f64
const CTX_LIST_OFF: usize = 0xb0;         // doubly-linked list head of sources
const CTX_IS_2D: usize = 0xe0;            // u8: non-zero for 2D contexts

// Source struct offsets (verified from FUN_180008d40 / FUN_180007de0 / SND_process)
const SRC_CTX_PTR: usize = 0x00;          // *const u8
const SRC_QTREE_ITEM: usize = 0x18;       // *const u8
const SRC_HOST_LINK: usize = 0x50;        // *const u8
const SRC_WORLD_X: usize = 0x68;          // f64
const SRC_WORLD_Z: usize = 0x78;          // f64
const SRC_MAX_RADIUS: usize = 0x148;      // f32
const SRC_LAST_FRAME: usize = 0x1b8;      // i32
const SRC_ACTIVE_FLAG: usize = 0x1bc;     // i32
const SRC_VOICE_PTR: usize = 0x1f0;       // *const u8

#[derive(Clone, Copy, Debug)]
struct ResolvedAddrs {
    update_source: usize,
    frame_counter: usize,
    snd_state: Option<usize>,
    ctx_count: Option<usize>,
    ctx_array: Option<usize>,
    active_voices: Option<usize>,
    sorted_voices: Option<usize>,
}

/// Read `OptionalHeader.SizeOfImage` from a mapped PE64 image.
unsafe fn pe_image_size(base: usize) -> Option<usize> {
    let dos_magic = ptr::read_unaligned(base as *const u16);
    if dos_magic != 0x5A4D {
        return None;
    }
    let e_lfanew = ptr::read_unaligned((base + 0x3c) as *const i32);
    if e_lfanew <= 0 || e_lfanew > 0x1000 {
        return None;
    }
    let pe_hdr = base + e_lfanew as usize;
    let pe_sig = ptr::read_unaligned(pe_hdr as *const u32);
    if pe_sig != 0x0000_4550 {
        return None;
    }
    let size_of_image = ptr::read_unaligned((pe_hdr + 0x50) as *const u32) as usize;
    if !(0x10000..=0x1000_0000).contains(&size_of_image) {
        return None;
    }
    Some(size_of_image)
}

// The three layout-sensitive hooks use offsets inside DCS objects. A matching instruction sequence
// alone does not establish those layouts on another build.
unsafe fn verified_image_size(base: usize, timestamp: u32, size: usize) -> Option<usize> {
    let actual_size = pe_image_size(base)?;
    let pe = base + ptr::read_unaligned((base + 0x3c) as *const u32) as usize;
    let actual_timestamp = ptr::read_unaligned((pe + 8) as *const u32);
    (actual_timestamp == timestamp && actual_size == size).then_some(actual_size)
}

#[inline]
fn in_image(addr: usize, base: usize, image_size: usize) -> bool {
    addr >= base && addr < base.saturating_add(image_size)
}

#[inline]
unsafe fn read_rip_rel32(next_ip: usize, disp_ptr: *const u8) -> usize {
    let disp = ptr::read_unaligned(disp_ptr as *const i32) as isize;
    (next_ip as isize).wrapping_add(disp) as usize
}

/// Dynamically resolve `update_source` and engine globals by scanning `SND_process`.
/// Avoids hardcoding RVAs so the hook survives DCS patches when code layout shifts.
unsafe fn resolve_from_snd_process(
    base: usize,
    image_size: usize,
    snd_process: usize,
) -> Result<ResolvedAddrs, String> {
    if !in_image(snd_process, base, image_size) {
        return Err(format!("SND_process 0x{:x} outside image", snd_process));
    }
    let max_scan = (base + image_size).saturating_sub(snd_process).min(0x400);
    if max_scan < 0x100 {
        return Err("SND_process scan range too small".to_string());
    }
    let code = std::slice::from_raw_parts(snd_process as *const u8, max_scan);

    // 1. Resolve update_source (FUN_180008d40):
    //    Preferred: exact 0xb0 loop callsite `lea rcx,[rbx-8]; mov rbx,[rbx]; call rel32`
    //    (48 8d 4b f8 48 8b 1b e8 <rel32>) whose target matches EXPECTED_UPDATE_SRC_PROLOGUE.
    let mut update_source: Option<usize> = None;
    for i in 0..code.len().saturating_sub(12) {
        if code[i..i + 8] == [0x48, 0x8d, 0x4b, 0xf8, 0x48, 0x8b, 0x1b, 0xe8] {
            let target = read_rip_rel32(snd_process + i + 12, code.as_ptr().add(i + 8));
            if in_image(target, base, image_size)
                && target + 15 <= base + image_size
                && std::slice::from_raw_parts(target as *const u8, 15) == EXPECTED_UPDATE_SRC_PROLOGUE
            {
                update_source = Some(target);
                break;
            }
        }
    }
    //    Fallback: any direct E8 rel32 call inside SND_process whose target starts with
    //    EXPECTED_UPDATE_SRC_PROLOGUE (resilient to register reallocation in SND_process).
    if update_source.is_none() {
        for i in 0..code.len().saturating_sub(5) {
            if code[i] == 0xe8 {
                let target = read_rip_rel32(snd_process + i + 5, code.as_ptr().add(i + 1));
                if in_image(target, base, image_size)
                    && target + 15 <= base + image_size
                    && std::slice::from_raw_parts(target as *const u8, 15) == EXPECTED_UPDATE_SRC_PROLOGUE
                {
                    update_source = Some(target);
                    break;
                }
            }
        }
    }
    let update_source = update_source
        .ok_or_else(|| "could not resolve update_source call target from SND_process".to_string())?;

    // 2. Resolve frame_counter: `inc dword ptr [rip+disp32]` (ff 05 <disp32>)
    let mut frame_counter: Option<(usize, usize)> = None;
    for i in 0..code.len().min(0x300).saturating_sub(6) {
        if code[i] == 0xff && code[i + 1] == 0x05 {
            let addr = read_rip_rel32(snd_process + i + 6, code.as_ptr().add(i + 2));
            if in_image(addr, base, image_size) {
                frame_counter = Some((i, addr));
                break;
            }
        }
    }
    let (fc_off, frame_counter) = frame_counter
        .ok_or_else(|| "could not resolve frame_counter (ff 05) from SND_process".to_string())?;

    // 3. Optional telemetry globals (used only for stats logging; never block hooking).
    //    snd_state: `cmp dword ptr [rip+disp32], 1` (83 3d <disp32> 01) near top of SND_process.
    let mut snd_state = None;
    for i in 0..code.len().min(0x80).saturating_sub(7) {
        if code[i] == 0x83 && code[i + 1] == 0x3d && code[i + 6] == 0x01 {
            let addr = read_rip_rel32(snd_process + i + 7, code.as_ptr().add(i + 2));
            if in_image(addr, base, image_size) {
                snd_state = Some(addr);
                break;
            }
        }
    }

    //    ctx_count: `mov eax, dword ptr [rip+disp32]; test eax, eax` (8b 05 <disp32> 85 c0)
    let mut ctx_count = None;
    for i in 0..code.len().min(0x120).saturating_sub(8) {
        if code[i] == 0x8b && code[i + 1] == 0x05 && code[i + 6] == 0x85 && code[i + 7] == 0xc0 {
            let addr = read_rip_rel32(snd_process + i + 6, code.as_ptr().add(i + 2));
            if in_image(addr, base, image_size) {
                ctx_count = Some(addr);
                break;
            }
        }
    }

    //    ctx_array: `lea r13, [rip+disp32]` (4c 8d 2d <disp32>) near top of SND_process
    let mut ctx_array = None;
    for i in 0..code.len().min(0x100).saturating_sub(7) {
        if code[i] == 0x4c && code[i + 1] == 0x8d && code[i + 2] == 0x2d {
            let addr = read_rip_rel32(snd_process + i + 7, code.as_ptr().add(i + 3));
            if in_image(addr, base, image_size) {
                ctx_array = Some(addr);
                break;
            }
        }
    }

    //    sorted_voices: `mov [rip+disp32], eax` (89 05 <disp32>) immediately after frame_counter inc
    let mut sorted_voices = None;
    let sv_end = (fc_off + 24).min(code.len().saturating_sub(6));
    for i in (fc_off + 6)..sv_end {
        if code[i] == 0x89 && code[i + 1] == 0x05 {
            let addr = read_rip_rel32(snd_process + i + 6, code.as_ptr().add(i + 2));
            if in_image(addr, base, image_size) {
                sorted_voices = Some(addr);
                break;
            }
        }
    }

    //    active_voices: `cmp [rip+disp32], esi` (39 35 <disp32>) shortly after sorted_voices
    let mut active_voices = None;
    let av_end = (fc_off + 40).min(code.len().saturating_sub(6));
    for i in (fc_off + 6)..av_end {
        if code[i] == 0x39 && code[i + 1] == 0x35 {
            let addr = read_rip_rel32(snd_process + i + 6, code.as_ptr().add(i + 2));
            if in_image(addr, base, image_size) {
                active_voices = Some(addr);
                break;
            }
        }
    }

    Ok(ResolvedAddrs {
        update_source,
        frame_counter,
        snd_state,
        ctx_count,
        ctx_array,
        active_voices,
        sorted_voices,
    })
}

// Expected 38-byte frame-sync spin loop in Visualizer.dll (RVA 0xba8c0..0xba8e6 in 2.9.31.29434):
//   cmp byte ptr [rbx], dil; je +0x21; mov rcx, [rbp+0x38]; test rcx, rcx; je +0x08;
//   mov rax, [rcx]; call [rax+0x10]; jmp +0x06; pause x4; mov eax, [rbx+0x4]; nop;
//   cmp esi, eax; jae -0x26
const EXPECTED_VIS_SPIN_LOOP: [u8; 38] = [
    0x40, 0x38, 0x3b, 0x74, 0x21,
    0x48, 0x8b, 0x4d, 0x38,
    0x48, 0x85, 0xc9, 0x74, 0x08,
    0x48, 0x8b, 0x01, 0xff, 0x50, 0x10, 0xeb, 0x06,
    0xf3, 0x90, 0xf3, 0x90, 0xf3, 0x90, 0xf3, 0x90,
    0x8b, 0x43, 0x04, 0x90, 0x3b, 0xf0, 0x73, 0xda,
];

/// Check CPUID Fn8000_0001_ECX[29] for AMD user-mode MONITORX / MWAITX support.
fn cpu_supports_monitorx() -> bool {
    #[cfg(target_arch = "x86_64")]
    {
        let max_ext = std::arch::x86_64::__cpuid(0x8000_0000).eax;
        if max_ext < 0x8000_0001 {
            return false;
        }
        (std::arch::x86_64::__cpuid(0x8000_0001).ecx & (1 << 29)) != 0
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        false
    }
}

/// Scan a mapped PE image's executable `.text` range (first section) for `EXPECTED_VIS_SPIN_LOOP`.
/// Returns `Ok(addr)` only when there is a single unique match.
unsafe fn find_visualizer_spin_loop(base: usize, image_size: usize) -> Result<usize, String> {
    let e_lfanew = ptr::read_unaligned((base + 0x3c) as *const i32) as usize;
    let pe_hdr = base + e_lfanew;
    let num_sections = ptr::read_unaligned((pe_hdr + 6) as *const u16) as usize;
    let opt_hdr_size = ptr::read_unaligned((pe_hdr + 20) as *const u16) as usize;
    let first_sec = pe_hdr + 24 + opt_hdr_size;

    let mut found: Option<usize> = None;
    for s in 0..num_sections.min(32) {
        let sec = first_sec + s * 40;
        let virt_size = ptr::read_unaligned((sec + 8) as *const u32) as usize;
        let virt_addr = ptr::read_unaligned((sec + 12) as *const u32) as usize;
        let chars = ptr::read_unaligned((sec + 36) as *const u32);
        // IMAGE_SCN_MEM_EXECUTE (0x20000000)
        if (chars & 0x2000_0000) == 0 || virt_size < EXPECTED_VIS_SPIN_LOOP.len() {
            continue;
        }
        let end = virt_addr.saturating_add(virt_size).min(image_size);
        if end <= virt_addr + EXPECTED_VIS_SPIN_LOOP.len() {
            continue;
        }
        let slice = std::slice::from_raw_parts((base + virt_addr) as *const u8, end - virt_addr);
        for (i, win) in slice.windows(EXPECTED_VIS_SPIN_LOOP.len()).enumerate() {
            if win == EXPECTED_VIS_SPIN_LOOP {
                let addr = base + virt_addr + i;
                if found.is_some() {
                    return Err("multiple Visualizer.dll spin loop matches".to_string());
                }
                found = Some(addr);
            }
        }
    }
    found.ok_or_else(|| "Visualizer.dll spin loop pattern not found".to_string())
}

// Only the first three PAUSE instructions can be replaced. The original callback returns to 20,
// where a jump lands on the fourth PAUSE at 28. Neither address is inside the patch.
const VIS_PAUSE_OFFSET: usize = 22;
const VIS_RESUME_OFFSET: usize = 30;

fn build_mwaitx_pacer_stub(resume_addr: u64, timeout_cycles: u32) -> [u8; 56] {
    let mut code = [0u8; 56];
    let [t0, t1, t2, t3] = timeout_cycles.to_le_bytes();
    let body: [u8; 48] = [
        0x51, 0x52, 0x53, // preserve RCX, RDX and RBX; the stock continuation reloads EAX
        0x48, 0x8d, 0x43, 0x04, 0x31, 0xc9, 0x31, 0xd2, 0x0f, 0x01, 0xfa, // monitor [rbx+4]
        0x40, 0x38, 0x3b, 0x74, 0x14, // cmp byte [rbx], dil; je restore
        0x3b, 0x73, 0x04, 0x72, 0x0f, // cmp esi,[rbx+4]; jb restore
        0x31, 0xc0, 0xb9, 0x02, 0x00, 0x00, 0x00, 0xbb, t0, t1, t2, t3, // C1 with timeout
        0x0f, 0x01, 0xfb, // mwaitx
        0x5b, 0x5a, 0x59, // restore RBX, RDX and RCX
        0xff, 0x25, 0, 0, 0, 0, // jmp [rip], to the untouched stock counter check
    ];
    code[..body.len()].copy_from_slice(&body);
    code[body.len()..].copy_from_slice(&resume_addr.to_le_bytes());
    code
}

fn pacer_jump(site: usize, stub: usize) -> Option<[u8; 6]> {
    let displacement = i32::try_from(stub as i64 - (site as i64 + 5)).ok()?;
    let [b0, b1, b2, b3] = displacement.to_le_bytes();
    Some([0xe9, b0, b1, b2, b3, 0x90])
}

unsafe fn patch_vis_pacer(target: *mut u8, timeout_cycles: u32) -> Result<(), String> {
    let site = target.add(VIS_PAUSE_OFFSET);
    let resume = target.add(VIS_RESUME_OFFSET) as usize as u64;
    let code = build_mwaitx_pacer_stub(resume, timeout_cycles);
    let stub = DcsCpuOpt_AllocNearPatch(site, code.len());
    if stub.is_null() {
        return Err("no nearby executable allocation for Visualizer.dll".to_string());
    }
    let Some(patch) = pacer_jump(site as usize, stub as usize) else {
        VirtualFree(stub as *mut c_void, 0, MEM_RELEASE);
        return Err("Visualizer.dll wait stub is outside rel32 reach".to_string());
    };
    ptr::copy_nonoverlapping(code.as_ptr(), stub, code.len());
    FlushInstructionCache(GetCurrentProcess(), stub as *const c_void, code.len());
    if !write_guarded_patch(site, &patch, target, &EXPECTED_VIS_SPIN_LOOP) {
        VirtualFree(stub as *mut c_void, 0, MEM_RELEASE);
        return Err("guarded Visualizer.dll patch refused".to_string());
    }
    Ok(())
}

const EXPECTED_TERRAIN_LOOKUP_PROLOGUE: [u8; 17] = [
    0x40, 0x55,                         // push rbp
    0x56,                               // push rsi
    0x41, 0x56,                         // push r14
    0x41, 0x57,                         // push r15
    0x48, 0x83, 0xec, 0x38,             // sub rsp, 0x38
    0x48, 0x8b, 0xf1,                   // mov rsi, rcx
    0x48, 0x8b, 0xea,                   // mov rbp, rdx
];

const FNV1A_PRIME_MOV: [u8; 10] = [0x49, 0xbc, 0xb3, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00];
const FNV1A_BASIS_MOV: [u8; 10] = [0x49, 0xbd, 0x25, 0x23, 0x22, 0x84, 0xe4, 0x9c, 0xf2, 0xcb];
const TERRAIN_INSERT_CALL_PREFIX: [u8; 18] = [
    0x48, 0x8d, 0x8e, 0x20, 0x02, 0x00, 0x00, // lea rcx, [rsi+0x220]
    0x4c, 0x8d, 0x44, 0x24, 0x60,             // lea r8, [rsp+0x60]
    0x48, 0x8d, 0x54, 0x24, 0x20,             // lea rdx, [rsp+0x20]
    0xe8,                                     // call rel32
];

/// Search a byte slice (representing code starting at `slice_addr`) for `FUN_15870` and its
/// `insert_set` callee. Returns `Ok((fn_addr, insert_set_addr))` on a unique match.
unsafe fn find_terrain_lookup_in_slice(
    slice: &[u8],
    slice_addr: usize,
    base: usize,
    image_size: usize,
) -> Result<Option<(usize, usize)>, String> {
    let mut found: Option<(usize, usize)> = None;
    if slice.len() < 0x190 {
        return Ok(None);
    }
    for i in 0..slice.len().saturating_sub(0x185) {
        if slice[i..i + 17] != EXPECTED_TERRAIN_LOOKUP_PROLOGUE {
            continue;
        }
        let window = &slice[i..i + 0x185];
        let has_prime = window[0x60..0xb0]
            .windows(FNV1A_PRIME_MOV.len())
            .any(|w| w == FNV1A_PRIME_MOV);
        let has_basis = window[0x60..0xb0]
            .windows(FNV1A_BASIS_MOV.len())
            .any(|w| w == FNV1A_BASIS_MOV);
        if !has_prime || !has_basis {
            continue;
        }
        let mut insert_addr = None;
        for j in 0x120..window.len().saturating_sub(TERRAIN_INSERT_CALL_PREFIX.len() + 4) {
            if window[j..j + TERRAIN_INSERT_CALL_PREFIX.len()] == TERRAIN_INSERT_CALL_PREFIX {
                let disp_off = j + TERRAIN_INSERT_CALL_PREFIX.len();
                let next_ip = slice_addr + i + disp_off + 4;
                let target = read_rip_rel32(next_ip, window.as_ptr().add(disp_off));
                if in_image(target, base, image_size) {
                    insert_addr = Some(target);
                    break;
                }
            }
        }
        if let Some(ins) = insert_addr {
            if found.is_some() {
                return Err("multiple edterrain4.dll lookup matches".to_string());
            }
            found = Some((slice_addr + i, ins));
        }
    }
    Ok(found)
}

unsafe fn find_terrain_lookup_fn(base: usize, image_size: usize) -> Result<(usize, usize), String> {
    let e_lfanew = ptr::read_unaligned((base + 0x3c) as *const i32) as usize;
    let pe_hdr = base + e_lfanew;
    let num_sections = ptr::read_unaligned((pe_hdr + 6) as *const u16) as usize;
    let opt_hdr_size = ptr::read_unaligned((pe_hdr + 20) as *const u16) as usize;
    let first_sec = pe_hdr + 24 + opt_hdr_size;

    let mut found: Option<(usize, usize)> = None;
    for s in 0..num_sections.min(32) {
        let sec = first_sec + s * 40;
        let virt_size = ptr::read_unaligned((sec + 8) as *const u32) as usize;
        let virt_addr = ptr::read_unaligned((sec + 12) as *const u32) as usize;
        let chars = ptr::read_unaligned((sec + 36) as *const u32);
        if (chars & 0x2000_0000) == 0 || virt_size < 0x200 {
            continue;
        }
        let end = virt_addr.saturating_add(virt_size).min(image_size);
        if end <= virt_addr + 0x200 {
            continue;
        }
        let slice = std::slice::from_raw_parts((base + virt_addr) as *const u8, end - virt_addr);
        if let Some(hit) = find_terrain_lookup_in_slice(slice, base + virt_addr, base, image_size)? {
            if found.is_some() {
                return Err("multiple edterrain4.dll lookup matches across sections".to_string());
            }
            found = Some(hit);
        }
    }
    found.ok_or_else(|| "edterrain4.dll lookup function not found".to_string())
}

#[derive(Clone, Copy)]
#[repr(C)]
struct FlatSlot {
    key: u32,
    val: u32,
}

#[derive(Default)]
struct FlatU32Map {
    slots: Vec<FlatSlot>,
    mask: usize,
    max_key_val: Option<u32>,
}

impl FlatU32Map {
    fn reset_with_capacity(&mut self, count: usize) {
        let cap = (count.saturating_mul(2).max(16)).next_power_of_two();
        self.slots.clear();
        self.slots.resize(cap, FlatSlot { key: u32::MAX, val: 0 });
        self.mask = cap - 1;
        self.max_key_val = None;
    }

    #[inline]
    fn insert(&mut self, key: u32, val: u32) {
        if key == u32::MAX {
            self.max_key_val = Some(val);
            return;
        }
        let mut idx = (key.wrapping_mul(0x9E37_79B1) as usize) & self.mask;
        loop {
            let slot = &mut self.slots[idx];
            if slot.key == u32::MAX || slot.key == key {
                slot.key = key;
                slot.val = val;
                return;
            }
            idx = (idx + 1) & self.mask;
        }
    }

    #[inline]
    fn get(&self, key: u32) -> u32 {
        if key == u32::MAX {
            return self.max_key_val.unwrap_or(u32::MAX);
        }
        if self.slots.is_empty() {
            return u32::MAX;
        }
        let mut idx = (key.wrapping_mul(0x9E37_79B1) as usize) & self.mask;
        loop {
            let slot = unsafe { *self.slots.get_unchecked(idx) };
            if slot.key == key {
                return slot.val;
            }
            if slot.key == u32::MAX {
                return u32::MAX;
            }
            idx = (idx + 1) & self.mask;
        }
    }
}

#[derive(Default)]
struct TerrainWorkspace {
    flat: FlatU32Map,
    matches: Vec<i32>,
}

impl TerrainWorkspace {
    /// Rebuild the lookup from the current MSVC map. The pointers and size can stay unchanged across edits.
    unsafe fn read_map(&mut self, map_obj: usize) -> bool {
        let head = ptr::read((map_obj + 0xb0) as *const usize);
        let size = ptr::read((map_obj + 0xb8) as *const usize);
        if head == 0 || size > 2_000_000 {
            return false;
        }
        self.flat.reset_with_capacity(size);
        let mut node = ptr::read(head as *const usize);
        for _ in 0..size {
            if node == 0 || node == head {
                return false;
            }
            let key = ptr::read((node + 0x10) as *const u32);
            let val = ptr::read((node + 0x14) as *const u32);
            self.flat.insert(key, val);
            node = ptr::read(node as *const usize);
        }
        node == head
    }

    /// Return chunks in the game's iteration order. Do not modify the game set until all inputs were checked.
    unsafe fn find_matches(&mut self, vec_begin: usize, count: usize, target: u32) -> bool {
        self.matches.clear();
        for i in 0..count {
            let entry = vec_begin + i * 0x1b0;
            let mut key = ptr::read((entry + 0x110) as *const usize);
            let end = ptr::read((entry + 0x118) as *const usize);
            if end < key || (end - key) % 0x40 != 0 || key == 0 && end != 0 {
                return false;
            }
            while key < end {
                if self.flat.get(ptr::read(key as *const u32)) == target {
                    self.matches.push(i as i32);
                    break;
                }
                key += 0x40;
            }
        }
        true
    }
}

thread_local! {
    // Reuse allocation capacity only. Map values and chunk matches belong to a single call.
    static TERRAIN_WORKSPACE: std::cell::RefCell<TerrainWorkspace> = Default::default();
}

type TerrainLookupFn = extern "C" fn(*mut u8, u64);
type TerrainInsertSetFn = extern "C" fn(*mut u8, *mut [usize; 2], *const i32) -> *mut [usize; 2];
type ProviderGetterFn = extern "C" fn(*mut u8) -> *mut u8;

static TERRAIN_TRAMP_PTR: AtomicU64 = AtomicU64::new(0);
static TERRAIN_INSERT_PTR: AtomicU64 = AtomicU64::new(0);
static TERRAIN_OPT_ACTIVE: AtomicBool = AtomicBool::new(false);
static TERRAIN_CALLS: AtomicU64 = AtomicU64::new(0);
static TERRAIN_QPC: AtomicU64 = AtomicU64::new(0);
static TERRAIN_HITS: AtomicU64 = AtomicU64::new(0);
static TERRAIN_REBUILDS: AtomicU64 = AtomicU64::new(0);
static TERRAIN_INV_REBUILDS: AtomicU64 = AtomicU64::new(0);

extern "C" fn hook_terrain_lookup(self_ptr: *mut u8, target_val: u64) {
    let tramp = TERRAIN_TRAMP_PTR.load(Ordering::Acquire);
    let ins_ptr = TERRAIN_INSERT_PTR.load(Ordering::Acquire);
    if !TERRAIN_OPT_ACTIVE.load(Ordering::Acquire)
        || self_ptr.is_null()
        || ins_ptr == 0
    {
        if tramp != 0 {
            let orig: TerrainLookupFn = unsafe { std::mem::transmute(tramp) };
            orig(self_ptr, target_val);
        }
        return;
    }

    let t0 = qpc_now();
    // Original compares zero-extended 32-bit `eax` against 64-bit `rbp` (`target_val`):
    // if target_val > u32::MAX, no match is possible.
    if target_val > u32::MAX as u64 {
        TERRAIN_CALLS.fetch_add(1, Ordering::Relaxed);
        return;
    }
    let target_u32 = target_val as u32;

    let ok = unsafe {
        let provider = ptr::read(self_ptr.add(0x08) as *const *mut u8);
        if provider.is_null() {
            false
        } else {
            let vtbl = ptr::read(provider as *const *const usize);
            let get_a: ProviderGetterFn = std::mem::transmute(ptr::read(vtbl.add(0x178 / 8)));
            let r15 = get_a(provider);
            let provider2 = ptr::read(self_ptr.add(0x08) as *const *mut u8);
            let vtbl2 = ptr::read(provider2 as *const *const usize);
            let get_b: ProviderGetterFn = std::mem::transmute(ptr::read(vtbl2.add(0xd0 / 8)));
            let r14 = get_b(provider2);

            if r15.is_null() || r14.is_null() {
                false
            } else {
                let vec_begin = ptr::read(r15.add(0xd8) as *const usize);
                let vec_end = ptr::read(r15.add(0xe0) as *const usize);
                if vec_end < vec_begin || (vec_end - vec_begin) % 0x1b0 != 0 || vec_begin == 0 && vec_end != 0 {
                    false
                } else {
                    let count = (vec_end - vec_begin) / 0x1b0;
                    if count == 0 {
                        true
                    } else {
                        let insert_set: TerrainInsertSetFn = std::mem::transmute(ins_ptr);
                        let set_ptr = self_ptr.add(0x220);
                        let mut local_hits = 0u64;
                        let synced = TERRAIN_WORKSPACE.with(|cell| {
                            let Ok(mut workspace) = cell.try_borrow_mut() else { return false };
                            if !workspace.read_map(r14 as usize) {
                                return false;
                            }
                            TERRAIN_REBUILDS.fetch_add(1, Ordering::Relaxed);
                            if !workspace.find_matches(vec_begin, count, target_u32) {
                                return false;
                            }
                            TERRAIN_INV_REBUILDS.fetch_add(1, Ordering::Relaxed);
                            for index in &workspace.matches {
                                let mut tmp = [0usize; 2];
                                insert_set(set_ptr, &mut tmp, index);
                                local_hits += 1;
                            }
                            true
                        });
                        if local_hits > 0 {
                            TERRAIN_HITS.fetch_add(local_hits, Ordering::Relaxed);
                        }
                        synced
                    }
                }
            }
        }
    };

    if !ok && tramp != 0 {
        let orig: TerrainLookupFn = unsafe { std::mem::transmute(tramp) };
        orig(self_ptr, target_val);
    }

    let dt = (qpc_now() - t0).max(0) as u64;
    TERRAIN_CALLS.fetch_add(1, Ordering::Relaxed);
    TERRAIN_QPC.fetch_add(dt, Ordering::Relaxed);
}

// ============================================================================
// Transport.dll!FUN_1800a1600 closest-point-on-route optimization (v0.6.0)
// ============================================================================
//
// In DCS 2.9.31.29434, Transport.dll!FUN_1800a1600 (RVA 0xa1600, identical on
// client and dedicated server) finds the closest point on a ground vehicle's
// polyline route (std::vector<PathNode>, stride 0x38 = 56 bytes) starting from
// std::upper_bound(start_s). Stock code linearly scans every remaining segment
// with TWO sqrt() calls, TWO divisions, THREE global static memory writes, and
// ZERO bounding-box pruning per segment â€” burning ~22% of the dedicated server
// Main thread under heavy moving ground traffic.
//
// Our 100% behavior-identical replacement:
//   - Uses zero inner-loop sqrt() calls (dot/len_sq projection + dist_sq compare)
//   - Zero global static scratch writes (thread-safe & register-only)
//   - Exact AABB axis fast-reject (skips >95% of distant route segments in 2 cmp)
//   - Early exit on exact zero distance (d_sq == 0.0)

const EXPECTED_TRANSPORT_ROUTE_PROLOGUE: [u8; 15] = [
    0x48, 0x89, 0x5c, 0x24, 0x18, // mov [rsp+0x18], rbx
    0x4c, 0x89, 0x4c, 0x24, 0x20, // mov [rsp+0x20], r9
    0x56,                         // push rsi
    0x41, 0x54,                   // push r12
    0x41, 0x55,                   // push r13
];

const TRANSPORT_ROUTE_SIGNATURE: [u8; 47] = [
    0x48, 0x89, 0x5c, 0x24, 0x18,
    0x4c, 0x89, 0x4c, 0x24, 0x20,
    0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x81, 0xec, 0x10, 0x01, 0x00, 0x00,
    0x48, 0x8b, 0x71, 0x18,
    0x4d, 0x8b, 0xf1,
    0x4c, 0x8b, 0x59, 0x20,
    0x48, 0xb8, 0x25, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49,
];

#[repr(C)]
#[derive(Clone, Copy, Debug)]
struct PathNode {
    s: f64,       // +0x00 cumulative arc-length
    seg_idx: u32, // +0x08 waypoint/segment index
    _pad: u32,    // +0x0c padding
    x: f64,       // +0x10 world X
    z: f64,       // +0x18 world Z
    f4: f64,      // +0x20 interpolated parameter 4
    f5: f64,      // +0x28 interpolated parameter 5
    f6: f64,      // +0x30 interpolated parameter 6
}

type TransportRouteFn = extern "C" fn(
    *const u8,
    *mut PathNode,
    *const [f64; 2],
    *mut f64,
    f64,
) -> *mut PathNode;

static ROUTE_TRAMP_PTR: AtomicU64 = AtomicU64::new(0);
static ROUTE_OPT_ACTIVE: AtomicBool = AtomicBool::new(false);
static ROUTE_CALLS: AtomicU64 = AtomicU64::new(0);
static ROUTE_QPC: AtomicU64 = AtomicU64::new(0);

unsafe fn find_transport_route_fn(base: usize, image_size: usize) -> Result<usize, String> {
    let pe_hdr = base + ptr::read_unaligned((base + 0x3c) as *const i32) as usize;
    let num_sections = ptr::read_unaligned((pe_hdr + 6) as *const u16) as usize;
    let opt_hdr_size = ptr::read_unaligned((pe_hdr + 20) as *const u16) as usize;
    let sec_table = pe_hdr + 24 + opt_hdr_size;

    let mut hit: Option<usize> = None;
    for s in 0..num_sections {
        let sh = sec_table + s * 40;
        let chars = ptr::read_unaligned((sh + 36) as *const u32);
        if (chars & 0x2000_0000) == 0 {
            continue;
        }
        let va = ptr::read_unaligned((sh + 12) as *const u32) as usize;
        let vsize = ptr::read_unaligned((sh + 8) as *const u32) as usize;
        if va >= image_size || vsize < TRANSPORT_ROUTE_SIGNATURE.len() {
            continue;
        }
        let len = vsize.min(image_size - va);
        let code = std::slice::from_raw_parts((base + va) as *const u8, len);
        for i in 0..=code.len() - TRANSPORT_ROUTE_SIGNATURE.len() {
            if code[i] == 0x48
                && code[i + 1] == 0x89
                && code[i + 37] == 0x48
                && code[i + 38] == 0xb8
                && code[i..i + TRANSPORT_ROUTE_SIGNATURE.len()] == TRANSPORT_ROUTE_SIGNATURE
            {
                let addr = base + va + i;
                if hit.is_some() {
                    return Err("ambiguous_route_match".to_string());
                }
                hit = Some(addr);
            }
        }
    }
    hit.ok_or_else(|| "route_pattern_not_found".to_string())
}

#[inline]
fn find_closest_point_fast(
    nodes: &[PathNode],
    qx: f64,
    qz: f64,
    start_s: f64,
) -> (PathNode, f64) {
    let n = nodes.len();
    if n < 2 {
        return (
            PathNode {
                s: -1.0,
                seg_idx: u32::MAX,
                _pad: 0,
                x: 0.0,
                z: 0.0,
                f4: f64::MAX,
                f5: 0.0,
                f6: 0.0,
            },
            f64::MAX,
        );
    }

    // std::upper_bound on PathNode::s for start_s
    let mut lo = 0usize;
    let mut rem = n;
    while rem > 0 {
        let half = rem >> 1;
        let mid = lo + half;
        if nodes[mid].s <= start_s {
            lo = mid + 1;
            rem -= half + 1;
        } else {
            rem = half;
        }
    }

    if lo == n {
        let last = nodes[n - 1];
        let dx = last.x - qx;
        let dz = last.z - qz;
        return (last, (dx * dx + dz * dz).sqrt());
    }

    let (start_i, anchor_i) = if lo == 0 { (0, 1) } else { (lo - 1, lo) };
    let anchor = nodes[anchor_i];
    let anchor_seg_idx = anchor.seg_idx;

    let mut best_dist_sq = f64::MAX;
    let mut qx_lo = f64::NEG_INFINITY;
    let mut qx_hi = f64::INFINITY;
    let mut qz_lo = f64::NEG_INFINITY;
    let mut qz_hi = f64::INFINITY;

    let mut best_s = f64::MAX;
    let mut best_seg_idx = anchor_seg_idx;
    let mut best_x = 0.0f64;
    let mut best_z = 0.0f64;
    let mut best_f4 = start_s;
    let mut best_f5 = 0.0f64;
    let mut best_f6 = 0.0f64;

    // Read the current route on every call. The game may change a path in place.
    for segment in nodes[start_i..].windows(2) {
        let a = &segment[0];
        let b = &segment[1];
        let ax = a.x;
        let bx = b.x;
        if (ax < qx_lo && bx < qx_lo) || (ax > qx_hi && bx > qx_hi) {
            continue;
        }
        let az = a.z;
        let bz = b.z;
        if (az < qz_lo && bz < qz_lo) || (az > qz_hi && bz > qz_hi) {
            continue;
        }

        let abx = bx - ax;
        let abz = bz - az;
        let len_sq = abx * abx + abz * abz;
        let (t, px, pz) = if len_sq > 0.0 {
            let dot = (qx - ax) * abx + (qz - az) * abz;
            if dot <= 0.0 {
                (0.0, ax, az)
            } else if dot >= len_sq {
                (1.0, bx, bz)
            } else {
                let t = dot / len_sq;
                (t, ax + t * abx, az + t * abz)
            }
        } else {
            (1.0, bx, bz)
        };

        let dx = qx - px;
        let dz = qz - pz;
        let d_sq = dx * dx + dz * dz;
        if d_sq < best_dist_sq {
            if start_s > 0.0 && a.seg_idx > anchor_seg_idx {
                best_s = anchor.s + 0.0001;
                best_seg_idx = anchor.seg_idx;
                best_x = anchor.x;
                best_z = anchor.z;
                best_f4 = anchor.f4;
                best_f5 = anchor.f5;
                best_f6 = anchor.f6;
                break;
            }
            best_dist_sq = d_sq;
            let pad = d_sq.sqrt() * 1.000000000001;
            qx_lo = qx - pad;
            qx_hi = qx + pad;
            qz_lo = qz - pad;
            qz_hi = qz + pad;

            best_seg_idx = a.seg_idx;
            best_s = a.s + (b.s - a.s) * t;
            best_x = px;
            best_z = pz;
            best_f4 = a.f4 + (b.f4 - a.f4) * t;
            best_f5 = a.f5 + (b.f5 - a.f5) * t;
            best_f6 = a.f6 + (b.f6 - a.f6) * t;
            if d_sq == 0.0 {
                break;
            }
        }
    }

    let dx = best_x - qx;
    let dz = best_z - qz;
    let out_dist = (dx * dx + dz * dz).sqrt();
    (
        PathNode {
            s: best_s,
            seg_idx: best_seg_idx,
            _pad: 0,
            x: best_x,
            z: best_z,
            f4: best_f4,
            f5: best_f5,
            f6: best_f6,
        },
        out_dist,
    )
}

extern "C" fn hook_transport_route(
    this_ptr: *const u8,
    out_node: *mut PathNode,
    query_xz: *const [f64; 2],
    out_dist: *mut f64,
    start_s: f64,
) -> *mut PathNode {
    let tramp = ROUTE_TRAMP_PTR.load(Ordering::Acquire);
    if !ROUTE_OPT_ACTIVE.load(Ordering::Acquire)
        || this_ptr.is_null()
        || out_node.is_null()
        || query_xz.is_null()
        || out_dist.is_null()
    {
        if tramp != 0 {
            let orig: TransportRouteFn = unsafe { std::mem::transmute(tramp) };
            return orig(this_ptr, out_node, query_xz, out_dist, start_s);
        }
        return out_node;
    }

    let t0 = qpc_now();
    unsafe {
        let begin = ptr::read((this_ptr.add(0x18)) as *const *const PathNode);
        let end = ptr::read((this_ptr.add(0x20)) as *const *const PathNode);
        let byte_len = (end as isize).wrapping_sub(begin as isize);
        let stride = std::mem::size_of::<PathNode>() as isize;
        let nodes = if !begin.is_null() && byte_len >= stride && (byte_len % stride) == 0 {
            std::slice::from_raw_parts(begin, (byte_len / stride) as usize)
        } else {
            &[]
        };
        let q = *query_xz;
        let (node, dist) = find_closest_point_fast(nodes, q[0], q[1], start_s);
        *out_node = node;
        *out_dist = dist;
    }
    let dt = (qpc_now() - t0).max(0) as u64;
    ROUTE_CALLS.fetch_add(1, Ordering::Relaxed);
    ROUTE_QPC.fetch_add(dt, Ordering::Relaxed);
    out_node
}

fn try_install_transport_route_opt() -> Option<(*mut u8, String)> {
    let hmod = unsafe { GetModuleHandleA(b"Transport.dll\0".as_ptr()) };
    if hmod.is_null() {
        return None;
    }
    let base = hmod as usize;
    let sz = match unsafe { verified_image_size(base, 0x6afebf8b, 0x367000) } {
        Some(s) => s,
        None => return Some((ptr::null_mut(), "skip(unverified_dcs_build)".to_string())),
    };
    let fn_addr = match unsafe { find_transport_route_fn(base, sz) } {
        Ok(a) => a,
        Err(e) => return Some((ptr::null_mut(), format!("skip({})", e))),
    };
    let tt = fn_addr as *mut u8;
    let detour = hook_transport_route as *const () as usize as u64;
    match unsafe { patch_inline(tt, &EXPECTED_TRANSPORT_ROUTE_PROLOGUE, detour, &ROUTE_TRAMP_PTR) } {
        Ok(()) => {
            ROUTE_OPT_ACTIVE.store(true, Ordering::Release);
            Some((tt, format!("fast_route@0x{:x}", fn_addr - base)))
        }
        Err(e) => Some((ptr::null_mut(), format!("err({})", e))),
    }
}

const PAGE_EXECUTE_READWRITE: u32 = 0x40;
const MEM_COMMIT: u32 = 0x1000;
const MEM_RESERVE: u32 = 0x2000;
const MEM_RELEASE: u32 = 0x8000;
const GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS: u32 = 0x04;
const GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT: u32 = 0x02;
const GET_MODULE_HANDLE_EX_FLAG_PIN: u32 = 0x01;

#[link(name = "kernel32")]
extern "system" {
    fn GetModuleHandleA(lpModuleName: *const u8) -> *mut c_void;
    fn GetModuleHandleExW(
        dwFlags: u32,
        lpModuleName: *const c_void,
        phModule: *mut *mut c_void,
    ) -> i32;
    fn GetModuleFileNameW(hModule: *mut c_void, lpFilename: *mut u16, nSize: u32) -> u32;
    fn GetProcAddress(hModule: *mut c_void, lpProcName: *const u8) -> *mut c_void;
    fn VirtualProtect(
        lpAddress: *mut c_void,
        dwSize: usize,
        flNewProtect: u32,
        lpflOldProtect: *mut u32,
    ) -> i32;
    fn VirtualAlloc(
        lpAddress: *mut c_void,
        dwSize: usize,
        flAllocationType: u32,
        flProtect: u32,
    ) -> *mut c_void;
    fn VirtualFree(lpAddress: *mut c_void, dwSize: usize, dwFreeType: u32) -> i32;
    fn FlushInstructionCache(
        hProcess: *mut c_void,
        lpBaseAddress: *const c_void,
        dwSize: usize,
    ) -> i32;
    fn GetCurrentProcess() -> *mut c_void;
    fn QueryPerformanceCounter(lpPerformanceCount: *mut i64) -> i32;
    fn QueryPerformanceFrequency(lpFrequency: *mut i64) -> i32;
}

type SndProcessFn = extern "C" fn();
type UpdateSrcFn = extern "C" fn(*mut u8);

#[derive(Clone, Copy, PartialEq, Eq)]
enum CullMode {
    Measure,
    Cull,
    Cull60,
}

impl CullMode {
    fn as_str(self) -> &'static str {
        match self {
            CullMode::Measure => "measure",
            CullMode::Cull => "cull",
            CullMode::Cull60 => "cull60",
        }
    }

    fn uses_cull(self) -> bool {
        matches!(self, CullMode::Cull | CullMode::Cull60)
    }
}

struct Config {
    enabled: bool,
    mode: CullMode,
    max_hz: i64,
    cull_mask: i32,
    safe_margin_m: f64,
    log_interval_sec: i64,
    low_power_pacer: bool,
    pacer_timeout_cycles: u32,
    terrain_lookup_opt: bool,
    transport_route_opt: bool,
    zlib_inflate_opt: bool,
    sound_cull: bool,
    ini_path: PathBuf,
    log_path: PathBuf,
}

struct HookState {
    mode: CullMode,
    max_hz: i64,
    log_interval_sec: i64,
    log_path: PathBuf,
    addrs: Option<ResolvedAddrs>,
    terrain_opt_wanted: bool,
    terrain_last_probe_qpc: i64,
    terrain_target: *mut u8,
    route_opt_wanted: bool,
    route_last_probe_qpc: i64,
    route_target: *mut u8,
    zlib_opt_wanted: bool,
    zlib_last_probe_qpc: i64,
    zlib_installed: bool,
    qpc_freq: i64,
    window_start_qpc: i64,
    last_exec_qpc: i64,
    calls_exec: u64,
    calls_skip: u64,
    total_qpc: u64,
}

unsafe impl Send for HookState {}

static STATE: Mutex<Option<HookState>> = Mutex::new(None);
static INIT_LOCK: Mutex<()> = Mutex::new(());
static SND_TRAMP_PTR: AtomicU64 = AtomicU64::new(0);
static UPD_TRAMP_PTR: AtomicU64 = AtomicU64::new(0);
static FRAME_COUNTER_ADDR: AtomicU64 = AtomicU64::new(0);
static HOOK_ACTIVE: AtomicBool = AtomicBool::new(false);
static CULL_ENABLED: AtomicBool = AtomicBool::new(false);
static IN_SND_PROCESS: AtomicBool = AtomicBool::new(false);
/// True once this DLL has pinned itself in memory (it can then never be unloaded while patches point into it).
static DLL_PINNED: AtomicBool = AtomicBool::new(false);
/// Number of `luaopen_dcs_cpu_opt` calls seen in this process (one per Lua state that loads the hook).
static INIT_CALLS: AtomicU64 = AtomicU64::new(0);
/// Number of `DcsCpuOpt_Shutdown` calls seen in this process (one per Lua state close / guard GC).
static LUA_CLOSE_CALLS: AtomicU64 = AtomicU64::new(0);
static mut SRC_EXEC_COUNT: u64 = 0;
static mut SRC_CULL_COUNT: u64 = 0;
static mut CACHED_FRAME: i32 = 0;
static mut CULL_MASK: i32 = 15;
static mut SAFE_MARGIN_M: f64 = 2000.0;

fn qpc_now() -> i64 {
    let mut v: i64 = 0;
    unsafe { QueryPerformanceCounter(&mut v) };
    v
}

fn qpc_freq() -> i64 {
    let mut v: i64 = 1;
    unsafe { QueryPerformanceFrequency(&mut v) };
    v.max(1)
}

fn self_dll_dir() -> PathBuf {
    unsafe {
        let mut hmod: *mut c_void = ptr::null_mut();
        let ok = GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            self_dll_dir as *const () as *const c_void,
            &mut hmod,
        );
        if ok != 0 && !hmod.is_null() {
            let mut buf = [0u16; 1024];
            let len = GetModuleFileNameW(hmod, buf.as_mut_ptr(), buf.len() as u32) as usize;
            if len > 0 && len < buf.len() {
                let path = PathBuf::from(String::from_utf16_lossy(&buf[..len]));
                if let Some(parent) = path.parent() {
                    return parent.to_path_buf();
                }
            }
        }
    }
    PathBuf::from(".")
}

/// Pins this DLL for the life of the process so `FreeLibrary` (e.g. from a closing Lua state) can never unload
/// code that live patches jump into. Idempotent; returns true when the module is pinned.
fn pin_self_module() -> bool {
    if DLL_PINNED.load(Ordering::Acquire) {
        return true;
    }
    let mut hmod: *mut c_void = ptr::null_mut();
    let ok = unsafe {
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            pin_self_module as *const () as *const c_void,
            &mut hmod,
        )
    };
    let pinned = ok != 0 && !hmod.is_null();
    if pinned {
        DLL_PINNED.store(true, Ordering::Release);
    }
    pinned
}

fn append_log(path: &PathBuf, line: &str) {
    if let Ok(mut f) = OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(f, "{}", line);
    }
}

fn read_config() -> Config {
    let dll_dir = self_dll_dir();
    let ini_path = dll_dir.join("DcsCpuOpt.ini");
    let log_path = dll_dir.join("DcsCpuOpt.log");

    let mut enabled = false;
    let mut sound_cull = false;
    let mut mode = CullMode::Cull60;
    let mut max_hz: i64 = 60;
    let mut divisor_opt: Option<u32> = None;
    let mut safe_margin_m: f64 = 2000.0;
    let mut log_interval_sec: i64 = 30;
    let mut low_power_pacer = false;
    let mut pacer_timeout_cycles: u32 = 200_000;
    let mut terrain_lookup_opt = false;
    let mut transport_route_opt = false;
    let mut zlib_inflate_opt = false;

    if let Ok(text) = fs::read_to_string(&ini_path) {
        for raw in text.lines() {
            let line = raw.trim();
            if line.is_empty() || line.starts_with(';') || line.starts_with('#') || line.starts_with('[') {
                continue;
            }
            if let Some((k, v)) = line.split_once('=') {
                let key = k.trim().to_ascii_lowercase();
                let val = v.trim();
                match key.as_str() {
                    "enabled" => enabled = val == "1",
                    "soundcull" => sound_cull = val == "1",
                    "mode" => {
                        mode = match val.to_ascii_lowercase().as_str() {
                            "measure" => CullMode::Measure,
                            "cull" => CullMode::Cull,
                            _ => CullMode::Cull60,
                        };
                    }
                    "maxhz" => {
                        if let Ok(n) = val.parse::<i64>() {
                            max_hz = n.clamp(30, 240);
                        }
                    }
                    "culldivisor" => {
                        if let Ok(n) = val.parse::<u32>() {
                            divisor_opt = Some(n);
                        }
                    }
                    "safemarginmeters" => {
                        if let Ok(n) = val.parse::<f64>() {
                            safe_margin_m = n.clamp(500.0, 20000.0);
                        }
                    }
                    "logintervalsec" => {
                        if let Ok(n) = val.parse::<i64>() {
                            log_interval_sec = n.clamp(0, 3600);
                        }
                    }
                    "lowpowerpacer" => {
                        low_power_pacer = val == "1";
                    }
                    "pacertimeoutcycles" => {
                        if let Ok(n) = val.parse::<u32>() {
                            pacer_timeout_cycles = n.clamp(10_000, 10_000_000);
                        }
                    }
                    "terrainlookupopt" => {
                        terrain_lookup_opt = val == "1";
                    }
                    "transportrouteopt" => {
                        transport_route_opt = val == "1";
                    }
                    "zlibinflateopt" => {
                        zlib_inflate_opt = val == "1";
                    }
                    _ => {}
                }
            }
        }
    }

    let default_div = if mode == CullMode::Cull60 { 16 } else { 32 };
    let div = divisor_opt
        .unwrap_or(default_div)
        .clamp(2, 64)
        .next_power_of_two()
        .min(64);
    let cull_mask = (div - 1) as i32;

    Config {
        enabled,
        mode,
        max_hz,
        cull_mask,
        safe_margin_m,
        log_interval_sec,
        low_power_pacer,
        pacer_timeout_cycles,
        terrain_lookup_opt,
        transport_route_opt,
        zlib_inflate_opt,
        sound_cull,
        ini_path,
        log_path,
    }
}

/// Count total nodes across all context 0xb0 source lists.
unsafe fn sample_engine_counts(addrs: &ResolvedAddrs) -> (i32, i32, i32, i32, usize) {
    let state = addrs
        .snd_state
        .map(|a| ptr::read_volatile(a as *const i32))
        .unwrap_or(-1);
    let ctx_count = addrs
        .ctx_count
        .map(|a| ptr::read_volatile(a as *const i32).clamp(0, 8))
        .unwrap_or(0);
    let active_voices = addrs
        .active_voices
        .map(|a| ptr::read_volatile(a as *const i32))
        .unwrap_or(0);
    let sorted_voices = addrs
        .sorted_voices
        .map(|a| ptr::read_volatile(a as *const i32))
        .unwrap_or(0);
    let mut total_sources: usize = 0;
    if let Some(ctx_array) = addrs.ctx_array {
        for i in 0..(ctx_count as usize) {
            let head = (ctx_array + i * CTX_STRIDE + CTX_LIST_OFF) as *const usize;
            let mut cur = ptr::read_volatile(head) as *const usize;
            let mut guard = 0usize;
            while !cur.is_null() && cur != head && guard < 100_000 {
                total_sources += 1;
                cur = ptr::read_volatile(cur) as *const usize;
                guard += 1;
            }
        }
    }
    (state, ctx_count, active_voices, sorted_voices, total_sources)
}

fn flush_window(st: &mut HookState, now_qpc: i64, reason: &str) {
    let elapsed_s = (now_qpc - st.window_start_qpc) as f64 / st.qpc_freq as f64;
    let total_ms = (st.total_qpc as f64 * 1000.0) / st.qpc_freq as f64;
    let pct_core = if elapsed_s > 0.0 {
        (total_ms / (elapsed_s * 1000.0)) * 100.0
    } else {
        0.0
    };
    let mean_us = if st.calls_exec > 0 {
        (total_ms * 1000.0) / st.calls_exec as f64
    } else {
        0.0
    };
    let (snd_state, ctx_count, active_voices, sorted_voices, total_sources) =
        st.addrs.as_ref().map(|addrs| unsafe { sample_engine_counts(addrs) }).unwrap_or((-1, -1, -1, -1, 0));
    let (src_exec, src_cull) = unsafe {
        let e = SRC_EXEC_COUNT;
        let c = SRC_CULL_COUNT;
        SRC_EXEC_COUNT = 0;
        SRC_CULL_COUNT = 0;
        (e, c)
    };
    let cull_pct = if src_exec + src_cull > 0 {
        (src_cull as f64 * 100.0) / ((src_exec + src_cull) as f64)
    } else {
        0.0
    };

    let terr_calls = TERRAIN_CALLS.swap(0, Ordering::Relaxed);
    let terr_qpc = TERRAIN_QPC.swap(0, Ordering::Relaxed);
    let terr_hits = TERRAIN_HITS.swap(0, Ordering::Relaxed);
    let terr_rebuilds = TERRAIN_REBUILDS.swap(0, Ordering::Relaxed);
    let terr_inv_rebuilds = TERRAIN_INV_REBUILDS.swap(0, Ordering::Relaxed);
    let terr_suffix = if !st.terrain_target.is_null() || terr_calls > 0 {
        let terr_ms = (terr_qpc as f64 * 1000.0) / st.qpc_freq as f64;
        let terr_core = if elapsed_s > 0.0 {
            (terr_ms / (elapsed_s * 1000.0)) * 100.0
        } else {
            0.0
        };
        format!(
            " terr_calls={} terr_core={:.2}% terr_hits={} terr_rebuilds={}/{}",
            terr_calls, terr_core, terr_hits, terr_rebuilds, terr_inv_rebuilds
        )
    } else {
        String::new()
    };

    let route_calls = ROUTE_CALLS.swap(0, Ordering::Relaxed);
    let route_qpc = ROUTE_QPC.swap(0, Ordering::Relaxed);
    let route_suffix = if !st.route_target.is_null() || route_calls > 0 {
        let route_ms = (route_qpc as f64 * 1000.0) / st.qpc_freq as f64;
        let route_core = if elapsed_s > 0.0 {
            (route_ms / (elapsed_s * 1000.0)) * 100.0
        } else {
            0.0
        };
        format!(
            " route_calls={} route_core={:.2}%",
            route_calls, route_core
        )
    } else {
        String::new()
    };

    let zlib_fast = zlib_opt::ZLIB_FAST.swap(0, Ordering::Relaxed);
    let zlib_bytes = zlib_opt::ZLIB_FAST_BYTES.swap(0, Ordering::Relaxed);
    let zlib_qpc = zlib_opt::ZLIB_FAST_QPC.swap(0, Ordering::Relaxed);
    let zlib_fallback = zlib_opt::ZLIB_FALLBACK.swap(0, Ordering::Relaxed);
    let zlib_pass = zlib_opt::ZLIB_PASS.swap(0, Ordering::Relaxed);
    let zlib_suffix = if st.zlib_installed || zlib_fast > 0 {
        format!(
            " zlib_fast={} zlib_fast_mb={:.1} zlib_fast_ms={:.1} zlib_fallback={} zlib_pass={}",
            zlib_fast,
            zlib_bytes as f64 / 1_048_576.0,
            (zlib_qpc as f64 * 1000.0) / st.qpc_freq as f64,
            zlib_fallback,
            zlib_pass
        )
    } else {
        String::new()
    };

    let line = format!(
        "event={} mode={} dt_s={:.1} exec={} skip={} snd_core={:.2}% mean_us={:.0} snd_state={} ctx={} voices={}/{} sources={} culled={:.1}%{}{}{}",
        reason,
        st.mode.as_str(),
        elapsed_s,
        st.calls_exec,
        st.calls_skip,
        pct_core,
        mean_us,
        snd_state,
        ctx_count,
        active_voices,
        sorted_voices,
        total_sources,
        cull_pct,
        terr_suffix,
        route_suffix,
        zlib_suffix,
    );
    append_log(&st.log_path, &line);

    st.window_start_qpc = now_qpc;
    st.calls_exec = 0;
    st.calls_skip = 0;
    st.total_qpc = 0;
}

extern "C" fn hook_update_source(src: *mut u8) {
    let tramp = UPD_TRAMP_PTR.load(Ordering::Acquire);
    if tramp == 0 {
        return;
    }

    if CULL_ENABLED.load(Ordering::Acquire)
        && IN_SND_PROCESS.load(Ordering::Relaxed)
        && !src.is_null()
    {
        unsafe {
            let cur_frame = CACHED_FRAME;
            let mask = CULL_MASK;
            let slot = (((src as usize) >> 6) as i32).wrapping_add(cur_frame) & mask;
            if slot != 0 {
                let voice_ptr = ptr::read((src.add(SRC_VOICE_PTR)) as *const *const u8);
                let active_flag = ptr::read((src.add(SRC_ACTIVE_FLAG)) as *const i32);
                let last_frame = ptr::read((src.add(SRC_LAST_FRAME)) as *const i32);
                if voice_ptr.is_null()
                    && active_flag == 0
                    && last_frame != 0
                    && cur_frame.wrapping_sub(last_frame) <= mask
                {
                    let qtree_item = ptr::read((src.add(SRC_QTREE_ITEM)) as *const *const u8);
                    let host_link = ptr::read((src.add(SRC_HOST_LINK)) as *const *const u8);
                    let ctx = ptr::read((src.add(SRC_CTX_PTR)) as *const *const u8);
                    if !qtree_item.is_null()
                        && !host_link.is_null()
                        && !ctx.is_null()
                        && ptr::read(ctx.add(CTX_IS_2D)) == 0
                    {
                        let lx = ptr::read(ctx.add(CTX_LISTENER_X) as *const f64);
                        let lz = ptr::read(ctx.add(CTX_LISTENER_Z) as *const f64);
                        let sx = ptr::read(src.add(SRC_WORLD_X) as *const f64);
                        let sz = ptr::read(src.add(SRC_WORLD_Z) as *const f64);
                        let max_r = ptr::read(src.add(SRC_MAX_RADIUS) as *const f32) as f64;
                        let dx = sx - lx;
                        let dz = sz - lz;
                        let safe_r = (max_r * 4.0).max(500.0) + SAFE_MARGIN_M;
                        if dx * dx + dz * dz > safe_r * safe_r {
                            SRC_CULL_COUNT += 1;
                            return;
                        }
                    }
                }
            }
            SRC_EXEC_COUNT += 1;
        }
    }

    let orig: UpdateSrcFn = unsafe { std::mem::transmute(tramp) };
    orig(src);
}

extern "C" fn hook_snd_process() {
    let tramp = SND_TRAMP_PTR.load(Ordering::Acquire);
    if tramp == 0 || !HOOK_ACTIVE.load(Ordering::Acquire) {
        if tramp != 0 {
            let orig: SndProcessFn = unsafe { std::mem::transmute(tramp) };
            orig();
        }
        return;
    }

    let mut do_skip = false;
    let now_before = qpc_now();

    if let Ok(mut guard) = STATE.lock() {
        if let Some(st) = guard.as_mut() {
            if st.mode == CullMode::Cull60 {
                let min_dt = st.qpc_freq / st.max_hz;
                if now_before - st.last_exec_qpc < min_dt {
                    do_skip = true;
                } else {
                    st.last_exec_qpc = now_before;
                }
            }
            if do_skip {
                st.calls_skip += 1;
                if st.log_interval_sec > 0
                    && (now_before - st.window_start_qpc) >= st.qpc_freq * st.log_interval_sec
                {
                    flush_window(st, now_before, "interval");
                }
            }
        }
    }

    if do_skip {
        return;
    }

    unsafe {
        let fc_addr = FRAME_COUNTER_ADDR.load(Ordering::Relaxed) as usize;
        if fc_addr != 0 {
            CACHED_FRAME = ptr::read(fc_addr as *const i32).wrapping_add(1);
        }
    }
    IN_SND_PROCESS.store(true, Ordering::Relaxed);
    let t0 = qpc_now();
    let orig: SndProcessFn = unsafe { std::mem::transmute(tramp) };
    orig();
    let t1 = qpc_now();
    IN_SND_PROCESS.store(false, Ordering::Relaxed);
    let dt = (t1 - t0).max(0) as u64;

    if let Ok(mut guard) = STATE.lock() {
        if let Some(st) = guard.as_mut() {
            st.calls_exec += 1;
            st.total_qpc += dt;
            if st.log_interval_sec > 0
                && (t1 - st.window_start_qpc) >= st.qpc_freq * st.log_interval_sec
            {
                flush_window(st, t1, "interval");
            }
        }
    }
}

unsafe fn patch_inline<const N: usize>(
    target: *mut u8,
    expected: &[u8; N],
    detour: u64,
    original: &AtomicU64,
) -> Result<(), String> {
    let tramp = VirtualAlloc(
        ptr::null_mut(),
        64,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE,
    ) as *mut u8;
    if tramp.is_null() {
        return Err("VirtualAlloc failed for the trampoline".to_string());
    }
    let jmp: [u8; 6] = [0xff, 0x25, 0, 0, 0, 0];
    let resume_addr = (target as usize + N) as u64;
    ptr::copy_nonoverlapping(expected.as_ptr(), tramp, N);
    ptr::copy_nonoverlapping(jmp.as_ptr(), tramp.add(N), jmp.len());
    ptr::copy_nonoverlapping(resume_addr.to_le_bytes().as_ptr(), tramp.add(N + jmp.len()), 8);
    FlushInstructionCache(GetCurrentProcess(), tramp as *const c_void, 64);

    let mut patch = [0x90u8; N];
    patch[..6].copy_from_slice(&jmp);
    patch[6..14].copy_from_slice(&detour.to_le_bytes());
    // Threads can run as soon as the guarded write returns. Publish the callable original first.
    original.store(tramp as u64, Ordering::Release);
    if !write_guarded_patch(target, &patch, target, expected) {
        // A refused single-site write installed no jump, so no caller can use this trampoline.
        original.store(0, Ordering::Release);
        VirtualFree(tramp as *mut c_void, 0, MEM_RELEASE);
        return Err(format!("guarded patch refused at 0x{:x}", target as usize));
    }
    Ok(())
}

/// Attempt to locate and hook `edterrain4.dll` lookup loop (`FUN_15870`).
/// Returns `Some((target, tramp, status_msg))` when `edterrain4.dll` is loaded and patched,
/// or `None` if `edterrain4.dll` is not loaded yet (will be retried lazily).
fn try_install_terrain_opt() -> Option<(*mut u8, String)> {
    let hterr = unsafe { GetModuleHandleA(b"edterrain4.dll\0".as_ptr()) };
    if hterr.is_null() {
        return None;
    }
    let tbase = hterr as usize;
    let tsz = match unsafe { verified_image_size(tbase, 0x6ae721cb, 0x14c2000) } {
        Some(s) => s,
        None => return Some((ptr::null_mut(), "skip(unverified_dcs_build)".to_string())),
    };
    let (fn_addr, ins_addr) = match unsafe { find_terrain_lookup_fn(tbase, tsz) } {
        Ok(pair) => pair,
        Err(e) => return Some((ptr::null_mut(), format!("skip({})", e))),
    };
    let tt = fn_addr as *mut u8;
    let detour = hook_terrain_lookup as *const () as usize as u64;
    match unsafe { patch_inline(tt, &EXPECTED_TERRAIN_LOOKUP_PROLOGUE, detour, &TERRAIN_TRAMP_PTR) } {
        Ok(()) => {
            TERRAIN_INSERT_PTR.store(ins_addr as u64, Ordering::Release);
            TERRAIN_OPT_ACTIVE.store(true, Ordering::Release);
            Some((
                tt,
                format!("flat_hash@0x{:x}(ins=0x{:x})", fn_addr - tbase, ins_addr - tbase),
            ))
        }
        Err(e) => Some((ptr::null_mut(), format!("err({})", e))),
    }
}

/// Attempt to redirect edCore.dll's zlib1.dll inflate imports (`ZlibInflateOpt`).
/// Returns `None` while edCore.dll / zlib1.dll are not loaded yet (retried lazily), otherwise the installed
/// state (if any) and a status string for the log.
fn try_install_zlib_opt() -> Option<(bool, String)> {
    match zlib_opt::try_install() {
        Ok(None) => None,
        Ok(Some(msg)) => Some((true, msg)),
        Err(e) => Some((false, format!("skip({})", e))),
    }
}

fn install_sound(cfg: &Config) -> Result<ResolvedAddrs, String> {
    let hmod = unsafe { GetModuleHandleA(b"ed_sound.dll\0".as_ptr()) };
    if hmod.is_null() {
        return Err("ed_sound.dll not loaded".to_string());
    }
    let base = hmod as usize;
    let image_size = unsafe { verified_image_size(base, 0x6ae7205a, 0xd0c4000) }
        .ok_or_else(|| format!("unverified ed_sound.dll image at 0x{:x}", base))?;

    let snd_target = unsafe { GetProcAddress(hmod, b"SND_process\0".as_ptr()) } as *mut u8;
    if snd_target.is_null() {
        return Err("SND_process export not found".to_string());
    }

    let addrs = unsafe { resolve_from_snd_process(base, image_size, snd_target as usize)? };
    FRAME_COUNTER_ADDR.store(addrs.frame_counter as u64, Ordering::Release);

    if cfg.mode.uses_cull() {
        unsafe {
            CULL_MASK = cfg.cull_mask;
            SAFE_MARGIN_M = cfg.safe_margin_m;
            let target = addrs.update_source as *mut u8;
            let detour = hook_update_source as *const () as usize as u64;
            patch_inline(target, &EXPECTED_UPDATE_SRC_PROLOGUE, detour, &UPD_TRAMP_PTR)?;
        }
    }

    let detour = hook_snd_process as *const () as usize as u64;
    // If this second patch is refused, the first stays a pass-through for the rest of the process.
    // Restoring it here would race callers already executing its trampoline.
    unsafe { patch_inline(snd_target, &EXPECTED_SND_PROCESS_PROLOGUE, detour, &SND_TRAMP_PTR)? };
    CULL_ENABLED.store(cfg.mode.uses_cull(), Ordering::Release);
    Ok(addrs)
}

fn install_hook() -> Result<String, String> {
    let cfg = read_config();
    if !cfg.enabled {
        let msg = format!(
            "event=disabled ini={}",
            cfg.ini_path.display()
        );
        append_log(&cfg.log_path, &msg);
        return Ok(msg);
    }

    if !pin_self_module() {
        return Err("could not pin the module".into());
    }

    let now = qpc_now();
    let (addrs, sound_status) = if cfg.sound_cull {
        match install_sound(&cfg) {
            Ok(addrs) => (Some(addrs), "on".to_string()),
            Err(e) => (None, format!("skip({e})")),
        }
    } else {
        (None, "disabled".to_string())
    };

    let pacer_status = if !cfg.low_power_pacer {
        "disabled".to_string()
    } else if !cpu_supports_monitorx() {
        "no_monitorx".to_string()
    } else {
        let hvis = unsafe { GetModuleHandleA(b"Visualizer.dll\0".as_ptr()) };
        if hvis.is_null() {
            "no_visualizer".to_string()
        } else {
            let vbase = hvis as usize;
            match unsafe { pe_image_size(vbase) }
                .ok_or_else(|| "bad_pe".to_string())
                .and_then(|vsz| unsafe { find_visualizer_spin_loop(vbase, vsz) })
            {
                Ok(vaddr) => match unsafe { patch_vis_pacer(vaddr as *mut u8, cfg.pacer_timeout_cycles) } {
                    Ok(()) => format!("mwaitx@0x{:x}(cycles={})", vaddr - vbase, cfg.pacer_timeout_cycles),
                    Err(e) => format!("err({e})"),
                },
                Err(e) => format!("skip({e})"),
            }
        }
    };

    let (terrain_target, terrain_opt_wanted, terrain_status) = if cfg.terrain_lookup_opt {
        match try_install_terrain_opt() {
            Some((tt, msg)) => (tt, false, msg),
            None => (ptr::null_mut(), true, "deferred".to_string()),
        }
    } else {
        (ptr::null_mut(), false, "disabled".to_string())
    };

    let (route_target, route_opt_wanted, route_status) = if cfg.transport_route_opt {
        match try_install_transport_route_opt() {
            Some((rt, msg)) => (rt, false, msg),
            None => (ptr::null_mut(), true, "deferred".to_string()),
        }
    } else {
        (ptr::null_mut(), false, "disabled".to_string())
    };

    let (zlib_installed, zlib_opt_wanted, zlib_status) = if cfg.zlib_inflate_opt {
        match try_install_zlib_opt() {
            Some((inst, msg)) => (inst, false, msg),
            None => (false, true, "deferred".to_string()),
        }
    } else {
        (false, false, "disabled".to_string())
    };

    {
        let mut guard = STATE.lock().unwrap();
        *guard = Some(HookState {
            mode: cfg.mode,
            max_hz: cfg.max_hz,
            log_interval_sec: cfg.log_interval_sec,
            log_path: cfg.log_path.clone(),
            addrs,
            terrain_opt_wanted,
            terrain_last_probe_qpc: now,
            terrain_target,
            route_opt_wanted,
            route_last_probe_qpc: now,
            route_target,
            zlib_opt_wanted,
            zlib_last_probe_qpc: now,
            zlib_installed,
            qpc_freq: qpc_freq(),
            window_start_qpc: now,
            last_exec_qpc: 0,
            calls_exec: 0,
            calls_skip: 0,
            total_qpc: 0,
        });
    }
    HOOK_ACTIVE.store(true, Ordering::Release);

    let msg = format!(
        "event=init version={} sound={} mode={} max_hz={} cull_div={} safe_margin_m={:.0} log_interval_s={} pacer={} terrain={} route={} zlib={}",
        env!("CARGO_PKG_VERSION"),
        sound_status,
        cfg.mode.as_str(),
        cfg.max_hz,
        cfg.cull_mask + 1,
        cfg.safe_margin_m,
        cfg.log_interval_sec,
        pacer_status,
        terrain_status,
        route_status,
        zlib_status,
    );
    append_log(&cfg.log_path, &msg);
    Ok(msg)
}

/// Returns true when the hooks are currently installed in this process.
fn hooks_installed() -> bool {
    match STATE.lock() {
        Ok(guard) => guard.is_some(),
        Err(_) => HOOK_ACTIVE.load(Ordering::Acquire),
    }
}

/// Lua C entry point: package.loadlib(dll, 'luaopen_dcs_cpu_opt')()
///
/// Idempotent per process: a second call (a new or re-run Lua state) never patches twice; it only logs
/// `event=init_again`.
#[no_mangle]
pub extern "C" fn luaopen_dcs_cpu_opt(_lua_state: *mut c_void) -> i32 {
    let Ok(_init) = INIT_LOCK.try_lock() else { return 0; };
    let n = INIT_CALLS.fetch_add(1, Ordering::AcqRel) + 1;
    if hooks_installed() {
        let cfg = read_config();
        append_log(
            &cfg.log_path,
            &format!(
                "event=init_again version={} call={} pinned={} hooks_kept=1",
                env!("CARGO_PKG_VERSION"),
                n,
                DLL_PINNED.load(Ordering::Acquire) as u8
            ),
        );
        return 0;
    }
    if let Err(e) = install_hook() {
        let cfg = read_config();
        append_log(&cfg.log_path, &format!("event=init_error msg={}", e));
    }
    0
}

/// Called on DCS's Lua thread by the managed hook. An independent optimization never needs the audio hook.
#[no_mangle]
pub extern "C" fn DcsCpuOpt_Tick(_lua_state: *mut c_void) -> i32 {
    if let Ok(mut state) = STATE.try_lock() {
        if let Some(st) = state.as_mut() {
            let t1 = qpc_now();
            if st.terrain_opt_wanted
                && st.terrain_target.is_null()
                && (t1 - st.terrain_last_probe_qpc) >= st.qpc_freq * 5
            {
                st.terrain_last_probe_qpc = t1;
                if let Some((tt, msg)) = try_install_terrain_opt() {
                    st.terrain_target = tt;
                    st.terrain_opt_wanted = false;
                    append_log(&st.log_path, &format!("event=terrain_init {}", msg));
                }
            }
            if st.route_opt_wanted
                && st.route_target.is_null()
                && (t1 - st.route_last_probe_qpc) >= st.qpc_freq * 5
            {
                st.route_last_probe_qpc = t1;
                if let Some((rt, msg)) = try_install_transport_route_opt() {
                    st.route_target = rt;
                    st.route_opt_wanted = false;
                    append_log(&st.log_path, &format!("event=route_init {}", msg));
                }
            }
            if st.zlib_opt_wanted
                && !st.zlib_installed
                && (t1 - st.zlib_last_probe_qpc) >= st.qpc_freq * 5
            {
                st.zlib_last_probe_qpc = t1;
                if let Some((inst, msg)) = try_install_zlib_opt() {
                    st.zlib_installed = inst;
                    st.zlib_opt_wanted = false;
                    append_log(&st.log_path, &format!("event=zlib_init {}", msg));
                }
            }
            if st.addrs.is_none() && st.log_interval_sec > 0
                && (t1 - st.window_start_qpc) >= st.qpc_freq * st.log_interval_sec
            {
                flush_window(st, t1, "interval");
            }
        }
    }
    0
}

/// Lua C hook called from the loader's `__gc` guard: package.loadlib(dll, 'DcsCpuOpt_Shutdown')()
///
/// A Lua state closing (or its guard being collected) must not unpatch: the patches stay for the whole process
/// lifetime and this DLL is pinned, so process exit needs no cleanup. It only logs `event=lua_state_closed`.
/// No hooks are installed when pinning fails.
#[no_mangle]
pub extern "C" fn DcsCpuOpt_Shutdown(_lua_state: *mut c_void) -> i32 {
    let n = LUA_CLOSE_CALLS.fetch_add(1, Ordering::AcqRel) + 1;
    let pinned = DLL_PINNED.load(Ordering::Acquire);
    let log_path = match STATE.try_lock() {
        Ok(guard) => guard.as_ref().map(|st| st.log_path.clone()),
        Err(_) => None,
    };
    let log_path = log_path.unwrap_or_else(|| read_config().log_path);
    append_log(
        &log_path,
        &format!(
            "event=lua_state_closed call={} pinned={} hooks_kept={}",
            n,
            pinned as u8,
            pinned as u8
        ),
    );
    0
}

#[cfg(test)]
fn test_dcs_file(name: &str) -> Option<Vec<u16>> {
    use std::os::windows::ffi::OsStrExt;
    let root = PathBuf::from(std::env::var_os("DCSC_CPU_OPT_DCS")?);
    let path = root.join("bin").join(name);
    assert!(path.is_file(), "Missing DCS test binary: {}", path.display());
    Some(path.as_os_str().encode_wide().chain([0]).collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    const DONT_RESOLVE_DLL_REFERENCES: u32 = 0x0000_0001;

    #[link(name = "kernel32")]
    extern "system" {
        fn LoadLibraryExW(lpLibFileName: *const u16, hFile: *mut c_void, dwFlags: u32) -> *mut c_void;
        fn FreeLibrary(hLibModule: *mut c_void) -> i32;
    }

    #[test]
    fn resolve_real_ed_sound_dll() {
        let Some(path) = test_dcs_file("ed_sound.dll") else { return };
        unsafe {
            let hmod = LoadLibraryExW(path.as_ptr(), ptr::null_mut(), DONT_RESOLVE_DLL_REFERENCES);
            assert!(!hmod.is_null(), "LoadLibraryExA failed for ed_sound.dll");
            let base = hmod as usize;
            let image_size = verified_image_size(base, 0x6ae7205a, 0xd0c4000).expect("unverified audio test build");
            let snd = GetProcAddress(hmod, b"SND_process\0".as_ptr()) as usize;
            assert!(snd > base, "SND_process export not found");

            let addrs = resolve_from_snd_process(base, image_size, snd)
                .expect("resolve_from_snd_process failed");

            assert_eq!(addrs.update_source - base, 0x8d40, "update_source RVA");
            assert_eq!(addrs.frame_counter - base, 0x3ad68, "frame_counter RVA");
            assert_eq!(addrs.snd_state.map(|a| a - base), Some(0x3ad70), "snd_state RVA");
            assert_eq!(addrs.ctx_count.map(|a| a - base), Some(0x3ad74), "ctx_count RVA");
            assert_eq!(addrs.active_voices.map(|a| a - base), Some(0x3ad80), "active_voices RVA");
            assert_eq!(addrs.sorted_voices.map(|a| a - base), Some(0x3ad84), "sorted_voices RVA");
            assert_eq!(addrs.ctx_array.map(|a| a - base), Some(0x3d370), "ctx_array RVA");

            FreeLibrary(hmod);
        }
    }

    #[test]
    fn find_real_visualizer_spin_loop() {
        let Some(path) = test_dcs_file("Visualizer.dll") else { return };
        unsafe {
            let hmod = LoadLibraryExW(path.as_ptr(), ptr::null_mut(), DONT_RESOLVE_DLL_REFERENCES);
            assert!(!hmod.is_null(), "LoadLibraryExA failed for Visualizer.dll");
            let base = hmod as usize;
            let image_size = pe_image_size(base).expect("pe_image_size failed for Visualizer.dll");
            let addr = find_visualizer_spin_loop(base, image_size)
                .expect("find_visualizer_spin_loop failed");
            assert!([0xba8c0, 0xbac30].contains(&(addr - base)), "Unexpected Visualizer wait site: 0x{:x}", addr - base);
            FreeLibrary(hmod);
        }
    }

    #[test]
    fn pacer_stub_jump_targets_exact() {
        let resume = 0x1122_3344_5566_7788;
        let stub = build_mwaitx_pacer_stub(resume, 200_000);
        assert_eq!(19isize + (stub[18] as i8) as isize, 39); // je restore
        assert_eq!(24isize + (stub[23] as i8) as isize, 39); // jb restore
        assert_eq!(&stub[39..42], &[0x5b, 0x5a, 0x59]);
        assert_eq!(&stub[42..48], &[0xff, 0x25, 0, 0, 0, 0]);
        assert_eq!(u64::from_le_bytes(stub[48..].try_into().unwrap()), resume);
        let mut loop_bytes = EXPECTED_VIS_SPIN_LOOP;
        let base = 0x1_0000_0000;
        let near_stub = base - 0x1_0000;
        let jump = pacer_jump(base + VIS_PAUSE_OFFSET, near_stub).unwrap();
        let displacement = i32::from_le_bytes(jump[1..5].try_into().unwrap());
        assert_eq!((base + VIS_PAUSE_OFFSET + 5) as i64 + displacement as i64, near_stub as i64);
        assert!(pacer_jump(base + VIS_PAUSE_OFFSET, base + 0x9000_0000).is_none());
        loop_bytes[VIS_PAUSE_OFFSET..VIS_PAUSE_OFFSET + jump.len()].copy_from_slice(&jump);
        let callback_return = 20;
        let callback_landing = (callback_return + 2) as i8 + (loop_bytes[callback_return + 1] as i8);
        assert_eq!(callback_landing, 28);
        assert_eq!(&loop_bytes[..VIS_PAUSE_OFFSET], &EXPECTED_VIS_SPIN_LOOP[..VIS_PAUSE_OFFSET]);
        assert_eq!(&loop_bytes[callback_landing as usize..], &EXPECTED_VIS_SPIN_LOOP[28..]);
        assert_eq!(VIS_RESUME_OFFSET, 30);
    }

    #[test]
    fn guarded_write_checks_full_signature_before_touching_the_patch() {
        let expected = EXPECTED_VIS_SPIN_LOOP;
        let patch = [0x90u8; 6];
        unsafe {
            let mem = VirtualAlloc(ptr::null_mut(), 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE) as *mut u8;
            assert!(!mem.is_null());
            ptr::copy_nonoverlapping(expected.as_ptr(), mem, expected.len());
            // Change a byte outside the overwritten range.
            *mem.add(17) = 0x90;
            let site = mem.add(VIS_PAUSE_OFFSET);
            let refused = DcsCpuOpt_WriteGuardedPatch(site, patch.as_ptr(), patch.len(), mem, expected.as_ptr(), expected.len());
            let unchanged = std::slice::from_raw_parts(site, patch.len()) == &expected[VIS_PAUSE_OFFSET..VIS_PAUSE_OFFSET + patch.len()];
            *mem.add(17) = expected[17];
            let accepted = DcsCpuOpt_WriteGuardedPatch(site, patch.as_ptr(), patch.len(), mem, expected.as_ptr(), expected.len());
            let applied = std::slice::from_raw_parts(site, patch.len()) == patch;
            VirtualFree(mem as *mut c_void, 0, MEM_RELEASE);
            assert_eq!(refused, 0);
            assert!(unchanged);
            assert_eq!(accepted, 1);
            assert!(applied);
        }
    }

    #[test]
    fn flat_u32_map_handles_edge_keys_and_collisions() {
        let mut m = FlatU32Map::default();
        m.reset_with_capacity(64);
        for k in 0..64u32 {
            m.insert(k * 7, k + 100);
        }
        m.insert(u32::MAX, 42);
        for k in 0..64u32 {
            assert_eq!(m.get(k * 7), k + 100);
            assert_eq!(m.get(k * 7 + 1), u32::MAX);
        }
        assert_eq!(m.get(u32::MAX), 42);
    }

    #[test]
    fn find_terrain_lookup_from_dumped_bytes() {
        let Some(dump) = std::env::var_os("DCSC_CPU_OPT_TERRAIN_DUMP") else { return };
        let bytes = fs::read(dump).expect("read terrain test dump");
        let base = 0x1_8000_0000usize;
        let hit = unsafe {
            find_terrain_lookup_in_slice(&bytes, base + 0x15860, base, 0x50_0000)
                .expect("scan failed")
                .expect("pattern not found")
        };
        assert_eq!(hit.0 - base, 0x15870, "FUN_15870 RVA");
        assert_eq!(hit.1 - base, 0x105f0, "insert_set RVA");
    }

    #[test]
    fn find_real_terrain_lookup_fn() {
        let Some(path) = test_dcs_file("edterrain4.dll") else { return };
        unsafe {
            // Loading edterrain4 unpacks its code before the scan.
            let module = LoadLibraryExW(path.as_ptr(), ptr::null_mut(), 0x1100);
            assert!(!module.is_null(), "LoadLibraryExW failed for edterrain4.dll");
            let base = module as usize;
            let size = verified_image_size(base, 0x6ae721cb, 0x14c2000).expect("unverified terrain test build");
            let result = find_terrain_lookup_fn(base, size);
            FreeLibrary(module);
            let (lookup, insert) = result.expect("terrain lookup signatures");
            println!("terrain lookup RVA 0x{:x}, set insert RVA 0x{:x}", lookup - base, insert - base);
        }
    }

    #[test]
    fn find_real_transport_route_fn() {
        let Some(path) = test_dcs_file("Transport.dll") else { return };
        unsafe {
            let hmod = LoadLibraryExW(path.as_ptr(), ptr::null_mut(), DONT_RESOLVE_DLL_REFERENCES);
            assert!(!hmod.is_null(), "LoadLibraryExA failed for Transport.dll");
            let base = hmod as usize;
            let image_size = verified_image_size(base, 0x6afebf8b, 0x367000).expect("unverified transport test build");
            let addr = find_transport_route_fn(base, image_size)
                .expect("find_transport_route_fn failed");
            assert_eq!(addr - base, 0xa1600, "Transport route RVA");
            FreeLibrary(hmod);
        }
    }

    fn find_closest_point_reference(
        nodes: &[PathNode],
        qx: f64,
        qz: f64,
        start_s: f64,
    ) -> (PathNode, f64) {
        let n = nodes.len();
        if n < 2 {
            return (
                PathNode {
                    s: -1.0,
                    seg_idx: u32::MAX,
                    _pad: 0,
                    x: 0.0,
                    z: 0.0,
                    f4: f64::MAX,
                    f5: 0.0,
                    f6: 0.0,
                },
                f64::MAX,
            );
        }
        let ub = nodes.partition_point(|nd| nd.s <= start_s);
        if ub == n {
            let last = nodes[n - 1];
            let d = ((last.x - qx).powi(2) + (last.z - qz).powi(2)).sqrt();
            return (last, d);
        }
        let (start_i, anchor_i) = if ub == 0 { (0, 1) } else { (ub - 1, ub) };
        let anchor = nodes[anchor_i];
        let mut best_d = f64::MAX;
        let mut best_s = f64::MAX;
        let mut best_seg = anchor.seg_idx;
        let mut best_x = 0.0;
        let mut best_z = 0.0;
        let mut best_f4 = start_s;
        let mut best_f5 = 0.0;
        let mut best_f6 = 0.0;
        for i in start_i..n - 1 {
            let a = nodes[i];
            let b = nodes[i + 1];
            let mut dx = b.x - a.x;
            let mut dz = b.z - a.z;
            let len = (dx * dx + dz * dz).sqrt();
            if len > 0.0 {
                dx /= len;
                dz /= len;
            }
            let proj = (qx - a.x) * dx + (qz - a.z) * dz;
            let (px, pz, ds) = if proj >= 0.0 {
                let cand_x = a.x + dx * proj;
                let cand_z = a.z + dz * proj;
                let seg_ds = b.s - a.s;
                let frac_ds = seg_ds * (proj / len);
                if frac_ds <= seg_ds {
                    (cand_x, cand_z, frac_ds)
                } else {
                    (b.x, b.z, seg_ds)
                }
            } else {
                (a.x, a.z, 0.0)
            };
            let d = ((qx - px).powi(2) + (qz - pz).powi(2)).sqrt();
            if d < best_d {
                if start_s > 0.0 && a.seg_idx > anchor.seg_idx {
                    best_s = anchor.s + 0.0001;
                    best_seg = anchor.seg_idx;
                    best_x = anchor.x;
                    best_z = anchor.z;
                    best_f4 = anchor.f4;
                    best_f5 = anchor.f5;
                    best_f6 = anchor.f6;
                    break;
                }
                best_d = d;
                best_seg = a.seg_idx;
                best_s = a.s + ds;
                let ratio = (best_s - a.s) / (b.s - a.s);
                best_f4 = a.f4 + (b.f4 - a.f4) * ratio;
                best_f5 = a.f5 + (b.f5 - a.f5) * ratio;
                best_f6 = a.f6 + (b.f6 - a.f6) * ratio;
                best_x = px;
                best_z = pz;
            }
        }
        let out_d = ((best_x - qx).powi(2) + (best_z - qz).powi(2)).sqrt();
        (
            PathNode {
                s: best_s,
                seg_idx: best_seg,
                _pad: 0,
                x: best_x,
                z: best_z,
                f4: best_f4,
                f5: best_f5,
                f6: best_f6,
            },
            out_d,
        )
    }

    #[test]
    fn transport_route_sees_an_in_place_edit() {
        let mut nodes: Vec<_> = (0..193)
            .map(|i| PathNode {
                s: i as f64,
                seg_idx: 0,
                _pad: 0,
                x: 0.0,
                z: if i < 64 { 10.0 } else { 100.0 },
                f4: 0.0,
                f5: 0.0,
                f6: 0.0,
            })
            .collect();
        assert_eq!(find_closest_point_fast(&nodes, 0.0, 0.0, 0.0).1, 10.0);
        nodes[70].z = 0.0;
        assert_eq!(find_closest_point_fast(&nodes, 0.0, 0.0, 0.0).1, 0.0);
    }

    #[test]
    fn terrain_workspace_reads_in_place_map_and_chunk_edits() {
        let mut head = [0usize; 2];
        let mut node = [head.as_mut_ptr() as usize, 0, (4usize << 32) | 9];
        head[0] = node.as_mut_ptr() as usize;
        let mut map = [0usize; 24];
        map[0xb0 / 8] = head.as_ptr() as usize;
        map[0xb8 / 8] = 1;
        let mut keys = [0usize; 16];
        keys[8] = 9;
        let mut chunk = [0usize; 0x1b0 / 8];
        chunk[0x110 / 8] = keys.as_mut_ptr() as usize;
        chunk[0x118 / 8] = chunk[0x110 / 8] + 0x80;
        let mut workspace = TerrainWorkspace::default();
        unsafe {
            assert!(workspace.read_map(map.as_ptr() as usize));
            assert!(workspace.find_matches(chunk.as_ptr() as usize, 1, 4));
            assert_eq!(workspace.matches, [0]);
            node[2] = (7usize << 32) | 9;
            assert!(workspace.read_map(map.as_ptr() as usize));
            assert!(workspace.find_matches(chunk.as_ptr() as usize, 1, 4));
            assert!(workspace.matches.is_empty());
            assert!(workspace.find_matches(chunk.as_ptr() as usize, 1, 7));
            assert_eq!(workspace.matches, [0]);
            keys[8] = 10;
            assert!(workspace.find_matches(chunk.as_ptr() as usize, 1, 7));
            assert!(workspace.matches.is_empty());
            std::hint::black_box((node, keys, head));
        }
    }

    #[test]
    fn transport_route_fast_matches_reference() {
        assert_eq!(std::mem::size_of::<PathNode>(), 0x38);
        let mut seed: u64 = 0x1234_5678_9abc_def0;
        let mut next_f64 = || -> f64 {
            seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            ((seed >> 11) as f64) / ((1u64 << 53) as f64)
        };

        for case in 0..250 {
            let n = if case < 3 {
                case
            } else if case >= 200 {
                200 + (case - 200) * 25 // Long routes also exercise segment pruning.
            } else {
                2 + (case % 80)
            };
            let mut nodes = Vec::with_capacity(n);
            let mut s = 0.0;
            let mut x = 0.0;
            let mut z = 0.0;
            for i in 0..n {
                let seg_len = if i > 0 && (case % 17 == 0 && i == n / 2) {
                    0.0
                } else {
                    10.0 + next_f64() * 90.0
                };
                if i > 0 {
                    let ang = next_f64() * std::f64::consts::TAU;
                    x += ang.cos() * seg_len;
                    z += ang.sin() * seg_len;
                    s += seg_len.max(0.01);
                }
                nodes.push(PathNode {
                    s,
                    seg_idx: (i / 10) as u32,
                    _pad: 0,
                    x,
                    z,
                    f4: 15.0 + next_f64() * 10.0,
                    f5: next_f64() * 3.14,
                    f6: next_f64() * 100.0,
                });
            }

            let qx = (next_f64() - 0.5) * 400.0;
            let qz = (next_f64() - 0.5) * 400.0;
            let start_s = match case % 5 {
                0 => 0.0,
                1 => -10.0,
                2 => s + 50.0,
                _ => next_f64() * s,
            };

            let (r_node, r_dist) = find_closest_point_reference(&nodes, qx, qz, start_s);
            let (f_node, f_dist) = find_closest_point_fast(&nodes, qx, qz, start_s);

            assert_eq!(f_node.seg_idx, r_node.seg_idx, "case {} seg_idx", case);
            assert!((f_dist - r_dist).abs() <= 1e-9 * r_dist.abs().max(1.0), "case {} dist: {} vs {}", case, f_dist, r_dist);
            assert!((f_node.s - r_node.s).abs() <= 1e-9 * r_node.s.abs().max(1.0), "case {} s: {} vs {}", case, f_node.s, r_node.s);
            assert!((f_node.x - r_node.x).abs() <= 1e-9 * r_node.x.abs().max(1.0), "case {} x", case);
            assert!((f_node.z - r_node.z).abs() <= 1e-9 * r_node.z.abs().max(1.0), "case {} z", case);
            assert!((f_node.f4 - r_node.f4).abs() <= 1e-9 * r_node.f4.abs().max(1.0), "case {} f4", case);
            assert!((f_node.f5 - r_node.f5).abs() <= 1e-9 * r_node.f5.abs().max(1.0), "case {} f5", case);
            assert!((f_node.f6 - r_node.f6).abs() <= 1e-9 * r_node.f6.abs().max(1.0), "case {} f6", case);
        }
    }
}

//! `ZlibInflateOpt` equivalence tests against the REAL DCS `zlib1.dll` (zlib-ng 2.2.1) and `edCore.dll`.
//! They are skipped (pass trivially) when the DCS install is not present, like the other DLL tests.
use super::*;
use std::sync::OnceLock;

#[link(name = "kernel32")]
extern "system" {
    fn LoadLibraryW(n: *const u16) -> *mut c_void;
    fn LoadLibraryExW(n: *const u16, f: *mut c_void, fl: u32) -> *mut c_void;
    fn GetProcAddress(m: *mut c_void, n: *const u8) -> *mut c_void;
}

/// zlib1.dll `deflateInit2_` signature.
type DefInit2 = unsafe extern "C" fn(*mut ZStream, c_int, c_int, c_int, c_int, c_int, *const c_char, c_int) -> c_int;

/// zlib1.dll `deflate` signature.
type Def = unsafe extern "C" fn(*mut ZStream, c_int) -> c_int;

/// Real zlib1.dll entry points.
struct Z {
    /// `inflateInit2_`.
    init2: InflateInit2Fn,
    /// `inflate`.
    inflate: InflateFn,
    /// `inflateEnd`.
    end: InflateEndFn,
    /// `deflateInit2_`.
    dinit2: DefInit2,
    /// `deflate`.
    deflate: Def,
    /// `deflateEnd`.
    dend: InflateEndFn,
    /// `crc32`.
    crc32: unsafe extern "C" fn(u32, *const u8, u32) -> u32,
}

/// Loads the real zlib1.dll once and points the wrappers at it; `None` without a DCS install.
fn z() -> Option<&'static Z> {
    static Z1: OnceLock<Option<Z>> = OnceLock::new();
    Z1.get_or_init(|| unsafe {
        let p = crate::test_dcs_file("zlib1.dll")?;
        let m = LoadLibraryW(p.as_ptr());
        assert!(!m.is_null(), "LoadLibraryA zlib1.dll");
        let g = |n: &[u8]| {
            let f = GetProcAddress(m, n.as_ptr());
            assert!(!f.is_null());
            f
        };
        let r = Z {
            init2: std::mem::transmute(g(b"inflateInit2_\0")),
            inflate: std::mem::transmute(g(b"inflate\0")),
            end: std::mem::transmute(g(b"inflateEnd\0")),
            dinit2: std::mem::transmute(g(b"deflateInit2_\0")),
            deflate: std::mem::transmute(g(b"deflate\0")),
            dend: std::mem::transmute(g(b"deflateEnd\0")),
            crc32: std::mem::transmute(g(b"crc32\0")),
        };
        set_real(r.init2 as usize, r.inflate as usize, r.end as usize);
        Some(r)
    })
    .as_ref()
}

/// xorshift64* generator.
struct Rng(u64);

impl Rng {
    /// Next 64 random bits.
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545F4914F6CDD1D)
    }

    /// Uniform value in `0..n` (`0` when `n == 0`).
    fn below(&mut self, n: u64) -> u64 {
        self.next() % n.max(1)
    }
}

/// Test data: random, text-like, long-distance repeats (window wrap), runs and BC-texture-like blocks.
fn gen_data(r: &mut Rng) -> Vec<u8> {
    let len = match r.below(6) {
        0 => r.below(64) as usize,
        1 => r.below(4096) as usize,
        2 => r.below(70_000) as usize,
        _ => 30_000 + r.below(600_000) as usize,
    };
    let mut v = Vec::with_capacity(len);
    let kind = r.below(5);
    while v.len() < len {
        match (kind + r.below(3)) % 5 {
            0 => v.push(r.next() as u8),
            1 => {
                let w = b"the quick brown fox jumps over the lazy dog DXT5 mip level ";
                let s = r.below(w.len() as u64) as usize;
                v.extend_from_slice(&w[s..]);
            }
            2 if v.len() > 40_000 => {
                let d = 1 + r.below(40_000.min(v.len() as u64)) as usize;
                for _ in 0..3 + r.below(300) {
                    let b = v[v.len() - d];
                    v.push(b);
                }
            }
            3 => {
                let b = r.next() as u8;
                let n = 1 + r.below(2000) as usize;
                v.extend(std::iter::repeat_n(b, n));
            }
            _ => {
                let base = r.next();
                for _ in 0..(1 + r.below(64)) {
                    v.extend_from_slice(&(base ^ (r.below(4) << (8 * r.below(8)))).to_le_bytes());
                    v.extend_from_slice(&(r.next() & 0x5555_5555_5555_5555).to_le_bytes());
                }
            }
        }
    }
    v.truncate(len);
    v
}

/// Compresses with zlib1.dll's own deflate: random level/window/memLevel/strategy and random flush points.
fn compress(z: &Z, r: &mut Rng, data: &[u8], wbits: c_int) -> Vec<u8> {
    unsafe {
        let mut s: ZStream = std::mem::zeroed();
        let level = r.below(11) as c_int - 1;
        let mem = 1 + r.below(9) as c_int;
        let strat = r.below(5) as c_int;
        let wb = if wbits < 0 { -(9 + r.below(7) as c_int) } else { wbits };
        assert_eq!((z.dinit2)(&mut s, level, 8, wb, mem, strat, c"1.3.1".as_ptr(), 0x58), 0);
        let mut out = vec![0u8; data.len() * 2 + 4096];
        s.next_out = out.as_mut_ptr();
        s.avail_out = out.len() as u32;
        let mut pos = 0;
        loop {
            let chunk = if r.below(3) == 0 { data.len() - pos } else { (r.below(50_000) as usize).min(data.len() - pos) };
            s.next_in = data[pos..].as_ptr();
            s.avail_in = chunk as u32;
            pos += chunk;
            // Z_NO_FLUSH, Z_PARTIAL_FLUSH, Z_SYNC_FLUSH, Z_FULL_FLUSH, Z_BLOCK
            let fl = if pos == data.len() { 4 } else { [0, 0, 1, 2, 3, 5][r.below(6) as usize] };
            let rc = (z.deflate)(&mut s, fl);
            assert!(rc == 0 || rc == 1 || rc == -5, "deflate rc {rc}");
            if fl == 4 {
                assert_eq!(rc, 1);
                break;
            }
        }
        out.truncate(s.total_out as usize);
        (z.dend)(&mut s);
        out
    }
}

/// One decode: rc, avail_in, total_in, avail_out, total_out, data_type, msg, output buffer, second-call rc.
#[derive(PartialEq, Debug)]
struct Outcome(c_int, u32, u32, u32, u32, c_int, usize, Vec<u8>, c_int);

/// Decodes the edCore way (init2 / inflate Z_FINISH / optional second inflate / end), hooked or stock.
fn decode(z: &Z, hooked: bool, src: &[u8], out_len: usize, wbits: c_int, second: bool) -> Outcome {
    let (i2, inf, end): (InflateInit2Fn, InflateFn, InflateEndFn) =
        if hooked { (hook_inflate_init2, hook_inflate, hook_inflate_end) } else { (z.init2, z.inflate, z.end) };
    let mut out = vec![0xEEu8; out_len + 64];
    unsafe {
        let mut s: ZStream = std::mem::zeroed();
        assert_eq!(i2(&mut s, wbits, c"1.3.1".as_ptr(), 0x58), 0);
        s.next_in = src.as_ptr();
        s.avail_in = src.len() as u32;
        s.next_out = out.as_mut_ptr();
        s.avail_out = out_len as u32;
        let rc = inf(&mut s, Z_FINISH);
        let rc2 = if second { inf(&mut s, Z_FINISH) } else { 99 };
        let o = Outcome(rc, s.avail_in, s.total_in, s.avail_out, s.total_out, s.data_type, s.msg as usize, out, rc2);
        end(&mut s);
        o
    }
}

/// Streaming decode in random chunks with random flush modes (must take the pass-through path).
fn decode_stream(z: &Z, hooked: bool, src: &[u8], out_len: usize, seed: u64) -> (c_int, Vec<u8>, u32) {
    let mut r = Rng(seed);
    let (i2, inf, end): (InflateInit2Fn, InflateFn, InflateEndFn) =
        if hooked { (hook_inflate_init2, hook_inflate, hook_inflate_end) } else { (z.init2, z.inflate, z.end) };
    let mut out = vec![0u8; out_len + 16];
    unsafe {
        let mut s: ZStream = std::mem::zeroed();
        assert_eq!(i2(&mut s, -15, c"1.3.1".as_ptr(), 0x58), 0);
        s.next_out = out.as_mut_ptr();
        let mut pos = 0usize;
        let mut rc;
        loop {
            let n = (1 + r.below(9000) as usize).min(src.len() - pos);
            s.next_in = src[pos..].as_ptr();
            s.avail_in = n as u32;
            s.avail_out = (1 + r.below(70_000) as usize).min(out.len() - s.total_out as usize) as u32;
            rc = inf(&mut s, [0, 2, 3, 5, 4][r.below(5) as usize]);
            pos += n - s.avail_in as usize;
            if (rc != 0 && rc != -5) || (pos >= src.len() && rc == -5) {
                break;
            }
        }
        let t = s.total_out;
        end(&mut s);
        out.truncate(t as usize);
        (rc, out, t)
    }
}

/// Compares a stock and a hooked outcome; returns true when the stock decoder failed but libdeflate succeeded.
fn check(a: &Outcome, b: &Outcome, ctx: &str) -> bool {
    if a.0 != 1 && b.0 == 1 {
        return true;
    }
    let t = a.4 as usize;
    assert_eq!((a.0, a.1, a.2, a.3, a.4, a.5, a.6, a.8), (b.0, b.1, b.2, b.3, b.4, b.5, b.6, b.8), "{ctx}: fields");
    // On success the decoded bytes must match; bytes past total_out are scratch in both decoders.
    // On error only the prefix zlib-ng wrote is compared (edCore discards a failed buffer).
    assert!(a.7[..t] == b.7[..t], "{ctx}: output bytes differ");
    false
}

#[test]
fn zlib_randomized_equivalence_vs_real_zlib1() {
    let Some(z) = z() else { return };
    let iters: usize = std::env::var("ZLIB_FUZZ_ITERS").ok().and_then(|v| v.parse().ok()).unwrap_or(300);
    let mut r = Rng(0x2026_1125_dc5c_0001);
    let (mut cases, mut lenient) = (0usize, 0usize);
    let fast0 = ZLIB_FAST.load(Relaxed);
    for iter in 0..iters {
        let data = gen_data(&mut r);
        let comp = compress(z, &mut r, &data, -15);
        let n = data.len();
        let mut garbage = comp.clone();
        for _ in 0..r.below(40) {
            garbage.push(r.next() as u8);
        }
        let mut v: Vec<(Vec<u8>, usize, bool)> = vec![
            (comp.clone(), n, true),                                // exact size + a second inflate call
            (comp.clone(), n + 1 + r.below(5000) as usize, false),  // output slack
            (garbage, n, false),                                    // trailing garbage
            (comp.clone(), n.saturating_sub(1 + r.below(n as u64) as usize), false), // output too small
            (comp[..r.below(comp.len() as u64) as usize].to_vec(), n, false), // truncated input
        ];
        for m in 0..7 {
            let mut c = comp.clone();
            match m {
                0..=2 => {
                    for _ in 0..(1 + r.below(4)) {
                        let i = r.below(c.len() as u64) as usize;
                        c[i] ^= 1 << r.below(8);
                    }
                }
                3 | 4 => {
                    // block headers and code-length tables sit at the start of each block
                    let h = c.len().min(48) as u64;
                    for _ in 0..(1 + r.below(3)) {
                        let i = r.below(h) as usize;
                        c[i] ^= 1 << r.below(8);
                    }
                }
                5 => {
                    let i = r.below(c.len() as u64) as usize;
                    for j in i..(i + 1 + r.below(8) as usize).min(c.len()) {
                        c[j] = r.next() as u8;
                    }
                }
                _ => c = (0..1 + r.below(300)).map(|_| r.next() as u8).collect(),
            }
            v.push((c, n.max(1), false));
        }
        for (k, (src, olen, second)) in v.iter().enumerate() {
            let a = decode(z, false, src, *olen, -15, *second);
            let b = decode(z, true, src, *olen, -15, *second);
            if check(&a, &b, &format!("iter {iter} variant {k}")) {
                lenient += 1;
            }
            cases += 1;
        }
        if iter % 4 == 0 {
            let sd = r.next();
            assert_eq!(decode_stream(z, false, &comp, n, sd), decode_stream(z, true, &comp, n, sd), "iter {iter} streaming");
        }
        if iter % 10 == 0 {
            for wb in [15, 31] {
                let c = compress(z, &mut r, &data, wb);
                assert_eq!(decode(z, false, &c, n, wb, true), decode(z, true, &c, n, wb, true), "iter {iter} wbits {wb}");
            }
        }
    }
    let fast = ZLIB_FAST.load(Relaxed) - fast0;
    println!("zlib fuzz: cases={cases} fast={fast} lenient={lenient}");
    assert!(fast as usize > iters, "fast path barely taken: {fast}");
    assert_eq!(lenient, 0, "libdeflate accepted input that zlib-ng rejects");
}

/// LSB-first DEFLATE bit writer.
struct Bits(Vec<u8>, u32);

impl Bits {
    /// Writes the low `n` bits of `v`, least significant first.
    fn put(&mut self, v: u32, n: u32) {
        for i in 0..n {
            if self.1 % 8 == 0 {
                self.0.push(0);
            }
            *self.0.last_mut().unwrap() |= (((v >> i) & 1) as u8) << (self.1 % 8);
            self.1 += 1;
        }
    }

    /// Writes an `n`-bit Huffman code, most significant code bit first.
    fn code(&mut self, c: u32, n: u32) {
        for i in (0..n).rev() {
            self.put((c >> i) & 1, 1);
        }
    }
}

/// Dynamic block with litlen {65:2, 256:2, 257:1} and distance symbol 0 of length `dist_len`
/// (1 = incomplete one-codeword code, 0 = empty code), followed by `body`.
fn crafted(dist_len: u32, body: &dyn Fn(&mut Bits)) -> Vec<u8> {
    let mut w = Bits(Vec::new(), 0);
    w.put(1, 1); // BFINAL
    w.put(2, 2); // dynamic Huffman
    w.put(1, 5); // HLIT: 258 litlen codes
    w.put(0, 5); // HDIST: 1 distance code
    w.put(14, 4); // HCLEN: 18 precode lengths
    for s in [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1] {
        w.put(if [18, 0, 1, 2].contains(&s) { 2 } else { 0 }, 3);
    }
    // canonical precode: 0->00 1->01 2->10 18->11
    let pc = |w: &mut Bits, s: u32| w.code(match s { 0 => 0, 1 => 1, 2 => 2, _ => 3 }, 2);
    pc(&mut w, 18);
    w.put(65 - 11, 7); // 65 zeros
    pc(&mut w, 2); // 'A' length 2
    pc(&mut w, 18);
    w.put(127, 7); // 138 zeros
    pc(&mut w, 18);
    w.put(52 - 11, 7); // 52 zeros
    pc(&mut w, 2); // 256 length 2
    pc(&mut w, 1); // 257 length 1
    pc(&mut w, dist_len); // distance symbol 0
    body(&mut w); // litlen canonical: 257 -> 0, 'A' -> 10, 256 -> 11
    w.0
}

#[test]
fn zlib_incomplete_codes_match_zlib() {
    let Some(z) = z() else { return };
    let lit_a = |w: &mut Bits| w.code(2, 2);
    let eob = |w: &mut Bits| w.code(3, 2);
    let len3 = |w: &mut Bits| w.code(0, 1);
    let cases: Vec<(&str, Vec<u8>)> = vec![
        ("valid copy via codeword 0", crafted(1, &|w| { lit_a(w); len3(w); w.put(0, 1); eob(w) })),
        ("unused distance codeword 1", crafted(1, &|w| { lit_a(w); len3(w); w.put(1, 1); eob(w) })),
        ("empty distance code used", crafted(0, &|w| { lit_a(w); len3(w); w.put(0, 1); eob(w) })),
        ("empty distance code unused", crafted(0, &|w| { lit_a(w); lit_a(w); eob(w) })),
    ];
    for (name, c) in cases {
        for out in [4usize, 2, 64] {
            let a = decode(z, false, &c, out, -15, false);
            let b = decode(z, true, &c, out, -15, false);
            assert!(!check(&a, &b, name), "{name}: libdeflate lenient");
        }
    }
    // the unused codeword must be an error in zlib-ng (proves the case is meaningful)
    let bad = crafted(1, &|w| { lit_a(w); len3(w); w.put(1, 1); eob(w) });
    assert_eq!(decode(z, false, &bad, 4, -15, false).0, -3);
}

/// Reads up to `budget` bytes of method-8 entries (raw deflate, uncompressed size, CRC-32) from a zip file.
fn zip_entries(path: &std::path::Path, budget: usize) -> Vec<(Vec<u8>, usize, u32)> {
    let Ok(f) = std::fs::read(path) else { return vec![] };
    let rd16 = |o: usize| u16::from_le_bytes([f[o], f[o + 1]]) as usize;
    let rd32 = |o: usize| u32::from_le_bytes(f[o..o + 4].try_into().unwrap()) as usize;
    let Some(eocd) = (f.len().saturating_sub(65_557)..f.len().saturating_sub(21)).rev().find(|&i| rd32(i) == 0x0605_4b50) else {
        return vec![];
    };
    let (n, mut p) = (rd16(eocd + 10), rd32(eocd + 16));
    let (mut out, mut used) = (vec![], 0);
    for _ in 0..n {
        if p + 46 > f.len() || rd32(p) != 0x0201_4b50 {
            break;
        }
        let (method, crc, csize, usize_, lho) = (rd16(p + 10), rd32(p + 16) as u32, rd32(p + 20), rd32(p + 24), rd32(p + 42));
        let next = p + 46 + rd16(p + 28) + rd16(p + 30) + rd16(p + 32);
        if method == 8 && csize > 0 && used + csize <= budget && lho + 30 <= f.len() && rd32(lho) == 0x0403_4b50 {
            let d = lho + 30 + rd16(lho + 26) + rd16(lho + 28);
            if d + csize <= f.len() {
                out.push((f[d..d + csize].to_vec(), usize_, crc));
                used += csize;
            }
        }
        p = next;
    }
    out
}

/// Collects real DCS zip entries: up to `per_zip` compressed bytes from each of the first `zips` archives.
fn dcs_zip_corpus(zips: usize, per_zip: usize) -> Vec<(Vec<u8>, usize, u32)> {
    let Some(root) = std::env::var_os("DCSC_CPU_OPT_DCS").map(std::path::PathBuf::from) else { return vec![] };
    let mut stack = vec![root.join("Bazar"), root.join("Mods"), root.join("CoreMods")];
    let mut files = vec![];
    while let Some(d) = stack.pop() {
        let Ok(rd) = std::fs::read_dir(&d) else { continue };
        let mut ents: Vec<_> = rd.flatten().map(|e| e.path()).collect();
        ents.sort();
        for p in ents {
            if p.is_dir() {
                stack.push(p);
            } else if p.extension().is_some_and(|e| e.eq_ignore_ascii_case("zip")) {
                files.push(p);
            }
        }
    }
    files.sort();
    let step = (files.len() / zips.max(1)).max(1);
    files.iter().step_by(step).take(zips).flat_map(|p| zip_entries(p, per_zip)).collect()
}

/// edCore-sequence decode into a caller buffer (no per-call allocation), hooked or stock; returns the rc.
fn seq(z: &Z, hooked: bool, src: &[u8], dst: &mut [u8]) -> c_int {
    let (i2, inf, end): (InflateInit2Fn, InflateFn, InflateEndFn) =
        if hooked { (hook_inflate_init2, hook_inflate, hook_inflate_end) } else { (z.init2, z.inflate, z.end) };
    unsafe {
        let mut s: ZStream = std::mem::zeroed();
        i2(&mut s, -15, c"1.3.1".as_ptr(), 0x58);
        s.next_in = src.as_ptr();
        s.avail_in = src.len() as u32;
        s.next_out = dst.as_mut_ptr();
        s.avail_out = dst.len() as u32;
        let rc = inf(&mut s, Z_FINISH);
        end(&mut s);
        rc
    }
}

#[test]
fn zlib_real_dcs_zip_entries_match() {
    let Some(z) = z() else { return };
    let corpus = dcs_zip_corpus(12, 4 << 20);
    assert!(!corpus.is_empty(), "DCS asset corpus is empty");
    let max = corpus.iter().map(|e| e.1).max().unwrap();
    let (mut a, mut b) = (vec![0u8; max], vec![0u8; max]);
    for (i, (c, u, crc)) in corpus.iter().enumerate() {
        let (u, crc) = (*u, *crc);
        assert_eq!(seq(z, false, c, &mut a[..u]), 1, "entry {i} stock");
        assert_eq!(seq(z, true, c, &mut b[..u]), 1, "entry {i} hooked");
        assert!(a[..u] == b[..u], "entry {i} bytes differ");
        assert_eq!(unsafe { (z.crc32)(0, b.as_ptr(), u as u32) }, crc, "entry {i} zip CRC-32");
    }
    println!("zlib: {} real DCS zip entries identical", corpus.len());
}

#[test]
fn zlib_finds_real_edcore_iat_slots() {
    let Some(p) = crate::test_dcs_file("edCore.dll") else { return };
    // Map the image and read the import table without running the DLL.
    let h = unsafe { LoadLibraryExW(p.as_ptr(), std::ptr::null_mut(), 0x20) };
    assert!(!h.is_null());
    let base = (h as usize & !3) as *const u8;
    let s = unsafe { find_iat_slots(base, "ZLIB1.DLL", &["inflateInit2_", "inflate", "inflateEnd", "deflate"]) };
    let rva: Vec<Option<usize>> = s.iter().map(|p| p.map(|p| p as usize - base as usize)).collect();
    assert_eq!(rva, vec![Some(0x195f48), Some(0x195f40), Some(0x195f50), None]);
}

/// Throughput on real DCS zip entries, hooked vs stock (`cargo test --release --lib zlib_bench -- --ignored --nocapture`).
#[test]
#[ignore]
fn zlib_bench_real_dcs_zips() {
    let Some(z) = z() else { return };
    let corpus = dcs_zip_corpus(60, 6 << 20);
    let tot: usize = corpus.iter().map(|e| e.1).sum();
    let mut a = vec![0u8; corpus.iter().map(|e| e.1).max().unwrap_or(1)];
    let mut run = |h: bool| {
        let mut best = f64::MAX;
        for _ in 0..3 {
            let t0 = std::time::Instant::now();
            for (c, u, _) in &corpus {
                std::hint::black_box(seq(z, h, c, &mut a[..*u]));
            }
            best = best.min(t0.elapsed().as_secs_f64());
        }
        tot as f64 / 1048576.0 / best
    };
    let (s, h) = (run(false), run(true));
    println!("zlib bench: {} entries {:.0} MB out; stock {s:.1} MB/s, hooked {h:.1} MB/s, {:.2}x", corpus.len(), tot as f64 / 1048576.0, h / s);
}

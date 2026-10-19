//! NCSD (cartridge image) and NCCH (content partition) parsing.

use crate::util::{u32_at, u64_at};

pub const MEDIA_UNIT: u64 = 0x200;

/// Byte range of partition 0 (the game's executable content) inside a
/// `.3ds`/`.cci`, the whole file for a bare `.cxi`, or a decrypted `.cia`'s
/// first content (an update's, as GodMode9's "Decrypt file" writes it).
pub fn main_partition(data: &[u8]) -> Result<(usize, usize), String> {
    if data.len() >= 0x200 && &data[0x100..0x104] == b"NCSD" {
        let offset = u32_at(data, 0x120) as u64 * MEDIA_UNIT;
        let size = u32_at(data, 0x124) as u64 * MEDIA_UNIT;
        let (start, end) = (offset as usize, (offset + size) as usize);
        if end > data.len() {
            return Err("NCSD partition 0 extends past end of file (truncated dump?)".into());
        }
        Ok((start, end))
    } else if data.len() >= 0x200 && &data[0x100..0x104] == b"NCCH" {
        Ok((0, data.len()))
    } else if data.len() >= 0x20 && u32_at(data, 0) == 0x2020 {
        cia_content(data)
    } else {
        Err("not an NCSD (.3ds/.cci) or NCCH (.cxi) image".into())
    }
}

/// A CIA's first content: header, certificates, ticket and TMD come first, each
/// 64-byte aligned. Its TMD record says whether the content is still encrypted
fn cia_content(data: &[u8]) -> Result<(usize, usize), String> {
    let align = |x: u64| (x + 63) & !63;
    let certs = align(0x2020);
    let ticket = certs + align(u32_at(data, 8) as u64);
    let tmd = ticket + align(u32_at(data, 12) as u64);
    let contents = tmd + align(u32_at(data, 16) as u64);
    let be32 = |at: usize| -> Result<u32, String> {
        data.get(at..at + 4).map(|b| u32::from_be_bytes([b[0], b[1], b[2], b[3]])).ok_or("CIA is truncated".to_string())
    };
    let sig = match be32(tmd as usize)? {
        0x10003 => 0x23C,
        0x10004 => 0x13C,
        0x10005 => 0x7C,
        t => return Err(format!("CIA: unknown TMD signature type {t:#x}")),
    };
    let record = tmd as usize + 4 + sig + 0xC4 + 64 * 0x24;
    let flags = be32(record + 4)?;
    if flags & 1 != 0 {
        return Err("this .cia is encrypted; decrypt it with GodMode9 (Decrypt file) first".into());
    }
    let size = (be32(record + 8)? as u64) << 32 | be32(record + 12)? as u64;
    let (start, end) = (contents as usize, (contents + size) as usize);
    if end > data.len() {
        return Err("CIA content extends past end of file (truncated?)".into());
    }
    Ok((start, end))
}

pub struct Region {
    pub offset: usize,
    pub size: usize,
}

pub struct Ncch<'a> {
    pub data: &'a [u8],
    pub program_id: u64,
    pub product_code: String,
    pub exheader: Option<&'a [u8]>,
    pub exefs: Option<Region>,
    pub romfs: Option<Region>,
}

impl<'a> Ncch<'a> {
    pub fn parse(data: &'a [u8]) -> Result<Self, String> {
        if data.len() < 0x200 || &data[0x100..0x104] != b"NCCH" {
            return Err("missing NCCH magic".into());
        }
        // flags[7] bit 2: NoCrypto. Anything else needs keys we don't handle.
        let flags7 = data[0x18F];
        if flags7 & 0x4 == 0 {
            return Err("NCCH is encrypted; decrypt it with GodMode9 (\"Decrypt file\") first".into());
        }

        let program_id = u64_at(data, 0x118);
        let product_code = String::from_utf8_lossy(&data[0x150..0x160])
            .trim_end_matches('\0')
            .to_string();

        let exheader_size = u32_at(data, 0x180) as usize;
        let exheader = (exheader_size >= 0x400).then(|| &data[0x200..0x200 + 0x400]);

        let region = |off_at: usize| {
            let offset = u32_at(data, off_at) as usize * MEDIA_UNIT as usize;
            let size = u32_at(data, off_at + 4) as usize * MEDIA_UNIT as usize;
            (size != 0 && offset + size <= data.len()).then_some(Region { offset, size })
        };

        Ok(Ncch {
            data,
            program_id,
            product_code,
            exheader,
            exefs: region(0x1A0),
            romfs: region(0x1B0),
        })
    }
}

pub struct CodeSegment {
    pub address: u32,
    pub pages: u32,
    pub size: u32,
}

pub struct ExHeader {
    pub title: String,
    pub code_compressed: bool,
    pub text: CodeSegment,
    pub rodata: CodeSegment,
    pub data: CodeSegment,
    pub stack_size: u32,
    pub bss_size: u32,
    pub dependencies: Vec<u64>,
}

impl ExHeader {
    pub fn parse(raw: &[u8]) -> Self {
        let seg = |at: usize| CodeSegment {
            address: u32_at(raw, at),
            pages: u32_at(raw, at + 4),
            size: u32_at(raw, at + 8),
        };
        let dependencies = (0..48)
            .map(|i| u64_at(raw, 0x40 + i * 8))
            .filter(|&id| id != 0)
            .collect();
        ExHeader {
            title: String::from_utf8_lossy(&raw[0..8]).trim_end_matches('\0').to_string(),
            code_compressed: raw[0xD] & 1 != 0,
            text: seg(0x10),
            stack_size: u32_at(raw, 0x1C),
            rodata: seg(0x20),
            data: seg(0x30),
            bss_size: u32_at(raw, 0x3C),
            dependencies,
        }
    }
}

pub struct ExefsFile<'a> {
    pub name: String,
    pub data: &'a [u8],
}

pub fn exefs_files(exefs: &[u8]) -> Result<Vec<ExefsFile<'_>>, String> {
    if exefs.len() < 0x200 {
        return Err("ExeFS shorter than its header".into());
    }
    let mut files = Vec::new();
    for i in 0..10 {
        let entry = &exefs[i * 16..i * 16 + 16];
        if entry[0] == 0 {
            continue;
        }
        let name = String::from_utf8_lossy(&entry[..8]).trim_end_matches('\0').to_string();
        let offset = 0x200 + u32_at(entry, 8) as usize;
        let size = u32_at(entry, 12) as usize;
        if offset + size > exefs.len() {
            return Err(format!("ExeFS file {name} extends past the ExeFS"));
        }
        files.push(ExefsFile {
            name,
            data: &exefs[offset..offset + size],
        });
    }
    Ok(files)
}

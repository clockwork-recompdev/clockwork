//! RomFS (IVFC level 3) directory walk and extraction.

use crate::util::{u32_at, u64_at};
use std::path::{Path, PathBuf};

const NONE: u32 = 0xFFFF_FFFF;

pub struct RomFile {
    pub path: PathBuf,
    pub offset: usize,
    pub size: usize,
}

struct Level3<'a> {
    base: &'a [u8],
    dir_meta: &'a [u8],
    file_meta: &'a [u8],
    file_data: usize,
}

/// Lists every file in the RomFS with its byte range inside `romfs`.
pub fn list(romfs: &[u8]) -> Result<Vec<RomFile>, String> {
    if romfs.len() < 0x60 || &romfs[0..4] != b"IVFC" {
        return Err("RomFS missing IVFC magic".into());
    }
    let master_hash_size = u32_at(romfs, 0x08) as usize;
    let block_size = 1usize << u32_at(romfs, 0x4C);
    let l3_off = (0x60 + master_hash_size).next_multiple_of(block_size);
    let base = romfs.get(l3_off..).ok_or("RomFS level 3 offset out of range")?;

    let slice = |at: usize| -> Result<&[u8], String> {
        let off = u32_at(base, at) as usize;
        let len = u32_at(base, at + 4) as usize;
        base.get(off..off + len)
            .ok_or_else(|| "RomFS level 3 table out of range".to_string())
    };
    let l3 = Level3 {
        base,
        dir_meta: slice(0x0C)?,
        file_meta: slice(0x1C)?,
        file_data: u32_at(base, 0x24) as usize,
    };

    let mut out = Vec::new();
    walk_dir(&l3, 0, Path::new(""), &mut out, 0)?;
    for f in &mut out {
        f.offset += l3_off;
    }
    Ok(out)
}

fn name_at(meta: &[u8], at: usize, len: usize) -> Result<String, String> {
    let raw = meta.get(at..at + len).ok_or("RomFS name out of range")?;
    let units: Vec<u16> = raw.as_chunks::<2>().0.iter().map(|&c| u16::from_le_bytes(c)).collect();
    let name = String::from_utf16_lossy(&units);
    // names become output paths, so refuse anything that could escape the output dir
    if name.is_empty() || name == "." || name == ".." || name.contains(['/', '\\', '\0']) {
        return Err(format!("RomFS entry has an unsafe name: {name:?}"));
    }
    Ok(name)
}

fn walk_dir(l3: &Level3, dir: u32, path: &Path, out: &mut Vec<RomFile>, depth: u32) -> Result<(), String> {
    if depth > 64 {
        return Err("RomFS directory tree too deep (corrupt?)".into());
    }
    let d = dir as usize;
    if d + 0x18 > l3.dir_meta.len() {
        return Err("RomFS directory entry out of range".into());
    }

    let mut file = u32_at(l3.dir_meta, d + 0x0C);
    while file != NONE {
        let f = file as usize;
        if f + 0x20 > l3.file_meta.len() {
            return Err("RomFS file entry out of range".into());
        }
        let data_off = u64_at(l3.file_meta, f + 0x08) as usize;
        let size = u64_at(l3.file_meta, f + 0x10) as usize;
        let name_len = u32_at(l3.file_meta, f + 0x1C) as usize;
        let name = name_at(l3.file_meta, f + 0x20, name_len)?;
        let offset = l3.file_data + data_off;
        if offset + size > l3.base.len() {
            return Err(format!("RomFS file {name} extends past the RomFS"));
        }
        out.push(RomFile {
            path: path.join(name),
            offset,
            size,
        });
        file = u32_at(l3.file_meta, f + 0x04);
    }

    let mut child = u32_at(l3.dir_meta, d + 0x08);
    while child != NONE {
        let c = child as usize;
        if c + 0x18 > l3.dir_meta.len() {
            return Err("RomFS directory entry out of range".into());
        }
        let name_len = u32_at(l3.dir_meta, c + 0x14) as usize;
        let name = name_at(l3.dir_meta, c + 0x18, name_len)?;
        walk_dir(l3, child, &path.join(name), out, depth + 1)?;
        child = u32_at(l3.dir_meta, c + 0x04);
    }
    Ok(())
}

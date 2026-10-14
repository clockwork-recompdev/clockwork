//! The game executable as laid out in memory, built from ctrx's `code.bin` + `exheader.bin`.

use std::path::Path;

pub struct Segment {
    pub addr: u32,
    pub size: u32,
}

pub struct Image {
    /// Base address of `bytes` (the start of .text).
    pub base: u32,
    /// .text, .rodata and .data back to back, each padded to whole pages.
    pub bytes: Vec<u8>,
    pub text: Segment,
    pub rodata: Segment,
    pub data: Segment,
}

fn u32_at(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}

impl Image {
    /// Loads from a ctrx output directory.
    pub fn load(extracted: &Path) -> Result<Image, String> {
        let read = |name: &str| {
            let p = extracted.join(name);
            std::fs::read(&p).map_err(|e| format!("{}: {e} (run ctrx first)", p.display()))
        };
        let exheader = read("exheader.bin")?;
        let bytes = read("code.bin")?;
        let seg = |at| Segment {
            addr: u32_at(&exheader, at),
            size: u32_at(&exheader, at + 8),
        };
        let image = Image {
            base: u32_at(&exheader, 0x10),
            text: seg(0x10),
            rodata: seg(0x20),
            data: seg(0x30),
            bytes,
        };
        let end = image.data.addr + image.data.size;
        if (end - image.base) as usize > image.bytes.len() {
            return Err("code.bin is smaller than the exheader's segments".into());
        }
        Ok(image)
    }

    pub fn in_text(&self, addr: u32) -> bool {
        addr >= self.text.addr && addr < self.text.addr + self.text.size
    }

    pub fn u32(&self, addr: u32) -> Option<u32> {
        let off = addr.checked_sub(self.base)? as usize;
        self.bytes
            .get(off..off + 4)
            .map(|b| u32::from_le_bytes(b.try_into().unwrap()))
    }

    pub fn u16(&self, addr: u32) -> Option<u16> {
        let off = addr.checked_sub(self.base)? as usize;
        self.bytes
            .get(off..off + 2)
            .map(|b| u16::from_le_bytes(b.try_into().unwrap()))
    }
}

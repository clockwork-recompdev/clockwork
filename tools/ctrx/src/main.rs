//! ctrx: unpacks a decrypted 3DS dump for analysis.
//!
//! Usage: ctrx <dump.3ds|dump.cxi> [out_dir]   (out_dir defaults to ./extracted)
//!
//! Writes exheader.bin, exefs/*, code.bin (decompressed .code), romfs/**,
//! and info.txt with the memory layout and anything relevant to recompilation.

mod lz;
mod ncch;
mod romfs;
mod util;

use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 || args.len() > 3 {
        eprintln!("usage: ctrx <dump.3ds|dump.cxi> [out_dir]");
        std::process::exit(2);
    }
    let out = PathBuf::from(args.get(2).map(String::as_str).unwrap_or("extracted"));
    if let Err(e) = run(Path::new(&args[1]), &out) {
        eprintln!("error: {e}");
        std::process::exit(1);
    }
}

fn write(path: &Path, data: &[u8]) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
    }
    fs::write(path, data).map_err(|e| format!("{}: {e}", path.display()))
}

fn run(input: &Path, out: &Path) -> Result<(), String> {
    // mapped, not read: untrimmed dumps are 4 GiB and we only touch partition 0
    let handle = fs::File::open(input).map_err(|e| format!("{}: {e}", input.display()))?;
    // SAFETY: the dump is only read, and isn't expected to change while we run
    let file = unsafe { memmap2::Mmap::map(&handle) }.map_err(|e| format!("{}: {e}", input.display()))?;
    let (start, end) = ncch::main_partition(&file)?;
    let n = ncch::Ncch::parse(&file[start..end])?;

    let mut info = String::new();
    writeln!(info, "program id:   {:016X}", n.program_id).unwrap();
    writeln!(info, "product code: {}", n.product_code).unwrap();

    let exheader = n.exheader.ok_or("NCCH has no exheader (not an executable partition)")?;
    write(&out.join("exheader.bin"), exheader)?;
    let ex = ncch::ExHeader::parse(exheader);
    writeln!(info, "title:        {}", ex.title).unwrap();
    writeln!(info, "\n[memory layout]").unwrap();
    for (name, seg) in [(".text", &ex.text), (".rodata", &ex.rodata), (".data", &ex.data)] {
        writeln!(
            info,
            "{name:<8} addr {:08X}  size {:08X}  pages {}",
            seg.address, seg.size, seg.pages
        )
        .unwrap();
    }
    writeln!(info, ".bss     size {:08X}", ex.bss_size).unwrap();
    writeln!(info, "stack    size {:08X}", ex.stack_size).unwrap();
    writeln!(info, "code compressed: {}", ex.code_compressed).unwrap();
    writeln!(info, "\n[system module dependencies]").unwrap();
    for id in &ex.dependencies {
        writeln!(info, "{id:016X}").unwrap();
    }

    let exefs_region = n.exefs.as_ref().ok_or("NCCH has no ExeFS")?;
    let exefs = &n.data[exefs_region.offset..exefs_region.offset + exefs_region.size];
    writeln!(info, "\n[exefs]").unwrap();
    for f in ncch::exefs_files(exefs)? {
        writeln!(info, "{:<8} {:08X} bytes", f.name, f.data.len()).unwrap();
        write(&out.join("exefs").join(&f.name), f.data)?;
        if f.name == ".code" {
            let code = if ex.code_compressed {
                lz::decompress(f.data)?
            } else {
                f.data.to_vec()
            };
            let expected = (ex.text.pages + ex.rodata.pages + ex.data.pages) as usize * 0x1000;
            writeln!(
                info,
                "code.bin {:08X} bytes (segments span {:08X})",
                code.len(),
                expected
            )
            .unwrap();
            write(&out.join("code.bin"), &code)?;
        }
    }

    if let Some(r) = &n.romfs {
        let romfs = &n.data[r.offset..r.offset + r.size];
        let files = romfs::list(romfs)?;
        let mut modules = Vec::new();
        for f in &files {
            write(&out.join("romfs").join(&f.path), &romfs[f.offset..f.offset + f.size])?;
            let ext = f.path.extension().and_then(|e| e.to_str()).unwrap_or("");
            if matches!(ext, "cro" | "crs" | "crr") {
                modules.push(f.path.display().to_string());
            }
        }
        writeln!(info, "\n[romfs]\n{} files", files.len()).unwrap();
        writeln!(info, "\n[dynamic code modules (CRO/CRS/CRR)]").unwrap();
        if modules.is_empty() {
            writeln!(info, "none").unwrap();
        }
        for m in modules {
            writeln!(info, "{m}").unwrap();
        }
    }

    write(&out.join("info.txt"), info.as_bytes())?;
    print!("{info}");
    Ok(())
}

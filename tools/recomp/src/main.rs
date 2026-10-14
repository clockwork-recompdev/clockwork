mod arm;
mod difftest;
mod discover;
mod emit;
mod fmt;
mod image;
mod insn;
mod thumb;

use image::Image;
use std::path::Path;

const USAGE: &str = "usage:
  recomp disasm <extracted_dir> arm|thumb [start end]   linear-sweep disassembly of .text (hex addresses)
  recomp analyze <extracted_dir>                       function discovery report
  recomp fn <extracted_dir> <addr>                     what discovery knows about one function
  recomp emit <extracted_dir> <out_dir> [<other_version_dir>...]
                                                       generate C for every discovered function; with
                                                       other versions of the same code, the C runs those too
  recomp difftest <extracted_dir> <out_dir> <count> <states> <seed>
                                                       generate qemu-vs-native instruction tests";

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let result = match args.first().map(String::as_str) {
        Some("disasm") if args.len() == 3 || args.len() == 5 => disasm(&args[1..]),
        Some("analyze") if args.len() == 2 => analyze(Path::new(&args[1])),
        Some("emit") if args.len() >= 3 => emit_c(Path::new(&args[1]), Path::new(&args[2]), &args[3..]),
        Some("difftest") if args.len() == 6 => difftest_gen(&args[1..]),
        Some("fn") if args.len() == 3 => parse_hex(&args[2]).and_then(|a| function_info(Path::new(&args[1]), a)),
        _ => Err(USAGE.to_string()),
    };
    if let Err(e) = result {
        eprintln!("{e}");
        std::process::exit(1);
    }
}

fn parse_hex(s: &str) -> Result<u32, String> {
    u32::from_str_radix(s.trim_start_matches("0x"), 16).map_err(|e| format!("{s}: {e}"))
}

fn disasm(args: &[String]) -> Result<(), String> {
    use std::io::Write;
    let image = Image::load(Path::new(&args[0]))?;
    let thumb = match args[1].as_str() {
        "arm" => false,
        "thumb" => true,
        other => return Err(format!("unknown mode {other}\n{USAGE}")),
    };
    let (start, end) = if args.len() == 4 {
        (parse_hex(&args[2])?, parse_hex(&args[3])?)
    } else {
        (image.text.addr, image.text.addr + image.text.size)
    };

    let mut out = std::io::BufWriter::new(std::io::stdout().lock());
    let mut addr = start;
    while addr < end {
        let (insn, step) = if thumb {
            let Some(hw) = image.u16(addr) else { break };
            let insn = thumb::decode(addr, hw, image.u16(addr + 2));
            (insn, insn.map_or(2, |i| i.size as u32))
        } else {
            let Some(w) = image.u32(addr) else { break };
            (arm::decode(addr, w), 4)
        };
        let text = insn.map(|i| i.to_string()).unwrap_or_else(|| "<undefined>".into());
        writeln!(out, "{addr:x}: {text}").map_err(|e| e.to_string())?;
        addr += step;
    }
    Ok(())
}

fn analyze(extracted: &Path) -> Result<(), String> {
    use discover::IssueKind;
    let image = Image::load(extracted)?;
    let discovery = discover::discover(&image);
    println!(
        "reached by direct calls from the entry point: {} functions; rest from pointers and gap filling \
         (rejected candidates: {} ARM, {} Thumb)",
        discovery.direct, discovery.rejected_arm, discovery.rejected_thumb
    );
    println!(
        "noreturn functions: {} (after {} passes)",
        discovery.noreturn.len(),
        discovery.passes
    );
    let a = discovery.analysis;

    let text = &image.text;
    let mut covered = vec![false; text.size as usize];
    let mut mark = |addr: u32, len: u32| {
        for b in addr..addr + len {
            if let Some(c) = covered.get_mut((b - text.addr) as usize) {
                *c = true;
            }
        }
    };
    let (mut arm_fns, mut thumb_fns, mut insn_count) = (0, 0, 0);
    for f in a.functions.values() {
        if f.thumb {
            thumb_fns += 1
        } else {
            arm_fns += 1
        }
        insn_count += f.insns.len();
        for i in f.insns.values() {
            mark(i.addr, i.size as u32);
        }
    }
    for &w in &a.data_words {
        mark(w, 4);
    }
    let covered_bytes = covered.iter().filter(|&&c| c).count();

    println!("functions: {} ({arm_fns} ARM, {thumb_fns} Thumb)", a.functions.len());
    println!("instructions: {insn_count}");
    println!(
        "coverage of .text: {:.1}% ({covered_bytes} of {} bytes incl. {} literal/table words)",
        100.0 * covered_bytes as f64 / text.size as f64,
        text.size,
        a.data_words.len()
    );
    println!(
        "jump tables resolved: {}",
        a.functions.values().map(|f| f.jump_tables.len()).sum::<usize>()
    );

    for kind in [
        IssueKind::Undecodable,
        IssueKind::IndirectJump,
        IssueKind::TargetOutsideText,
        IssueKind::ModeConflict,
    ] {
        let list: Vec<_> = a.issues.iter().filter(|i| i.kind == kind).collect();
        println!("\n{kind:?}: {}", list.len());
        for i in list.iter().take(8) {
            let text = a
                .functions
                .get(&i.function)
                .and_then(|f| f.insns.range(..=i.addr).next_back())
                .map(|(_, insn)| insn.to_string())
                .unwrap_or_default();
            println!("  {:08x} in fn {:08x}  (last insn: {text})", i.addr, i.function);
        }
    }

    let mut gaps = Vec::new();
    let mut start = None;
    for (i, &c) in covered.iter().chain(std::iter::once(&true)).enumerate() {
        match (c, start) {
            (false, None) => start = Some(i),
            (true, Some(s)) => {
                gaps.push((i - s, text.addr + s as u32));
                start = None;
            }
            _ => {}
        }
    }
    println!("\nrejected after exploring: {}", a.rejections.len());
    for thumb in [false, true] {
        for kind in [
            IssueKind::Undecodable,
            IssueKind::ModeConflict,
            IssueKind::TargetOutsideText,
        ] {
            let list: Vec<_> = a
                .rejections
                .iter()
                .filter(|r| r.kind == kind && r.thumb == thumb)
                .collect();
            let examples: Vec<String> = list
                .iter()
                .take(4)
                .map(|r| format!("{:08x}->{:08x}", r.entry, r.at))
                .collect();
            let mode = if thumb { "Thumb" } else { "ARM" };
            println!("  {mode} {kind:?}: {}  e.g. {}", list.len(), examples.join(" "));
        }
    }

    // uncovered ARM prologues (push {.., lr}) hint at functions discovery missed
    let missed_prologues = gaps
        .iter()
        .flat_map(|&(len, addr)| (addr..addr + len as u32).step_by(4).filter(|a| a % 4 == 0))
        .filter(|&a| {
            arm::decode(a, image.u32(a).unwrap_or(0)).is_some_and(|i| {
                i.cond == insn::Cond::Al
                    && matches!(i.op, insn::Op::Multi { load: false, rn: insn::SP, regs, writeback: true, .. }
                        if regs & (1 << insn::LR) != 0)
            })
        })
        .count();
    println!("\nuncovered ARM push-{{.., lr}} prologues: {missed_prologues}");
    gaps.sort_unstable_by(|a, b| b.cmp(a));
    println!("\nuncovered: {} gaps; largest:", gaps.len());
    for (len, addr) in gaps.iter().take(10) {
        println!("  {addr:08x} +{len:#x}");
    }
    Ok(())
}

fn function_info(extracted: &Path, addr: u32) -> Result<(), String> {
    let image = Image::load(extracted)?;
    let discovery = discover::discover(&image);
    let functions = &discovery.analysis.functions;
    let Some(f) = functions.get(&addr) else {
        let owner = functions.values().find(|f| f.insns.contains_key(&addr));
        let rejection = discovery.analysis.rejections.iter().find(|r| r.entry == addr);
        return Err(match (owner, rejection) {
            (Some(f), _) => format!("{addr:08x} is inside function {:08x}", f.entry),
            (None, Some(r)) => format!(
                "{addr:08x} was tried as a {} function and rejected: {:?} at {:08x}",
                if r.thumb { "Thumb" } else { "ARM" },
                r.kind,
                r.at
            ),
            (None, None) => format!("{addr:08x} is not a discovered function (never a candidate)"),
        });
    };
    println!(
        "{:08x} {} {} instructions, noreturn: {}, exits: {}",
        f.entry,
        if f.thumb { "Thumb" } else { "ARM" },
        f.insns.len(),
        discovery.noreturn.contains(&f.entry),
        f.exits
    );
    let tails: Vec<String> = f
        .tail_calls
        .iter()
        .map(|t| {
            format!(
                "{t:08x}{}",
                if discovery.noreturn.contains(t) {
                    " (noreturn)"
                } else {
                    ""
                }
            )
        })
        .collect();
    println!("tail calls: {}", tails.join(", "));
    for insn in f.insns.values() {
        let mark = match insn.op {
            insn::Op::Branch { target, link: true, .. } if discovery.noreturn.contains(&target) => {
                "   <- noreturn call"
            }
            _ => "",
        };
        println!("  {:08x}: {insn}{mark}", insn.addr);
    }
    Ok(())
}

fn emit_c(extracted: &Path, out_dir: &Path, others: &[String]) -> Result<(), String> {
    let image = Image::load(extracted)?;
    let discovery = discover::discover(&image);
    // other versions: the same layout, differing in a few instructions
    let mut variants = emit::Variants::new();
    for other in others {
        let o = Image::load(Path::new(other))?;
        if (o.text.addr, o.text.size) != (image.text.addr, image.text.size) {
            return Err(format!("{other}: .text is laid out differently, so it isn't the same code"));
        }
        let mut differ = 0;
        for addr in (image.text.addr..image.text.addr + image.text.size).step_by(4) {
            let (a, b) = (image.u32(addr), o.u32(addr));
            if a != b {
                let w = b.ok_or_else(|| format!("{other}: .text ends early"))?;
                let words = variants.entry(addr).or_default();
                if !words.contains(&w) {
                    words.push(w);
                }
                differ += 1;
            }
        }
        println!("{other}: {differ} instruction(s) differ");
    }
    let stats = emit::write_all(&discovery.analysis, &discovery.noreturn, &variants, out_dir)?;
    println!(
        "wrote {} functions in {} files ({:.1} MB) to {}",
        stats.functions,
        stats.files,
        stats.bytes as f64 / 1e6,
        out_dir.display()
    );
    Ok(())
}

fn difftest_gen(args: &[String]) -> Result<(), String> {
    let num = |s: &str| s.parse::<u64>().map_err(|e| format!("{s}: {e}"));
    let image = Image::load(Path::new(&args[0]))?;
    let discovery = discover::discover(&image);
    let n = difftest::generate(
        &discovery.analysis,
        Path::new(&args[1]),
        num(&args[2])? as usize,
        num(&args[3])? as usize,
        num(&args[4])?,
    )?;
    println!("wrote {n} tests to {}", args[1]);
    Ok(())
}

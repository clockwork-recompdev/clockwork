//! Decompressor for the backwards LZ77 ("BLZ") used on ExeFS `.code`
//! when the exheader's CompressExefsCode flag is set.
//!
//! The compressed data is read from the end towards the start. The last
//! 8 bytes are a footer:
//!   u32 at len-8: bits 24..32 = footer length, bits 0..24 = compressed region size
//!   u32 at len-4: extra bytes the output grows by

pub fn decompress(input: &[u8]) -> Result<Vec<u8>, String> {
    if input.len() < 8 {
        return Err("BLZ input shorter than its footer".into());
    }
    let len = input.len();
    let info = u32::from_le_bytes(input[len - 8..len - 4].try_into().unwrap());
    let extra = u32::from_le_bytes(input[len - 4..].try_into().unwrap()) as usize;

    let footer_len = (info >> 24) as usize;
    let region_len = (info & 0x00FF_FFFF) as usize;
    if footer_len > len || region_len > len {
        return Err("BLZ footer points outside the input".into());
    }

    let out_len = len + extra;
    let mut out = vec![0u8; out_len];
    out[..len].copy_from_slice(input);

    let mut src = len - footer_len;
    let stop = len - region_len;
    let mut dst = out_len;

    while src > stop {
        src -= 1;
        let mut control = input[src];
        for _ in 0..8 {
            if src <= stop {
                break;
            }
            if control & 0x80 != 0 {
                if src < stop + 2 {
                    return Err("BLZ back-reference truncated".into());
                }
                src -= 2;
                let pair = u16::from_le_bytes([input[src], input[src + 1]]) as usize;
                let count = ((pair >> 12) & 0xF) + 3;
                let offset = (pair & 0x0FFF) + 2;
                if dst < count {
                    return Err("BLZ output underflow".into());
                }
                for _ in 0..count {
                    let from = dst + offset;
                    if from >= out_len {
                        return Err("BLZ back-reference past end of output".into());
                    }
                    dst -= 1;
                    out[dst] = out[from];
                }
            } else {
                if dst == 0 {
                    return Err("BLZ output underflow".into());
                }
                src -= 1;
                dst -= 1;
                out[dst] = input[src];
            }
            control <<= 1;
        }
    }

    Ok(out)
}

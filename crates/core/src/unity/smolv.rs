// SPDX-License-Identifier: GPL-3.0-or-later
//! SMOL-V decoder adapted from Aras Pranckevicius's public-domain reference
//! (https://github.com/aras-p/smol-v/tree/6303e979b59153ee7007fd14e06d3fed8687b64c).

use super::{MAX_BLOCK, le_u32, take};

const SPIRV_MAGIC: u32 = 0x0723_0203;
pub(super) const MAGIC: &[u8] = b"LOMS";
const DECORATE: u32 = 71;
const MEMBER_DECORATE: u32 = 72;
const SHUFFLE: u32 = 79;
const COMPACT_SHUFFLE: u32 = 13;

const HAS_RESULT: u8 = 1;
const HAS_TYPE: u8 = 2;
const VARINT_OPERANDS: u8 = 4;

// Indexed by SPIR-V opcode. The low three bits hold the flags above,
// followed by the number of operands stored relative to the result ID.
#[rustfmt::skip]
const OP_ENCODING: [u8; 367] = [
    0, 3, 0, 4, 0, 0, 0, 0, 4, 3, 0, 1, 7, 23, 4, 4, 4, 4, 3, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 4,
    3, 3, 3, 3, 75, 7, 3, 3, 3, 3, 3, 75, 3, 3, 7, 3, 0, 75, 3, 7,
    3, 15, 20, 0, 0, 7, 3, 3, 3, 3, 3, 4, 4, 1, 0, 0, 3, 15, 23, 23,
    75, 15, 23, 11, 3, 3, 3, 23, 23, 31, 31, 23, 23, 31, 31, 23, 31, 31, 23, 28,
    11, 11, 11, 19, 11, 19, 11, 11, 3, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 15, 11, 3, 11, 11, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 3, 11, 11, 11, 11, 11, 11,
    11, 19, 19, 19, 19, 19, 19, 19, 11, 27, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 3, 3, 19, 19, 19, 19, 19, 19,
    11, 35, 27, 27, 11, 11, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0,
    0, 0, 3, 3, 24, 16, 3, 3, 0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 20, 12, 1, 8, 28, 0, 0, 0, 0, 0, 0, 0, 3, 3,
    0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    0, 0, 3, 3, 3, 3, 3, 0, 0, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 3,
    3, 0, 0, 3, 3, 23, 23, 31, 31, 23, 23, 31, 31, 23, 31, 31, 11, 0, 3, 0,
    3, 3, 3, 3, 3, 3, 3, 3, 7, 20, 3, 4, 4, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15,
];

fn varint(data: &mut &[u8]) -> Option<u32> {
    let mut value = 0;
    for shift in (0..=28).step_by(7) {
        let byte = take(data, 1)?[0];
        if shift == 28 && byte > 15 {
            return None;
        }
        value |= u32::from(byte & 127) << shift;
        if byte & 128 == 0 {
            return Some(value);
        }
    }
    None
}

fn zigzag(value: u32) -> u32 {
    (value >> 1) ^ 0u32.wrapping_sub(value & 1)
}

fn opcode(encoded: u32) -> u32 {
    const SWAPS: [(u32, u32); 12] = [
        (0, DECORATE),
        (1, 61),
        (2, 62),
        (3, 65),
        (4, SHUFFLE),
        (7, MEMBER_DECORATE),
        (8, 248),
        (9, 59),
        (10, 133),
        (11, 129),
        (14, 32),
        (15, 127),
    ];
    SWAPS
        .iter()
        .find_map(|&(left, right)| {
            if encoded == left {
                Some(right)
            } else if encoded == right {
                Some(left)
            } else {
                None
            }
        })
        .unwrap_or(encoded)
}

fn push(words: &mut Vec<u8>, word: u32) {
    words.extend_from_slice(&word.to_le_bytes());
}

/// Decode a module at the start of a Unity shader payload and return its input
/// length. The same version tag was used by two historical encodings.
pub(super) fn decode(data: &[u8], legacy_first: bool, words: &mut Vec<u8>) -> Option<usize> {
    decode_variant(data, legacy_first, words).or_else(|| decode_variant(data, !legacy_first, words))
}

fn decode_variant(mut data: &[u8], legacy: bool, words: &mut Vec<u8>) -> Option<usize> {
    let input_len = data.len();
    if take(&mut data, 4)? != MAGIC {
        return None;
    }
    let version = le_u32(&mut data)?;
    let generator = le_u32(&mut data)?;
    let bound = le_u32(&mut data)?;
    let schema = le_u32(&mut data)?;
    let size = le_u32(&mut data)? as usize;
    let encoding = version >> 24;
    let version = version & 0x00ff_ffff;
    if encoding > 1
        || (legacy && encoding != 0)
        || !(0x10000..=0x10600).contains(&version)
        || version & 0xff != 0
        || bound == 0
        || schema != 0
        || size % 4 != 0
        || !(20..=MAX_BLOCK).contains(&size)
    {
        return None;
    }
    let known_ops = if encoding == 0 {
        &OP_ENCODING[..331]
    } else {
        &OP_ENCODING
    };
    words.clear();
    for value in [SPIRV_MAGIC, version, generator, bound, schema] {
        push(words, value);
    }

    let (mut result, mut target) = (0u32, 0u32);
    while words.len() < size {
        let header = varint(&mut data)?;
        let op = opcode(((header >> 4) & 0xfff0) | (header & 0xf));
        let compact = op == COMPACT_SHUFFLE;
        let extra = match op {
            SHUFFLE | COMPACT_SHUFFLE => 4,
            DECORATE => 2,
            61 | 65 => 3, // Load, AccessChain.
            _ => 0,
        };
        let count = (((header >> 20) << 4) | ((header >> 4) & 0xf)) as usize + 1 + extra;
        let end = words.len() + 4 * count;
        if end > size || count > u16::MAX as usize {
            return None;
        }
        let op = if compact { SHUFFLE } else { op };
        push(words, (count as u32) << 16 | op);
        let flags = known_ops.get(op as usize).copied().unwrap_or(0);
        if flags & HAS_TYPE != 0 {
            push(words, varint(&mut data)?);
        }
        if flags & HAS_RESULT != 0 {
            result = result.wrapping_add(zigzag(varint(&mut data)?));
            push(words, result);
        }
        if matches!(op, DECORATE | MEMBER_DECORATE) {
            let delta = varint(&mut data)?;
            target = target.wrapping_add(if legacy { delta } else { zigzag(delta) });
            push(words, target);
        }
        if words.len() > end {
            return None;
        }
        if op == MEMBER_DECORATE && !legacy {
            member_decorations(&mut data, words, size, end, target)?;
            continue;
        }

        let zig = !legacy || matches!(op, 224 | 225 | 246 | 247 | 249 | 250 | 329);
        for _ in 0..usize::from(flags >> 3).min((end - words.len()) / 4) {
            let delta = varint(&mut data)?;
            push(
                words,
                result.wrapping_sub(if zig { zigzag(delta) } else { delta }),
            );
        }
        if compact && count <= 9 {
            let swizzle = take(&mut data, 1)?[0];
            if words.len() + 4 * count.saturating_sub(5) != end {
                return None;
            }
            for shift in [6, 4, 2, 0].into_iter().take(count - 5) {
                push(words, u32::from((swizzle >> shift) & 3));
            }
        } else if flags & VARINT_OPERANDS != 0 {
            while words.len() < end {
                push(words, varint(&mut data)?);
            }
        } else {
            words.extend_from_slice(take(&mut data, end - words.len())?);
        }
    }
    Some(input_len - data.len())
}

fn member_decorations(
    data: &mut &[u8],
    words: &mut Vec<u8>,
    size: usize,
    first_end: usize,
    target: u32,
) -> Option<()> {
    let count = take(data, 1)?[0];
    if count == 0 {
        return None;
    }
    let (mut member, mut offset) = (0u32, 0u32);
    for index in 0..count {
        member = member.wrapping_add(varint(data)?);
        let decoration = varint(data)?;
        let operands = match decoration {
            0 | 2..=5 => 0,
            29..=37 => 1,
            _ => varint(data)? as usize,
        };
        let end = words.len() + (if index == 0 { 2 } else { 4 } + operands) * 4;
        if end > size || operands > u16::MAX as usize - 4 || (index == 0 && end != first_end) {
            return None;
        }
        if index != 0 {
            push(words, ((operands as u32 + 4) << 16) | MEMBER_DECORATE);
            push(words, target);
        }
        push(words, member);
        push(words, decoration);
        for _ in 0..operands {
            let value = varint(data)?;
            if decoration == 35 {
                offset = offset.wrapping_add(value);
                push(words, offset);
            } else {
                push(words, value);
            }
        }
    }
    Some(())
}

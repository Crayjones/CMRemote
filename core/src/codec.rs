use std::ffi::{c_char, c_uint};

/// Decompresses one bzip2 stream of at most `limit` output bytes.
pub fn decompress_bzip2(data: &[u8], limit: usize) -> Result<Vec<u8>, String> {
    let source_len = c_uint::try_from(data.len()).map_err(|_| "bzip2 input is too large".to_owned())?;
    let mut length = c_uint::try_from(limit).map_err(|_| "bzip2 output limit is too large".to_owned())?;
    let mut output = vec![0u8; limit];
    let mut input = data.to_vec();
    // SAFETY: both buffers are live, sized as passed, and not otherwise borrowed.
    let status = unsafe {
        libbz2_rs_sys::BZ2_bzBuffToBuffDecompress(output.as_mut_ptr() as *mut c_char, &mut length,
            input.as_mut_ptr() as *mut c_char, source_len, 0, 0)
    };
    match status {
        libbz2_rs_sys::BZ_OK => { output.truncate(length as usize); Ok(output) }
        libbz2_rs_sys::BZ_OUTBUFF_FULL => Err("bzip2 output exceeds its limit".to_owned()),
        _ => Err(format!("bzip2 data is invalid ({status})")),
    }
}

#[cfg(test)]
mod tests {
    use super::decompress_bzip2;
    // `printf 'CMRemote OpenH264\n' | bzip2 -9`
    const SAMPLE: &[u8] = &[0x42, 0x5a, 0x68, 0x39, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0x6, 0xf, 0x7b, 0x18, 0x0, 0x0, 0x2, 0xdf, 0x80, 0x0, 0x10, 0x40, 0x0, 0x15, 0x0, 0x8, 0x42, 0x90, 0x0, 0x2, 0x3, 0xc4, 0x0, 0x20, 0x0, 0x22, 0x9a, 0x61, 0x34, 0x3d, 0x4d, 0xa1, 0x4c, 0x0, 0x13, 0x45, 0xee, 0xe2, 0x1c, 0x4c, 0xf0, 0x83, 0xec, 0x55, 0x1b, 0x17, 0x72, 0x45, 0x38, 0x50, 0x90, 0x6, 0xf, 0x7b, 0x18];

    #[test]
    fn decompresses_within_limit() {
        assert_eq!(decompress_bzip2(SAMPLE, 64).unwrap(), b"CMRemote OpenH264\n");
        assert_eq!(decompress_bzip2(SAMPLE, 4).unwrap_err(), "bzip2 output exceeds its limit");
        assert!(decompress_bzip2(&SAMPLE[..SAMPLE.len() - 4], 64).is_err());
        assert!(decompress_bzip2(b"not bzip2", 64).is_err());
    }
}

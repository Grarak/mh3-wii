//! Small, synchronous C bridge to nod. All source I/O goes through native
//! 64-bit positioned reads; the decoder never creates a file or a worker.
use nod::{Disc, PartitionBase, PartitionKind};
use std::{
    ffi::c_void,
    io::{self, Read, Seek, SeekFrom},
    ptr,
};

type ReadAt = unsafe extern "C" fn(*mut c_void, u64, *mut u8, usize) -> isize;
#[derive(Clone)]
struct Source {
    context: *mut c_void,
    size: u64,
    position: u64,
    read_at: ReadAt,
}
// C++ owns a read-only handle with native pread. Clones have their own cursors.
unsafe impl Send for Source {}
unsafe impl Sync for Source {}
impl Read for Source {
    fn read(&mut self, output: &mut [u8]) -> io::Result<usize> {
        let count = output.len().min(
            self.size
                .saturating_sub(self.position)
                .min(usize::MAX as u64) as usize,
        );
        if count == 0 {
            return Ok(0);
        }
        let result =
            unsafe { (self.read_at)(self.context, self.position, output.as_mut_ptr(), count) };
        if result < 0 || result as usize > count {
            return Err(io::Error::other("native ROM read failed"));
        }
        self.position += result as u64;
        Ok(result as usize)
    }
}
impl Seek for Source {
    fn seek(&mut self, seek: SeekFrom) -> io::Result<u64> {
        let next = match seek {
            SeekFrom::Start(at) => i128::from(at),
            SeekFrom::Current(delta) => i128::from(self.position) + i128::from(delta),
            SeekFrom::End(delta) => i128::from(self.size) + i128::from(delta),
        };
        if next < 0 || next > i128::from(u64::MAX) {
            return Err(io::Error::other("invalid ROM seek"));
        }
        self.position = next as u64;
        Ok(self.position)
    }
}
struct Reader {
    disc: Disc,
    partition: Box<dyn PartitionBase>,
    flat_size: u64,
}
fn error(buffer: *mut u8, capacity: usize, message: &str) {
    if buffer.is_null() || capacity == 0 {
        return;
    }
    let length = message.len().min(capacity - 1);
    unsafe {
        ptr::copy_nonoverlapping(message.as_ptr(), buffer, length);
        *buffer.add(length) = 0;
    }
}
fn word(bytes: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(bytes[at..at + 4].try_into().unwrap())
}
fn validate_envelope(source: &mut Source) -> io::Result<()> {
    let mut header = [0u8; 0x124];
    source.read_exact(&mut header)?;
    source.seek(SeekFrom::Start(0))?;
    if &header[..4] == b"RVZ\x01" || &header[..4] == b"WIA\x01" {
        // Limit chunk buffers and uncompressed tables before nod allocates.
        // This covers the usual 128KiB/2MiB RVZ chunk sizes on a retail Vita.
        if word(&header, 0x0c) > 0x1000
            || !(0x8000..=0x200000).contains(&word(&header, 0x54))
            || word(&header, 0xd8) > 32
            || word(&header, 0xfc) > 16384
            || word(&header, 0x10c) > 131072
            || word(&header, 0x108) > 0x100000
            || word(&header, 0x118) > 0x400000
        {
            return Err(io::Error::other(
                "RVZ metadata/chunk size exceeds the Vita processing limit",
            ));
        }
    }
    Ok(())
}
#[no_mangle]
pub unsafe extern "C" fn mh3_rom_open(
    context: *mut c_void,
    size: u64,
    read_at: ReadAt,
    message: *mut u8,
    capacity: usize,
) -> *mut c_void {
    let result = (|| -> Result<Reader, String> {
        let mut source = Source {
            context,
            size,
            position: 0,
            read_at,
        };
        validate_envelope(&mut source).map_err(|e| e.to_string())?;
        // Default options leave expensive hash reconstruction disabled.
        let disc = Disc::new_stream(Box::new(source)).map_err(|e| e.to_string())?;
        let data: Vec<_> = disc
            .partitions()
            .iter()
            .filter(|p| p.kind == PartitionKind::Data)
            .collect();
        if data.len() != 1 {
            return Err("Expected exactly one Wii data partition".into());
        }
        let flat_size = u64::from(data[0].data_end_sector - data[0].data_start_sector) * 0x7c00;
        let partition = disc
            .open_partition(data[0].index)
            .map_err(|e| e.to_string())?;
        Ok(Reader {
            disc,
            partition,
            flat_size,
        })
    })();
    match result {
        Ok(reader) => Box::into_raw(Box::new(reader)).cast(),
        Err(e) => {
            error(message, capacity, &e);
            ptr::null_mut()
        }
    }
}
#[no_mangle]
pub unsafe extern "C" fn mh3_rom_size(reader: *mut c_void, flat: i32) -> u64 {
    let reader = &mut *reader.cast::<Reader>();
    if flat != 0 {
        reader.flat_size
    } else {
        reader.disc.disc_size()
    }
}
#[no_mangle]
pub unsafe extern "C" fn mh3_rom_read(
    reader: *mut c_void,
    flat: i32,
    at: u64,
    output: *mut u8,
    count: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    let reader = &mut *reader.cast::<Reader>();
    let output = std::slice::from_raw_parts_mut(output, count);
    let result = if flat != 0 {
        reader
            .partition
            .seek(SeekFrom::Start(at))
            .and_then(|_| reader.partition.read_exact(output))
    } else {
        reader
            .disc
            .seek(SeekFrom::Start(at))
            .and_then(|_| reader.disc.read_exact(output))
    };
    match result {
        Ok(()) => 0,
        Err(e) => {
            error(message, capacity, &e.to_string());
            -1
        }
    }
}
#[no_mangle]
pub unsafe extern "C" fn mh3_rom_close(reader: *mut c_void) {
    if !reader.is_null() {
        drop(Box::from_raw(reader.cast::<Reader>()));
    }
}

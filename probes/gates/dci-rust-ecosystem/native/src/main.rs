use std::io::{Read, Write};

fn main() {
    let directory = std::env::args().nth(1).expect("input directory");
    let directory = std::path::Path::new(&directory);
    std::fs::create_dir_all(directory).unwrap();
    let lengths = [0, 1, 7, 16, 255, 4096, 65537, 1048576];
    let mut results = std::fs::File::create(directory.join("expected.txt")).unwrap();
    for length in lengths {
        let bytes: Vec<u8> = (0..length).map(|i| ((i * 17 + (i >> 8) + 23) & 255) as u8).collect();
        let path = directory.join(format!("input-{length}.bin"));
        std::fs::write(&path, &bytes).unwrap();
        let mut file = std::fs::File::open(&path).unwrap();
        let mut crc = crc32fast::Hasher::new();
        let mut adler = adler2::Adler32::new();
        let mut chunk = [0u8; 997];
        loop {
            let count = file.read(&mut chunk).unwrap();
            if count == 0 { break; }
            crc.update(&chunk[..count]);
            adler.write_slice(&chunk[..count]);
        }
        let crc = crc.finalize();
        let adler = adler.checksum();
        assert_eq!(crc, crc32fast::hash(&bytes));
        assert_eq!(adler, adler2::adler32_slice(&bytes));
        writeln!(results, "{length} {crc} {adler}").unwrap();
    }
}

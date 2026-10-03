# std.miniz

`std.miniz` packages miniz 3.1.0 as a static Vyx dependency. The package owns
its native sources and does not require a system zlib, DLL, SO, include path,
or consumer link flag.

```toml
[dependencies]
miniz = { path = "std:miniz", target = "vyx_std_miniz" }
```

```vyx
use std.miniz;

match (Compressor.compress("payload")) {
    case Ok(data) => {
        match (data.decompress_to_string(1024)) {
            case Ok(text) => { print(text); }
            case Err(err) => { print(err.message); }
        }
    }
    case Err(err) => { print(err.message); }
}
```

`CompressedData` owns the compressed allocation, supports deep cloning, and
releases it from `drop()`. Decompression always requires an explicit output
limit. CRC32 and Adler32 operate on the complete Vyx string length rather than
stopping at the first zero byte.

The vendored miniz sources are from tag `3.1.0`; see `vendor/LICENSE`.

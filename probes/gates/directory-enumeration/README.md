# Native directory enumeration

Run on Linux or macOS with the current SDK compiler and its matching runtime:

```sh
python probes/gates/directory-enumeration/run.py --compiler /path/to/vyxc --cc /path/to/clang --output /tmp/vyx-directory-gate
```

The gate enumerates actual files twice, checks exact names including spaces,
UTF-8 and a long filename, then uses `std.fs.copyFilesWithExtension` through AOT.
Copies must contain exactly the two matching files with identical bytes.

`readdir` and access to `dirent.d_name` belong to the same native translation
unit. Platform headers select both the record layout and Darwin's inode ABI
symbol variant. Vyx callers receive a borrowed NUL-terminated name until the
next read or close; they do not inspect the record. End-of-directory and read
errors are separate outcomes. Windows returns 77 because it uses Win32 enumeration.

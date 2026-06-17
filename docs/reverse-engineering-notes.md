# Reverse Engineering Notes

## Local Evidence

The supplied folder contains these unique artifacts:

- `Editor.exe` and `Extractor.exe`, native 32-bit Visual C++ PE files from July 2011.
- `Joymax Pk2 Tools.exe`, native 32-bit MFC application identifying itself as Joymax PK2 Tools 1.2.
- `GFXFileManager.dll`, Joymax native 32-bit DLL exporting `GFXDllCreateObject`, `GFXDllReleaseObject`, and `GFXFMInfo`.
- `Password.txt`, which says official SRO uses `169841` and private-server passwords are produced by MD5 hashing the current PK2 Blowfish key.

Useful strings found in the binaries include:

- `JoyMax File Manager!`
- `PK2Header`
- `PK2EntryBlock`
- `Invalid Blowfish key.`
- `Invalid PK2 name.`
- `Invalid PK2 version.`
- `Extract Children?`
- `Import File`
- `Import Folder`

## Clean Rebuild Boundary

The new implementation does not link to, load, or redistribute `GFXFileManager.dll`. The old binaries are treated as behavior references only.

## Format Assumptions In V1

- Header size: 256 bytes.
- Entry block size: 2560 bytes.
- Entries per block: 20.
- Entry size: 128 bytes.
- Entry type: `0` empty, `1` folder, `2` file.
- Entry name: 81-byte null-terminated field.
- File/folder position field: little-endian 64-bit value.
- File size field: little-endian 32-bit value.
- Entry-chain field: little-endian 64-bit value.
- Directory blocks may be plaintext, Blowfish big-endian ECB, or Blowfish little-endian ECB.

These assumptions are isolated in `src/pk2core/archive.cpp`.

# Changelog

## 0.2.0 - 2026-07-13

### Highlights

- Added a built-in **Server Configuration** editor under `Tools > Server Configuration`.
- Added automatic saving after every successful archive modification.
- Improved compatibility with real Silkroad `Media.pk2` client configuration files.

### Server Configuration

- Edit the Content ID, client version, GatewayServer port, division names, and gateway IP addresses or host names without launching the legacy IPInput utility.
- Add, update, or remove multiple divisions and gateway addresses.
- Read and write the root-level `DIVISIONINFO.TXT`, `GATEPORT.TXT`, and encrypted `SV.T` files directly inside `Media.pk2`.
- Preserve unrelated bytes in `SV.T` while updating only the encrypted version block.
- Support the standard Silkroad little-endian version encryption and compatible alternate/legacy layouts.

### Automatic Saving

The open PK2 is now saved automatically after:

- Dragging files or folders into the archive.
- Importing a file or folder from the menu.
- Deleting an archive entry.
- Applying Server Configuration changes.

The existing `.bak` backup behavior remains enabled for every save. `File > Save` is still available as a manual fallback.

### Reliability

- Added in-memory archive file reading and replacement support.
- Added regression coverage for persisted byte replacement, multi-division server configuration, gateway ports, and both supported `SV.T` encryption byte orders.
- Updated the About dialog to display the application version.

## 0.1.0 - 2026-07-12

- Initial public source release of PK2 Workbench PRO.

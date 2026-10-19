# Saves: the NAND file system on host files

## What the original does

Wii games save through IOS's file system on the console's NAND flash
(`/dev/fs`, reached with `ISFS_*` calls that become `IOS_Open`/`Ioctl`).
MH3 creates its system file, six character slots, network data and a
save banner under its title directory on first boot; its save file is
`title/00010000/524d4850/data/nocopy/data00` (`524d4850` is `RMHP`). Every
NAND file and directory carries Wii ownership, permissions and attributes.

## What the port does instead

`/dev/fs` is answered from a directory of host files
(`runtime/src/ios.cpp`): `saves/nand` by default, `$MH3_USER_DIR/nand`
when set.

- Supported: directory and file creation, metadata get and set, rename,
  recursive delete, file status, reads, asynchronous writes and seeks;
  closing a written file syncs it.
- Wii ownership, permissions and attributes are kept in an extended
  attribute on Linux (`fsetxattr`), and in a journal beside the tree on the
  Vita (`nand-metadata.bin`; exFAT has no extended attributes,
  `runtime/vita/nand.inc`). The Vita syncs writes with `sceIoSync`.
- Paths are resolved relative to the tree: symlinks, `..` and special files
  are rejected; creation never truncates an existing file; recursive delete
  checks the whole tree before changing it and refuses open files.
- It is a single-title file system: open checks use the one title's UID and
  GID, not IOS's full security model (NAND bring-up).

## Why

The game's own save code (creating files, the "Creating file" screen,
reading slots) runs unchanged, and its saves are ordinary files the user can
back up or copy between hosts. Emulating the NAND at block level would need
a NAND image, its encryption keys and a file system implementation, none of
which the game needs.

## Example

A save is a plain file, so installing one for a test is a copy into the
user directory (`MH3_USER_DIR`, default `saves`):

```sh
mkdir -p saves/nand/title/00010000/524d4850/data/nocopy
cp my-data00 saves/nand/title/00010000/524d4850/data/nocopy/data00
```

## Applying it to another Wii game

Generic: the `/dev/fs` implementation, attribute storage and path checks
work for any title. Game-specific: the title ID in the paths, and which
NAND files outside its own title directory the game opens (system settings,
other titles' data, WiiConnect24): an absent file gets IOS's error, as on a
console without it, so log the `/dev/fs` paths a new game opens and check
what it does with each failure.

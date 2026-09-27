# Native NAND save storage

The original game save setup now creates and reopens its system file, six
character-save slots, network data, and save banner. Files live under
`saves/nand` (or `$MH3_USER_DIR/nand`). A Metal frame capture shows the original
“Creating file” screen. The save-creation boot completed 827 display copies;
a second boot reopened those files and reached the same next module prolog,
`com_data.rso` at `0x8086907C`. Gameplay and saving during gameplay remain unverified.

The IOS boundary supports directory/file creation, metadata get/set, rename,
recursive deletion, file status, asynchronous write and seek, and existing read
and close operations. Writable close synchronizes the host file. Wii ownership,
permissions, and attributes persist in a private extended attribute. Open checks
use the single-title UID/GID. This is a scoped single-title filesystem, not a
complete IOS ownership or filesystem-security model.

Descriptor-relative traversal rejects symlinks, invalid paths, and special files.
Creation never truncates an existing file. Recursive deletion validates the tree
before mutation and refuses open files. Tests use isolated temporary NAND roots;
they cover callback results, persistence, seek/readback, permissions, rename,
existing-file preservation, traversal/symlink rejection, and recursive deletion.

Evidence: `logs/nand-save-boot.log`, `logs/nand-reopen-boot.log`, and
`logs/nand-save-frame-750.png` (local, ignored). Both native and Metal builds have
runtime and audio CTest checks; the Metal boot uses the original guest save code.

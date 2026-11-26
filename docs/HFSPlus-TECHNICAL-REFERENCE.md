# HFSPlus: technical reference

## History and format discrimination

HFS Plus (Mac OS Extended) arrived with Mac OS 8.1 in 1998, enlarging allocation identifiers to 32 bits and adopting Unicode names. HFSX is a related format with version-specific catalog comparison semantics. A native HFS Plus volume begins with a header at partition-relative byte 1024, signature 0x482B and version 4; HFSX uses 0x4858 and version 5. An HFS wrapper may contain an embedded HFS Plus volume: resolve the wrapper allocation origin and embedded extent rather than applying the inner header offset to the outer partition. The alternate volume header is normally 1024 bytes before the end of the inner volume. Verify both independently.

## Volume header and special forks

All native integer fields are big-endian. Header offsets: signature/version 0/2, attributes 4, last mounted version 8, journal info block 12, creation/modify/backup/check dates 16/20/24/28, files/folders 32/36, allocation block bytes 40, total/free blocks 44/48, next allocation 52, resource/data clump sizes 56/60, next CNID 64, write count 68, encodings bitmap 72, Finder info 80..111. Five 80-byte fork descriptors start at 112 (allocation file), 192 (extents), 272 (catalog), 352 (attributes), 432 (startup). Validate block count, size, special fork ranges and feature/dirty flags. Volume attributes include unmounted bit 8, inconsistent bit 11 and journaled bit 13; journal existence does not prove recovery.

## Fork descriptors and extent mapping

An 80-byte fork descriptor has 64-bit logical size at 0, clump bytes 8, allocated block count 12 and eight extent pairs from 16; each pair contains 32-bit start and count. Allocation block n begins at n*block_bytes from the inner volume origin. Extents-overflow key: 16-bit length 0, fork type 2, pad 3, CNID 4 and 32-bit logical start block 8. Data fork type is 0, resource 0xff. A file logical block 9 in a first run of 6 and second run of 8 starting at physical block 200 maps to block 203. With 4096-byte blocks, byte 10 of that block maps to 831498. Check complete fork coverage, logical EOF, extent range and overflow ordering; do not expose slack. Allocation file bits are MSB-first, set means allocated.

## B-tree header and node traversal

A 14-byte node descriptor contains forward/backward node IDs at 0/4, signed kind 8, height 9, record count 10 and reserved 12. Kinds: index 0, header 1, map 2, leaf -1. Header node 0 begins with the descriptor then header record: depth 0, root 2, leaf records 6, first/last leaf 10/14, node size 18, max key length 20, total/free nodes 22/26, reserved 30, clump size 32, tree/key type 36/37 and attributes 38. Record offsets are 16-bit big-endian values at the end of each node, reverse order plus free-space boundary. The B-tree file's fork maps node n at n*node_size; nodes are not direct volume block addresses. Validate key lengths, offset ordering, map allocation, node height, siblings and child range before traversal.

## Catalog key and records

A catalog key has 16-bit key length at 0, parent CNID at 2, name length in UTF-16 units at 6 and big-endian code units from 8 (up to 255). Compare using the selected native key comparison, case-folding 0xCF or binary 0xBC where admitted. HFS Plus uses a particular Unicode decomposition/collation version, not generic contemporary normalization. Values begin with 16-bit type: 1 folder, 2 file, 3 folder thread, 4 file thread. A file record has flags 2, reserved 4, CNID 8, five date fields from 12, permissions at 32, Finder fields 48..79, text encoding 80, reserved 84, data fork 88 and resource fork 168; total 248 bytes. Thread stores parent CNID at 4, name length at 8. Match thread, parent, record type and unique identity.

## Object IDs, attributes and links

Root CNID is 2; extents 3, catalog 4, bad blocks 5, allocation 6, startup 7 and attributes 8; ordinary CNIDs begin at 16. Permissions encode owner, group, flags and mode; the special field has type-dependent semantics. HFS Plus hard links use native private-directory and special inode conventions, not simple repeated catalog CNIDs. Directory hard links have further restrictions. Attributes tree keys contain length, pad, CNID, start block and Unicode attribute name; value types 0x10 inline, 0x20 fork and 0x30 extent continuation require their own mapping. Preserve resource forks, FinderInfo, security attributes and unknown admitted metadata. Native timestamps are seconds from 1904-01-01 UTC.

## Journal and volume recovery

For journaled media the volume header points to a journal information block in allocation-block units. It identifies the journal storage and geometry. Journal headers and transaction block lists have versioned byte-order, checksum, circular-range and target-address semantics. Replay only fully validated committed transactions and only against the matching volume; handle external journal identity when admitted. Do not treat a clean flag, a readable catalog or alternate header as a substitute for native replay. A writer must order bitmap, catalog, extent, attribute and journal publication consistently, and respect volume lock and inconsistent-state flags. Exact journal record layouts and replay algorithms require additional primary-reference validation.

## Defragmentation, formatting and references

Reconstruct allocation from both forks of each file, special metadata files, overflow records and bad-block reservations; cross-check tree maps and volume bitmap. Relocation preserves fork identity, metadata and native link semantics; reserve destination, copy/verify, publish mappings under native transaction rules, then release source. Formatter must establish all special files and tree headers/maps, root and thread records, primary/alternate header and correct feature flags. Test native HFS Plus and HFSX fixtures, wrapped volumes, both forks, overflow, Unicode collisions, attributes, hard links, dirty journals and interrupted updates. Normative Apple Technical Note TN1150: https://developer.apple.com/library/archive/technotes/tn/tn1150.html ; pinned cross-check https://github.com/torvalds/linux/tree/v6.12/fs/hfsplus (hfsplus_raw.h, btree.c, extents.c, catalog.c). Earlier documentation is preserved as historical evidence; writer and replay qualification are separate.

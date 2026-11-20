# HFSPLUS / HFSX Common V2 migration status

The canonical `HFSPLUS.so` now contains real hosted Common V2
`CORE.PROBE`, independent read-only `MOUNT`/`UNMOUNT`,
`CORE.STAT`, `CORE.LIST`, `CORE.READ` and `VOLUME.QUERY`
operations. `native/hfsplus_catalog_v2.c` is the only format-owned reader:
it decodes the genuine HFS+ volume header and allocation-file bitmap, checks
B-tree leaf chains and Unicode-key record lengths, resolves directories
and reads file data through actual on-disk inline fork extents. Its source is
shared with a freestanding no-libc ELF embedded in the hosted SO as
`.infiltrator_boot`.

The production SO contains no V1 runtime service, Linux VFS reader, FUSE
adapter, or filesystem-specific `.ko`. The historical native worker is
retained for non-installed regression testing. Disposable Common V2 synthetic
images exercise nested catalogue entries, directory enumeration, byte-exact
file reads, missing paths and nonmutation.

**Firmware release remains blocked until all gates pass**:
(1) independently formatted `mkfs.hfsplus` images populated by `hpcopy`,
with real contents and corrupt metadata; (2) the production BOOT 0.1.77
validator against the actual packaged binary; (3) a disposable OVMF boot
that reads the real kernel and initramfs; (4) SHA-verified nonmutation and
portable DEB/native RUN CI. Source presence or compiling alone is not
completion. No firmware-qualified HFSPLUS release has been published here.

The present feature subset rejects UTF-16 names outside qualified ASCII
lookup/case handling, extended fork/extent-overflow records, compressed
or hardlinked files, unsupported HFSX collation and dirty journalled volumes
rather than manufacturing boot compatibility. Additional feature sets need
their own native engines and regression coverage.

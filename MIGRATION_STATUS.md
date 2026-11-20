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
(1) independently formatted `mkfs.hfsplus` images populated by the
CI runner's independent HFS+ writer on a disposable loopback image, with
real contents, UTF-8 filenames and corrupt metadata; (2) the unmodified
production BOOT validator against the actual packaged binary; (3) a disposable OVMF boot
that reads the real kernel and initramfs; (4) SHA-verified nonmutation and
portable DEB/native RUN CI. Source presence or compiling alone is not
completion. No firmware-qualified HFSPLUS release has been published here.

The on-disk catalogue uses genuine UTF-16BE keys. Directory enumeration now
converts valid BMP and supplementary-plane names to bounded UTF-8, recording
their actual output byte length. Apple's specifically documented internal
hard-link metadata directory (four U+0000 units plus "HFS+ Private Data")
is recognized and excluded from public directory enumeration, without
masking arbitrary embedded-NUL corruption. Filenames returned by LIST now
round-trip through STAT and READ using exact UTF-8 bytes, demonstrated on a
genuinely formatted HFS+ image containing a non-ASCII filename. ASCII-only
case folding remains supported; unqualified Unicode case-folding and
normalization aliases are not silently assumed. Extended fork/extent-overflow
records, compressed or hardlinked file payloads, unsupported HFSX collation
and dirty journalled volumes fail closed; no compatibility is faked.

The former `hpmount` test-media population step could not open the raw
`mkfs.hfsplus` volume and was replaced by a disposable, CI-only reference
writer. This never substitutes for the Common V2 reader in the installed SO.
The exact BOOT 0.1.77 validator artifact referenced by firmware CI is
currently unavailable at its prior official release URL (HTTP 404).
No release can be marked firmware-qualified until that external validator
and the real OVMF kernel/initramfs handoff are available and pass.

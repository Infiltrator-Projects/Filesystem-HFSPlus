# Filesystem-HFSPlus

## 1. Purpose and format scope

Apple HFS Plus, distinct from classic HFS. This repository owns the format-specific implementation of `HFSPLUS.so` for InfiltratorOS.

## 2. On-disk architecture and invariants

The volume header, allocation file, extents overflow and catalog B-tree define the volume. Interpret Unicode catalog keys and extent mappings according to the actual format, not HFS Pascal-name conventions.

## 3. Common V2 implementation and capabilities

The current hosted descriptor declares probe, mount, stat, directory enumeration, file read, format, extent mapping, and bounded root-file creation and append. Mutation is limited to the native clean non-journalled 4 KiB allocation / 8 KiB two-node catalogue profile; names are ASCII letters, digits, period, underscore or hyphen, and data forks use at most eight inline extents. A full catalogue or fork refuses expansion rather than fabricating success. The freestanding descriptor remains read-only. Durable mutation marks the volume inconsistent before changing records and refuses further operations after a failed commit; repair and recovery are not advertised. The implementation acquires volume identity before destructive formatting. Hosted and freestanding qualification workflows are present. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

## 4. Integrity, mutation, allocation and recovery

A freestanding build is not a production BOOT handoff guarantee. Check profile masks, supported catalog features, resource forks and mutation limitations before claiming compatibility. For destructive operations, require the applicable exclusive writable host, durability contract and format-specific validation. Never silently substitute a kernel or FUSE driver or treat a rejected or unavailable operation as success. Preserve original disk-format semantics; relocation and recovery are separate from ordinary file I/O.

## 5. Build, installation and release

The target installation is `/System/Filesystems/HFSPLUS.so` and `/System/Resources/Documentation/Filesystem-HFSPlus.pdf`. The project release policy is a portable amd64 DEB and a CPU-native source-first RUN, with GitHub's automatic source archive; do not treat this policy statement as verification of the current installer payload. The release workflow is the authoritative executable build and package specification. Historical adapters and test artifacts are not additional production filesystem authorities.

## 6. Qualification and limitations

[Recorded qualification workflow](https://github.com/Infiltrator-Projects/Filesystem-HFSPlus/actions/runs/45520343723) · [Current workflows](https://github.com/Infiltrator-Projects/Filesystem-HFSPlus/actions?query=branch%3Amain). A successful workflow establishes the tested scope only, not complete original-format parity, physical-media crash safety, or production BOOT/kernel integration. Use disposable images for destructive qualification.

## 7. Technical references and provenance

- [Provider implementation](module/hfsplus_module.c)
- [Native design](native/filesystems/hfsplus/DESIGN.md)
- [Migration and source history](MIGRATION_STATUS.md)
- [Original-format reference source](docs/HFSPlus.pdfstreams) and [PDF](docs/HFSPlus.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

The original-format reference and historical migration notes describe different evidence from the current production provider. When descriptions disagree, the current exported descriptor, implementation, executable tests and exact-revision release evidence take precedence.

The [engineering reference](docs/HFSPlus-TECHNICAL-REFERENCE.md) adds field-level original-format guidance. Earlier material is preserved in the PDF.

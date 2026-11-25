# HFS+ / HFSX Design and Ownership

## Current production architecture

The independent `HFSPLUS.so` is the canonical installed filesystem provider. The current hosted descriptor declares probe, mount, stat, directory enumeration, file read, format, extent mapping, and bounded root-file creation and append. Mutation is limited to the native clean non-journalled 4 KiB allocation / 8 KiB two-node catalogue profile; names are ASCII letters, digits, period, underscore or hyphen, and data forks use at most eight inline extents. A full catalogue or fork refuses expansion rather than fabricating success. The freestanding descriptor remains read-only. Durable mutation marks the volume inconsistent before changing records and refuses further operations after a failed commit; repair and recovery are not advertised. The implementation acquires volume identity before destructive formatting. Hosted and freestanding qualification workflows are present. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

The hosted and freestanding profiles are qualified separately. Only the shared object and its technical PDF are installed by the filesystem package; the native RUN downloads its exact source revision. No separate per-format kernel or EFI driver or FUSE substitute is a production delivery path. See the [current README](../../../README.md) and [migration evidence](../../../MIGRATION_STATUS.md) for exact scope and qualification. The inherited design and source-ownership record below is historical where it describes older source layouts, implementation status or adapters; it remains valuable for format semantics and attribution.

## Historical design and source record

> **Working source (23 October 2026).** This filesystem's `linux/` directory contains the actual copied Linux implementation source, preserved with its original attribution and available for direct modification. It is not a replaceable research-only reference tree. The old independently-rewrite-first policy and any old directory-layout/refresh language below are historical, not a restriction on editing this working baseline. Only explicitly qualified drivers appear in `data/native-filesystems.tsv`.


## Classification

HFS+ (Mac OS Extended) and HFSX are conventional local block filesystems.
HFSX is the case-sensitive variant of the same filesystem family; it is not a
second independent filesystem implementation.

Filesystems therefore applies the canonical local-filesystem
architecture:

```text
                 hfsplus/core/
          canonical HFS+/HFSX semantics
               /                \
              /                  \
     hfsplus/linux/         hfsplus/windows/
       thin VFS                thin IFS
       adapter                 adapter
```

## Historical implementation state

HFS+ was originally documented as **imported-source development state**.

There is no project-authored HFS+ canonical engine and no Filesystems
HFS+ native driver claimed by this directory. The source under
`linux/` is the pinned upstream Linux HFS+ implementation copied by
the reference-refresh workflow.

Those files retain their upstream SPDX identifiers, copyrights, filenames and
implementation boundaries. Moving the reference source beneath
`linux/` is provenance/layout hygiene only; it does not turn that
implementation into project-authored code.

The reference tree is not the permanent Filesystems source architecture.

## Historical directory layout

```text
native/filesystems/hfsplus/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux HFS+ source, reference only
```

When an independent rewrite begins, project-authored source is added beside the
reference tree:

```text
native/filesystems/hfsplus/
  DESIGN.md
  core/                  canonical HFS+/HFSX implementation
  linux/                 thin Linux VFS/module adapter
  windows/               thin Windows IFS/WDK adapter when implemented
  userspace/             optional image/inspection adapter over the same core
  linux/       preserved upstream evidence
```

## Canonical-core ownership

The future canonical core owns HFS+-defined and HFSX-defined behaviour that is
independent of the host operating system, including:

- volume-header decoding, validation and feature/state policy;
- allocation bitmap and block-allocation semantics;
- catalog-file B-tree keys, records, traversal and mutation;
- extents-overflow B-tree and fork extent mapping;
- attribute-file B-tree and extended-attribute records;
- file and directory CNID/object identity rules;
- data and resource fork representation;
- Unicode name encoding, normalization/comparison and catalog key ordering;
- HFSX case-sensitive/case-folding policy as defined by the format;
- timestamps, Finder metadata and filesystem-defined flags;
- hard-link and symbolic-link representation;
- wrapper/embedded HFS volume handling where required for HFS+ discovery;
- journal state and recovery semantics only for variants explicitly supported;
- corruption, bounds and structural validation;
- filesystem-defined mutation and crash-consistency rules.

Linux VFS objects, dentries/inodes, page cache, buffer heads, kernel Unicode
helpers, credentials, mount/module lifetime and Linux error translation belong
in the Linux adapter.

Windows IRPs, VCB/FCB/CCB lifetime, Cache Manager integration, native security
presentation and NTSTATUS translation belong in the Windows adapter.

## HFS+ and HFSX rule

HFS+ and HFSX share one canonical implementation.

Case-sensitive HFSX behaviour and any HFSX-specific catalog-key rules must be
explicit negotiated filesystem state in the canonical core. They must not be
implemented as a second host-specific filesystem engine.

Classic HFS remains a separate filesystem under
`native/filesystems/hfs/`; shared ancestry is not permission to merge HFS and
HFS+ implementations.

## File-boundary rule

The upstream reference filenames such as `btree.c`, `catalog.c`,
`extents.c`, `inode.c`, `super.c`, `unicode.c` and `xattr.c` remain
unchanged under `linux/` so their provenance is obvious.

Project-authored HFS+ files must be cut around Filesystems
responsibilities and portable invariants. Copying, renaming, merging,
recommenting or moving an upstream file is never evidence that its
implementation has been rewritten.

## Promotion sequence

HFS+ leaves imported-source development state only through an explicit rewrite:

1. define the supported HFS+/HFSX media variants and feature/journal limits;
2. implement bounded host-neutral volume-header and fork/extents decoding;
3. implement catalog/extents/attribute B-tree traversal with independent
   fixtures;
4. implement Unicode/name comparison rules and HFSX policy in the canonical
   core;
5. qualify read-only traversal against independently produced HFS+/HFSX media
   and malformed/corrupt fixtures;
6. add a thin Linux adapter over the canonical core;
7. add mutation only after allocation, B-tree update, journal/recovery and
   crash-consistency contracts are explicitly defined and destructively
   qualified;
8. add a Windows adapter against the same canonical engine when ready;
9. remove inherited provenance only from implementation bodies that have
   actually been independently replaced.

## Safety and failure behaviour

Unknown or unsupported format features, invalid B-tree nodes, impossible
extent/fork geometry, allocation-bitmap contradictions, invalid CNIDs,
malformed Unicode/catalog keys, corrupt attribute records and unsafe journal
state must fail closed.

Write support must never be inferred merely because the reference Linux driver
contains write paths.

## Copied implementation provenance

The editable `linux/` source contains original implementation code copied from the pinned Linux v6.12.107 source baseline. Original copyright, licence and attribution remain part of that code. Source refresh is not automatic and must never replace Filesystems modifications. Promotion into a managed native module requires its own host-integration and runtime qualification.

## Completion rule

A Filesystems HFS+ implementation is not claimed until a canonical core
and at least one adapter have independent implementation and qualification
evidence. The current reference tree is evidence only.

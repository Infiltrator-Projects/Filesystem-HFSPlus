# HFS+ native source consolidation — 6 November 2026

All 27 HFS+ native source/design files in central Filesystems main commit `a253fb724ce1fd760a361219281effb6abd71367` were imported into this repository, preserving kernel source attribution and the existing HFS+ Defragmenter module sources. HFS+ and HFSX remain supported variants of this single on-disk format engine, not competing providers.

The Linux `hfsplus.ko` build reads `native/filesystems/hfsplus/` locally. Its `standalone/kernel.json` no longer downloads a historical central Filesystems source snapshot. The `.so` build and qualifying tests remain in this repository, without claiming any unverified cross-platform read/write features.

Before deleting the original central source, the Filesystems manager CMake/CI/test/package references must be detached and the independent kernel build and native installer verified.

# HFS+ migration
The native HFS+/HFSX Defrag/Growth Defrag/recovery worker and native parser
are now built into HFSPLUS.so in this repository, through a typed host-service
adapter. Defragmenter must load this module and perform the unchanged native
HFS+ end-to-end tests before its local native engine is removed.

The Test Media HFS+ formatter/population source remains in Defragmenter and
is NOT YET MIGRATED; this commit does not mark the user-requested migration
complete. The host-service ABI is deliberately format-neutral and contains
no HFS+ allocation/parsing or placement logic.

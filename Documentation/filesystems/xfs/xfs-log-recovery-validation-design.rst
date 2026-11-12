.. SPDX-License-Identifier: GPL-2.0
.. _xfs_log_recovery_validation:

===================================
XFS Log Recovery Item Validation
===================================

Problem
=======

Log recovery replays the changes recorded in the journal into the filesystem
metadata after an unclean shutdown. Journal records are CRC protected. Storage
failures such as torn writes and media errors are caught by the record
checksums before any log item in the record is decoded.

The checksums only protect the record contents from changing after they were
written. They do not protect against the contents being wrong in the first
place. A runtime bug in the code that formats log items writes a malformed
structure into a record with a valid checksum. A fuzzer that mangles the log
items on disk and then recalculates the record checksum produces the same
result. Recovery trusts the structure and contents of the log items it decodes
from such a record. Their fields are used to size memory allocations, index
arrays, and select the metadata to read and rewrite. A malformed item can
therefore crash the kernel, overrun memory, or write corrupt metadata into the
filesystem.

Fuzzers and LLM based code analysis are finding these malformed structure bugs
in recovery one at a time. Each is fixed with a point check for the one field
or path that was reported. That approach never closes the class of bug, as the
next unchecked field is simply the next report.

The aim of this design is to remove this class of bug from journal recovery
altogether. Recovery never acts on log item contents that it has not first
validated. Every log item is validated as it is decoded from the journal,
before any of it is used to replay changes. A journal that fails validation is
rejected as corrupt. The mount then fails cleanly rather than crashing or
propagating the corruption into the filesystem.

Design
======

Overview
--------

Recovery decodes the journal in stages. A log record is a sequence of
operation headers, each followed by one fragment of a region. Fragments are
assembled into regions, and regions are assembled into log items. Complete log
items are queued on their transaction, and replayed when the transaction's
commit record is seen. Each stage has a layer of validation attached to it.
Each layer validates only what its stage can see.

Layer 1 runs as each operation header is decoded from the record. An
operation header describes the fragment that follows it. The walk over the
record uses its length to find the next operation header. The length,
alignment, flags, client ID and padding are checked against the values the log
writer produces. This keeps the walk over the record in sync. It also bounds
every fragment before anything is allocated for it. Nothing about the log
items is known at this layer.

Layer 2 runs when the first fragment of a new item arrives. That fragment may
be only 4 bytes long, enough to hold the item type and region count. Nothing
else in the item exists yet. The type selects the item's recovery operations,
and those operations validate the region count. An unknown type or a bad
region count is rejected before any memory is sized from it.

Layer 3 runs each time a region of the item has been fully assembled. A region
can be split across many fragments, so it cannot be checked until its final
fragment has arrived. Each region is checked against the size and layout its
item type requires. Regions arrive in order, so a region can be checked
against the format header in region 0, which has already been validated. A
region cannot be checked against regions that have not arrived yet.

Layer 4 runs once all of the item's regions have been assembled. This is the
first point at which the whole item is available. It checks that the regions
are consistent with each other, and validates the contents of the item's
fields, such as extent ranges, inode core fields and fork contents. Some item
types also convert the item to host format here, so replay only ever sees one
form.

The layers build on each other. A layer only runs once the layers before it
have passed, so it can rely on what they established. Region validation can
trust the region count and the minimum size of the format header. Full item
validation can trust that every region exists and has a valid size. Each layer
therefore only needs to check what has newly become available to it. Region
size is bounded at two points. Operation header validation bounds each
fragment, and region assembly bounds the assembled region.

Validation uses only the contents of the item and the in-core filesystem
geometry. It never reads filesystem metadata. Checks that depend on the
current on-disk state, such as whether a logged change is newer than the
metadata it applies to, remain in replay.

A failure at any layer aborts recovery with -EFSCORRUPTED, and the corrupt
item data is dumped to the log. An item that has not passed all four layers is
never queued for replay. A transaction whose commit record arrives while an
item is still incomplete is rejected. Replay can therefore trust every item it
is given, and the replay code no longer duplicates the validation checks.

The transaction header at the start of each transaction is not a log item, but
it is decoded through the same layers. It has its own item recovery
operations, is validated like any other item, and is then consumed into the
transaction rather than replayed.

Geometry changes during recovery
--------------------------------

Filesystem geometry can change during recovery. A growfs logs the superblock
geometry change ahead of any metadata that uses the new geometry. A shrink
logs the superblock geometry change after the metadata in the removed space
has been updated or invalidated. The in-core geometry changes when recovery
replays the superblock buffer.

Pass 2 recovers the journal one checkpoint at a time. The items in a
checkpoint are decoded and validated as its records are read, and the
checkpoint is replayed when its commit record is reached. Once a checkpoint
has been replayed, any geometry change it contains is in effect. Later
checkpoints are validated against the new geometry. A geometry change is
therefore correctly ordered against the items in the checkpoints that follow
it.

The ordering problem is confined to the checkpoint that contains the geometry
change. All of its items are decoded and validated before the checkpoint is
replayed, so they are validated against the geometry from before the change.
Items logged after a growfs in the same checkpoint can legitimately refer to
inode numbers, disk addresses and extents beyond that geometry. Checkpoints
can also overlap in the journal. The items of a later checkpoint can be
decoded before the commit record of the checkpoint containing the geometry
change is reached. Those items are also validated against the old geometry.
This affects every item type that carries inode numbers, disk addresses or
extents, including the realtime variants.

Each recovery pass needs a different amount of validation, and each runs at a
different point relative to the geometry change:

- Pass 1 walks the whole journal to find buffer cancellation items before
  anything is replayed. It must validate enough of the headers and items to
  walk the journal safely and record the cancellations correctly. How much
  validation that requires is not yet determined.

- Pass 2 readahead decodes disk addresses and issues I/O to them before the
  checkpoint is replayed. It needs some form of geometry validation, but it
  runs before any geometry change in the checkpoint has taken effect.

- Pass 2 replay needs the item to be fully validated. Geometry dependent
  fields must be validated against the geometry in effect at the point the
  item is replayed, which may differ from the geometry at decode time.

How to meet all of these requirements is an open problem. Validation
currently runs in full at decode time in both passes, against the geometry in
effect at that point. Recovery of a geometry change can therefore fail
validation on items that are valid.

The realtime btree inode fork roots work around this for now. Their maximum
height depends on the data and realtime device geometry, which growfs can
change. Recovery cannot know that height until the superblock has been
replayed, and pass 1 never replays it. Full item validation therefore bounds
the root level by the largest height any geometry allows. The root is only
checked against the actual geometry when the inode is loaded after recovery.
This needs to be addressed as part of solving the geometry problem, so that
the root level is validated against the geometry in effect when the item is
replayed.

Layer 1: Operation header decode (checkpoint based)
---------------------------------------------------

A log record carries the operation headers of one or more checkpoints. Each
operation header carries the transaction ID of the checkpoint it belongs to,
so the operation headers of different checkpoints can be interleaved within a
record. Recovery tracks each open checkpoint by its transaction ID. An
operation header with the start flag opens a new checkpoint. An operation
header with the commit flag closes the checkpoint, and the checkpoint is
replayed. Operation headers for a transaction ID with no open checkpoint are
skipped, unless they carry the start flag.

Between the start and commit records, each operation header carries one
fragment of a region. A region that does not fit in the space left in a record
is split across operation headers, and possibly across records. The flags on
each operation header describe where its fragment sits within the region:

=========================================  ====================================
Flags                                      Fragment
=========================================  ====================================
0                                          A whole region
XLOG_CONTINUE_TRANS                        The first fragment of a split region
XLOG_WAS_CONT_TRANS | XLOG_CONTINUE_TRANS  A middle fragment
XLOG_WAS_CONT_TRANS | XLOG_END_TRANS       The final fragment
=========================================  ====================================

Every operation header is validated by ``xlog_recover_validate_ophdr()`` as
it is decoded, before any of its fields are acted on:

- The operation header is 32 bit aligned, and lies wholly within the record.
- The fragment length is 32 bit aligned, and the fragment lies wholly within
  the record.
- The fragment length is no larger than ``XLOG_MAX_REGION_SIZE``, the largest
  region recovery assembles.
- The client ID is ``XFS_TRANSACTION`` or ``XFS_LOG``.
- The padding is zero.
- The flags are one of the combinations the log writer produces. The start,
  commit and unmount flags each stand alone, and the fragment flags must be one
  of the combinations above.

The alignment and length checks keep the walk over the record in sync with
the on-disk layout. A misaligned or oversized length would make every
following operation header in the record garbage. The length bound also
limits the memory any single fragment can make recovery allocate.

The start and commit records only bracket a checkpoint. They carry no data, so
their length must be zero.

The unmount record is written by the log itself rather than by a checkpoint,
and checkpoint processing skips it. It is fully validated here. It must have
the ``XFS_LOG`` client ID and be exactly the size of the unmount format
structure. Its magic number is written in host byte order, and a clean log can
be mounted on a host of either endianness, so both byte orders are accepted.
Its padding must be zero. Some kernels wrote the unmount record with a zero
length. Such a record has no payload to check, so it is accepted with a
warning so those filesystems still mount.

Finding the log tail reads the last record to see if it is an unmount record.
This happens before the log has been checked for torn writes, so the record
may legitimately be garbage. The same validation is run in a quiet mode for
this probe. A record that fails validation is not an error here. The log is
treated as dirty, and recovery then validates the record normally.

After an operation header has been validated, its flags are checked against
the state of its checkpoint. A start record for a checkpoint that is already
open is corrupt. A continuation fragment must have a region under assembly to
append to. A commit record must not arrive while an item is still
incomplete, as every item in a checkpoint is written before its commit record.
An unmount record carries the ``XFS_LOG`` client ID, so it never belongs to a
checkpoint. An unmount record that matches the transaction ID of an open
checkpoint means the checkpoint is corrupt, and it is rejected.

Operation header decode also tells the later layers when a region is complete.
A fragment without the continuation flag completes its region. That is the
only point at which region validation can run on it.

The log writer always ensures a record has room for an operation header and
at least some of its region before starting a new region in it. The one
exception is the transaction header at the start of a checkpoint. The
operation header that opens it can be written with a zero length fragment,
and the header data then follows in the continuation fragment. A zero length
opening fragment is therefore valid only for the first region of a checkpoint.
There is nothing to decode, so the checkpoint is left waiting for its
transaction header. The continuation fragment that follows is decoded as the
first fragment of the transaction header. A zero length fragment that opens
any other region is corrupt.

Layer 2: Region header decode (generic)
---------------------------------------

Region header decode starts a new log item. It runs when the first fragment of
the item's first region arrives, in ``xlog_recover_add_to_trans()``, when there
is no item under assembly in the checkpoint.

Every log item format begins with the same two 16 bit fields: the item type,
followed by the number of regions in the item. Region header decode reads
these two fields and nothing else. The first fragment may be all of the first
region or only the start of it, so nothing beyond these two fields can be
relied on yet. Operation header validation guarantees that the fragment length
is a non-zero multiple of 4 bytes, because a zero length opening fragment
never reaches this point. Both fields are therefore always present.

Starting an item
~~~~~~~~~~~~~~~~

``xlog_recover_add_item()`` starts the item from the two fields:

- The item type is used to look up the item's recovery operations with
  ``xlog_find_item_ops()``. An unknown type is rejected with
  ``-EFSCORRUPTED``.
- The region count is passed to the type's ``validate_nregions()`` method,
  which returns the number of regions to assemble or a negative error. A bad
  count is rejected with ``-EFSCORRUPTED``.
- The item is allocated, with its region array sized from the validated count.
  The recovery operations are attached to the item.

The item becomes the checkpoint's current item, ``r_cur_item``. It is not
added to the checkpoint's item queue until it has been fully assembled and has
passed full item validation. An item that fails validation is therefore never
visible to replay.

Each subsequent fragment either continues the current region or starts the
next region of the current item. A continuation fragment is appended to the
region under assembly. The assembled region is bounded by
``XLOG_MAX_REGION_SIZE`` before the region buffer is grown, so a corrupt
stream of continuations cannot drive an unbounded allocation. A fragment that
starts a region beyond the validated region count is rejected.

Recovery operations
~~~~~~~~~~~~~~~~~~~

Region header decode uses two members of ``struct xlog_recover_item_ops``::

    uint16_t    min_hdr_len;    /* minimum ri_buf[0].iov_len */
    int (*validate_nregions)(struct xlog *log, uint16_t nregions);

The region count is validated by a method rather than a static minimum and
maximum. The method is given the raw count read from the journal, and returns
the number of regions to assemble. Not every recovered type describes its
regions with the common type and count format header, so each type must be
able to extract and validate its own count. A type that supplies no method
falls back to the generic ``xlog_recover_nregions()`` helper, which accepts any
count from 1 to ``XLOG_MAX_REGIONS_IN_ITEM``.

``min_hdr_len`` is the minimum valid length of the item's format header in
region 0. It cannot be checked when the item is started, because the first
fragment may only be the start of region 0. It is checked once region 0 is
complete, before region validation runs on it.

The region count and minimum format header length for each type are:

===========  =================================  ===============================
Item Type    Regions                            min_hdr_len
===========  =================================  ===============================
TRANS_HDR    1                                  sizeof(xfs_trans_header)
BUF          1 .. ``XLOG_MAX_REGIONS_IN_ITEM``  offsetof(xfs_buf_log_format,
                                                blf_data_map)
INODE        2 .. 4                             sizeof(xfs_inode_log_format_32)
DQUOT        2                                  sizeof(xfs_dq_logformat)
EFI          1                                  sizeof(xfs_efi_log_format)
EFD          1                                  sizeof(xfs_efd_log_format)
RUI          1                                  sizeof(xfs_rui_log_format)
RUD          1                                  sizeof(xfs_rud_log_format)
CUI          1                                  sizeof(xfs_cui_log_format)
CUD          1                                  sizeof(xfs_cud_log_format)
BUI          1                                  sizeof(xfs_bui_log_format)
BUD          1                                  sizeof(xfs_bud_log_format)
ATTRI        2 .. 5                             sizeof(xfs_attri_log_format)
ATTRD        1                                  sizeof(xfs_attrd_log_format)
XMI          1                                  sizeof(xfs_xmi_log_format)
XMD          1                                  sizeof(xfs_xmd_log_format)
ICREATE      1                                  sizeof(xfs_icreate_log)
QUOTAOFF     1                                  sizeof(xfs_qoff_logformat)
===========  =================================  ===============================

The realtime variants of the intent and done items are the same as their
non-realtime counterparts.

Two types have a format header whose size varies. The BUF format header
contains an inline dirty bitmap of variable size, so ``min_hdr_len`` covers
only its fixed portion. Region validation checks the full header size
including the bitmap. The INODE format header is logged in either the host
format or a smaller 32 bit format, so ``min_hdr_len`` is the size of the
smaller one. Region validation checks that it is exactly one of the two sizes.

Transaction header as a validated type
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The transaction header (``struct xfs_trans_header``) is the first region of
every checkpoint. It is not a log item and is not replayed. It is still
decoded through the same layers as the log items, so it gets the same
validation. It has its own ``xlog_recover_item_ops`` entry:

- ``min_hdr_len`` is ``sizeof(struct xfs_trans_header)``.
- ``validate_nregions`` checks the magic number and returns a region count of
  1, as described below.
- ``validate_region`` checks that the region is exactly
  ``sizeof(struct xfs_trans_header)`` long, and checks the magic number.
- ``complete`` copies the header into ``trans->r_theader`` and returns
  ``XLOG_RECOVER_ITEM_CONSUMED``, so the item is freed rather than queued for
  replay.

The header does not start with the type and count fields of a log item
format. Its first 32 bits are ``th_magic``. When region header decode reads
the first two 16 bit words as the type and region count, they are the two
halves of the magic number. The type half selects the ops entry.
``validate_nregions`` checks that the count half holds the other half of the
magic, and returns 1.

The XFS log is written in host byte order, so which half of the magic is read
as the type depends on the endianness of the host that wrote the log. There is
an ops entry for each byte order, with ``item_type`` of ``0x414e`` and
``0x5452``. Each checks for the complementary half. On any given host only one
of them is ever matched. Neither type code collides with an ``XFS_LI_*`` value.

A zero length opening fragment is valid only for the transaction header, as
described for operation header validation. The continuation that follows it
is decoded as the header's first fragment, and the header is then assembled
and validated like any other single region item.

Layer 3: Per-region validation (type-specific)
----------------------------------------------

Region validation checks each region of an item as soon as it has been fully
assembled. It ensures that every region is the size and shape its item type
requires before anything indexes into it. Without it, a region that is shorter
than the structure it holds would be read past its end. A region that is
longer than its structure allows would hide trailing garbage. Rejecting a bad
region as soon as it is complete also stops recovery from assembling the rest
of a corrupt item.

Region validation has two constraints. A region can only be checked once it
is complete, which may be many fragments after it started. And regions
complete in order, so a region can only be checked against the regions before
it. Any check that needs a later region has to wait for full item validation.

The format header
~~~~~~~~~~~~~~~~~

Region 0 of every item holds the item's log format structure, its format
header. The format header describes how the rest of the item is laid out. It
can record which optional regions are present, the size of the variable length
regions, and the number of entries in an array within a region. It is the
first region to complete, and every later region is interpreted through it.

The format header is therefore validated before any other region. Its length
is checked against ``min_hdr_len``, and then against the exact sizes its type
allows. The format header fields that describe the item's region structure are
checked to the extent they can be from region 0 alone. Later regions can then
be checked against a format header that is known to be well formed.

Some format header fields describe regions whose layout also depends on
another region. The inode fork regions are an example. Their size depends on
both the format header and the fork formats and offsets in the inode core. Such
fields can only be checked once every region they relate to is present, so
they are left to full item validation.

A format header can be logged in more than one layout for the same type. The
log is written in host byte order, but some format structures differ in size
and alignment between 32 and 64 bit hosts. Region validation accepts each
layout the format header may legitimately have been logged in. Conversion to
a single host layout happens during full item validation.

Region completion and continuations
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A region is validated as soon as it has been fully rebuilt, and never before.
A region that arrives whole in a single fragment is complete as soon as it is
added to the item. A region split across fragments is complete only once its
final fragment has been appended. Each continuation fragment is appended to
the region under assembly. The assembled size must stay within
``XLOG_MAX_REGION_SIZE`` as each fragment is added, so a corrupt stream of
continuations cannot drive an unbounded allocation.

Region 0 must be at least ``min_hdr_len`` long before its contents are
validated. Each completed region is then validated by its type. Once the last
region of the item has been validated, the item passes to full item
validation. A region that fails validation is dumped to the log, and recovery
is aborted with ``-EFSCORRUPTED``.

The validation method
~~~~~~~~~~~~~~~~~~~~~

Region validation adds one method to ``struct xlog_recover_item_ops``::

    int (*validate_region)(struct xlog *log,
                           struct xlog_recover_item *item,
                           int region_index);

It is called with the index of the region that has just completed, and
returns 0 or a negative error. Every region before ``region_index`` has
already passed validation, so the method can rely on them.

Item region validation
~~~~~~~~~~~~~~~~~~~~~~

Transaction header
    - Length is exactly the size of the header.
    - The magic number is valid.

EFI, RUI, CUI and their realtime variants
    - The extent count is non-zero.
    - The extent count is bounded so the size calculation cannot overflow.
    - Length matches the format header plus that many extents.
    - EFI extents may use either the packed 12 byte layout written by i386
      hosts or the padded 16 byte layout written by all other hosts. RUI and
      CUI extents have a single layout.

BUI
    - The extent count is exactly one.
    - Length matches the format header plus one extent.

EFD and its realtime variant
    - The extent count is non-zero.
    - The extent count is bounded so the size calculation cannot overflow.
    - Length matches the format header plus that many extents, in either of
      the two EFI extent layouts.

RUD, CUD, BUD, XMD and ATTRD
    - Length is exactly the size of the format structure.
    - The padding is zero.

XMI and ICREATE
    - Length is exactly the size of the format structure.

QUOTAOFF
    - Length is 160 or 88 bytes, the 64 bit and 32 bit sizes of the in-core
      quota off log item that v5.14 kernels logged. Only the leading format
      structure was written.
    - The flags are a non-zero set of quota accounting flags.

DQUOT
    - Region 0 is exactly the size of the dquot format header.
    - Region 1 is exactly the size of an on-disk dquot.

ATTRI
    - Region 0 is exactly the size of the format header.
    - Region 1 is a name, no longer than the largest attribute name.
    - Regions 2 to 4 are names or values, no longer than the largest attribute
      value.

BUF
    - The dirty bitmap size is no larger than the largest bitmap.
    - The format header, including the bitmap, fits within region 0.
    - Each data region is non-zero in length.
    - Each data region is a multiple of the logging chunk size.

INODE
    - Region 0 is exactly the size of either the host or the 32 bit format
      header layout.
    - Region 1 is exactly the log dinode core size of the running filesystem.
    - The data and attribute fork regions are validated with the whole item,
      as their presence and size depend on the format header and the inode
      core.

Layer 4: Full item validation (type-specific, cross-region)
-----------------------------------------------------------

Full item validation runs once every region of an item has been assembled and
has passed region validation. It is the last check before the item is queued
on its checkpoint for replay. This is the first point at which the whole item
is available. Relationships between regions, and between the format header
and the regions it describes, can only be checked here.

Full item validation can rely on everything the earlier layers established.
The item has the region count its type allows. Every region is complete, and
is the right size for what it holds. The format header is well formed. Full
item validation therefore checks only what needs more than one region, or
needs the region contents to be interpreted:

- The format header agrees with the regions that are actually present.
- The fields of the item hold valid values, such as known flags and operation
  types, valid inode numbers, and extents that lie within the filesystem.
- Where a region carries a recognisable structure, that structure is
  consistent with the rest of the item.

Some item types also convert their format header or contents to the host
layout here. Replay then only ever sees one layout, and contains no
conversion or layout specific code.

Constraints
~~~~~~~~~~~

Full item validation cannot verify the complete metadata structures an item
changes. A log item records changes to a structure, not necessarily the whole
structure. A buffer item logs only the dirty ranges of a buffer, so its data
regions hold fragments of a metadata block. An inode item can log the core
without the forks. A structure may also be changed by several items in
different checkpoints. Its full state only exists once all of them have been
replayed into it.

Complete structures are therefore verified when the recovered metadata is
written back. Each recovered buffer is written with its metadata verifier
attached, so the write verifier checks the whole block after replay. A
recovered inode is verified after its logged state has been copied into the
inode buffer. Checks that need the current on-disk metadata are also made
during replay. For example, the buffer type recorded in the log item must
match the metadata found in the buffer being replayed into.

Full item validation never reads filesystem metadata. It uses only the
contents of the item and the in-core filesystem geometry. Validation of
geometry dependent fields is subject to the limitations described in the
section on geometry changes during recovery.

The validation methods
~~~~~~~~~~~~~~~~~~~~~~

Full item validation adds two methods to ``struct xlog_recover_item_ops``::

    int (*validate_item)(struct xlog *log,
                         struct xlog_recover_item *item);
    int (*complete)(struct xlog *log, struct xlog_recover *trans,
                    struct xlog_recover_item *item);

``validate_item`` validates the whole item, and may convert it to the host
layout. It returns 0 or a negative error. ``complete`` allows a type to
consume the item rather than have it queued for replay. Only the transaction
header uses it. A failure is dumped to the log, and recovery is aborted with
``-EFSCORRUPTED``.

Item validation
~~~~~~~~~~~~~~~

EFI, EFD and their realtime variants
    - The extents are converted to the host layout.
    - Each extent lies within the data device, or within the realtime device
      for the realtime variants.

RUI and its realtime variant
    - Each mapping has a known operation type and valid flags.
    - Each mapping's owner and file offset are valid.
    - Each mapping's extent lies within its device.

CUI and its realtime variant
    - Each extent has a known operation type.
    - Each extent lies within its device.

BUI
    - The mapping has a known operation type and valid flags.
    - The inode number and file offset are valid.
    - The extent lies within its device.

XMI
    - The padding is zero and the flags are known.
    - Both inode numbers are valid.
    - Both file ranges and the file sizes are valid.

DQUOT
    - The buffer length is exactly one dquot cluster.
    - The dquot's offset within the buffer matches its dquot ID.
    - The dquot fits within the buffer.
    - The buffer address is a valid filesystem block.
    - The logged dquot passes the dquot verifier.

ATTRI
    - The operation, flags and name and value lengths in the format header are
      valid.
    - The region count is exactly what the operation requires.
    - Each name and value region matches the length recorded for it in the
      format header.
    - Each name is a valid attribute name.

BUF
    - The item size in the format header matches the region count.
    - The buffer length is non-zero and no larger than ``XLOG_MAX_REGION_SIZE``.
    - The buffer lies wholly within the data device.
    - A cancelled buffer logs only its format header.
    - Any other buffer logs at least one data region.
    - A type flag, if set, matches the buffer type. The type flags select
      special replay handling, so a buffer logged for full replay carries only
      its type, even if it holds inodes or dquots.
    - Only a V4 filesystem may log an untyped buffer, with at most one type
      flag set.
    - Each run of dirty bits maps to exactly one data region of the same
      length, within the buffer.
    - The total dirty data is no larger than the buffer.
    - A buffer with a dquot type flag logs one data region covering the whole
      buffer, and every dquot in it passes the dquot verifier.

INODE
    - The format header is converted to the host layout.
    - The inode number is valid.
    - The inode core is logged.
    - The item size in the format header matches the region count.
    - The region count matches the fork regions the format header flags say
      are present.
    - The cluster buffer length matches the inode cluster size.
    - The cluster buffer address and offset match the inode number, where the
      inode geometry allows them to be calculated without a btree lookup.
    - The log dinode core passes a host order counterpart of the on-disk
      inode verifier.
    - Each fork format is valid for the inode mode.
    - Each logged fork's flag matches its fork format.
    - Each fork region's length matches the fork size in the format header.
    - A local or extents format fork fits within the space the fork offset
      gives it.
    - A local data fork is a valid shortform directory or symlink target, and
      a local attribute fork is a valid shortform attribute fork.
    - Every record in an extents format fork is valid.
    - A btree format fork root has a valid header and level, a record count
      that matches its size, and valid keys and pointers.
    - A metadata btree fork root has a valid header, a level no deeper than
      any geometry allows, a record count that matches its size, and valid
      node pointers. Leaf records are not checked, because the realtime
      groups they refer to are not set up until after recovery. Tighter
      validation of the root level depends on solving the problem described
      in the section on geometry changes during recovery.

ICREATE, QUOTAOFF, RUD, CUD, BUD, XMD and ATTRD are fully validated by region
validation, and have no full item validation.

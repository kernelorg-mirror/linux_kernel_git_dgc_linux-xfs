.. SPDX-License-Identifier: GPL-2.0
.. _xfs_log_recovery_validation:

===================================
XFS Log Recovery Item Validation
===================================

Problem
=======

When recovering the journal, ``xlog_recover_add_to_trans()`` decodes ophdr
regions to rebuild log items from the journal data. The first 4 bytes of
each new item's first region are used to determine the item type (2 bytes)
and region count (2 bytes). These values drive memory allocation, region
accumulation, and later the ``commit_pass1``/``commit_pass2`` handlers cast
the accumulated region data to type-specific format structures and use fields
from them to drive buffer reads, inode updates, and intent replay.

The transaction header (``struct xfs_trans_header``) is also decoded inline
in ``add_to_trans`` with its own bespoke validation (magic number check, max
length check). It does not handle the zero-length first fragment case
that occurs when the iclog has exactly enough space for the start record
ophdr plus one more ophdr but no data — in that case ``add_to_trans``
silently returns at the ``len == 0`` check before ever reaching the header
parsing code.

The problem is that this data comes from the journal and cannot be fully
trusted. Corruption — whether from torn writes, hardware errors, or
software bugs — can produce invalid type codes, wrong region counts,
truncated regions, or internally inconsistent format structures. The
current code has minimal validation:

- ``ilf_size`` is checked against 0 and ``XLOG_MAX_REGIONS_IN_ITEM``
- ``oh_len`` is checked against the log record boundary
- The transaction header checks magic and max length but not ``len == 0``
- Some commit handlers check individual field sizes

However, there is no systematic validation, and many commit handlers cast
``ri_buf[N].iov_base`` to format structures without checking
``ri_cnt >= N+1`` or ``iov_len >= sizeof(format_struct)``. This risks
crashes, buffer overruns, and use of garbage data to drive disk I/O during
recovery.

Design
======

Add three layers of validation, each catching problems at the earliest
possible point.

Layer 1: Region header decode (generic)
---------------------------------------

When: In ``xlog_recover_add_to_trans()`` when ``ri_total == 0`` (first
region of a new item).

Currently the code reads ``ilf_size`` from the region data to set
``ri_total``, and the item type is not looked up until much later in
``xlog_recover_reorder_trans()``. The transaction header is handled as a
special case with inline validation. Move all first-region validation
earlier and make it uniform:

a) Validate ``len >= 4`` (minimum to read type + size fields). All log
   regions are 32-bit aligned, so the minimum fragment of any region
   is 4 bytes. A first fragment smaller than 4 bytes is corruption.
   Note: ``len == 0`` is valid for the transaction header when the iclog
   has exactly enough space for the start ophdr plus one more ophdr
   but no data — this must be handled as a continuation (see below).
b) Read the item type from the first 2 bytes
c) Look up the item ops via ``xlog_find_item_ops()``
d) If the type is unknown, reject immediately with ``-EFSCORRUPTED``
e) Store ``item->ri_ops`` at this point (currently done in
   ``reorder_trans``)
f) Validate the region count via ``ops->validate_nregions()`` (or the
   generic ``xlog_recover_nregions()`` helper if the type does not supply
   one), and use its return value as the number of regions to assemble
g) Validate ``len >= ops->min_hdr_len`` (the minimum format header size)

Transaction header as a validated type
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The transaction header (``struct xfs_trans_header``) is currently decoded
inline in ``add_to_trans`` with bespoke magic number and length checks, and
its continuation is handled separately in ``add_to_cont_trans``. Instead,
treat it as a regular validated type within the same framework by giving it
an ``xlog_recover_item_ops`` entry:

- ``min_hdr_len = sizeof(struct xfs_trans_header)``
- ``validate_nregions`` always returns a region count of 1 (see the
  endianness note below)
- ``validate_region`` checks ``iov_len == sizeof(struct xfs_trans_header)``
  and the magic number
- ``complete`` copies the decoded header to ``trans->r_theader`` and
  returns ``XLOG_RECOVER_ITEM_CONSUMED`` so it is freed rather than queued
  for replay — the header is not a log item and has no
  ``commit_pass1``/``commit_pass2`` handler

The header has no format header of the ``type/size`` shape: its first 32
bits are ``th_magic``. When ``add_to_trans`` reads the first two 16 bit
words as the type and region count, they land on the two halves of the
magic. The type-half is used as the ops key, and ``validate_nregions``
checks that the count-half holds the other half of the magic before
returning 1 (the header always has a single region).

Because the XFS log is written in host byte order, which half of the magic
is read as the type depends on the endianness of the host that wrote the
log. Register two ops entries, one for each order (``item_type`` of
``0x414e``/``0x5452``, with the complementary half checked by
``validate_nregions``); on any given host only one is ever matched, and
neither type code collides with an ``XFS_LI_*`` value.

Routing the header through the generic assembly removes the special-case
parsing in ``add_to_trans`` and ``add_to_cont_trans``, and lets
``xlog_recover_init_new_trans()``, the ``r_hdr_decoded`` flag and the
header dispatch branch in ``xlog_recovery_process_trans()`` be deleted — a
header op record now falls through to the generic region assembly like any
other. The zero-length first fragment case is handled uniformly: it arrives
as a continuation (``oh_len == 0``, ``XLOG_CONTINUE_TRANS`` set), and the
continuation infrastructure assembles the complete region before
``validate_region`` checks it.

New members in ``struct xlog_recover_item_ops``::

    uint16_t    min_hdr_len;    /* minimum ri_buf[0].iov_len */
    int (*validate_nregions)(struct xlog *log, uint16_t nregions);

``min_hdr_len`` is a compile-time constant per item type. The region count
is validated by the ``validate_nregions()`` method rather than a static
min/max pair: it is given the raw count read from the journal and returns
the number of regions to assemble, or a negative error. This is a method,
not a bound, because not all recovered types describe their regions with the
same "type/size" format header that log items use, so each such type must
extract and validate its own count. Types that do use the common layout
supply no method and fall back to the generic ``xlog_recover_nregions()``
helper, which accepts any ``0 < count <= XLOG_MAX_REGIONS_IN_ITEM``.

Examples of the region count each type should accept and the minimum header
length:

===========  =====================  ==============================
Item Type    regions                min_hdr_len
===========  =====================  ==============================
TRANS_HDR    1 (magic-derived)      sizeof(xfs_trans_header)
BUF          2 .. XLOG_MAX..        sizeof(xfs_buf_log_format)
INODE        2 .. 4                 sizeof(xfs_inode_log_format)
DQUOT        2                      sizeof(xfs_dq_logformat)
EFI          1                      sizeof(xfs_efi_log_format)
EFD          1                      sizeof(xfs_efd_log_format)
RUI          1                      sizeof(xfs_rui_log_format)
RUD          1                      sizeof(xfs_rud_log_format)
CUI          1                      sizeof(xfs_cui_log_format)
CUD          1                      sizeof(xfs_cud_log_format)
BUI          1                      sizeof(xfs_bui_log_format)
BUD          1                      sizeof(xfs_bud_log_format)
ATTRI        2 .. 5                 sizeof(xfs_attri_log_format)
ATTRD        1                      sizeof(xfs_attrd_log_format)
XMI          1                      sizeof(xfs_xmi_log_format)
XMD          1                      sizeof(xfs_xmd_log_format)
ICREATE      1                      sizeof(xfs_icreate_log)
QUOTAOFF     1                      sizeof(xfs_qoff_logformat)
===========  =====================  ==============================

(RT variants same as their non-RT counterparts.)

Handling first region split across continuations
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

All log regions are 32-bit aligned, so the minimum first fragment size
is 4 bytes — enough to read both the type and size fields. If ``len < 4``
on a non-continuation ophdr, this is unconditionally corrupt.

The exception is the transaction header: when the iclog has exactly
enough space for the ``XLOG_START_TRANS`` ophdr plus one more ophdr but no
data, ``oh_len == 0`` for the transaction header ophdr and the data arrives
entirely via continuation. This is a valid zero-length first fragment
that the generic code must handle:

- When ``len == 0`` and this is the first region of a new item (``r_itemq``
  is empty or the current item is full), we cannot read the type.
  Since only the transaction header can legitimately have ``len == 0``
  at this point, and it will always arrive as the first item in the
  transaction, we can identify this case by checking that the
  transaction's item list is empty (i.e. we haven't seen the
  transaction header yet). Set ``ri_in_continuation`` and defer all
  validation to when the continuation completes the region.

- For all other items (non-empty ``r_itemq``), ``len == 0`` on a new region
  is corruption.

When the first region is split but ``len >= 4`` (the common continuation
case for log items), the ops lookup and ``ilf_size`` validation can be done
immediately. The ``min_hdr_len`` check is deferred until the continuation
completes the region, at which point ``validate_region`` runs on the
complete ``ri_buf[0]``.

The continuation path (``add_to_cont_trans``) uses ``kvrealloc`` to grow
the region buffer. Since ``ri_ops`` is set (or will be set when the
continuation completes for the deferred transaction header case), the
accumulated size (``old_len + len``) can be bounds-checked against a
type-specific maximum for the current region index before doing the
realloc.

The BUF item is special: it always has at least 2 regions (the format
header and at least one data region), but the format header's size is
variable because it contains an inline bitmap. The ``min_hdr_len`` should
be the fixed portion of ``xfs_buf_log_format`` (without the bitmap), and
per-region validation (layer 2) checks the full header size including
the bitmap.

Layer 2: Per-region validation (type-specific)
----------------------------------------------

When: In ``xlog_recover_add_to_trans()`` after each region is added to the
item (after ``ri_cnt`` is incremented), and in
``xlog_recover_add_to_cont_trans()`` after a continuation region is
appended.

New callback in ``struct xlog_recover_item_ops``::

    int (*validate_region)(struct xlog *log,
                           struct xlog_recover_item *item,
                           int region_index);

Called after the region at ``region_index`` has been fully assembled.

For regions that arrive complete in a single ophdr, ``validate_region`` is
called from ``xlog_recover_add_to_trans()`` immediately after the region
is added.

For regions that are split across op records (continuations), the region
is built incrementally by ``xlog_recover_add_to_cont_trans()`` which
reallocates and appends data. Two levels of validation apply:

a) Before the realloc in ``add_to_cont_trans``: bounds check the
   accumulated size (``old_len + len``) against the type-specific maximum
   for the current region index. This uses the ``ops->max_region_size``
   field or a simple per-type upper bound to prevent unbounded memory
   allocation from a corrupt continuation stream.

b) After the continuation region is complete: call ``validate_region`` to
   do the full type-specific validation. A continuation is complete
   when the next non-continuation ophdr arrives (either a new region
   via ``add_to_trans`` or a commit via ``xlog_recover_commit_trans``).

   To track this, add a boolean ``ri_in_continuation`` flag to
   ``struct xlog_recover_item``. Set it in ``add_to_cont_trans`` when data
   is appended. When ``add_to_trans`` is next called and the tail item has
   ``ri_in_continuation`` set, the previous region was completed by the
   continuation — call ``validate_region`` for it (at ``ri_cnt - 1``) and
   clear the flag before proceeding with the new region. Similarly,
   when ``xlog_recover_commit_trans`` is called, check the tail item for
   ``ri_in_continuation`` and validate the final region if needed.

Each item type implements ``validate_region`` to check size bounds:

INODE
    | region 0: ``iov_len == sizeof(xfs_inode_log_format)`` or
      ``iov_len == sizeof(xfs_inode_log_format_32)``
    | region 1: ``iov_len >= sizeof(xfs_dinode)``,
      ``iov_len <= xfs_log_dinode_size(mp)``
    | region 2: ``iov_len >= 0`` (data fork, variable size),
      ``iov_len <= XFS_DFORK_DSIZE(...)`` (need inode core to check)
    | region 3: ``iov_len >= 0`` (attr fork, variable size),
      ``iov_len <= XFS_DFORK_ASIZE(...)``

BUF
    | region 0: ``iov_len >= sizeof(xfs_buf_log_format)`` base size,
      ``blf_map_size`` is consistent with ``iov_len``
    | region 1+: data regions, ``iov_len > 0``,
      ``iov_len <= blf_len * BBSIZE`` (can't exceed buffer size),
      ``iov_len % XFS_BLF_CHUNK == 0``

DQUOT
    | region 0: ``iov_len == sizeof(xfs_dq_logformat)``
    | region 1: ``iov_len >= sizeof(xfs_disk_dquot)``

EFI/RUI/CUI/BUI (single-region intent items)
    | region 0: ``iov_len ==`` calculated size based on ``nextents`` field,
      ``nextents >= 1``, ``nextents <=`` type-specific maximum

EFD/RUD/CUD/BUD/ATTRD/XMD (single-region done items)
    | region 0: ``iov_len == sizeof(format_struct)``

ATTRI
    | region 0: ``iov_len == sizeof(xfs_attri_log_format)``
    | region 1+: name/value regions, sizes bounded by
      ``XATTR_NAME_MAX``, ``XATTR_SIZE_MAX``

XMI
    | region 0: ``iov_len == sizeof(xfs_xmi_log_format)``

ICREATE
    | region 0: ``iov_len == sizeof(xfs_icreate_log)``

QUOTAOFF
    | region 0: ``iov_len == sizeof(xfs_qoff_logformat)``

Layer 3: Full item validation (type-specific, cross-region)
-----------------------------------------------------------

When: After the item is fully assembled (``ri_cnt == ri_total``). This can
be checked in ``xlog_recover_add_to_trans()`` right after incrementing
``ri_cnt``, or in ``xlog_recover_reorder_trans()`` before the item is
sorted into the replay lists. The latter is simpler as it's a single call
site, but the former catches problems before the item is added to the
transaction's item list.

Preferred: validate in ``xlog_recover_reorder_trans()`` after the ops
lookup (which will now be a simple ``ri_ops`` dereference since we set it in
layer 1). This keeps the validation in one place and runs after all
continuations have been resolved.

New callback in ``struct xlog_recover_item_ops``::

    int (*validate_item)(struct xlog *log,
                         struct xlog_recover_item *item);

Each item type implements ``validate_item`` to do cross-region structural
checks:

INODE
    - Verify ``ri_cnt`` matches the fields set in ``ilf_fields`` (e.g. if
      ``XFS_ILOG_DDATA`` is set, ``ri_buf[2]`` must exist and have sane
      size)
    - Verify dinode core fields in ``ri_buf[1]`` are self-consistent:
      fork format vs fork size vs region length
    - Verify ``ilf_ino`` is within valid range

BUF
    - Verify ``blf_blkno + blf_len`` doesn't exceed filesystem size
    - Verify the number of data regions matches the bitmap in the
      format header
    - Verify ``blf_flags`` contains only valid flag bits

DQUOT
    - Verify ``dq_id``, ``dq_type`` are valid
    - Run ``xfs_dquot_verify()`` on ``ri_buf[1]`` data

EFI
    - Verify ``efi_nextents`` matches the region size
    - Verify each extent's startblock/blockcount are within fs bounds

ATTRI
    - Verify ``alfi_op_flags`` matches a known operation
    - Verify ``ri_cnt`` matches the expected region count for the operation
    - Verify name/value region sizes match
      ``alfi_name_len``/``alfi_value_len``

Other intent/done items
    similar field-level validation

ICREATE
    - Already has validation in
      ``xlog_recover_icreate_commit_pass2()``, move the checks to
      ``validate_item``

QUOTAOFF
    - Verify ``qf_flags`` contains only valid quota flags

Additional fixes in the generic code
------------------------------------

Beyond the three validation layers, fix these issues in the generic
region assembly code:

1. ``xlog_recover_add_to_cont_trans()``:

   - Check ``ri_cnt > 0`` before accessing ``ri_buf[ri_cnt-1]``
   - Check ``item->ri_buf != NULL`` before accessing it
   - Bounds-check ``(old_len + len)`` against type-specific max region
     size before calling ``kvrealloc``

2. ``xlog_recover_add_to_trans()``:

   - Check ``len >= 4`` before reading the type and size fields (except
     for the zero-length transaction header continuation case)
   - After looking up ops, validate the region count via
     ``ops->validate_nregions()`` (or the generic helper) and use its
     return value for the ``kzalloc_objs`` allocation

3. ``xlog_recover_process_data()``:

   - Validate ``oh_len`` is 4-byte aligned (all regions must be 32-bit
     aligned per the existing comment)

4. ``ITEM_TYPE`` macro:

   - Add a check in ``xlog_recover_reorder_trans()`` that
     ``ri_buf[0].iov_len >= 4`` before accessing the type code (this is
     now redundant with layer 1 but is a safety net)

Implementation Plan
===================

The infrastructure in ``struct xlog_recover_item_ops`` (the ``min_hdr_len``
field and the ``validate_nregions``, ``validate_region``, ``validate_item``
and ``complete`` callbacks) is introduced first, along with the generic
region and item completion path that calls them, before the transaction
header ops entry that uses them. Later patches populate those callbacks for
the remaining item types. Each patch builds cleanly and is independently
testable.

Phase 1: Generic infrastructure, early ops lookup, transaction header
---------------------------------------------------------------------

Patch 1: Add validation infrastructure to the ops struct
    - Add the ``min_hdr_len`` field and the ``validate_nregions``,
      ``validate_region``, ``validate_item`` and ``complete`` callbacks to
      ``struct xlog_recover_item_ops`` (plus the ``XLOG_RECOVER_ITEM_CONSUMED``
      return value for ``complete``)
    - Fold the start of a new item into ``xlog_recover_add_item()``, which
      takes the type and region count, looks up the ops, rejects unknown
      types, validates the region count (via ``validate_nregions`` or the
      generic ``xlog_recover_nregions()`` helper) and returns the item or an
      ERR_PTR(); remove the ops lookup from ``reorder_trans``
    - Add the generic region/item completion path
      (``xlog_recover_complete_region()``/``xlog_recover_complete_item()``)
      that runs ``min_hdr_len``, ``validate_region``, ``validate_item`` and
      the ``complete`` hook, driven from both region assembly functions
    - No item type supplies the new field or callbacks yet, so the generic
      helper is always used and there is no behaviour change beyond the
      unknown-type rejection moving earlier

Patch 2: Treat the transaction header as a validated region type
    - Add two ops entries for the transaction header, keyed on the two
      halves of ``XFS_TRANS_HEADER_MAGIC`` (one per host byte order), each
      with ``min_hdr_len = sizeof(struct xfs_trans_header)``, a
      ``validate_nregions`` that checks the other half of the magic and
      returns 1, a ``validate_region`` that checks ``iov_len`` and the magic
      number, and a ``complete`` that copies the header to
      ``trans->r_theader`` and returns ``XLOG_RECOVER_ITEM_CONSUMED``
    - Remove the bespoke transaction header parsing from ``add_to_trans``
      and ``add_to_cont_trans``; route through the generic region assembly
    - Delete ``xlog_recover_init_new_trans()``, the ``r_hdr_decoded`` flag
      and the header dispatch branch in ``xlog_recovery_process_trans()``
    - The zero-length first fragment case is handled by the continuation
      path (see the note below on ``add_to_cont_trans`` needing a
      ``r_cur_item != NULL`` guard, added in Patch 5)

Patch 3: Move item ops lookup to add_to_trans (first region decode)
    - Look up and store ``ri_ops`` when ``ri_total == 0``
    - Validate ``len >= 4`` before reading type/size
    - Reject unknown item types immediately
    - Remove the ops lookup from ``xlog_recover_reorder_trans()`` (it
      becomes a simple NULL check / assertion)

Patch 4: Populate validate_nregions/min_hdr_len for all item types
    - The ``min_hdr_len`` field and ``validate_nregions`` callback were added
      to ``struct xlog_recover_item_ops`` in Patch 1; populate them for all
      remaining item types (each ``validate_nregions`` enforces the type's
      region-count range and returns the count)
    - The generic ``min_hdr_len`` check already runs at region-completion
      time from Patch 1

Patch 5: Fix generic safety issues
    - ``add_to_cont_trans``: check there is an item under assembly
      (``trans->r_cur_item != NULL``) before dereferencing it. A corrupt
      log whose first op record is a continuation fragment
      (``XLOG_WAS_CONT_TRANS`` with no preceding initial fragment) reaches
      ``add_to_cont_trans`` with no current item; reject it rather than
      dereferencing NULL. (Now that the transaction header is decoded as an
      item in Patch 2, this also covers the header's leading-continuation
      case, which the old ``xlog_recover_init_new_trans()`` handled itself.)
    - ``add_to_cont_trans``: bounds-check accumulated region size before
      ``kvrealloc``
    - ``process_data``: validate ``oh_len`` alignment

Patch 6: Add ri_in_continuation tracking
    - Add ``ri_in_continuation`` flag to ``struct xlog_recover_item``
    - Set in ``add_to_cont_trans`` when data is appended
    - Check and clear in ``add_to_trans`` when a new region starts
      (previous continuation region is now complete)
    - Check in ``xlog_recover_commit_trans`` for the final region case

Phase 2: Per-region validation
------------------------------

Patch 7: Wire up generic validate_region call sites
    - The ``validate_region`` callback was added to the ops struct in
      Patch 1; wire up the generic call sites for all item types
    - Call it from ``add_to_trans`` after each complete region is added
    - Call it from ``add_to_trans`` when ``ri_in_continuation`` is cleared
      (just-completed continuation region)
    - Call it from ``xlog_recover_commit_trans`` if the final region was
      a continuation

Patches 8-N: Implement validate_region for each item type
    - Start with the transaction header (magic, exact size)
    - Then simple fixed-size types (done items, ICREATE, QUOTAOFF)
    - Then single-region variable types (EFI, RUI, CUI, BUI)
    - Then multi-region types (DQUOT, BUF)
    - Finally complex types (INODE, ATTRI)

Phase 3: Full item validation
-----------------------------

Patch M: Add validate_item callback infrastructure
    - Add the callback to the ops struct
    - Call it from ``xlog_recover_reorder_trans()`` after ops verification
    - Return ``-EFSCORRUPTED`` on failure, aborting recovery

Patches M+1 to M+N: Implement validate_item for each item type
    - Move existing validation out of ``commit_pass2`` into
      ``validate_item`` where possible (e.g. ICREATE field checks)
    - Add new cross-region validation
    - Order: simple types first, complex types last
    - INODE validation is the most complex (fork format vs region size)

Phase 4: Cleanup
----------------

- Remove redundant validation from ``commit_pass1``/``commit_pass2`` that
  is now covered by ``validate_region``/``validate_item``
- Add ``ASSERT()``\ s in commit handlers to verify validation has run
- Review and update error messages for consistency

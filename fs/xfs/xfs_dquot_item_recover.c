// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "xfs_platform.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_quota.h"
#include "xfs_trans.h"
#include "xfs_buf_item.h"
#include "xfs_trans_priv.h"
#include "xfs_qm.h"
#include "xfs_log.h"
#include "xfs_log_priv.h"
#include "xfs_log_recover.h"
#include "xfs_error.h"

STATIC void
xlog_recover_dquot_ra_pass2(
	struct xlog			*log,
	struct xlog_recover_item	*item)
{
	struct xfs_mount	*mp = log->l_mp;
	struct xfs_disk_dquot	*recddq;
	struct xfs_dq_logformat	*dq_f;
	uint			type;

	if (mp->m_qflags == 0)
		return;

	recddq = item->ri_buf[1].iov_base;
	type = recddq->d_type & XFS_DQTYPE_REC_MASK;
	ASSERT(type);
	if (log->l_quotaoffs_flag & type)
		return;

	dq_f = item->ri_buf[0].iov_base;
	ASSERT(dq_f);
	ASSERT(dq_f->qlf_len == 1);

	xlog_buf_readahead(log, dq_f->qlf_blkno,
			XFS_FSB_TO_BB(mp, dq_f->qlf_len),
			&xfs_dquot_buf_ra_ops);
}

/*
 * Recover a dquot record
 */
STATIC int
xlog_recover_dquot_commit_pass2(
	struct xlog			*log,
	struct list_head		*buffer_list,
	struct xlog_recover_item	*item,
	xfs_lsn_t			current_lsn)
{
	struct xfs_mount		*mp = log->l_mp;
	struct xfs_buf			*bp;
	struct xfs_dqblk		*dqb;
	struct xfs_disk_dquot		*ddq, *recddq;
	struct xfs_dq_logformat		*dq_f;
	xfs_failaddr_t			fa;
	int				error;
	uint				type;

	/*
	 * Filesystems are required to send in quota flags at mount time.
	 */
	if (mp->m_qflags == 0)
		return 0;

	recddq = item->ri_buf[1].iov_base;

	/*
	 * This type of quotas was turned off, so ignore this record.
	 */
	type = recddq->d_type & XFS_DQTYPE_REC_MASK;
	ASSERT(type);
	if (log->l_quotaoffs_flag & type)
		return 0;

	/*
	 * At this point we know that quota was _not_ turned off.
	 * Since the mount flags are not indicating to us otherwise, this
	 * must mean that quota is on, and the dquot needs to be replayed.
	 * Remember that we may not have fully recovered the superblock yet,
	 * so we can't do the usual trick of looking at the SB quota bits.
	 *
	 * The other possibility, of course, is that the quota subsystem was
	 * removed since the last mount - ENOSYS.
	 */
	dq_f = item->ri_buf[0].iov_base;

	/*
	 * At this point we are assuming that the dquots have been allocated
	 * and hence the buffer has valid dquots stamped in it. It should,
	 * therefore, pass verifier validation. If the dquot is bad, then the
	 * we'll return an error here, so we don't need to specifically check
	 * the dquot in the buffer after the verifier has run.
	 */
	error = xfs_trans_read_buf(mp, NULL, mp->m_ddev_targp, dq_f->qlf_blkno,
				   XFS_FSB_TO_BB(mp, dq_f->qlf_len), 0, &bp,
				   &xfs_dquot_buf_ops);
	if (error)
		return error;

	ASSERT(bp);
	dqb = xfs_buf_offset(bp, dq_f->qlf_boffset);
	ddq = &dqb->dd_diskdq;

	/*
	 * If the dquot has an LSN in it, recover the dquot only if it's less
	 * than the lsn of the transaction we are replaying.
	 */
	if (xfs_has_crc(mp)) {
		xfs_lsn_t	lsn = be64_to_cpu(dqb->dd_lsn);

		if (lsn && lsn != -1 && XFS_LSN_CMP(lsn, current_lsn) >= 0) {
			goto out_release;
		}
	}

	memcpy(ddq, recddq, item->ri_buf[1].iov_len);
	if (xfs_has_crc(mp)) {
		xfs_update_cksum((char *)dqb, sizeof(struct xfs_dqblk),
				 XFS_DQUOT_CRC_OFF);
	}

	/* Validate the recovered dquot. */
	fa = xfs_dqblk_verify(log->l_mp, dqb, dq_f->qlf_id);
	if (fa) {
		XFS_CORRUPTION_ERROR("Bad dquot after recovery",
				XFS_ERRLEVEL_LOW, mp, dqb,
				sizeof(struct xfs_dqblk));
		xfs_alert(mp,
 "Metadata corruption detected at %pS, dquot 0x%x",
				fa, dq_f->qlf_id);
		error = -EFSCORRUPTED;
		goto out_release;
	}

	ASSERT(dq_f->qlf_size == 2);
	ASSERT(bp->b_mount == mp);
	xfs_buf_delwri_queue(bp, buffer_list);

out_release:
	xfs_buf_relse(bp);
	return error;
}

/* A dquot item is a format header region followed by the dquot record. */
static int
xlog_recover_dquot_nregions(
	struct xlog			*log,
	uint16_t			nregions)
{
	if (nregions != 2)
		return -EFSCORRUPTED;
	return 2;
}

static int
xlog_recover_dquot_validate_region(
	struct xlog			*log,
	struct xlog_recover_item	*item,
	int				region_index)
{
	size_t				len = item->ri_buf[region_index].iov_len;
	size_t				expected;

	if (region_index == 0)
		expected = sizeof(struct xfs_dq_logformat);
	else
		expected = sizeof(struct xfs_disk_dquot);

	if (len != expected) {
		xfs_warn(log->l_mp,
			"DQUOT: bad region %d length %zu recovered.",
			region_index, len);
		return -EFSCORRUPTED;
	}

	return 0;
}

static int
xlog_recover_dquot_validate_item(
	struct xlog			*log,
	struct xlog_recover_item	*item)
{
	struct xfs_mount		*mp = log->l_mp;
	struct xfs_dq_logformat		*dq_f = item->ri_buf[0].iov_base;
	struct xfs_disk_dquot		*ddq = item->ri_buf[1].iov_base;
	xfs_failaddr_t			fa;

	if (dq_f->qlf_len != XFS_DQUOT_CLUSTER_SIZE_FSB) {
		xfs_warn(mp, "DQUOT: bad buffer length %d recovered.",
				dq_f->qlf_len);
		return -EFSCORRUPTED;
	}

	if (dq_f->qlf_boffset + sizeof(struct xfs_dqblk) >
			XFS_FSB_TO_B(mp, XFS_DQUOT_CLUSTER_SIZE_FSB)) {
		xfs_warn(mp, "DQUOT: bad buffer offset %u recovered.",
				dq_f->qlf_boffset);
		return -EFSCORRUPTED;
	}

	if (!xfs_verify_fsbno(mp, XFS_DADDR_TO_FSB(mp, dq_f->qlf_blkno))) {
		xfs_warn(mp, "DQUOT: bad buffer block 0x%llx recovered.",
				dq_f->qlf_blkno);
		return -EFSCORRUPTED;
	}

	fa = xfs_dquot_verify(mp, ddq, dq_f->qlf_id);
	if (fa) {
		xfs_warn(mp, "DQUOT: dquot id 0x%x failed validation at %pS",
				dq_f->qlf_id, fa);
		return -EFSCORRUPTED;
	}

	return 0;
}

const struct xlog_recover_item_ops xlog_dquot_item_ops = {
	.item_type		= XFS_LI_DQUOT,
	.min_hdr_len		= sizeof(struct xfs_dq_logformat),
	.validate_nregions	= xlog_recover_dquot_nregions,
	.validate_region	= xlog_recover_dquot_validate_region,
	.validate_item		= xlog_recover_dquot_validate_item,
	.ra_pass2		= xlog_recover_dquot_ra_pass2,
	.commit_pass2		= xlog_recover_dquot_commit_pass2,
};

/*
 * Recover QUOTAOFF records. We simply make a note of it in the xlog
 * structure, so that we know not to do any dquot item or dquot buffer recovery,
 * of that type.
 */
STATIC int
xlog_recover_quotaoff_commit_pass1(
	struct xlog			*log,
	struct xlog_recover_item	*item)
{
	struct xfs_qoff_logformat	*qoff_f = item->ri_buf[0].iov_base;
	ASSERT(qoff_f);

	/*
	 * The logitem format's flag tells us if this was user quotaoff,
	 * group/project quotaoff or both.
	 */
	if (qoff_f->qf_flags & XFS_UQUOTA_ACCT)
		log->l_quotaoffs_flag |= XFS_DQTYPE_USER;
	if (qoff_f->qf_flags & XFS_PQUOTA_ACCT)
		log->l_quotaoffs_flag |= XFS_DQTYPE_PROJ;
	if (qoff_f->qf_flags & XFS_GQUOTA_ACCT)
		log->l_quotaoffs_flag |= XFS_DQTYPE_GROUP;

	return 0;
}

static int
xlog_recover_quotaoff_validate_region(
	struct xlog			*log,
	struct xlog_recover_item	*item,
	int				region_index)
{
	if (item->ri_buf[region_index].iov_len !=
			sizeof(struct xfs_qoff_logformat)) {
		xfs_warn(log->l_mp, "QOFF: bad item region length %zu recovered.",
				item->ri_buf[region_index].iov_len);
		return -EFSCORRUPTED;
	}
	return 0;
}

const struct xlog_recover_item_ops xlog_quotaoff_item_ops = {
	.item_type		= XFS_LI_QUOTAOFF,
	.min_hdr_len		= sizeof(struct xfs_qoff_logformat),
	.validate_nregions	= xlog_recover_single_region,
	.validate_region	= xlog_recover_quotaoff_validate_region,
	.commit_pass1		= xlog_recover_quotaoff_commit_pass1,
	/* nothing to commit in pass2 */
};

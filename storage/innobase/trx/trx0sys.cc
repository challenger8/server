/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file trx/trx0sys.cc
Transaction system

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "ha_prototypes.h"

#include "mysqld.h"
#include "trx0sys.h"
#include "sql_error.h"

#include "fsp0fsp.h"
#include "mtr0log.h"
#include "mtr0log.h"
#include "trx0trx.h"
#include "trx0rseg.h"
#include "trx0undo.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0purge.h"
#include "log0log.h"
#include "log0recv.h"
#include "os0file.h"
#include "read0read.h"
#include "fsp0sysspace.h"

#include <mysql/service_wsrep.h>

/** The transaction system */
trx_sys_t		trx_sys;

/** Check whether transaction id is valid.
@param[in]	id              transaction id to check
@param[in]      name            table name */
void
ReadView::check_trx_id_sanity(
	trx_id_t		id,
	const table_name_t&	name)
{
	if (id >= trx_sys.get_max_trx_id()) {

		ib::warn() << "A transaction id"
			   << " in a record of table "
			   << name
			   << " is newer than the"
			   << " system-wide maximum.";
		ut_ad(0);
		THD *thd = current_thd;
		if (thd != NULL) {
			char    table_name[MAX_FULL_NAME_LEN + 1];

			innobase_format_name(
				table_name, sizeof(table_name),
				name.m_name);

			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    ER_SIGNAL_WARN,
					    "InnoDB: Transaction id"
					    " in a record of table"
					    " %s is newer than system-wide"
					    " maximum.", table_name);
		}
	}
}

#ifdef UNIV_DEBUG
/* Flag to control TRX_RSEG_N_SLOTS behavior debugging. */
uint	trx_rseg_n_slots_debug = 0;
#endif


/**
  Writes the value of m_max_trx_id to the file based trx system header.
*/

void trx_sys_t::flush_max_trx_id()
{
  ut_ad(trx_sys.mutex.is_owned());
  if (!srv_read_only_mode)
  {
    mtr_t mtr;
    mtr.start();
    mlog_write_ull(trx_sysf_get(&mtr) + TRX_SYS_TRX_ID_STORE,
                   trx_sys.get_max_trx_id(), &mtr);
    mtr.commit();
  }
}


/*****************************************************************//**
Updates the offset information about the end of the MySQL binlog entry
which corresponds to the transaction just being committed. In a MySQL
replication slave updates the latest master binlog position up to which
replication has proceeded. */
void
trx_sys_update_mysql_binlog_offset(
/*===============================*/
	const char*	file_name,/*!< in: MySQL log file name */
	int64_t		offset,	/*!< in: position in that log file */
        trx_sysf_t*     sys_header, /*!< in: trx sys header */
	mtr_t*		mtr)	/*!< in: mtr */
{
	DBUG_PRINT("InnoDB",("trx_mysql_binlog_offset: %lld", (longlong) offset));

	const size_t len = strlen(file_name) + 1;

	if (len > TRX_SYS_MYSQL_LOG_NAME_LEN) {

		/* We cannot fit the name to the 512 bytes we have reserved */

		return;
	}

	if (mach_read_from_4(TRX_SYS_MYSQL_LOG_MAGIC_N_FLD
			     + TRX_SYS_MYSQL_LOG_INFO + sys_header)
	    != TRX_SYS_MYSQL_LOG_MAGIC_N) {

		mlog_write_ulint(TRX_SYS_MYSQL_LOG_MAGIC_N_FLD
				 + TRX_SYS_MYSQL_LOG_INFO + sys_header,
				 TRX_SYS_MYSQL_LOG_MAGIC_N,
				 MLOG_4BYTES, mtr);
	}

	if (memcmp(file_name, TRX_SYS_MYSQL_LOG_NAME + TRX_SYS_MYSQL_LOG_INFO
		   + sys_header, len)) {
		mlog_write_string(TRX_SYS_MYSQL_LOG_NAME
				  + TRX_SYS_MYSQL_LOG_INFO
				  + sys_header,
				  reinterpret_cast<const byte*>(file_name),
				  len, mtr);
	}

	mlog_write_ull(TRX_SYS_MYSQL_LOG_INFO + TRX_SYS_MYSQL_LOG_OFFSET
		       + sys_header, offset, mtr);
}

/** Display the MySQL binlog offset info if it is present in the trx
system header. */
void
trx_sys_print_mysql_binlog_offset()
{
	mtr_t		mtr;

	mtr.start();

	const trx_sysf_t*	sys_header = trx_sysf_get(&mtr);

	if (mach_read_from_4(TRX_SYS_MYSQL_LOG_INFO
			     + TRX_SYS_MYSQL_LOG_MAGIC_N_FLD + sys_header)
	    == TRX_SYS_MYSQL_LOG_MAGIC_N) {
		ib::info() << "Last binlog file '"
			<< TRX_SYS_MYSQL_LOG_INFO + TRX_SYS_MYSQL_LOG_NAME
			+ sys_header
			<< "', position "
			<< mach_read_from_8(TRX_SYS_MYSQL_LOG_INFO
					    + TRX_SYS_MYSQL_LOG_OFFSET
					    + sys_header);
	}

	mtr.commit();
}

#ifdef WITH_WSREP

#ifdef UNIV_DEBUG
static long long trx_sys_cur_xid_seqno = -1;
static unsigned char trx_sys_cur_xid_uuid[16];

/** Read WSREP XID seqno */
static inline long long read_wsrep_xid_seqno(const XID* xid)
{
	long long seqno;
	memcpy(&seqno, xid->data + 24, sizeof(long long));
	return seqno;
}

/** Read WSREP XID UUID */
static inline void read_wsrep_xid_uuid(const XID* xid, unsigned char* buf)
{
	memcpy(buf, xid->data + 8, 16);
}

#endif /* UNIV_DEBUG */

/** Update WSREP XID info in sys_header of TRX_SYS_PAGE_NO = 5.
@param[in]	xid		Transaction XID
@param[in,out]	sys_header	sys_header
@param[in]	mtr		minitransaction */
UNIV_INTERN
void
trx_sys_update_wsrep_checkpoint(
	const XID*	xid,
	trx_sysf_t*	sys_header,
	mtr_t*		mtr)
{
	ut_ad(xid->formatID == 1);
	ut_ad(wsrep_is_wsrep_xid(xid));

	if (mach_read_from_4(sys_header + TRX_SYS_WSREP_XID_INFO
			     + TRX_SYS_WSREP_XID_MAGIC_N_FLD)
		!= TRX_SYS_WSREP_XID_MAGIC_N) {
		mlog_write_ulint(sys_header + TRX_SYS_WSREP_XID_INFO
			+ TRX_SYS_WSREP_XID_MAGIC_N_FLD,
			TRX_SYS_WSREP_XID_MAGIC_N,
			MLOG_4BYTES, mtr);
#ifdef UNIV_DEBUG
	} else {
		/* Check that seqno is monotonically increasing */
		unsigned char xid_uuid[16];
		long long xid_seqno = read_wsrep_xid_seqno(xid);
		read_wsrep_xid_uuid(xid, xid_uuid);

		if (!memcmp(xid_uuid, trx_sys_cur_xid_uuid, 8)) {
			ut_ad(xid_seqno > trx_sys_cur_xid_seqno);
			trx_sys_cur_xid_seqno = xid_seqno;
		} else {
			memcpy(trx_sys_cur_xid_uuid, xid_uuid, 16);
		}

		trx_sys_cur_xid_seqno = xid_seqno;
#endif /* UNIV_DEBUG */
	}

	mlog_write_ulint(sys_header + TRX_SYS_WSREP_XID_INFO
		+ TRX_SYS_WSREP_XID_FORMAT,
		(int)xid->formatID,
		MLOG_4BYTES, mtr);
	mlog_write_ulint(sys_header + TRX_SYS_WSREP_XID_INFO
		+ TRX_SYS_WSREP_XID_GTRID_LEN,
		(int)xid->gtrid_length,
		MLOG_4BYTES, mtr);
	mlog_write_ulint(sys_header + TRX_SYS_WSREP_XID_INFO
		+ TRX_SYS_WSREP_XID_BQUAL_LEN,
		(int)xid->bqual_length,
		MLOG_4BYTES, mtr);
	mlog_write_string(sys_header + TRX_SYS_WSREP_XID_INFO
		+ TRX_SYS_WSREP_XID_DATA,
		(const unsigned char*) xid->data,
		XIDDATASIZE, mtr);
}

/** Read WSREP checkpoint XID from sys header.
@param[out]	xid	WSREP XID
@return	whether the checkpoint was present */
UNIV_INTERN
bool
trx_sys_read_wsrep_checkpoint(XID* xid)
{
	trx_sysf_t*	sys_header;
	mtr_t		mtr;
	ulint		magic;

	ut_ad(xid);

	mtr_start(&mtr);

	sys_header = trx_sysf_get(&mtr);

	if ((magic = mach_read_from_4(sys_header + TRX_SYS_WSREP_XID_INFO
					+ TRX_SYS_WSREP_XID_MAGIC_N_FLD))
	    != TRX_SYS_WSREP_XID_MAGIC_N) {
		memset(xid, 0, sizeof(*xid));
		long long seqno= -1;
		memcpy(xid->data + 24, &seqno, sizeof(long long));
		xid->formatID = -1;
		mtr_commit(&mtr);
		return false;
	}

	xid->formatID = (int)mach_read_from_4(
			sys_header
			+ TRX_SYS_WSREP_XID_INFO + TRX_SYS_WSREP_XID_FORMAT);
	xid->gtrid_length = (int)mach_read_from_4(
			sys_header
			+ TRX_SYS_WSREP_XID_INFO + TRX_SYS_WSREP_XID_GTRID_LEN);
	xid->bqual_length = (int)mach_read_from_4(
			sys_header
			+ TRX_SYS_WSREP_XID_INFO + TRX_SYS_WSREP_XID_BQUAL_LEN);
	ut_memcpy(xid->data,
		  sys_header + TRX_SYS_WSREP_XID_INFO + TRX_SYS_WSREP_XID_DATA,
		  XIDDATASIZE);

	mtr_commit(&mtr);
	return true;
}

#endif /* WITH_WSREP */

/** @return an unallocated rollback segment slot in the TRX_SYS header
@retval ULINT_UNDEFINED if not found */
ulint
trx_sysf_rseg_find_free(mtr_t* mtr)
{
	trx_sysf_t*	sys_header = trx_sysf_get(mtr);

	for (ulint i = 0; i < TRX_SYS_N_RSEGS; i++) {
		if (trx_sysf_rseg_get_page_no(sys_header, i, mtr)
		    == FIL_NULL) {
			return(i);
		}
	}

	return(ULINT_UNDEFINED);
}

/** Count the number of initialized persistent rollback segment slots. */
static
void
trx_sysf_get_n_rseg_slots()
{
	mtr_t		mtr;
	mtr.start();

	trx_sysf_t*	sys_header	= trx_sysf_get(&mtr);
	srv_available_undo_logs = 0;

	for (ulint i = 0; i < TRX_SYS_N_RSEGS; i++) {
		srv_available_undo_logs
			+= trx_sysf_rseg_get_page_no(sys_header, i, &mtr)
			!= FIL_NULL;
	}

	mtr.commit();
}

/*****************************************************************//**
Creates the file page for the transaction system. This function is called only
at the database creation, before trx_sys_init. */
static
void
trx_sysf_create(
/*============*/
	mtr_t*	mtr)	/*!< in: mtr */
{
	trx_sysf_t*	sys_header;
	ulint		slot_no;
	buf_block_t*	block;
	page_t*		page;
	ulint		page_no;
	byte*		ptr;

	ut_ad(mtr);

	/* Note that below we first reserve the file space x-latch, and
	then enter the kernel: we must do it in this order to conform
	to the latching order rules. */

	mtr_x_lock_space(TRX_SYS_SPACE, mtr);

	/* Create the trx sys file block in a new allocated file segment */
	block = fseg_create(TRX_SYS_SPACE, 0, TRX_SYS + TRX_SYS_FSEG_HEADER,
			    mtr);
	buf_block_dbg_add_level(block, SYNC_TRX_SYS_HEADER);

	ut_a(block->page.id.page_no() == TRX_SYS_PAGE_NO);

	page = buf_block_get_frame(block);

	mlog_write_ulint(page + FIL_PAGE_TYPE, FIL_PAGE_TYPE_TRX_SYS,
			 MLOG_2BYTES, mtr);

	/* Reset the doublewrite buffer magic number to zero so that we
	know that the doublewrite buffer has not yet been created (this
	suppresses a Valgrind warning) */

	mlog_write_ulint(page + TRX_SYS_DOUBLEWRITE
			 + TRX_SYS_DOUBLEWRITE_MAGIC, 0, MLOG_4BYTES, mtr);

	sys_header = trx_sysf_get(mtr);

	/* Start counting transaction ids from number 1 up */
	mach_write_to_8(sys_header + TRX_SYS_TRX_ID_STORE, 1);

	/* Reset the rollback segment slots.  Old versions of InnoDB
	(before MySQL 5.5) define TRX_SYS_N_RSEGS as 256 and expect
	that the whole array is initialized. */
	ptr = TRX_SYS_RSEGS + sys_header;
	compile_time_assert(256 >= TRX_SYS_N_RSEGS);
	memset(ptr, 0xff, 256 * TRX_SYS_RSEG_SLOT_SIZE);
	ptr += 256 * TRX_SYS_RSEG_SLOT_SIZE;
	ut_a(ptr <= page + (UNIV_PAGE_SIZE - FIL_PAGE_DATA_END));

	/* Initialize all of the page.  This part used to be uninitialized. */
	memset(ptr, 0, UNIV_PAGE_SIZE - FIL_PAGE_DATA_END + page - ptr);

	mlog_log_string(sys_header, UNIV_PAGE_SIZE - FIL_PAGE_DATA_END
			+ page - sys_header, mtr);

	/* Create the first rollback segment in the SYSTEM tablespace */
	slot_no = trx_sysf_rseg_find_free(mtr);
	page_no = trx_rseg_header_create(TRX_SYS_SPACE,
					 ULINT_MAX, slot_no, mtr);

	ut_a(slot_no == TRX_SYS_SYSTEM_RSEG_ID);
	ut_a(page_no == FSP_FIRST_RSEG_PAGE_NO);
}

/** Initialize the transaction system main-memory data structures. */
void
trx_sys_init_at_db_start()
{
	trx_sysf_t*	sys_header;

	/* VERY important: after the database is started, max_trx_id value is
	divisible by TRX_SYS_TRX_ID_WRITE_MARGIN, and the 'if' in
	trx_sys.get_new_trx_id will evaluate to TRUE when the function
	is first time called, and the value for trx id will be written
	to the disk-based header! Thus trx id values will not overlap when
	the database is repeatedly started! */

	mtr_t	mtr;
	mtr.start();

	sys_header = trx_sysf_get(&mtr);

	trx_sys.init_max_trx_id(2 * TRX_SYS_TRX_ID_WRITE_MARGIN
		+ ut_uint64_align_up(mach_read_from_8(sys_header
						   + TRX_SYS_TRX_ID_STORE),
				     TRX_SYS_TRX_ID_WRITE_MARGIN));

	mtr.commit();

	trx_dummy_sess = sess_open();

	trx_lists_init_at_db_start();
	trx_sys.mvcc.clone_oldest_view(&purge_sys->view);
}

/** Create the instance */
void
trx_sys_t::create()
{
	ut_ad(this == &trx_sys);
	ut_ad(!is_initialised());
	m_initialised = true;
	mutex_create(LATCH_ID_TRX_SYS, &mutex);

	UT_LIST_INIT(serialisation_list, &trx_t::no_list);
	UT_LIST_INIT(mysql_trx_list, &trx_t::mysql_trx_list);

	mvcc.create(1024);

	rw_trx_hash.init();
}

/*****************************************************************//**
Creates and initializes the transaction system at the database creation. */
void
trx_sys_create_sys_pages(void)
/*==========================*/
{
	mtr_t	mtr;

	mtr_start(&mtr);

	trx_sysf_create(&mtr);

	mtr_commit(&mtr);
}

/** Create the rollback segments.
@return	whether the creation succeeded */
bool
trx_sys_create_rsegs()
{
	/* srv_available_undo_logs reflects the number of persistent
	rollback segments that have been initialized in the
	transaction system header page.

	srv_undo_logs determines how many of the
	srv_available_undo_logs rollback segments may be used for
	logging new transactions. */
	ut_ad(srv_undo_tablespaces <= TRX_SYS_MAX_UNDO_SPACES);
	ut_ad(srv_undo_logs <= TRX_SYS_N_RSEGS);

	if (srv_read_only_mode) {
		srv_undo_logs = srv_available_undo_logs = ULONG_UNDEFINED;
		return(true);
	}

	/* This is executed in single-threaded mode therefore it is not
	necessary to use the same mtr in trx_rseg_create(). n_used cannot
	change while the function is executing. */
	trx_sysf_get_n_rseg_slots();

	ut_ad(srv_available_undo_logs <= TRX_SYS_N_RSEGS);

	/* The first persistent rollback segment is always initialized
	in the system tablespace. */
	ut_a(srv_available_undo_logs > 0);

	if (srv_force_recovery) {
		/* Do not create additional rollback segments if
		innodb_force_recovery has been set. */
		if (srv_undo_logs > srv_available_undo_logs) {
			srv_undo_logs = srv_available_undo_logs;
		}
	} else {
		for (ulint i = 0; srv_available_undo_logs < srv_undo_logs;
		     i++, srv_available_undo_logs++) {
			/* Tablespace 0 is the system tablespace.
			Dedicated undo log tablespaces start from 1. */
			ulint space = srv_undo_tablespaces > 0
				? (i % srv_undo_tablespaces)
				+ srv_undo_space_id_start
				: TRX_SYS_SPACE;

			if (!trx_rseg_create(space)) {
				ib::error() << "Unable to allocate the"
					" requested innodb_undo_logs";
				return(false);
			}

			/* Increase the number of active undo
			tablespace in case new rollback segment
			assigned to new undo tablespace. */
			if (space > srv_undo_tablespaces_active) {
				srv_undo_tablespaces_active++;

				ut_ad(srv_undo_tablespaces_active == space);
			}
		}
	}

	ut_ad(srv_undo_logs <= srv_available_undo_logs);

	ib::info info;
	info << srv_undo_logs << " out of " << srv_available_undo_logs;
	if (srv_undo_tablespaces_active) {
		info << " rollback segments in " << srv_undo_tablespaces_active
		<< " undo tablespaces are active.";
	} else {
		info << " rollback segments are active.";
	}

	return(true);
}

/** Close the transaction system on shutdown */
void
trx_sys_t::close()
{
	ut_ad(srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS);
	if (!is_initialised()) {
		return;
	}

	if (ulint size = mvcc.size()) {
		ib::error() << "All read views were not closed before"
			" shutdown: " << size << " read views open";
	}

	if (trx_dummy_sess) {
		sess_close(trx_dummy_sess);
		trx_dummy_sess = NULL;
	}

	rw_trx_hash.destroy();

	/* There can't be any active transactions. */

	for (ulint i = 0; i < TRX_SYS_N_RSEGS; ++i) {
		if (trx_rseg_t* rseg = rseg_array[i]) {
			trx_rseg_mem_free(rseg);
		}

		if (trx_rseg_t* rseg = temp_rsegs[i]) {
			trx_rseg_mem_free(rseg);
		}
	}

	mvcc.close();

	ut_a(UT_LIST_GET_LEN(mysql_trx_list) == 0);
	ut_a(UT_LIST_GET_LEN(serialisation_list) == 0);

	/* We used placement new to create this mutex. Call the destructor. */
	mutex_free(&mutex);
	m_initialised = false;
}


static my_bool active_count_callback(rw_trx_hash_element_t *element,
                                     uint32_t *count)
{
  mutex_enter(&element->mutex);
  if (trx_t *trx= element->trx)
  {
    mutex_enter(&trx->mutex);
    if (trx_state_eq(trx, TRX_STATE_ACTIVE))
      ++*count;
    mutex_exit(&trx->mutex);
  }
  mutex_exit(&element->mutex);
  return 0;
}


/** @return total number of active (non-prepared) transactions */
ulint trx_sys_t::any_active_transactions()
{
	uint32_t total_trx = 0;

	trx_sys.rw_trx_hash.iterate_no_dups(
				reinterpret_cast<my_hash_walk_action>
				(active_count_callback), &total_trx);

	mutex_enter(&trx_sys.mutex);
	for (trx_t* trx = UT_LIST_GET_FIRST(trx_sys.mysql_trx_list);
	     trx != NULL;
	     trx = UT_LIST_GET_NEXT(mysql_trx_list, trx)) {
		if (trx->state != TRX_STATE_NOT_STARTED && !trx->id) {
			total_trx++;
		}
	}
	mutex_exit(&trx_sys.mutex);

	return(total_trx);
}

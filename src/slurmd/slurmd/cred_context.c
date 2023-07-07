/*****************************************************************************\
 *  cred_context.c
 *****************************************************************************
 *  Copyright (C) 2023 SchedMD LLC.
 *  Written by Tim Wickberg <tim@schedmd.com>
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "src/common/pack.h"
#include "src/common/slurm_protocol_pack.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/interfaces/cred.h"

#include "src/slurmd/slurmd/slurmd.h"

/* FIXME: Y2038 problem */
#define MAX_TIME 0x7fffffff

static void _drain_node(char *reason)
{
	update_node_msg_t update_node_msg;

	slurm_init_update_node_msg(&update_node_msg);
	update_node_msg.node_names = conf->node_name;
	update_node_msg.node_state = NODE_STATE_DRAIN;
	update_node_msg.reason = reason;

	(void) slurm_update_node(&update_node_msg);
}

static void _job_state_pack(void *x, uint16_t protocol_version, buf_t *buffer)
{
	job_state_t *j = x;

	pack32(j->jobid, buffer);
	pack_time(j->revoked, buffer);
	pack_time(j->ctime, buffer);
	pack_time(j->expiration, buffer);
}

static int _job_state_unpack(void **out, uint16_t protocol_version,
			     buf_t *buffer)
{
	job_state_t *j = xmalloc(sizeof(*j));

	safe_unpack32(&j->jobid, buffer);
	safe_unpack_time(&j->revoked, buffer);
	safe_unpack_time(&j->ctime, buffer);
	safe_unpack_time(&j->expiration, buffer);

	debug3("cred_unpack: job %u ctime:%ld revoked:%ld expires:%ld",
	       j->jobid, j->ctime, j->revoked, j->expiration);

	if ((j->revoked) && (j->expiration == (time_t) MAX_TIME)) {
		warning("revoke on job %u has no expiration", j->jobid);
		j->expiration = j->revoked + 600;
	}

	*out = j;
	return SLURM_SUCCESS;

unpack_error:
	xfree(j);
	*out = NULL;
	return SLURM_ERROR;
}

static void _cred_state_pack(void *x, uint16_t protocol_version, buf_t *buffer)
{
	cred_state_t *s = x;

	/* WARNING: this is not safe if pack_step_id() ever changes format */
	pack_step_id(&s->step_id, buffer, protocol_version);
	pack_time(s->ctime, buffer);
	pack_time(s->expiration, buffer);
}

static int _cred_state_unpack(void **out, uint16_t protocol_version,
			      buf_t *buffer)
{
	cred_state_t *s = xmalloc(sizeof(*s));

	if (unpack_step_id_members(&s->step_id, buffer,
				   SLURM_PROTOCOL_VERSION) != SLURM_SUCCESS)
		goto unpack_error;
	safe_unpack_time(&s->ctime, buffer);
	safe_unpack_time(&s->expiration, buffer);

	*out = s;
	return SLURM_SUCCESS;

unpack_error:
	xfree(s);
	*out = NULL;
	return SLURM_ERROR;
}

static void _cred_context_pack(buf_t *buffer)
{
	/* FIXME: find a way to version this file at some point */
	uint16_t version = SLURM_PROTOCOL_VERSION;

	slurm_mutex_lock(&cred_cache_mutex);
	slurm_pack_list(cred_job_list, _job_state_pack, buffer, version);
	slurm_pack_list(cred_state_list, _cred_state_pack, buffer, version);
	slurm_mutex_unlock(&cred_cache_mutex);
}

static void _cred_context_unpack(buf_t *buffer)
{
	/* FIXME: find a way to version this file at some point */
	uint16_t version = SLURM_PROTOCOL_VERSION;

	slurm_mutex_lock(&cred_cache_mutex);

	FREE_NULL_LIST(cred_job_list);
	if (slurm_unpack_list(&cred_job_list, _job_state_unpack,
			      xfree_ptr, buffer, version)) {
		warning("%s: failed to restore job state from file", __func__);
		cred_job_list = list_create(xfree_ptr);
	}
	/* FIXME: run _clear_expired_job_states() */

	FREE_NULL_LIST(cred_state_list);
	if (slurm_unpack_list(&cred_state_list, _cred_state_unpack,
			      xfree_ptr, buffer, version)) {
		warning("%s: failed to restore job state from file", __func__);
		cred_state_list = list_create(xfree_ptr);
	}
	/* FIXME: run _clear_expired_credential_states() */

	slurm_mutex_unlock(&cred_cache_mutex);
}

extern void save_cred_state(void)
{
	char *new_file, *reg_file;
	int cred_fd = -1, rc;
	buf_t *buffer = NULL;
	static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

	reg_file = xstrdup(conf->spooldir);
	xstrcat(reg_file, "/cred_state");
	new_file = xstrdup(conf->spooldir);
	xstrcat(new_file, "/cred_state.new");

	slurm_mutex_lock(&state_mutex);
	if ((cred_fd = creat(new_file, 0600)) < 0) {
		error("creat(%s): %m", new_file);
		if (errno == ENOSPC)
			_drain_node("SlurmdSpoolDir is full");
		goto cleanup;
	}
	buffer = init_buf(1024);
	_cred_context_pack(buffer);
	rc = write(cred_fd, get_buf_data(buffer), get_buf_offset(buffer));
	if (rc != get_buf_offset(buffer)) {
		error("write %s error %m", new_file);
		(void) unlink(new_file);
		if ((rc < 0) && (errno == ENOSPC))
			_drain_node("SlurmdSpoolDir is full");
		goto cleanup;
	}
	(void) unlink(reg_file);
	if (link(new_file, reg_file))
		debug4("unable to create link for %s -> %s: %m",
		       new_file, reg_file);
	(void) unlink(new_file);

cleanup:
	slurm_mutex_unlock(&state_mutex);
	xfree(reg_file);
	xfree(new_file);
	FREE_NULL_BUFFER(buffer);
	if (cred_fd >= 0)
		close(cred_fd);
}

extern void restore_cred_state(void)
{
	char *file_name = NULL;
	buf_t *buffer = NULL;

	file_name = xstrdup(conf->spooldir);
	xstrcat(file_name, "/cred_state");

	if (!(buffer = create_mmap_buf(file_name)))
		goto cleanup;

	_cred_context_unpack(buffer);

cleanup:
	xfree(file_name);
	FREE_NULL_BUFFER(buffer);
}
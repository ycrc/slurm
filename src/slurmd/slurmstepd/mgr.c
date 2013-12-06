/*****************************************************************************\
 *  src/slurmd/slurmstepd/mgr.c - job manager functions for slurmstepd
 *  $Id$
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2009 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <mgrondona@llnl.gov>.
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
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
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#if HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif

#if HAVE_SYS_PRCTL_H
#  include <sys/prctl.h>
#endif

#ifdef HAVE_AIX
#  undef HAVE_UNSETENV
#  include <sys/checkpnt.h>
#endif
#ifndef HAVE_UNSETENV
#  include "src/common/unsetenv.h"
#endif

#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/poll.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <time.h>

#if HAVE_STDLIB_H
#  include <stdlib.h>
#endif

#ifdef HAVE_PTY_H
#  include <pty.h>
#  ifdef HAVE_UTMP_H
#    include <utmp.h>
#  endif
#endif

#include "slurm/slurm_errno.h"

#include "src/common/cbuf.h"
#include "src/common/cpu_frequency.h"
#include "src/common/env.h"
#include "src/common/fd.h"
#include "src/common/forward.h"
#include "src/common/hostlist.h"
#include "src/common/log.h"
#include "src/common/mpi.h"
#include "src/common/node_select.h"
#include "src/common/plugstack.h"
#include "src/common/safeopen.h"
#include "src/common/slurm_jobacct_gather.h"
#include "src/common/slurm_acct_gather_profile.h"
#include "src/common/switch.h"
#include "src/common/util-net.h"
#include "src/common/xmalloc.h"
#include "src/common/xsignal.h"
#include "src/common/xstring.h"

#include "src/slurmd/slurmd/slurmd.h"

#include "src/slurmd/common/setproctitle.h"
#include "src/slurmd/common/proctrack.h"
#include "src/slurmd/common/task_plugin.h"
#include "src/slurmd/common/run_script.h"
#include "src/slurmd/common/reverse_tree.h"
#include "src/slurmd/common/set_oomadj.h"

#include "src/slurmd/slurmstepd/slurmstepd.h"
#include "src/slurmd/slurmstepd/mgr.h"
#include "src/slurmd/slurmstepd/task.h"
#include "src/slurmd/slurmstepd/io.h"
#include "src/slurmd/slurmstepd/pdebug.h"
#include "src/slurmd/slurmstepd/req.h"
#include "src/slurmd/slurmstepd/pam_ses.h"
#include "src/slurmd/slurmstepd/ulimits.h"
#include "src/slurmd/slurmstepd/step_terminate_monitor.h"

#define RETRY_DELAY 15		/* retry every 15 seconds */
#define MAX_RETRY   240		/* retry 240 times (one hour max) */

/*
 *  List of signals to block in this process
 */
static int mgr_sigarray[] = {
	SIGINT,  SIGTERM, SIGTSTP,
	SIGQUIT, SIGPIPE, SIGUSR1,
	SIGUSR2, SIGALRM, SIGHUP, 0
};

struct priv_state {
	uid_t	saved_uid;
	gid_t	saved_gid;
	gid_t *	gid_list;
	int	ngids;
	char	saved_cwd [4096];
};

step_complete_t step_complete = {
	PTHREAD_COND_INITIALIZER,
	PTHREAD_MUTEX_INITIALIZER,
	-1,
	-1,
	-1,
	{},
	-1,
	-1,
	true,
	(bitstr_t *)NULL,
	0,
	NULL
};

typedef struct kill_thread {
	pthread_t thread_id;
	int       secs;
} kill_thread_t;


/*
 * Prototypes
 */

/*
 * Job manager related prototypes
 */
static int  _access(const char *path, int modes, uid_t uid, gid_t gid);
static void _send_launch_failure(launch_tasks_request_msg_t *,
				 slurm_addr_t *, int);
static int  _drain_node(char *reason);
static int  _fork_all_tasks(slurmd_job_t *job, bool *io_initialized);
static int  _become_user(slurmd_job_t *job, struct priv_state *ps);
static void  _set_prio_process (slurmd_job_t *job);
static void _set_job_log_prefix(slurmd_job_t *job);
static int  _setup_normal_io(slurmd_job_t *job);
static int  _drop_privileges(slurmd_job_t *job, bool do_setuid,
			     struct priv_state *state, bool get_list);
static int  _reclaim_privileges(struct priv_state *state);
static void _send_launch_resp(slurmd_job_t *job, int rc);
static int  _slurmd_job_log_init(slurmd_job_t *job);
static void _wait_for_io(slurmd_job_t *job);
static int  _send_exit_msg(slurmd_job_t *job, uint32_t *tid, int n,
			   int status);
static void _wait_for_children_slurmstepd(slurmd_job_t *job);
static int  _send_pending_exit_msgs(slurmd_job_t *job);
static void _send_step_complete_msgs(slurmd_job_t *job);
static void _wait_for_all_tasks(slurmd_job_t *job);
static int  _wait_for_any_task(slurmd_job_t *job, bool waitflag);

static void _setargs(slurmd_job_t *job);

static void _random_sleep(slurmd_job_t *job);
static int  _run_script_as_user(const char *name, const char *path,
				slurmd_job_t *job, int max_wait, char **env);
static void _unblock_signals(void);

/*
 * Batch job management prototypes:
 */
static char * _make_batch_dir(slurmd_job_t *job);
static char * _make_batch_script(batch_job_launch_msg_t *msg, char *path);
static int    _send_complete_batch_script_msg(slurmd_job_t *job,
					      int err, int status);

/*
 * Initialize the group list using the list of gids from the slurmd if
 * available.  Otherwise initialize the groups with initgroups().
 */
static int _initgroups(slurmd_job_t *job);


static slurmd_job_t *reattach_job;

/*
 * Launch an job step on the current node
 */
extern slurmd_job_t *
mgr_launch_tasks_setup(launch_tasks_request_msg_t *msg, slurm_addr_t *cli,
		       slurm_addr_t *self)
{
	slurmd_job_t *job = NULL;

	if (!(job = job_create(msg))) {
		/* We want to send back to the slurmd the reason we
		   failed so keep track of it since errno could be
		   reset in _send_launch_failure.
		*/
		int fail = errno;
		_send_launch_failure (msg, cli, errno);
		errno = fail;
		return NULL;
	}

	_set_job_log_prefix(job);

	_setargs(job);

	job->envtp->cli = cli;
	job->envtp->self = self;
	job->envtp->select_jobinfo = msg->select_jobinfo;

	return job;
}

/*
 * Find the maximum task return code
 */
static uint32_t _get_exit_code(slurmd_job_t *job)
{
	int i;
	uint32_t step_rc = NO_VAL;

	for (i = 0; i < job->node_tasks; i++) {
		/* If signalled we only need to check one and then
		 * break out of the loop */
		if (WIFSIGNALED(job->task[i]->estatus)) {
			step_rc = job->task[i]->estatus;
			break;
		}
		step_rc = MAX(step_complete.step_rc, job->task[i]->estatus);
	}
	return step_rc;
}

#ifdef HAVE_CRAY
/*
 * Kludge to better inter-operate with ALPS layer:
 * - CONFIRM method requires the SID of the shell executing the job script,
 * - RELEASE method is more robustly called from stepdmgr.
 *
 * To avoid calling the same select/cray plugin function also in slurmctld,
 * we use the following convention:
 * - only job_id, job_state, alloc_sid, and select_jobinfo set to non-NULL,
 * - batch_flag is 0 (corresponding call in slurmctld uses batch_flag = 1),
 * - job_state set to the unlikely value of 'NO_VAL'.
 */
static int _call_select_plugin_from_stepd(slurmd_job_t *job, uint64_t pagg_id,
					  int (*select_fn)(struct job_record *))
{
	struct job_record fake_job_record = {0};
	int rc;

	fake_job_record.job_id		= job->jobid;
	fake_job_record.job_state	= (uint16_t)NO_VAL;
	fake_job_record.select_jobinfo	= select_g_select_jobinfo_alloc();
	select_g_select_jobinfo_set(fake_job_record.select_jobinfo,
				    SELECT_JOBDATA_RESV_ID, &job->resv_id);
	if (pagg_id)
		select_g_select_jobinfo_set(fake_job_record.select_jobinfo,
					    SELECT_JOBDATA_PAGG_ID, &pagg_id);
	rc = (*select_fn)(&fake_job_record);
	select_g_select_jobinfo_free(fake_job_record.select_jobinfo);
	return rc;
}

static int _select_cray_plugin_job_ready(slurmd_job_t *job)
{
	uint64_t pagg_id = slurm_container_find(job->jmgr_pid);

	if (pagg_id == 0) {
		error("no PAGG ID: job service disabled on this host?");
		/*
		 * If this process is not attached to a container, there is no
		 * sense in trying to use the SID as fallback, since the call to
		 * slurm_container_add() in _fork_all_tasks() will fail later.
		 * Hence drain the node until sgi_job returns proper PAGG IDs.
		 */
		return READY_JOB_FATAL;
	}
	return _call_select_plugin_from_stepd(job, pagg_id, select_g_job_ready);
}
#endif

/*
 * Send batch exit code to slurmctld. Non-zero rc will DRAIN the node.
 */
extern void
batch_finish(slurmd_job_t *job, int rc)
{
	step_complete.step_rc = _get_exit_code(job);

	if (job->argv[0] && (unlink(job->argv[0]) < 0))
		error("unlink(%s): %m", job->argv[0]);

#ifdef HAVE_CRAY
	_call_select_plugin_from_stepd(job, 0, select_g_job_fini);
#endif

	if (job->aborted) {
		if ((job->stepid == NO_VAL) ||
		    (job->stepid == SLURM_BATCH_SCRIPT)) {
			info("step %u.%u abort completed",
			     job->jobid, job->stepid);
		} else
			info("job %u abort completed", job->jobid);
	} else if ((job->stepid == NO_VAL) ||
		   (job->stepid == SLURM_BATCH_SCRIPT)) {
		verbose("job %u completed with slurm_rc = %d, job_rc = %d",
			job->jobid, rc, step_complete.step_rc);
		_send_complete_batch_script_msg(job, rc, job->task[0]->estatus);
	} else {
		_wait_for_children_slurmstepd(job);
		verbose("job %u.%u completed with slurm_rc = %d, job_rc = %d",
			job->jobid, job->stepid, rc, step_complete.step_rc);
		_send_step_complete_msgs(job);
	}

	/* Do not purge directory until slurmctld is notified of batch job
	 * completion to avoid race condition with slurmd registering missing
	 * batch job. */
	if (job->batchdir && (rmdir(job->batchdir) < 0))
		error("rmdir(%s): %m",  job->batchdir);
	xfree(job->batchdir);
}

/*
 * Launch a batch job script on the current node
 */
slurmd_job_t *
mgr_launch_batch_job_setup(batch_job_launch_msg_t *msg, slurm_addr_t *cli)
{
	slurmd_job_t *job = NULL;

	if (!(job = job_batch_job_create(msg))) {
		error("job_batch_job_create() failed: %m");
		return NULL;
	}

	_set_job_log_prefix(job);

	_setargs(job);

	if ((job->batchdir = _make_batch_dir(job)) == NULL) {
		goto cleanup;
	}

	xfree(job->argv[0]);

	if ((job->argv[0] = _make_batch_script(msg, job->batchdir)) == NULL) {
		goto cleanup;
	}

	/* this is the new way of setting environment variables */
	env_array_for_batch_job(&job->env, msg, conf->node_name);

	/* this is the old way of setting environment variables (but
	 * needed) */
	job->envtp->overcommit = msg->overcommit;
	job->envtp->select_jobinfo = msg->select_jobinfo;

	return job;

cleanup:
	error("batch script setup failed for job %u.%u",
	      msg->job_id, msg->step_id);

	if (job->aborted)
		verbose("job %u abort complete", job->jobid);
	else if (msg->step_id == SLURM_BATCH_SCRIPT) {
		_send_complete_batch_script_msg(
			job, ESLURMD_CREATE_BATCH_DIR_ERROR, -1);
	} else
		_send_step_complete_msgs(job);

	/* Do not purge directory until slurmctld is notified of batch job
	 * completion to avoid race condition with slurmd registering missing
	 * batch job. */
	if (job->batchdir && (rmdir(job->batchdir) < 0))
		error("rmdir(%s): %m",  job->batchdir);
	xfree(job->batchdir);

	return NULL;
}

static void
_set_job_log_prefix(slurmd_job_t *job)
{
	char buf[256];

	if (job->jobid > MAX_NOALLOC_JOBID)
		return;

	if ((job->jobid >= MIN_NOALLOC_JOBID) || (job->stepid == NO_VAL))
		snprintf(buf, sizeof(buf), "[%u]", job->jobid);
	else
		snprintf(buf, sizeof(buf), "[%u.%u]", job->jobid, job->stepid);

	log_set_fpfx(buf);
}

static int
_setup_normal_io(slurmd_job_t *job)
{
	int rc = 0, ii = 0;
	struct priv_state sprivs;

	debug2("Entering _setup_normal_io");

	/*
	 * Temporarily drop permissions, initialize task stdio file
	 * descriptors (which may be connected to files), then
	 * reclaim privileges.
	 */
	if (_drop_privileges(job, true, &sprivs, true) < 0)
		return ESLURMD_SET_UID_OR_GID_ERROR;

	if (io_init_tasks_stdio(job) != SLURM_SUCCESS) {
		rc = ESLURMD_IO_ERROR;
		goto claim;
	}

	/*
	 * MUST create the initial client object before starting
	 * the IO thread, or we risk losing stdout/err traffic.
	 */
	if (!job->batch) {
		srun_info_t *srun = list_peek(job->sruns);

		/* local id of task that sends to srun, -1 for all tasks,
		   any other value for no tasks */
		int srun_stdout_tasks = -1;
		int srun_stderr_tasks = -1;

		xassert(srun != NULL);

		/* If I/O is labelled with task num, and if a separate file is
		   written per node or per task, the I/O needs to be sent
		   back to the stepd, get a label appended, and written from
		   the stepd rather than sent back to srun or written directly
		   from the node.  When a task has ofname or efname == NULL, it
		   means data gets sent back to the client. */

		if (job->labelio) {
			slurmd_filename_pattern_t outpattern, errpattern;
			bool same = false;
			int file_flags;

			io_find_filename_pattern(job, &outpattern, &errpattern,
						 &same);
			file_flags = io_get_file_flags(job);

			/* Make eio objects to write from the slurmstepd */
			if (outpattern == SLURMD_ALL_UNIQUE) {
				/* Open a separate file per task */
				for (ii = 0; ii < job->node_tasks; ii++) {
					rc = io_create_local_client(
						job->task[ii]->ofname,
						file_flags, job, job->labelio,
						job->task[ii]->id,
						same ? job->task[ii]->id : -2);
					if (rc != SLURM_SUCCESS) {
						error("Could not open output "
						      "file %s: %m",
						      job->task[ii]->ofname);
						rc = ESLURMD_IO_ERROR;
						goto claim;
					}
				}
				srun_stdout_tasks = -2;
				if (same)
					srun_stderr_tasks = -2;
			} else if (outpattern == SLURMD_ALL_SAME) {
				/* Open a file for all tasks */
				rc = io_create_local_client(
					job->task[0]->ofname,
					file_flags, job, job->labelio,
					-1, same ? -1 : -2);
				if (rc != SLURM_SUCCESS) {
					error("Could not open output "
					      "file %s: %m",
					      job->task[0]->ofname);
					rc = ESLURMD_IO_ERROR;
					goto claim;
				}
				srun_stdout_tasks = -2;
				if (same)
					srun_stderr_tasks = -2;
			}

			if (!same) {
				if (errpattern == SLURMD_ALL_UNIQUE) {
					/* Open a separate file per task */
					for (ii = 0; ii < job->node_tasks; ii++) {
						rc = io_create_local_client(
							job->task[ii]->efname,
							file_flags, job,
							job->labelio,
							-2, job->task[ii]->id);
						if (rc != SLURM_SUCCESS) {
							error("Could not "
							      "open error "
							      "file %s: %m",
							      job->task[ii]->
							      efname);
							rc = ESLURMD_IO_ERROR;
							goto claim;
						}
					}
					srun_stderr_tasks = -2;
				} else if (errpattern == SLURMD_ALL_SAME) {
					/* Open a file for all tasks */
					rc = io_create_local_client(
						job->task[0]->efname,
						file_flags, job, job->labelio,
						-2, -1);
					if (rc != SLURM_SUCCESS) {
						error("Could not open error "
						      "file %s: %m",
						      job->task[0]->efname);
						rc = ESLURMD_IO_ERROR;
						goto claim;
					}
					srun_stderr_tasks = -2;
				}
			}
		}

		if (io_initial_client_connect(srun, job, srun_stdout_tasks,
					     srun_stderr_tasks) < 0) {
			rc = ESLURMD_IO_ERROR;
			goto claim;
		}
	}

claim:
	if (_reclaim_privileges(&sprivs) < 0) {
		error("sete{u/g}id(%lu/%lu): %m",
		      (u_long) sprivs.saved_uid, (u_long) sprivs.saved_gid);
	}

	if (!rc && !job->batch) {
		if (io_thread_start(job) < 0)
			rc = ESLURMD_IO_ERROR;
	}

	debug2("Leaving  _setup_normal_io");
	return rc;
}

static int
_setup_user_managed_io(slurmd_job_t *job)
{
	srun_info_t *srun;

	if ((srun = list_peek(job->sruns)) == NULL) {
		error("_setup_user_managed_io: no clients!");
		return SLURM_ERROR;
	}

	return user_managed_io_client_connect(job->node_tasks, srun, job->task);
}

static void
_random_sleep(slurmd_job_t *job)
{
#if !defined HAVE_FRONT_END
	long int delay = 0;
	long int max   = (3 * job->nnodes);

	srand48((long int) (job->jobid + job->nodeid));

	delay = lrand48() % ( max + 1 );
	debug3("delaying %ldms", delay);
	poll(NULL, 0, delay);
#endif
}

/*
 * Send task exit message for n tasks. tid is the list of _global_
 * task ids that have exited
 */
static int
_send_exit_msg(slurmd_job_t *job, uint32_t *tid, int n, int status)
{
	slurm_msg_t     resp;
	task_exit_msg_t msg;
	ListIterator    i       = NULL;
	srun_info_t    *srun    = NULL;

	debug3("sending task exit msg for %d tasks", n);

	msg.task_id_list	= tid;
	msg.num_tasks		= n;
	msg.return_code		= status;
	msg.job_id		= job->jobid;
	msg.step_id		= job->stepid;
	slurm_msg_t_init(&resp);
	resp.data		= &msg;
	resp.msg_type		= MESSAGE_TASK_EXIT;

	/*
	 *  Hack for TCP timeouts on exit of large, synchronized job
	 *  termination. Delay a random amount if job->nnodes > 100
	 */
	if (job->nnodes > 100)
		_random_sleep(job);

	/*
	 * Notify each srun and sattach.
	 * No message for poe or batch jobs
	 */
	i = list_iterator_create(job->sruns);
	while ((srun = list_next(i))) {
		resp.address = srun->resp_addr;
		if ((resp.address.sin_family == 0) &&
		    (resp.address.sin_port == 0)   &&
		    (resp.address.sin_addr.s_addr == 0))
			continue;	/* no srun or sattach here */
		if (slurm_send_only_node_msg(&resp) != SLURM_SUCCESS)
			verbose("Failed to send MESSAGE_TASK_EXIT: %m");
	}
	list_iterator_destroy(i);

	return SLURM_SUCCESS;
}

static void
_wait_for_children_slurmstepd(slurmd_job_t *job)
{
	int left = 0;
	int rc;
	struct timespec ts = {0, 0};

	pthread_mutex_lock(&step_complete.lock);

	/* wait an extra 3 seconds for every level of tree below this level */
	if (step_complete.children > 0) {
		ts.tv_sec += 3 * (step_complete.max_depth-step_complete.depth);
		ts.tv_sec += time(NULL) + REVERSE_TREE_CHILDREN_TIMEOUT;

		while((left = bit_clear_count(step_complete.bits)) > 0) {
			debug3("Rank %d waiting for %d (of %d) children",
			       step_complete.rank, left, step_complete.children);
			rc = pthread_cond_timedwait(&step_complete.cond,
						    &step_complete.lock, &ts);
			if (rc == ETIMEDOUT) {
				debug2("Rank %d timed out waiting for %d"
				       " (of %d) children", step_complete.rank,
				       left, step_complete.children);
				break;
			}
		}
		if (left == 0) {
			debug2("Rank %d got all children completions",
			       step_complete.rank);
		}
	} else {
		debug2("Rank %d has no children slurmstepd",
		       step_complete.rank);
	}

	step_complete.step_rc = _get_exit_code(job);
	step_complete.wait_children = false;

	pthread_mutex_unlock(&step_complete.lock);
}


/*
 * Send a single step completion message, which represents a single range
 * of complete job step nodes.
 */
/* caller is holding step_complete.lock */
static void
_one_step_complete_msg(slurmd_job_t *job, int first, int last)
{
	slurm_msg_t req;
	step_complete_msg_t msg;
	int rc = -1;
	int retcode;
	int i;
	uint16_t port = 0;
	char ip_buf[16];
	static bool acct_sent = false;

	debug2("_one_step_complete_msg: first=%d, last=%d", first, last);

	memset(&msg, 0, sizeof(step_complete_msg_t));
	msg.job_id = job->jobid;
	msg.job_step_id = job->stepid;
	msg.range_first = first;
	msg.range_last = last;
	msg.step_rc = step_complete.step_rc;
	msg.jobacct = jobacctinfo_create(NULL);
	/************* acct stuff ********************/
	if (!acct_sent) {
		jobacctinfo_aggregate(step_complete.jobacct, job->jobacct);
		jobacctinfo_getinfo(step_complete.jobacct,
				    JOBACCT_DATA_TOTAL, msg.jobacct,
				    SLURM_PROTOCOL_VERSION);
		acct_sent = true;
	}
	/*********************************************/
	slurm_msg_t_init(&req);
	req.msg_type = REQUEST_STEP_COMPLETE;
	req.data = &msg;
	req.address = step_complete.parent_addr;

	/* Do NOT change this check to "step_complete.rank != 0", because
	 * there are odd situations where SlurmUser or root could
	 * craft a launch without a valid credential, and no tree information
	 * can be built with out the hostlist from the credential.
	 */
	if (step_complete.parent_rank != -1) {
		debug3("Rank %d sending complete to rank %d, range %d to %d",
		       step_complete.rank, step_complete.parent_rank,
		       first, last);
		/* On error, pause then try sending to parent again.
		 * The parent slurmstepd may just not have started yet, because
		 * of the way that the launch message forwarding works.
		 */
		for (i = 0; i < REVERSE_TREE_PARENT_RETRY; i++) {
			if (i)
				sleep(1);
			retcode = slurm_send_recv_rc_msg_only_one(&req, &rc, 0);
			if ((retcode == 0) && (rc == 0))
				goto finished;
		}
		/* on error AGAIN, send to the slurmctld instead */
		debug3("Rank %d sending complete to slurmctld instead, range "
		       "%d to %d", step_complete.rank, first, last);
	} else {
		/* this is the base of the tree, its parent is slurmctld */
		debug3("Rank %d sending complete to slurmctld, range %d to %d",
		       step_complete.rank, first, last);
	}

	/* Retry step complete RPC send to slurmctld indefinitely.
	 * Prevent orphan job step if slurmctld is down */
	i = 1;
	while (slurm_send_recv_controller_rc_msg(&req, &rc) < 0) {
		if (i++ == 1) {
			slurm_get_ip_str(&step_complete.parent_addr, &port,
					 ip_buf, sizeof(ip_buf));
			error("Rank %d failed sending step completion message "
			      "directly to slurmctld (%s:%u), retrying",
			      step_complete.rank, ip_buf, port);
		}
		sleep(60);
	}
	if (i > 1) {
		info("Rank %d sent step completion message directly to "
		     "slurmctld (%s:%u)", step_complete.rank, ip_buf, port);
	}

finished:
	jobacctinfo_destroy(msg.jobacct);
}

/* Given a starting bit in the step_complete.bits bitstring, "start",
 * find the next contiguous range of set bits and return the first
 * and last indices of the range in "first" and "last".
 *
 * caller is holding step_complete.lock
 */
static int
_bit_getrange(int start, int size, int *first, int *last)
{
	int i;
	bool found_first = false;

	for (i = start; i < size; i++) {
		if (bit_test(step_complete.bits, i)) {
			if (found_first) {
				*last = i;
				continue;
			} else {
				found_first = true;
				*first = i;
				*last = i;
			}
		} else {
			if (!found_first) {
				continue;
			} else {
				*last = i - 1;
				break;
			}
		}
	}

	if (found_first)
		return 1;
	else
		return 0;
}

/*
 * Send as many step completion messages as necessary to represent
 * all completed nodes in the job step.  There may be nodes that have
 * not yet signalled their completion, so there will be gaps in the
 * completed node bitmap, requiring that more than one message be sent.
 */
static void
_send_step_complete_msgs(slurmd_job_t *job)
{
	int start, size;
	int first=-1, last=-1;
	bool sent_own_comp_msg = false;

	pthread_mutex_lock(&step_complete.lock);
	start = 0;
	size = bit_size(step_complete.bits);

	/* If no children, send message and return early */
	if (size == 0) {
		_one_step_complete_msg(job, step_complete.rank,
				       step_complete.rank);
		pthread_mutex_unlock(&step_complete.lock);
		return;
	}

	while(_bit_getrange(start, size, &first, &last)) {
		/* THIS node is not in the bit string, so we need to prepend
		   the local rank */
		if (start == 0 && first == 0) {
			sent_own_comp_msg = true;
			first = -1;
		}

		_one_step_complete_msg(job, (first + step_complete.rank + 1),
	      			       (last + step_complete.rank + 1));
		start = last + 1;
	}

	if (!sent_own_comp_msg)
		_one_step_complete_msg(job, step_complete.rank,
				       step_complete.rank);

	pthread_mutex_unlock(&step_complete.lock);
}

/* This dummy function is provided so that the checkpoint functions can
 * 	resolve this symbol name (as needed for some of the checkpoint
 *	functions used by slurmctld). */
extern void agent_queue_request(void *dummy)
{
	fatal("Invalid agent_queue_request function call, likely from "
	      "checkpoint plugin");
}

/*
 * Executes the functions of the slurmd job manager process,
 * which runs as root and performs shared memory and interconnect
 * initialization, etc.
 *
 * Returns 0 if job ran and completed successfully.
 * Returns errno if job startup failed. NOTE: This will DRAIN the node.
 */
int
job_manager(slurmd_job_t *job)
{
	int  rc = SLURM_SUCCESS;
	bool io_initialized = false;
	char *ckpt_type = slurm_get_checkpoint_type();

	debug3("Entered job_manager for %u.%u pid=%d",
	       job->jobid, job->stepid, job->jmgr_pid);

#ifdef PR_SET_DUMPABLE
	if (prctl(PR_SET_DUMPABLE, 1) < 0)
		debug ("Unable to set dumpable to 1");
#endif /* PR_SET_DUMPABLE */

	/* run now so we don't drop permissions on any of the gather plugins */
	acct_gather_conf_init();

	/*
	 * Preload plugins.
	 */
	if ((switch_init() != SLURM_SUCCESS)			||
	    (slurmd_task_init() != SLURM_SUCCESS)		||
	    (slurm_proctrack_init() != SLURM_SUCCESS)		||
	    (checkpoint_init(ckpt_type) != SLURM_SUCCESS)	||
	    (jobacct_gather_init() != SLURM_SUCCESS)) {
		rc = SLURM_PLUGIN_NAME_INVALID;
		goto fail1;
	}
	if (mpi_hook_slurmstepd_init(&job->env) != SLURM_SUCCESS) {
		rc = SLURM_MPI_PLUGIN_NAME_INVALID;
		goto fail1;
	}

	if (!job->batch &&
	    (interconnect_preinit(job->switch_job) < 0)) {
		rc = ESLURM_INTERCONNECT_FAILURE;
		goto fail1;
	}

	if ((job->cont_id == 0) &&
	    (slurm_container_create(job) != SLURM_SUCCESS)) {
		error("slurm_container_create: %m");
		rc = ESLURMD_SETUP_ENVIRONMENT_ERROR;
		goto fail1;
	}

#ifdef HAVE_CRAY
	/*
	 * Note that the previously called slurm_container_create function is   
	 * mandatory since the select/cray plugin needs the job container
	 * ID in order to CONFIRM the ALPS reservation.
	 * It is not a good idea to perform this setup in _fork_all_tasks(),
	 * since any transient failure of ALPS (which can happen in practice)
	 * will then set the frontend node to DRAIN.
	 *
	 * ALso note that we do not check the reservation for batch jobs with
	 * a reservation ID of zero and no CPUs. These are SLURM job
	 * allocations containing no compute nodes and thus have no ALPS
	 * reservation.
	 */
	if (!job->batch || job->resv_id || job->cpus) {
		rc = _select_cray_plugin_job_ready(job);
		if (rc != SLURM_SUCCESS) {
			/*
			 * Transient error: slurmctld knows this condition to
			 * mean that the ALPS (not the SLURM) reservation
			 * failed and tries again.
			 */
			if (rc == READY_JOB_ERROR)
				rc = ESLURM_RESERVATION_NOT_USABLE;
			else
				rc = ESLURMD_SETUP_ENVIRONMENT_ERROR;
			error("could not confirm ALPS reservation #%u",
			      job->resv_id);
			goto fail1;
		}
	}
#endif

	debug2("Before call to spank_init()");
	if (spank_init (job) < 0) {
		error ("Plugin stack initialization failed.");
		rc = SLURM_PLUGIN_NAME_INVALID;
		goto fail1;
	}
	debug2("After call to spank_init()");

	/* Call interconnect_init() before becoming user */
	if (!job->batch && job->argv &&
	    (interconnect_init(job->switch_job, job->uid, job->argv[0]) < 0)) {
		/* error("interconnect_init: %m"); already logged */
		rc = ESLURM_INTERCONNECT_FAILURE;
		goto fail2;
	}

	/* fork necessary threads for checkpoint */
	if (checkpoint_stepd_prefork(job) != SLURM_SUCCESS) {
		error("Failed checkpoint_stepd_prefork");
		rc = SLURM_FAILURE;
		io_close_task_fds(job);
		goto fail2;
	}

	/* fork necessary threads for MPI */
	if (mpi_hook_slurmstepd_prefork(job, &job->env) != SLURM_SUCCESS) {
		error("Failed mpi_hook_slurmstepd_prefork");
		rc = SLURM_FAILURE;
		goto fail2;
	}

	/* Calls pam_setup() and requires pam_finish() if
	 * successful.  Only check for < 0 here since other slurm
	 * error codes could come that are more descriptive. */
	if ((rc = _fork_all_tasks(job, &io_initialized)) < 0) {
		debug("_fork_all_tasks failed");
		rc = ESLURMD_EXECVE_FAILED;
		goto fail2;
	}

	/*
	 * If IO initialization failed, return SLURM_SUCCESS (on a
	 * batch step) or the node will be drain otherwise.  Regular
	 * srun needs the error sent or it will hang waiting for the
	 * launch to happen.
	 */
	if ((rc != SLURM_SUCCESS) || !io_initialized)
		goto fail2;

	io_close_task_fds(job);

	xsignal_block (mgr_sigarray);
	reattach_job = job;

	job->state = SLURMSTEPD_STEP_RUNNING;

	/* if we are not polling then we need to make sure we get some
	 * information here
	 */
	if (!conf->job_acct_gather_freq)
		jobacct_gather_stat_task(0);

	/* Send job launch response with list of pids */
	_send_launch_resp(job, 0);

	_wait_for_all_tasks(job);
	acct_gather_profile_endpoll();
	acct_gather_profile_g_node_step_end();
	acct_gather_profile_fini();

	job->state = SLURMSTEPD_STEP_ENDING;

	if (!job->batch &&
	    (interconnect_fini(job->switch_job) < 0)) {
		error("interconnect_fini: %m");
	}

fail2:
	/*
	 * First call interconnect_postfini() - In at least one case,
	 * this will clean up any straggling processes. If this call
	 * is moved behind wait_for_io(), we may block waiting for IO
	 * on a hung process.
	 *
	 * Make sure all processes in session are dead. On systems
	 * with an IBM Federation switch, all processes must be
	 * terminated before the switch window can be released by
	 * interconnect_postfini().
	 */
	step_terminate_monitor_start(job->jobid, job->stepid);
	if (job->cont_id != 0) {
		slurm_container_signal(job->cont_id, SIGKILL);
		slurm_container_wait(job->cont_id);
	}
	step_terminate_monitor_stop();
	if (!job->batch) {
		if (interconnect_postfini(job->switch_job, job->jmgr_pid,
					  job->jobid, job->stepid) < 0)
			error("interconnect_postfini: %m");
	}

	/*
	 * Wait for io thread to complete (if there is one)
	 */
	if (!job->batch && !job->user_managed_io && io_initialized)
		_wait_for_io(job);

	/*
	 * Reset cpu frequency if it was changed
	 */

	if (job->cpu_freq != NO_VAL)
		cpu_freq_reset(job);

	/*
	 * Warn task plugin that the user's step have terminated
	 */

	post_step(job);

	/*
	 * This just cleans up all of the PAM state in case rc == 0
	 * which means _fork_all_tasks performs well.
	 * Must be done after IO termination in case of IO operations
	 * require something provided by the PAM (i.e. security token)
	 */
	if (!rc)
		pam_finish();

	debug2("Before call to spank_fini()");
	if (spank_fini (job)  < 0) {
		error ("spank_fini failed");
	}
	debug2("After call to spank_fini()");

fail1:
	/* If interactive job startup was abnormal,
	 * be sure to notify client.
	 */
	if (rc != 0) {
		error("job_manager exiting abnormally, rc = %d", rc);
		_send_launch_resp(job, rc);
	}

	if (!job->batch && (step_complete.rank > -1)) {
		if (job->aborted)
			info("job_manager exiting with aborted job");
		else
			_wait_for_children_slurmstepd(job);
		_send_step_complete_msgs(job);
	}

	xfree(ckpt_type);
	return(rc);
}

static int
_pre_task_privileged(slurmd_job_t *job, int taskid, struct priv_state *sp)
{
	if (_reclaim_privileges(sp) < 0)
		return SLURM_ERROR;

	if (spank_task_privileged (job, taskid) < 0)
		return error("spank_task_init_privileged failed");

	if (pre_launch_priv(job) < 0)
		return error("pre_launch_priv failed");

	return(_drop_privileges (job, true, sp, false));
}

struct exec_wait_info {
	int id;
	pid_t pid;
	int parentfd;
	int childfd;
};

static struct exec_wait_info * exec_wait_info_create (int i)
{
	int fdpair[2];
	struct exec_wait_info * e;

	if (pipe (fdpair) < 0) {
		error ("exec_wait_info_create: pipe: %m");
		return NULL;
	}

	fd_set_close_on_exec(fdpair[0]);
	fd_set_close_on_exec(fdpair[1]);

	e = xmalloc (sizeof (*e));
	e->childfd = fdpair[0];
	e->parentfd = fdpair[1];
	e->id = i;
	e->pid = -1;

	return (e);
}

static void exec_wait_info_destroy (struct exec_wait_info *e)
{
	if (e == NULL)
		return;

	if (e->parentfd >= 0)
		close (e->parentfd);
	if (e->childfd >= 0)
		close (e->childfd);
	e->id = -1;
	e->pid = -1;
	xfree(e);
}

static pid_t exec_wait_get_pid (struct exec_wait_info *e)
{
	if (e == NULL)
		return (-1);
	return (e->pid);
}

static struct exec_wait_info * fork_child_with_wait_info (int id)
{
	struct exec_wait_info *e;

	if (!(e = exec_wait_info_create (id)))
		return (NULL);

	if ((e->pid = fork ()) < 0) {
		exec_wait_info_destroy (e);
		return (NULL);
	}
	/*
	 *  Close parentfd in child, and childfd in parent:
	 */
	if (e->pid == 0) {
		close (e->parentfd);
		e->parentfd = -1;
	}
	else {
		close (e->childfd);
		e->childfd = -1;
	}
	return (e);
}

static int exec_wait_child_wait_for_parent (struct exec_wait_info *e)
{
	char c;

	if (read (e->childfd, &c, sizeof (c)) != 1)
		return error ("wait_for_parent: failed: %m");

	return (0);
}

static int exec_wait_signal_child (struct exec_wait_info *e)
{
	char c = '\0';

	if (write (e->parentfd, &c, sizeof (c)) != 1)
		return error ("write to unblock task %d failed: %m", e->id);

	return (0);
}

static int exec_wait_signal (struct exec_wait_info *e, slurmd_job_t *job)
{
	debug3 ("Unblocking %u.%u task %d, writefd = %d",
	        job->jobid, job->stepid, e->id, e->parentfd);
	exec_wait_signal_child (e);
	return (0);
}

/*
 *  Send SIGKILL to child in exec_wait_info 'e'
 *  Returns 0 for success, -1 for failure.
 */
static int exec_wait_kill_child (struct exec_wait_info *e)
{
	if (e->pid < 0)
		return (-1);
	if (kill (e->pid, SIGKILL) < 0)
		return (-1);
	e->pid = -1;
	return (0);
}

/*
 *  Send all children in exec_wait_list SIGKILL.
 *  Returns 0 for success or  < 0 on failure.
 */
static int exec_wait_kill_children (List exec_wait_list)
{
	int rc = 0;
	int count;
	struct exec_wait_info *e;
	ListIterator i;

	if ((count = list_count (exec_wait_list)) == 0)
		return (0);

	verbose ("Killing %d remaining child%s",
		 count, (count > 1 ? "ren" : ""));

	i = list_iterator_create (exec_wait_list);
	if (i == NULL)
		return error ("exec_wait_kill_children: iterator_create: %m");

	while ((e = list_next (i)))
		rc += exec_wait_kill_child (e);
	list_iterator_destroy (i);
	return (rc);
}

static void prepare_stdio (slurmd_job_t *job, slurmd_task_info_t *task)
{
#ifdef HAVE_PTY_H
	if (job->pty && (task->gtid == 0)) {
		if (login_tty(task->stdin_fd))
			error("login_tty: %m");
		else
			debug3("login_tty good");
		return;
	}
#endif
	io_dup_stdio(task);
	return;
}

static void _unblock_signals (void)
{
	sigset_t set;
	int i;

	for (i = 0; mgr_sigarray[i]; i++) {
		/* eliminate pending signals, then set to default */
		xsignal(mgr_sigarray[i], SIG_IGN);
		xsignal(mgr_sigarray[i], SIG_DFL);
	}
	sigemptyset(&set);
	xsignal_set_mask (&set);
}

/* fork and exec N tasks
 */
static int
_fork_all_tasks(slurmd_job_t *job, bool *io_initialized)
{
	int rc = SLURM_SUCCESS;
	int i;
	struct priv_state sprivs;
	jobacct_id_t jobacct_id;
	char *oom_value;
	List exec_wait_list = NULL;

	xassert(job != NULL);

	set_oom_adj(0);	/* the tasks may be killed by OOM */
	if (pre_setuid(job)) {
		error("Failed task affinity setup");
		return SLURM_ERROR;
	}

	/* Temporarily drop effective privileges, except for the euid.
	 * We need to wait until after pam_setup() to drop euid.
	 */
	if (_drop_privileges (job, false, &sprivs, true) < 0)
		return ESLURMD_SET_UID_OR_GID_ERROR;

	if (pam_setup(job->pwd->pw_name, conf->hostname)
	    != SLURM_SUCCESS){
		error ("error in pam_setup");
		rc = SLURM_ERROR;
	}

	/*
	 * Reclaim privileges to do the io setup
	 */
	_reclaim_privileges (&sprivs);
	if (rc)
		goto fail1; /* pam_setup error */

	set_umask(job);		/* set umask for stdout/err files */
	if (job->user_managed_io)
		rc = _setup_user_managed_io(job);
	else
		rc = _setup_normal_io(job);
	/*
	 * Initialize log facility to copy errors back to srun
	 */
	if (!rc)
		rc = _slurmd_job_log_init(job);

	if (rc) {
		error("IO setup failed: %m");
		job->task[0]->estatus = 0x0100;
		step_complete.step_rc = 0x0100;
		if (job->batch)
			rc = SLURM_SUCCESS;	/* drains node otherwise */
		goto fail1;
	} else {
		*io_initialized = true;
	}

	/*
	 * Temporarily drop effective privileges
	 */
	if (_drop_privileges (job, true, &sprivs, true) < 0) {
		error ("_drop_privileges: %m");
		rc = SLURM_ERROR;
		goto fail2;
	}

	if (chdir(job->cwd) < 0) {
		error("couldn't chdir to `%s': %m: going to /tmp instead",
		      job->cwd);
		if (chdir("/tmp") < 0) {
			error("couldn't chdir to /tmp either. dying.");
			rc = SLURM_ERROR;
			goto fail3;
		}
	}

	if (spank_user (job) < 0) {
		error("spank_user failed.");
		rc = SLURM_ERROR;
		goto fail4;
	}

	exec_wait_list = list_create ((ListDelF) exec_wait_info_destroy);
	if (!exec_wait_list) {
		error ("Unable to create exec_wait_list");
		rc = SLURM_ERROR;
		goto fail4;
	}

	/*
	 * Fork all of the task processes.
	 */
	for (i = 0; i < job->node_tasks; i++) {
		char time_stamp[256];
		pid_t pid;
		struct exec_wait_info *ei;

		acct_gather_profile_g_task_start(i);
		if ((ei = fork_child_with_wait_info (i)) == NULL) {
			error("child fork: %m");
			exec_wait_kill_children (exec_wait_list);
			rc = SLURM_ERROR;
			goto fail4;
		} else if ((pid = exec_wait_get_pid (ei)) == 0)  { /* child */
			/*
			 *  Destroy exec_wait_list in the child.
			 *   Only exec_wait_info for previous tasks have been
			 *   added to the list so far, so everything else
			 *   can be discarded.
			 */
			list_destroy (exec_wait_list);

#ifdef HAVE_AIX
			(void) mkcrid(0);
#endif
			/* jobacctinfo_endpoll();
			 * closing jobacct files here causes deadlock */

			if (conf->propagate_prio)
				_set_prio_process(job);

			/*
			 *  Reclaim privileges and call any plugin hooks
			 *   that may require elevated privs
			 */
			if (_pre_task_privileged(job, i, &sprivs) < 0)
				exit(1);

 			if (_become_user(job, &sprivs) < 0) {
 				error("_become_user failed: %m");
				/* child process, should not return */
				exit(1);
 			}

			/* log_fini(); */ /* note: moved into exec_task() */

			_unblock_signals();

			/*
			 *  Need to setup stdio before setpgid() is called
			 *   in case we are setting up a tty. (login_tty()
			 *   must be called before setpgid() or it is
			 *   effectively disabled).
			 */
			prepare_stdio (job, job->task[i]);

			/*
			 *  Block until parent notifies us that it is ok to
			 *   proceed. This allows the parent to place all
			 *   children in any process groups or containers
			 *   before they make a call to exec(2).
			 */
			if (exec_wait_child_wait_for_parent (ei) < 0)
				exit (1);

			exec_task(job, i);
		}

		/*
		 * Parent continues:
		 */

		list_append (exec_wait_list, ei);

		log_timestamp(time_stamp, sizeof(time_stamp));
		verbose ("task %lu (%lu) started %s",
			 (unsigned long) job->task[i]->gtid,
			 (unsigned long) pid, time_stamp);

		job->task[i]->pid = pid;
		if (i == 0)
			job->pgid = pid;
	}

	/*
	 * All tasks are now forked and running as the user, but
	 * will wait for our signal before calling exec.
	 */

	/*
	 * Reclaim privileges
	 */
	if (_reclaim_privileges (&sprivs) < 0) {
		error ("Unable to reclaim privileges");
		/* Don't bother erroring out here */
	}

	if ((oom_value = getenv("SLURMSTEPD_OOM_ADJ"))) {
		int i = atoi(oom_value);
		debug("Setting slurmstepd oom_adj to %d", i);
		set_oom_adj(i);
	}

	if (chdir (sprivs.saved_cwd) < 0) {
		error ("Unable to return to working directory");
	}

	for (i = 0; i < job->node_tasks; i++) {
		/*
		 * Put this task in the step process group
		 * login_tty() must put task zero in its own
		 * session, causing setpgid() to fail, setsid()
		 * has already set its process group as desired
		 */
		if ((job->pty == 0)
		    &&  (setpgid (job->task[i]->pid, job->pgid) < 0)) {
			error("Unable to put task %d (pid %d) into "
			      "pgrp %d: %m",
			      i,
			      job->task[i]->pid,
			      job->pgid);
		}

		if (slurm_container_add(job, job->task[i]->pid)
		    == SLURM_ERROR) {
			error("slurm_container_add: %m");
			rc = SLURM_ERROR;
			goto fail2;
		}
		jobacct_id.nodeid = job->nodeid;
		jobacct_id.taskid = job->task[i]->gtid;
		jobacct_id.job    = job;
		if (i == (job->node_tasks - 1)) {
			/* start polling on the last task */
			jobacct_gather_set_proctrack_container_id(job->cont_id);
			jobacct_gather_add_task(job->task[i]->pid, &jobacct_id,
						1);
		} else {
			/* don't poll yet */
			jobacct_gather_add_task(job->task[i]->pid, &jobacct_id,
						0);
		}
		if (spank_task_post_fork (job, i) < 0) {
			error ("spank task %d post-fork failed", i);
			rc = SLURM_ERROR;
			goto fail2;
		}
	}
//	jobacct_gather_set_proctrack_container_id(job->cont_id);

	/*
	 * Now it's ok to unblock the tasks, so they may call exec.
	 */
	list_for_each (exec_wait_list, (ListForF) exec_wait_signal, job);
	list_destroy (exec_wait_list);

	for (i = 0; i < job->node_tasks; i++) {
		/*
		 * Prepare process for attach by parallel debugger
		 * (if specified and able)
		 */
		if (pdebug_trace_process(job, job->task[i]->pid)
		    == SLURM_ERROR)
			rc = SLURM_ERROR;
	}

	return rc;

fail4:
	if (chdir (sprivs.saved_cwd) < 0) {
		error ("Unable to return to working directory");
	}
fail3:
	_reclaim_privileges (&sprivs);
	if (exec_wait_list)
		list_destroy (exec_wait_list);
fail2:
	io_close_task_fds(job);
fail1:
	pam_finish();
	return rc;
}

/*
 * Loop once through tasks looking for all tasks that have exited with
 * the same exit status (and whose statuses have not been sent back to
 * the client) Aggregate these tasks into a single task exit message.
 *
 */
static int
_send_pending_exit_msgs(slurmd_job_t *job)
{
	int  i;
	int  nsent  = 0;
	int  status = 0;
	bool set    = false;
	uint32_t  tid[job->node_tasks];

	/*
	 * Collect all exit codes with the same status into a
	 * single message.
	 */
	for (i = 0; i < job->node_tasks; i++) {
		slurmd_task_info_t *t = job->task[i];

		if (!t->exited || t->esent)
			continue;

		if (!set) {
			status = t->estatus;
			set    = true;
		} else if (status != t->estatus)
			continue;

		tid[nsent++] = t->gtid;
		t->esent = true;
	}

	if (nsent) {
		debug2("Aggregated %d task exit messages", nsent);
		_send_exit_msg(job, tid, nsent, status);
	}

	return nsent;
}

static inline void
_log_task_exit(unsigned long taskid, unsigned long pid, int status)
{
	/*
	 *  Print a nice message to the log describing the task exit status.
	 *
	 *  The final else is there just in case there is ever an exit status
	 *   that isn't WIFEXITED || WIFSIGNALED. We'll probably never reach
	 *   that code, but it is better than dropping a potentially useful
	 *   exit status.
	 */
	if (WIFEXITED(status))
		verbose("task %lu (%lu) exited with exit code %d.",
			taskid, pid, WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		/* WCOREDUMP isn't available on AIX */
		verbose("task %lu (%lu) exited. Killed by signal %d%s.",
			taskid, pid, WTERMSIG(status),
#ifdef WCOREDUMP
			WCOREDUMP(status) ? " (core dumped)" : ""
#else
			""
#endif
			);
	else
		verbose("task %lu (%lu) exited with status 0x%04x.",
			taskid, pid, status);
}

/*
 * If waitflag is true, perform a blocking wait for a single process
 * and then return.
 *
 * If waitflag is false, do repeated non-blocking waits until
 * there are no more processes to reap (waitpid returns 0).
 *
 * Returns the number of tasks for which a wait3() was successfully
 * performed, or -1 if there are no child tasks.
 */
static int
_wait_for_any_task(slurmd_job_t *job, bool waitflag)
{
	slurmd_task_info_t *t = NULL;
	int status = 0;
	pid_t pid;
	int completed = 0;
	jobacctinfo_t *jobacct = NULL;
	struct rusage rusage;
	do {
		pid = wait3(&status, waitflag ? 0 : WNOHANG, &rusage);
		if (pid == -1) {
			if (errno == ECHILD) {
				debug("No child processes");
				if (completed == 0)
					completed = -1;
				goto done;
			} else if (errno == EINTR) {
				debug("wait3 was interrupted");
				continue;
			} else {
				debug("Unknown errno %d", errno);
				continue;
			}
		} else if (pid == 0) { /* WNOHANG and no pids available */
			goto done;
		}

		/************* acct stuff ********************/
		jobacct = jobacct_gather_remove_task(pid);
		if (jobacct) {
			jobacctinfo_setinfo(jobacct,
					    JOBACCT_DATA_RUSAGE, &rusage,
					    SLURM_PROTOCOL_VERSION);
			/* Since we currently don't track energy
			   usage per task (only per step).  We take
			   into account only the last poll of the last task.
			   Odds are it is the only one with
			   information anyway, but just to be safe we
			   will zero out the previous value since this
			   one will over ride it.
			   If this ever changes in the future this logic
			   will need to change.
			*/
			if (jobacct->energy.consumed_energy)
				job->jobacct->energy.consumed_energy = 0;
			jobacctinfo_aggregate(job->jobacct, jobacct);
			jobacctinfo_destroy(jobacct);
		}
		acct_gather_profile_g_task_end(pid);
		/*********************************************/

		if ((t = job_task_info_by_pid(job, pid))) {
			completed++;
			_log_task_exit(t->gtid, pid, status);

			t->exited  = true;
			t->estatus = status;
			job->envtp->env = job->env;
			job->envtp->procid = t->gtid;
			job->envtp->localid = t->id;

			job->envtp->distribution = -1;
			job->envtp->batch_flag = job->batch;
			setup_env(job->envtp, false);
			job->env = job->envtp->env;
			if (job->task_epilog) {
				_run_script_as_user("user task_epilog",
						    job->task_epilog,
						    job, 5, job->env);
			}
			if (conf->task_epilog) {
				char *my_epilog;
				slurm_mutex_lock(&conf->config_mutex);
				my_epilog = xstrdup(conf->task_epilog);
				slurm_mutex_unlock(&conf->config_mutex);
				_run_script_as_user("slurm task_epilog",
						    my_epilog,
						    job, -1, job->env);
				xfree(my_epilog);
			}
			job->envtp->procid = t->id;

			if (spank_task_exit (job, t->id) < 0) {
				error ("Unable to spank task %d at exit",
				       t->id);
			}
			post_term(job);
		}

	} while ((pid > 0) && !waitflag);

done:
	return completed;
}

static void
_wait_for_all_tasks(slurmd_job_t *job)
{
	int tasks_left = 0;
	int i;

	for (i = 0; i < job->node_tasks; i++) {
		if (job->task[i]->state < SLURMD_TASK_COMPLETE) {
			tasks_left++;
		}
	}
	if (tasks_left < job->node_tasks)
		verbose("Only %d of %d requested tasks successfully launched",
			tasks_left, job->node_tasks);

	for (i = 0; i < tasks_left; ) {
		int rc;
		rc = _wait_for_any_task(job, true);
		if (rc != -1) {
			i += rc;
			if (i < tasks_left) {
				/* To limit the amount of traffic back
				 * we will sleep a bit to make sure we
				 * have most if not all the tasks
				 * completed before we return */
				usleep(100000);	/* 100 msec */
				rc = _wait_for_any_task(job, false);
				if (rc != -1)
					i += rc;
			}
		}

		while (_send_pending_exit_msgs(job)) {;}
	}
}

static void *_kill_thr(void *args)
{
	kill_thread_t *kt = ( kill_thread_t *) args;
	unsigned int pause = kt->secs;
	do {
		pause = sleep(pause);
	} while (pause > 0);
	pthread_cancel(kt->thread_id);
	xfree(kt);
	return NULL;
}

static void _delay_kill_thread(pthread_t thread_id, int secs)
{
	pthread_t kill_id;
	pthread_attr_t attr;
	kill_thread_t *kt = xmalloc(sizeof(kill_thread_t));
	int retries = 0;

	kt->thread_id = thread_id;
	kt->secs = secs;
	slurm_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	while (pthread_create(&kill_id, &attr, &_kill_thr, (void *) kt)) {
		error("_delay_kill_thread: pthread_create: %m");
		if (++retries > MAX_RETRIES) {
			error("_delay_kill_thread: Can't create pthread");
			break;
		}
		usleep(10);	/* sleep and again */
	}
	slurm_attr_destroy(&attr);
}

/*
 * Wait for IO
 */
static void
_wait_for_io(slurmd_job_t *job)
{
	debug("Waiting for IO");
	io_close_all(job);

	/*
	 * Wait until IO thread exits or kill it after 300 seconds
	 */
	if (job->ioid) {
		_delay_kill_thread(job->ioid, 300);
		pthread_join(job->ioid, NULL);
	} else
		info("_wait_for_io: ioid==0");

	/* Close any files for stdout/stderr opened by the stepd */
	io_close_local_fds(job);

	return;
}


static char *
_make_batch_dir(slurmd_job_t *job)
{
	char path[MAXPATHLEN];

	if (job->stepid == NO_VAL)
		snprintf(path, sizeof(path), "%s/job%05u",
			 conf->spooldir, job->jobid);
	else {
		snprintf(path, sizeof(path), "%s/job%05u.%05u",
			 conf->spooldir, job->jobid, job->stepid);
	}

	if ((mkdir(path, 0750) < 0) && (errno != EEXIST)) {
		error("mkdir(%s): %m", path);
		if (errno == ENOSPC)
			_drain_node("SlurmdSpoolDir is full");
		goto error;
	}

	if (chown(path, (uid_t) -1, (gid_t) job->pwd->pw_gid) < 0) {
		error("chown(%s): %m", path);
		goto error;
	}

	if (chmod(path, 0750) < 0) {
		error("chmod(%s, 750): %m", path);
		goto error;
	}

	return xstrdup(path);

error:
	return NULL;
}

static char *
_make_batch_script(batch_job_launch_msg_t *msg, char *path)
{
	FILE *fp = NULL;
	char  script[MAXPATHLEN];

	if (msg->script == NULL) {
		error("_make_batch_script: called with NULL script");
		return NULL;
	}

	snprintf(script, sizeof(script), "%s/%s", path, "slurm_script");

again:
	if ((fp = safeopen(script, "w", SAFEOPEN_CREATE_ONLY)) == NULL) {
		if ((errno != EEXIST) || (unlink(script) < 0))  {
			error("couldn't open `%s': %m", script);
			goto error;
		}
		goto again;
	}

	if (fputs(msg->script, fp) < 0) {
		(void) fclose(fp);
		error("fputs: %m");
		if (errno == ENOSPC)
			_drain_node("SlurmdSpoolDir is full");
		goto error;
	}

	if (fclose(fp) < 0) {
		error("fclose: %m");
	}

	if (chown(script, (uid_t) msg->uid, (gid_t) -1) < 0) {
		error("chown(%s): %m", path);
		goto error;
	}

	if (chmod(script, 0500) < 0) {
		error("chmod: %m");
	}

	return xstrdup(script);

error:
	(void) unlink(script);
	return NULL;

}

static int _drain_node(char *reason)
{
	slurm_msg_t req_msg;
	update_node_msg_t update_node_msg;

	memset(&update_node_msg, 0, sizeof(update_node_msg_t));
	update_node_msg.node_names = conf->node_name;
	update_node_msg.node_state = NODE_STATE_DRAIN;
	update_node_msg.reason = reason;
	update_node_msg.reason_uid = getuid();
	update_node_msg.weight = NO_VAL;
	slurm_msg_t_init(&req_msg);
	req_msg.msg_type = REQUEST_UPDATE_NODE;
	req_msg.data = &update_node_msg;

	if (slurm_send_only_controller_msg(&req_msg) < 0)
		return SLURM_ERROR;

	return SLURM_SUCCESS;
}

inline static int
_send_launch_resp_msg(slurm_msg_t *resp_msg, uint32_t nnodes)
{
	int rc, retry = 0, max_retry;
	unsigned long delay = 100000;

	max_retry = (nnodes / 1024) + 1;
	while (1) {
		rc = slurm_send_only_node_msg(resp_msg);
		if ((rc == SLURM_SUCCESS) ||
		    (errno != ETIMEDOUT) ||
		    (retry > max_retry))
			break;
		debug3("_send_launch_resp_msg: failed to send msg: %m");
		usleep(delay);
		if (delay < 800000)
			delay *= 2;
		retry ++;
	}
	return rc;
}

static void
_send_launch_failure (launch_tasks_request_msg_t *msg, slurm_addr_t *cli, int rc)
{
	slurm_msg_t resp_msg;
	launch_tasks_response_msg_t resp;
	int nodeid;
	char *name = NULL;

#ifndef HAVE_FRONT_END
	nodeid = nodelist_find(msg->complete_nodelist, conf->node_name);
	name = xstrdup(conf->node_name);
#else
	nodeid = 0;
	name = xstrdup(msg->complete_nodelist);
#endif
	debug ("sending launch failure message: %s", slurm_strerror (rc));

	slurm_msg_t_init(&resp_msg);
	memcpy(&resp_msg.address, cli, sizeof(slurm_addr_t));
	slurm_set_addr(&resp_msg.address,
		       msg->resp_port[nodeid % msg->num_resp_port],
		       NULL);
	resp_msg.data = &resp;
	resp_msg.msg_type = RESPONSE_LAUNCH_TASKS;

	resp.node_name     = name;
	resp.return_code   = rc ? rc : -1;
	resp.count_of_pids = 0;

	if (_send_launch_resp_msg(&resp_msg, msg->nnodes) != SLURM_SUCCESS)
		error("Failed to send RESPONSE_LAUNCH_TASKS: %m");
	xfree(name);
	return;
}

static void
_send_launch_resp(slurmd_job_t *job, int rc)
{
	int i;
	slurm_msg_t resp_msg;
	launch_tasks_response_msg_t resp;
	srun_info_t *srun = list_peek(job->sruns);

	if (job->batch)
		return;

	debug("Sending launch resp rc=%d", rc);

	slurm_msg_t_init(&resp_msg);
	resp_msg.address	= srun->resp_addr;
	resp_msg.data		= &resp;
	resp_msg.msg_type	= RESPONSE_LAUNCH_TASKS;

	resp.node_name		= xstrdup(job->node_name);
	resp.return_code	= rc;
	resp.count_of_pids	= job->node_tasks;

	resp.local_pids = xmalloc(job->node_tasks * sizeof(*resp.local_pids));
	resp.task_ids = xmalloc(job->node_tasks * sizeof(*resp.task_ids));
	for (i = 0; i < job->node_tasks; i++) {
		resp.local_pids[i] = job->task[i]->pid;
		resp.task_ids[i] = job->task[i]->gtid;
	}

	if (_send_launch_resp_msg(&resp_msg, job->nnodes) != SLURM_SUCCESS)
		error("failed to send RESPONSE_LAUNCH_TASKS: %m");

	xfree(resp.local_pids);
	xfree(resp.task_ids);
	xfree(resp.node_name);
}


static int
_send_complete_batch_script_msg(slurmd_job_t *job, int err, int status)
{
	int		rc, i, msg_rc;
	slurm_msg_t	req_msg;
	complete_batch_script_msg_t req;
	char *		select_type;
	bool		msg_to_ctld;

	select_type = slurm_get_select_type();
	msg_to_ctld = strcmp(select_type, "select/serial");
	xfree(select_type);

	req.job_id	= job->jobid;
	req.job_rc      = status;
	req.jobacct	= job->jobacct;
	req.node_name	= job->node_name;
	req.slurm_rc	= err;
	req.user_id	= (uint32_t) job->uid;
	slurm_msg_t_init(&req_msg);
	req_msg.msg_type= REQUEST_COMPLETE_BATCH_SCRIPT;
	req_msg.data	= &req;

	info("sending REQUEST_COMPLETE_BATCH_SCRIPT, error:%u", err);

	/* Note: these log messages don't go to slurmd.log from here */
	for (i = 0; i <= MAX_RETRY; i++) {
		if (msg_to_ctld) {
			msg_rc = slurm_send_recv_controller_rc_msg(&req_msg,
								   &rc);
		} else {
			if (i == 0) {
				slurm_set_addr_char(&req_msg.address,
						    conf->port, "localhost");
			}
			msg_rc = slurm_send_recv_rc_msg_only_one(&req_msg,
								 &rc, 0);
		}
		if (msg_rc == SLURM_SUCCESS)
			break;
		info("Retrying job complete RPC for %u.%u",
		     job->jobid, job->stepid);
		sleep(RETRY_DELAY);
	}
	if (i > MAX_RETRY) {
		error("Unable to send job complete message: %m");
		return SLURM_ERROR;
	}

	if ((rc == ESLURM_ALREADY_DONE) || (rc == ESLURM_INVALID_JOB_ID))
		rc = SLURM_SUCCESS;
	if (rc)
		slurm_seterrno_ret(rc);

	return SLURM_SUCCESS;
}


static int
_drop_privileges(slurmd_job_t *job, bool do_setuid,
		 struct priv_state *ps, bool get_list)
{
	ps->saved_uid = getuid();
	ps->saved_gid = getgid();

	if (!getcwd (ps->saved_cwd, sizeof (ps->saved_cwd))) {
		error ("Unable to get current working directory: %m");
		strncpy (ps->saved_cwd, "/tmp", sizeof (ps->saved_cwd));
	}

	ps->ngids = getgroups(0, NULL);
	if (get_list) {
		ps->gid_list = (gid_t *) xmalloc(ps->ngids * sizeof(gid_t));

		if (getgroups(ps->ngids, ps->gid_list) == -1) {
			error("_drop_privileges: couldn't get %d groups: %m",
			      ps->ngids);
			xfree(ps->gid_list);
			return -1;
		}
	}

	/*
	 * No need to drop privileges if we're not running as root
	 */
	if (getuid() != (uid_t) 0)
		return SLURM_SUCCESS;

	if (setegid(job->pwd->pw_gid) < 0) {
		error("setegid: %m");
		return -1;
	}

	if (_initgroups(job) < 0) {
		error("_initgroups: %m");
	}

	if (do_setuid && seteuid(job->pwd->pw_uid) < 0) {
		error("seteuid: %m");
		return -1;
	}

	return SLURM_SUCCESS;
}

static int
_reclaim_privileges(struct priv_state *ps)
{
	int rc = SLURM_SUCCESS;

	/*
	 * No need to reclaim privileges if our uid == pwd->pw_uid
	 */
	if (geteuid() == ps->saved_uid)
		goto done;
	else if (seteuid(ps->saved_uid) < 0) {
		error("seteuid: %m");
		rc = -1;
	} else if (setegid(ps->saved_gid) < 0) {
		error("setegid: %m");
		rc = -1;
	} else
		setgroups(ps->ngids, ps->gid_list);
done:
	xfree(ps->gid_list);

	return rc;
}


static int
_slurmd_job_log_init(slurmd_job_t *job)
{
	char argv0[64];

	conf->log_opts.buffered = 1;

	/*
	 * Reset stderr logging to user requested level
	 * (Logfile and syslog levels remain the same)
	 *
	 * The maximum stderr log level is LOG_LEVEL_DEBUG3 because
	 * some higher level debug messages are generated in the
	 * stdio code, which would otherwise create more stderr traffic
	 * to srun and therefore more debug messages in an endless loop.
	 */
	conf->log_opts.stderr_level = LOG_LEVEL_ERROR + job->debug;
	if (conf->log_opts.stderr_level > LOG_LEVEL_DEBUG3)
		conf->log_opts.stderr_level = LOG_LEVEL_DEBUG3;

	snprintf(argv0, sizeof(argv0), "slurmd[%s]", conf->node_name);
	/*
	 * reinitialize log
	 */

	log_alter(conf->log_opts, 0, NULL);
	log_set_argv0(argv0);

	/*  Connect slurmd stderr to stderr of job, unless we are using
	 *   user_managed_io or a pty.
	 *
	 *  user_managed_io directly connects the client (e.g. poe) to the tasks
	 *   over a TCP connection, and we fully leave it up to the client
	 *   to manage the stream with no buffering on slurm's part.
	 *   We also promise that we will not insert any foreign data into
	 *   the stream, so here we need to avoid connecting slurmstepd's
	 *   STDERR_FILENO to the tasks's stderr.
	 *
	 *  When pty terminal emulation is used, the pts can potentially
	 *   cause IO to block, so we need to avoid connecting slurmstepd's
	 *   STDERR_FILENO to the task's pts on stderr to avoid hangs in
	 *   the slurmstepd.
	 */
	if (!job->user_managed_io && !job->pty && job->task != NULL) {
		if (dup2(job->task[0]->stderr_fd, STDERR_FILENO) < 0) {
			error("job_log_init: dup2(stderr): %m");
			return ESLURMD_IO_ERROR;
		}
	}
	verbose("debug level = %d", conf->log_opts.stderr_level);
	return SLURM_SUCCESS;
}


static void
_setargs(slurmd_job_t *job)
{
	if (job->jobid > MAX_NOALLOC_JOBID)
		return;

	if ((job->jobid >= MIN_NOALLOC_JOBID) || (job->stepid == NO_VAL))
		setproctitle("[%u]",    job->jobid);
	else
		setproctitle("[%u.%u]", job->jobid, job->stepid);

	return;
}

/*
 * Set the priority of the job to be the same as the priority of
 * the process that launched the job on the submit node.
 * In support of the "PropagatePrioProcess" config keyword.
 */
static void _set_prio_process (slurmd_job_t *job)
{
	char *env_name = "SLURM_PRIO_PROCESS";
	char *env_val;
	int prio_daemon, prio_process;

	if (!(env_val = getenvp( job->env, env_name ))) {
		error( "Couldn't find %s in environment", env_name );
		prio_process = 0;
	} else {
		/* Users shouldn't get this in their environment */
		unsetenvp( job->env, env_name );
		prio_process = atoi( env_val );
	}

	if (conf->propagate_prio == PROP_PRIO_NICER) {
		prio_daemon = getpriority( PRIO_PROCESS, 0 );
		prio_process = MAX( prio_process, (prio_daemon + 1) );
	}

	if (setpriority( PRIO_PROCESS, 0, prio_process ))
		error( "setpriority(PRIO_PROCESS, %d): %m", prio_process );
	else {
		debug2( "_set_prio_process: setpriority %d succeeded",
			prio_process);
	}
}

static int
_become_user(slurmd_job_t *job, struct priv_state *ps)
{
	/*
	 * First reclaim the effective uid and gid
	 */
	if (geteuid() == ps->saved_uid)
		return SLURM_SUCCESS;

	if (seteuid(ps->saved_uid) < 0) {
		error("_become_user seteuid: %m");
		return SLURM_ERROR;
	}

	if (setegid(ps->saved_gid) < 0) {
		error("_become_user setegid: %m");
		return SLURM_ERROR;
	}

	/*
	 * Now drop real, effective, and saved uid/gid
	 */
	if (setregid(job->pwd->pw_gid, job->pwd->pw_gid) < 0) {
		error("setregid: %m");
		return SLURM_ERROR;
	}

	if (setreuid(job->pwd->pw_uid, job->pwd->pw_uid) < 0) {
		error("setreuid: %m");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}


static int
_initgroups(slurmd_job_t *job)
{
	int rc;
	char *username;
	gid_t gid;

	if (job->ngids > 0) {
		xassert(job->gids);
		debug2("Using gid list sent by slurmd");
		return setgroups(job->ngids, job->gids);
	}

	username = job->pwd->pw_name;
	gid = job->pwd->pw_gid;
	debug2("Uncached user/gid: %s/%ld", username, (long)gid);
	if ((rc = initgroups(username, gid))) {
		if ((errno == EPERM) && (getuid() != (uid_t) 0)) {
			debug("Error in initgroups(%s, %ld): %m",
			      username, (long)gid);
		} else {
			error("Error in initgroups(%s, %ld): %m",
			      username, (long)gid);
		}
		return -1;
	}
	return 0;
}

/*
 * Check this user's access rights to a file
 * path IN: pathname of file to test
 * modes IN: desired access
 * uid IN: user ID to access the file
 * gid IN: group ID to access the file
 * RET 0 on success, -1 on failure
 */
static int _access(const char *path, int modes, uid_t uid, gid_t gid)
{
	struct stat buf;
	int f_mode;

	if (stat(path, &buf) != 0)
		return -1;

	if (buf.st_uid == uid)
		f_mode = (buf.st_mode >> 6) & 07;
	else if (buf.st_gid == gid)
		f_mode = (buf.st_mode >> 3) & 07;
	else
		f_mode = buf.st_mode & 07;

	if ((f_mode & modes) == modes)
		return 0;
	return -1;
}

/*
 * Run a script as a specific user, with the specified uid, gid, and
 * extended groups.
 *
 * name IN: class of program (task prolog, task epilog, etc.),
 * path IN: pathname of program to run
 * job IN: slurd job structue, used to get uid, gid, and groups
 * max_wait IN: maximum time to wait in seconds, -1 for no limit
 * env IN: environment variables to use on exec, sets minimal environment
 *	if NULL
 *
 * RET 0 on success, -1 on failure.
 */
int
_run_script_as_user(const char *name, const char *path, slurmd_job_t *job,
		    int max_wait, char **env)
{
	int status, rc, opt;
	pid_t cpid;
	struct exec_wait_info *ei;

	xassert(env);
	if (path == NULL || path[0] == '\0')
		return 0;

	debug("[job %u] attempting to run %s [%s]", job->jobid, name, path);

	if (_access(path, 5, job->pwd->pw_uid, job->pwd->pw_gid) < 0) {
		error("Could not run %s [%s]: access denied", name, path);
		return -1;
	}

	if ((job->cont_id == 0) &&
	    (slurm_container_create(job) != SLURM_SUCCESS))
		error("slurm_container_create: %m");

	if ((ei = fork_child_with_wait_info(0)) == NULL) {
		error ("executing %s: fork: %m", name);
		return -1;
	}
	if ((cpid = exec_wait_get_pid (ei)) == 0) {
		struct priv_state sprivs;
		char *argv[2];

		argv[0] = (char *)xstrdup(path);
		argv[1] = NULL;

		if (_drop_privileges(job, true, &sprivs, false) < 0) {
			error("run_script_as_user _drop_privileges: %m");
			/* child process, should not return */
			exit(127);
		}

		if (_become_user(job, &sprivs) < 0) {
			error("run_script_as_user _become_user failed: %m");
			/* child process, should not return */
			exit(127);
		}

		if (chdir(job->cwd) == -1)
			error("run_script_as_user: couldn't "
			      "change working dir to %s: %m", job->cwd);
#ifdef SETPGRP_TWO_ARGS
		setpgrp(0, 0);
#else
		setpgrp();
#endif
		/*
		 *  Wait for signal from parent
		 */
		exec_wait_child_wait_for_parent (ei);

		execve(path, argv, env);
		error("execve(): %m");
		exit(127);
	}

	if (slurm_container_add(job, cpid) != SLURM_SUCCESS)
		error("slurm_container_add: %m");

	if (exec_wait_signal_child (ei) < 0)
		error ("run_script_as_user: Failed to wakeup %s", name);
	exec_wait_info_destroy (ei);

	if (max_wait < 0)
		opt = 0;
	else
		opt = WNOHANG;

	while (1) {
		rc = waitpid(cpid, &status, opt);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			error("waidpid: %m");
			status = 0;
			break;
		} else if (rc == 0) {
			sleep(1);
			if ((--max_wait) <= 0) {
				killpg(cpid, SIGKILL);
				opt = 0;
			}
		} else  {
			/* spawned process exited */
			break;
		}
	}
	/* Insure that all child processes get killed, one last time */
	killpg(cpid, SIGKILL);

	return status;
}

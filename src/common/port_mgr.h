/*****************************************************************************\
 *  port_mgr.h - manage the reservation of I/O ports on the nodes.
 *	Design for use with OpenMPI.
 *****************************************************************************
 *  Copyright (C) 2009 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
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

#ifndef _HAVE_PORT_MGR_H
#define _HAVE_PORT_MGR_H

/* Configure reserved ports.
 * Call with mpi_params==NULL to free memory */
extern int reserve_port_config(char *mpi_params, list_t *job_list);

/*
 * Configure reserved ports passed on job's resv_ports and resv_port_cnt
 * Call with job_ptr->resv_ports == NULL to free memory.
 * This should only be called by the slurmstepd when acting as step manager.
 */
extern int reserve_port_stepmgr_init(job_record_t *job_ptr);

/* Reserve ports for a job step
 * RET SLURM_SUCCESS or an error code */
extern int resv_port_step_alloc(step_record_t *step_ptr);

/*
 * Reserve ports for a job
 * Used when the step manager is enabled
 * RET SLURM_SUCCESS or an error code
 */
extern int resv_port_job_alloc(job_record_t *job_ptr);

/*
 * Verify that the requested resv_port_cnt is valid.
 */
extern int resv_port_check_job_request_cnt(job_record_t *job_ptr);

/* Release reserved ports for a job step
 * RET SLURM_SUCCESS or an error code */
extern void resv_port_step_free(step_record_t *step_ptr);

/*
 * Release reserved ports for a job
 * Used when the step manager is enabled
 * RET SLURM_SUCCESS or an error code
 */
extern void resv_port_job_free(job_record_t *job_ptr);

#endif	/* !_HAVE_PORT_MGR_H */

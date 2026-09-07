/*-------------------------------------------------------------------------
 *
 * subprocess.h
 *		Locating and running the pg_basebackup/pg_combinebackup binaries
 *		this tool now drives as subprocesses, instead of doing its own
 *		bulk file transfer.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pg_upgrade_replica/subprocess.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGUR_SUBPROCESS_H
#define PGUR_SUBPROCESS_H

extern char *find_sibling_exec(const char *argv0, const char *progname);
extern void run_pg_tool(const char *cmd);
extern char *run_pg_tool_capture(const char *cmd);

#endif							/* PGUR_SUBPROCESS_H */

/*-------------------------------------------------------------------------
 *
 * subprocess.c
 *		Locating and running the pg_basebackup/pg_combinebackup binaries
 *		this tool now drives as subprocesses. Follows the same pattern
 *		pg_createsubscriber and pg_upgrade already use for shelling out to
 *		a sibling frontend tool: find_other_exec() to locate and
 *		version-check the binary, system() to run it. Callers build each
 *		command line with appendShellString() (fe_utils/string_utils.h),
 *		the same properly shell-quoting helper pg_createsubscriber and
 *		pg_rewind already use for their own subprocess arguments, rather
 *		than a bare double-quoted %s.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pg_upgrade_replica/subprocess.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "common/logging.h"
#include "lib/stringinfo.h"
#include "port.h"

#include "subprocess.h"

/*
 * Finds progname (e.g. "pg_basebackup") next to this tool's own binary,
 * and confirms it's the same PostgreSQL version. Returns its full path;
 * the caller owns the returned string.
 */
char *
find_sibling_exec(const char *argv0, const char *progname)
{
	char	   *versionstr;
	char	   *exec_path;
	int			ret;

	versionstr = psprintf("%s (PostgreSQL) %s\n", progname, PG_VERSION);
	exec_path = pg_malloc(MAXPGPATH);
	ret = find_other_exec(argv0, progname, versionstr, exec_path);

	if (ret < 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv0, full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		if (ret == -1)
			pg_fatal("program \"%s\" is needed by %s but was not found in "
					 "the same directory as \"%s\"",
					 progname, "pg_upgrade_replica", full_path);
		else
			pg_fatal("program \"%s\" was found by \"%s\" but was not the "
					 "same version as pg_upgrade_replica",
					 progname, full_path);
	}

	pg_free(versionstr);
	pg_log_debug("%s path is: %s", progname, exec_path);

	return exec_path;
}

/*
 * Runs an already-fully-quoted shell command, fatal on anything but a clean
 * exit. Output is not redirected: pg_basebackup/pg_combinebackup's own
 * progress and error messages go straight to this tool's stdout/stderr,
 * same as any other command a user runs interactively.
 */
void
run_pg_tool(const char *cmd)
{
	int			rc;

	pg_log_debug("running: %s", cmd);
	fflush(NULL);
	rc = system(cmd);
	if (rc != 0)
		pg_fatal("command failed: %s: %s", cmd, wait_result_to_str(rc));
}

/*
 * Like run_pg_tool(), but for a command whose stdout this tool needs to
 * read itself (pg_controldata's text output, in practice), rather than one
 * whose progress/error messages should just pass through to the user.
 * Returns the captured output; the caller owns the returned string.
 */
char *
run_pg_tool_capture(const char *cmd)
{
	FILE	   *fp;
	StringInfoData buf;
	char		chunk[4096];
	size_t		nread;
	int			rc;

	pg_log_debug("running: %s", cmd);
	fflush(NULL);
	fp = popen(cmd, "r");
	if (fp == NULL)
		pg_fatal("could not execute command \"%s\": %m", cmd);

	initStringInfo(&buf);
	while ((nread = fread(chunk, 1, sizeof(chunk), fp)) > 0)
		appendBinaryStringInfo(&buf, chunk, nread);

	rc = pclose(fp);
	if (rc != 0)
		pg_fatal("command failed: %s: %s", cmd, wait_result_to_str(rc));

	return buf.data;
}

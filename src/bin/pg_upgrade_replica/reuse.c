/*-------------------------------------------------------------------------
 *
 * reuse.c
 *		Assembles the new standby by forging a backup_manifest anchored at
 *		the new cluster's own checkpoint (see forge_manifest.h) and driving
 *		two already-hardened core tools from it: pg_basebackup --incremental
 *		fetches only what actually changed since that checkpoint (whole
 *		files for anything pg_upgrade rewrote, changed blocks only for
 *		anything it left alone but that was written to since), and
 *		pg_combinebackup reconstructs the final directory by combining that
 *		against --old-replica's own copy of every relation the manifest
 *		lists as unchanged.
 *
 * Bulk file transfer, tablespace placement, and backup_label/pg_control
 * handling for --new-replica itself are core's job here, the same code
 * every pg_basebackup/pg_combinebackup user already relies on. What
 * remains in this file: validating --old-replica is actually caught up,
 * presenting --old-replica's data under paths matching the *new*
 * cluster's layout so pg_combinebackup can find it
 * (build_old_replica_view(), needed only because tablespace directories
 * are named after the catalog version, and --old-replica's is
 * necessarily the old one), and recovery config
 * (standby.signal/primary_conninfo), which neither child tool has any
 * reason to know it should write.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pg_upgrade_replica/reuse.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "access/xlog_internal.h"
#include "catalog/catversion.h"
#include "catalog/pg_control.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "fe_utils/recovery_gen.h"
#include "fe_utils/string_utils.h"
#include "lib/stringinfo.h"
#include "port.h"

#include "fetch.h"
#include "forge_manifest.h"
#include "pg_upgrade_replica.h"
#include "subprocess.h"

/*
 * The work directory holds a full staged incremental backup; on any
 * pg_fatal() exit it would otherwise be left behind, multi-gigabyte and
 * orphaned, the same failure mode pg_basebackup's and pg_combinebackup's
 * own output-directory atexit cleanup exists to avoid. Cleared once the
 * normal, successful path has already removed it itself.
 */
static char *work_dir_to_cleanup = NULL;

static void
cleanup_work_dir_atexit(void)
{
	if (work_dir_to_cleanup == NULL)
		return;
	if (!rmtree(work_dir_to_cleanup, true))
		pg_log_warning("could not remove temporary directory \"%s\"",
					   work_dir_to_cleanup);
}

typedef struct TablespaceInfo
{
	Oid			oid;
	bool		in_place;		/* stored inside PGDATA itself, no separate
								 * location */
	char	   *primary_path;	/* as pg_tablespace_location() reports on the
								 * new primary */
	char	   *old_target;		/* --old-replica's own copy of this
								 * tablespace's data, or "" */
	char	   *chosen_path;	/* where this tablespace's data will live
								 * under --new-replica, unused when in_place */
} TablespaceInfo;

static const char *
find_tablespace_mapping(const SyncOptions *opts, const char *primary_path)
{
	for (TablespaceMapping *m = opts->tablespace_mappings; m != NULL; m = m->next)
	{
		if (strcmp(m->old_dir, primary_path) == 0)
			return m->new_dir;
	}
	return NULL;
}

/*
 * Appends path to buf with every literal "=" backslash-escaped, the
 * encoding side of the same OLDDIR=NEWDIR convention
 * add_tablespace_mapping()'s decoder (pg_upgrade_replica.c, mirroring
 * pg_basebackup's own tablespace_list_append()) expects on the way in.
 * Needed here because the OLDDIR/NEWDIR operands we build below come from
 * pg_tablespace_location() and --tablespace-mapping's own NEWDIR, neither
 * of which is guaranteed "=" free.
 */
static void
append_tablespace_mapping_operand(PQExpBuffer buf, const char *path)
{
	for (const char *p = path; *p; p++)
	{
		if (*p == '=')
			appendPQExpBufferChar(buf, '\\');
		appendPQExpBufferChar(buf, *p);
	}
}

/*
 * Reads --old-replica's own system identifier and latest checkpoint
 * location by shelling out to old_bindir's own pg_controldata, the same
 * pattern pg_upgrade itself uses (controldata.c) rather than parsing
 * pg_control as a struct: --old-replica is necessarily an *old* major
 * version's data directory (that's the whole point of this tool), and
 * pg_control's own binary layout is not guaranteed stable across major
 * versions -- get_controlfile(), compiled against this tool's own
 * version, cannot reliably parse a different one's. sscanf("%X/%X", ...)
 * for the LSN tolerates the zero-padding differences across
 * pg_controldata builds that a byte-for-byte struct read would not need
 * to worry about, but a text-parsing approach does.
 *
 * Nothing here verifies --old-bindir's own version actually matches
 * --old-replica's on-disk major version, the same way pg_upgrade itself
 * doesn't either (check_bin_dir() in exec.c only checks -V for the *new*
 * cluster's own bindir, never the old one's): a wrong --old-bindir would
 * need its own pg_controldata to coincidentally produce a sysid and LSN
 * matching the manifest to slip past check_old_replica_caught_up(), so
 * this is the same accepted trust in the operator's own arguments core
 * already extends on the old side.
 */
static void
get_old_replica_controldata(const char *old_bindir, const char *old_replica,
							uint64 *sysid, XLogRecPtr *chkpnt_loc)
{
	PQExpBuffer cmd;
	char	   *output;
	char	   *line;
	bool		got_sysid = false;
	bool		got_chkpnt = false;
	char	   *lc_collate = NULL;
	char	   *lc_ctype = NULL;
	char	   *lc_monetary = NULL;
	char	   *lc_numeric = NULL;
	char	   *lc_time = NULL;
	char	   *lang = NULL;
	char	   *language = NULL;
	char	   *lc_all = NULL;
	char	   *lc_messages = NULL;

	/*
	 * We test pg_controldata's output as English strings below, so it has to
	 * actually be in English. Same env-save/force-C/restore dance
	 * pg_upgrade's own get_control_data() (controldata.c) does around its own
	 * pg_controldata call, for the same reason.
	 */
	if (getenv("LC_COLLATE"))
		lc_collate = pg_strdup(getenv("LC_COLLATE"));
	if (getenv("LC_CTYPE"))
		lc_ctype = pg_strdup(getenv("LC_CTYPE"));
	if (getenv("LC_MONETARY"))
		lc_monetary = pg_strdup(getenv("LC_MONETARY"));
	if (getenv("LC_NUMERIC"))
		lc_numeric = pg_strdup(getenv("LC_NUMERIC"));
	if (getenv("LC_TIME"))
		lc_time = pg_strdup(getenv("LC_TIME"));
	if (getenv("LANG"))
		lang = pg_strdup(getenv("LANG"));
	if (getenv("LANGUAGE"))
		language = pg_strdup(getenv("LANGUAGE"));
	if (getenv("LC_ALL"))
		lc_all = pg_strdup(getenv("LC_ALL"));
	if (getenv("LC_MESSAGES"))
		lc_messages = pg_strdup(getenv("LC_MESSAGES"));

	unsetenv("LC_COLLATE");
	unsetenv("LC_CTYPE");
	unsetenv("LC_MONETARY");
	unsetenv("LC_NUMERIC");
	unsetenv("LC_TIME");
#ifndef WIN32
	unsetenv("LANG");
#else
	/* On Windows the default locale may not be English, so force it */
	setenv("LANG", "en", 1);
#endif
	unsetenv("LANGUAGE");
	unsetenv("LC_ALL");
	setenv("LC_MESSAGES", "C", 1);

	cmd = createPQExpBuffer();
	{
		char		pg_controldata_path[MAXPGPATH];

		snprintf(pg_controldata_path, sizeof(pg_controldata_path),
				 "%s/pg_controldata", old_bindir);
		appendShellString(cmd, pg_controldata_path);
	}
	appendPQExpBufferChar(cmd, ' ');
	appendShellString(cmd, old_replica);
	output = run_pg_tool_capture(cmd->data);
	destroyPQExpBuffer(cmd);

	if (lc_collate)
		setenv("LC_COLLATE", lc_collate, 1);
	if (lc_ctype)
		setenv("LC_CTYPE", lc_ctype, 1);
	if (lc_monetary)
		setenv("LC_MONETARY", lc_monetary, 1);
	if (lc_numeric)
		setenv("LC_NUMERIC", lc_numeric, 1);
	if (lc_time)
		setenv("LC_TIME", lc_time, 1);
	if (lang)
		setenv("LANG", lang, 1);
	else
		unsetenv("LANG");
	if (language)
		setenv("LANGUAGE", language, 1);
	if (lc_all)
		setenv("LC_ALL", lc_all, 1);
	if (lc_messages)
		setenv("LC_MESSAGES", lc_messages, 1);
	else
		unsetenv("LC_MESSAGES");

	pg_free(lc_collate);
	pg_free(lc_ctype);
	pg_free(lc_monetary);
	pg_free(lc_numeric);
	pg_free(lc_time);
	pg_free(lang);
	pg_free(language);
	pg_free(lc_all);
	pg_free(lc_messages);

	for (line = strtok(output, "\n"); line != NULL; line = strtok(NULL, "\n"))
	{
		char	   *p;

		if ((p = strstr(line, "Database system identifier:")) != NULL)
		{
			p = strchr(p, ':');
			if (p == NULL || strlen(p) <= 1)
				pg_fatal("could not parse system identifier reported by "
						 "\"%s/pg_controldata\" for \"%s\"", old_bindir, old_replica);
			p++;
			*sysid = strtou64(p, NULL, 10);
			got_sysid = true;
		}
		else if ((p = strstr(line, "Latest checkpoint location:")) != NULL)
		{
			uint32		hi;
			uint32		lo;

			p = strchr(p, ':');
			if (p == NULL || strlen(p) <= 1)
				pg_fatal("could not parse checkpoint location reported by "
						 "\"%s/pg_controldata\" for \"%s\"", old_bindir, old_replica);
			p++;
			if (sscanf(p, "%X/%X", &hi, &lo) != 2)
				pg_fatal("could not parse checkpoint location reported by "
						 "\"%s/pg_controldata\" for \"%s\"", old_bindir, old_replica);
			*chkpnt_loc = ((uint64) hi) << 32 | lo;
			got_chkpnt = true;
		}
	}
	pg_free(output);

	if (!got_sysid || !got_chkpnt)
		pg_fatal("\"%s/pg_controldata\" did not report a system identifier "
				 "and checkpoint location for \"%s\"", old_bindir, old_replica);
}

/*
 * Refuses to proceed unless the old replica's own pg_control shows it was
 * caught up to the exact old-cluster checkpoint recorded in the manifest.
 * Without this, a lagging replica's reused files would be silently stale
 * forever: the new cluster's recovery only ever replays forward from its
 * own checkpoint, never backward to fill in what an old replica missed.
 */
static void
check_old_replica_caught_up(const char *old_bindir, const char *old_replica,
							const Manifest *manifest)
{
	uint64		sysid;
	XLogRecPtr	chkpnt_loc;
	char		postmaster_pid_path[MAXPGPATH];

	/*
	 * pg_controldata's own "Latest checkpoint location" only reflects a
	 * restartpoint actually flushed to pg_control, which for a running
	 * standby happens on an unpredictable schedule -- reliably only via a
	 * clean shutdown. Refuse rather than risk a confusing "not caught up"
	 * rejection against a replica that genuinely received everything but just
	 * hasn't taken a restartpoint at exactly this LSN yet, and rather than
	 * reuse files out of a directory whose state was never confirmed static.
	 */
	snprintf(postmaster_pid_path, sizeof(postmaster_pid_path),
			 "%s/postmaster.pid", old_replica);
	if (access(postmaster_pid_path, F_OK) == 0)
		pg_fatal("refusing to sync: \"%s\" exists -- --old-replica must be "
				 "cleanly shut down before running this tool", postmaster_pid_path);

	get_old_replica_controldata(old_bindir, old_replica, &sysid, &chkpnt_loc);

	if (sysid != manifest->old_sysid)
		pg_fatal("refusing to sync: old replica system identifier "
				 "%llu does not match old primary's %llu "
				 "(wrong replica for this upgrade)",
				 (unsigned long long) sysid,
				 (unsigned long long) manifest->old_sysid);

	if (chkpnt_loc != manifest->old_chkpnt_loc)
		pg_fatal("refusing to sync: old replica's latest checkpoint location "
				 "%X/%08X does not match the old primary's shutdown checkpoint "
				 "%X/%08X recorded in the manifest -- the replica is not "
				 "caught up (it must stay running/streaming through the "
				 "primary's pre-upgrade shutdown), so unchanged relation "
				 "files on it cannot be trusted",
				 LSN_FORMAT_ARGS(chkpnt_loc),
				 LSN_FORMAT_ARGS(manifest->old_chkpnt_loc));

	pg_log_info("old replica caught up: system identifier and checkpoint "
				"location both match the old primary");
}

/*
 * TABLESPACE_VERSION_DIRECTORY (relpath.h) names each tablespace
 * subdirectory "PG_" PG_MAJORVERSION "_" CATALOG_VERSION_NO. Derive it from
 * the connection's own server_version_num and this build's own compiled-in
 * CATALOG_VERSION_NO -- not from anything fetched from the new primary or
 * read from a file, since remote_connect() (fetch.c) already refused to
 * proceed if this build's own catalog version didn't match the new
 * primary's, so its own catalog version already equals the new cluster's.
 */
static char *
tablespace_version_dir(RemoteConn *rconn)
{
	int			version_num = PQserverVersion(rconn->conn);

	return psprintf("PG_%d_%u", version_num / 10000, CATALOG_VERSION_NO);
}

/*
 * Discovers every non-default tablespace via the pg_tablespace catalog
 * (authoritative, unlike inferring it from a directory listing) and
 * decides where each one's data should live on this host. Doesn't create
 * anything: pg_basebackup's own -T handling already verifies each target
 * directory is empty (or creates it) exactly the same way it already does
 * for -D itself, so there's nothing left here for this tool to duplicate.
 */
static TablespaceInfo *
discover_tablespaces(RemoteConn *rconn, const SyncOptions *opts, int *n_entries)
{
	PGresult   *res;
	int			n;
	TablespaceInfo *entries;

	res = PQexec(rconn->conn,
				 "SELECT oid, pg_tablespace_location(oid) FROM pg_tablespace "
				 "WHERE pg_tablespace_location(oid) != ''");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not list tablespaces: %s", PQresultErrorMessage(res));

	n = PQntuples(res);
	entries = pg_malloc_array(TablespaceInfo, n);

	for (int i = 0; i < n; i++)
	{
		Oid			oid = (Oid) strtoul(PQgetvalue(res, i, 0), NULL, 10);
		const char *primary_path = PQgetvalue(res, i, 1);
		char		old_target[MAXPGPATH];
		const char *mapped;
		bool		in_place = !is_absolute_path(primary_path);

		if (in_place)
		{
			/*
			 * pg_tablespace_location() returns a path relative to PGDATA for
			 * an in-place tablespace (allow_in_place_tablespaces,
			 * testing-only): its data lives at pg_tblspc/<oid> inside the
			 * data directory itself, not at a separate, relocatable location
			 * -- there is no symlink to read, on either side, and neither
			 * pg_basebackup nor pg_combinebackup take a -T entry for it, they
			 * place it automatically. --old-replica's own copy is at the same
			 * fixed spot for the same reason.
			 */
			struct stat st;

			snprintf(old_target, sizeof(old_target), "%s/pg_tblspc/%u",
					 opts->old_replica, oid);
			if (stat(old_target, &st) != 0)
			{
				if (errno == ENOENT)
					old_target[0] = '\0';
				else
					pg_fatal("could not stat \"%s\": %m", old_target);
			}
		}
		else
		{
			char		old_link[MAXPGPATH];
			ssize_t		len;

			snprintf(old_link, sizeof(old_link), "%s/pg_tblspc/%u",
					 opts->old_replica, oid);
			len = readlink(old_link, old_target, sizeof(old_target) - 1);
			if (len >= 0)
			{
				if (len == sizeof(old_target) - 1)
					pg_fatal("refusing to sync: symlink target of \"%s\" is too "
							 "long to fit in %zu bytes", old_link, sizeof(old_target));
				old_target[len] = '\0';
			}
			else if (errno == ENOENT)
				old_target[0] = '\0';
			else
				pg_fatal("could not read symbolic link \"%s\": %m", old_link);
		}

		/*
		 * find_tablespace_mapping() matches by primary_path now, and every
		 * --tablespace-mapping OLDDIR is required to be absolute (checked at
		 * parse time) -- an in-place tablespace's own primary_path never is,
		 * so no valid mapping can ever match one. Nothing further to check
		 * here for that case.
		 */
		mapped = find_tablespace_mapping(opts, primary_path);

		entries[i].oid = oid;
		entries[i].in_place = in_place;
		entries[i].primary_path = pg_strdup(primary_path);
		entries[i].old_target = pg_strdup(old_target);
		entries[i].chosen_path = pg_strdup(mapped != NULL ? mapped :
										   old_target[0] != '\0' ? old_target :
										   primary_path);
	}
	PQclear(res);

	*n_entries = n;
	return entries;
}

/*
 * pg_upgrade always resets the new cluster to timeline 1 (see
 * copy_xact_xlog_xid()'s final pg_resetwal call), but that's a fact about
 * the moment pg_upgrade finished, not necessarily about the moment this
 * tool runs against it -- ask the new primary for its actual current
 * timeline rather than assuming, in case anything timeline-advancing
 * happened to it in between.
 */
static uint32
get_current_timeline(RemoteConn *rconn)
{
	char	   *str = run_scalar_query(rconn->conn,
									   "SELECT timeline_id FROM pg_control_checkpoint()");
	uint32		timeline = (uint32) strtoul(str, NULL, 10);

	pg_free(str);
	return timeline;
}

static uint64
get_system_identifier(RemoteConn *rconn)
{
	char	   *str = run_scalar_query(rconn->conn,
									   "SELECT system_identifier FROM pg_control_system()");
	uint64		sysid = strtou64(str, NULL, 10);

	pg_free(str);
	return sysid;
}

/*
 * pg_combinebackup requires every input directory -- including
 * --old-replica, standing in for the "prior backup" in its combine chain
 * -- to carry a valid global/pg_control (matching the *new* cluster's own
 * system identifier: this run's whole point is treating --old-replica's
 * files as a stand-in for what the new cluster looked like at its own
 * checkpoint, not for the old cluster it actually is) and a backup_label
 * establishing where that stand-in "backup" starts, matching exactly the
 * anchor forge_manifest() already put in the manifest's WAL-Ranges entry.
 * pg_basebackup's own generated backup_label for the incremental side
 * encodes "INCREMENTAL FROM" using that same anchor, taken from the
 * manifest we hand it -- so the two agree by construction, not by luck.
 *
 * A plain byte copy of the new primary's live pg_control is safe to trust
 * here even though it's never parsed or rewritten: read_backup_label()
 * overrides checkPoint/state and re-derives minRecoveryPoint from
 * checkPoint.redo on startup regardless of what's already in this file,
 * and pg_control's active content is capped at PG_CONTROL_MAX_SAFE_SIZE
 * (512 bytes, one disk sector) specifically so concurrent reads through
 * the same page cache are atomic. This copy of pg_control is never itself
 * part of the final --new-replica output -- that one comes from
 * pg_basebackup's own real fetch of it -- so its ordering relative to
 * everything else here doesn't matter.
 */
static void
write_old_replica_view_metadata(RemoteConn *rconn, const char *view_dir,
								XLogRecPtr new_chkpnt_loc, uint32 timeline)
{
	char	   *control_raw;
	size_t		control_len;
	ControlFileData *control_data;
	char		path[MAXPGPATH];
	FILE	   *f;
	char		strftime_buf[128];
	time_t		now = time(NULL);
	XLogSegNo	segno;
	char		wal_file_name[MAXFNAMELEN];

	snprintf(path, sizeof(path), "%s/global", view_dir);
	if (pg_mkdir_p(path, pg_dir_create_mode) != 0 && errno != EEXIST)
		pg_fatal("could not create directory \"%s\": %m", path);

	snprintf(path, sizeof(path), "%s/global/pg_control", rconn->data_directory);
	control_raw = remote_read_whole_file(rconn, path, &control_len);
	if (control_len < sizeof(ControlFileData))
		pg_fatal("file \"%s\" is shorter than expected", path);

	/*
	 * remote_connect() (fetch.c) already refused to proceed if this build's
	 * own pg_control_version didn't match the new primary's, so pg_control's
	 * own layout here is guaranteed byte-compatible -- unlike --old-replica's
	 * copy (see get_old_replica_controldata()'s own comment), which is
	 * necessarily a different major version and has to be read via
	 * pg_controldata text output instead.
	 */
	control_data = (ControlFileData *) control_raw;
	XLByteToSeg(new_chkpnt_loc, segno, control_data->xlog_seg_size);
	XLogFileName(wal_file_name, timeline, segno, control_data->xlog_seg_size);

	snprintf(path, sizeof(path), "%s/global/pg_control", view_dir);
	f = fopen(path, "wb");
	if (f == NULL)
		pg_fatal("could not create file \"%s\": %m", path);
	if (fwrite(control_raw, 1, control_len, f) != control_len)
		pg_fatal("could not write file \"%s\": %m", path);
	if (fclose(f) != 0)
		pg_fatal("could not write file \"%s\": %m", path);
	pg_free(control_raw);

	strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S %Z",
			 localtime(&now));

	snprintf(path, sizeof(path), "%s/backup_label", view_dir);
	f = fopen(path, "wb");
	if (f == NULL)
		pg_fatal("could not create file \"%s\": %m", path);
	/* Deliberately no "INCREMENTAL FROM": this is the base of the chain. */
	fprintf(f,
			"START WAL LOCATION: %X/%08X (file %s)\n"
			"CHECKPOINT LOCATION: %X/%08X\n"
			"BACKUP METHOD: streamed\n"
			"BACKUP FROM: primary\n"
			"START TIME: %s\n"
			"LABEL: pg_upgrade_replica old-replica-view\n"
			"START TIMELINE: %u\n",
			LSN_FORMAT_ARGS(new_chkpnt_loc), wal_file_name,
			LSN_FORMAT_ARGS(new_chkpnt_loc), strftime_buf, timeline);
	if (fclose(f) != 0)
		pg_fatal("could not write file \"%s\": %m", path);
}

/*
 * Makes --old-replica's data reachable under paths matching the *new*
 * cluster's layout, without copying or modifying anything in
 * --old-replica itself: pg_combinebackup looks up a reused file's "prior"
 * copy by joining the *new* cluster's own relative path (exactly as the
 * forged manifest states it) onto this view directory, and a tablespace's
 * relative path includes its version-dir component, which necessarily
 * differs between the two clusters (old catalog version vs. new).
 *
 * base/ has no such component, so it can just be symlinked wholesale.
 * Each tablespace with any local data on --old-replica gets its own real
 * directory containing one symlink, renaming only the version-dir
 * component; a tablespace --old-replica has no copy of at all is simply
 * omitted here (forge_manifest() already won't have listed anything from
 * it, so pg_basebackup fetches all of it fresh, and pg_combinebackup never
 * needs to look inside this view for it).
 */
static char *
build_old_replica_view(const char *work_dir, const char *old_replica,
					   const char *new_version_dir,
					   const TablespaceInfo *tablespaces, int n_tablespaces)
{
	char		view_dir[MAXPGPATH];
	char		link_path[MAXPGPATH];
	char		target_path[MAXPGPATH];

	snprintf(view_dir, sizeof(view_dir), "%s/old_replica_view", work_dir);
	if (pg_mkdir_p(view_dir, pg_dir_create_mode) != 0 && errno != EEXIST)
		pg_fatal("could not create directory \"%s\": %m", view_dir);

	snprintf(link_path, sizeof(link_path), "%s/base", view_dir);
	snprintf(target_path, sizeof(target_path), "%s/base", old_replica);
	if (symlink(target_path, link_path) != 0)
		pg_fatal("could not create symbolic link \"%s\": %m", link_path);

	if (n_tablespaces > 0)
	{
		snprintf(link_path, sizeof(link_path), "%s/pg_tblspc", view_dir);
		if (pg_mkdir_p(link_path, pg_dir_create_mode) != 0 && errno != EEXIST)
			pg_fatal("could not create directory \"%s\": %m", link_path);
	}

	for (int i = 0; i < n_tablespaces; i++)
	{
		char		ts_dir[MAXPGPATH];
		DIR		   *dir;
		struct dirent *de;
		char	   *old_version_dir = NULL;

		if (tablespaces[i].old_target[0] == '\0')
			continue;

		dir = opendir(tablespaces[i].old_target);
		if (dir == NULL)
			pg_fatal("could not open directory \"%s\": %m",
					 tablespaces[i].old_target);
		while ((de = readdir(dir)) != NULL)
		{
			unsigned	major,
						catver;
			int			nchars;

			if (sscanf(de->d_name, "PG_%u_%u%n", &major, &catver, &nchars) == 2 &&
				nchars == (int) strlen(de->d_name))
			{
				if (old_version_dir != NULL)
					pg_fatal("refusing to sync: \"%s\" has more than one "
							 "version subdirectory (\"%s\" and \"%s\")",
							 tablespaces[i].old_target, old_version_dir,
							 de->d_name);
				old_version_dir = pg_strdup(de->d_name);
			}
		}
		closedir(dir);
		if (old_version_dir == NULL)
			pg_fatal("refusing to sync: \"%s\" has no version subdirectory",
					 tablespaces[i].old_target);

		snprintf(ts_dir, sizeof(ts_dir), "%s/pg_tblspc/%u", view_dir,
				 tablespaces[i].oid);
		if (pg_mkdir_p(ts_dir, pg_dir_create_mode) != 0 && errno != EEXIST)
			pg_fatal("could not create directory \"%s\": %m", ts_dir);

		snprintf(link_path, sizeof(link_path), "%s/%s", ts_dir, new_version_dir);
		snprintf(target_path, sizeof(target_path), "%s/%s",
				 tablespaces[i].old_target, old_version_dir);
		if (symlink(target_path, link_path) != 0)
			pg_fatal("could not create symbolic link \"%s\": %m", link_path);
		pg_free(old_version_dir);
	}

	return pg_strdup(view_dir);
}

/*
 * Builds a KeptTablespace array (just oid + old_target, forge_manifest()'s
 * own concern) from the richer TablespaceInfo array this file otherwise
 * needs.
 */
static KeptTablespace *
as_kept_tablespaces(const TablespaceInfo *tablespaces, int n)
{
	KeptTablespace *out = pg_malloc_array(KeptTablespace, Max(n, 1));

	for (int i = 0; i < n; i++)
	{
		out[i].oid = tablespaces[i].oid;
		out[i].old_target = tablespaces[i].old_target;
	}
	return out;
}

void
sync_replica(RemoteConn *rconn, const ConnParams *cparams,
			 const SyncOptions *opts, const char *argv0)
{
	char	   *manifest_raw;
	size_t		manifest_len;
	char		manifest_path[MAXPGPATH];
	Manifest   *manifest;
	uint32		timeline;
	char	   *version_dir;
	int			n_tablespaces;
	TablespaceInfo *tablespaces;
	KeptTablespace *kept_tablespaces;
	uint64		system_identifier;
	char		work_dir_template[MAXPGPATH];
	char	   *work_dir;
	char	   *view_dir;
	char		staging_dir[MAXPGPATH];
	char	   *pg_basebackup_path;
	char	   *pg_combinebackup_path;
	PQExpBuffer cmd;
	int			n_kept_files;

	snprintf(manifest_path, sizeof(manifest_path), "%s/pg_upgrade_manifest",
			 rconn->data_directory);
	manifest_raw = remote_read_whole_file(rconn, manifest_path, &manifest_len);
	manifest = parse_manifest(manifest_raw, manifest_len);
	pg_free(manifest_raw);
	pg_log_info("manifest fetched and parsed successfully");

	check_old_replica_caught_up(opts->old_bindir, opts->old_replica, manifest);

	version_dir = tablespace_version_dir(rconn);
	tablespaces = discover_tablespaces(rconn, opts, &n_tablespaces);
	if (n_tablespaces > 0)
		pg_log_info("tablespaces: %d found", n_tablespaces);

	system_identifier = get_system_identifier(rconn);
	timeline = get_current_timeline(rconn);

	/*
	 * The work directory is created next to --new-replica (not under it):
	 * --new-replica itself must not exist or be empty when pg_basebackup gets
	 * to -D it, and pg_combinebackup's own --link needs its inputs on the
	 * same filesystem as its output to hardlink at all.
	 */
	{
		char	   *new_replica_abs = make_absolute_path(opts->new_replica);
		char		new_replica_parent[MAXPGPATH];

		strlcpy(new_replica_parent, new_replica_abs, sizeof(new_replica_parent));
		get_parent_directory(new_replica_parent);
		snprintf(work_dir_template, sizeof(work_dir_template),
				 "%s/pgur_work-XXXXXX", new_replica_parent);
		pg_free(new_replica_abs);
	}
	work_dir = mkdtemp(work_dir_template);
	if (work_dir == NULL)
		pg_fatal("could not create temporary directory \"%s\": %m",
				 work_dir_template);
	work_dir_to_cleanup = work_dir;
	atexit(cleanup_work_dir_atexit);

	view_dir = build_old_replica_view(work_dir, opts->old_replica, version_dir,
									  tablespaces, n_tablespaces);
	write_old_replica_view_metadata(rconn, view_dir, manifest->new_chkpnt_loc,
									timeline);

	kept_tablespaces = as_kept_tablespaces(tablespaces, n_tablespaces);
	n_kept_files = forge_manifest(work_dir, opts->old_replica,
								  manifest, system_identifier, timeline,
								  version_dir, kept_tablespaces, n_tablespaces);
	pg_log_info("manifest lists %d relation files as unchanged, anchored at "
				"checkpoint %X/%08X", n_kept_files,
				LSN_FORMAT_ARGS(manifest->new_chkpnt_loc));
	pg_free(kept_tablespaces);

	/*
	 * pg_combinebackup treats a missing backup_manifest in an input directory
	 * as merely unable to cross-check its WAL range, not fatal, but it warns
	 * loudly. view_dir describes the exact same forged backup as
	 * work_dir/backup_manifest, so give it the same manifest too, rather than
	 * leave every run printing a warning about something that isn't actually
	 * wrong.
	 */
	{
		char		forged_manifest[MAXPGPATH];
		char		view_manifest[MAXPGPATH];

		snprintf(forged_manifest, sizeof(forged_manifest), "%s/backup_manifest",
				 work_dir);
		snprintf(view_manifest, sizeof(view_manifest), "%s/backup_manifest",
				 view_dir);
		if (link(forged_manifest, view_manifest) != 0)
			pg_fatal("could not link \"%s\" to \"%s\": %m",
					 forged_manifest, view_manifest);
	}

	pg_basebackup_path = find_sibling_exec(argv0, "pg_basebackup");
	pg_combinebackup_path = find_sibling_exec(argv0, "pg_combinebackup");

	snprintf(staging_dir, sizeof(staging_dir), "%s/staging", work_dir);

	cmd = createPQExpBuffer();
	appendShellString(cmd, pg_basebackup_path);
	appendPQExpBufferStr(cmd, " -D ");
	appendShellString(cmd, staging_dir);
	appendPQExpBufferStr(cmd, " -i ");
	{
		char		staging_manifest_path[MAXPGPATH];

		snprintf(staging_manifest_path, sizeof(staging_manifest_path),
				 "%s/backup_manifest", work_dir);
		appendShellString(cmd, staging_manifest_path);
	}
	appendPQExpBufferStr(cmd, " --no-sync");
	if (cparams->pghost)
	{
		appendPQExpBufferStr(cmd, " -h ");
		appendShellString(cmd, cparams->pghost);
	}
	if (cparams->pgport)
	{
		appendPQExpBufferStr(cmd, " -p ");
		appendShellString(cmd, cparams->pgport);
	}
	if (cparams->pguser)
	{
		appendPQExpBufferStr(cmd, " -U ");
		appendShellString(cmd, cparams->pguser);
	}
	if (cparams->prompt_password == TRI_NO)
		appendPQExpBufferStr(cmd, " -w");
	for (int i = 0; i < n_tablespaces; i++)
		if (!tablespaces[i].in_place)
		{
			char		staging_ts_dir[MAXPGPATH];
			PQExpBuffer mapping;

			/*
			 * pg_basebackup's own -T target is where it stages this
			 * tablespace's incremental output (INCREMENTAL.* fragments, same
			 * as -D itself), not the final destination: it needs
			 * pg_combinebackup to process it afterward, same as -D's own
			 * staging_dir does. Pointing pg_basebackup directly at
			 * chosen_path (the actual final directory) here would make
			 * pg_combinebackup's own -T below try to treat chosen_path as
			 * both its input and its output at once, which it refuses (wants
			 * its output pre-empty).
			 */
			snprintf(staging_ts_dir, sizeof(staging_ts_dir),
					 "%s/staging_ts_%u", work_dir, tablespaces[i].oid);
			mapping = createPQExpBuffer();
			append_tablespace_mapping_operand(mapping, tablespaces[i].primary_path);
			appendPQExpBufferChar(mapping, '=');
			append_tablespace_mapping_operand(mapping, staging_ts_dir);
			appendPQExpBufferStr(cmd, " -T ");
			appendShellString(cmd, mapping->data);
			destroyPQExpBuffer(mapping);
		}

	pg_log_info("fetching incremental backup from the new primary ...");
	run_pg_tool(cmd->data);
	resetPQExpBuffer(cmd);

	appendShellString(cmd, pg_combinebackup_path);
	appendPQExpBufferChar(cmd, ' ');
	appendShellString(cmd, view_dir);
	appendPQExpBufferChar(cmd, ' ');
	appendShellString(cmd, staging_dir);
	appendPQExpBufferStr(cmd, " -o ");
	appendShellString(cmd, opts->new_replica);
	for (int i = 0; i < n_tablespaces; i++)
		if (!tablespaces[i].in_place)
		{
			char		staging_ts_dir[MAXPGPATH];
			PQExpBuffer mapping;

			snprintf(staging_ts_dir, sizeof(staging_ts_dir),
					 "%s/staging_ts_%u", work_dir, tablespaces[i].oid);
			mapping = createPQExpBuffer();
			append_tablespace_mapping_operand(mapping, staging_ts_dir);
			appendPQExpBufferChar(mapping, '=');
			append_tablespace_mapping_operand(mapping, tablespaces[i].chosen_path);
			appendPQExpBufferStr(cmd, " -T ");
			appendShellString(cmd, mapping->data);
			destroyPQExpBuffer(mapping);
		}
	if (opts->link)
		appendPQExpBufferStr(cmd, " -k");
	if (opts->no_sync)
		appendPQExpBufferStr(cmd, " -N");

	pg_log_info("reconstructing the new standby ...");
	run_pg_tool(cmd->data);
	destroyPQExpBuffer(cmd);

	/*
	 * From here until WriteRecoveryConfig() finishes, --new-replica has a
	 * backup_label (pg_combinebackup already wrote it) but not yet a
	 * standby.signal: if this process is killed in that window, the result
	 * looks like a complete data directory but would come up as an
	 * independent primary, not a standby, if started directly. This is the
	 * same window pg_basebackup -R has with this same pair of calls, just
	 * wider here because pg_combinebackup's own run sits in front of it
	 * instead of a single tar extraction. See the tool's own documentation
	 * for the operational guidance this implies.
	 */
	{
		PQExpBuffer recovery_conf = GenerateRecoveryConfig(rconn->conn, NULL, NULL);

		WriteRecoveryConfig(rconn->conn, opts->new_replica, recovery_conf);
		destroyPQExpBuffer(recovery_conf);
	}

	if (!rmtree(work_dir, true))
		pg_log_warning("could not remove temporary directory \"%s\"", work_dir);
	work_dir_to_cleanup = NULL;

	pg_free(pg_basebackup_path);
	pg_free(pg_combinebackup_path);
	pg_free(view_dir);
	for (int i = 0; i < n_tablespaces; i++)
	{
		pg_free(tablespaces[i].primary_path);
		pg_free(tablespaces[i].old_target);
		pg_free(tablespaces[i].chosen_path);
	}
	pg_free(tablespaces);
	pg_free(version_dir);
}

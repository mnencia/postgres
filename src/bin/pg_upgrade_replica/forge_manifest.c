/*-------------------------------------------------------------------------
 *
 * forge_manifest.c
 *		Builds a backup_manifest anchored at the new cluster's own
 *		checkpoint, listing every relation file the pg_upgrade manifest
 *		says was left unchanged. Only Path is ever consulted by the
 *		server's incremental-backup logic (basebackup_incremental.c's
 *		GetFileBackupMethod() looks the path up and stats the file itself
 *		on the new primary rather than trusting this manifest's own Size,
 *		and ignores the checksum fields entirely), so no checksum is
 *		computed here -- --old-replica gets the same trust any stopped
 *		PostgreSQL data directory gets when starting recovery from it, the
 *		same trust model this tool has always used. Size is still filled
 *		in faithfully below (walk_db_oids()'s own stat() call), just not
 *		because the incremental decision itself needs it.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pg_upgrade_replica/forge_manifest.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <dirent.h>
#include <sys/stat.h>

#include "common/logging.h"
#include "fe_utils/write_manifest.h"

#include "forge_manifest.h"

/*
 * Matches a relation file's path relative to PGDATA against
 * base/<db_oid>/<relfilenumber>[_fsm|_vm|_init][.<segment>], or the
 * tablespace-relative equivalent
 * pg_tblspc/<ts_oid>/<version_dir>/<db_oid>/<relfilenumber>[...]. The
 * version-dir component isn't validated against any particular value:
 * callers here always pass one they already know is correct.
 *
 * Deliberately returns false for an _init fork: pg_upgrade never transfers
 * one (transfer_relfile() only ever uses "", "_fsm", "_vm"), an unlogged
 * relation's init fork is created fresh by the new cluster's own DDL
 * replay during restore, so the copy already sitting on the new primary
 * is the authoritative one -- reusing --old-replica's would mean trusting
 * that its init fork and a freshly-created one are byte-identical across
 * whatever version gap this upgrade spans, which is never actually
 * checked. Returning false here means it's never added to this manifest,
 * so pg_basebackup fetches it fresh; the fetch is cheap regardless, since
 * an init fork is always a single page.
 */
static bool
parse_rel_path(const char *rel, Oid *db_oid, Oid *relfilenumber, int *segment)
{
	const char *p = rel;
	char	   *end;
	unsigned long db,
				relnum,
				seg;

	if (strncmp(p, "base/", 5) == 0)
		p += 5;
	else if (strncmp(p, "pg_tblspc/", 10) == 0)
	{
		int			slashes_left = 2;	/* skip "<oid>/<version_dir>/" */

		p += 10;
		while (*p && slashes_left > 0)
		{
			if (*p == '/')
				slashes_left--;
			p++;
		}
		if (slashes_left != 0)
			return false;
	}
	else
		return false;

	db = strtoul(p, &end, 10);
	if (end == p || *end != '/')
		return false;
	p = end + 1;

	relnum = strtoul(p, &end, 10);
	if (end == p)
		return false;
	p = end;

	if (strncmp(p, "_fsm", 4) == 0)
		p += 4;
	else if (strncmp(p, "_vm", 3) == 0)
		p += 3;
	else if (strncmp(p, "_init", 5) == 0)
		return false;

	if (*p == '.')
	{
		p++;
		seg = strtoul(p, &end, 10);
		if (end == p)
			return false;
		p = end;
	}
	else
		seg = 0;

	if (*p != '\0')
		return false;

	*db_oid = (Oid) db;
	*relfilenumber = (Oid) relnum;
	*segment = (int) seg;
	return true;
}

/*
 * Finds the single version-dir subdirectory (PG_<major>_<catver>) under a
 * tablespace's physical location on --old-replica. A frozen standby's own
 * tablespace directory should hold exactly one: unlike the *new primary's*
 * tablespace root (which legitimately keeps the old cluster's own
 * version-dir around until delete_old_cluster.sh runs), --old-replica was
 * never itself pg_upgrade'd, so there is nothing else it should ever have
 * collected here. Zero or more than one is refused rather than guessed at.
 */
static char *
find_old_version_dir(const char *tablespace_dir)
{
	DIR		   *dir = opendir(tablespace_dir);
	struct dirent *de;
	char	   *found = NULL;

	if (dir == NULL)
		pg_fatal("could not open directory \"%s\": %m", tablespace_dir);

	while ((de = readdir(dir)) != NULL)
	{
		unsigned	major,
					catver;
		int			nchars;

		if (sscanf(de->d_name, "PG_%u_%u%n", &major, &catver, &nchars) == 2 &&
			nchars == (int) strlen(de->d_name))
		{
			if (found != NULL)
				pg_fatal("refusing to sync: \"%s\" has more than one version "
						 "subdirectory (\"%s\" and \"%s\") -- --old-replica "
						 "should hold only its own data, not a leftover from "
						 "some other installation",
						 tablespace_dir, found, de->d_name);
			found = pg_strdup(de->d_name);
		}
	}
	closedir(dir);

	if (found == NULL)
		pg_fatal("refusing to sync: \"%s\" has no version subdirectory",
				 tablespace_dir);

	return found;
}

/*
 * Walks one physical directory on --old-replica whose immediate
 * subdirectories are database OIDs -- either old_replica/base, or one
 * tablespace's version-dir -- and adds a manifest entry for every relation
 * file the pg_upgrade manifest lists as unchanged.
 *
 * manifest_prefix is the PGDATA-relative prefix to use when constructing
 * each entry's Path: "base" for the default tablespace, or
 * "pg_tblspc/<oid>/<new_version_dir>" for a tablespace. That's what makes
 * the manifest describe the *new* primary's own layout even though the
 * size being recorded comes from reading --old-replica's differently
 * versioned copy of the same relation.
 */
static int
walk_db_oids(manifest_writer *mwriter, const Manifest *manifest,
			 const char *physical_dir, const char *manifest_prefix)
{
	DIR		   *dbdir = opendir(physical_dir);
	struct dirent *dbent;
	int			count = 0;

	if (dbdir == NULL)
	{
		if (errno == ENOENT)
			return 0;
		pg_fatal("could not open directory \"%s\": %m", physical_dir);
	}

	while ((dbent = readdir(dbdir)) != NULL)
	{
		char		db_physical_dir[MAXPGPATH];
		DIR		   *reldir;
		struct dirent *relent;

		if (dbent->d_name[0] == '\0' ||
			strspn(dbent->d_name, "0123456789") != strlen(dbent->d_name))
			continue;			/* not a db_oid directory */

		snprintf(db_physical_dir, sizeof(db_physical_dir), "%s/%s",
				 physical_dir, dbent->d_name);

		reldir = opendir(db_physical_dir);
		if (reldir == NULL)
			continue;			/* not actually a directory */

		while ((relent = readdir(reldir)) != NULL)
		{
			char		rel_path[MAXPGPATH];
			Oid			db_oid,
						relfilenumber;
			int			segment;
			char		physical_path[MAXPGPATH];
			struct stat sb;

			if (strcmp(relent->d_name, ".") == 0 ||
				strcmp(relent->d_name, "..") == 0)
				continue;

			snprintf(rel_path, sizeof(rel_path), "%s/%s/%s",
					 manifest_prefix, dbent->d_name, relent->d_name);

			if (!parse_rel_path(rel_path, &db_oid, &relfilenumber, &segment) ||
				!manifest_is_kept(manifest, db_oid, relfilenumber))
				continue;

			snprintf(physical_path, sizeof(physical_path), "%s/%s",
					 db_physical_dir, relent->d_name);
			if (stat(physical_path, &sb) != 0)
				pg_fatal("could not stat file \"%s\": %m", physical_path);

			add_file_to_manifest(mwriter, rel_path, sb.st_size, sb.st_mtime,
								 CHECKSUM_TYPE_NONE, 0, NULL);
			count++;
		}
		closedir(reldir);
	}
	closedir(dbdir);

	return count;
}

int
forge_manifest(char *manifest_dir, const char *old_replica,
			   const Manifest *manifest, uint64 system_identifier,
			   uint32 timeline, const char *new_version_dir,
			   const KeptTablespace *tablespaces, int n_tablespaces)
{
	manifest_writer *mwriter;
	manifest_wal_range wal_range = {0};
	int			count = 0;
	char		base_dir[MAXPGPATH];

	mwriter = create_manifest_writer(manifest_dir, system_identifier);

	snprintf(base_dir, sizeof(base_dir), "%s/base", old_replica);
	count += walk_db_oids(mwriter, manifest, base_dir, "base");

	for (int i = 0; i < n_tablespaces; i++)
	{
		char	   *old_version_dir;
		char		physical_dir[MAXPGPATH];
		char		manifest_prefix[MAXPGPATH];

		if (tablespaces[i].old_target[0] == '\0')
			continue;

		old_version_dir = find_old_version_dir(tablespaces[i].old_target);
		snprintf(physical_dir, sizeof(physical_dir), "%s/%s",
				 tablespaces[i].old_target, old_version_dir);
		snprintf(manifest_prefix, sizeof(manifest_prefix), "pg_tblspc/%u/%s",
				 tablespaces[i].oid, new_version_dir);

		count += walk_db_oids(mwriter, manifest, physical_dir, manifest_prefix);
		pg_free(old_version_dir);
	}

	wal_range.tli = timeline;
	wal_range.start_lsn = manifest->new_chkpnt_loc;
	wal_range.end_lsn = manifest->new_chkpnt_loc;
	wal_range.next = NULL;
	finalize_manifest(mwriter, &wal_range);

	return count;
}

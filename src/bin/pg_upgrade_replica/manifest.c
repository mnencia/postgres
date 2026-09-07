/*-------------------------------------------------------------------------
 *
 * manifest.c
 *		Parsing of pg_upgrade's pg_upgrade_manifest file.
 *
 * The format is deliberately tiny and line-based, see
 * relfilenumber.c:finalize_upgrade_manifest() and
 * append_new_cluster_checkpoint() on the pg_upgrade side:
 *
 *   PG_UPGRADE_MANIFEST 1 <old_sysid> <old_chkpnt_loc>
 *   <db_oid> <relfilenumber>
 *   ... one line per relation pg_upgrade transferred unchanged ...
 *   NEW_CHECKPOINT <new_chkpnt_loc>
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pg_upgrade_replica/manifest.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "common/logging.h"

#include "manifest.h"

#define SH_PREFIX		kept_rels
#define SH_ELEMENT_TYPE	KeptRel
#define SH_KEY_TYPE		uint64
#define SH_KEY			key
#define SH_HASH_KEY(tb, key)	fasthash32((const char *) &(key), sizeof(uint64), 0)
#define SH_EQUAL(tb, a, b)		((a) == (b))
#define SH_SCOPE		extern
#define SH_RAW_ALLOCATOR	pg_malloc0
#define SH_DEFINE
#include "lib/simplehash.h"

static bool
parse_lsn(const char *s, XLogRecPtr *result)
{
	uint32		hi;
	uint32		lo;

	if (sscanf(s, "%X/%X", &hi, &lo) != 2)
		return false;
	*result = ((uint64) hi << 32) | (uint64) lo;
	return true;
}

Manifest *
parse_manifest(const char *raw, size_t rawlen)
{
	Manifest   *manifest;
	char	   *copy;
	char	   *line;
	char	   *lines_saveptr;
	bool		have_new_checkpoint = false;

	copy = pg_malloc(rawlen + 1);
	memcpy(copy, raw, rawlen);
	copy[rawlen] = '\0';

	manifest = pg_malloc0(sizeof(Manifest));
	manifest->kept = kept_rels_create(1024, NULL);

	line = strtok_r(copy, "\n", &lines_saveptr);
	if (line == NULL)
		pg_fatal("empty pg_upgrade_manifest");

	{
		char	   *header_saveptr;
		char	   *magic,
				   *version_str,
				   *old_sysid_str,
				   *old_chkpnt_str;
		unsigned long version;

		magic = strtok_r(line, " ", &header_saveptr);
		version_str = strtok_r(NULL, " ", &header_saveptr);
		old_sysid_str = strtok_r(NULL, " ", &header_saveptr);
		old_chkpnt_str = strtok_r(NULL, " ", &header_saveptr);

		if (magic == NULL || version_str == NULL || old_sysid_str == NULL ||
			old_chkpnt_str == NULL || strcmp(magic, "PG_UPGRADE_MANIFEST") != 0)
			pg_fatal("malformed pg_upgrade_manifest header: \"%s\"", line);

		version = strtoul(version_str, NULL, 10);
		if (version != 1)
			pg_fatal("unsupported pg_upgrade_manifest format version %lu "
					 "(this client only understands version 1)", version);

		manifest->old_sysid = strtou64(old_sysid_str, NULL, 10);
		if (!parse_lsn(old_chkpnt_str, &manifest->old_chkpnt_loc))
			pg_fatal("malformed checkpoint location in pg_upgrade_manifest header: \"%s\"",
					 old_chkpnt_str);
	}

	for (line = strtok_r(NULL, "\n", &lines_saveptr);
		 line != NULL;
		 line = strtok_r(NULL, "\n", &lines_saveptr))
	{
		char	   *entry_saveptr;
		char	   *db_oid_str,
				   *relfilenumber_str;
		uint64		key;
		KeptRel    *entry;
		bool		found;		/* unused: a duplicate line just re-inserts
								 * the same key harmlessly */

		if (strncmp(line, "NEW_CHECKPOINT ", 15) == 0)
		{
			if (!parse_lsn(line + 15, &manifest->new_chkpnt_loc))
				pg_fatal("malformed NEW_CHECKPOINT line in pg_upgrade_manifest: \"%s\"",
						 line);
			have_new_checkpoint = true;
			continue;
		}

		db_oid_str = strtok_r(line, " ", &entry_saveptr);
		relfilenumber_str = strtok_r(NULL, " ", &entry_saveptr);
		if (db_oid_str == NULL || relfilenumber_str == NULL)
			pg_fatal("malformed pg_upgrade_manifest line: \"%s\"", line);

		key = kept_rel_key((Oid) strtoul(db_oid_str, NULL, 10),
						   (Oid) strtoul(relfilenumber_str, NULL, 10));
		entry = kept_rels_insert(manifest->kept, key, &found);
		entry->key = key;
	}

	if (!have_new_checkpoint)
		pg_fatal("pg_upgrade_manifest has no NEW_CHECKPOINT trailer -- "
				 "was pg_upgrade interrupted before it finished?");

	pg_free(copy);
	return manifest;
}

bool
manifest_is_kept(const Manifest *manifest, Oid db_oid, Oid relfilenumber)
{
	uint64		key = kept_rel_key(db_oid, relfilenumber);

	return kept_rels_lookup(manifest->kept, key) != NULL;
}

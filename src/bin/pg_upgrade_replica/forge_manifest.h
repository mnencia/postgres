/*-------------------------------------------------------------------------
 *
 * forge_manifest.h
 *		Forges a real backup_manifest from pg_upgrade's own manifest (see
 *		manifest.h), listing every file --old-replica's own copy of which
 *		pg_upgrade left unchanged, so pg_basebackup --incremental can be
 *		pointed at it directly.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pg_upgrade_replica/forge_manifest.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGUR_FORGE_MANIFEST_H
#define PGUR_FORGE_MANIFEST_H

#include "manifest.h"

/* One non-default tablespace, as needed to locate its files under --old-replica. */
typedef struct KeptTablespace
{
	Oid			oid;

	/*
	 * --old-replica's own pg_tblspc/<oid> symlink target, or "" if
	 * --old-replica has no local copy of this tablespace at all (nothing from
	 * it can be reused; pg_basebackup will fetch all of it fresh).
	 */
	char	   *old_target;
} KeptTablespace;

extern int	forge_manifest(char *manifest_dir, const char *old_replica,
						   const Manifest *manifest, uint64 system_identifier,
						   uint32 timeline, const char *new_version_dir,
						   const KeptTablespace *tablespaces, int n_tablespaces);

#endif							/* PGUR_FORGE_MANIFEST_H */

/*-------------------------------------------------------------------------
 *
 * pg_upgrade_replica.h
 *		Command-line options and the sync_replica() entry point.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pg_upgrade_replica/pg_upgrade_replica.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_UPGRADE_REPLICA_H
#define PG_UPGRADE_REPLICA_H

#include "access/xlogdefs.h"
#include "fetch.h"

/*
 * Command-line/connection options, gathered in one place so every module
 * can take a single (const SyncOptions *) instead of a growing list of
 * separate arguments.
 */
typedef struct SyncOptions
{
	char	   *old_bindir;
	char	   *old_replica;
	char	   *new_replica;
	bool		link;
	bool		no_sync;
	/* old dir -> new dir, from repeated --tablespace-mapping OLDDIR=NEWDIR */
	struct TablespaceMapping *tablespace_mappings;
} SyncOptions;

typedef struct TablespaceMapping
{
	char	   *old_dir;
	char	   *new_dir;
	struct TablespaceMapping *next;
} TablespaceMapping;

extern void sync_replica(RemoteConn *rconn, const ConnParams *cparams,
						 const SyncOptions *opts, const char *argv0);

#endif							/* PG_UPGRADE_REPLICA_H */

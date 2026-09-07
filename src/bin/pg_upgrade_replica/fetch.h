/*-------------------------------------------------------------------------
 *
 * fetch.h
 *		Everything that talks to the new primary over a plain libpq/SQL
 *		connection: connection setup, and the one-off reads (the small
 *		pg_upgrade_manifest file, a few catalog/GUC values) this tool still
 *		needs directly. Bulk data transfer is no longer this tool's own
 *		protocol -- see forge_manifest.h and the pg_basebackup/
 *		pg_combinebackup subprocesses driven from reuse.c.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pg_upgrade_replica/fetch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGUR_FETCH_H
#define PGUR_FETCH_H

#include "fe_utils/connect_utils.h"
#include "libpq-fe.h"

typedef struct RemoteConn
{
	PGconn	   *conn;
	char	   *data_directory;
} RemoteConn;

extern RemoteConn *remote_connect(const ConnParams *cparams,
								  const char *progname);
extern void remote_disconnect(RemoteConn *rconn);
extern char *run_scalar_query(PGconn *conn, const char *sql);

extern char *remote_read_whole_file(RemoteConn *rconn, const char *abspath,
									size_t *len_p);

#endif							/* PGUR_FETCH_H */

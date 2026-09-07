/*-------------------------------------------------------------------------
 *
 * Write a new backup manifest.
 *
 * manifest_wal_range is defined here, rather than by each caller
 * separately, so that finalize_manifest()'s single walk of the chain (via
 * "next") is over one real, shared type rather than an informally
 * documented shape multiple independently-declared structs happen to
 * match. "prev" exists only for a caller that wants to build the chain
 * back-to-front while parsing a manifest whose own entries arrive in
 * forward order; finalize_manifest() itself never reads it.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/write_manifest.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WRITE_MANIFEST_H
#define WRITE_MANIFEST_H

#include "access/xlogdefs.h"
#include "common/checksum_helper.h"

typedef struct manifest_wal_range
{
	TimeLineID	tli;
	XLogRecPtr	start_lsn;
	XLogRecPtr	end_lsn;
	struct manifest_wal_range *next;
	struct manifest_wal_range *prev;
} manifest_wal_range;

struct manifest_writer;
typedef struct manifest_writer manifest_writer;

extern manifest_writer *create_manifest_writer(const char *directory,
											   uint64 system_identifier);
extern void add_file_to_manifest(manifest_writer *mwriter,
								 const char *manifest_path,
								 uint64 size, time_t mtime,
								 pg_checksum_type checksum_type,
								 int checksum_length,
								 uint8 *checksum_payload);
extern void finalize_manifest(manifest_writer *mwriter,
							  manifest_wal_range *first_wal_range);

#endif							/* WRITE_MANIFEST_H */

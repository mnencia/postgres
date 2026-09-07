/*-------------------------------------------------------------------------
 *
 * manifest.h
 *		Parsing of pg_upgrade's pg_upgrade_manifest file.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pg_upgrade_replica/manifest.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGUR_MANIFEST_H
#define PGUR_MANIFEST_H

#include "access/xlogdefs.h"
#include "common/hashfn_unstable.h"

/*
 * One entry per (db_oid, relfilenumber) pair the manifest lists as
 * unchanged. The two are packed into a single uint64 key (db_oid in the
 * high 32 bits) so a plain scalar hash table can be used instead of a
 * struct-keyed one.
 */
typedef struct KeptRel
{
	uint32		status;			/* hash status, required by simplehash */
	uint64		key;
} KeptRel;

static inline uint64
kept_rel_key(Oid db_oid, Oid relfilenumber)
{
	return ((uint64) db_oid << 32) | (uint64) relfilenumber;
}

#define SH_PREFIX		kept_rels
#define SH_ELEMENT_TYPE	KeptRel
#define SH_KEY_TYPE		uint64
#define SH_KEY			key
#define SH_HASH_KEY(tb, key)	fasthash32((const char *) &(key), sizeof(uint64), 0)
#define SH_EQUAL(tb, a, b)		((a) == (b))
#define SH_SCOPE		extern
#define SH_RAW_ALLOCATOR	pg_malloc0
#define SH_DECLARE
#include "lib/simplehash.h"

typedef struct Manifest
{
	uint64		old_sysid;
	XLogRecPtr	old_chkpnt_loc;
	XLogRecPtr	new_chkpnt_loc;
	kept_rels_hash *kept;
} Manifest;

extern Manifest *parse_manifest(const char *raw, size_t rawlen);
extern bool manifest_is_kept(const Manifest *manifest, Oid db_oid,
							 Oid relfilenumber);

#endif							/* PGUR_MANIFEST_H */

/*
 *	hcb.c:		implementation of the extension definition
 *			functions for the Hop Count extension block.
 *
 *	Copyright (c) 2019, California Institute of Technology.
 *	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
 *	acknowledged.
 *
 *	Author: Scott Burleigh, JPL
 */

#include "bpP.h"
#include "bei.h"
#include "hcb.h"

#ifndef	ION_HOP_LIMIT
#define	ION_HOP_LIMIT	15
#endif

int	hcb_offer(ExtensionBlock *blk, Bundle *bundle)
{
	bundle->hopCount = 0;
	bundle->hopLimit = ION_HOP_LIMIT;
	blk->blkProcFlags = BLK_MUST_BE_COPIED;
	blk->dataLength = 0;	/*	Will know length at dequeue.	*/
	blk->length = 3;	/*	Just to keep block alive.	*/
	blk->size = 0;
	blk->object = 0;
	return 0;
}

int	hcb_serialize(ExtensionBlock *blk, Bundle *bundle)
{
	unsigned char	dataBuffer[24];
	unsigned char	*cursor;
	uvast		uvtemp;

	cursor = dataBuffer;
	uvtemp = 2;
	oK(cbor_encode_array_open(uvtemp, &cursor));
	uvtemp = bundle->hopLimit;
	oK(cbor_encode_integer(uvtemp, &cursor));
	uvtemp = bundle->hopCount;
	oK(cbor_encode_integer(uvtemp, &cursor));
	blk->dataLength = cursor - dataBuffer;
	return serializeExtBlk(blk, (char *) dataBuffer);
}

int	hcb_processOnDequeue(ExtensionBlock *blk, Bundle *bundle, void *ctxt)
{
	bundle->hopCount += 1;
	if (bundle->hopCount > bundle->hopLimit)
	{
		bundle->corrupt = 1;	/*	Hop limit exceeded.	*/
		return 0;
	}

	return hcb_serialize(blk, bundle);
}

int	hcb_parse(AcqExtBlock *blk, AcqWorkArea *wk)
{
	Bundle		*bundle = &wk->bundle;
	unsigned char	*cursor;
	unsigned int	unparsedBytes = blk->dataLength;
	uvast		arrayLength;
	uvast		uvtemp;

	if (unparsedBytes < 1)
	{
		writeMemo("[?] Can't decode Hop Count block.");
		return 0;		/*	Malformed.		*/
	}

	/*	Data parsed out of the hcb byte array go directly
	 *	into the bundle structure, not into a block-specific
	 *	workspace object.					*/

	blk->size = 0;
	blk->object = NULL;
	cursor = blk->bytes + (blk->length - blk->dataLength);
	arrayLength = 2;
	if (cbor_decode_array_open(&arrayLength, &cursor, &unparsedBytes) < 1)
	{
		writeMemo("[?] Can't decode Hop Count block array.");
		return 0;
	}

	if (cbor_decode_integer(&uvtemp, CborAny, &cursor, &unparsedBytes) < 1)
	{
		writeMemo("[?] Can't decode hop limit.");
	}

	bundle->hopLimit = uvtemp;
	if (cbor_decode_integer(&uvtemp, CborAny, &cursor, &unparsedBytes) < 1)
	{
		writeMemo("[?] Can't decode hop count.");
	}

	bundle->hopCount = uvtemp;
	if (unparsedBytes != 0)
	{
		writeMemo("[?] Excess bytes at end of Hop Count block.");
		return 0;		/*	Malformed.		*/
	}

	return 1;
}

int	hcb_check(AcqExtBlock *blk, AcqWorkArea *wk)
{
	return 1;
}

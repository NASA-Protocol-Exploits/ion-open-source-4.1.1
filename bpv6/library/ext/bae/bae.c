/*
 *	bae.c:		implementation of the extension definition
 *			functions for the Bundle Age Extension block.
 *
 *	Copyright (c) 2012, California Institute of Technology.
 *	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
 *	acknowledged.
 *
 *	Author: Scott Burleigh, JPL
 */

#include "bpP.h"
#include "bei.h"
#include "bae.h"

int	bae_offer(ExtensionBlock *blk, Bundle *bundle)
{
	Sdnv	ageSdnv;
	char	dataBuffer[32];

	bundle->age = 0;
	blk->blkProcFlags = BLK_MUST_BE_COPIED;
	encodeSdnv(&ageSdnv, bundle->age);
	blk->dataLength = ageSdnv.length;
	blk->size = 0;
	blk->object = 0;
	memcpy(dataBuffer, ageSdnv.text, ageSdnv.length);
	return serializeExtBlk(blk, NULL, dataBuffer);
}

void	bae_release(ExtensionBlock *blk)
{
	return;
}

int	bae_record(ExtensionBlock *sdrBlk, AcqExtBlock *ramBlk)
{
	return 0;
}

int	bae_copy(ExtensionBlock *newBlk, ExtensionBlock *oldBlk)
{
	return 0;
}

int	bae_processOnFwd(ExtensionBlock *blk, Bundle *bundle, void *ctxt)
{
	return 0;
}

int	bae_processOnAccept(ExtensionBlock *blk, Bundle *bundle, void *ctxt)
{
	return 0;
}

int	bae_processOnEnqueue(ExtensionBlock *blk, Bundle *bundle, void *ctxt)
{
	return 0;
}

int	bae_processOnDequeue(ExtensionBlock *blk, Bundle *bundle, void *ctxt)
{
	struct timeval	currentTime;
	Sdnv		ageSdnv;
	char		dataBuffer[32];

	if (ionClockIsSynchronized() && bundle->id.creationTime.seconds > 0)
	{
		bundle->age = 1000000 * ((getCtime() - EPOCH_2000_SEC)
				- bundle->id.creationTime.seconds);
	}
	else
	{
		getCurrentTime(&currentTime);
		if (currentTime.tv_usec < bundle->arrivalTime.tv_usec)
		{
			currentTime.tv_usec += 1000000;
			currentTime.tv_sec -= 1;
		}

		currentTime.tv_sec -= bundle->arrivalTime.tv_sec;
		currentTime.tv_usec -= bundle->arrivalTime.tv_usec;
		bundle->age += currentTime.tv_usec
				+ (1000000 * currentTime.tv_sec);
	}

	encodeSdnv(&ageSdnv, bundle->age);
	blk->dataLength = ageSdnv.length;
	memcpy(dataBuffer, ageSdnv.text, ageSdnv.length);
	return serializeExtBlk(blk, NULL, dataBuffer);
}

int	bae_parse(AcqExtBlock *blk, AcqWorkArea *wk)
{
	Bundle		*bundle = &wk->bundle;
	unsigned char	*cursor;
	int		bytesRemaining = blk->dataLength;

	if (bytesRemaining < 1)
	{
		return 0;		/*	Malformed.		*/
	}

	/*	Data parsed out of the bae byte array go directly
	 *	into the bundle structure, not into a block-specific
	 *	workspace object.					*/

	blk->size = 0;
	blk->object = NULL;
	cursor = blk->bytes + (blk->length - blk->dataLength);
	extractSmallSdnv(&(bundle->age), &cursor, &bytesRemaining);
	if (bytesRemaining != 0)
	{
		return 0;		/*	Malformed.		*/
	}

	getCurrentTime(&(bundle->arrivalTime));
	return 1;
}

int	bae_check(AcqExtBlock *blk, AcqWorkArea *wk)
{
	return 1;
}

void	bae_clear(AcqExtBlock *blk)
{
	return;
}

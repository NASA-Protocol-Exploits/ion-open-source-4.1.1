/*

	saga.c:	API for managing ION's distributed history of
       		discovered contacts and generating predicted
		contacts from that history.

	Author:	Scott Burleigh, JPL

	Copyright (c) 2019, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.

									*/
#include "bpP.h"
#include "lyst.h"

#define CONFIDENCE_BASIS	(MAX_CONTACT_LOG_LENGTH * 2.0)

/*	*	Contact prediction functions	*	*	*	*/

#define	LOW_BASE_CONFIDENCE	(.05)
#define	HIGH_BASE_CONFIDENCE	(.20)

/*	A PbContact is a contact note in the prediction base.		*/

typedef struct
{
	uvast	duration;
	uvast	capacity;
	uvast	fromNode;
	uvast	toNode;
	time_t	fromTime;
	time_t	toTime;
	size_t	xmitRate;
} PbContact;

static int	removePredictedContacts(int regionIdx)
{
	Sdr		sdr = getIonsdr();
	IonDB		iondb;
	uint32_t	regionNbr;
	Object		obj;
	Object		elt;
	Object		nextElt;
	IonContact	contact;

	sdr_read(sdr, (char *) &iondb, getIonDbObject(), sizeof(IonDB));
	regionNbr = iondb.regions[regionIdx].regionNbr;
	CHKERR(sdr_begin_xn(sdr));
	for (elt = sdr_list_first(sdr, iondb.regions[regionIdx].contacts); elt;
		       	elt = nextElt)
	{
		nextElt = sdr_list_next(sdr, elt);
		obj = sdr_list_data(sdr, elt);
		sdr_read(sdr, (char *) &contact, obj,
				sizeof(IonContact));
		if (contact.type != CtPredicted)
		{
			continue;	/*	Not predicted.	*/
		}

		/*	This is a predicted contact.		*/

		if (rfx_remove_contact(regionNbr, &contact.fromTime,
				contact.fromNode, contact.toNode, 0) < 0)
		{
			putErrmsg("Failure in rfx_remove_contact.", NULL);
			break;
		}
	}

	if (sdr_end_xn(sdr) < 0)
	{
		putErrmsg("Can't remove predicted contacts.", NULL);
		return -1;
	}

	return 0;
}

static void	freePbContents(LystElt elt, void *userdata)
{
	MRELEASE(lyst_data(elt));
}

static int	insertIntoPredictionBase(Lyst pb, Object sagaElt)
{
	Sdr		sdr = getIonsdr();
	Encounter	encounter;
	vast		duration;
	LystElt		pbElt;
	PbContact	*contact;

	sdr_read(sdr, (char *) &encounter, sdr_list_data(sdr, sagaElt),
			sizeof(Encounter));
#ifdef SAGA_DEBUG
char	buf1[64];
char	buf2[64];

writeTimestampLocal(encounter->fromTime, buf1);
writeTimestampLocal(encounter->toTime, buf2);
printf("Inserting contact into prediction base, from "
UVAST_FIELDSPEC " to " UVAST_FIELDSPEC ", start %s, stop %s.\n",
encounter->fromNode, encounter->toNode, buf1, buf2);
#endif
	duration = encounter.toTime - encounter.fromTime;
	if (duration <= 0 || encounter.xmitRate == 0)
	{
		sdr_list_delete(sdr, sagaElt, NULL, NULL);
		return 0;	/*	Useless encounter.		*/
	}

	/*	Find insertion point for this encounter.		*/

	for (pbElt = lyst_first(pb); pbElt; pbElt = lyst_next(pbElt))
	{
		contact = (PbContact *) lyst_data(pbElt);
		if (contact->fromNode < encounter.fromNode)
		{
			continue;
		}

		if (contact->fromNode > encounter.fromNode)
		{
			break;
		}

		if (contact->toNode < encounter.toNode)
		{
			continue;
		}

		if (contact->toNode > encounter.toNode)
		{
			break;
		}

		if (contact->fromTime < encounter.fromTime)
		{
			continue;
		}

		if (contact->fromTime > encounter.toTime)
		{
			break;
		}

		/*	This encounter duplicates one that has
		 *	already been inserted into the prediction
		 *	base, so it must be deleted from the saga.	*/

		sdr_list_delete(sdr, sagaElt, NULL, NULL);
		return 0;
	}

	/*	Insert the new encounter before this one in the
	 *	prediction base.					*/

	contact = MTAKE(sizeof(PbContact));
	if (contact == NULL)
	{
		putErrmsg("No memory for prediction base contact.", NULL);
		return -1;
	}

	contact->duration = duration;
	contact->capacity = duration * encounter.xmitRate;
	contact->fromNode = encounter.fromNode;
	contact->toNode = encounter.toNode;
	contact->fromTime = encounter.fromTime;
	contact->toTime = encounter.toTime;
	contact->xmitRate = encounter.xmitRate;
	if (pbElt)
	{
		pbElt = lyst_insert_before(pbElt, contact);
	}
	else
	{
		pbElt = lyst_insert_last(pb, contact);
	}

	if (pbElt == NULL)
	{
		putErrmsg("No memory for prediction base list element.", NULL);
		return -1;
	}

	return 0;
}

static Lyst	constructPredictionBase(int regionIdx)
{
	Sdr	sdr = getIonsdr();
	Lyst	pb;
	BpDB	db;
	Object	elt;
	Object	nextElt;

#ifdef SAGA_DEBUG
puts("Building prediction base.");
#endif
	pb = lyst_create_using(getIonMemoryMgr());
	if (pb == NULL)
	{
		putErrmsg("No memory for prediction base.", NULL);
		return NULL;
	}

	lyst_delete_set(pb, freePbContents, NULL);
	sdr_read(sdr, (char *) &db, getBpDbObject(), sizeof(BpDB));
	CHKNULL(sdr_begin_xn(sdr));	/*	To lock memory.		*/
	for (elt = sdr_list_first(sdr, db.saga[regionIdx]); elt; elt = nextElt)
	{
		nextElt = sdr_list_next(sdr, elt);
		if (insertIntoPredictionBase(pb, elt) < 0)
		{
			putErrmsg("Can't insert into prediction base.", NULL);
			lyst_destroy(pb);
			sdr_exit_xn(sdr);
			return NULL;
		}
	}

	sdr_exit_xn(sdr);
	return pb;
}

static int	processSequence(LystElt start, LystElt end, time_t currentTime,
			int regionIdx)
{
	Sdr		sdr = getIonsdr();
	Object		dbobj = getBpDbObject();
	BpDB		db;
	Object		iondbObj = getIonDbObject();
	IonDB		iondb;
	uvast		fromNode;
	uvast		toNode;
	time_t		horizon;
	LystElt		elt;
	PbContact	*contact;

	/*	NOTE: may need to change these vast and uvast
	 *	variables to double, for platforms on which uvast
	 *	is only 32 bits.					*/

	uvast		totalCapacity = 0;
	uvast		totalContactDuration = 0;
	unsigned int	contactsCount = 0;
	uvast		totalGapDuration = 0;
	unsigned int	gapsCount = 0;
	LystElt		prevElt;
	PbContact	*prevContact;
	uvast		gapDuration;
	uvast		meanContactDuration;
	uvast		meanGapDuration;
	vast		deviation;
	uvast		contactDeviationsTotal = 0;
	uvast		gapDeviationsTotal = 0;
	uvast		contactStdDev;
	uvast		gapStdDev;
	float		contactDoubt;
	float		gapDoubt;
	int		confidenceBasis;
	float		contactConfidence;
	float		gapConfidence;
	float		netConfidence;
	size_t		xmitRate;
	PsmAddress	cxaddr;
#ifdef SAGA_DEBUG
char	buf[255];
#endif

	if (start == NULL)	/*	No sequence found yet.		*/
	{
		return 0;
	}

	sdr_read(sdr, (char *) &db, dbobj, sizeof(BpDB));
	contact = (PbContact *) lyst_data(start);
	fromNode = contact->fromNode;
	toNode = contact->toNode;
	horizon = currentTime + (currentTime - contact->fromTime);
#ifdef SAGA_DEBUG
writeTimestampLocal(currentTime, buf);
printf("Current time: %s\n", buf);
writeTimestampLocal(horizon, buf);
printf("Horizon: %s\n", buf);
#endif

	/*	Compute totals and means.				*/

	elt = start;
	prevElt = NULL;
	while (1)
	{
#ifdef SAGA_DEBUG
printf("Contact capacity " UVAST_FIELDSPEC ", duration " UVAST_FIELDSPEC ".\n",
contact->capacity, contact->duration);
#endif
		totalCapacity += contact->capacity;
		totalContactDuration += contact->duration;
		contactsCount++;
		if (prevElt)
		{
			prevContact = (PbContact *) lyst_data(prevElt);
			gapDuration = contact->fromTime - prevContact->toTime;
#ifdef SAGA_DEBUG
printf("Gap duration " UVAST_FIELDSPEC ".\n", gapDuration);
#endif
			totalGapDuration += gapDuration;
			gapsCount++;
		}

		prevElt = elt;
		if (lyst_data(elt) == lyst_data(end))
		{
			/*	Have reached end of sequence; no
			 *	further discovered contacts for
			 *	this from/to node pair.			*/

			break;
		}

		elt = lyst_next(elt);
		contact = (PbContact *) lyst_data(elt);
	}

	if (gapsCount == 0)
	{
#ifdef SAGA_DEBUG
puts("No gaps in contact log, can't predict contacts.");
#endif
		return 0;
	}

	meanContactDuration = totalContactDuration / contactsCount;
	meanGapDuration = totalGapDuration / gapsCount;
#ifdef SAGA_DEBUG
printf("Mean contact duration " UVAST_FIELDSPEC ", mean gap duration "
UVAST_FIELDSPEC ".\n", meanContactDuration, meanGapDuration);
#endif

	/*	Compute standard deviations.				*/

	contact = (PbContact *) lyst_data(start);
	elt = start;
	prevElt = NULL;
	while (1)
	{
		deviation = contact->duration - meanContactDuration;
		contactDeviationsTotal += (deviation * deviation);
		if (prevElt)
		{
			prevContact = (PbContact *) lyst_data(prevElt);
			gapDuration = contact->fromTime - prevContact->toTime;
			deviation = gapDuration - meanGapDuration;
			gapDeviationsTotal += (deviation * deviation);
		}

		prevElt = elt;
		if (lyst_data(elt) == lyst_data(end))
		{
			break;
		}

		elt = lyst_next(elt);
		contact = (PbContact *) lyst_data(elt);
	}

	contactStdDev = sqrt(contactDeviationsTotal / contactsCount);
	gapStdDev = sqrt(gapDeviationsTotal / gapsCount);
#ifdef SAGA_DEBUG
printf("Contact duration sigma " UVAST_FIELDSPEC ", gap duration sigma "
UVAST_FIELDSPEC ".\n", contactStdDev, gapStdDev);
#endif

	/*	Compute net confidence for predicted contact between
	 *	these two nodes.					*/

	if (contactStdDev > meanContactDuration)
	{
		contactDoubt = 1.0;
	}
	else
	{
		contactDoubt = MAX(0.1,
			((double) contactStdDev) / meanContactDuration);
	}

#ifdef SAGA_DEBUG
printf("Contact doubt %f.\n", contactDoubt);
#endif
	if (gapStdDev > meanGapDuration)
	{
		gapDoubt = 1.0;
	}
	else
	{
		gapDoubt = MAX(0.1, ((double) gapStdDev) / meanGapDuration);
	}

#ifdef SAGA_DEBUG
printf("Gap doubt %f.\n", gapDoubt);
#endif
	confidenceBasis = sdr_list_length(sdr, db.saga[regionIdx]);
	contactConfidence = 1.0 - powf(contactDoubt,
			contactsCount / confidenceBasis);
#ifdef SAGA_DEBUG
printf("Contact confidence %f.\n", contactConfidence);
#endif
	gapConfidence = 1.0 - powf(gapDoubt, gapsCount / confidenceBasis);
#ifdef SAGA_DEBUG
printf("Gap confidence %f.\n", gapConfidence);
#endif
	netConfidence = MAX(((contactConfidence + gapConfidence) / 2.0), .01);
#ifdef SAGA_DEBUG
printf("Net confidence %f.\n", netConfidence);
#endif

	/*	Insert predicted contact (aggregate).  Note that we
	 *	indicate to rfx_insert_contact that this is a Predicted
	 *	contact by setting both From and To times to the
	 *	horizon time.  The From time will be corrected to
	 *	the current time by rfx_insert_contact.			*/

	xmitRate = totalCapacity / (horizon - currentTime);
	sdr_read(sdr, (char *) &iondb, iondbObj, sizeof(IonDB));
	if (xmitRate > 1)
	{
		if (rfx_insert_contact(iondb.regions[regionIdx].regionNbr,
				horizon, horizon, fromNode, toNode,
				xmitRate, netConfidence, &cxaddr, 0) < 0
		|| cxaddr == 0)
		{
			putErrmsg("Can't insert predicted contact.", NULL);
			return -1;
		}
	}

	return 0;
}

static int	predictContacts(int idx)
{
	time_t		currentTime = getCtime();
	Lyst		predictionBase;
	LystElt		elt;
	PbContact	*contact;
	LystElt		startOfSequence = NULL;
	LystElt	 	endOfSequence = NULL;
	uvast		sequenceFromNode = 0;
	uvast		sequenceToNode = 0;
	int		result = 0;

	/*	First, construct a prediction base from the current
	 *	contents of the indicated saga (containing all
	 *	discovered contacts that are known to have occurred
	 *	in the past within the indicated region).		*/

	predictionBase = constructPredictionBase(idx);
	if (predictionBase == NULL)
	{
		putErrmsg("Can't predict contacts.", NULL);
		return -1;
	}

	/*	Now generate predicted contacts from the prediction
	 *	base.							*/

	for (elt = lyst_first(predictionBase); elt; elt = lyst_next(elt))
	{
		contact = (PbContact *) lyst_data(elt);
		if (contact->fromNode > sequenceFromNode
		|| contact->toNode > sequenceToNode)
		{
			if (processSequence(startOfSequence, endOfSequence,
					currentTime, idx) < 0)
			{
				putErrmsg("Can't predict contacts.", NULL);
				lyst_destroy(predictionBase);
				return -1;
			}

			/*	Note start of new sequence.		*/

			sequenceFromNode = contact->fromNode;
			sequenceToNode = contact->toNode;
			startOfSequence = elt;
		}

		/*	Continuation of current prediction sequence.	*/

		endOfSequence = elt;
	}

	/*	Process the last sequence in the prediction base.	*/

	if (processSequence(startOfSequence, endOfSequence, currentTime, idx)
			< 0)
	{
		putErrmsg("Can't predict contacts.", NULL);
		result = -1;
	}

	lyst_destroy(predictionBase);
	return result;
}

/*	*	Saga management functions	*	*	*	*/

void	saga_insert(time_t fromTime, time_t toTime, uvast fromNode,
		uvast toNode, size_t xmitRate, int idx)
{
	Sdr		sdr = getIonsdr();
	Object		dbobj = getBpDbObject();
	BpDB		db;
	Object		saga;
	Encounter	encounter;
	Object		encounterObj;
#ifdef SAGA_DEBUG
char	buf1[64];
char	buf2[64];

writeTimestampLocal(fromTime, buf1);
writeTimestampLocal(toTime, buf2);
printf("Inserting new encounter into saga, from "
UVAST_FIELDSPEC " to " UVAST_FIELDSPEC ", start %s, stop %s.\n",
fromNode, toNode, buf1, buf2);
#endif
	/*	Saga is not sorted in any way and might contain
	 *	duplicate encounters.  Construction of the
	 *	sorted prediction base scrubs the saga, causing
	 *	removal of any duplicate encounters inserted
	 *	from other nodes' saga messages; any additional
	 *	encounters inserted in the course of the local
	 *	node's own contact discovery operations will not
	 *	duplicate anything.					*/

	sdr_read(sdr, (char *) &db, dbobj, sizeof(BpDB));
	saga = db.saga[idx];
	encounter.fromTime = fromTime;
	encounter.toTime = toTime;
	encounter.fromNode = fromNode;
	encounter.toNode = toNode;
	encounter.xmitRate = xmitRate;
	encounterObj = sdr_malloc(sdr, sizeof(Encounter));
	if (encounterObj)
	{
		sdr_write(sdr, encounterObj, (char *) &encounter,
				sizeof(Encounter));
		oK(sdr_list_insert_last(sdr, saga, encounterObj));
	}
}

int	saga_ingest(int regionIdx)
{
	/*	First remove all predicted contacts from contact plan.	*/

	if (removePredictedContacts(regionIdx) < 0)
	{
		putErrmsg("Can't remove predicted contacts.", NULL);
		return -1;
	}

	/*	Then compute all new predicted contacts.		*/

	if (predictContacts(regionIdx) < 0)
	{
		putErrmsg("Can't predict contacts.", NULL);
		return -1;
	}

	return 0;
}

int	saga_send(uvast destinationNodeNbr, int regionIdx)
{
	Sdr		sdr = getIonsdr();
	BpDB		*bpConstants = getBpConstants();
	char		ownEid[32];
	MetaEid		ownMetaEid;
	VScheme		*vscheme;
	PsmAddress	vschemeElt;
	char		destEid[32];
	time_t		timeFloor;
	int		maxSaga;
	unsigned char	*buffer;
	unsigned char	*cursor;
	IonDB		iondb;
	uvast		uvtemp;
	Object		encounterElt;
	Object		nextEncounterElt;
	Object		encounterAddr;
	Encounter	encounter;
	int		aduLength;
	Object		aduObj;
	Object		aduZco;

	isprintf(ownEid, sizeof(ownEid), "ipn:" UVAST_FIELDSPEC ".0",
			getOwnNodeNbr());
	oK(parseEidString(ownEid, &ownMetaEid, &vscheme, &vschemeElt));
	isprintf(destEid, sizeof(destEid), "ipn:" UVAST_FIELDSPEC ".0",
			destinationNodeNbr);

	/*	Set cutoff to current time minus 30 days.		*/

	timeFloor = getCtime();
	timeFloor -= (86400 * 30);

	/*	Create buffer for serializing saga message.		*/

	maxSaga = 1	/*	admin record array (2 items)		*/
		+ 2	/*	record type, indefinite-length array	*/
		+ (sdr_list_length(sdr, bpConstants->saga[regionIdx])
			* (1 + (5 * 45))) 	/*	per encounter	*/
		+ 1;	/*	break character				*/
	buffer = MTAKE(maxSaga);
	if (buffer == NULL)
	{
		putErrmsg("Can't allocate buffer for saga.", NULL);
		return -1;
	}

	cursor = buffer;

	/*	Sending an admin record, an array of 2 items.		*/

	uvtemp = 2;
	oK(cbor_encode_array_open(uvtemp, &cursor));

	/*	First item of admin record is record type code.		*/

	uvtemp = BP_SAGA_MESSAGE;
	oK(cbor_encode_integer(uvtemp, &cursor));

	/*	Second item of admin record is content, the saga
	 *	message, which is an indefinite-length array.		*/

	uvtemp = -1;
	oK(cbor_encode_array_open(uvtemp, &cursor));

	/*	First item of array is the applicable region number.	*/

	sdr_read(sdr, (char *) &iondb, getIonDbObject(), sizeof(IonDB));
	uvtemp = iondb.regions[regionIdx].regionNbr;
	oK(cbor_encode_integer(uvtemp, &cursor));

	/*	Next append one definite-length array of 5 integers
	 *	for every Encounter in the saga, except that Encounters
	 *	that have aged out are simply deleted.			*/

	for (encounterElt = sdr_list_first(sdr, bpConstants->saga[regionIdx]);
			encounterElt; encounterElt = nextEncounterElt)
	{
		nextEncounterElt = sdr_list_next(sdr, encounterElt);
		encounterAddr = sdr_list_data(sdr, encounterElt);
		sdr_read(sdr, (char *) &encounter, encounterAddr,
				sizeof(Encounter));
		if (encounter.toTime < timeFloor)
		{
			sdr_free(sdr, encounterAddr);
			sdr_list_delete(sdr, encounterElt, NULL, NULL);
			continue;
		}

		/*	This encounter has not yet aged out.		*/

		uvtemp = 5;
		oK(cbor_encode_array_open(uvtemp, &cursor));
		uvtemp = encounter.fromNode;
		oK(cbor_encode_integer(uvtemp, &cursor));
		uvtemp = encounter.toNode;
		oK(cbor_encode_integer(uvtemp, &cursor));
		uvtemp = encounter.fromTime;
		oK(cbor_encode_integer(uvtemp, &cursor));
		uvtemp = encounter.toTime;
		oK(cbor_encode_integer(uvtemp, &cursor));
		uvtemp = encounter.xmitRate;
		oK(cbor_encode_integer(uvtemp, &cursor));
	}

	/*	Finally, end the indefinite array.			*/

	oK(cbor_encode_break(&cursor));

	/*	Now wrap the record buffer in a ZCO and send it to
	 *	the destination node.					*/

	aduLength = cursor - buffer;
	oK(sdr_begin_xn(sdr));
	aduObj = sdr_malloc(sdr, aduLength);
	if (aduObj == 0)
	{
		sdr_cancel_xn(sdr);
		putErrmsg("Can't create saga message.", NULL);
		return -1;
	}

	sdr_write(sdr, aduObj, (char *) buffer, aduLength);
	MRELEASE(buffer);
	aduZco = ionCreateZco(ZcoSdrSource, aduObj, 0, aduLength,
			BP_STD_PRIORITY, 0, ZcoOutbound, NULL);
	if (aduZco == 0 || aduZco == (Object) ERROR)
	{
		sdr_cancel_xn(sdr);
		putErrmsg("Failed creating saga message ZCO.", NULL);
		return -1;
	}

	if (sdr_end_xn(sdr) < 0)
	{
		putErrmsg("Failed sending saga message.", NULL);
		return -1;
	}

	/*	Note: time-to-live must be in milliseconds for BP
	 *	processing.  Hard-coded time-to-live is 1 minute.	*/

	if (bpSend(&ownMetaEid, destEid, NULL, 60000, BP_STD_PRIORITY,
			NoCustodyRequested, 0, 0, NULL, aduZco, NULL,
			BP_SAGA_MESSAGE) <= 0)
	{
		writeMemo("[?] Unable to send saga message.");
	}

	return 0;
}

int	saga_receive(BpDelivery *dlv, unsigned char *cursor,
		unsigned int unparsedBytes)
{
	Sdr		sdr = getIonsdr();
	uvast		uvtemp;
	uint32_t	regionNbr;
	int		regionIdx;
	int		majorType;
	int		additionalInfo;
	time_t		fromTime;
	time_t		toTime;
	uvast		fromNode;
	uvast		toNode;
	size_t		xmitRate;
	BpDB		*bpConstants = getBpConstants();
	Object		bundleElt;
	Object		nextBundleElt;

	uvtemp = 0;
	if (cbor_decode_array_open(&uvtemp, &cursor, &unparsedBytes) < 0)
	{
		writeMemo("[?] Can't decode saga array.");
		return 0;
	}

	/*	Get region number.					*/

	if (cbor_decode_integer(&uvtemp, CborAny, &cursor, &unparsedBytes) < 0)
	{
		writeMemo("[?] Can't decode saga region number.");
		return 0;
	}

	regionNbr = uvtemp;
	regionIdx = ionPickRegion(regionNbr);
	if (regionIdx < 0 || regionIdx > 1)
	{
		/*	This message is for a region of which the
		 *	local node is not a member, so the message
		 *	is not relevant.				*/

		return 0;
	}

	/*	Now extract all of the encounters in the saga.		*/

	oK(sdr_begin_xn(sdr));
	while (1)
	{
		if (cbor_decode_initial_byte(&cursor, &unparsedBytes,
				&majorType, &additionalInfo) < 0)
		{
			writeMemo("[?] Can't decode saga encounter.");
			return 0;
		}

		if (majorType == CborSimpleValue && additionalInfo == 31)
		{
			break;	/*	End of indefinite-length array.	*/
		}

		if (majorType != CborArray || additionalInfo != 5)
		{
			writeMemo("[?] Malformed saga encounter.");
			return 0;
		}

		if (cbor_decode_integer(&uvtemp, CborAny, &cursor,
				&unparsedBytes) < 0)
		{
			writeMemo("[?] Can't decode encounter fromTime.");
			return 0;
		}

		fromTime = uvtemp;
		if (cbor_decode_integer(&uvtemp, CborAny, &cursor,
				&unparsedBytes) < 0)
		{
			writeMemo("[?] Can't decode encounter toTime.");
			return 0;
		}

		toTime = uvtemp;
		if (cbor_decode_integer(&uvtemp, CborAny, &cursor,
				&unparsedBytes) < 0)
		{
			writeMemo("[?] Can't decode encounter fromNode.");
			return 0;
		}

		fromNode = uvtemp;
		if (cbor_decode_integer(&uvtemp, CborAny, &cursor,
				&unparsedBytes) < 0)
		{
			writeMemo("[?] Can't decode encounter toNode.");
			return 0;
		}

		toNode = uvtemp;
		if (cbor_decode_integer(&uvtemp, CborAny, &cursor,
				&unparsedBytes) < 0)
		{
			writeMemo("[?] Can't decode encounter xmitRate.");
			return 0;
		}

		xmitRate = uvtemp;
		saga_insert(fromTime, toTime, fromNode, toNode, xmitRate,
				regionIdx);
	}

	/*	Now call saga_ingest to erase all existing predicted
	 *	contacts and compute new (hopefully better and/or
	 *	additional) ones.					*/

	if (saga_ingest(regionIdx) < 0)
	{
		putErrmsg("Can't process contact discovery msg from neighbor.",
				NULL);
		sdr_cancel_xn(sdr);
		return -1;
	}

	/*	Finally, release all bundles from limbo, to take
	 *	advantage of the new predicted contacts.		*/

	for (bundleElt = sdr_list_first(sdr, bpConstants->limboQueue);
			bundleElt; bundleElt = nextBundleElt)
	{
		nextBundleElt = sdr_list_next(sdr, bundleElt);
		if (releaseFromLimbo(bundleElt, 0) < 0)
		{
			putErrmsg("Failed releasing bundle from limbo.", NULL);
			sdr_cancel_xn(sdr);
			return-1;
		}
	}

	if (sdr_end_xn(sdr) < 0)
	{
		putErrmsg("Failed recording saga encounter.", NULL);
		return -1;
	}

	return 0;
}

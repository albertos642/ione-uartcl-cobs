/*
	imcfw.c:	scheme-specific forwarder for the "imc"
			scheme, used for Interplanetary Multicast.

	Author: Scott Burleigh, JPL

	Copyright (c) 2012, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
									*/
#include "ipnfw.h"
#include "bei.h"
#include "imcfwP.h"

typedef struct
{
	uvast	entryNode;
	Lyst	members;
} ImcGang;

#ifndef CGR_DEBUG
#define CGR_DEBUG	0
#endif

#if CGR_DEBUG == 1
static void	printCgrTraceLine(void *data, unsigned int lineNbr,
			CgrTraceType traceType, ...)
{
	va_list args;
	const char *text;

	va_start(args, traceType);
	text = cgr_tracepoint_text(traceType);
	vprintf(text, args);
	switch (traceType)
	{
	case CgrIgnoreContact:
	case CgrExcludeRoute:
	case CgrSkipRoute:
		fputc(' ', stdout);
		fputs(cgr_reason_text(va_arg(args, CgrReason)), stdout);
	default:
		break;
	}

	putchar('\n');
	fflush(stdout);
	va_end(args);
}
#endif

static sm_SemId		_imcfwSemaphore(sm_SemId *newValue)
{
	uaddr		temp;
	void		*value;
	sm_SemId	sem;

	if (newValue)			/*	Add task variable.	*/
	{
		temp = *newValue;
		value = (void *) temp;
		value = sm_TaskVar(&value);
	}
	else				/*	Retrieve task variable.	*/
	{
		value = sm_TaskVar(NULL);
	}

	temp = (uaddr) value;
	sem = temp;
	return sem;
}

static void	shutDown(int signum)
{
	isignal(SIGTERM, shutDown);
	sm_SemEnd(_imcfwSemaphore(NULL));
}

/*	*	imcfw clock thread functions	*	*	*	*/

static void	destroyGroup(Object groupElt)
{
	Sdr	sdr = getIonsdr();
	Object	groupAddr;
		OBJ_POINTER(ImcGroup, group);

	groupAddr = sdr_list_data(sdr, groupElt);
	GET_OBJ_POINTER(sdr, ImcGroup, group, groupAddr);
	sdr_list_destroy(sdr, group->members, NULL, NULL);
	sdr_free(sdr, groupAddr);
	sdr_list_delete(sdr, groupElt, NULL, NULL);
}

static void	*imcClock(void *parm)
{
	int		*running = (int *) parm;
	ImcDB		*imcConstants = getImcConstants();
	Sdr		sdr = getIonsdr();
	Object		elt;
	Object		nextElt;
	Object		groupAddr;
	ImcGroup	group;

	/*	Main loop for time-driven IMC functionality.		*/

	iblock(SIGTERM);
	while (*running)
	{
		/*	Destroy unused groups.				*/

		CHKNULL((sdr_begin_xn(sdr)));
		for (elt = sdr_list_first(sdr, imcConstants->groups); elt;
				elt = nextElt)
		{
			nextElt = sdr_list_next(sdr, elt);
			groupAddr = sdr_list_data(sdr, elt);
			sdr_stage(sdr, (char *) &group, groupAddr,
					sizeof(ImcGroup));
			switch (group.secUntilDelete)
			{
			case -1:	/*	Still has members.	*/
				continue;

			case 0:
				destroyGroup(elt);
				continue;

			default:
				group.secUntilDelete--;
				sdr_write(sdr, groupAddr, (char *) &group,
					sizeof(ImcGroup));
			}
		}

		if (sdr_end_xn(sdr) < 0)
		{
			putErrmsg("imcClock failed.", NULL);
			shutDown(SIGTERM);
			*running = 0;
			continue;
		}

		snooze(1);
	}

	writeErrmsgMemos();
	writeMemo("[i] imcClock thread has ended.");
	return NULL;
}

/*	*	imcfw main thread functions	*	*	*	*/

static int	relayImcBundle(Bundle *bundle, Object bundleAddr,
			ExtensionBlock *imcblock, Object imcblkAddr)
{
	Sdr	sdr = getIonsdr();
	uvast	ownNodeNbr = getOwnNodeNbr();
	int	destinationsCount;
	char	*nodeNbrsArray;
	int	i;
	uvast	*nodeNbrPtr;

	/*	Load the bundle's list of destinations from the
	 *	array of gang members in the bundle's IMC extension
	 *	block, except for self.					*/

	destinationsCount = imcblock->size / sizeof(uvast);
	if (destinationsCount < 1)
	{
		writeMemo("[?] IMC block has no destinations.");
#if IMCDEBUG
writeMemo("no destinations");
#endif
		oK(bpAbandon(bundleAddr, bundle, BP_REASON_NO_ROUTE));
		return 0;
	}

	nodeNbrsArray = MTAKE(imcblock->size);
	if (nodeNbrsArray == NULL)
	{
		putErrmsg("Can't read node numbers array.",
				itoa(destinationsCount));
		return -1;
	}

	sdr_read(sdr, nodeNbrsArray, imcblock->object, imcblock->size);
	nodeNbrPtr = (uvast *) nodeNbrsArray;
#if IMCDEBUG
writeMemo("Preparing to relay IMC bundle.");
#endif
	for (i = 0; i < destinationsCount; i++, nodeNbrPtr++)
	{
#if IMCDEBUG
writeMemoNote("Checking IMC block destination", itoa(*nodeNbrPtr));
#endif
		if (*nodeNbrPtr == ownNodeNbr)
		{
			/*	Omit self from destinations list.	*/

			continue;
		}

		/*	Load this destination into the bundle.		*/

		if (imcLoadDestination(bundle, *nodeNbrPtr) < 0)
		{
			MRELEASE(nodeNbrsArray);
			putErrmsg("Can't load from IMC extension block.", NULL);
			return -1;
		}
	}

	MRELEASE(nodeNbrsArray);

	/*	Reinitialize the IMC extension block.			*/

	bundle->extensionsLength -= imcblock->length;
	sdr_write(sdr, bundleAddr, (char *) bundle, sizeof(Bundle));
	sdr_free(sdr, imcblock->bytes);
	imcblock->bytes = 0;
	imcblock->length = 0;
	sdr_free(sdr, imcblock->object);
	imcblock->object = 0;
	imcblock->size = 1;
	sdr_write(sdr, imcblkAddr, (char *) imcblock, sizeof(ExtensionBlock));

	/*	Don't forward the bundle if there are no remaining
	 *	destinations.						*/

	if (sdr_list_length(sdr, bundle->destinations) == 0)
	{
		/*	No remaining gang members to forward to.	*/

		return bpDestroyBundle(bundleAddr, 2);
	}

	/*	Forward the bundle.					*/

	return imcForwardBundle(bundle, bundleAddr);
}

static int	loadRegionMembers(Bundle *bundle, uint32_t regionNbr, IonDB *db)
{
	Sdr		sdr = getIonsdr();
	Object		elt;
	Object		memberAddr;
	RegionMember	member;

#if IMCDEBUG
writeMemo("In loadRegionMembers...");
#endif
	for (elt = sdr_list_first(sdr, db->rolodex); elt;
			elt = sdr_list_next(sdr, elt))
	{
		memberAddr = sdr_list_data(sdr, elt);
		sdr_read(sdr, (char *) &member, memberAddr,
				sizeof(RegionMember));
#if IMCDEBUG
writeMemoNote("Checking rolodex member", itoa(member.nodeNbr));
#endif
		if (member.homeRegionNbr == regionNbr
		|| member.outerRegionNbr == regionNbr)
		{
			if (imcLoadDestination(bundle, member.nodeNbr) < 0)
			{
				putErrmsg("Can't add region member.", NULL);
				return -1;
			}
		}
	}

	return 0;
}

static int	loadRolodexMembers(Bundle *bundle, IonDB *db)
{
	Sdr		sdr = getIonsdr();
	Object		elt;
	Object		memberAddr;
	RegionMember	member;

#if IMCDEBUG
writeMemo("In loadRolodexMembers...");
#endif
	for (elt = sdr_list_first(sdr, db->rolodex); elt;
			elt = sdr_list_next(sdr, elt))
	{
		memberAddr = sdr_list_data(sdr, elt);
		sdr_read(sdr, (char *) &member, memberAddr,
				sizeof(RegionMember));
		if (imcLoadDestination(bundle, member.nodeNbr) < 0)
		{
			putErrmsg("Can't add region member.", NULL);
			return -1;
		}
	}

	return 0;
}

static int	originateImcBundle(Bundle *bundle, Object bundleAddr)
{
	Sdr		sdr = getIonsdr();
	uvast		ownNodeNbr = getOwnNodeNbr();
	Object		iondbObj;
	IonDB		iondb;
	uint32_t	regionNbr;
	int		regionIdx;
	uvast		groupNbr;
	Object		groupAddr;
	Object		groupElt;
	ImcGroup	group;
	Object		elt;
	uvast		nodeNbr;

	groupNbr = bundle->destination.ssp.imc.groupNbr;

	/*	Load the bundle's list of destinations, either from
	 *	region membership (for a dispatch) or from group
	 *	membership (for an application multicast message).	*/

	if (groupNbr == 0)
	{
		/*	Broadcast to region members.			*/
#if IMCDEBUG
writeMemo("Multicasting to region members.");
#endif
		regionNbr = bundle->ancillaryData.imcRegionNbr;
		iondbObj = getIonDbObject();
		sdr_read(sdr, (char *) &iondb, iondbObj, sizeof(IonDB));
		if (regionNbr == 0)	/*	Fwd in both regions.	*/
		{
#if IMCDEBUG
writeMemo("Sending to all members of both regions.");
#endif
			/*	Send to all members of both home
			 *	region and (if any) outer region.	*/

			if (loadRolodexMembers(bundle, &iondb) < 0)
			{
				putErrmsg("IMC can't add members.", NULL);
				return -1;
			}
		}
		else			/*	Fwd within this region.	*/
		{
#if IMCDEBUG
writeMemoNote("Sending to all members of region", itoa(regionNbr));
#endif
			/*	Send only to all members of the
			 *	specified region.			*/

			regionIdx = ionPickRegion(regionNbr);
#if IMCDEBUG
writeMemoNote("regionIdx is", itoa(regionIdx));
#endif
			if (regionIdx < 0)
			{
				putErrmsg("Not a member of region.",
						itoa(regionNbr));
				return 0;
			}

#if IMCDEBUG
writeMemoNote("regions[regionIdx].regionNbr is", itoa(iondb.regions[regionIdx].regionNbr));
#endif
			if (loadRegionMembers(bundle, 
				iondb.regions[regionIdx].regionNbr, &iondb) < 0)
			{
				putErrmsg("IMC can't add region members.",
						NULL);
				return -1;
			}
		}
#if IMCDEBUG
writeMemoNote("Multicasting message to members",
itoa(sdr_list_length(sdr, bundle->destinations)));
writeMemoNote("...of region", itoa(regionNbr));
#endif
	}
	else			/*	Multicast to group members.	*/
	{
#if IMCDEBUG
writeMemoNote("Multicasting to members of group", itoa(groupNbr));
#endif
		imcFindGroup(groupNbr, &groupAddr, &groupElt);
		if (groupElt == 0)
		{
			/*	Nobody subscribes to bundles destined
			 *	for this group.				*/
#if IMCDEBUG
writeMemo("no such group");
#endif
			oK(bpAbandon(bundleAddr, bundle, BP_REASON_NO_ROUTE));
			return 0;
		}

		sdr_read(sdr, (char *) &group, groupAddr, sizeof(ImcGroup));
#if IMCDEBUG
writeMemoNote("Number of members in group",
itoa(sdr_list_length(sdr, group.members)));
#endif
		for (elt = sdr_list_first(sdr, group.members); elt;
				elt = sdr_list_next(sdr, elt))
		{
			nodeNbr = sdr_list_data(sdr, elt);
			if (nodeNbr == ownNodeNbr)
			{
				if (group.isMember == 0)
				{
					/*	Only an "ex officio"
					 *	(passageway) member of
					 *	this multicast group.
					 *	Omit from destinations.	*/
	
					continue;
				}
			}
#if IMCDEBUG
writeMemoNote("Loading group member", itoa(nodeNbr));
#endif
			if (imcLoadDestination(bundle, nodeNbr) < 0)
			{
				putErrmsg("Can't add IMC group member.", NULL);
				return -1;
			}
		}
	}

	/*	Forward the bundle.					*/

	return imcForwardBundle(bundle, bundleAddr);
}

#if defined (ION_LWT)
int	imcfw(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
#else
int	main(int argc, char *argv[])
{
#endif
	int		running = 1;
	Sdr		sdr;
	uvast		ownNodeNbr;
	VScheme		*vscheme;
	PsmAddress	vschemeElt;
	Scheme		scheme;
	pthread_t	clockThread;
	Object		elt;
	Object		bundleAddr;
	Bundle		bundle;
	Object		imcblkElt;
	Object		imcblkAddr;
	ExtensionBlock	imcblock;
	Object		iondbObj;
	IonDB		iondb;

	if (bpAttach() < 0)
	{
		putErrmsg("imcfw can't attach to BP.", NULL);
		return 1;
	}

	sdr = getIonsdr();
	if (imcInit() < 0)
	{
		putErrmsg("imcfw can't load IMC routing database.", NULL);
		return 1;
	}

	ownNodeNbr = getOwnNodeNbr();
	findScheme("imc", &vscheme, &vschemeElt);
	if (vschemeElt == 0)
	{
		putErrmsg("'imc' scheme is unknown.", NULL);
		return 1;
	}

	sdr_read(sdr, (char *) &scheme, sdr_list_data(sdr,
			vscheme->schemeElt), sizeof(Scheme));

	/*	Set up signal handling.  SIGTERM is shutdown signal.	*/

	oK(_imcfwSemaphore(&vscheme->semaphore));
	isignal(SIGTERM, shutDown);

	/*	Start the clock thread, for deleting empty multicast
	 *	groups and filling in new nodes on current multicast
	 *	group membership.					*/

	if (pthread_begin(&clockThread, NULL, imcClock, &running, "imcClock"))
	{
		putSysErrmsg("imcfw can't create clock thread", NULL);
		return 1;
	}

	/*	Main loop: wait until forwarding queue is non-empty,
	 *	then drain it.						*/

	writeMemo("[i] imcfw is running.");
	while (running && !(sm_SemEnded(vscheme->semaphore)))
	{
		/*	Wrapping forwarding in an SDR transaction
		 *	prevents race condition with bpclock (which
		 *	is destroying bundles as their TTLs expire).	*/

		CHKZERO(sdr_begin_xn(sdr));
		elt = sdr_list_first(sdr, scheme.forwardQueue);
		if (elt == 0)	/*	Wait for forwarding notice.	*/
		{
			sdr_exit_xn(sdr);
			if (sm_SemTake(vscheme->semaphore) < 0)
			{
				putErrmsg("Can't take forwarder semaphore.",
						NULL);
				running = 0;
			}

			continue;
		}

		bundleAddr = (Object) sdr_list_data(sdr, elt);
		sdr_stage(sdr, (char *) &bundle, bundleAddr, sizeof(Bundle));
		bundle.priority = bundle.classOfService;
		bundle.ordinal = bundle.ancillaryData.ordinal;
		bundle.qosFlags = bundle.ancillaryData.flags;

		/*	No override mechanism at this time.		*/

		sdr_list_delete(sdr, elt, NULL, NULL);
		bundle.fwdQueueElt = 0;

		/*	Clear the bundle's imc destinations list.	*/

		while ((elt = sdr_list_first(sdr, bundle.destinations)))
		{
			sdr_list_delete(sdr, elt, NULL, NULL);
		}

		/*	Must rewrite bundle to note removal of
		 *	fwdQueueElt, in case the bundle is abandoned
		 *	and bpDestroyBundle re-reads it from the
		 *	database.					*/

		sdr_write(sdr, bundleAddr, (char *) &bundle, sizeof(Bundle));

		/*	Is bundle being relayed or sourced?		*/

		imcblkElt = findExtensionBlock(&bundle, ImcDestinationsBlk, 0);
		if (imcblkElt == 0)
		{
			writeMemo("[?] IMC extension block is missing.");
			oK(bpAbandon(bundleAddr, &bundle, BP_REASON_NO_ROUTE));
			continue;
		}

		imcblkAddr = sdr_list_data(sdr, imcblkElt);
		sdr_stage(sdr, (char *) &imcblock, imcblkAddr,
				sizeof(ExtensionBlock));
		if (imcblock.object == 0)
		{
			/*	Bundle was never previously forwarded.	*/

			if (originateImcBundle(&bundle, bundleAddr) < 0)
			{
				putErrmsg("Can't source IMC bundle.", NULL);
				sdr_cancel_xn(sdr);
				running = 0;
				continue;
			}

			/*	If this multicast is unregistration of
			 *	the local node, that unregistration can
			 *	now proceed: the information rquired
			 *	in order to announce the unregistration
			 *	has been used and is no longer needed.	*/

			if (bundle.destination.ssp.imc.groupNbr == 0
			&& bundle.destination.ssp.imc.serviceNbr == 1)
			{
				iondbObj = getIonDbObject();
				if (iondbObj == 0)
				{
					putErrmsg("Can't load ION database.",
							NULL);
					sdr_cancel_xn(sdr);
					running = 0;
					continue;
				}

				sdr_stage(sdr, (char *) &iondb, iondbObj,
						sizeof(IonDB));
				if (iondb.regions[0].locked)
				{
#if IMCDEBUG
writeMemoNote("imcfw unlocking region", itoa(iondb.regions[0].regionNbr));
#endif
					iondb.regions[0].locked = 0;
					sdr_write(sdr, iondbObj,
						(char *) &iondb, sizeof(IonDB));
				}
				else if (iondb.regions[1].locked)
				{
#if IMCDEBUG
writeMemoNote("imcfw unlocking region", itoa(iondb.regions[1].regionNbr));
#endif
					iondb.regions[1].locked = 0;
					sdr_write(sdr, iondbObj,
						(char *) &iondb, sizeof(IonDB));
				}
			}
		}
		else	/*	Received from some node, possibly self.	*/
		{
			if (bundle.id.source.ssp.ipn.nodeNbr == ownNodeNbr)
			{
				if (bundle.clDossier.senderNodeNbr == 0)
				{
					/*	Received from unknown
					 *	node, can't safely
					 *	relay the bundle.	*/
#if IMCDEBUG
writeMemo("imcfw received bundle from unknown node");
#endif
					oK(bpAbandon(bundleAddr, &bundle,
						BP_REASON_NO_ROUTE));
				}
				else
				{
					/*	Bundle has been
					 *	locally delivered,
					 *	must now be destroyed:
					 *	it was either received
					 *	via loopback, in which
					 *	case no need to relay
				 	*	(self can't be on
					*	CGR path to any other
					*	node) or received
				 	*	from some other node,
					*	in which case relaying
					*	would introduce a
					*	routing loop.		*/

					oK(bpDestroyBundle(bundleAddr, 2));
				}
			}
			else	/*	Bundle sourced by another node.	*/
			{
				if (relayImcBundle(&bundle, bundleAddr,
						&imcblock, imcblkAddr) < 0)
				{
					putErrmsg("Can't relay IMC bundle.",
							NULL);
					sdr_cancel_xn(sdr);
					running = 0;
					continue;
				}
			}
		}

		if (sdr_end_xn(sdr) < 0)
		{
			putErrmsg("Can't forward IMC bundle.", NULL);
			sdr_cancel_xn(sdr);
			running = 0;
			continue;
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	/*	Stop the clock thread.					*/

	running = 0;
	pthread_join(clockThread, NULL);

	/*	Wrap up.						*/

	writeErrmsgMemos();
	writeMemo("[i] imcfw forwarder has ended.");
	ionDetach();
	return 0;
}

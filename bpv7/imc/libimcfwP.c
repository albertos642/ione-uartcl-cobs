/*
 *	libimcfwP.c:	functions enabling the implementation of
 *			a multicast forwarder for the IMC endpoint
 *			ID scheme.
 *
 *	Copyright (c) 2012, California Institute of Technology.
 *	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
 *	acknowledged.
 *
 *	Author: Scott Burleigh, JPL
 */

#include "imcfwP.h"
#include "ipnfw.h"
#include "bei.h"

#ifndef CGR_DEBUG
#define CGR_DEBUG	0
#endif

typedef struct
{
	uvast	entryNode;
	Lyst	members;
} ImcGang;

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

/*	*	*	Multicast group mgt functions	*	*	*/

static Object	createGroup(uvast groupNbr, Object nextGroup)
{
	Sdr		sdr = getIonsdr();
	ImcDB		*db = getImcConstants();
	ImcGroup	group;
	Object		addr;
	Object		elt = 0;	/*	Default.		*/

#if IMCDEBUG
writeMemoNote("Creating multicast group", itoa(groupNbr));
#endif
	group.groupNbr = groupNbr;
	group.secUntilDelete = -1;
	group.isMember = 0;
	group.members = sdr_list_create(sdr);
	group.count[0] = 0;
	group.count[1] = 0;
	addr = sdr_malloc(sdr, sizeof(ImcGroup));
	if (addr)
	{
		sdr_write(sdr, addr, (char *) &group, sizeof(ImcGroup));
		if (nextGroup)
		{
			elt = sdr_list_insert_before(sdr, nextGroup, addr);
		}
		else
		{
			elt = sdr_list_insert_last(sdr, db->groups, addr);
		}
	}

	return elt;
}

static Object	locateGroup(uvast groupNbr, Object *nextGroup)
{
	Sdr	sdr = getIonsdr();
	ImcDB	*imcdb = getImcConstants();;
	Object	elt;
		OBJ_POINTER(ImcGroup, group);

	if (nextGroup)
	{
		*nextGroup = 0;	/*	Default.		*/
	}

	for (elt = sdr_list_first(sdr, imcdb->groups); elt;
			elt = sdr_list_next(sdr, elt))
	{
		GET_OBJ_POINTER(sdr, ImcGroup, group, sdr_list_data(sdr, elt));
		if (group->groupNbr < groupNbr)
		{
			continue;
		}

		if (group->groupNbr > groupNbr)
		{
			if (nextGroup)
			{
				*nextGroup = elt;
			}

			break;		/*	Same as end of list.	*/
		}

		return elt;		/*	Found group.		*/
	}

	return 0;
}

void	imcFindGroup(uvast groupNbr, Object *addr, Object *eltp)
{
	Sdr	sdr = getIonsdr();
	Object	elt;
	Object	nextGroupElt;

	CHKVOID(addr);
	CHKVOID(eltp);
	CHKVOID(ionLocked());
	*eltp = 0;			/*	Default.		*/
	elt = locateGroup(groupNbr, &nextGroupElt);
	if (elt == 0)			/*	Not found.		*/
	{
		elt = createGroup(groupNbr, nextGroupElt);
	       	if (elt == 0)
		{
			putErrmsg("Can't create multicast group.", NULL);
			return;
		}
	}

	*addr = sdr_list_data(sdr, elt);
	*eltp = elt;
}

/*	*	Private multicast functions.	*	*	*	*/

int	imcLoadDestination(Bundle *bundle, uvast newNodeNbr)
{
	Sdr	sdr = getIonsdr();
	Object	elt;
	uvast	nodeNbr;

	/*	Ensure no duplication in destinations list.		*/

#if IMCDEBUG
writeMemoNote("Loading multicast destination", itoa(newNodeNbr));
#endif
	for (elt = sdr_list_first(sdr, bundle->destinations); elt;
			elt = sdr_list_next(sdr, elt))
	{
		nodeNbr = sdr_list_data(sdr, elt);
		if (nodeNbr < newNodeNbr)
		{
			continue;
		}

		if (nodeNbr == newNodeNbr)	/*	Duplicate.	*/
		{
#if IMCDEBUG
writeMemo("Duplicate.");
#endif
			return 0;
		}

		break;
	}

	if (elt)
	{
		if (sdr_list_insert_before(sdr, elt, newNodeNbr) == 0)
		{
			putErrmsg("Can't add node to destinations.", NULL);
			return -1;
		}
	}
	else
	{
		if (sdr_list_insert_last(sdr, bundle->destinations, newNodeNbr)
				== 0)
		{
			putErrmsg("Can't add node to destinations.", NULL);
			return -1;
		}
	}

#if IMCDEBUG
writeMemoNote("Adding node to multicast destinations list", itoa(newNodeNbr));
#endif
	return 0;
}

static void	deleteObject(LystElt elt, void *userData)
{
	void	*object = lyst_data(elt);

	if (object)
	{
		MRELEASE(object);
	}
}

static uvast 	getBestEntryNode(Bundle *bundle, IonNode *terminusNode,
			time_t atTime)
{
	IonVdb		*ionvdb = getIonVdb();
	CgrVdb		*cgrvdb = cgr_get_vdb();
	int		ionMemIdx;
	Lyst		bestRoutes;
	Lyst		excludedNodes;
	LystElt		elt;
	CgrRoute	*route;
#if CGR_DEBUG == 1
	CgrTrace	*trace = &(CgrTrace) { .fn = printCgrTraceLine };
#else
	CgrTrace	*trace = NULL;
#endif

	/*	Determine whether or not the contact graph for the
	 *	terminus node identifies one or more routes over
	 *	which the bundle may be sent in order to get it
	 *	delivered to the terminus node.  If so, return the
	 *	number of the entry node of the best route.		*/

	if (ionvdb->lastEditTime.tv_sec > cgrvdb->lastLoadTime.tv_sec
	|| (ionvdb->lastEditTime.tv_sec == cgrvdb->lastLoadTime.tv_sec
	    && ionvdb->lastEditTime.tv_usec > cgrvdb->lastLoadTime.tv_usec)) 
	{
		/*	Contact plan has been modified, so must discard
		 *	all route lists and reconstruct them as needed.	*/

		cgr_clear_vdb(cgrvdb);
		getCurrentTime(&(cgrvdb->lastLoadTime));
	}

	ionMemIdx = getIonMemoryMgr();
	bestRoutes = lyst_create_using(ionMemIdx);
	if (bestRoutes == NULL)
	{
		putErrmsg("Can't create list for route computation.", NULL);
		return 0;
	}

	lyst_delete_set(bestRoutes, deleteObject, NULL);
	excludedNodes = lyst_create_using(ionMemIdx);
	if (excludedNodes == NULL)
	{
		lyst_destroy(bestRoutes);
		putErrmsg("Can't create lists for route computation.", NULL);
		return 0;
	}

	/*	Must exclude sender of bundle from consideration as
	 *	a station on the route, to minimize routing loops.  	*/

	if (bundle->clDossier.senderNodeNbr != 0
	&& bundle->clDossier.senderNodeNbr != getOwnNodeNbr())
	{
		if (lyst_insert_last(excludedNodes, (void *)
			((uaddr) bundle->clDossier.senderNodeNbr)) == NULL)
		{
			putErrmsg("Can't exclude sender from routes.", NULL);
			lyst_destroy(excludedNodes);
			lyst_destroy(bestRoutes);
			return 0;
		}
	}

	/*	Consult the contact graph to identify the neighboring
	 *	node(s) to forward the bundle to.			*/

	if (terminusNode->routingObject == 0)
	{
		if (cgr_create_routing_object(terminusNode) < 0)
		{
			putErrmsg("Can't initialize routing object.", NULL);
			lyst_destroy(excludedNodes);
			lyst_destroy(bestRoutes);
			return 0;
		}
	}

	if (cgr_identify_best_routes(terminusNode, bundle, excludedNodes,
			atTime, NULL, trace, bestRoutes) < 0)
	{
		putErrmsg("Can't identify best route(s) for bundle.", NULL);
		lyst_destroy(excludedNodes);
		lyst_destroy(bestRoutes);
		return 0;
	}

	lyst_destroy(excludedNodes);
	elt = lyst_first(bestRoutes);
	if (elt)
	{
		route = (CgrRoute *) lyst_data_set(elt, NULL);
		lyst_destroy(bestRoutes);
#if IMCDEBUG
writeMemoNote("Computed best route to ", itoa(terminusNode->nodeNbr));
writeMemoNote("...begins with transmission to ", itoa(route->toNodeNbr));
#endif
		return route->toNodeNbr;
	}

	lyst_destroy(bestRoutes);
	return 0;
}

static uvast	getViaNode(Bundle *bundle, uvast destinationNodeNbr)
{
	Sdr		sdr = getIonsdr();
	IonVdb		*ionvdb = getIonVdb();
	IonNode		*node;
	PsmAddress	nextNode;
	uvast		viaNodeNbr;
	char		eid[MAX_EID_LEN + 1];
	VPlan		*vplan;
	PsmAddress	vplanElt;
	BpPlan		plan;

	node = findNode(ionvdb, destinationNodeNbr, &nextNode);
	if (node == NULL)
	{
		node = addNode(ionvdb, destinationNodeNbr);
		if (node == NULL)
		{
			putErrmsg("Can't add node.", NULL);
			return -1;
		}
	}

	viaNodeNbr = getBestEntryNode(bundle, node, getCtime());
	if (viaNodeNbr)
	{
		return viaNodeNbr;
	}

	/*	No luck using the contact graph to compute a route
	 *	to the destination node, so see if destination node
	 *	is a neighbor (not identified in the contact plan);
	 *	if so, direct transmission works.			*/

	isprintf(eid, sizeof eid, "ipn:" UVAST_FIELDSPEC ".0",
			destinationNodeNbr);
	findPlan(eid, &vplan, &vplanElt);
	if (vplanElt == 0)
	{
		return 0;
	}

	sdr_read(sdr, (char *) &plan, sdr_list_data(sdr, vplan->planElt),
			sizeof(BpPlan));
	if (plan.blocked)
	{
		return 0;
	}

	return destinationNodeNbr;
}

static void	deleteGang(LystElt elt, void *userData)
{
	ImcGang	*gang = (ImcGang *) lyst_data(elt);

	lyst_destroy(gang->members);
	MRELEASE(gang);
}

static int	addNodeToGang(Lyst gangs, uvast viaNode, uvast nodeNbr)
{
	LystElt	elt;
	ImcGang	*gang;

	for (elt = lyst_first(gangs); elt; elt = lyst_next(elt))
	{
		gang = (ImcGang *) lyst_data(elt);
		if (gang->entryNode < viaNode)
		{
			continue;
		}

		if (gang->entryNode == viaNode)
		{
			/*	Join this gang.				*/

#if IMCDEBUG
writeMemoNote("Adding node to gang", itoa(nodeNbr));
#endif
			if (lyst_insert_last(gang->members,
					(void *) ((uaddr) nodeNbr)) == NULL)
			{
				return -1;
			}

			return 0;
		}

		/*	Requisite gang not found.			*/

		break;
	}

	/*	Must create new gang.					*/
#if IMCDEBUG
writeMemo("Creating new gang.");
#endif
	gang = MTAKE(sizeof(ImcGang));
	if (gang == NULL)
	{
		return -1;
	}

	gang->entryNode = viaNode;
	gang->members = lyst_create_using(getIonMemoryMgr());
	if (gang->members == NULL)
	{
		return -1;
	}

	if (elt)
	{
		if (lyst_insert_before(elt, gang) == NULL)
		{
			return -1;
		}
	}
	else
	{
		if (lyst_insert_last(gangs, gang) == NULL)
		{
			return -1;
		}
	}

	/*	Now have got gang that this node can join.		*/
#if IMCDEBUG
writeMemoNote("Adding node to new gang", itoa(nodeNbr));
#endif
	if (lyst_insert_last(gang->members, (void *) ((uaddr) nodeNbr)) == NULL)
	{
		return -1;
	}

	return 0;
}

static int	enqueueToNeighbor(Bundle *bundle, Object bundleObj,
			uvast nodeNbr)
{
	char		eid[MAX_EID_LEN + 1];
	VPlan		*vplan;
	PsmAddress	vplanElt;

	isprintf(eid, sizeof eid, "ipn:" UVAST_FIELDSPEC ".0", nodeNbr);
#if IMCDEBUG
writeMemoNote("Preparing to send to neighbor", eid);
#endif
	findPlan(eid, &vplan, &vplanElt);
	if (vplanElt == 0)
	{
		return 0;
	}

#if IMCDEBUG
writeMemo("Sending to neighbor.");
#endif
	if (bpEnqueue(vplan, bundle, bundleObj) < 0)
	{
		putErrmsg("Can't enqueue bundle.", NULL);
		return -1;
	}

	return 0;
}

static int	enqueueBundle(Bundle *bundle, Object bundleObj, uvast nodeNbr)
{
	/*	Entry node for Gang must be a neighbor.			*/

	if (enqueueToNeighbor(bundle, bundleObj, nodeNbr) < 0)
	{
		putErrmsg("Can't send bundle to neighbor.", NULL);
		return -1;
	}

	if (bundle->planXmitElt)
	{
		/*	Enqueued.					*/

		return bpAccept(bundleObj, bundle);
	}

	/*	No plan for conveying bundle to this neighbor, so
	 *	must give up on forwarding it.				*/

#if IMCDEBUG
writeMemoNote("enqueueBundle to node", itoa(nodeNbr));
#endif
	return bpAbandon(bundleObj, bundle, BP_REASON_NO_ROUTE);
}

#if IMCDEBUG
static void	printContacts()
{
	Sdr		sdr = getIonsdr();
	PsmPartition	ionwm = getIonwm();
	IonVdb		*vdb = getIonVdb();
	PsmAddress	elt;
	PsmAddress	addr;
	char		buffer[1024];

	oK(sdr_begin_xn(sdr));
	for (elt = sm_rbt_first(ionwm, vdb->contactIndex); elt;
				elt = sm_rbt_next(ionwm, elt))
	{
		addr = sm_rbt_data(ionwm, elt);
		rfx_print_contact(addr, buffer);
		writeMemo(buffer);
	}

	sdr_exit_xn(sdr);
}
#endif

int	imcForwardBundle(Bundle *bundle, Object bundleAddr)
{
	Sdr		sdr = getIonsdr();
	unsigned int	memmgr = getIonMemoryMgr();
	uvast		ownNodeNbr = getOwnNodeNbr();
	Lyst		gangs;
	Object		elt;
	uvast		nodeNbr;
	int		regionIdx;
	uint32_t	regionNbr;
	uvast		viaNode = 0;
	LystElt		elt2;
	ImcGang		*gang;
	Bundle		newBundle;
	Object		newBundleObj;
	LystElt		elt3;

	gangs = lyst_create_using(memmgr);
	if (gangs == NULL)
	{
		putErrmsg("Can't create list of Gangs for CGR multicast.",
				NULL);
		return -1;
	}

	lyst_delete_set(gangs, deleteGang, NULL);

	/*	First, divide all of the bundle's destinations into
	 *	gangs.  Each gang is characterized by the entry node
	 *	number that is common to the best routes for
	 *	forwarding the bundle to all members of the gang.	*/

	for (elt = sdr_list_first(sdr, bundle->destinations); elt;
			elt = sdr_list_next(sdr, elt))
	{
		nodeNbr = sdr_list_data(sdr, elt);
#if IMCDEBUG
writeMemoNote("Outbound destination is", itoa(nodeNbr));
#endif
		regionIdx = ionRegionOf(nodeNbr, ownNodeNbr, &regionNbr);
		if (regionIdx < 0)
		{
			/*	Some other node will be forwarding
			 *	the bundle to this destination node,
			 *	or else it's impossble to forward
			 *	the bundle to this destination node.	*/
#if IMCDEBUG
writeMemoNote("No common region for node", itoa(nodeNbr));
#endif
			continue;
		}

		viaNode = getViaNode(bundle, nodeNbr);
		if (viaNode == 0)
		{
			/*	No way to get the bundle to this
			 *	destination.				*/
#if IMCDEBUG
writeMemoNote("No via node for node", itoa(nodeNbr));
printContacts();
#endif
			continue;
		}

		/*	Add this node to the gang headed by this
		 *	viaNode.					*/

		if (addNodeToGang(gangs, viaNode, nodeNbr) < 0)
		{
			putErrmsg("Can't add node to gang.", NULL);
			lyst_destroy(gangs);
			return -1;
		}
	}

	/*	Then, for each gang, clone the bundle and set the
	 *	destinations list of the clone to all and only the
	 *	members of the gang, then enqueue the clone for
	 *	transmission to the gang's common entry node.		*/

	for (elt2 = lyst_first(gangs); elt2; elt2 = lyst_next(elt2))
	{
#if IMCDEBUG
writeMemo("Processing multicast gang.");
#endif
		gang = (ImcGang *) lyst_data(elt2);
		if (bpClone(bundle, &newBundle, &newBundleObj, 0, 0) < 0)
		{
			putErrmsg("Failed on clone.", NULL);
			lyst_destroy(gangs);
			return -1;
		}

		/*	Erase clone's original destinations list.	*/

		while ((elt = sdr_list_first(sdr, newBundle.destinations)))
		{
			sdr_list_delete(sdr, elt, NULL, NULL);
		}

		/*	Insert all new destinations.			*/

		for (elt3 = lyst_first(gang->members); elt3;
				elt3 = lyst_next(elt3))
		{
			nodeNbr = (uaddr) lyst_data(elt3);
#if IMCDEBUG
writeMemoNote("Loading destination", itoa(nodeNbr));
#endif
			if (imcLoadDestination(&newBundle, nodeNbr) < 0)
			{
				putErrmsg("Failed loading destination.", NULL);
				lyst_destroy(gangs);
				return -1;
			}
		}

		/*	Finally, enqueue the new bundle for xmit.	*/
#if IMCDEBUG
writeMemoNote("Gang bundle sent to", itoa(gang->entryNode));
writeMemoNote("...has this many members", itoa(lyst_length(gang->members)));
#endif
		if (enqueueBundle(&newBundle, newBundleObj, gang->entryNode)
				< 0)
		{
			putErrmsg("Failed on enqueue.", NULL);
			lyst_destroy(gangs);
			return -1;
		}
	}

	/*	Destroy gangs list and originally received multicast
	 *	bundle.							*/

	lyst_destroy(gangs);
	return bpDestroyBundle(bundleAddr, 2);
}

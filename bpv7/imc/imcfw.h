/*
 	imcfw.h:	definitions supporting elements of ION that
			participate in Interplanetary Multicast.

	Author: Scott Burleigh, JPL

	Modification History:
	Date      Who   What

	Copyright (c) 2020, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
 									*/
#ifndef _IMCFW_H_
#define _IMCFW_H_

#include "bpP.h"

#ifndef IMCDEBUG
#define	IMCDEBUG	0
#endif

/*	Administrative record types	*/
#define	BP_MULTICAST_BRIEFING	(5)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	Object		groups;		/*	SDR list of ImcGroups	*/
} ImcDB;

typedef struct
{
	uvast		groupNbr;
	int		isMember;	/*	Boolean			*/
} ImcPetition;

extern int		imcInit();
extern Object		getImcDbObject();
extern ImcDB		*getImcConstants();

extern int		imcHandleBriefing(BpDelivery *dlv,
				unsigned char *cursor,
				unsigned int unparsedBytes);

/*	A "dispatch" is a bundle that is privately multicast to
 *	all (and only) members of the indicated region.
 *
 *	"Petitions" are dispatches that convey information about
 *	multicast group membership.					*/

extern int		imcSendDispatch(char *destEid, uint32_t toRegion,
				unsigned char *buffer, int length);

extern int		imcSendPetition(ImcPetition *petition,
				uint32_t toRegion);

extern int		imcGroupMember(uvast groupNbr);

/*	For inter-regional multicast, the original (intra-regional)
 *	multicast of the bundle must be replicated in other regions.	*/

extern int		imcReplicate(Bundle *bundle, Object bundleObj);

#ifdef __cplusplus
}
#endif

#endif  /* _IMCFW_H_ */

/*
 	imcfwP.h:	definitions supporting the implementation
			of Interplanetary Multicast.

	Author: Scott Burleigh, JPL

	Modification History:
	Date      Who   What

	Copyright (c) 2020, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
 									*/
#ifndef _IMCFWP_H_
#define _IMCFWP_H_

#include "bpP.h"
#include "imcfw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	uvast		groupNbr;
	long		secUntilDelete;	/*	Default is -1.		*/
	int		isMember;	/*	Boolean: local node	*/
	Object		members;	/*	SDR list of node nbrs	*/
	int		count[2];	/*	Passageway's counts	*/
} ImcGroup;

extern void		imcFindGroup(uvast groupNbr, Object *addr,
				Object *eltp);

extern int		imcLoadDestination(Bundle *bundle, uvast newNodeNbr);

extern int		imcForwardBundle(Bundle *bundle, Object bundleAddr);

#ifdef __cplusplus
}
#endif

#endif  /* _IMCFWP_H_ */

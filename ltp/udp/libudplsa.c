/*

	libudplsa.c:	Common functions for the LTP UDP-based link
			service.

	Author: Scott Burleigh, JPL

	Copyright (c) 2020, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
	
									*/
#include "udplsa.h"

void	*udplsa_handle_datagrams(void *parm)
{
	/*	Main loop for UDP datagram reception and handling.	*/

	ReceiverThreadParms	*rtp = (ReceiverThreadParms *) parm;
	char			*procName = "udplsi";
	char			*buffer;
	int			segmentLength;
#ifdef UDP_MULTISEND
	char			*buffers;
	struct iovec		*iovecs;
	struct mmsghdr		*msgs;
	unsigned int		batchLength;
	int			i;
#ifdef LTPGRO
	int			messageLength;
	struct cmsghdr          *cmsgs;
	struct cmsghdr          *cmsg;
	char			*segment;
#ifdef LTPPARCEL
	int			nSegs;
	uint16_t		*csum;
#endif
#endif

	snooze(1);	/*	Let main thread become interruptable.	*/

	/*	Initialize recvmmsg buffers.				*/

	buffers = MTAKE((UDPLSA_BUFSZ + 1)* MULTIRECV_BUFFER_COUNT);
	if (buffers == NULL)
	{
		putErrmsg("No space for segment buffer array.", NULL);
		ionKillMainThread(procName);
		return NULL;
	}

	iovecs = MTAKE(sizeof(struct iovec) * MULTIRECV_BUFFER_COUNT);
	if (iovecs == NULL)
	{
		MRELEASE(buffers);
		putErrmsg("No space for iovec array.", NULL);
		ionKillMainThread(procName);
		return NULL;
	}

	msgs = MTAKE(sizeof(struct mmsghdr) * MULTIRECV_BUFFER_COUNT);
	if (msgs == NULL)
	{
		MRELEASE(iovecs);
		MRELEASE(buffers);
		putErrmsg("No space for mmsghdr array.", NULL);
		ionKillMainThread(procName);
		return NULL;
	}

	memset(msgs, 0, sizeof(struct mmsghdr) * MULTIRECV_BUFFER_COUNT);
#ifdef LTPGRO

        /* Allocate cmsg block but DO NOT set up any fields here at startup
	 * time, since they may be blitzed at runtime after sm_TaskYield()
	 * gets called. This caused the GRO API to fail because msg_controllen
	 * was being zeroed. Now reset fields at each recvmmmg() iteration
	 * at runtime. */

	cmsgs = MTAKE(CMSG_LEN(sizeof (int)) * MULTIRECV_BUFFER_COUNT);
	if (cmsgs == NULL)
	{
		MRELEASE(iovecs);
		MRELEASE(buffers);
		MRELEASE(msgs);
		putErrmsg("No space for cmsghdr array.", NULL);
		ionKillMainThread(procName);
		return NULL;
	}
	memset(cmsgs, 0, CMSG_LEN(sizeof (int)) * MULTIRECV_BUFFER_COUNT);
	cmsg = (struct cmsghdr *)cmsgs;
#endif

	for (i = 0; i < MULTIRECV_BUFFER_COUNT; i++)
	{
		iovecs[i].iov_base = buffers + (i * (UDPLSA_BUFSZ + 1));
		iovecs[i].iov_len = UDPLSA_BUFSZ;
		msgs[i].msg_hdr.msg_iov = iovecs + i;
		msgs[i].msg_hdr.msg_iovlen = 1;
	}

	/*	Can now start receiving bundles.  On failure, take
	 *	down the daemon.					*/

	while (rtp->running)
	{	
#ifdef LTPGRO

		/* Re-init everything before each recvmmsg(). It would be
		 * better if this could be done once at startup time, but
		 * see above for reason. */

		cmsg = (struct cmsghdr *)cmsgs;
		for (i = 0; i < MULTIRECV_BUFFER_COUNT; i++)
		{
			iovecs[i].iov_base = buffers + (i * (UDPLSA_BUFSZ + 1));
			iovecs[i].iov_len = UDPLSA_BUFSZ;
			msgs[i].msg_hdr.msg_iov = iovecs + i;
			msgs[i].msg_hdr.msg_iovlen = 1;
			cmsg->cmsg_len = CMSG_LEN(sizeof(int));
			*((int *)CMSG_DATA(cmsg)) = 0;
			cmsg->cmsg_level = SOL_UDP;
			cmsg->cmsg_type = UDP_GRO;
			msgs[i].msg_hdr.msg_control = (void *)cmsg;
			msgs[i].msg_hdr.msg_controllen = cmsg->cmsg_len;
			cmsg =
			    (struct cmsghdr *)((void *)cmsg + cmsg->cmsg_len);
		}
#endif
		batchLength = recvmmsg(rtp->linkSocket, msgs,
				MULTIRECV_BUFFER_COUNT, MSG_WAITFORONE, NULL);
		switch (batchLength)
		{
		case -1:
			putSysErrmsg("Can't acquire segments", NULL);
			ionKillMainThread(procName);
			rtp->running = 0;

			/*	Intentional fall-through to next case.	*/

		case 0:	/*	Interrupted system call.		*/
			continue;
		}

		buffer = buffers;
#ifdef LTPGRO
		segment = buffer;
		for (i = 0; i < batchLength; i++)
		{

			/* The API is unpublished, but GRO returns a zero
			 * segmentLength when only a single segment is
			 * returned and non-zero for multiple. */

			messageLength = msgs[i].msg_len;
			cmsg = (struct cmsghdr *)msgs[i].msg_hdr.msg_control;
#ifdef LTPPARCEL
			segmentLength = *((int *)CMSG_DATA(cmsg));
			if (segmentLength & 0xffff0000)
			{
				csum = (uint16_t *)buffer;
				nSegs = (segmentLength & 0xffff0000) >> 16;
				segmentLength &= 0xffff; 
				messageLength -= (nSegs * 2);
				segment = buffer + (nSegs * 2);
			} else {
				csum = 0;
				if (segmentLength == 0)
					segmentLength = messageLength;
			}
#else /* LTPPARCEL */
			if ((segmentLength = *((int *)CMSG_DATA(cmsg))) == 0)
				segmentLength = messageLength;
#endif /* LTPPARCEL */
#ifdef LTPSTAT
			if (segmentLength) {
#ifdef LTPSTAT_NOTDEF
				char txt[500];

				isprintf(txt, sizeof(txt),
				    "[i] udplsi got Parcel or GRO (%d / %d)",
				    messageLength, segmentLength);
				writeMemo(txt);
#endif
				rtp->recvGRO++;
			}

			if (messageLength >= 1200) {
				rtp->recvBigMsgs++;
				rtp->recvBigBytes += messageLength;
			}
#endif /* LTPSTAT */

			/* process non-final segments */
			while (messageLength > segmentLength)
			{
#ifdef LTPPARCEL
				/* csum non-null only for parcels */
				if (csum && *csum)
				{
					uint16_t chk, chk2;
#ifdef LTPPARCEL_CSUM_RX
					chk = in_csum(segment, segmentLength);
					chk = chk ? : 0xffff;
#else
					chk = 0x01;
#endif
					chk2 = *csum++;
					if (chk != chk2)
					{
#ifdef LTPSTAT
					char txt[500];
					isprintf(txt, sizeof(txt),
					"[i] udplsi: bad checksum (1) (%d : %x %x)",
					segmentLength, chk, chk2);
					writeMemo(txt);
#endif
					goto gro_dropseg;
					}
				}
#endif /* LTPPARCEL */
				if (ltpHandleInboundSegment(segment,
				    segmentLength) < 0)
				{
					putErrmsg("Can't handle inbound seg.",
						NULL);
					ionKillMainThread(procName);
					rtp->running = 0;
					goto taskyield;
				}
#ifdef LTPPARCEL
gro_dropseg:
#endif
				segment += segmentLength;
				messageLength -= segmentLength;
#ifdef LTPSTAT
				rtp->recvSegs++;
#endif
			}

			/* exit on terminating segment */
			if (messageLength == 1)
			{
				/*	Normal stop.			*/
				rtp->running = 0;
#ifdef LTPSTAT
				rtp->recvSegs++;
#endif
				goto taskyield;
			}

#ifdef LTPPARCEL
			if (csum && *csum)
			{
				uint16_t chk;
#ifdef LTPPARCEL_CSUM_RX
				chk = in_csum(segment, segmentLength);
				chk = chk ? : 0xffff;
#else
				chk = 0x1;
#endif
				if (chk != *csum)
				{
#ifdef LTPSTAT
				char txt[500];
				isprintf(txt, sizeof(txt),
				"[i] udplsi: bad checksum (2) (%d : %x %x)",
					messageLength, chk, *csum);
				writeMemo(txt);
#endif
				goto gro_dropseg2;
				}
			}
#endif /* LTPPARCEL */

			/* process message remainder */
			if (ltpHandleInboundSegment(segment, messageLength) < 0)
			{
				putErrmsg("Can't handle inbound seg.", NULL);
				ionKillMainThread(procName);
				rtp->running = 0;
				goto taskyield;
			}
#ifdef LTPSTAT
			rtp->recvSegs++;
#endif
#ifdef LTPPARCEL
gro_dropseg2:
#endif
			buffer += (UDPLSA_BUFSZ + 1);
			segment = buffer;
		}
taskyield:
#else /* LTPGRO */
		for (i = 0; i < batchLength; i++)
		{

			segmentLength = msgs[i].msg_len;
			if (segmentLength == 1)
			{
				/*	Normal stop.			*/

				rtp->running = 0;
				break;
			}

			if (ltpHandleInboundSegment(buffer, segmentLength) < 0)
			{
				putErrmsg("Can't handle inbound segment.",
						NULL);
				ionKillMainThread(procName);
				rtp->running = 0;
				break;
			}
#ifdef LTPSTAT
			rtp->recvSegs++;
#endif

			buffer += (UDPLSA_BUFSZ + 1);
		}
#endif /* LTPGRO */

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	MRELEASE(msgs);
	MRELEASE(iovecs);
	MRELEASE(buffers);
#ifdef LTPGRO
	MRELEASE(cmsgs);
#endif
#else /* UDP_MULTISEND */
	struct sockaddr_in	fromAddr;
	socklen_t		fromSize;

	snooze(1);	/*	Let main thread become interruptable.	*/

	/*	Initialize buffer.					*/

	buffer = MTAKE(UDPLSA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("udplsi can't get UDP buffer.", NULL);
		ionKillMainThread(procName);
		return NULL;
	}

	/*	Can now start receiving bundles.  On failure, take
	 *	down the link service input thread.			*/

	while (rtp->running)
	{	
		fromSize = sizeof fromAddr;
		segmentLength = irecvfrom(rtp->linkSocket, buffer, UDPLSA_BUFSZ,
				0, (struct sockaddr *) &fromAddr, &fromSize);
		switch (segmentLength)
		{
		case 0:	/*	Interrupted system call.		*/
			continue;

		case -1:
			putSysErrmsg("Can't acquire segment", NULL);
			ionKillMainThread(procName);

			/*	Intentional fall-through to next case.	*/

		case 1:				/*	Normal stop.	*/
			rtp->running = 0;
			continue;
		}

		if (ltpHandleInboundSegment(buffer, segmentLength) < 0)
		{
			putErrmsg("Can't handle inbound segment.", NULL);
			ionKillMainThread(procName);
			rtp->running = 0;
			continue;
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	MRELEASE(buffer);
#endif /* UDP_MULTISEND */
	writeErrmsgMemos();
	writeMemo("[i] udplsa receiver thread has ended.");
#ifdef LTPSTAT
	{
		char	txt[500];

		isprintf(txt, sizeof(txt),
			"[i] udplsi received %d segments", rtp->recvSegs);
		writeMemo(txt);
		isprintf(txt, sizeof(txt),
			"[i] udplsi received %d GRO buffers", rtp->recvGRO);
		writeMemo(txt);
		isprintf(txt, sizeof(txt),
			"[i] udplsi received %d large messages",
			rtp->recvBigMsgs);
		writeMemo(txt);
		isprintf(txt, sizeof(txt),
			"[i] udplsi received %d large message bytes",
			rtp->recvBigBytes);
		writeMemo(txt);
	}
#endif

	/*	Free resources.						*/

	return NULL;
}

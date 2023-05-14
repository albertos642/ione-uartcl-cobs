/*
	libipplsa.c:	Common functions for the LTP link service
			based on GRO/GSO and IP parcels.

	Author: Fred Templin, Boeing Corp.

	Adapted from libudplsa.c.

	Copyright (c) 2023, Boeing Corp.  ALL RIGHTS RESERVED.
									*/
#include "ipplsa.h"
#include "llcv.h"

#ifndef	IPPLSA_HANDLERS
#define	IPPLSA_HANDLERS		4
#endif

typedef struct
{
	pthread_t		handlerThread;
	Lyst			segments;
	struct llcv_str		segmentsLlcv;
	Llcv			llcv;
} IpplsaHandler;

typedef struct
{
	int			segmentLength;
	char			*segment;
} IpplsaCapsule;

static char			*procName = "ipplsi";

void	*ipplsa_handle_datagrams(void *parm)
{
	/*	Main loop for UDP datagram reception and handling.	*/

	ReceiverThreadParms	*rtp = (ReceiverThreadParms *) parm;
	char			*buffer;
	int			segmentLength;
	char			*buffers;
	struct iovec		*iovecs;
	struct mmsghdr		*msgs;
	unsigned int		batchLength;
	int			i;
	int			messageLength;
	struct cmsghdr          *cmsgs;
	struct cmsghdr          *cmsg;
	char			*segment;
#ifdef LTPPARCEL
	int			nSegs;
	uint16_t		*csum;
#endif /* LTPPARCEL */

	snooze(1);	/*	Let main thread become interruptable.	*/

	/*	Initialize recvmmsg buffers.				*/

	buffers = MTAKE((IPPLSA_BUFSZ + 1) * MULTIRECV_BUFFER_COUNT);
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
	for (i = 0; i < MULTIRECV_BUFFER_COUNT; i++)
	{
		iovecs[i].iov_base = buffers + (i * (IPPLSA_BUFSZ + 1));
		iovecs[i].iov_len = IPPLSA_BUFSZ;
		msgs[i].msg_hdr.msg_iov = iovecs + i;
		msgs[i].msg_hdr.msg_iovlen = 1;
	}

	/*	Can now start receiving bundles.  On failure, take
	 *	down the daemon.					*/

	while (rtp->running)
	{
		/* Re-init everything before each recvmmsg(). It would be
		 * better if this could be done once at startup time, but
		 * see above for reason. */

		cmsg = (struct cmsghdr *)cmsgs;
		for (i = 0; i < MULTIRECV_BUFFER_COUNT; i++)
		{
			iovecs[i].iov_base = buffers + (i * (IPPLSA_BUFSZ + 1));
			iovecs[i].iov_len = IPPLSA_BUFSZ;
			msgs[i].msg_hdr.msg_iov = iovecs + i;
			msgs[i].msg_hdr.msg_iovlen = 1;
			cmsg->cmsg_len = CMSG_LEN(sizeof(int));
			*((int *)CMSG_DATA(cmsg)) = 0;
			cmsg->cmsg_level = SOL_UDP;
			cmsg->cmsg_type = UDP_GRO;
			msgs[i].msg_hdr.msg_control = (void *)cmsg;
			msgs[i].msg_hdr.msg_controllen = cmsg->cmsg_len;
			cmsg = (struct cmsghdr *)((void *) cmsg
					+ cmsg->cmsg_len);
		}

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
			}
			else
			{
				csum = 0;
				if (segmentLength == 0)
				{
					segmentLength = messageLength;
				}
			}
#else /* LTPPARCEL */
			if ((segmentLength = *((int *) CMSG_DATA(cmsg))) == 0)
			{
				segmentLength = messageLength;
			}
#endif /* LTPPARCEL */
#ifdef LTPSTAT
			if (segmentLength)
			{
#ifdef LTPSTAT_NOTDEF
				char txt[500];

				isprintf(txt, sizeof(txt),
					"[i] ipplsi got Parcel/GRO (%d / %d)",
					messageLength, segmentLength);
				writeMemo(txt);
#endif /* LTPSTAT_NOTDEF */
				rtp->recvGRO++;
			}

			if (messageLength >= 1200)
			{
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
#else /* LTPPARCEL_CSUM_RX */
					chk = 0x01;
#endif /* LTPPARCEL_CSUM_RX */
					chk2 = *csum++;
					if (chk != chk2)
					{
#ifdef LTPSTAT
						char	txt[500];

						isprintf(txt, sizeof(txt),
								"[i] ipplsi: \
bad checksum (1) (%d : %x %x)", segmentLength, chk, chk2);
						writeMemo(txt);
#endif /* LTPSTAT */
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
#endif /* LTPPARCEL */
				segment += segmentLength;
				messageLength -= segmentLength;
#ifdef LTPSTAT
				rtp->recvSegs++;
#endif /* LTPSTAT */
			}

			/* exit on terminating segment */
			if (messageLength == 1)
			{
				/*	Normal stop.			*/
				rtp->running = 0;
#ifdef LTPSTAT
				rtp->recvSegs++;
#endif /* LTPSTAT */
				goto taskyield;
			}

#ifdef LTPPARCEL
			if (csum && *csum)
			{
				uint16_t chk;
#ifdef LTPPARCEL_CSUM_RX
				chk = in_csum(segment, segmentLength);
				chk = chk ? : 0xffff;
#else /* LTPPARCEL_CSUM_RX */
				chk = 0x1;
#endif /* LTPPARCEL_CSUM_RX */
				if (chk != *csum)
				{
#ifdef LTPSTAT
					char	txt[500];

					isprintf(txt, sizeof(txt), "[i] \
ipplsi: bad checksum (2) (%d : %x %x)", messageLength, chk, *csum);
					writeMemo(txt);
#endif /* LTPSTAT */
					goto gro_dropseg2;
				}
			}
#endif /* LTPPARCEL */
			/* process message remainder */

			if (ltpHandleInboundSegment(segment, segmentLength) < 0)
			{
				putErrmsg("Can't handle inbound seg.", NULL);
				ionKillMainThread(procName);
				rtp->running = 0;
				goto taskyield;
			}
#ifdef LTPSTAT
			rtp->recvSegs++;
#endif /* LTPSTAT */
#ifdef LTPPARCEL
		gro_dropseg2:
#endif /* LTPPARCEL */
			buffer += (IPPLSA_BUFSZ + 1);
			segment = buffer;
		}

	taskyield:

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	/*	Free resources, shut down all segment handlers.		*/

	MRELEASE(msgs);
	MRELEASE(iovecs);
	MRELEASE(buffers);
	MRELEASE(cmsgs);
	writeErrmsgMemos();
	writeMemo("[i] ipplsi receiver thread has ended.");
#ifdef LTPSTAT
	{
		char	txt[500];

		isprintf(txt, sizeof(txt),
			"[i] ipplsi received %d segments", rtp->recvSegs);
		writeMemo(txt);
		isprintf(txt, sizeof(txt),
			"[i] ipplsi received %d GRO buffers", rtp->recvGRO);
		writeMemo(txt);
		isprintf(txt, sizeof(txt),
			"[i] ipplsi received %d large messages",
			rtp->recvBigMsgs);
		writeMemo(txt);
		isprintf(txt, sizeof(txt),
			"[i] ipplsi received %d large message bytes",
			rtp->recvBigBytes);
		writeMemo(txt);
	}
#endif /* LTPSTAT */

	return NULL;
}

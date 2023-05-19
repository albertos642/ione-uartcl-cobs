/*
 *	tcpbso.c:	BSSP TCP-based link service output daemon.
 *			Dedicated to TCP blocks transmission to a 
 *			single remote BSSP engine.
 *
 *	Authors: Sotirios-Angelos Lenas, SPICE
 *		 Scott Burleigh, JPL
 *
 *	Copyright (c) 2013, California Institute of Technology.
 *	Copyright (c) 2013, Space Internetworking Center,
 *	Democritus University of Thrace.
 *	
 *	All rights reserved. U.S. Government and E.U. Sponsorship acknowledged.
 *
 */
#include "tcpbsa.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <netdb.h>

static sm_SemId	tcpbsoSemaphore(sm_SemId *semid)
{
	uaddr		temp;
	void		*value;
	sm_SemId	semaphore;

	if (semid)			/*	Add task variable.	*/
	{
		temp = *semid;
		value = (void *) temp;
		value = sm_TaskVar(&value);
	}
	else				/*	Retrieve task variable.	*/
	{
		value = sm_TaskVar(NULL);
	}

	temp = (uaddr) value;
	semaphore = temp;
	return semaphore;
}

static void	shutDownBso(int signum)	/*	Commands CLO shutdown.	*/
{
	isignal(SIGTERM, shutDownBso);
	sm_SemEnd(tcpbsoSemaphore(NULL));
}

/*	*	*	Keepalive thread functions	*	*	*/

typedef struct
{
	int		*bsoRunning;
	pthread_mutex_t	*mutex;
	struct sockaddr	*socketName;
	int		*flowSocket;
} KeepaliveThreadParms;

static void	*sendKeepalives(void *parm)
{
	KeepaliveThreadParms	*parms = (KeepaliveThreadParms *) parm;
	int			count = KEEPALIVE_PERIOD;
	int			bytesSent;

	iblock(SIGTERM);
	while (*(parms->bsoRunning))
	{
		snooze(1);
		count++;
		if (count < KEEPALIVE_PERIOD)
		{
			continue;
		}

		/*	Time to send a keepalive.  Note that the
		 *	interval between keepalive attempts will be
		 *	KEEPALIVE_PERIOD plus (if the remote inflow
		 *	is not reachable) the length of time taken
		 *	by TCP to determine that the connection
		 *	attempt will not succeed (e.g., 3 seconds).	*/

		count = 0;
		pthread_mutex_lock(parms->mutex);
		bytesSent = sendBlockByTCP(parms->socketName,
				parms->flowSocket, 0, NULL);
		pthread_mutex_unlock(parms->mutex);
		if (bytesSent < 0)
		{
			shutDownBso(SIGTERM);
			break;
		}
	}

	return NULL;
}

/*	*	*	Main thread functions	*	*	*	*/

#if defined (ION_LWT)
int	tcpbso6(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char	*flowName = (char *) a1;
	uvast	remoteEngineId = a2 != 0 ? strtouvast((char *) a2) : 0;
#else
int	main(int argc, char *argv[])
{
	char	*flowName = (argc > 1 ? argv[1] : NULL);
	uvast	remoteEngineId = argc > 2 ? strtouvast(argv[2]) : 0;
#endif
	Sdr			sdr;
	BsspVspan		*vspan;
	PsmAddress		vspanElt;
	struct sockaddr         socketName;
	struct sockaddr_in6	inetName;
	int			running = 1;
	pthread_mutex_t		mutex;
	KeepaliveThreadParms	parms;
	pthread_t		keepaliveThread;
	int			blockLength;
	char			*block;
	int			flowSocket = -1;
	int			bytesSent;

	if (remoteEngineId == 0 || flowName == NULL)
	{
		PUTS("Usage: tcpbso6 <remote host name>[:<port number>] <remote \
engine number>");
		return 0;
	}

	if (bsspInit(0) < 0)
	{
		putErrmsg("tcpbso6 can't initialize BSSP.", NULL);
		return 1;
	}

	sdr = getIonsdr();
	CHKZERO(sdr_begin_xn(sdr));	/*	Just to lock memory.	*/
	findBsspSpan(remoteEngineId, &vspan, &vspanElt);
	if (vspanElt == 0)
	{
		sdr_exit_xn(sdr);
		putErrmsg("No such engine in database.", itoa(remoteEngineId));
		return 1;
	}

	if (vspan->bsoRLPid != ERROR && vspan->bsoRLPid != sm_TaskIdSelf())
	{
		sdr_exit_xn(sdr);
		putErrmsg("TCP-BSO task is already started for this span.",
				itoa(vspan->bsoRLPid));
		return 1;
	}

	sdr_exit_xn(sdr);

	/*	All command-line arguments are now validated.		*/

	if (parseSocketSpecSix(flowName, &inetName) != 0)
	{
	    putErrmsg("TCPBSO6 can't get IP/port for host.", flowName);
		return -1;
	}

	if (inetName.sin6_port == 0)
	{
		inetName.sin6_port = htons(bsspTcpDefaultPortNbr);
	}


	/*	Set up signal handling.  SIGTERM is shutdown signal.	*/

	oK(tcpbsoSemaphore(&(vspan->rlSemaphore)));
	isignal(SIGTERM, shutDownBso);
#ifndef mingw
	isignal(SIGPIPE, itcp_handleConnectionLoss);
#endif

	/*	Start the keepalive thread to manage the connection.	*/

	parms.bsoRunning = &running;
	pthread_mutex_init(&mutex, NULL);
	parms.mutex = &mutex;
	parms.socketName = &socketName;
	parms.flowSocket = &flowSocket;
	if (pthread_begin(&keepaliveThread, NULL, sendKeepalives, &parms, "tcpbso_keepalive"))
	{
		putSysErrmsg("tcpbso can't create keepalive thread", NULL);
		pthread_mutex_destroy(&mutex);
		return 1;
	}

	/*	Can now begin transmitting to remote flow.		*/

	{
		char	txt[500];

		isprintf(txt, sizeof(txt),
			"[i] tcpbso6 is running, spec=[%s:%d].", 
			flowName,
			ntohs(inetName.sin6_port));
		writeMemo(txt);
	}

	while (!(sm_SemEnded(tcpbsoSemaphore(NULL))))
	{
		
		blockLength = bsspDequeueRLOutboundBlock(vspan, &block);
		if (blockLength < 0)
		{
			sm_SemEnd(tcpbsoSemaphore(NULL));/*	Stop BSO.*/
			continue;
		}

		if (blockLength == 0)		/*	Interrupted.	*/
		{
			continue;
		}

		if (blockLength > TCPBSA_BUFSZ)
		{
			putErrmsg("Block is too big for TCP BSO.",
					itoa(blockLength));
			sm_SemEnd(tcpbsoSemaphore(NULL));/*	Stop BSO.*/
		}
		else
		{
			pthread_mutex_lock(&mutex);
			bytesSent = sendBlockByTCP(&socketName, &flowSocket,
					blockLength, block);
			pthread_mutex_unlock(&mutex);
			if (bytesSent < blockLength)	/*	Stop BSO.*/
			{
				sm_SemEnd(tcpbsoSemaphore(NULL));
				continue;
			}
		}
		
		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	running = 0;		/*	Terminate keepalive thread.	*/
	pthread_join(keepaliveThread, NULL);
	if (flowSocket != -1)
	{
		closesocket(flowSocket);
	}

	pthread_mutex_destroy(&mutex);
	writeErrmsgMemos();
	writeMemo("[i] tcpbso has ended.");
	return 0;
}

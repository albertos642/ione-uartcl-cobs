#include "uartcobscla.h"
#include <signal.h>

static void interruptThread(int signum) {
	isignal(SIGTERM, interruptThread);
	ionKillMainThread("uartcobscli");
}

typedef struct {
	VInduct		*vduct;
	int		*ductSocket;
	int		running;
	struct uartcobsdescriptor *uartPort;
} ReceiverThreadParms;

static void *handleDatagrams(void *parm) {
	ReceiverThreadParms *rtp = (ReceiverThreadParms *) parm;
	AcqWorkArea *work;
	unsigned char *buffer;

	snooze(1);
	work = bpGetAcqArea(rtp->vduct);
	if (work == NULL) {
		ionKillMainThread("uartcobscli");
		return NULL;
	}

	buffer = MTAKE(UARTCOBS_MAX_PAYLOAD + 10);
	if (buffer == NULL) {
		ionKillMainThread("uartcobscli");
		return NULL;
	}

	while (rtp->running) {	
		int bundleLength = receiveFrameByUartCobs(rtp->ductSocket, rtp->uartPort, buffer);
		
		if (bundleLength < 0) {
			snooze(1);
			continue;
		}

		if (bundleLength > 0) {
			// Valid frame received, pass raw payload to ION SDR
			if (bpBeginAcq(work, 0, NULL) < 0 || 
			    bpContinueAcq(work, (char*)buffer, bundleLength, 0, 0) < 0 || 
			    bpEndAcq(work) < 0) {
				putErrmsg("uartcobscli: Can't acquire bundle.", NULL);
				ionKillMainThread("uartcobscli");
				rtp->running = 0;
				continue;
			}
		}
		sm_TaskYield();
	}

	writeMemo("[i] UART-COBS CL Induct receiver thread has ended.");
	bpReleaseAcqArea(work);
	MRELEASE(buffer);
	return NULL;
}

#if defined (ION_LWT)
int udpcli(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char *ductName = (char *) a1;
#else
int main(int argc, char *argv[])
{
	char *ductName = (argc > 1 ? argv[1] : NULL);
#endif
	VInduct *vduct;
	PsmAddress vductElt;
	struct uartcobsdescriptor hostNbr;
	ReceiverThreadParms rtp;
	pthread_t receiverThread;
	int ductSocket = -1;

	if (ductName == NULL) return 0;
	if (bpAttach() < 0) return -1;

	findInduct("uartcobs", ductName, &vduct, &vductElt);
	if (vductElt == 0) return -1;

	if (parseUartCobsSpec(ductName, &hostNbr) != 0) return -1;

	rtp.vduct = vduct;
	rtp.ductSocket = &ductSocket;
	rtp.uartPort = &hostNbr;

	ionNoteMainThread("uartcobscli");
	isignal(SIGTERM, interruptThread);

	rtp.running = 1;
	if (pthread_begin(&receiverThread, NULL, handleDatagrams, &rtp)) return -1;

    writeMemo("[i] uartcobscli is running");

	ionPauseMainThread(-1);

	rtp.running = 0;
	writeMemo("[i] UART-COBS CL Induct has ended.");
	ionDetach();
	return 0;
}
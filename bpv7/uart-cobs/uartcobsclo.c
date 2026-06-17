#include "uartcobscla.h"
#include <signal.h>

static sm_SemId uartcloSemaphore(sm_SemId *semid) {
	static sm_SemId semaphore = -1;
	if (semid) semaphore = *semid;
	return semaphore;
}

static void shutDownClo(int signum) {
	sm_SemEnd(uartcloSemaphore(NULL));
}

#if defined (ION_LWT)
int uartclo(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char *fileDescriptor = (char *) a1;
#else
int main(int argc, char *argv[])
{
	char *fileDescriptor = (argc > 1 ? argv[1] : NULL);
#endif
	struct uartcobsdescriptor hostNbr;
	unsigned char *buffer;
	VOutduct *vduct;
	PsmAddress vductElt;
	Sdr sdr;
	Object bundleZco;
	BpAncillaryData ancillaryData;
	unsigned int bundleLength;
	int ductSocket = -1;

	if (fileDescriptor == NULL) return 0;
	if (parseUartCobsSpec(fileDescriptor, &hostNbr) != 0) return -1;
	if (bpAttach() < 0) return -1;

	buffer = MTAKE(UARTCOBS_MAX_PAYLOAD + 10);
	if (buffer == NULL) return -1;

	findOutduct("uartcobs", fileDescriptor, &vduct, &vductElt);
	if (vductElt == 0) {
		MRELEASE(buffer);
		return -1;
	}

	sdr = getIonsdr();
	oK(uartcloSemaphore(&(vduct->semaphore)));
	isignal(SIGTERM, shutDownClo);

	writeMemo("[i] uartcobsclo is running");

	while (!(sm_SemEnded(vduct->semaphore))) {
		if (bpDequeue(vduct, &bundleZco, &ancillaryData, 0) < 0) break;
		if (bundleZco == 0) {
			sm_SemEnd(uartcloSemaphore(NULL));
			continue;
		}
		if (bundleZco == 1) continue;

		CHKZERO(sdr_begin_xn(sdr));
		bundleLength = zco_length(sdr, bundleZco);
		sdr_exit_xn(sdr);

		int bytesSent = sendBundleByUartCobs(&hostNbr, &ductSocket, bundleLength, bundleZco, buffer);
		
		if (bytesSent < bundleLength) {
			sm_SemEnd(uartcloSemaphore(NULL));
			continue;
		}
		sm_TaskYield();
	}

	if (ductSocket != -1) close(ductSocket);
	writeMemo("[i] UART-COBS CL Outduct has ended.");
	MRELEASE(buffer);
	ionDetach();
	return 0;
}
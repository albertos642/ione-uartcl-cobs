/*
	bppower.c:	control 12 gpios by parsing bundle.
									*/
/*									*/
/*	Copyright (c) 2004, California Institute of Technology.		*/
/*	All rights reserved.						*/
/*	Author: Scott Burleigh, Jet Propulsion Laboratory		*/
/*	Modified for conditional parsing, 2024.  Scott Johnson		*/

#include <bp.h>

typedef struct
{
	BpSAP	sap;
	int	running;
} BptestState;

static BptestState	*_bptestState(BptestState *newState)
{
	void		*value;
	BptestState	*state;

	if (newState)			/*	Add task variable.	*/
	{
		value = (void *) (newState);
		state = (BptestState *) sm_TaskVar(&value);
	}
	else				/*	Retrieve task variable.	*/
	{
		state = (BptestState *) sm_TaskVar(NULL);
	}

	return state;
}

static void	handleQuit(int signum)
{
	BptestState	*state;

	isignal(SIGINT, handleQuit);
	PUTS("BP reception interrupted.");
	fflush(NULL);
	state = _bptestState(NULL);
	bp_interrupt(state->sap);
	state->running = 0;
}

#if defined (ION_LWT)
int	bppower(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char		*ownEid = (char *) a1;
#else
int	main(int argc, char **argv)
{
	char		*ownEid = (argc > 1 ? argv[1] : NULL);
#endif
	static char	*deliveryTypes[] =	{
				"Payload delivered.",
				"Reception timed out.",
				"Reception interrupted.",
				"Endpoint stopped."
						};
	BptestState	state = { NULL, 1 };
	Sdr		sdr;
	BpDelivery	dlv;
	int		contentLength;
	ZcoReader	reader;
	int		len;
	char		content[80];
	char		line[84];
	FILE		*fptr;
#ifndef mingw
	setlinebuf(stdout);
#endif
	if (ownEid == NULL)
	{
		PUTS("Usage: bppower <own endpoint ID>");
		fflush(NULL);
		return 0;
	}

	if (bp_attach() < 0)
	{
		putErrmsg("Can't attach to BP.", NULL);
		return 0;
	}

	if (bp_open(ownEid, &state.sap) < 0)
	{
		putErrmsg("Can't open own endpoint.", ownEid);
		return 0;
	}

	oK(_bptestState(&state));
	sdr = bp_get_sdr();
	isignal(SIGINT, handleQuit);
	while (state.running)
	{
		if (bp_receive(state.sap, &dlv, BP_BLOCKING) < 0)
		{
			putErrmsg("bppower bundle reception failed.", NULL);
			state.running = 0;
			continue;
		}

		PUTMEMO("ION event", deliveryTypes[dlv.result - 1]);
		fflush(NULL);
		if (dlv.result == BpReceptionInterrupted)
		{
			continue;
		}

		if (dlv.result == BpEndpointStopped)
		{
			state.running = 0;
			continue;
		}

		if (dlv.result == BpPayloadPresent)
		{
			CHKZERO(sdr_begin_xn(sdr));
			contentLength = zco_source_data_length(sdr, dlv.adu);
			sdr_exit_xn(sdr);
			isprintf(line, sizeof line, "\tpayload length is %d.",
					contentLength);
			PUTS(line);
			fflush(NULL);
			if (contentLength < sizeof content)
			{
				zco_start_receiving(dlv.adu, &reader);
				CHKZERO(sdr_begin_xn(sdr));
				len = zco_receive_source(sdr, &reader,
						contentLength, content);
				if (sdr_end_xn(sdr) < 0 || len < 0)
				{
					putErrmsg("Can't handle delivery.",
							NULL);
					state.running = 0;
					continue;
				}

				content[contentLength] = '\0';
				if (strcmp(content, "ona") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio81/value", "w");
					fprintf(fptr, "%s", "1");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "onb") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio9/value", "w");
					fprintf(fptr, "%s", "1");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "onc") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio80/value", "w");
					fprintf(fptr, "%s", "1");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "ond") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio8/value", "w");
					fprintf(fptr, "%s", "1");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "one") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio79/value", "w");
					fprintf(fptr, "%s", "1");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "onf") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio78/value", "w");
					fprintf(fptr, "%s", "1");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "ong") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio77/value", "w");
					fprintf(fptr, "%s", "1");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "onh") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio76/value", "w");
					fprintf(fptr, "%s", "1");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "oni") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio75/value", "w");
					fprintf(fptr, "%s", "1");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "onj") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio74/value", "w");
					fprintf(fptr, "%s", "1");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "onk") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio73/value", "w");
					fprintf(fptr, "%s", "1");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "onl") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio72/value", "w");
					fprintf(fptr, "%s", "1");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "offa") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio81/value", "w");
					fprintf(fptr, "%s", "0");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "offb") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio9/value", "w");
					fprintf(fptr, "%s", "0");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "offc") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio80/value", "w");
					fprintf(fptr, "%s", "0");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "offd") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio8/value", "w");
					fprintf(fptr, "%s", "0");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "offe") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio79/value", "w");
					fprintf(fptr, "%s", "0");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "offf") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio78/value", "w");
					fprintf(fptr, "%s", "0");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "offg") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio77/value", "w");
					fprintf(fptr, "%s", "0");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "offh") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio76/value", "w");
					fprintf(fptr, "%s", "0");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "offi") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio75/value", "w");
					fprintf(fptr, "%s", "0");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "offj") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio74/value", "w");
					fprintf(fptr, "%s", "0");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "offk") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio73/value", "w");
					fprintf(fptr, "%s", "0");
					fclose(fptr);
					continue;
				}
				else
				if (strcmp(content, "offl") == 0)
				{
					fptr = fopen("/sys/class/gpio/gpio72/value", "w");
					fprintf(fptr, "%s", "0");
					fclose(fptr);
					continue;
				}
				
				fflush(NULL);

			}
		}

		bp_release_delivery(&dlv, 1);
	}

	bp_close(state.sap);
	writeErrmsgMemos();
	PUTS("Stopping bpsink.");
	fflush(NULL);
	bp_detach();
	return 0;
}

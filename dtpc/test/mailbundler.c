/*      mailbundler.c: encapsulate batch mode emails into compressed
	bundles with dtpc reliability and elision features.

	Derived from bpsource.c by Scott Burleigh, with addition
	of dtpc reliability and zlib compression.
	Copyright (c) 2025 Scott Johnson, Spacely Packets, LLC.
	Released under the GNU GPLv3 License.
								*/

#include <bp.h>
#include <dtpc.h>
#include <zlib.h>
#include <stdio.h>
#include <string.h>

#define	DEFAULT_TTL 300

static DtpcSAP  _dtpcsap(DtpcSAP *newSAP)
{
        static DtpcSAP  sap = NULL;

        if (newSAP)
        {
                sap = *newSAP;
                sm_TaskVar((void **) &sap);
        }

        return sap;
}


static int      checkElision(Object recordsList)
{
        Sdr             sdr = getIonsdr();
        Object          elt;
        Object          nextElt;
        Object          obj;
        PayloadRecord   item;
        uvast           firstLength;
        uvast           length;

        CHKZERO(sdr_begin_xn(sdr));
        firstLength = 0;
        for (elt = sdr_list_first(sdr, recordsList); elt; elt = nextElt)
        {
                nextElt = sdr_list_next(sdr, elt);
                obj = sdr_list_data(sdr, elt);
                sdr_read(sdr, (char *) &item, obj, sizeof(PayloadRecord));
                oK(decodeSdnv(&length, item.length.text));
                if (firstLength == 0)
                {
                        firstLength = length;
                        continue;
                }

                if (length == firstLength)      /*      Duplicate.      */
                {
                        sdr_list_delete(sdr, elt, NULL, NULL);
                        sdr_free(sdr, item.payload);
                        sdr_free(sdr, obj);
                }
        }

        return sdr_end_xn(sdr);
}

static int	_running(int *newState)
{
	int	state = 1;

	if (newState)
	{
		state = *newState;
	}

	return state;
}

static void	handleQuit(int signum)
{
	int	stop = 0;


	oK(_running(&stop));
}

void zerr(int ret)
{
    switch (ret) {
    case Z_STREAM_ERROR:
        putErrmsg("invalid compression level\n", NULL);
        break;
    case Z_DATA_ERROR:
        putErrmsg("invalid or incomplete deflate data\n", NULL);
        break;
    case Z_MEM_ERROR:
        putErrmsg("out of memory\n", NULL);
        break;

    }
}


#if defined (ION_LWT)
int	mailbundler(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char		*destEid = (char *) a1;

#else
int	main(int argc, char **argv)
{
	char		*destEid = NULL;

	switch (argc)
	{
	case 1:
		destEid = argv[1];
	case 2:
		destEid = argv[1];
	default:
		break;
	}
#endif
	unsigned int	topicID = 25;
	int		ret = 12; 
	int		stop;
	Sdr		sdr;
	unsigned char	*compr = 0;
	uLong		comprLen = 0;
	/*uLong		structLen = 0;*/
	uLong 		textBufferLength = 0;
	int		textLength = 0;
	Object		extent;
	Address		addr;
	DtpcSAP         sap;
	DtpcElisionFn   elisionFn;
	unsigned int 	profileID = 25;
	int 		bufferLength = 0;
	int		fd;
	struct comprPl {
		uLongf 		uncomprSize;
		uLong 		comprSize;
		unsigned char	*comprData;
	} comprPayload = {0, 0, 0};

	elisionFn = checkElision;

	if (destEid == NULL)
	{
		PUTS("Usage: mailbundler <destination endpoint ID> [\"<text>\"]");
		return 0;
	}

	if (dtpc_attach() < 0)
	{
		putErrmsg("Can't attach to DTPC.", NULL);
		return 0;
	}

	if (dtpc_open(topicID, elisionFn, &sap) < 0)
        {
                putErrmsg("Can't open mail topic.", itoa(topicID));
                return 0;
        }

	oK(_dtpcsap(&sap));
	sdr = getIonsdr();
	isignal(SIGINT, handleQuit);
	fd = fileno(stdin);
	char    	*text = MTAKE(4096);
	char		*textBuffer = MTAKE(1000000);
	/*loop to collect email from stdin line by line into buffer*/
	while (igets(fd, text, 4096, &textLength) != NULL)
	{
		/*denotes end of smtp message*/
		if ((strcmp (text, ".")) == 0)
		{
			putErrmsg("last", text);
	                memcpy (textBuffer + bufferLength, text, textLength);
        	        textBuffer[bufferLength + textLength] = '\0';
                	bufferLength += textLength + 1;
			putErrmsg("bsize3", itoa(bufferLength));
			MRELEASE(text);
			close(fileno(stdin));
			break;
		}
		/*nono buffer overflow!*/
		if (bufferLength + textLength + 1 > 1000000)
		{
			MRELEASE(text);
			MRELEASE(textBuffer);
			break;
		}
		/*handle blank line*/
		if (istrlen(text, 4096) == 0)
		{
			text[0] = '\n';
			textLength++;
			/*text[1] = '\0';
			textLength++;
			text[2] = 't';
			textLength++;
			text[3] = 'r';
			textLength++;
			text[4] = '1';
			textLength++;
			text[5] = 'p';
			textLength++;
			text[6] = '-';
			textLength++;
			text[7] = '\0';
			textLength++;*/
		putErrmsg("blank", text);
		}

		/*todo: add switch/case handler for textLength return values*/
		
		putErrmsg("text", text);
		putErrmsg("bsize1", itoa(bufferLength));
		memcpy (textBuffer + bufferLength, text, textLength); 
		textBuffer[bufferLength + textLength] = '\n';
		bufferLength += textLength + 1;
		putErrmsg("bsize2", itoa(bufferLength));

	}


	if (bufferLength == 0)
	{
		putErrmsg("[?] No mail for mailbundler to send.", NULL);
		dtpc_detach();
		dtpc_close(sap);
		ionDetach();
		return 0;
	}
	/*	compression  */
	putErrmsg("compress!", NULL);

	textBufferLength = bufferLength;
	putErrmsg("get uncompressed length", itoa(textBufferLength));
	comprLen = compressBound(textBufferLength);
	putErrmsg("compress bound", itoa(comprLen));
	compr = MTAKE(comprLen);
	putErrmsg("mtake", NULL);
	ret = compress2(compr, &comprLen, (const unsigned char *)textBuffer, textBufferLength, 6);
	putErrmsg("compressed size", itoa(comprLen));
	putErrmsg("compressed!", NULL);
        if (ret != Z_OK)
	{
       	    zerr(ret);
	}
	putErrmsg("zlib return value", itoa(ret));
	/*	 end of compression  */
	/*	 pack structure  */

	comprPayload.uncomprSize = bufferLength;
	putErrmsg("uncompressed size packed into struct!", itoa(comprPayload.uncomprSize));
	comprPayload.comprSize = comprLen;
	putErrmsg("compressed size packed into struct!", itoa(comprPayload.comprSize));
	comprPayload.comprData = compr;
	putErrmsg("struct payload packed into struct!", NULL);

	 for (size_t j = 0; j < comprPayload.comprSize; j++) {
                        printf("%02x ",comprPayload.comprData[j]);
                        }


	CHKZERO(sdr_begin_xn(sdr));
	putErrmsg("begin sdr transaction!", NULL);
	/*structLen = sizeof(uLongf) + sizeof(uLong) + comprLen;*/
	extent = sdr_malloc(sdr, bufferLength);
	addr = extent;
	if (extent)
	{
		sdr_write(sdr, addr, (char *) textBuffer, bufferLength);
	}
	putErrmsg("write to sdr!", NULL);
	MRELEASE(textBuffer);

	if (sdr_end_xn(sdr) < 0)
	{
		putErrmsg("No space for mail ADU.", NULL);
		dtpc_detach();
		dtpc_close(sap);
		return 0;
	}

	switch (dtpc_send(profileID, sap, destEid, 0, 0, 0, 0, NULL, 0, 0, NULL, 0, addr, bufferLength))
	{
	case -1:
                putErrmsg("Can't send adu.", NULL);
                oK(_running(&stop));

        case 0:         /* This payload does not fit in an adu  */
 		if (sdr_begin_xn(sdr) == 0)
        	{
                	putErrmsg("Can't discard payload.", NULL);
                        oK(_running(&stop));
                }

                sdr_free(sdr, extent);

		if (sdr_end_xn(sdr) < 0)
                {
                        putErrmsg("Can't discard payload.", NULL);
                	oK(_running(&stop));
                }

                break;
	case 1:
		putErrmsg("dtpc send!", NULL);

		MRELEASE(compr);
		putErrmsg("release compressed data", NULL);


	default:
		break;
	}
	dtpc_close(sap);
	putErrmsg("dtpc close", NULL);

	dtpc_detach();
	putErrmsg("dtpc detach", NULL);

	putErrmsg("Mail bundled.", NULL);
	ionDetach();
	putErrmsg("ion detach", NULL);
	return 0;

}

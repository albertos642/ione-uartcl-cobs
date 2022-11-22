/*	libipv6.c:	IPv6 related library functions

        Author: Scott Johnson
        based on functions from platform.c
        Copyright (c) 2022, Scott Mitchell Johnson
	Dedicated to Space Pioneer SMSgt Leo B. G. Johnson, USAF, Retired

        This program is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2 of the License, or
        (at your option) any later version.

        This program is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with this program; if not, write to the Free Software
        Foundation, Inc., at:
		51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA	*/

#include "platform.h"
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

char getNameOf6Host(char *buffer, int bufferLength)
{
        char     result;

        CHKERR(buffer);
        CHKERR(bufferLength > 0);
        result = gethostname(buffer, bufferLength);
        if (result < 0)
        {
                putSysErrmsg("can't get local host name", NULL);
        }

        return result;
}

int     parseSocketSpecSix(char *socketSpec, struct sockaddr_in6 *ip6Address)
{
	char            *delimiter;
        char            *hostname;
        char            *hostAddr;
	char            hostnameBuf[MAXHOSTNAMELEN + 1];
	struct addrinfo hints, *res, *res0;
        int		error;
        char		host[NI_MAXHOST];

	CHKERR(ip6Address);

	/*initialize socket structure component values*/
	ip6Address->sin6_family = AF_INET6;
	ip6Address->sin6_addr = in6addr_any;
	ip6Address->sin6_port = htons(0);
	ip6Address->sin6_flowinfo = 0;

	if (socketSpec == NULL || *socketSpec == '\0')
        {
                return 0;               /*      Use defaults.           */
        }

	/*initialize dns lookup stucture component values*/
        memset(&hints, 0, sizeof hints);
     	hints.ai_family = AF_INET6;
        hints.ai_socktype = SOCK_DGRAM;

	/*	Start parsing.						*/
	
	delimiter = strchr(socketSpec, '!');	
	if (delimiter)			/*	Port number provided.	*/
	{
		*delimiter ='\0';	/*      Delimit host name.      */
	}

	/*	Extract IPv6 address, convert to network byte order,
	 *	and store in value in struct returned to calling daemon	*/

	hostname = socketSpec;
	if (strlen(hostname) != 0)
	{
		if (strstr(hostname, ".") != NULL)
		{
			/*	Hostname is qualified.  call dns lookup
			 *	function, and populate struct address
			 *	component.				*/

			error = getaddrinfo(hostname, NULL, &hints, &res0);
        		if (error)
			{
                		writeMemoNote("Lookup Error A:",
						(char *) gai_strerror(error));
                		return 1;
        		}

        		for (res = res0; res; res = res->ai_next)
			{
                		error = getnameinfo(res->ai_addr,
						res->ai_addrlen, host,
						sizeof host, NULL, 0,
						NI_NUMERICHOST);
                		if (error)
				{
                        		writeMemoNote("Lookup Error B:",
						(char *) gai_strerror(error));
                        		return 1;
                		}
                		else
                		{
                        		writeMemoNote("[i] Lookup Successful",
							hostname);
                        		writeMemoNote("[i] Resolves to", host);
                		}
        		}

        		freeaddrinfo(res0);
			hostAddr = (char *) host;
			inet_pton(AF_INET6, hostAddr, &ip6Address->sin6_addr);
		}
		else if (strcmp(hostname, "@") == 0)
                {
			/*	Using hostname of local machine.  find
			 *	local hostname, call dns lookup function,
			 *	and populate struct address component.	*/

                        getNameOf6Host(hostnameBuf, sizeof hostnameBuf);
                	hostname = hostnameBuf;
                        error = getaddrinfo(hostname, NULL, &hints, &res0);
                        if (error)
			{
				writeMemoNote("Lookup Error C:",
						(char *) gai_strerror(error));
                        	return 1;
                        }

                        for (res = res0; res; res = res->ai_next)
			{
                                error = getnameinfo(res->ai_addr,
						res->ai_addrlen, host,
						izeof host, NULL, 0,
						NI_NUMERICHOST);
                                if (error)
				{
                                        writeMemoNote("Lookup Error D:",
						(char *) gai_strerror(error));
                                	return 1;
                                }
                                else
                                {
                                        writeMemoNote("[i] Lookup Successful",
							hostname);
                        		writeMemoNote("[i] Resolves to", host);
                                }
                        }

                        freeaddrinfo(res0);
			hostAddr = (char *) host;
			inet_pton(AF_INET6, hostAddr, &ip6Address->sin6_addr);

		}
		else	/*	IPv6 address provided.			*/
		{
			/*	populate struct address component with
			 *	the provided address.			*/

			inet_pton(AF_INET6, hostname, &ip6Address->sin6_addr);
		}	
	}

	/*	Extract port number, convert to network byte order, and
	 *	store value in struct returned to calling daemon	*/

	if (delimiter == NULL)	/*	No port number was provided.	*/
	{
		return 0;
	}

	*delimiter = '!';	/*	Non-destructive parsing.	*/
	delimiter++;		/*	Point to start of port number.	*/
	ip6Address->sin6_port = atoi(delimiter); 
	if (ip6Address->sin6_port != 0) 
	{
		if (ip6Address->sin6_port < 1024
		|| ip6Address->sin6_port > 65535)
		{
			writeMemoNote("[?] Invalid port number.", delimiter);
			return -1;
		}

		ip6Address->sin6_port = htons(ip6Address->sin6_port);
	}

	return 0;
}

int     itcp_connect6(char *socketSpec, unsigned short defaultPort, int *sock)
{
        struct sockaddr_in6      inetName;

        CHKERR(socketSpec);
        CHKERR(sock);
        *sock = -1;             /*      Default value.                  */
        if (*socketSpec == '\0')
        {
                return 0;       /*      Don't try to connect.           */
        }

        /*      Construct socket name.                                  */

        parseSocketSpecSix(socketSpec, &inetName);
        if (inetName.sin6_port == 0)
        {
                inetName.sin6_port = htons(4556);
        }

        *sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (*sock < 0)
        {
                putSysErrmsg("Can't open TCP socket", socketSpec);
                return -1;
        }

        if (connect(*sock, (struct sockaddr *) &inetName, sizeof(inetName)) < 0)
        {
                if (errno == ECONNREFUSED)
                {
                        writeMemoNote("[i] Can't connect to TCP socket \
(refused)", socketSpec);
                }
                else
                {
                        putSysErrmsg("Can't connect to TCP socket", socketSpec);
                }

                closesocket(*sock);
                *sock = -1;
                return 0;
        }

        writeMemoNote("[i] Connected to TCP socket", socketSpec);
        return 1;       /*      Connected to remote socket.             */
}

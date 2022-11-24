/*
	tcpclo6.c:	dummy tcpclo daemon for backward compatibility.
        Author: Scott Johnson
        based on tcpclo.c by Scott Burleigh
        Copyright (c) 2022, Scott Mitchell Johnson

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
		51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
									*/
#include <bpP.h>

#if defined (ION_LWT)
int	tcpclo6(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
#else
int	main(int argc, char **argv)
{
#endif
	if (bp_attach() < 0)
	{
		putErrmsg("tcpclo6 can't attach to BP.", NULL);
		return 0;
	}

	writeMemo("[i] tcpclo6 is deprecated.  tcpcl outducts are now drained by tcpcli6 threads.");
	bp_detach();
	return 0;
}

/**************************************************************************
 *
 * Copyright (c) 2000-2003 Intel Corporation 
 * All rights reserved. 
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions are met: 
 *
 * - Redistributions of source code must retain the above copyright notice, 
 * this list of conditions and the following disclaimer. 
 * - Redistributions in binary form must reproduce the above copyright notice, 
 * this list of conditions and the following disclaimer in the documentation 
 * and/or other materials provided with the distribution. 
 * - Neither name of Intel Corporation nor the names of its contributors 
 * may be used to endorse or promote products derived from this software 
 * without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR 
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL INTEL OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, 
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, 
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR 
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY 
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS 
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 **************************************************************************/

/*!
 * \file
 *
 * \brief Implements the sockets functionality.
 */

#include "config.h"

#include "sock.h"

#include "unixutil.h"	/* for socklen_t, EAFNOSUPPORT */
#include "upnp.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>	/* for F_GETFL, F_SETFL, O_NONBLOCK */
#include <time.h>
#include <string.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int sock_init(OUT SOCKINFO *info, IN SOCKET sockfd)
{
	assert(info);

	memset(info, 0, sizeof(SOCKINFO));
	info->socket = sockfd;

	return UPNP_E_SUCCESS;
}

int sock_init_with_ip(OUT SOCKINFO *info, IN SOCKET sockfd,
	IN struct sockaddr *foreign_sockaddr)
{
	int ret;

	ret = sock_init(info, sockfd);
	if (ret != UPNP_E_SUCCESS) {
		return ret;
	}

	memcpy(&info->foreign_sockaddr, foreign_sockaddr,
	       sizeof(info->foreign_sockaddr));

	return UPNP_E_SUCCESS;
}

int sock_destroy(INOUT SOCKINFO *info, int ShutdownMethod)
{
	int ret = UPNP_E_SUCCESS;

	if (info->socket != -1) {
		shutdown(info->socket, ShutdownMethod);
		if (sock_close(info->socket) == -1) {
			ret = UPNP_E_SOCKET_ERROR;
		}
		info->socket = -1;
	}

	return ret;
}

/*!
 * \brief Receives or sends data. Also returns the time taken to receive or
 * send data.
 *
 * \return
 *	\li \c numBytes - On Success, no of bytes received or sent or
 *	\li \c UPNP_E_TIMEDOUT - Timeout
 *	\li \c UPNP_E_SOCKET_ERROR - Error on socket calls
 */
static int sock_read_write(
	/*! Socket Information Object. */
	IN SOCKINFO *info,
	/*! Buffer to get data to or send data from. */
	OUT char *buffer,
	/*! Size of the buffer. */
	IN int bufsize,
	/*! timeout value. */
	IN int *timeoutSecs,
	/*! Boolean value specifying read or write option. */
	IN int bRead)
{
	int retCode;
	fd_set readSet;
	fd_set writeSet;
	struct timeval timeout;
	long numBytes;
	time_t start_time = time(NULL);
	SOCKET sockfd = info->socket;
	long bytes_sent = 0;
	long byte_left = 0;
	long num_written;

	if (*timeoutSecs < 0)
		return UPNP_E_TIMEDOUT;
	FD_ZERO(&readSet);
	FD_ZERO(&writeSet);
	if (bRead)
		FD_SET(sockfd, &readSet);
	else
		FD_SET(sockfd, &writeSet);
	timeout.tv_sec = *timeoutSecs;
	timeout.tv_usec = 0;
	while (TRUE) {
		if (*timeoutSecs == 0)
			retCode = select(sockfd + 1, &readSet, &writeSet,
				NULL, NULL);
		else
			retCode = select(sockfd + 1, &readSet, &writeSet,
				NULL, &timeout);
		if (retCode == 0)
			return UPNP_E_TIMEDOUT;
		if (retCode == -1) {
			if (errno == EINTR)
				continue;
			return UPNP_E_SOCKET_ERROR;
		} else
			/* read or write. */
			break;
	}
#ifdef SO_NOSIGPIPE
	{
		int old;
		int set = 1;
		socklen_t olen = sizeof(old);
		getsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &old, &olen);
		setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
#endif
		if (bRead) {
			/* read data. */
			numBytes = (long)recv(sockfd, buffer, (size_t)bufsize, MSG_NOSIGNAL);
		} else {
			byte_left = bufsize;
			bytes_sent = 0;
			while (byte_left > 0) {
				/* write data. */
				num_written = send(sockfd,
					buffer + bytes_sent, (size_t)byte_left,
					MSG_DONTROUTE | MSG_NOSIGNAL);
				if (num_written == -1) {
#ifdef SO_NOSIGPIPE
					setsockopt(sockfd, SOL_SOCKET,
						SO_NOSIGPIPE, &old, olen);
#endif
					return (int)num_written;
				}
				byte_left = byte_left - num_written;
				bytes_sent += num_written;
			}
			numBytes = bytes_sent;
		}
#ifdef SO_NOSIGPIPE
		setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &old, olen);
	}
#endif
	if (numBytes < 0)
		return UPNP_E_SOCKET_ERROR;
	/* subtract time used for reading/writing. */
	if (*timeoutSecs != 0)
		*timeoutSecs -= (int)(time(NULL) - start_time);

	return (int)numBytes;
}

int sock_read(IN SOCKINFO *info, OUT char *buffer, IN int bufsize,
	      INOUT int *timeoutSecs)
{
	return sock_read_write(info, buffer, bufsize, timeoutSecs, TRUE);
}

int sock_write(IN SOCKINFO *info, IN const char *buffer, IN int bufsize,
	       INOUT int *timeoutSecs)
{
	return sock_read_write(info, (char *)buffer, bufsize, timeoutSecs, FALSE);
}

int sock_make_blocking(SOCKET sock)
{
#ifdef WIN32
	u_long val = 0;
	return ioctlsocket(sock, FIONBIO, &val);
#else
	int val;

	val = fcntl(sock, F_GETFL, 0);
	if (fcntl(sock, F_SETFL, val & ~O_NONBLOCK) == -1) {
		return -1;
	}
#endif
	return 0;
}


int sock_make_no_blocking(SOCKET sock)
{
#ifdef WIN32
	u_long val = 1;
	return ioctlsocket(sock, FIONBIO, &val);
#else /* WIN32 */
	int val;

	val = fcntl(sock, F_GETFL, 0);
	if (fcntl(sock, F_SETFL, val | O_NONBLOCK) == -1) {
		return -1;
	}
#endif /* WIN32 */
	return 0;
}

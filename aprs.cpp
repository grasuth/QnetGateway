/*
 *   Copyright (C) 2016 by Thomas A. Early N7TAE
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

 #include <string.h>
 #include <fcntl.h>
 #include <unistd.h>
 #include <sys/types.h>
 #include <sys/socket.h>
 #include <netdb.h>
 #include <netinet/tcp.h>

 #include "aprs.h"

// This is called when header comes in from repeater
void CAPRS::SelectBand(short int rptr_idx, unsigned short streamID)
{
	if ((rptr_idx < 0) || (rptr_idx > 2)) {
		traceit("ERROR in aprs_select_band, invalid mod %d\n", rptr_idx);
		return;
	}

	/* lock on the streamID */
	aprs_streamID[rptr_idx].streamID = streamID;
	// aprs_streamID[rptr_idx].last_time = 0;

	Reset(rptr_idx);
	return;
}

// This is called when data(text) comes in from repeater
// Parameter buf is either:
//              12 bytes(packet from repeater was 29 bytes) or
//              15 bytes(packet from repeater was 32 bytes)
// Parameter len is either 12 or 15, because we took passed over the first 17 bytes
//           in the repeater data
// Paramter seq is the byte at pos# 16(counting from zero) in the repeater data
void CAPRS::ProcessText(unsigned short streamID, unsigned char seq, unsigned char *buf, unsigned int len)
{
	unsigned char aprs_data[200];
	char aprs_buf[1024];
	time_t tnow = 0;

	short int rptr_idx = -1;

	len = len;

	for (short int i = 0; i < 3; i++) {
		if (streamID == aprs_streamID[i].streamID) {
			rptr_idx = i;
			break;
		}
	}

	if ((rptr_idx < 0) || (rptr_idx > 2)) {
		// traceit("ERROR in aprs_process_text: rptr_idx %d is invalid\n", rptr_idx);
		return;
	}

	if ((seq & 0x40) == 0x40)
		return;

	if ((seq & 0x1f) == 0x00) {
		SyncIt(rptr_idx);
		return;
	}

	bool done = WriteData(rptr_idx, buf + 9);
	if (!done)
		return;

	unsigned int aprs_len = GetData(rptr_idx, aprs_data, 200);
	aprs_data[aprs_len] = '\0';

	time(&tnow);
	if ((tnow - aprs_streamID[rptr_idx].last_time) < 30)
		return;

	if (aprs_sock == -1)
		return;

	char *p = strchr((char*)aprs_data, ':');
	if (!p) {
		Reset(rptr_idx);
		return;
	}
	*p = '\0';


	char *hdr = (char *)aprs_data;
	char *aud = p + 1;
	if (strchr(hdr, 'q') != NULL)
		return;

	p = strchr(aud, '\r');
	*p = '\0';

	sprintf(aprs_buf, "%s,qAR,%s:%s\r\n", hdr, rptr.mod[rptr_idx].call.c_str(), aud);
	// traceit("GPS-A=%s", aprs_buf);
	int rc = WriteSock(aprs_buf, strlen(aprs_buf));
	if (rc == -1) {
		if ((errno == EPIPE) ||
		        (errno == ECONNRESET) ||
		        (errno == ETIMEDOUT) ||
		        (errno == ECONNABORTED) ||
		        (errno == ESHUTDOWN) ||
		        (errno == EHOSTUNREACH) ||
		        (errno == ENETRESET) ||
		        (errno == ENETDOWN) ||
		        (errno == ENETUNREACH) ||
		        (errno == EHOSTDOWN) ||
		        (errno == ENOTCONN)) {
			traceit("CAPRS::ProcessText(): APRS_HOST closed connection,error=%d\n",errno);
			close(aprs_sock);
			aprs_sock = -1;
		} else /* if it is WOULDBLOCK, we will not go into a loop here */
			traceit("CAPRS::ProcessText(): send error=%d\n", errno);
	}

	time(&aprs_streamID[rptr_idx].last_time);

	return;
}

void CAPRS::Init()
{
	/* Initialize the statistics on the APRS packets */
	for (short int rptr_idx = 0; rptr_idx < 3; rptr_idx++) {
		aprs_pack[rptr_idx].al = al_none;
		aprs_pack[rptr_idx].data[0] = '\0';
		aprs_pack[rptr_idx].len = 0;
		aprs_pack[rptr_idx].buf[0] = '\0';
		aprs_pack[rptr_idx].sl = sl_first;
		aprs_pack[rptr_idx].is_sent = false;
	}

	for (short int i = 0; i < 3; i++) {
		aprs_streamID[i].streamID = 0;
		aprs_streamID[i].last_time = 0;
	}
	
	/* Initialize the APRS host */
	memset(&aprs_addr,0,sizeof(struct sockaddr_in));
	aprs_addr_len = sizeof(aprs_addr);

	return;
}

int CAPRS::GetSock()
{
	return aprs_sock;
}

void CAPRS::SetSock(int value)
{
	aprs_sock = value;
}

bool CAPRS::WriteData(short int rptr_idx, unsigned char *data)
{

	if ((rptr_idx < 0) || (rptr_idx > 2)) {
		traceit("CAPRS::WriteData: rptr_idx %d is invalid\n", rptr_idx);
		return false;
	}

	if (aprs_pack[rptr_idx].is_sent)
		return false;

	switch (aprs_pack[rptr_idx].sl) {
	case sl_first:
		aprs_pack[rptr_idx].buf[0] = data[0] ^ 0x70;
		aprs_pack[rptr_idx].buf[1] = data[1] ^ 0x4f;
		aprs_pack[rptr_idx].buf[2] = data[2] ^ 0x93;
		aprs_pack[rptr_idx].sl = sl_second;
		return false;

	case sl_second:
		aprs_pack[rptr_idx].buf[3] = data[0] ^ 0x70;
		aprs_pack[rptr_idx].buf[4] = data[1] ^ 0x4f;
		aprs_pack[rptr_idx].buf[5] = data[2] ^ 0x93;
		aprs_pack[rptr_idx].sl = sl_first;
		break;
	}

	if ((aprs_pack[rptr_idx].buf[0] & 0xf0) != 0x30)
		return false;

	return AddData(rptr_idx, aprs_pack[rptr_idx].buf + 1);

}

void CAPRS::SyncIt(short int rptr_idx)
{
	if ((rptr_idx < 0) || (rptr_idx > 2)) {
		traceit("CAPRS::SyncIt(): rptr_idx %d is invalid\n", rptr_idx);
		return;
	}

	aprs_pack[rptr_idx].sl = sl_first;
	return;
}

void CAPRS::Reset(short int rptr_idx)
{
	if ((rptr_idx < 0) || (rptr_idx > 2)) {
		traceit("CAPRS::Reset(): rptr_idx %d is invalid\n", rptr_idx);
		return;
	}

	aprs_pack[rptr_idx].al = al_none;
	aprs_pack[rptr_idx].len = 0;
	aprs_pack[rptr_idx].sl = sl_first;
	aprs_pack[rptr_idx].is_sent = false;

	return;
}

unsigned int CAPRS::GetData(short int rptr_idx, unsigned char *data, unsigned int len)
{
	if ((rptr_idx < 0) || (rptr_idx > 2)) {
		traceit("CAPRS::GetData: rptr_idx %d is invalid\n", rptr_idx);
		return 0;
	}

	unsigned int l = aprs_pack[rptr_idx].len - 10;

	if (l > len)
		l = len;

	memcpy(data, aprs_pack[rptr_idx].data  + 10, l);

	aprs_pack[rptr_idx].al = al_none;
	aprs_pack[rptr_idx].len = 0;
	aprs_pack[rptr_idx].is_sent = true;

	return l;
}

void CAPRS::Open(const std::string OWNER)
{
	struct timespec req;

	fd_set fdset;
	struct timeval tv;
	short int MAX_WAIT = 15; /* 15 seconds wait time MAX */
	int val = 1;
	socklen_t val_len;
	char snd_buf[512];
	char rcv_buf[512];

	bool ok = ResolveRmt(rptr.aprs.ip.c_str(), SOCK_STREAM, &aprs_addr);
	if (!ok) {
		traceit("Can't resolve APRS_HOST %s\n", rptr.aprs.ip.c_str());
		return;
	}

	/* fill it in */
	aprs_addr.sin_family = AF_INET;
	aprs_addr.sin_port = htons(rptr.aprs.port);

	aprs_addr_len = sizeof(aprs_addr);

	aprs_sock = socket(PF_INET, SOCK_STREAM, 0);
	if (aprs_sock == -1) {
		traceit("Failed to create aprs socket,error=%d\n",errno);
		return;
	}
	fcntl(aprs_sock,F_SETFL,O_NONBLOCK);

	val = 1;
	if (setsockopt(aprs_sock,IPPROTO_TCP,TCP_NODELAY,(char *)&val, sizeof(val)) == -1) {
		traceit("setsockopt TCP_NODELAY TCP for aprs socket failed,error=%d\n",errno);
		close(aprs_sock);
		aprs_sock = -1;
		return;
	}

	traceit("Trying to connect to APRS...\n");
	int rc = connect(aprs_sock, (struct sockaddr *)&aprs_addr, aprs_addr_len);
	if (rc != 0) {
		if (errno == EINPROGRESS) {
			traceit("Waiting for up to %d seconds for APRS_HOST\n", MAX_WAIT);
			while (MAX_WAIT > 0) {
				tv.tv_sec = 0;
				tv.tv_usec = 0;
				FD_ZERO(&fdset);
				FD_SET(aprs_sock, &fdset);
				rc = select(aprs_sock + 1, NULL,  &fdset, NULL, &tv);

				if (rc < 0) {
					traceit("Failed to connect to APRS...select,error=%d\n", errno);
					close(aprs_sock);
					aprs_sock = -1;
					return;
				} else if (rc == 0) { /* timeout */
					MAX_WAIT--;
					sleep(1);
				} else {
					val = 1; /* Assume it fails */
					val_len = sizeof(val);
					if (getsockopt(aprs_sock, SOL_SOCKET, SO_ERROR, (char *) &val, &val_len) < 0) {
						traceit("Failed to connect to APRS...getsockopt, error=%d\n", errno);
						close(aprs_sock);
						aprs_sock = -1;
						return;
					} else if (val == 0)
						break;

					MAX_WAIT--;
					sleep(1);
				}
			}
			if (MAX_WAIT == 0) {
				traceit("Failed to connect to APRS...timeout\n");
				close(aprs_sock);
				aprs_sock = -1;
				return;
			}
		} else {
			traceit("Failed to connect to APRS, error=%d\n", errno);
			close(aprs_sock);
			aprs_sock = -1;
			return;
		}
	}
	traceit("Connected to APRS %s:%d\n", rptr.aprs.ip.c_str(), rptr.aprs.port);

	/* login to aprs */
	sprintf(snd_buf, "user %s pass %d vers g2_ircddb 2.99 UDP 5 ", OWNER.c_str(), rptr.aprs_hash);

	/* add the user's filter */
	if (rptr.aprs_filter.length()) {
		strcat(snd_buf, "filter ");
		strcat(snd_buf, rptr.aprs_filter.c_str());
	}
	// traceit("APRS login command:[%s]\n", snd_buf);
	strcat(snd_buf, "\r\n");

	while (true) {
		rc = WriteSock(snd_buf, strlen(snd_buf));
		if (rc < 0) {
			if (errno == EWOULDBLOCK) {
				recv(aprs_sock, rcv_buf, sizeof(rcv_buf), 0);
				req.tv_sec = 0;
				req.tv_nsec = 100000000; // 100 milli
				nanosleep(&req, NULL);
			} else {
				traceit("APRS login command failed, error=%d\n", errno);
				break;
			}
		} else {
			// traceit("APRS login command sent\n");
			break;
		}
	}
	recv(aprs_sock, rcv_buf, sizeof(rcv_buf), 0);

	return;
}

bool CAPRS::AddData(short int rptr_idx, unsigned char *data)
{
	if ((rptr_idx < 0) || (rptr_idx > 2)) {
		traceit("CAPRS::AddData(): rptr_idx %d is invalid\n", rptr_idx);
		return false;
	}

	for (unsigned int i = 0; i < 5; i++) {
		unsigned char c = data[i];

		if ((aprs_pack[rptr_idx].al == al_none) && (c == '$')) {
			aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
			aprs_pack[rptr_idx].len++;
			aprs_pack[rptr_idx].al = al_$1;
		} else if ((aprs_pack[rptr_idx].al == al_$1) && (c == '$')) {
			aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
			aprs_pack[rptr_idx].len++;
			aprs_pack[rptr_idx].al = al_$2;
		} else if ((aprs_pack[rptr_idx].al == al_$2) && (c == 'C')) {
			aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
			aprs_pack[rptr_idx].len++;
			aprs_pack[rptr_idx].al = al_c1;
		} else if ((aprs_pack[rptr_idx].al == al_c1) && (c == 'R')) {
			aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
			aprs_pack[rptr_idx].len++;
			aprs_pack[rptr_idx].al = al_r1;
		} else if ((aprs_pack[rptr_idx].al  == al_r1) && (c == 'C')) {
			aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
			aprs_pack[rptr_idx].len++;
			aprs_pack[rptr_idx].al = al_c2;
		} else if (aprs_pack[rptr_idx].al == al_c2) {
			aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
			aprs_pack[rptr_idx].len++;
			aprs_pack[rptr_idx].al = al_csum1;
		} else if (aprs_pack[rptr_idx].al == al_csum1) {
			aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
			aprs_pack[rptr_idx].len++;
			aprs_pack[rptr_idx].al = al_csum2;
		} else if (aprs_pack[rptr_idx].al == al_csum2) {
			aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
			aprs_pack[rptr_idx].len++;
			aprs_pack[rptr_idx].al = al_csum3;
		} else if (aprs_pack[rptr_idx].al == al_csum3) {
			aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
			aprs_pack[rptr_idx].len++;
			aprs_pack[rptr_idx].al = al_csum4;
		} else if ((aprs_pack[rptr_idx].al == al_csum4) && (c == ',')) {
			aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
			aprs_pack[rptr_idx].len++;
			aprs_pack[rptr_idx].al = al_data;
		} else if ((aprs_pack[rptr_idx].al == al_data) && (c != '\r')) {
			aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
			aprs_pack[rptr_idx].len++;

			if (aprs_pack[rptr_idx].len >= 300) {
				traceit("ERROR in aprs_add_data: Expected END of APRS data\n");
				aprs_pack[rptr_idx].len = 0;
				aprs_pack[rptr_idx].al  = al_none;
			}
		} else if ((aprs_pack[rptr_idx].al == al_data) && (c == '\r')) {
			aprs_pack[rptr_idx].data[aprs_pack[rptr_idx].len] = c;
			aprs_pack[rptr_idx].len++;


			bool ok = CheckData(rptr_idx);
			if (ok) {
				aprs_pack[rptr_idx].al = al_end;
				return true;
			} else {
				traceit("BAD checksum in APRS data\n");
				aprs_pack[rptr_idx].al  = al_none;
				aprs_pack[rptr_idx].len = 0;
			}
		} else {
			aprs_pack[rptr_idx].al  = al_none;
			aprs_pack[rptr_idx].len = 0;
		}
	}
	return false;
}

bool CAPRS::CheckData(short int rptr_idx)
{
	unsigned int my_sum;
	char buf[5];

	if ((rptr_idx < 0) || (rptr_idx > 2)) {
		traceit("CAPRS::CheckData(): rptr_idx %d is invalid\n", rptr_idx);
		return false;
	}
	my_sum = CalcCRC(aprs_pack[rptr_idx].data + 10, aprs_pack[rptr_idx].len - 10);

	sprintf(buf, "%04X", my_sum);

	return (0 == memcmp(buf, aprs_pack[rptr_idx].data + 5, 4));
}

unsigned int CAPRS::CalcCRC(unsigned char* buf, unsigned int len)
{
	unsigned int my_crc = 0xffff;

	if (!buf)
		return 0;

	if (len <= 0)
		return 0;

	for (unsigned int j = 0; j < len; j++) {
		unsigned int c = buf[j];

		for (unsigned int i = 0; i < 8; i++) {
			bool xor_val = (((my_crc ^ c) & 0x01) == 0x01);
			my_crc >>= 1;

			if (xor_val)
				my_crc ^= 0x8408;

			c >>= 1;
		}
	}
	return (~my_crc & 0xffff);
}

ssize_t CAPRS::WriteSock(char *buffer, size_t n)
{
	ssize_t num_written = 0;
	size_t tot_written = 0;
	char *buf = buffer;
	
	for (tot_written = 0; tot_written < n;) {
		num_written = write(aprs_sock, buf, n - tot_written);
		if (num_written <= 0) {
			if ((num_written == -1) && (errno == EINTR))
				continue;
			else
				return num_written;
		}
		tot_written += num_written;
		buf += num_written;
	}
	return tot_written;
}

bool CAPRS::ResolveRmt(const char *name, int type, struct sockaddr_in *addr)
{
	struct addrinfo hints;
	struct addrinfo *res;
	struct addrinfo *rp;
	bool found = false;

	memset(&hints, 0x00, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	hints.ai_socktype = type;

	int rc = getaddrinfo(name, NULL, &hints, &res);
	if (rc != 0) {
		traceit("getaddrinfo return error code %d for [%s]\n", rc, name);
		return false;
	}

	for (rp = res; rp != NULL; rp = rp->ai_next) {
		if ((rp->ai_family == AF_INET) &&
		        (rp->ai_socktype == type)) {
			memcpy(addr, rp->ai_addr, sizeof(struct sockaddr_in));
			found = true;
			break;
		}
	}
	freeaddrinfo(res);
	return found;
}

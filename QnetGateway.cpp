/*
 *   Copyright (C) 2010 by Scott Lawson KI4LKF
 *   Copyright (C) 2017-2020 by Thomas Early N7TAE
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

#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <regex>
#include <future>
#include <exception>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>

#include "IRCDDB.h"
#include "IRCutils.h"
#include "QnetConfigure.h"
#include "QnetGateway.h"
#include "Utilities.h"

#ifndef CFG_DIR
#define CFG_DIR "/usr/local/etc"
#endif

const std::string GW_VERSION("QnetGateway-9.4.1");

static std::atomic<bool> keep_running(true);

/* signal catching function */
static void sigCatch(int)
{
	keep_running = false;
}

int CQnetGateway::FindIndex(const int i) const
{
    if (i<0 || i > 2)
        return -1;
    int index = Index[i];
    if (index < 0) {
        if (AF_INET == link_family[i]) {
            index = ii[1] ? 1 : 0;
        } else if (AF_INET6 == link_family[i]) {
            index = 0;
        }
    }
    return index;
}

bool CQnetGateway::VoicePacketIsSync(const unsigned char *text)
{
	return *text==0x55U && *(text+1)==0x2DU && *(text+2)==0x16U;
}

void CQnetGateway::UnpackCallsigns(const std::string &str, std::set<std::string> &set, const std::string &delimiters)
{
	std::string::size_type lastPos = str.find_first_not_of(delimiters, 0);	// Skip delimiters at beginning.
	std::string::size_type pos = str.find_first_of(delimiters, lastPos);	// Find first non-delimiter.

	while (std::string::npos != pos || std::string::npos != lastPos) {
		std::string element = str.substr(lastPos, pos-lastPos);
		if (element.length()>=3 && element.length()<=8) {
			ToUpper(element);
			element.resize(CALL_SIZE, ' ');
			set.insert(element);	// Found a token, add it to the list.
		} else
			fprintf(stderr, "found bad callsign in list: %s\n", str.c_str());
		lastPos = str.find_first_not_of(delimiters, pos);	// Skip delimiters.
		pos = str.find_first_of(delimiters, lastPos);	// Find next non-delimiter.
	}
}

void CQnetGateway::PrintCallsigns(const std::string &key, const std::set<std::string> &set)
{
	printf("%s = [", key.c_str());
	for (auto it=set.begin(); it!=set.end(); it++) {
		if (it != set.begin())
			printf(",");
		printf("%s", (*it).c_str());
	}
	printf("]\n");
}


void CQnetGateway::set_dest_rptr(int mod_ndx, char *dest_rptr)
{
	FILE *statusfp = fopen(FILE_STATUS.c_str(), "r");
	if (statusfp) {
		setvbuf(statusfp, (char *)NULL, _IOLBF, 0);

		char statusbuf[1024];
		while (fgets(statusbuf, 1020, statusfp) != NULL) {
			char *p = strchr(statusbuf, '\r');
			if (p)
				*p = '\0';
			p = strchr(statusbuf, '\n');
			if (p)
				*p = '\0';

			const char *delim = ",";
			char *saveptr = NULL;
			char *status_local_mod = strtok_r(statusbuf, delim, &saveptr);
			char *status_remote_stm = strtok_r(NULL, delim, &saveptr);
			char *status_remote_mod = strtok_r(NULL, delim, &saveptr);

			if (!status_local_mod || !status_remote_stm || !status_remote_mod)
				continue;

			if ( ((*status_local_mod == 'A') && (mod_ndx == 0))  ||
			     ((*status_local_mod == 'B') && (mod_ndx == 1))  ||
			     ((*status_local_mod == 'C') && (mod_ndx == 2)) ) {
				strncpy(dest_rptr, status_remote_stm, CALL_SIZE);
				dest_rptr[7] = *status_remote_mod;
				dest_rptr[CALL_SIZE] = '\0';
				break;
			}
		}
		fclose(statusfp);
	}
	return;
}

/* compute checksum */
void CQnetGateway::calcPFCS(unsigned char *packet, int len)
{
	const unsigned short crc_tabccitt[256] = {
		0x0000,0x1189,0x2312,0x329b,0x4624,0x57ad,0x6536,0x74bf,0x8c48,0x9dc1,0xaf5a,0xbed3,0xca6c,0xdbe5,0xe97e,0xf8f7,
		0x1081,0x0108,0x3393,0x221a,0x56a5,0x472c,0x75b7,0x643e,0x9cc9,0x8d40,0xbfdb,0xae52,0xdaed,0xcb64,0xf9ff,0xe876,
		0x2102,0x308b,0x0210,0x1399,0x6726,0x76af,0x4434,0x55bd,0xad4a,0xbcc3,0x8e58,0x9fd1,0xeb6e,0xfae7,0xc87c,0xd9f5,
		0x3183,0x200a,0x1291,0x0318,0x77a7,0x662e,0x54b5,0x453c,0xbdcb,0xac42,0x9ed9,0x8f50,0xfbef,0xea66,0xd8fd,0xc974,
		0x4204,0x538d,0x6116,0x709f,0x0420,0x15a9,0x2732,0x36bb,0xce4c,0xdfc5,0xed5e,0xfcd7,0x8868,0x99e1,0xab7a,0xbaf3,
		0x5285,0x430c,0x7197,0x601e,0x14a1,0x0528,0x37b3,0x263a,0xdecd,0xcf44,0xfddf,0xec56,0x98e9,0x8960,0xbbfb,0xaa72,
		0x6306,0x728f,0x4014,0x519d,0x2522,0x34ab,0x0630,0x17b9,0xef4e,0xfec7,0xcc5c,0xddd5,0xa96a,0xb8e3,0x8a78,0x9bf1,
		0x7387,0x620e,0x5095,0x411c,0x35a3,0x242a,0x16b1,0x0738,0xffcf,0xee46,0xdcdd,0xcd54,0xb9eb,0xa862,0x9af9,0x8b70,
		0x8408,0x9581,0xa71a,0xb693,0xc22c,0xd3a5,0xe13e,0xf0b7,0x0840,0x19c9,0x2b52,0x3adb,0x4e64,0x5fed,0x6d76,0x7cff,
		0x9489,0x8500,0xb79b,0xa612,0xd2ad,0xc324,0xf1bf,0xe036,0x18c1,0x0948,0x3bd3,0x2a5a,0x5ee5,0x4f6c,0x7df7,0x6c7e,
		0xa50a,0xb483,0x8618,0x9791,0xe32e,0xf2a7,0xc03c,0xd1b5,0x2942,0x38cb,0x0a50,0x1bd9,0x6f66,0x7eef,0x4c74,0x5dfd,
		0xb58b,0xa402,0x9699,0x8710,0xf3af,0xe226,0xd0bd,0xc134,0x39c3,0x284a,0x1ad1,0x0b58,0x7fe7,0x6e6e,0x5cf5,0x4d7c,
		0xc60c,0xd785,0xe51e,0xf497,0x8028,0x91a1,0xa33a,0xb2b3,0x4a44,0x5bcd,0x6956,0x78df,0x0c60,0x1de9,0x2f72,0x3efb,
		0xd68d,0xc704,0xf59f,0xe416,0x90a9,0x8120,0xb3bb,0xa232,0x5ac5,0x4b4c,0x79d7,0x685e,0x1ce1,0x0d68,0x3ff3,0x2e7a,
		0xe70e,0xf687,0xc41c,0xd595,0xa12a,0xb0a3,0x8238,0x93b1,0x6b46,0x7acf,0x4854,0x59dd,0x2d62,0x3ceb,0x0e70,0x1ff9,
		0xf78f,0xe606,0xd49d,0xc514,0xb1ab,0xa022,0x92b9,0x8330,0x7bc7,0x6a4e,0x58d5,0x495c,0x3de3,0x2c6a,0x1ef1,0x0f78
	};
	unsigned short crc_dstar_ffff = 0xffff;
	short int low, high;
	unsigned short tmp;

	switch (len) {
		case 56:
			low = 15;
			high = 54;
			break;
		case 58:
			low = 17;
			high = 56;
			break;
		default:
			return;
	}

	for (unsigned short int i = low; i < high ; i++) {
		unsigned short short_c = 0x00ff & (unsigned short)packet[i];
		tmp = (crc_dstar_ffff & 0x00ff) ^ short_c;
		crc_dstar_ffff = (crc_dstar_ffff >> 8) ^ crc_tabccitt[tmp];
	}
	crc_dstar_ffff =  ~crc_dstar_ffff;
	tmp = crc_dstar_ffff;

	if (len == 56) {
		packet[54] = (unsigned char)(crc_dstar_ffff & 0xff);
		packet[55] = (unsigned char)((tmp >> 8) & 0xff);
	} else {
		packet[56] = (unsigned char)(crc_dstar_ffff & 0xff);
		packet[57] = (unsigned char)((tmp >> 8) & 0xff);
	}

	return;
}

/* process configuration file */
bool CQnetGateway::ReadConfig(char *cfgFile)
{
	const std::string estr;	// an empty string
	CQnetConfigure cfg;
	if (cfg.Initialize(cfgFile))
		return true;

	// ircddb
	std::string path("ircddb_login");
	if (cfg.GetValue(path, estr, owner, 3, CALL_SIZE-2))
		return true;
	OWNER = owner;
	ToLower(owner);
	ToUpper(OWNER);
	printf("OWNER='%s'\n", OWNER.c_str());
	OWNER.resize(CALL_SIZE, ' ');

	path.assign("ircddb");
	for (int i=0; i<2; i++) {
		std::string p(path + std::to_string(i) + "_");
		cfg.GetValue(p+"host", estr, ircddb[i].ip, 0, MAXHOSTNAMELEN);
		cfg.GetValue(p+"port", estr, ircddb[i].port, 1000, 65535);
		cfg.GetValue(p+"password", estr, IRCDDB_PASSWORD[i], 0, 512);
	}
	if (0 == ircddb[0].ip.compare(ircddb[1].ip)) {
		fprintf(stderr, "IRC networks must be different\n");
		return true;
	}

	// module
	for (int m=0; m<3; m++) {
		path.assign("module_");
		path.append(1, 'a' + m);
		std::string type;
		if (cfg.GetValue(path, estr, type, 1, 16)) {
			Rptr.mod[m].defined = false;
		} else {
			printf("Found Module: %s = '%s'\n", path.c_str(), type.c_str());
			if      (0 == type.compare("dvap"))       { Rptr.mod[m].package_version.assign(GW_VERSION+".DVAP"); }
			else if (0 == type.compare("dvrptr"))     { Rptr.mod[m].package_version.assign(GW_VERSION+".DVRPTR"); }
			else if (0 == type.compare("mmdvmhost"))  { Rptr.mod[m].package_version.assign(GW_VERSION+".Relay"); }
			else if (0 == type.compare("mmdvmmodem")) { Rptr.mod[m].package_version.assign(GW_VERSION+".Modem"); }
			else if (0 == type.compare("itap"))       { Rptr.mod[m].package_version.assign(GW_VERSION+".ITAP"); }
			else {
				printf("module type '%s' is invalid\n", type.c_str());
				return true;
			}
			Rptr.mod[m].defined = true;

			path.append(1, '_');
			if (cfg.KeyExists(path+"tx_frequency")) {
				cfg.GetValue(path+"tx_frequency", type, Rptr.mod[m].frequency, 0.0, 6.0E9);
				double rx_freq;
				cfg.GetValue(path+"rx_frequency", type, rx_freq, 0.0, 6.0E9);
				if (0.0 == rx_freq)
					rx_freq = Rptr.mod[m].frequency;
				Rptr.mod[m].offset = rx_freq - Rptr.mod[m].frequency;
			} else if (cfg.KeyExists(path+"frequency")) {
				cfg.GetValue(path+"frequency", type, Rptr.mod[m].frequency, 0.0, 1.0E9);
				Rptr.mod[m].offset = 0.0;
			} else {
				Rptr.mod[m].frequency = Rptr.mod[m].offset = 0.0;
			}
			cfg.GetValue(path+"range", type, Rptr.mod[m].range, 0.0, 1609344.0);
			cfg.GetValue(path+"agl", type, Rptr.mod[m].agl, 0.0, 1000.0);
		}
	}
	if (! (Rptr.mod[0].defined || Rptr.mod[1].defined || Rptr.mod[2].defined)) {
		printf("No modules defined!\n");
		return true;
	}

	// gateway
	path.assign("gateway_");
	cfg.GetValue(path+"ip", estr, g2_external.ip, 7, 64);
	cfg.GetValue(path+"port", estr, g2_external.port, 1024, 65535);
	cfg.GetValue(path+"ipv6_ip", estr, g2_ipv6_external.ip, 7, 64);
	cfg.GetValue(path+"ipv6_port", estr, g2_ipv6_external.port, 1024, 65535);
	cfg.GetValue(path+"header_regen", estr, GATEWAY_HEADER_REGEN);
	cfg.GetValue(path+"send_qrgs_maps", estr, GATEWAY_SEND_QRGS_MAP);
	cfg.GetValue(path+"gate2link", estr, gate2link, 1, FILENAME_MAX);
	cfg.GetValue(path+"link2gate", estr, link2gate, 1, FILENAME_MAX);
	cfg.GetValue(path+"modem2gate", estr, modem2gate, 1, FILENAME_MAX);
	for (int m=0; m<3; m++) {
		if (Rptr.mod[m].defined) {
			cfg.GetValue(path+"gate2modem"+std::string(1, 'a'+m), estr, gate2modem[m], 1, FILENAME_MAX);
			cfg.GetValue(path+"latitude", estr, Rptr.mod[m].latitude, -90.0, 90.0);
			cfg.GetValue(path+"longitude", estr, Rptr.mod[m].longitude, -180.0, 180.0);
			cfg.GetValue(path+"desc1", estr, Rptr.mod[m].desc1, 0, 20);
			cfg.GetValue(path+"desc2", estr, Rptr.mod[m].desc2, 0, 20);
			cfg.GetValue(path+"url", estr, Rptr.mod[m].url, 0, 80);
		}
	}
	path.append("find_route");
	if (cfg.KeyExists(path)) {
		std::string csv;
		cfg.GetValue(path, estr, csv, 0, 10240);
		UnpackCallsigns(csv, findRoute);
		PrintCallsigns(path, findRoute);
	}

	// APRS
	path.assign("aprs_");
	cfg.GetValue(path+"enable", estr, APRS_ENABLE);
	cfg.GetValue(path+"host", estr, Rptr.aprs.ip, 7, MAXHOSTNAMELEN);
	cfg.GetValue(path+"port", estr, Rptr.aprs.port, 10000, 65535);
	cfg.GetValue(path+"interval", estr, Rptr.aprs_interval, 40, 1000);
	cfg.GetValue(path+"filter", estr, Rptr.aprs_filter, 0, 512);

	// log
	path.assign("log_");
	cfg.GetValue(path+"qso", estr, LOG_QSO);
	cfg.GetValue(path+"irc", estr, LOG_IRC);
	cfg.GetValue(path+"dtmf", estr, LOG_DTMF);
	cfg.GetValue(path+"debug", estr, LOG_DEBUG);

	// file
	path.assign("file_");
	cfg.GetValue(path+"echotest", estr, FILE_ECHOTEST, 2, FILENAME_MAX);
	cfg.GetValue(path+"dtmf", estr, FILE_DTMF, 2, FILENAME_MAX);
	cfg.GetValue(path+"status", estr, FILE_STATUS, 2, FILENAME_MAX);
	cfg.GetValue(path+"qnvoice_file", estr, FILE_QNVOICE_FILE, 2, FILENAME_MAX);

	// timing
	path.assign("timing_play_");
	cfg.GetValue(path+"wait", estr, TIMING_PLAY_WAIT, 1, 10);
	cfg.GetValue(path+"delay", estr, TIMING_PLAY_DELAY, 9, 25);
	path.assign("timing_timeout_");
	cfg.GetValue(path+"echo", estr, TIMING_TIMEOUT_ECHO, 1, 10);
	cfg.GetValue(path+"voicemail", estr, TIMING_TIMEOUT_VOICEMAIL, 1, 10);
	cfg.GetValue(path+"remote_g2", estr, TIMING_TIMEOUT_REMOTE_G2, 1, 10);
	cfg.GetValue(path+"local_rptr", estr, TIMING_TIMEOUT_LOCAL_RPTR, 1, 10);

	// dashboard
	path.assign("dash_");
	cfg.GetValue(path+"show_lh", estr, DASH_SHOW_LH);
	cfg.GetValue(path+"sql_filename", estr, DASH_SQL_NAME, 1, 32);

	return false;
}

// Create ports
int CQnetGateway::open_port(const SPORTIP *pip, int family)
{
	CSockAddress sin(family, pip->port, pip->ip.c_str());

	int sock = socket(family, SOCK_DGRAM, 0);
	if (0 > sock) {
		printf("Failed to create socket on %s:%d, errno=%d, %s\n", pip->ip.c_str(), pip->port, errno, strerror(errno));
		return -1;
	}
	fcntl(sock, F_SETFL, O_NONBLOCK);

	//int reuse = 1;
	//if (::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse)) == -1) {
	//	printf("Cannot set the UDP socket (port %u) option, err: %d, %s\n", pip->port, errno, strerror(errno));
	//	return -1;
	//}

	if (bind(sock, sin.GetPointer(), sizeof(struct sockaddr_storage)) != 0) {
		printf("Failed to bind %s:%d, errno=%d, %s\n", pip->ip.c_str(), pip->port, errno, strerror(errno));
		close(sock);
		return -1;
	}

	return sock;
}

/* receive data from the irc server and save it */
void CQnetGateway::GetIRCDataThread(const int i)
{
	IRCDDB_RESPONSE_TYPE type;
	short last_status = 0;

	short threshold = 0;
	bool not_announced[3];
	for (int i=0; i<3; i++)
		not_announced[i] = this->Rptr.mod[i].defined;	// announce to all modules that are defined!
	bool is_quadnet = (std::string::npos != ircddb[i].ip.find(".openquad.net"));
	bool doFind = true;
	while (keep_running) {
		int rc = ii[i]->getConnectionState();
		if (rc > 5 && rc < 8 && is_quadnet) {
			char ch = '\0';
			if (not_announced[0])
				ch = 'A';
			else if (not_announced[1])
				ch = 'B';
			else if (not_announced[2])
				ch = 'C';
			if (ch) {
				// we need to announce, but can we?
				struct stat sbuf;
				if (stat(FILE_QNVOICE_FILE.c_str(), &sbuf)) {
					// yes, there is no FILE_QNVOICE_FILE, so create it
					FILE *fp = fopen(FILE_QNVOICE_FILE.c_str(), "w");
					if (fp) {
						fprintf(fp, "%c_connected2network.dat_WELCOME_TO_QUADNET", ch);
						fclose(fp);
						not_announced[ch - 'A'] = false;
					} else
						fprintf(stderr, "could not open %s\n", FILE_QNVOICE_FILE.c_str());
				}
			}
			if (doFind) {
				printf("Finding Routes for...\n");
				for (auto it=findRoute.begin(); it!=findRoute.end(); it++) {
					std::this_thread::sleep_for(std::chrono::milliseconds(800));
					printf("\t'%s'\n", it->c_str());
					ii[i]->findUser(*it);
				}
				doFind = false;
			}
		}
		threshold++;
		if (threshold >= 100) {
			if ((rc == 0) || (rc == 10)) {
				if (last_status != 0) {
					printf("irc status=%d, probable disconnect...\n", rc);
					last_status = 0;
				}
			} else if (rc == 7) {
				if (last_status != 2) {
					printf("irc status=%d, probable connect...\n", rc);
					last_status = 2;
				}
			} else {
				if (last_status != 1) {
					printf("irc status=%d, probable connect...\n", rc);
					last_status = 1;
				}
			}
			threshold = 0;
		}

		while (((type = ii[i]->getMessageType()) != IDRT_NONE) && keep_running) {
			switch (type) {
				case IDRT_USER: {
					std::string user, rptr, gate, addr, utime;
					ii[i]->receiveUser(user, rptr, gate, addr, utime);
					if (LOG_IRC)
						printf("U%d u[%s] r[%s] g[%s] a[%s] t[%s]\n", i, user.c_str(), rptr.c_str(), gate.c_str(), addr.c_str(), utime.c_str());
					cache.updateUser(user, rptr, gate, addr, utime);
				}
				break;

				case IDRT_PING: {
					std::string rptr, gate, addr;
					ii[i]->receivePing(rptr);
					if (! rptr.empty()) {
						ReplaceChar(rptr, '_', ' ');
						cache.findRptrData(rptr, gate, addr);
						if (addr.empty())
							break;
						CSockAddress to;
						if (addr.npos == rptr.find(':'))
							to.Initialize(AF_INET, (unsigned short)g2_external.port, addr.c_str());
						else
							to.Initialize(AF_INET6, (unsigned short)g2_ipv6_external.port, addr.c_str());
						sendto(g2_sock[i], "PONG", 4, 0, to.GetPointer(), to.GetSize());
						if (LOG_QSO)
							printf("Sent 'PONG' to %s\n", addr.c_str());
					}
				}
				break;
			default:
				break;
			}	// switch (type)
		}	// while (keep_running)
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}
	printf("GetIRCDataThread[%i] exiting...\n", i);
	return;
}

/* return codes: 0=OK(found it), 1=TRY AGAIN, 2=FAILED(bad data) */
int CQnetGateway::get_yrcall_rptr_from_cache(const std::string &call, std::string &rptr, std::string &gate, std::string &addr, char RoU)
{
	switch (RoU) {
		case 'U':
			cache.findUserData(call, rptr, gate, addr);
			if (rptr.empty()) {
				printf("Could not find last heard repeater for user '%s'\n", call.c_str());
				return 1;
			}
			break;
		case 'R':
			rptr.assign(call);
			cache.findRptrData(call, gate, addr);
			break;
		default:
			fprintf(stderr, "ERROR: Invalid Rou of '%c'\n", RoU);
			return 2;
	}
	std::string temp;

	if (rptr.at(7) == 'G') {
		fprintf(stderr, "ERROR: Invalid module %c\n", rptr.at(7));
		return 2;
	}
	if (addr.empty()) {
		printf("Couldn't find IP address for %s\n", ('R' == RoU) ? "repeater" : "user");
		return 1;
	}
	return 0;
}

int CQnetGateway::get_yrcall_rptr(const std::string &call, std::string &rptr, std::string &gate, std::string &addr, char RoU)
// returns 0 if unsuccessful, otherwise returns ii index plus one
{
	int i = 0;
	int rc = get_yrcall_rptr_from_cache(call, rptr, gate, addr, RoU);
	if (0 == rc) {
		//printf("get_yrcall_rptr_from_cache: call='%s' rptr='%s' gate='%s', addr='%s' RoU=%c\n", call.c_str(), rptr.c_str(), gate.c_str(), addr.c_str(), RoU);
		if ((addr.npos == addr.find(':')) && ii[1])
			i = 1;
		return i + 1;
	} else if (2 == rc)
		return 0;

	/* at this point, the data is not in cache */
	int cstate = ii[i]->getConnectionState();
	if (cstate < 6) {
		return 0;
	} else {
		// we can try a find
		if (RoU == 'U') {
			printf("User [%s] not in local cache, try again\n", call.c_str());
			/*** YRCALL=KJ4NHFBL ***/
			if (((call.at(6) == 'A') || (call.at(6) == 'B') || (call.at(6) == 'C')) && (call.at(7) == 'L'))
				printf("If this was a gateway link request, that is ok\n");
			if (!ii[i]->findUser(call))
				printf("findUser(%s): Network error\n", call.c_str());
		} else if (RoU == 'R') {
			printf("Repeater [%s] not in local cache, try again\n", call.c_str());
		}
		return 0;
	}
	return i + 1;
}

bool CQnetGateway::Flag_is_ok(unsigned char flag)
{
	//      normal          break          emr          emr+break
	return 0x00U==flag || 0x08U==flag || 0x20U==flag || 0x28U==flag;
}

void CQnetGateway::ProcessTimeouts()
{
	for (int i=0; i<3; i++) {
		time_t t_now;
		/* echotest recording timed out? */
		if (recd[i].last_time != 0) {
			time(&t_now);
			if ((t_now - recd[i].last_time) > TIMING_TIMEOUT_ECHO) {
				printf("Inactivity on echotest recording module %c, removing stream id=%04x\n", 'A'+i, ntohs(recd[i].streamid));

				recd[i].streamid = 0;
				recd[i].last_time = 0;
				close(recd[i].fd);
				recd[i].fd = -1;
				// printf("Closed echotest audio file:[%s]\n", recd[i].file);

				/* START: echotest thread setup */
				try {
					std::async(std::launch::async, &CQnetGateway::PlayFileThread, this, std::ref(recd[i]));
				} catch (const std::exception &e) {
					printf("Failed to start echotest thread. Exception: %s\n", e.what());
					// when the echotest thread runs, it deletes the file,
					// Because the echotest thread did NOT start, we delete the file here
					unlink(recd[i].file);
				}
				/* END: echotest thread setup */
			}
		}

		/* voicemail recording timed out? */
		if (vm[i].last_time != 0) {
			time(&t_now);
			if ((t_now - vm[i].last_time) > TIMING_TIMEOUT_VOICEMAIL) {
				printf("Inactivity on voicemail recording module %c, removing stream id=%04x\n", 'A'+i, ntohs(vm[i].streamid));

				vm[i].streamid = 0;
				vm[i].last_time = 0;
				close(vm[i].fd);
				vm[i].fd = -1;
				// printf("Closed voicemail audio file:[%s]\n", vm[i].file);
			}
		}

		// any stream going to local repeater timed out?
		if (toRptr[i].last_time != 0) {
			time(&t_now);
			//   The stream can be from a cross-band, or from a remote system,
			//   so we could use either FROM_LOCAL_RPTR_TIMEOUT or FROM_REMOTE_G2_TIMEOUT
			//   but FROM_REMOTE_G2_TIMEOUT makes more sense, probably is a bigger number
			if ((t_now - toRptr[i].last_time) > TIMING_TIMEOUT_REMOTE_G2) {
				printf("Inactivity to local rptr module %c, removing stream id %04x\n", 'A'+i, ntohs(toRptr[i].streamid));

				// Send end_of_audio to local repeater.
				// Let the repeater re-initialize
				end_of_audio.streamid = toRptr[i].streamid;
				end_of_audio.ctrl = toRptr[i].sequence | 0x40;

				for (int j=0; j<2; j++)
					Gate2Modem[i].Write(end_of_audio.title, 27);


				toRptr[i].streamid = 0;
				toRptr[i].addr.ClearAddress();
				toRptr[i].last_time = 0;
			}
		}

		/* any stream coming from local repeater timed out ? */
		if (band_txt[i].last_time != 0) {
			time(&t_now);
			if ((t_now - band_txt[i].last_time) > TIMING_TIMEOUT_LOCAL_RPTR) {
				/* This local stream never went to a remote system, so trace the timeout */
				if (to_remote_g2[i].toDstar.AddressIsZero())
					printf("Inactivity from local rptr module %c, removing stream id %04x\n", 'A'+i, ntohs(band_txt[i].streamID));

				band_txt[i].streamID = 0;
				band_txt[i].flags[0] = band_txt[i].flags[1] = band_txt[i].flags[2] = 0x0;
				band_txt[i].lh_mycall[0] = '\0';
				band_txt[i].lh_sfx[0] = '\0';
				band_txt[i].lh_yrcall[0] = '\0';
				band_txt[i].lh_rpt1[0] = '\0';
				band_txt[i].lh_rpt2[0] = '\0';

				band_txt[i].last_time = 0;

				band_txt[i].txt[0] = '\0';
				band_txt[i].txt_cnt = 0;

				band_txt[i].dest_rptr[0] = '\0';

				band_txt[i].num_dv_frames = 0;
				band_txt[i].num_dv_silent_frames = 0;
				band_txt[i].num_bit_errors = 0;
			}
		}

		/* any stream from local repeater to a remote gateway timed out ? */
		if (! to_remote_g2[i].toDstar.AddressIsZero()) {
			time(&t_now);
			if ((t_now - to_remote_g2[i].last_time) > TIMING_TIMEOUT_LOCAL_RPTR) {
				printf("Inactivity from local rptr mod %c, removing stream id %04x\n", 'A'+i, ntohs(to_remote_g2[i].streamid));

				to_remote_g2[i].toDstar.Initialize(AF_UNSPEC);
				to_remote_g2[i].streamid = 0;
				to_remote_g2[i].last_time = 0;
			}
		}
	}
}

// new_group is true if we are processing the first voice packet of a 2-voice packet pair. The high order nibble of the first byte of
// this first packet specifed the type of slow data that is being sent.
// the to_print is an integer that counts down how many 2-voice-frame pairs remain to be processed.
// ABC_grp means that we are processing a 20-character message.
// C_seen means that we are processing the last 2-voice-frame packet on a 20 character message.
void CQnetGateway::ProcessSlowData(unsigned char *data, unsigned short sid)
{
	/* extract 20-byte RADIO ID */
	if ((data[0] != 0x55) || (data[1] != 0x2d) || (data[2] != 0x16)) {

		// first, unscramble
		unsigned char c1 = data[0] ^ 0x70u;
		unsigned char c2 = data[1] ^ 0x4fu;
		unsigned char c3 = data[2] ^ 0x93u;

		for (int i=0; i<3; i++) {
			if (band_txt[i].streamID == sid) {
				if (new_group[i]) {
					header_type = c1 & 0xf0;

					//                 header                   squelch
					if ((header_type == 0x50) || (header_type == 0xc0)) {
						new_group[i] = false;
						to_print[i] = 0;
						ABC_grp[i] = false;
					}
					else if (header_type == 0x30) { /* GPS or GPS id or APRS */
						new_group[i] = false;
						to_print[i] = c1 & 0x0f;
						ABC_grp[i] = false;
						if (to_print[i] > 5)
							to_print[i] = 5;
						else if (to_print[i] < 1)
							to_print[i] = 1;

						if ((to_print[i] > 1) && (to_print[i] <= 5)) {
							/* something went wrong? all bets are off */
							if (band_txt[i].temp_line_cnt > 200) {
								printf("Reached the limit in the OLD gps mode\n");
								band_txt[i].temp_line[0] = '\0';
								band_txt[i].temp_line_cnt = 0;
							}

							/* fresh GPS string, re-initialize */
							if ((to_print[i] == 5) && (c2 == '$')) {
								band_txt[i].temp_line[0] = '\0';
								band_txt[i].temp_line_cnt = 0;
							}

							/* do not copy CR, NL */
							if ((c2 != '\r') && (c2 != '\n')) {
								band_txt[i].temp_line[band_txt[i].temp_line_cnt] = c2;
								band_txt[i].temp_line_cnt++;
							}
							if ((c3 != '\r') && (c3 != '\n')) {
								band_txt[i].temp_line[band_txt[i].temp_line_cnt] = c3;
								band_txt[i].temp_line_cnt++;
							}

							if ((c2 == '\r') || (c3 == '\r')) {
								if (memcmp(band_txt[i].temp_line, "$GPRMC", 6) == 0) {
									memcpy(band_txt[i].gprmc, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
									band_txt[i].gprmc[band_txt[i].temp_line_cnt] = '\0';
								} else if (band_txt[i].temp_line[0] != '$') {
									memcpy(band_txt[i].gpid, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
									band_txt[i].gpid[band_txt[i].temp_line_cnt] = '\0';
									if (APRS_ENABLE && !band_txt[i].is_gps_sent)
										gps_send(i);
								}
								band_txt[i].temp_line[0] = '\0';
								band_txt[i].temp_line_cnt = 0;
							} else if ((c2 == '\n') || (c3 == '\n')) {
								band_txt[i].temp_line[0] = '\0';
								band_txt[i].temp_line_cnt = 0;
							}
							to_print[i] -= 2;
						} else {
							/* something went wrong? all bets are off */
							if (band_txt[i].temp_line_cnt > 200) {
								printf("Reached the limit in the OLD gps mode\n");
								band_txt[i].temp_line[0] = '\0';
								band_txt[i].temp_line_cnt = 0;
							}

							/* do not copy CR, NL */
							if ((c2 != '\r') && (c2 != '\n')) {
								band_txt[i].temp_line[band_txt[i].temp_line_cnt] = c2;
								band_txt[i].temp_line_cnt++;
							}

							if (c2 == '\r') {
								if (memcmp(band_txt[i].temp_line, "$GPRMC", 6) == 0) {
									memcpy(band_txt[i].gprmc, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
									band_txt[i].gprmc[band_txt[i].temp_line_cnt] = '\0';
								} else if (band_txt[i].temp_line[0] != '$') {
									memcpy(band_txt[i].gpid, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
									band_txt[i].gpid[band_txt[i].temp_line_cnt] = '\0';
									if (APRS_ENABLE && !band_txt[i].is_gps_sent)
										gps_send(i);
								}
								band_txt[i].temp_line[0] = '\0';
								band_txt[i].temp_line_cnt = 0;
							} else if (c2 == '\n') {
								band_txt[i].temp_line[0] = '\0';
								band_txt[i].temp_line_cnt = 0;
							}
							to_print[i] --;
						}
					}
					else if (header_type == 0x40) { /* ABC text */
						new_group[i] = false;
						to_print[i] = 3;
						ABC_grp[i] = true;
						C_seen[i] = ((c1 & 0x0f) == 0x03) ? true : false;

						band_txt[i].txt[band_txt[i].txt_cnt] = c2;
						band_txt[i].txt_cnt++;

						band_txt[i].txt[band_txt[i].txt_cnt] = c3;
						band_txt[i].txt_cnt++;

						/* We should NOT see any more text,
						   if we already processed text,
						   so blank out the codes. */
						if (band_txt[i].sent_key_on_msg) {
							data[0] = 0x70;
							data[1] = 0x4f;
							data[2] = 0x93;
						}

						if (band_txt[i].txt_cnt >= 20) {
							band_txt[i].txt[band_txt[i].txt_cnt] = '\0';
							band_txt[i].txt_cnt = 0;
						}
					}
					else {	// header type is not header, squelch, gps or message
						new_group[i] = false;
						to_print[i] = 0;
						ABC_grp[i] = false;
					}
				}
				else { // not a new_group, this is the second of a two-voice-frame pair
					if (! band_txt[i].sent_key_on_msg && vPacketCount[i] > 100) {
						// 100 voice packets received and still no 20-char message!
						/*** if YRCALL is CQCQCQ, set dest_rptr ***/
						band_txt[i].txt[0] = '\0';
						if (memcmp(band_txt[i].lh_yrcall, "CQCQCQ", 6) == 0) {
							set_dest_rptr(i, band_txt[i].dest_rptr);
							if (memcmp(band_txt[i].dest_rptr, "REF", 3) == 0)
								band_txt[i].dest_rptr[0] = '\0';
						}

						int x = FindIndex(i);
						if (x >= 0)
							ii[x]->sendHeardWithTXMsg(band_txt[i].lh_mycall, band_txt[i].lh_sfx, (strstr(band_txt[i].lh_yrcall,"REF") == NULL)?band_txt[i].lh_yrcall:"CQCQCQ  ", band_txt[i].lh_rpt1, band_txt[i].lh_rpt2, band_txt[i].flags[0], band_txt[i].flags[1], band_txt[i].flags[2], band_txt[i].dest_rptr, band_txt[i].txt);
						band_txt[i].sent_key_on_msg = true;
					}
					if (to_print[i] == 3) {
						if (ABC_grp[i]) {
							band_txt[i].txt[band_txt[i].txt_cnt] = c1;
							band_txt[i].txt_cnt++;

							band_txt[i].txt[band_txt[i].txt_cnt] = c2;
							band_txt[i].txt_cnt++;

							band_txt[i].txt[band_txt[i].txt_cnt] = c3;
							band_txt[i].txt_cnt++;

							/* We should NOT see any more text,
							   if we already processed text,
							   so blank out the codes. */
							if (band_txt[i].sent_key_on_msg) {
								data[0] = 0x70;
								data[1] = 0x4f;
								data[2] = 0x93;
							}

							if ((band_txt[i].txt_cnt >= 20) || C_seen[i]) {
								band_txt[i].txt[band_txt[i].txt_cnt] = '\0';
								if ( ! band_txt[i].sent_key_on_msg) {
									/*** if YRCALL is CQCQCQ, set dest_rptr ***/
									if (memcmp(band_txt[i].lh_yrcall, "CQCQCQ", 6) == 0) {
										set_dest_rptr(i, band_txt[i].dest_rptr);
										if (memcmp(band_txt[i].dest_rptr, "REF", 3) == 0)
											band_txt[i].dest_rptr[0] = '\0';
									}
									// we have the 20-character message, send it to the server...
                                    int x = FindIndex(i);
									if (x >= 0)
										ii[x]->sendHeardWithTXMsg(band_txt[i].lh_mycall, band_txt[i].lh_sfx, (strstr(band_txt[i].lh_yrcall,"REF") == NULL)?band_txt[i].lh_yrcall:"CQCQCQ  ", band_txt[i].lh_rpt1, band_txt[i].lh_rpt2, band_txt[i].flags[0], band_txt[i].flags[1], band_txt[i].flags[2], band_txt[i].dest_rptr, band_txt[i].txt);
									band_txt[i].sent_key_on_msg = true;
								}
								band_txt[i].txt_cnt = 0;
							}
							if (C_seen[i])
								C_seen[i] = false;
						} else {
							/* something went wrong? all bets are off */
							if (band_txt[i].temp_line_cnt > 200) {
								printf("Reached the limit in the OLD gps mode\n");
								band_txt[i].temp_line[0] = '\0';
								band_txt[i].temp_line_cnt = 0;
							}

							/* do not copy carrige return or newline */
							if ((c1 != '\r') && (c1 != '\n')) {
								band_txt[i].temp_line[band_txt[i].temp_line_cnt] = c1;
								band_txt[i].temp_line_cnt++;
							}
							if ((c2 != '\r') && (c2 != '\n')) {
								band_txt[i].temp_line[band_txt[i].temp_line_cnt] = c2;
								band_txt[i].temp_line_cnt++;
							}
							if ((c3 != '\r') && (c3 != '\n')) {
								band_txt[i].temp_line[band_txt[i].temp_line_cnt] = c3;
								band_txt[i].temp_line_cnt++;
							}

							if ( (c1 == '\r') || (c2 == '\r') || (c3 == '\r') ) {
								if (memcmp(band_txt[i].temp_line, "$GPRMC", 6) == 0) {
									memcpy(band_txt[i].gprmc, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
									band_txt[i].gprmc[band_txt[i].temp_line_cnt] = '\0';
								} else if (band_txt[i].temp_line[0] != '$') {
									memcpy(band_txt[i].gpid, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
									band_txt[i].gpid[band_txt[i].temp_line_cnt] = '\0';
									if (APRS_ENABLE && !band_txt[i].is_gps_sent)
										gps_send(i);
								}
								band_txt[i].temp_line[0] = '\0';
								band_txt[i].temp_line_cnt = 0;
							}
							else if ((c1 == '\n') || (c2 == '\n') ||(c3 == '\n')) {
								band_txt[i].temp_line[0] = '\0';
								band_txt[i].temp_line_cnt = 0;
							}
						}
					} else if (to_print[i] == 2) {
						/* something went wrong? all bets are off */
						if (band_txt[i].temp_line_cnt > 200) {
							printf("Reached the limit in the OLD gps mode\n");
							band_txt[i].temp_line[0] = '\0';
							band_txt[i].temp_line_cnt = 0;
						}

						/* do not copy CR, NL */
						if ((c1 != '\r') && (c1 != '\n')) {
							band_txt[i].temp_line[band_txt[i].temp_line_cnt] = c1;
							band_txt[i].temp_line_cnt++;
						}
						if ((c2 != '\r') && (c2 != '\n')) {
							band_txt[i].temp_line[band_txt[i].temp_line_cnt] = c2;
							band_txt[i].temp_line_cnt++;
						}

						if ((c1 == '\r') || (c2 == '\r')) {
							if (memcmp(band_txt[i].temp_line, "$GPRMC", 6) == 0) {
								memcpy(band_txt[i].gprmc, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
								band_txt[i].gprmc[band_txt[i].temp_line_cnt] = '\0';
							} else if (band_txt[i].temp_line[0] != '$') {
								memcpy(band_txt[i].gpid, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
								band_txt[i].gpid[band_txt[i].temp_line_cnt] = '\0';
								if (APRS_ENABLE && !band_txt[i].is_gps_sent)
									gps_send(i);
							}
							band_txt[i].temp_line[0] = '\0';
							band_txt[i].temp_line_cnt = 0;
						} else if ((c1 == '\n') || (c2  == '\n')) {
							band_txt[i].temp_line[0] = '\0';
							band_txt[i].temp_line_cnt = 0;
						}
					} else if (to_print[i] == 1) {
						/* something went wrong? all bets are off */
						if (band_txt[i].temp_line_cnt > 200) {
							printf("Reached the limit in the OLD gps mode\n");
							band_txt[i].temp_line[0] = '\0';
							band_txt[i].temp_line_cnt = 0;
						}

						/* do not copy CR, NL */
						if ((c1 != '\r') && (c1 != '\n')) {
							band_txt[i].temp_line[band_txt[i].temp_line_cnt] = c1;
							band_txt[i].temp_line_cnt++;
						}

						if (c1 == '\r') {
							if (memcmp(band_txt[i].temp_line, "$GPRMC", 6) == 0) {
								memcpy(band_txt[i].gprmc, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
								band_txt[i].gprmc[band_txt[i].temp_line_cnt] = '\0';
							} else if (band_txt[i].temp_line[0] != '$') {
								memcpy(band_txt[i].gpid, band_txt[i].temp_line, band_txt[i].temp_line_cnt);
								band_txt[i].gpid[band_txt[i].temp_line_cnt] = '\0';
								if (APRS_ENABLE && !band_txt[i].is_gps_sent)
									gps_send(i);
							}
							band_txt[i].temp_line[0] = '\0';
							band_txt[i].temp_line_cnt = 0;
						} else if (c1 == '\n') {
							band_txt[i].temp_line[0] = '\0';
							band_txt[i].temp_line_cnt = 0;
						}
					}
					new_group[i] = true;
					to_print[i] = 0;
					ABC_grp[i] = false;
				}
				break;
			}
		}
	}
}

void CQnetGateway::ProcessG2(const ssize_t g2buflen, const SDSVT &g2buf, const int source_sock)
// source_sock is the socket number of the incoming data, or -1 if it's a unix socket
{
	static unsigned char lastctrl = 20U;
	static std::string superframe[3];
	if ( (g2buflen==56 || g2buflen==27) && 0==memcmp(g2buf.title, "DSVT", 4) && (g2buf.config==0x10 || g2buf.config==0x20) && g2buf.id==0x20) {
		if (g2buflen == 56) {
			// Find out the local repeater module IP/port to send the data to
			int i = g2buf.hdr.rpt1[7] - 'A';
			/* valid repeater module? */
			if (i>=0 && i<3 && Rptr.mod[i].defined) {
				// toRptr[i] is active if a remote system is talking to it or
				// toRptr[i] is receiving data from a cross-band
				if (0==toRptr[i].last_time && 0==band_txt[i].last_time && (Flag_is_ok(g2buf.hdr.flag[0]) || 0x01U==g2buf.hdr.flag[0] || 0x40U==g2buf.hdr.flag[0])) {
					superframe[i].clear();
					if (LOG_QSO) {
						printf("id=%04x flags=%02x:%02x:%02x ur=%.8s r1=%.8s r2=%.8s my=%.8s/%.4s ", ntohs(g2buf.streamid), g2buf.hdr.flag[0], g2buf.hdr.flag[1], g2buf.hdr.flag[2], g2buf.hdr.urcall, g2buf.hdr.rpt1, g2buf.hdr.rpt2, g2buf.hdr.mycall, g2buf.hdr.sfx);
						if (source_sock >= 0)
							printf("IP=[%s]:%u\n", fromDstar.GetAddress(), fromDstar.GetPort());
						else
							printf("UnixSock=%s\n", link2gate.c_str());
					}

					std::string  mycall((const char *)g2buf.hdr.mycall, 8);
					if (DASH_SHOW_LH && memcmp(g2buf.hdr.sfx, "RPTR", 4) && std::regex_match(mycall.c_str(), preg)) {
						std::string     sfx((const char *)g2buf.hdr.sfx,    4);
						std::string  urcall((const char *)g2buf.hdr.urcall, 8);
						rtrim(mycall);
						rtrim(sfx);
						qnDB.Update(mycall.c_str(), sfx.c_str(), urcall.c_str());
					}

					Gate2Modem[i].Write(g2buf.title, 56);
					lastctrl = 20U;

					/* save the header */
					if (source_sock < 0) {
						//if (AF_INET == af_family) {
							char address[16];
							snprintf(address, 16, "%d.0.0.0", i);
							fromDstar.Initialize(AF_INET, 40000U, address);
						//} else {
						//	char address[8];
						//	snprintf(address, 8, "%d::", i);
						//	fromDstar.Initialize(AF_INET6, 40000U, address);
						//}
					}
					memcpy(toRptr[i].saved_hdr.title, g2buf.title, 56);
					toRptr[i].saved_addr = fromDstar;

					/* This is the active streamid */
					toRptr[i].streamid = g2buf.streamid;
					toRptr[i].addr = fromDstar;;

					/* time it, in case stream times out */
					time(&toRptr[i].last_time);

					toRptr[i].sequence = g2buf.ctrl;
				}
			}
		} else {	// g2buflen == 27
			/* find out which repeater module to send the data to */
			int i;
			for (i=0; i<3; i++) {
				if (Rptr.mod[i].defined) {
					/* streamid match ? */
					bool match = (toRptr[i].streamid == g2buf.streamid);
					if (source_sock >= 0)
						match = match && (toRptr[i].addr == fromDstar);
					if (match) {
						if (LOG_DEBUG) {
							const unsigned int ctrl = g2buf.ctrl & 0x3FU;
							if (VoicePacketIsSync(g2buf.vasd.text)) {
								if (superframe[i].size() > 65U) {
									printf("Frame[%c]: %s\n", 'A'+i, superframe[i].c_str());
									superframe[i].clear();
								}
								const char *ch = "#abcdefghijklmnopqrstuvwxyz";
								superframe[i].append(1, (ctrl<27U) ? ch[ctrl] : '%' );
							} else {
								const char *ch = "!ABCDEFGHIJKLMNOPQRSTUVWXYZ";
								superframe[i].append(1, (ctrl<27U) ? ch[ctrl] : '*' );
							}
						}

						int diff = int(0x3FU & g2buf.ctrl) - int(lastctrl);
						if (diff < 0)
							diff += 21;
						if (diff > 1 && diff < 6) {	// fill up to 5 missing voice frames
							if (LOG_DEBUG)
								fprintf(stderr, "Warning: inserting %d missing voice frame(s)\n", diff - 1);
							SDSVT dsvt;
							memcpy(dsvt.title, g2buf.title, 14U);	// everything but the ctrl and voice data
							while (--diff > 0) {
								lastctrl = (lastctrl + 1U) % 21U;
								dsvt.ctrl = lastctrl;
								if (dsvt.ctrl) {
									const unsigned char silence[12] = { 0x9EU,0x8DU,0x32U,0x88U,0x26U,0x1AU,0x3FU,0x61U,0xE8U,0x70U,0x4FU,0x93U };
									memcpy(dsvt.vasd.voice, silence, 12U);
								} else {
									const unsigned char sync[12] = { 0x9EU,0x8DU,0x32U,0x88U,0x26U,0x1AU,0x3FU,0x61U,0xE8U,0x55U,0x2DU,0x16U };
									memcpy(dsvt.vasd.voice, sync, 12U);
								}
								Gate2Modem[i].Write(dsvt.title, 27);
							}
						}

						if (((lastctrl + 1U) % 21U == (0x3FU & g2buf.ctrl)) || (0x40U & g2buf.ctrl)) {
							// no matter what, we will send this on if it is the closing frame
							lastctrl = (0x3FU & g2buf.ctrl);
							Gate2Modem[i].Write(g2buf.title, 27);
						} else {
							if (LOG_DEBUG)
								fprintf(stderr, "Warning: Ignoring packet because its ctrl=0x%02xU and lastctrl=0x%02xU\n", g2buf.ctrl, lastctrl);
						}

						/* timeit */
						time(&toRptr[i].last_time);

						toRptr[i].sequence = g2buf.ctrl;

						/* End of stream ? */
						if (g2buf.ctrl & 0x40U) {
							/* clear the saved header */
							memset(toRptr[i].saved_hdr.title, 0U, 56U);
							toRptr[i].saved_addr.ClearAddress();

							toRptr[i].last_time = 0;
							toRptr[i].streamid = 0;
							toRptr[i].addr.ClearAddress();
							if (LOG_DEBUG && superframe[i].size()) {
								printf("Final[%c]: %s\n", 'A'+i, superframe[i].c_str());
								superframe[i].clear();
							}
							if (LOG_QSO)
								printf("id=%04x END\n", ntohs(g2buf.streamid));
						}
						break;	// we're done
					}
				}
			}

			/* no match ? */
			if ((i == 3) && GATEWAY_HEADER_REGEN) {
				/* check if this a continuation of audio that timed out */

				if (g2buf.ctrl & 0x40)
					;  /* we do not care about end-of-QSO */
				else {
					/* for which repeater this stream has timed out ?  */
					for (i=0; i<3; i++) {
						if (! Rptr.mod[i].defined)
							continue;
						/* match saved stream ? */
						bool match = (toRptr[i].saved_hdr.streamid == g2buf.streamid);
						if (source_sock >= 0)
							match = match && (toRptr[i].saved_addr == fromDstar);
						if (match) {
							/* repeater module is inactive ?  */
							if (toRptr[i].last_time==0 && band_txt[i].last_time==0) {
								printf("Re-generating header for streamID=%04x\n", ntohs(g2buf.streamid));

								/* re-generate/send the header */
								Gate2Modem[i].Write(toRptr[i].saved_hdr.title, 56);

								/* send this audio packet to repeater */
								Gate2Modem[i].Write(g2buf.title, 27);

								/* make sure that any more audio arriving will be accepted */
								toRptr[i].streamid = g2buf.streamid;
								toRptr[i].addr = fromDstar;

								/* time it, in case stream times out */
								time(&toRptr[i].last_time);

								toRptr[i].sequence = g2buf.ctrl;

							}
							break;
						}
					}
				}
			}
		}
	}
}

void CQnetGateway::ProcessModem()
{
	char tempfile[FILENAME_MAX];

	SDSVT dsvt;

	int recvlen = Modem2Gate.Read(dsvt.title, 56);

	if (0 == memcmp(dsvt.title, "DSVT", 4)) {
		if ( (recvlen==56 || recvlen==27) && dsvt.id==0x20U && (dsvt.config==0x10U || dsvt.config==0x20U) ) {
			if (recvlen == 56) {
				if (LOG_QSO)
					printf("id=%04x start RPTR flag0=%02x ur=%.8s r1=%.8s r2=%.8s my=%.8s/%.4s\n", ntohs(dsvt.streamid), dsvt.hdr.flag[0],  dsvt.hdr.urcall, dsvt.hdr.rpt1, dsvt.hdr.rpt2, dsvt.hdr.mycall, dsvt.hdr.sfx);
				if (0==memcmp(dsvt.hdr.rpt1, "DIRECT  ", 8) && 0==memcmp(dsvt.hdr.rpt2, "DIRECT  ", 8)) {	// DIRECT mode???
					memcpy(dsvt.hdr.rpt1, OWNER.c_str(), 7);
					switch (dsvt.flagb[2]) {
						case 0x01U:
							dsvt.hdr.rpt1[7] = 'B';
							break;
						case 0x02U:
							dsvt.hdr.rpt1[7] = 'C';
							break;
						default:
							dsvt.hdr.rpt1[7] = 'A';
							break;
					}
					memcpy(dsvt.hdr.rpt2, OWNER.c_str(), 7);
					dsvt.hdr.rpt2[7] = 'G';
					calcPFCS(dsvt.title, 56);
					if (LOG_QSO)
						printf("Resetting: r1=%.8s r2=%.8s\n", dsvt.hdr.rpt1, dsvt.hdr.rpt2);
				}

				if (0==memcmp(dsvt.hdr.rpt1, OWNER.c_str(), 7) && Flag_is_ok(dsvt.hdr.flag[0])) {

					int i = dsvt.hdr.rpt1[7] - 'A';

					if (i>=0  && i<3) {
                        vPacketCount[i] = 0;
                        Index[i] = -1;
						if (LOG_DTMF)
							printf("resetting dtmf[%d] (got a header)\n", i);
						dtmf_last_frame[i] = 0;
						dtmf_counter[i] = 0;
						memset(dtmf_buf[i], 0, sizeof(dtmf_buf[i]));
						dtmf_buf_count[i] = 0;

						/* Initialize the LAST HEARD data for the band */

						band_txt[i].streamID = dsvt.streamid;

						memcpy(band_txt[i].flags, dsvt.hdr.flag, 3);

						memcpy(band_txt[i].lh_mycall, dsvt.hdr.mycall, 8);
						band_txt[i].lh_mycall[8] = '\0';

						memcpy(band_txt[i].lh_sfx, dsvt.hdr.sfx, 4);
						band_txt[i].lh_sfx[4] = '\0';

						memcpy(band_txt[i].lh_yrcall, dsvt.hdr.urcall, 8);
						band_txt[i].lh_yrcall[8] = '\0';

						memcpy(band_txt[i].lh_rpt1, dsvt.hdr.rpt1, 8);
						band_txt[i].lh_rpt1[8] = '\0';

						memcpy(band_txt[i].lh_rpt2, dsvt.hdr.rpt2, 8);
						band_txt[i].lh_rpt2[8] = '\0';

						time(&band_txt[i].last_time);

						band_txt[i].txt[0] = '\0';
						band_txt[i].txt_cnt = 0;
						band_txt[i].sent_key_on_msg = false;

						band_txt[i].dest_rptr[0] = '\0';

						/* try to process GPS mode: GPRMC and ID */
						band_txt[i].temp_line[0] = '\0';
						band_txt[i].temp_line_cnt = 0;
						band_txt[i].gprmc[0] = '\0';
						band_txt[i].gpid[0] = '\0';
						band_txt[i].is_gps_sent = false;
						// band_txt[i].gps_last_time = 0; DO NOT reset it

						new_group[i] = true;
						to_print[i] = 0;
						ABC_grp[i] = false;

						band_txt[i].num_dv_frames = 0;
						band_txt[i].num_dv_silent_frames = 0;
						band_txt[i].num_bit_errors = 0;

						/* select the band for aprs processing, and lock on the stream ID */
						if (APRS_ENABLE)
							aprs->SelectBand(i, ntohs(dsvt.streamid));
					}
				}

				/* Is MYCALL valid ? */
				std::string call;
				call.assign((char *)dsvt.hdr.mycall, 8);

				bool mycall_valid = std::regex_match(call.c_str(), preg);

				if (mycall_valid)
					Gate2Link.Write(dsvt.title, recvlen);
				else
					printf("MYCALL [%s] failed IRC expression validation\n", call.c_str());

				if ( mycall_valid &&
					memcmp(dsvt.hdr.urcall, "XLX", 3) &&		// not a reflector
					memcmp(dsvt.hdr.urcall, "XRF", 3) &&
					memcmp(dsvt.hdr.urcall, "REF", 3) &&
					memcmp(dsvt.hdr.urcall, "DCS", 3) &&
					dsvt.hdr.urcall[0]!=' ' && 					// must have something
					memcmp(dsvt.hdr.urcall, "CQCQCQ", 6) ) {	// urcall is NOT CQCQCQ
					std::string user, rptr, gate, addr;
					if ( dsvt.hdr.urcall[0]=='/' &&								// repeater routing!
							0==memcmp(dsvt.hdr.rpt1, OWNER.c_str(), 7) &&	// rpt1 this repeater
							(dsvt.hdr.rpt1[7]>='A' && dsvt.hdr.rpt1[7]<='C') &&	// with a valid module
							0==memcmp(dsvt.hdr.rpt2, OWNER.c_str(), 7) && 	// rpt2 is this repeater
							dsvt.hdr.rpt2[7]=='G' &&						// local Gateway
							Flag_is_ok(dsvt.hdr.flag[0]) )
					{
						if (memcmp(dsvt.hdr.urcall+1, OWNER.c_str(), 6)) {	// the value after the slash is NOT this repeater
							int i = dsvt.hdr.rpt1[7] - 'A';

							if (i>=0 && i<3) {
								/* one radio user on a repeater module at a time */
								if (to_remote_g2[i].toDstar.AddressIsZero()) {
									/* YRCALL=/repeater + mod */
									/* YRCALL=/KJ4NHFB */

									user.assign((char *)dsvt.hdr.urcall, 1, 6);
									user.append(" ");
									user.append(dsvt.hdr.urcall[7], 1);
									if (isspace(user.at(7)))
										user[7] = 'A';

									Index[i] = get_yrcall_rptr(user, rptr, gate, addr, 'R');
									if (Index[i]--) { /* it is a repeater */
										//std::string from = OWNER.substr(0, 7);
										//from.append(1, i+'A');
										//ii[Index[i]]->sendPing(user, from);
										to_remote_g2[i].streamid = dsvt.streamid;
										if (addr.npos == addr.find(':') && af_family[Index[i]] == AF_INET6)
											fprintf(stderr, "ERROR using IRC[%d]: IP returned from cache is IPV4, %s, but family is AF_INET6!\n", Index[i], addr.c_str());
										to_remote_g2[i].toDstar.Initialize(af_family[Index[i]], (uint16_t)((af_family[Index[i]]==AF_INET6) ? g2_ipv6_external.port : g2_external.port), addr.c_str());

										/* set rpt1 */
										memset(dsvt.hdr.rpt1, ' ', 8);
										memcpy(dsvt.hdr.rpt1, rptr.c_str(), 8);
										/* set rpt2 */
										memcpy(dsvt.hdr.rpt2, gate.c_str(), 8);
										/* set yrcall, can NOT let it be slash and repeater + module */
										memcpy(dsvt.hdr.urcall, "CQCQCQ  ", 8);

										/* set PFCS */
										calcPFCS(dsvt.title, 56);

										// The remote repeater has been set, lets fill in the dest_rptr
										// so that later we can send that to the LIVE web site
										memcpy(band_txt[i].dest_rptr, dsvt.hdr.rpt1, 8);
										band_txt[i].dest_rptr[CALL_SIZE] = '\0';

										// send to remote gateway
										for (int j=0; j<5; j++)
											sendto(g2_sock[Index[i]], dsvt.title, 56, 0, to_remote_g2[i].toDstar.GetPointer(), to_remote_g2[i].toDstar.GetSize());

										printf("id=%04x zone route to [%s]:%u ur=%.8s r1=%.8s r2=%.8s my=%.8s/%.4s\n",
										ntohs(dsvt.streamid), to_remote_g2[i].toDstar.GetAddress(), to_remote_g2[i].toDstar.GetPort(),
										dsvt.hdr.urcall, dsvt.hdr.rpt1, dsvt.hdr.rpt2, dsvt.hdr.mycall, dsvt.hdr.sfx);

										time(&(to_remote_g2[i].last_time));
									}
								}
							}
						}
					}
					else if (memcmp(dsvt.hdr.urcall, OWNER.c_str(), 7) &&		// urcall is not this repeater
							0==memcmp(dsvt.hdr.rpt1, OWNER.c_str(), 7) &&		// rpt1 is this repeater
							(dsvt.hdr.rpt1[7]>='A'&& dsvt.hdr.rpt1[7]<='C') &&	// mod is A,B,C
							0==memcmp(dsvt.hdr.rpt2, OWNER.c_str(), 7) &&		// rpt2 is this repeater
							dsvt.hdr.rpt2[7]=='G' &&							// local Gateway
							Flag_is_ok(dsvt.hdr.flag[0])) {

						user.assign((char *)dsvt.hdr.urcall, 8);
						trim(user);
                        int i = dsvt.hdr.rpt1[7] - 'A';
                        if (i>=0 && i<3) {
						    Index[i] = get_yrcall_rptr(user, rptr, gate, addr, 'U');
                            if (Index[i]--) {
                                /* destination is a remote system */
                                if (0 != gate.compare(0, 7, OWNER, 0, 7)) {

									/* one radio user on a repeater module at a time */
									if (to_remote_g2[i].toDstar.AddressIsZero()) {
										if (std::regex_match(user.c_str(), preg)) {
											// do a sendPing, but not to a routing group. URCALL must be a regular callsign
											std::string from((char *)dsvt.hdr.mycall, 8);
											trim(from);
											ii[Index[i]]->sendPing(user, from);	// user and from are in uppercase
										}
										/* set the destination */
										to_remote_g2[i].streamid = dsvt.streamid;
										if (addr.npos == addr.find(':') && af_family[Index[i]] == AF_INET6)
											fprintf(stderr, "ERROR using IRC[%d]: IP returned from cache, %s, is IPV4 but family is AF_INET6!\n", Index[i], addr.c_str());
										to_remote_g2[i].toDstar.Initialize(af_family[Index[i]], (uint16_t)((af_family[Index[i]]==AF_INET6) ? g2_ipv6_external.port : g2_external.port), addr.c_str());

										/* set rpt1 */
										memcpy(dsvt.hdr.rpt1, rptr.c_str(), 8);
										/* set rpt2 */
										memcpy(dsvt.hdr.rpt2, gate.c_str(), 8);
										/* set PFCS */
										calcPFCS(dsvt.title, 56);

										// The remote repeater has been set, lets fill in the dest_rptr
										// so that later we can send that to the LIVE web site
										memcpy(band_txt[i].dest_rptr, dsvt.hdr.rpt1, 8);
										band_txt[i].dest_rptr[CALL_SIZE] = '\0';

										/* send to remote gateway */
										for (int j=0; j<5; j++)
											sendto(g2_sock[Index[i]], dsvt.title, 56, 0, to_remote_g2[i].toDstar.GetPointer(), to_remote_g2[i].toDstar.GetSize());

										printf("Callsign route to [%s]:%u id=%04x my=%.8s/%.4s ur=%.8s rpt1=%.8s rpt2=%.8s\n", to_remote_g2[i].toDstar.GetAddress(), to_remote_g2[i].toDstar.GetPort(), ntohs(dsvt.streamid), dsvt.hdr.mycall, dsvt.hdr.sfx, dsvt.hdr.urcall, dsvt.hdr.rpt1, dsvt.hdr.rpt2);

										time(&(to_remote_g2[i].last_time));
									}
								}
    							else
	    						{
		    						int i = dsvt.hdr.rpt1[7] - 'A';

			    					if (i>=0 && i<3) {
				    					/* the user we are trying to contact is on our gateway */
					    				/* make sure they are on a different module */
						    			if (rptr.at(7) != dsvt.hdr.rpt1[7]) {
							    			/*
								    		   The remote repeater has been set, lets fill in the dest_rptr
									    	   so that later we can send that to the LIVE web site
										    */
    										memcpy(band_txt[i].dest_rptr, dsvt.hdr.rpt2, 8);
	    									band_txt[i].dest_rptr[7] = rptr.at(7);
		    								band_txt[i].dest_rptr[8] = '\0';

    										i = rptr.at(7) - 'A';

	    									/* valid destination repeater module? */
		    								if (i>=0 && i<3) {
			    								/*
				    							   toRptr[i] :    receiving from a remote system or cross-band
					    						   band_txt[i] :  local RF is talking.
						    					*/
							    				if ((toRptr[i].last_time == 0) && (band_txt[i].last_time == 0)) {
								    				printf("CALLmode cross-banding from mod %c to %c\n",  dsvt.hdr.rpt1[7], rptr.at(7));

									    			dsvt.hdr.rpt2[7] = rptr.at(7);
										    		dsvt.hdr.rpt1[7] = 'G';
											    	calcPFCS(dsvt.title, 56);

												    Gate2Modem[i].Write(dsvt.title, 56);

    												/* This is the active streamid */
	    											toRptr[i].streamid = dsvt.streamid;

		    										/* time it, in case stream times out */
			    									time(&toRptr[i].last_time);

				    								toRptr[i].sequence = dsvt.ctrl;
                                                }
											}
										}
									}
									else
                                    {
										printf("icom rule: no routing from %.8s to %s%c\n", dsvt.hdr.rpt1, rptr.c_str(), rptr.at(7));
                                    }
								}
							}
						    else
						    {
							    if ('L' != dsvt.hdr.urcall[7]) // as long as this doesn't look like a linking command
								    playNotInCache = true; // we need to wait until user's transmission is over
                            }
						}
					}
				}
				else if (0 == memcmp(dsvt.hdr.urcall, "      C0", 8)) {
					int i = dsvt.hdr.rpt1[7] - 'A';

					if (i>=0 && i<3) {
						/* voicemail file is closed */
						if ((vm[i].fd == -1) && (vm[i].file[0] != '\0')) {
							unlink(vm[i].file);
							printf("removed voicemail file: %s\n", vm[i].file);
							vm[i].file[0] = '\0';
						} else
							printf("No voicemail to clear or still recording\n");
					}
				}
				else if (0 == memcmp(dsvt.hdr.urcall, "      R0", 8)) {
					int i = dsvt.hdr.rpt1[7] - 'A';

					if (i>=0 && i<3) {
						/* voicemail file is closed */
						if ((vm[i].fd == -1) && (vm[i].file[0] != '\0')) {
                            band_txt[i].last_time = 0;
                            band_txt[i].streamID = 0U;  // prevent vm timeout
							snprintf(vm[i].message, 21, "VOICEMAIL ON MOD %c  ", 'A'+i);
							try {
								std::async(std::launch::async, &CQnetGateway::PlayFileThread, this, std::ref(vm[i]));
							} catch (const std::exception &e) {
								printf("Failed to start voicemail playback. Exception: %s\n", e.what());
							}
						} else
							printf("No voicemail to recall or still recording\n");
					}
				}
				else if (0 == memcmp(dsvt.hdr.urcall, "      S0", 8)) {
					int i = dsvt.hdr.rpt1[7] - 'A';

					if (i>=0 && i<3) {
						if (vm[i].fd >= 0)
							printf("Already recording for voicemail on mod %d\n", i);
						else {
							memset(tempfile, '\0', sizeof(tempfile));
							snprintf(tempfile, FILENAME_MAX, "%s/%c_%s", FILE_ECHOTEST.c_str(), dsvt.hdr.rpt1[7], "voicemail.dat2");

							vm[i].fd = open(tempfile, O_CREAT | O_WRONLY | O_TRUNC | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
							if (vm[i].fd < 0)
								printf("Failed to create file %s for voicemail\n", tempfile);
							else {
								strcpy(vm[i].file, tempfile);
								printf("Recording mod %c for voicemail into file:[%s]\n", dsvt.hdr.rpt1[7], vm[i].file);

								time(&vm[i].last_time);
                                vm[i].streamid = dsvt.streamid;
								memcpy(recbuf.title, dsvt.title, 56);
								memset(recbuf.hdr.rpt1, ' ', 8);
								memcpy(recbuf.hdr.rpt1, OWNER.c_str(), OWNER.size());
								recbuf.hdr.rpt1[7] = dsvt.hdr.rpt1[7];
								memset(recbuf.hdr.rpt2, ' ', 8);
								memcpy(recbuf.hdr.rpt2,  OWNER.c_str(), OWNER.size());
								recbuf.hdr.rpt2[7] = 'G';
								memcpy(recbuf.hdr.urcall, "CQCQCQ  ", 8);

								calcPFCS(recbuf.title, 56);

                                write(vm[i].fd, recbuf.title, 56);
							}
						}
					}
				}
				else if (0 == memcmp(dsvt.hdr.urcall, "       E", 8)) {
					int i = dsvt.hdr.rpt1[7] - 'A';

					if (i>=0 && i<3) {
						if (recd[i].fd >= 0)
							printf("Already recording for echotest on mod %d\n", i);
						else {
							memset(tempfile, '\0', sizeof(tempfile));
							snprintf(tempfile, FILENAME_MAX, "%s/%c_%s", FILE_ECHOTEST.c_str(), dsvt.hdr.rpt1[7], "echotest.dat");

							recd[i].fd = open(tempfile, O_CREAT | O_WRONLY | O_EXCL | O_TRUNC | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
							if (recd[i].fd < 0)
								printf("Failed to create file %s for echotest\n", tempfile);
							else {
								strcpy(recd[i].file, tempfile);
								printf("Recording mod %c for echotest into file:[%s]\n", dsvt.hdr.rpt1[7], recd[i].file);
								snprintf(recd[i].message, 21, "ECHO ON MODULE %c    ", 'A' + i);
								time(&recd[i].last_time);
								recd[i].streamid = dsvt.streamid;

								memcpy(recbuf.title, dsvt.title, 56);
								memset(recbuf.hdr.rpt1, ' ', 8);
								memcpy(recbuf.hdr.rpt1, OWNER.c_str(), OWNER.length());
								recbuf.hdr.rpt1[7] = dsvt.hdr.rpt1[7];
								memset(recbuf.hdr.rpt2, ' ', 8);
								memcpy(recbuf.hdr.rpt2,  OWNER.c_str(), OWNER.length());
								recbuf.hdr.rpt2[7] = 'G';
								memcpy(recbuf.hdr.urcall, "CQCQCQ  ", 8);

								calcPFCS(recbuf.title, 56);

                                write (recd[i].fd, recbuf.title, 56);
							}
						}
					}
				/* check for cross-banding */
				}
				else if ( 0==memcmp(dsvt.hdr.urcall, "CQCQCQ", 6) &&		// yrcall is CQCQCQ
							0==memcmp(dsvt.hdr.rpt2, OWNER.c_str(), 7) &&	// rpt1 is this repeater
							0==memcmp(dsvt.hdr.rpt1, OWNER.c_str(), 7) &&	// rpt2 is this repeater
						(dsvt.hdr.rpt1[7]>='A' && dsvt.hdr.rpt1[7]<='C') &&	// mod of rpt1 is A,B,C
						(dsvt.hdr.rpt2[7]>='A' && dsvt.hdr.rpt2[7]<='C') &&	// !!! usually G on rpt2, but we see A,B,C with
						dsvt.hdr.rpt2[7]!=dsvt.hdr.rpt1[7] ) {				// cross-banding? make sure NOT the same
					int i = dsvt.hdr.rpt1[7] - 'A';

					if (i>=0 && i<3) {
						// The remote repeater has been set, lets fill in the dest_rptr
						// so that later we can send that to the LIVE web site
						memcpy(band_txt[i].dest_rptr, dsvt.hdr.rpt2, 8);
						band_txt[i].dest_rptr[8] = '\0';
					}

					i = dsvt.hdr.rpt2[7] - 'A';

					// valid destination repeater module?
					if (i>=0 && i<3) {
						// toRptr[i] :    receiving from a remote system or cross-band
						// band_txt[i] :  local RF is talking.
						if ((toRptr[i].last_time == 0) && (band_txt[i].last_time == 0)) {
							printf("ZONEmode cross-banding from mod %c to %c\n",  dsvt.hdr.rpt1[7], dsvt.hdr.rpt2[7]);

							dsvt.hdr.rpt1[7] = 'G';
							calcPFCS(dsvt.title, 56);

							Gate2Modem[i].Write(dsvt.title, 56);

							/* This is the active streamid */
							toRptr[i].streamid = dsvt.streamid;

							/* time it, in case stream times out */
							time(&toRptr[i].last_time);

							toRptr[i].sequence = dsvt.ctrl;
						}
					}
				}
			}
			else
			{	// recvlen is 29 or 32
				for (int i=0; i<3; i++) {
					if (band_txt[i].streamID == dsvt.streamid) {
						time(&band_txt[i].last_time);

						if (dsvt.ctrl & 0x40) {	// end of voice data
							if (dtmf_buf_count[i] > 0) {
								std::string dtmf_file(FILE_DTMF);
								dtmf_file.push_back('/');
								dtmf_file.push_back('A'+i);
								dtmf_file += "_mod_DTMF_NOTIFY";
								if (LOG_DTMF)
									printf("Saving dtmfs=[%s] into file: [%s]\n", dtmf_buf[i], dtmf_file.c_str());
								FILE *dtmf_fp = fopen(dtmf_file.c_str(), "w");
								if (dtmf_fp) {
									fprintf(dtmf_fp, "%s\n%s", dtmf_buf[i], band_txt[i].lh_mycall);
									fclose(dtmf_fp);
								} else
									printf("Failed to create dtmf file %s\n", dtmf_file.c_str());


								if (LOG_DTMF)
									printf("resetting dtmf[%d] (printed dtmf code %s from %s)\n", i, dtmf_buf[i], band_txt[i].lh_mycall);
								memset(dtmf_buf[i], 0, sizeof(dtmf_buf[i]));
								dtmf_buf_count[i] = 0;
								dtmf_counter[i] = 0;
								dtmf_last_frame[i] = 0;
							}
							if (! band_txt[i].sent_key_on_msg) {
								band_txt[i].txt[0] = '\0';
								if (memcmp(band_txt[i].lh_yrcall, "CQCQCQ", 6) == 0) {
									set_dest_rptr(i, band_txt[i].dest_rptr);
									if (memcmp(band_txt[i].dest_rptr, "REF", 3) == 0)
										band_txt[i].dest_rptr[0] = '\0';
								}
                                int x = FindIndex(i);
								if (x >= 0)
									ii[x]->sendHeardWithTXMsg(band_txt[i].lh_mycall, band_txt[i].lh_sfx, (strstr(band_txt[i].lh_yrcall,"REF") == NULL)?band_txt[i].lh_yrcall:"CQCQCQ  ", band_txt[i].lh_rpt1, band_txt[i].lh_rpt2, band_txt[i].flags[0], band_txt[i].flags[1], band_txt[i].flags[2], band_txt[i].dest_rptr, band_txt[i].txt);
								band_txt[i].sent_key_on_msg = true;
							}
							// send the "key off" message, this will end up in the openquad.net Last Heard webpage.
                            int index = Index[i];
                            if (index < 0) {
                                if (AF_INET == link_family[i]) {
                                    index = ii[1] ? 1 : 0;
                                } else if (AF_INET6 == link_family[i]) {
                                    index = 0;
                                }
                            }
							if (index >= 0)
								ii[index]->sendHeardWithTXStats(band_txt[i].lh_mycall, band_txt[i].lh_sfx, band_txt[i].lh_yrcall, band_txt[i].lh_rpt1, band_txt[i].lh_rpt2, band_txt[i].flags[0], band_txt[i].flags[1], band_txt[i].flags[2], band_txt[i].num_dv_frames, band_txt[i].num_dv_silent_frames, band_txt[i].num_bit_errors);

							if (playNotInCache) {
								// Not in cache, please try again!
								FILE *fp = fopen(FILE_QNVOICE_FILE.c_str(), "w");
								if (fp) {
									fprintf(fp, "%c_notincache.dat_NOT_IN_CACHE\n", band_txt[i].lh_rpt1[7]);
									fclose(fp);
								}
								playNotInCache = false;
							}

							band_txt[i].streamID = 0;
							band_txt[i].flags[0] = band_txt[i].flags[1] = band_txt[i].flags[2] = 0;
							band_txt[i].lh_mycall[0] = '\0';
							band_txt[i].lh_sfx[0] = '\0';
							band_txt[i].lh_yrcall[0] = '\0';
							band_txt[i].lh_rpt1[0] = '\0';
							band_txt[i].lh_rpt2[0] = '\0';

							band_txt[i].last_time = 0;

							band_txt[i].txt[0] = '\0';
							band_txt[i].txt_cnt = 0;

							band_txt[i].dest_rptr[0] = '\0';

							band_txt[i].num_dv_frames = 0;
							band_txt[i].num_dv_silent_frames = 0;
							band_txt[i].num_bit_errors = 0;
						}
						else
						{	// not the end of the voice stream
							int ber_data[3];
							int ber_errs = decode.Decode(dsvt.vasd.voice, ber_data);
							if (ber_data[0] == 0xf85)
								band_txt[i].num_dv_silent_frames++;
							band_txt[i].num_bit_errors += ber_errs;
							band_txt[i].num_dv_frames++;

							if ((ber_data[0] & 0x0ffc) == 0xfc0) {
								dtmf_digit = (ber_data[0] & 0x03) | ((ber_data[2] & 0x60) >> 3);
								if (dtmf_counter[i] > 0) {
									if (dtmf_last_frame[i] != dtmf_digit)
										dtmf_counter[i] = 0;
								}
								dtmf_last_frame[i] = dtmf_digit;
								dtmf_counter[i]++;

								if ((dtmf_counter[i] == 5) && (dtmf_digit >= 0) && (dtmf_digit <= 15)) {
									if (dtmf_buf_count[i] < MAX_DTMF_BUF) {
										const char *dtmf_chars = "147*2580369#ABCD";
										dtmf_buf[i][ dtmf_buf_count[i] ] = dtmf_chars[dtmf_digit];
										dtmf_buf_count[i]++;
									}
								}
								const unsigned char silence[9] = { 0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8 };
								memcpy(dsvt.vasd.voice, silence, 9);
							} else
								dtmf_counter[i] = 0;
						}
						break;
					}
                    vPacketCount[i]++;
				}
				ProcessSlowData(dsvt.vasd.text,  dsvt.streamid);

				/* send data to qnlink */
				Gate2Link.Write(dsvt.title, 27);

				/* aprs processing */
				if (APRS_ENABLE)
					aprs->ProcessText(ntohs(dsvt.streamid), dsvt.ctrl, dsvt.vasd.voice);

				for (int i=0; i<3; i++) {
					/* find out if data must go to the remote G2 */
					if (to_remote_g2[i].streamid==dsvt.streamid && Index[i]>=0) {
						sendto(g2_sock[Index[i]], dsvt.title, 27, 0, to_remote_g2[i].toDstar.GetPointer(), to_remote_g2[i].toDstar.GetSize());

						time(&(to_remote_g2[i].last_time));

						/* Is this the end-of-stream */
						if (dsvt.ctrl & 0x40) {
							to_remote_g2[i].toDstar.Initialize(AF_UNSPEC);
							to_remote_g2[i].streamid = 0;
							to_remote_g2[i].last_time = 0;
						}
						break;
					}
					else if (recd[i].fd>=0 && recd[i].streamid==dsvt.streamid) {	// Is the data to be recorded for echotest
						time(&recd[i].last_time);

						write(recd[i].fd, dsvt.vasd.voice, 9);

						if ((dsvt.ctrl & 0x40) != 0) {
							recd[i].streamid = 0;
							recd[i].last_time = 0;
							close(recd[i].fd);
							recd[i].fd = -1;
							// printf("Closed echotest audio file:[%s]\n", recd[i].file);

							/* we are in echotest mode, so play it back */
							try {
								std::async(std::launch::async, &CQnetGateway::PlayFileThread, this, std::ref(recd[i]));
							} catch (const std::exception &e) {
								printf("failed to start PlayFileThread. Exception: %s\n", e.what());
								//   When the echotest thread runs, it deletes the file,
								//   Because the echotest thread did NOT start, we delete the file here
								unlink(recd[i].file);
							}
						}
						break;
					}
					else if ((vm[i].fd >= 0) && (vm[i].streamid==dsvt.streamid)) {	// Is the data to be recorded for voicemail
						time(&vm[i].last_time);

						write(vm[i].fd, dsvt.vasd.voice, 9);

						if ((dsvt.ctrl & 0x40) != 0) {
							vm[i].streamid = 0;
							vm[i].last_time = 0;
							close(vm[i].fd);
							vm[i].fd = -1;
							// printf("Closed voicemail audio file:[%s]\n", vm[i].file);
						}
						break;
					}
					else if (toRptr[i].streamid == dsvt.streamid) {	// or maybe this is cross-banding data
						Gate2Modem[i].Write(dsvt.title, 27);

						/* timeit */
						time(&toRptr[i].last_time);

						toRptr[i].sequence = dsvt.ctrl;

						/* End of stream ? */
						if (dsvt.ctrl & 0x40) {
							toRptr[i].last_time = 0;
							toRptr[i].streamid = 0;
							toRptr[i].addr.Initialize(AF_UNSPEC);
						}
						break;
					}
				}

				if (LOG_QSO && dsvt.ctrl&0x40U)
					printf("id=%04x END RPTR\n", ntohs(dsvt.streamid));
			}
		}
	}
}

void CQnetGateway::AddFDSet(int &max, int newfd, fd_set *set)
{
	if (newfd > max)
		max = newfd;
	FD_SET(newfd, set);
}

/* run the main loop for QnetGateway */
void CQnetGateway::Process()
{
	// dtmf stuff initialize
	for (int i=0; i<3; i++) {
		dtmf_buf_count[i] = 0;
		dtmf_buf[i][0] = '\0';
		dtmf_last_frame[i] = 0;
		dtmf_counter[i] = 0U;
	}

	std::future<void> aprs_future, irc_data_future[2];
	if (APRS_ENABLE) {	// start the beacon thread
		try {
			aprs_future = std::async(std::launch::async, &CQnetGateway::APRSBeaconThread, this);
		} catch (const std::exception &e) {
			printf("Failed to start the APRSBeaconThread. Exception: %s\n", e.what());
		}
		if (aprs_future.valid())
			printf("APRS beacon thread started\n");
	}

	for (int i=0; i<2; i++) {
		if (ii[i]) {
			try {	// start the IRC read thread
				irc_data_future[i] = std::async(std::launch::async, &CQnetGateway::GetIRCDataThread, this, i);
			} catch (const std::exception &e) {
				printf("Failed to start GetIRCDataThread[%d]. Exception: %s\n", i, e.what());
				keep_running = false;
			}
			if (keep_running)
				printf("get_irc_data thread[%d] started\n", i);

			ii[i]->kickWatchdog(GW_VERSION);
		}
	}

	while (keep_running) {
		ProcessTimeouts();

		// wait 20 ms max
		int max_nfds = 0;
		fd_set fdset;
		FD_ZERO(&fdset);
		if (g2_sock[0] >= 0)
			AddFDSet(max_nfds, g2_sock[0], &fdset);
		if (g2_sock[1] >= 0)
			AddFDSet(max_nfds, g2_sock[1], &fdset);
		AddFDSet(max_nfds, Link2Gate.GetFD(), &fdset);
		AddFDSet(max_nfds, Modem2Gate.GetFD(), &fdset);
		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 20000; // 20 ms
		(void)select(max_nfds + 1, &fdset, 0, 0, &tv);

		// process packets coming from remote G2
		for (int i=0; i<2; i++) {
			if (g2_sock[i] < 0)
				continue;
			if (keep_running && FD_ISSET(g2_sock[i], &fdset)) {
				SDSVT dsvt;
				socklen_t fromlen = sizeof(struct sockaddr_storage);
				ssize_t g2buflen = recvfrom(g2_sock[i], dsvt.title, 56, 0, fromDstar.GetPointer(), &fromlen);
				if (LOG_QSO && 4==g2buflen && 0==memcmp(dsvt.title, "PONG", 4))
					printf("Got a pong from [%s]:%u\n", fromDstar.GetAddress(), fromDstar.GetPort());
				else
					ProcessG2(g2buflen, dsvt, i);
				FD_CLR(g2_sock[i], &fdset);
			}
		}

		// process packets from qnlink
		if (keep_running && FD_ISSET(Link2Gate.GetFD(), &fdset)) {
			SDSVT dsvt;
			ssize_t g2buflen = Link2Gate.Read(dsvt.title, 56);
            if (16==g2buflen && 0==memcmp(dsvt.title, "LINK", 4)) {
                SLINKFAMILY fam;
                memcpy(fam.title, dsvt.title, 16);
                if (LOG_DEBUG) {
                    printf("Families of linked nodes: A=AF_%s, B=AF_%s, C=AF_%s\n",
                    (AF_UNSPEC==fam.family[0]) ? "UNSPEC" : ((AF_INET==fam.family[0]) ? "INET" : "INET6"),
                    (AF_UNSPEC==fam.family[1]) ? "UNSPEC" : ((AF_INET==fam.family[1]) ? "INET" : "INET6"),
                    (AF_UNSPEC==fam.family[2]) ? "UNSPEC" : ((AF_INET==fam.family[2]) ? "INET" : "INET6")
                    );
                }
                memcpy(link_family, fam.family, 12);
            } else {
			    ProcessG2(g2buflen, dsvt, -1);
            }
			FD_CLR(Link2Gate.GetFD(), &fdset);
		}

		// process packets coming from local repeater module(s)
		if (keep_running && FD_ISSET(Modem2Gate.GetFD(), &fdset)) {
			ProcessModem();
			FD_CLR(Modem2Gate.GetFD(), &fdset);
		}
	}

	// thread clean-up
	if (APRS_ENABLE) {
		if (aprs_future.valid())
			aprs_future.get();
	}
	for (int i=0; i<2; i++) {
		if (ii[i])
			irc_data_future[i].get();
	}
	return;
}

void CQnetGateway::compute_aprs_hash()
{
	short hash = 0x73e2;
	char rptr_sign[CALL_SIZE + 1];

	strcpy(rptr_sign, OWNER.c_str());
	char *p = strchr(rptr_sign, ' ');
	if (!p) {
		printf("Failed to build repeater callsign for aprs hash\n");
		return;
	}
	*p = '\0';
	p = rptr_sign;
	short int len = strlen(rptr_sign);

	for (short int i=0; i < len; i+=2) {
		hash ^= (*p++) << 8;
		hash ^= (*p++);
	}
	Rptr.aprs_hash = hash;

	return;
}

void CQnetGateway::APRSBeaconThread()
{
	char snd_buf[512];
	char rcv_buf[512];
	time_t tnow = 0;

	/*
	   Every 20 seconds, the remote APRS host sends a KEEPALIVE packet-comment
	   on the TCP/APRS port.
	   If we have not received any KEEPALIVE packet-comment after 5 minutes
	   we must assume that the remote APRS host is down or disappeared
	   or has dropped the connection. In these cases, we must re-connect.
	   There are 3 keepalive packets in one minute, or every 20 seconds.
	   In 5 minutes, we should have received a total of 15 keepalive packets.
	*/
	short THRESHOLD_COUNTDOWN = 15;

	time_t last_keepalive_time;
	time(&last_keepalive_time);

	time_t last_beacon_time = 0;
	/* This thread is also saying to the APRS_HOST that we are ALIVE */
	while (keep_running) {
		if (aprs->aprs_sock.GetFD() == -1) {
			aprs->Open(OWNER);
			if (aprs->aprs_sock.GetFD() == -1)
				sleep(1);
			else
				THRESHOLD_COUNTDOWN = 15;
		}

		time(&tnow);
		if ((tnow - last_beacon_time) > (Rptr.aprs_interval * 60)) {
			for (short int i=0; i<3; i++) {
				if (Rptr.mod[i].defined) {
					float tmp_lat = fabs(Rptr.mod[i].latitude);
					float tmp_lon = fabs(Rptr.mod[i].longitude);
					float lat = floor(tmp_lat);
					float lon = floor(tmp_lon);
					lat = (tmp_lat - lat) * 60.0F + lat  * 100.0F;
					lon = (tmp_lon - lon) * 60.0F + lon  * 100.0F;

					char lat_s[15], lon_s[15];
					if (lat >= 1000.0F)
						sprintf(lat_s, "%.2f", lat);
					else if (lat >= 100.0F)
						sprintf(lat_s, "0%.2f", lat);
					else if (lat >= 10.0F)
						sprintf(lat_s, "00%.2f", lat);
					else
						sprintf(lat_s, "000%.2f", lat);

					if (lon >= 10000.0F)
						sprintf(lon_s, "%.2f", lon);
					else if (lon >= 1000.0F)
						sprintf(lon_s, "0%.2f", lon);
					else if (lon >= 100.0F)
						sprintf(lon_s, "00%.2f", lon);
					else if (lon >= 10.0F)
						sprintf(lon_s, "000%.2f", lon);
					else
						sprintf(lon_s, "0000%.2f", lon);

					/* send to aprs */
					sprintf(snd_buf, "%s>APJI23,TCPIP*,qAC,%sS:!%s%cD%s%c&RNG%04u %s %s",
					        Rptr.mod[i].call.c_str(),  Rptr.mod[i].call.c_str(),
					        lat_s,  (Rptr.mod[i].latitude < 0.0)  ? 'S' : 'N',
					        lon_s,  (Rptr.mod[i].longitude < 0.0) ? 'W' : 'E',
					        (unsigned int)Rptr.mod[i].range, Rptr.mod[i].band.c_str(), GW_VERSION.c_str());
					if (LOG_DEBUG)
						printf("APRS Beacon =[%s]\n", snd_buf);
					strcat(snd_buf, "\r\n");

					while (keep_running) {
						if (aprs->aprs_sock.GetFD() == -1) {
							aprs->Open(OWNER);
							if (aprs->aprs_sock.GetFD() == -1)
								sleep(1);
							else
								THRESHOLD_COUNTDOWN = 15;
						} else {
							int rc = aprs->aprs_sock.Write((unsigned char *)snd_buf, strlen(snd_buf));
							if (rc < 0) {
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
									printf("send_aprs_beacon: APRS_HOST closed connection,error=%d\n",errno);
									aprs->aprs_sock.Close();
								} else if (errno == EWOULDBLOCK) {
									std::this_thread::sleep_for(std::chrono::milliseconds(100));
								} else {
									/* Cant do nothing about it */
									printf("send_aprs_beacon failed, error=%d\n", errno);
									break;
								}
							} else {
								// printf("APRS beacon sent\n");
								break;
							}
						}
						int rc = aprs->aprs_sock.Read((unsigned char *)rcv_buf, sizeof(rcv_buf));
						if (rc > 0)
							THRESHOLD_COUNTDOWN = 15;
					}
				}
				int rc = aprs->aprs_sock.Read((unsigned char *)rcv_buf, sizeof(rcv_buf));
				if (rc > 0)
					THRESHOLD_COUNTDOWN = 15;
			}
			time(&last_beacon_time);
		}
		/*
		   Are we still receiving from APRS host ?
		*/
		int rc = aprs->aprs_sock.Read((unsigned char *)rcv_buf, sizeof(rcv_buf));
		if (rc < 0) {
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
				printf("send_aprs_beacon: recv error: APRS_HOST closed connection,error=%d\n",errno);
				aprs->aprs_sock.Close();
			}
		} else if (rc == 0) {
			printf("send_aprs_beacon: recv: APRS shutdown\n");
			aprs->aprs_sock.Close();
		} else
			THRESHOLD_COUNTDOWN = 15;

		std::this_thread::sleep_for(std::chrono::milliseconds(100));

		/* 20 seconds passed already ? */
		time(&tnow);
		if ((tnow - last_keepalive_time) > 20) {
			/* we should be receving keepalive packets ONLY if the connection is alive */
			if (aprs->aprs_sock.GetFD() >= 0) {
				if (THRESHOLD_COUNTDOWN > 0)
					THRESHOLD_COUNTDOWN--;

				if (THRESHOLD_COUNTDOWN == 0) {
					printf("APRS host keepalive timeout\n");
					aprs->aprs_sock.Close();
				}
			}
			/* reset timer */
			time(&last_keepalive_time);
		}
	}
	printf("APRS beacon thread exiting...\n");
	return;
}

void CQnetGateway::PlayFileThread(SECHO &edata)
{
	SDSVT dsvt;
	const unsigned char sdsilence[3] = { 0x16U, 0x29U, 0xF5U };
	const unsigned char sdsync[3] = { 0x55U, 0x2DU, 0x16U };

	printf("File to playback:[%s]\n", edata.file);

	struct stat sbuf;
	if (stat(edata.file, &sbuf)) {
		fprintf(stderr, "Can't stat %s\n", edata.file);
		return;
	}

    if (sbuf.st_size < 65) {
        fprintf(stderr, "Error %s file is too small!\n", edata.file);
        return;
    }

	if ((sbuf.st_size - 56) % 9)
		printf("Warning %s file size of %ld is unexpected!\n", edata.file, sbuf.st_size);
	int ambeblocks = ((int)sbuf.st_size - 56) / 9;

	FILE *fp = fopen(edata.file, "rb");
	if (!fp) {
		fprintf(stderr, "Failed to open file %s\n", edata.file);
		return;
	}

    if (1 != fread(dsvt.title, 56, 1, fp)) {
        fprintf(stderr, "PlayFile Error: Can't read header from %s\n", edata.file);
        fclose(fp);
        return;
    }
	int mod = dsvt.hdr.rpt1[7] - 'A';
	if (mod<0 || mod>2) {
		fprintf(stderr, "unknown module suffix '%s'\n", dsvt.hdr.rpt1);
		return;
	}

	sleep(TIMING_PLAY_WAIT);

	// reformat and send it
	memcpy(dsvt.hdr.urcall, "CQCQCQ  ", 8);
	calcPFCS(dsvt.title, 56);

	Gate2Modem[mod].Write(dsvt.title, 56);

	dsvt.config = 0x20U;

	for (int i=0; i<ambeblocks; i++) {

		int nread = fread(dsvt.vasd.voice, 9, 1, fp);
		if (nread == 1) {
			dsvt.ctrl = (unsigned char)(i % 21);
			if (0x0U == dsvt.ctrl) {
				memcpy(dsvt.vasd.text, sdsync, 3);
			} else {
				switch (i) {
					case 1:
						dsvt.vasd.text[0] = '@' ^ 0x70;
						dsvt.vasd.text[1] = edata.message[0] ^ 0x4f;
						dsvt.vasd.text[2] = edata.message[1] ^ 0x93;
						break;
					case 2:
						dsvt.vasd.text[0] = edata.message[2] ^ 0x70;
						dsvt.vasd.text[1] = edata.message[3] ^ 0x4f;
						dsvt.vasd.text[2] = edata.message[4] ^ 0x93;
						break;
					case 3:
						dsvt.vasd.text[0] = 'A' ^ 0x70;
						dsvt.vasd.text[1] = edata.message[5] ^ 0x4f;
						dsvt.vasd.text[2] = edata.message[6] ^ 0x93;
						break;
					case 4:
						dsvt.vasd.text[0] = edata.message[7] ^ 0x70;
						dsvt.vasd.text[1] = edata.message[8] ^ 0x4f;
						dsvt.vasd.text[2] = edata.message[9] ^ 0x93;
						break;
					case 5:
						dsvt.vasd.text[0] = 'B' ^ 0x70;
						dsvt.vasd.text[1] = edata.message[10] ^ 0x4f;
						dsvt.vasd.text[2] = edata.message[11] ^ 0x93;
						break;
					case 6:
						dsvt.vasd.text[0] = edata.message[12] ^ 0x70;
						dsvt.vasd.text[1] = edata.message[13] ^ 0x4f;
						dsvt.vasd.text[2] = edata.message[14] ^ 0x93;
						break;
					case 7:
						dsvt.vasd.text[0] = 'C' ^ 0x70;
						dsvt.vasd.text[1] = edata.message[15] ^ 0x4f;
						dsvt.vasd.text[2] = edata.message[16] ^ 0x93;
						break;
					case 8:
						dsvt.vasd.text[0] = edata.message[17] ^ 0x70;
						dsvt.vasd.text[1] = edata.message[18] ^ 0x4f;
						dsvt.vasd.text[2] = edata.message[19] ^ 0x93;
						break;
					default:
						memcpy(dsvt.vasd.text, sdsilence, 3);
						break;
				}
			}
			if (i+1 == ambeblocks)
				dsvt.ctrl |= 0x40U;

			Gate2Modem[mod].Write(dsvt.title, 27);

			std::this_thread::sleep_for(std::chrono::milliseconds(TIMING_PLAY_DELAY));
		}
	}
	fclose(fp);
	printf("Finished playing\n");
	// if it's an echo file, delete it!
	if (strstr(edata.file, "echotest.dat")) {
		unlink(edata.file);
		edata.file[0] = edata.message[0] = '\0';
	}
	return;
}

void CQnetGateway::qrgs_and_maps()
{
	for (int i=0; i<3; i++) {
		std::string rptrcall = OWNER;
		rptrcall.resize(CALL_SIZE-1);
		rptrcall += i + 'A';
		for (int j=0; j<2; j++) {
			if (ii[j]) {
				if (Rptr.mod[i].latitude || Rptr.mod[i].longitude || Rptr.mod[i].desc1.length() || Rptr.mod[i].url.length())
					ii[j]->rptrQTH(rptrcall, Rptr.mod[i].latitude, Rptr.mod[i].longitude, Rptr.mod[i].desc1, Rptr.mod[i].desc2, Rptr.mod[i].url, Rptr.mod[i].package_version);
				if (Rptr.mod[i].frequency)
					ii[j]->rptrQRG(rptrcall, Rptr.mod[i].frequency, Rptr.mod[i].offset, Rptr.mod[i].range, Rptr.mod[i].agl);
			}
		}
	}

	return;
}

bool CQnetGateway::Init(char *cfgfile)
{
	short int i;

	setvbuf(stdout, (char *)NULL, _IOLBF, 0);


	/* Used to validate MYCALL input */
	try {
		preg = std::regex("^(([1-9][A-Z])|([A-Z][0-9])|([A-Z][A-Z][0-9]))[0-9A-Z]*[A-Z][ ]*[ A-RT-Z]$", std::regex::extended);
	} catch (std::regex_error &e) {
		printf("Regular expression error: %s\n", e.what());
		return true;
	}

	std::signal(SIGTERM, sigCatch);
	std::signal(SIGINT,  sigCatch);
	std::signal(SIGHUP,  sigCatch);

	for (i = 0; i < 3; i++)
		memset(&band_txt[0], 0, sizeof(SBANDTXT));

	/* process configuration file */
	if ( ReadConfig(cfgfile) ) {
		printf("Failed to process config file %s\n", cfgfile);
		return true;
	}

	// open database
	if (DASH_SHOW_LH) {
		std::string dbname(CFG_DIR);
		dbname.append("/");
		dbname.append(DASH_SQL_NAME);
		if (qnDB.Open(dbname.c_str()))
			return true;
	}

	playNotInCache = false;

	/* build the repeater callsigns for aprs */
	Rptr.mod[0].call = OWNER;
	for (i=OWNER.length(); i; i--)
		if (! isspace(OWNER[i-1]))
			break;
	Rptr.mod[0].call.resize(i);

	Rptr.mod[1].call = Rptr.mod[0].call;
	Rptr.mod[2].call = Rptr.mod[0].call;
	Rptr.mod[0].call += "-A";
	Rptr.mod[1].call += "-B";
	Rptr.mod[2].call += "-C";
	Rptr.mod[0].band = "23cm";
	Rptr.mod[1].band = "70cm";
	Rptr.mod[2].band = "2m";
	printf("Repeater callsigns: [%s] [%s] [%s]\n", Rptr.mod[0].call.c_str(), Rptr.mod[1].call.c_str(), Rptr.mod[2].call.c_str());

	for (i = 0; i < 3; i++) {
		//Rptr.mod[i].frequency = Rptr.mod[i].offset = Rptr.mod[i].latitude = Rptr.mod[i].longitude = Rptr.mod[i].agl = Rptr.mod[i].range = 0.0;
		band_txt[i].streamID = 0;
		band_txt[i].flags[0] = band_txt[i].flags[1] = band_txt[i].flags[2] = 0;
		band_txt[i].lh_mycall[0] = '\0';
		band_txt[i].lh_sfx[0] = '\0';
		band_txt[i].lh_yrcall[0] = '\0';
		band_txt[i].lh_rpt1[0] = '\0';
		band_txt[i].lh_rpt2[0] = '\0';

		band_txt[i].last_time = 0;

		band_txt[i].txt[0] = '\0';
		band_txt[i].txt_cnt = 0;
		band_txt[i].sent_key_on_msg = false;

		band_txt[i].dest_rptr[0] = '\0';

		band_txt[i].temp_line[0] = '\0';
		band_txt[i].temp_line_cnt = 0;
		band_txt[i].gprmc[0] = '\0';
		band_txt[i].gpid[0] = '\0';
		band_txt[i].is_gps_sent = false;
		band_txt[i].gps_last_time = 0;

		band_txt[i].num_dv_frames = 0;
		band_txt[i].num_dv_silent_frames = 0;
		band_txt[i].num_bit_errors = 0;

	}

	if (APRS_ENABLE) {
		aprs = new CAPRS(&Rptr);
		if (aprs)
			aprs->Init();
		else {
			printf("aprs class init failed!\nAPRS will be turned off");
			APRS_ENABLE = false;
		}
	}
	compute_aprs_hash();

	for (int j=0; j<2; j++) {
		if (ircddb[j].ip.empty())
			continue;
		ii[j] = new CIRCDDB(ircddb[j].ip, ircddb[j].port, owner, IRCDDB_PASSWORD[j], GW_VERSION.c_str(), &cache);
		bool ok = ii[j]->open();
		if (!ok) {
			printf("%s open failed\n", ircddb[j].ip.c_str());
			return true;
		}

		int rc = ii[j]->getConnectionState();
		printf("Waiting for %s connection status of 2\n", ircddb[j].ip.c_str());
		i = 0;
		while (rc < 2) {
			printf("%s status=%d\n", ircddb[j].ip.c_str(), rc);
			if (rc < 2) {
				i++;
				sleep(5);
			} else
				break;

			if (!keep_running)
				break;

			if (i > 5) {
				printf("We can not wait any longer for %s...\n", ircddb[j].ip.c_str());
				break;
			}
			rc = ii[j]->getConnectionState();
		}
		switch (ii[j]->GetFamily()) {
		case AF_INET:
			printf("IRC server is using IPV4\n");
			af_family[j] = AF_INET;
			break;
		case AF_INET6:
			printf("IRC server is using IPV6\n");
			af_family[j] = AF_INET6;
			break;
		default:
			printf("%s server is using unknown protocol! Shutting down...\n", ircddb[j].ip.c_str());
			return true;
		}
	}
		/* udp port 40000 must open first */
	if (ii[0]) {
		SPORTIP *pip = (AF_INET == af_family[0]) ? &g2_external : & g2_ipv6_external;
		g2_sock[0] = open_port(pip, af_family[0]);
		if (0 > g2_sock[0]) {
			printf("Can't open %s:%d for %s\n", pip->ip.c_str(), pip->port, ircddb[i].ip.c_str());
			return true;
		}
		if (ii[1] && (af_family[0] != af_family[1])) {	// we only need to open a second port if the family for the irc servers are different!
			SPORTIP *pip = (AF_INET == af_family[1]) ? &g2_external : & g2_ipv6_external;
			g2_sock[1] = open_port(pip, af_family[1]);
			if (0 > g2_sock[1]) {
				printf("Can't open %s:%d for %s\n", pip->ip.c_str(), pip->port, ircddb[1].ip.c_str());
				return true;
			}
		}
	} else if (ii[1]) {
		SPORTIP *pip = (AF_INET == af_family[1]) ? &g2_external : & g2_ipv6_external;
		g2_sock[1] = open_port(pip, af_family[1]);
		if (0 > g2_sock[1]) {
			printf("Can't open %s:%d for %s\n", pip->ip.c_str(), pip->port, ircddb[1].ip.c_str());
			return true;
		}
	}

	// Open unix sockets between qngateway and qnlink
	Gate2Link.SetUp(gate2link.c_str());
	if (Link2Gate.Open(link2gate.c_str()))
		return true;
	if (Modem2Gate.Open(modem2gate.c_str()))
		return true;

	for (i=0; i<3; i++) {
		if (Rptr.mod[i].defined) {	// open unix sockets between qngateway and each defined modem
			Gate2Modem[i].SetUp(gate2modem[i].c_str());
		}
		// recording for echotest on local repeater modules
		recd[i].last_time = 0;
		recd[i].streamid = 0;
		recd[i].fd = -1;
		memset(recd[i].file, 0, sizeof(recd[i].file));

		// recording for voicemail on local repeater modules
		vm[i].last_time = 0;
		vm[i].streamid = 0;
		vm[i].fd = -1;
		memset(vm[i].file, 0, sizeof(vm[i].file));

		snprintf(vm[i].file, FILENAME_MAX, "%s/%c_%s", FILE_ECHOTEST.c_str(), 'A'+i, "voicemail.dat2");

		if (access(vm[i].file, F_OK) != 0)
			memset(vm[i].file, 0, sizeof(vm[i].file));
		else
			printf("Loaded voicemail file: %s for mod %d\n", vm[i].file, i);

		// the repeater modules run on these ports
		memset(toRptr[i].saved_hdr.title, 0, 56);
		toRptr[i].saved_addr.Initialize(AF_UNSPEC);

		toRptr[i].streamid = 0;
		toRptr[i].addr.Initialize(AF_UNSPEC);

		toRptr[i].last_time = 0;

		toRptr[i].sequence = 0x0;
	}

	/*
	   Initialize the end_of_audio that will be sent to the local repeater
	   when audio from remote G2 has timed out
	*/
	memset(end_of_audio.title, 0U, 27U);
	memcpy(end_of_audio.title, "DSVT", 4U);
	end_of_audio.id = end_of_audio.config = 0x20U;

	/* to remote systems */
	for (i = 0; i < 3; i++) {
		to_remote_g2[i].toDstar.Initialize(AF_UNSPEC);
		to_remote_g2[i].streamid = 0;
		to_remote_g2[i].last_time = 0;
	}

	printf("QnetGateway...entering processing loop\n");

	if (GATEWAY_SEND_QRGS_MAP)
		qrgs_and_maps();
	return false;
}

CQnetGateway::CQnetGateway()
{
	ii[0] = ii[1] = NULL;
}

CQnetGateway::~CQnetGateway()
{
	Link2Gate.Close();
	Modem2Gate.Close();

	if (APRS_ENABLE) {
		if (aprs->aprs_sock.GetFD() != -1) {
			aprs->aprs_sock.Close();
			printf("Closed APRS\n");
		}
		delete aprs;
	}

	for (int i=0; i<3; i++) {
		recd[i].last_time = 0;
		recd[i].streamid = 0;
		if (recd[i].fd >= 0) {
			close(recd[i].fd);
			unlink(recd[i].file);
		}
	}

	for (int i=0; i<2; i++) {
		if (g2_sock[i] >= 0) {
			close(g2_sock[i]);
			printf("Closed G2_EXTERNAL_PORT %d\n", i);
		}
		if (ii[i]) {
			ii[i]->close();
			delete ii[i];
		}
	}

	printf("QnetGateway exiting\n");
}

bool CQnetGateway::validate_csum(SBANDTXT &bt, bool is_gps)
{
	const char *name = is_gps ? "GPS" : "GPRMC";
	char *s = is_gps ? bt.gpid : bt.gprmc;
	char *p = strrchr(s, '*');
	if (!p) {
		// BAD news, something went wrong
		printf("Missing asterisk before checksum in %s\n", name);
		bt.gprmc[0] = bt.gpid[0] = '\0';
		return true;
	} else {
		*p = '\0';
		// verify csum in GPRMC
		bool ok = verify_gps_csum(s + 1, p + 1);
		if (!ok) {
			printf("csum in %s not good\n", name);
			bt.gprmc[0] = bt.gpid[0] = '\0';
			return true;
		}
	}
	return false;
}

void CQnetGateway::gps_send(short int rptr_idx)
{
	time_t tnow = 0;
	static char old_mycall[CALL_SIZE + 1] = { "        " };

	if ((rptr_idx < 0) || (rptr_idx > 2)) {
		printf("ERROR in gps_send: rptr_idx %d is invalid\n", rptr_idx);
		return;
	}

	if (band_txt[rptr_idx].gprmc[0] == '\0') {
		band_txt[rptr_idx].gpid[0] = '\0';
		printf("missing GPS ID\n");
		return;
	}
	if (band_txt[rptr_idx].gpid[0] == '\0') {
		band_txt[rptr_idx].gprmc[0] = '\0';
		printf("Missing GPSRMC\n");
		return;
	}
	if (memcmp(band_txt[rptr_idx].gpid, band_txt[rptr_idx].lh_mycall, CALL_SIZE) != 0) {
		printf("MYCALL [%s] does not match first 8 characters of GPS ID [%.8s]\n", band_txt[rptr_idx].lh_mycall, band_txt[rptr_idx].gpid);
		band_txt[rptr_idx].gprmc[0] = '\0';
		band_txt[rptr_idx].gpid[0] = '\0';
		return;
	}

	/* if new station, reset last time */
	if (strcmp(old_mycall, band_txt[rptr_idx].lh_mycall) != 0) {
		strcpy(old_mycall, band_txt[rptr_idx].lh_mycall);
		band_txt[rptr_idx].gps_last_time = 0;
	}

	/* do NOT process often */
	time(&tnow);
	if ((tnow - band_txt[rptr_idx].gps_last_time) < 31)
		return;

	printf("GPRMC=[%s]\n", band_txt[rptr_idx].gprmc);
	printf("GPS id=[%s]\n",band_txt[rptr_idx].gpid);

	if (validate_csum(band_txt[rptr_idx], false))	// || validate_csum(band_txt[rptr_idx], true))
		return;

	/* now convert GPS into APRS and send it */
	build_aprs_from_gps_and_send(rptr_idx);

	band_txt[rptr_idx].is_gps_sent = true;
	time(&(band_txt[rptr_idx].gps_last_time));
	return;
}

void CQnetGateway::build_aprs_from_gps_and_send(short int rptr_idx)
{
	char buf[512];
	const char *delim = ",";

	char *saveptr = NULL;

	/*** dont care about the rest */

	strcpy(buf, band_txt[rptr_idx].lh_mycall);
	char *p = strchr(buf, ' ');
	if (p) {
		if (band_txt[rptr_idx].lh_mycall[7] != ' ') {
			*p = '-';
			*(p + 1) = band_txt[rptr_idx].lh_mycall[7];
			*(p + 2) = '>';
			*(p + 3) = '\0';
		} else {
			*p = '>';
			*(p + 1) = '\0';
		}
	} else
		strcat(buf, ">");

	strcat(buf, "APDPRS,DSTAR*,qAR,");
	strcat(buf, Rptr.mod[rptr_idx].call.c_str());
	strcat(buf, ":!");

	//GPRMC =
	strtok_r(band_txt[rptr_idx].gprmc, delim, &saveptr);
	//time_utc =
	strtok_r(NULL, delim, &saveptr);
	//nav =
	strtok_r(NULL, delim, &saveptr);
	char *lat_str = strtok_r(NULL, delim, &saveptr);
	char *lat_NS = strtok_r(NULL, delim, &saveptr);
	char *lon_str = strtok_r(NULL, delim, &saveptr);
	char *lon_EW = strtok_r(NULL, delim, &saveptr);

	if (lat_str && lat_NS) {
		if ((*lat_NS != 'N') && (*lat_NS != 'S')) {
			printf("Invalid North or South indicator in latitude\n");
			return;
		}
		if (strlen(lat_str) > 9) {
			printf("Invalid latitude\n");
			return;
		}
		if (lat_str[4] != '.') {
			printf("Invalid latitude\n");
			return;
		}
		lat_str[7] = '\0';
		strcat(buf, lat_str);
		strcat(buf, lat_NS);
	} else {
		printf("Invalid latitude\n");
		return;
	}
	/* secondary table */
	strcat(buf, "/");

	if (lon_str && lon_EW) {
		if ((*lon_EW != 'E') && (*lon_EW != 'W')) {
			printf("Invalid East or West indicator in longitude\n");
			return;
		}
		if (strlen(lon_str) > 10) {
			printf("Invalid longitude\n");
			return;
		}
		if (lon_str[5] != '.') {
			printf("Invalid longitude\n");
			return;
		}
		lon_str[8] = '\0';
		strcat(buf, lon_str);
		strcat(buf, lon_EW);
	} else {
		printf("Invalid longitude\n");
		return;
	}

	/* Just this symbolcode only */
	strcat(buf, "/");
	strncat(buf, band_txt[rptr_idx].gpid + 13, 32);

	// printf("Built APRS from old GPS mode=[%s]\n", buf);
	strcat(buf, "\r\n");

	if (aprs->aprs_sock.Write((unsigned char *)buf, strlen(buf))) {
		if ((errno == EPIPE) || (errno == ECONNRESET) || (errno == ETIMEDOUT) || (errno == ECONNABORTED) ||
		    (errno == ESHUTDOWN) || (errno == EHOSTUNREACH) || (errno == ENETRESET) || (errno == ENETDOWN) ||
		    (errno == ENETUNREACH) || (errno == EHOSTDOWN) || (errno == ENOTCONN)) {
			printf("build_aprs_from_gps_and_send: APRS_HOST closed connection, error=%d\n", errno);
			aprs->aprs_sock.Close();
		} else
			printf("build_aprs_from_gps_and_send: send error=%d\n", errno);
	}
	return;
}

bool CQnetGateway::verify_gps_csum(char *gps_text, char *csum_text)
{
	short computed_csum = 0;
	char computed_csum_text[16];

	short int len = strlen(gps_text);
	for (short int i=0; i<len; i++) {
		char c = gps_text[i];
		if (computed_csum == 0)
			computed_csum = (char)c;
		else
			computed_csum = computed_csum ^ ((char)c);
	}
	sprintf(computed_csum_text, "%02X", computed_csum);
	// printf("computed_csum_text=[%s]\n", computed_csum_text);

	char *p = strchr(csum_text, ' ');
	if (p)
		*p = '\0';

	if (strcmp(computed_csum_text, csum_text) == 0)
		return true;
	else
		return false;
}

int main(int argc, char **argv)
{
	printf("VERSION %s\n", GW_VERSION.c_str());
	if (argc != 2) {
		printf("usage: %s qn.cfg\n", argv[0]);
		return 1;
	}
	CQnetGateway QnetGateway;
	if (QnetGateway.Init(argv[1])) {
		keep_running = false;	// shutdown any processes that may have been started
		return 1;
	}
	QnetGateway.Process();
	printf("Leaving processing loop...\n");
}

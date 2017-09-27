/*
 * ZeroTier SDK - Network Virtualization Everywhere
 * Copyright (C) 2011-2017  ZeroTier, Inc.  https://www.zerotier.com/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * You can be released from the requirements of the license by purchasing
 * a commercial license. Buying such a license is mandatory as soon as you
 * develop commercial closed-source software that incorporates or links
 * directly against ZeroTier software without disclosing the source code
 * of your own application.
 */

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <arpa/inet.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <fstream>
#include <map>
#include <ctime>
#include <sys/time.h>
#include <pthread.h>
#include <signal.h>
#include <cstring>

#include "libzt.h"

#if defined(__SELFTEST__)
#include "Utils.hpp"
#endif

#define EXIT_ON_FAIL           false

#define PASSED                 1
#define FAILED                 0

#define ECHO_INTERVAL          1000000 // microseconds
#define SLAM_INTERVAL          500000  // microseconds

#define WAIT_FOR_TEST_TO_CONCLUDE         0
#define ARTIFICIAL_SOCKET_LINGER          1

#define STR_SIZE               32

#define TEST_OP_N_BYTES        10
#define TEST_OP_N_SECONDS      11
#define TEST_OP_N_TIMES        12

#define TEST_MODE_CLIENT       20
#define TEST_MODE_SERVER       21

#define TEST_TYPE_SIMPLE       30
#define TEST_TYPE_SUSTAINED    31
#define TEST_TYPE_PERF         32
#define TEST_TYPE_PERF_TO_ECHO 33

#define MIN_PORT               5000
#define MAX_PORT               50000

#define TCP_UNIT_TEST_SIG_4    struct sockaddr_in *addr, int op, int cnt, char *details, \
									bool *passed
#define UDP_UNIT_TEST_SIG_4    struct sockaddr_in *local_addr, struct sockaddr_in *remote_addr, \
									int op, int cnt, char *details, bool *passed

#define TCP_UNIT_TEST_SIG_6    struct sockaddr_in6 *addr, int op, int cnt, char *details, \
									bool *passed
#define UDP_UNIT_TEST_SIG_6    struct sockaddr_in6 *local_addr, struct sockaddr_in6 *remote_addr, \
									int op, int cnt, char *details, bool *passed

#define ECHOTEST_MODE_RX       333
#define ECHOTEST_MODE_TX       666

#define DATA_BUF_SZ            1024*32

#define MAX_RX_BUF_SZ          2048
#define MAX_TX_BUF_SZ          2048

#define ONE_MEGABYTE           1024 * 1024

#define DETAILS_STR_LEN        128


// If running a self test, use libzt calls
#if defined(__SELFTEST__)
#define SOCKET zts_socket
#define BIND zts_bind
#define LISTEN zts_listen
#define ACCEPT zts_accept
#define CONNECT zts_connect
#define READ zts_read
#define WRITE zts_write
#define RECV zts_recvmsg
#define SEND zts_send
#define RECVFROM zts_recvfrom
#define SENDTO zts_sendto
#define RECVMSG zts_recvmsg
#define SENDMSG zts_sendmsg
#define SETSOCKOPT zts_setsockopt
#define GETSOCKOPT zts_getsockopt
#define IOCTL zts_ioctl
#define FCNTL zts_fcntl
#define CLOSE zts_close
#define GETPEERNAME zts_getpeername
#endif

// If running a native instance to test against, use system calls
#if defined(__NATIVETEST__)
inline unsigned int gettid()
{
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__unix__)
    return static_cast<unsigned int>(::syscall(__NR_gettid));
#elif defined(__APPLE__)
    uint64_t tid64;
    pthread_threadid_np(NULL, &tid64);
    return static_cast<unsigned int>(tid64);
#endif
}

#define SOCKET socket
#define BIND bind
#define LISTEN listen
#define ACCEPT accept
#define CONNECT connect
#define READ read
#define WRITE write
#define RECV recvmsg
#define SEND send
#define RECVFROM recvfrom
#define SENDTO sendto
#define RECVMSG recvmsg
#define SENDMSG sendmsg
#define SETSOCKOPT setsockopt
#define GETSOCKOPT getsockopt
#define IOCTL ioctl
#define FCNTL fcntl
#define CLOSE close
#define GETPEERNAME getpeername
#endif

std::map<std::string, std::string> testConf;

/* Tests in this file:

	Basic RX/TX connect()/accept() Functionality:

	[ ?]                      slam - perform thousands of the same call per second
	[  ]                    random - act like a monkey, press all the buttons
	[OK]        simple client ipv4 - connect, send one message and wait for an echo
	[OK]        simple server ipv4 - accept, read one message and echo it back
	[OK]        simple client ipv6 - connect, send one message and wait for an echo
	[OK]        simple server ipv6 - accept, read one message and echo it back
	[OK]     sustained client ipv4 - connect and rx/tx many messages, VERIFIES data integrity
	[OK]     sustained server ipv4 - accept and echo messages, VERIFIES data integrity
	[OK]     sustained client ipv6 - connect and rx/tx many messages, VERIFIES data integrity
	[OK]     sustained server ipv6 - accept and echo messages, VERIFIES data integrity
	[OK] comprehensive client ipv4 - test all ipv4/6 client simple/sustained modes
	[OK] comprehensive server ipv6 - test all ipv4/6 server simple/sustained modes
	[ ?]       SOCK_RAW (VL2) ipv4 - See test/layer2.cpp
	[ ?]       SOCK_RAW (VL2) ipv6 - See test/layer2.cpp

	Performance: 
		 (See libzt.h, compile libzt with appropriate ZT_TCP_TX_BUF_SZ, ZT_TCP_RX_BUF_SZ, ZT_UDP_TX_BUF_SZ, and ZT_UDO_RX_BUF_SZ for your test)

	[OK]                Throughput - Test maximum RX/TX speeds
	[  ]              Memory Usage - Test memory consumption profile
	[  ]                 CPU Usage - Test processor usage
	[  ]               

	Correctness:

	[  ]           Block/Non-block - Test that blocking and non-blocking behaviour is consistent
	[  ]      Release of resources - Test that all destructor methods/blocks function properly
	[OK]    Multi-network handling - Test internal Tap multiplexing works for multiple networks
	[  ]          Address handling - Test that addresses are copied/parsed/returned properly

*/





/****************************************************************************/
/* Helper Functions                                                         */
/****************************************************************************/

void displayResults(int *results, int size) 
{
	int success = 0, failure = 0;
	for (int i=0; i<size; i++) {
		if (results[i] == 0) {
			success++;
		}
		else {
			failure++;
		}
	}
	std::cout << "tials: " << size << std::endl;
	std::cout << " - success = " << (float)success / (float)size << std::endl;
	std::cout << " - failure = " << (float)failure / (float)size << std::endl;
}

void loadTestConfigFile(std::string filepath) 
{
	std::string key, value, prefix;
	std::ifstream testFile;
	testFile.open(filepath.c_str());
	while (testFile >> key >> value) {
		if (key == "name") {
			prefix = value;
		}
		if (key[0] != '#' && key[0] != ';') {
			testConf[prefix + "." + key] = value;
			fprintf(stderr, "%s.%s = %s\n", prefix.c_str(), key.c_str(), testConf[prefix + "." + key].c_str());
		}

	}
	testFile.close();
}

long int get_now_ts() 
{
	struct timeval tp;
	gettimeofday(&tp, NULL);
	return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

// for syncronizing tests
void wait_until_tplus(long int original_time, int tplus_ms) 
{
	while (original_time + tplus_ms > get_now_ts()) { 
		sleep(1);
	}		
}
void wait_until_tplus_s(long int original_time, int tplus_s) 
{
	int current_time_offset = (get_now_ts() - original_time) / 1000;
	fprintf(stderr, "\n\n--- WAITING FOR T+%d --- (current: T+%d)\n\n", tplus_s, current_time_offset);
	if (current_time_offset > tplus_s) {
		DEBUG_ERROR("--- ABORTING TEST: Tests are out of sync and might not yield valid results. ---");
		//exit(0);
	}
	if (current_time_offset == tplus_s) {
		DEBUG_ERROR("--- WARNING: Tests might be out of sync and might not yield valid results. ---");
	}
	wait_until_tplus(original_time, tplus_s * 1000);
}

int rand_in_range(int min, int max)
{
#if defined(__SELFTEST__)
	unsigned int seed;
	ZeroTier::Utils::getSecureRandom((void*)&seed,sizeof(seed));
	srand(seed);
#else
	srand((unsigned int)time(NULL));
#endif
	return min + rand() % static_cast<int>(max - min + 1);
}

void generate_random_data(void *buf, size_t n, int min, int max) 
{
	char *b = (char*)buf;
	for (int i=0; i<n; i++) {
		b[i] = rand_in_range(min, max);
	}
}

void str2addr(std::string ipstr, int port, int ipv, struct sockaddr *saddr) 
{
	if (ipv == 4) {
		struct sockaddr_in *in4 = (struct sockaddr_in*)saddr;
		in4->sin_port = htons(port);
		in4->sin_addr.s_addr = inet_addr(ipstr.c_str());
		in4->sin_family = AF_INET;
	}
	if (ipv == 6) {
		struct sockaddr_in6 *in6 = (struct sockaddr_in6*)saddr;
		inet_pton(AF_INET6, ipstr.c_str(), &(in6->sin6_addr));
		in6->sin6_flowinfo = 0;
		in6->sin6_family = AF_INET6;
		in6->sin6_port = htons(port);
	}
}

void RECORD_RESULTS(bool passed, char *details, std::vector<std::string> *results)
{
	char *ok_str   = (char*)"[  OK  ]";
	char *fail_str = (char*)"[ FAIL ]";
	if (passed == PASSED) {
		DEBUG_TEST("%s", ok_str);
		results->push_back(std::string(ok_str) + " " + std::string(details));
	}
	else {
		DEBUG_ERROR("%s", fail_str);		
		results->push_back(std::string(fail_str) + " " + std::string(details));
	}
	if (EXIT_ON_FAIL && !passed) {
		fprintf(stderr, "%s\n", results->at(results->size()-1).c_str());
		exit(0);
	}
	memset(details, 0, DETAILS_STR_LEN);
}





/****************************************************************************/
/* SIMPLE                                                                   */
/****************************************************************************/





// TCP

// TEST-1
void tcp_client_4(TCP_UNIT_TEST_SIG_4)
{
	std::string testname = "tcp_client_4";
	std::string msg = "tcp_cs_4";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "connect to remote host with IPv4 address, write string, read string, compare.\n");
	int r, w, fd, err, len = strlen(msg.c_str());
	char rbuf[STR_SIZE];
	memset(rbuf, 0, sizeof rbuf);
	if ((fd = SOCKET(AF_INET, SOCK_STREAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = CONNECT(fd, (const struct sockaddr *)addr, sizeof(*addr))) < 0) {
		DEBUG_ERROR("error connecting to remote host (%d)", err);
		perror("connect");
		*passed = false;
		return;
	}
	// TODO: Put this test in the general API section
	struct sockaddr_storage peer_addr;
	struct sockaddr_in *in4 = (struct sockaddr_in*)&peer_addr;
	socklen_t peer_addrlen = sizeof(peer_addr);
	
	if ((err = GETPEERNAME(fd, (struct sockaddr*)&peer_addr, &peer_addrlen)) < 0) {
		perror("getpeername");
		*passed = false;
		return;
	}
	DEBUG_TEST("getpeername() => %s : %d", inet_ntoa(in4->sin_addr), ntohs(in4->sin_port));

	w = WRITE(fd, msg.c_str(), len);
	r = READ(fd, rbuf, len);
	DEBUG_TEST("Sent     : %s", msg.c_str());
	DEBUG_TEST("Received : %s", rbuf);
	sleep(ARTIFICIAL_SOCKET_LINGER);
	err = CLOSE(fd);
	sprintf(details, "%s, err=%d, r=%d, w=%d", testname.c_str(), err, r, w);
	*passed = (w == len && r == len && !err) && !strcmp(rbuf, msg.c_str());
}





// TEST-2
void tcp_server_4(TCP_UNIT_TEST_SIG_4)
{
	std::string testname = "tcp_server_4";
	std::string msg = "tcp_cs_4";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "accept connection with IPv4 address, read string, write string, compare.\n");
	int w=0, r=0, fd, client_fd, err, len = strlen(msg.c_str());
	char rbuf[STR_SIZE];
	memset(rbuf, 0, sizeof rbuf);
	if ((fd = SOCKET(AF_INET, SOCK_STREAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = BIND(fd, (struct sockaddr *)addr, sizeof(struct sockaddr_in)) < 0)) {
		DEBUG_ERROR("error binding to interface (%d)", err);
		perror("bind");
		*passed = false;
		return;
	}
	if ((err = LISTEN(fd, 100)) < 0) {
		printf("error placing socket in LISTENING state (%d)", err);
		perror("listen");
		*passed = false;
		return;
	}
	struct sockaddr_in client;
	socklen_t client_addrlen = sizeof(sockaddr_in);
	if ((client_fd = ACCEPT(fd, (struct sockaddr *)&client, &client_addrlen)) < 0) {
		perror("accept");
		*passed = false;
		return;
	}
	DEBUG_TEST("accepted connection from %s, on port %d", inet_ntoa(client.sin_addr), ntohs(client.sin_port));
	// TODO: Put this test in the general API section
	struct sockaddr_storage peer_addr;
	struct sockaddr_in *in4 = (struct sockaddr_in*)&peer_addr;
	socklen_t peer_addrlen = sizeof(peer_addr);

	if ((err = GETPEERNAME(client_fd, (struct sockaddr*)&peer_addr, &peer_addrlen)) < 0) {
		perror("getpeername");
		*passed = false;
		return;
	}
	DEBUG_TEST("getpeername() => %s : %d", inet_ntoa(in4->sin_addr), ntohs(in4->sin_port));
	r = READ(client_fd, rbuf, len);
	w = WRITE(client_fd, rbuf, len);
	DEBUG_TEST("Received : %s, r=%d, w=%d", rbuf, r, w);
	sleep(ARTIFICIAL_SOCKET_LINGER);
	err = CLOSE(fd);
	err = CLOSE(client_fd);
	sprintf(details, "%s, err=%d, r=%d, w=%d", testname.c_str(), err, r, w);
	*passed = (w == len && r == len && !err) && !strcmp(rbuf, msg.c_str());
}





// TEST-3
void tcp_client_6(TCP_UNIT_TEST_SIG_6)
{
	std::string testname = "tcp_client_6";
	std::string msg = "tcp_cs_6";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "connect to remote host with IPv6 address, write string, read string, compare.\n");
	int r, w, fd, err, len = strlen(msg.c_str());
	char rbuf[STR_SIZE];
	memset(rbuf, 0, sizeof rbuf);
	if ((fd = SOCKET(AF_INET6, SOCK_STREAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = CONNECT(fd, (const struct sockaddr *)addr, sizeof(*addr))) < 0) {
		DEBUG_ERROR("error connecting to remote host (%d)", err);
		perror("connect");
		*passed = false;
		return;
	}
	// TODO: Put this test in the general API section
	struct sockaddr_storage peer_addr;
	struct sockaddr_in6 *p6 = (struct sockaddr_in6*)&peer_addr;
	socklen_t peer_addrlen = sizeof(peer_addr);
	if ((err = GETPEERNAME(fd, (struct sockaddr*)&peer_addr, &peer_addrlen)) < 0) {
		perror("getpeername");
		*passed = false;
		return;
	}
	char peer_addrstr[INET6_ADDRSTRLEN];
	inet_ntop(AF_INET6, &(p6->sin6_addr), peer_addrstr, INET6_ADDRSTRLEN);
	DEBUG_TEST("getpeername() => %s : %d", peer_addrstr, ntohs(p6->sin6_port));

	w = WRITE(fd, msg.c_str(), len);
	r = READ(fd, rbuf, len);
	sleep(ARTIFICIAL_SOCKET_LINGER);
	err = CLOSE(fd);
	sprintf(details, "%s, err=%d, r=%d, w=%d", testname.c_str(), err, r, w);
	DEBUG_TEST("Sent     : %s", msg.c_str());
	DEBUG_TEST("Received : %s", rbuf);
	*passed = (w == len && r == len && !err) && !strcmp(rbuf, msg.c_str());
}





// TEST-4
void tcp_server_6(TCP_UNIT_TEST_SIG_6)
{
	std::string testname = "tcp_server_6";
	std::string msg = "tcp_cs_6";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "accept connection with IPv6 address, read string, write string, compare.\n");
	int w=0, r=0, fd, client_fd, err, len = strlen(msg.c_str());
	char rbuf[STR_SIZE];
	memset(rbuf, 0, sizeof rbuf);
	if ((fd = SOCKET(AF_INET6, SOCK_STREAM, 0)) < 0) { 
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = BIND(fd, (struct sockaddr *)addr, sizeof(struct sockaddr_in6)) < 0)) {
		DEBUG_ERROR("error binding to interface (%d)", err);
		perror("bind");
		*passed = false;
		return;
	}
	if ((err = LISTEN(fd, 100)) < 0) {
		DEBUG_ERROR("error placing socket in LISTENING state (%d)", err);
		perror("listen");
		*passed = false;
		return;
	}
	struct sockaddr_in6 client;
	socklen_t client_addrlen = sizeof(sockaddr_in6);
	if ((client_fd = ACCEPT(fd, (struct sockaddr *)&client, &client_addrlen)) < 0) {
		perror("accept");
		*passed = false;
		return;
	}
	char ipstr[INET6_ADDRSTRLEN];
	inet_ntop(AF_INET6, &client.sin6_addr, ipstr, sizeof ipstr);
	DEBUG_TEST("accepted connection from %s, on port %d", ipstr, ntohs(client.sin6_port));
	// TODO: Put this test in the general API section
	struct sockaddr_storage peer_addr;
	struct sockaddr_in6 *p6 = (struct sockaddr_in6*)&peer_addr;
	socklen_t peer_addrlen = sizeof(peer_addr);
	if ((err = GETPEERNAME(client_fd, (struct sockaddr*)&peer_addr, &peer_addrlen)) < 0) {
		perror("getpeername");
		*passed = false;
		return;
	}
	char peer_addrstr[INET6_ADDRSTRLEN];
	inet_ntop(AF_INET6, &(p6->sin6_addr), peer_addrstr, INET6_ADDRSTRLEN);
	DEBUG_TEST("getpeername() => %s : %d", peer_addrstr, ntohs(p6->sin6_port));
	r = READ(client_fd, rbuf, sizeof rbuf);
	w = WRITE(client_fd, rbuf, len);
	DEBUG_TEST("Received : %s", rbuf);
	sleep(ARTIFICIAL_SOCKET_LINGER);
	err = CLOSE(fd);
	err = CLOSE(client_fd);
	sprintf(details, "%s, err=%d, r=%d, w=%d", testname.c_str(), err, r, w);
	*passed = (w == len && r == len && !err) && !strcmp(rbuf, msg.c_str());
}





// UDP

// TEST-5
void udp_client_4(UDP_UNIT_TEST_SIG_4)
{
	std::string testname = "udp_client_4";
	std::string msg = "udp_cs_4";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "bind to interface with IPv4 address, send string until response is seen. compare.\n");
	int r, w, fd, err, len = strlen(msg.c_str());
	char rbuf[STR_SIZE];
	memset(rbuf, 0, sizeof rbuf);
	if ((fd = SOCKET(AF_INET, SOCK_DGRAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = FCNTL(fd, F_SETFL, O_NONBLOCK) < 0)) {
		fprintf(stderr, "error setting O_NONBLOCK (errno=%d)",  errno);
		perror("fcntl");
		*passed = false;
		return;
	}
	DEBUG_TEST("sending UDP packets until I get a single response...");
	if ((err = BIND(fd, (struct sockaddr *)local_addr, sizeof(struct sockaddr_in)) < 0)) {
		DEBUG_ERROR("error binding to interface (%d)", err);
		perror("bind");
		*passed = false;
		return;
	}
	struct sockaddr_storage saddr;
	while (true) {
		sleep(1);
		// tx
		if ((w = SENDTO(fd, msg.c_str(), strlen(msg.c_str()), 0, (struct sockaddr *)remote_addr, sizeof(*remote_addr))) < 0) {
			DEBUG_ERROR("error sending packet, err=%d", errno);
		}
		memset(rbuf, 0, sizeof(rbuf));
		int serverlen = sizeof(struct sockaddr_storage);
		// rx
		r = RECVFROM(fd, rbuf, STR_SIZE, 0, (struct sockaddr *)&saddr, (socklen_t *)&serverlen);
		if (r == strlen(msg.c_str())) {
			sleep(ARTIFICIAL_SOCKET_LINGER);
			err = CLOSE(fd);
			DEBUG_TEST("%s, err=%d, r=%d, w=%d", testname.c_str(), err, r, w);
			sprintf(details, "%s, err=%d, r=%d, w=%d", testname.c_str(), err, r, w);
			DEBUG_TEST("Sent     : %s", msg.c_str());
			DEBUG_TEST("Received : %s", rbuf);
			*passed = (w == len && r == len && !err) && !strcmp(rbuf, msg.c_str());
			return;
		}
	}
}





// TEST-6
void udp_server_4(UDP_UNIT_TEST_SIG_4)
{
	std::string testname = "udp_server_4";
	std::string msg = "udp_cs_4";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "bind to interface with IPv4 address, read single string, send many responses. compare.\n");
	int r, w, fd, err, len = strlen(msg.c_str());
	char rbuf[STR_SIZE];
	memset(rbuf, 0, sizeof rbuf);
	if ((fd = SOCKET(AF_INET, SOCK_DGRAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = BIND(fd, (struct sockaddr *)local_addr, sizeof(struct sockaddr_in)) < 0)) {
		DEBUG_ERROR("error binding to interface (%d)", err);  
		perror("bind");  
		*passed = false;
		return;
	}
	// rx
	DEBUG_TEST("waiting for UDP packet...");
	struct sockaddr_storage saddr;
	struct sockaddr_in *in4 = (struct sockaddr_in*)&saddr;
	int serverlen = sizeof(saddr);
	memset(&saddr, 0, sizeof(saddr));
	if ((r = RECVFROM(fd, rbuf, STR_SIZE, 0, (struct sockaddr *)in4, (socklen_t *)&serverlen)) < 0) {
		perror("recvfrom");
		*passed = false;
		return;
	}
	char addrstr[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &(in4->sin_addr), addrstr, INET_ADDRSTRLEN);
	// once we receive a UDP packet, spend 10 seconds sending responses in the hopes that the client will see
	DEBUG_TEST("received DGRAM from %s : %d", inet_ntoa(in4->sin_addr), ntohs(in4->sin_port));
	DEBUG_TEST("sending DGRAM(s) to %s : %d", inet_ntoa(remote_addr->sin_addr), ntohs(remote_addr->sin_port));
	// tx
	long int tx_ti = get_now_ts();	
	while (true) {
		sleep(1);
		if ((w = SENDTO(fd, msg.c_str(), len, 0, (struct sockaddr *)remote_addr, sizeof(*remote_addr))) < 0) {
			DEBUG_ERROR("error sending packet, err=%d", errno);
		}
		if (get_now_ts() >= tx_ti + 10000) {
			break;
		}
	}
	sleep(ARTIFICIAL_SOCKET_LINGER);
	err = CLOSE(fd);
	DEBUG_TEST("%s, err=%d, r=%d, w=%d", testname.c_str(), err, r, w);
	sprintf(details, "%s, err=%d, r=%d, w=%d", testname.c_str(), err, r, w);
	DEBUG_TEST("Sent     : %s", msg.c_str());
	DEBUG_TEST("Received : %s", rbuf);
	*passed = (w == len && r == len && !err) && !strcmp(rbuf, msg.c_str());
}





// TEST-7
void udp_client_6(UDP_UNIT_TEST_SIG_6)
{
	std::string testname = "udp_client_6";
	std::string msg = "udp_cs_6";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "bind to interface with IPv6 address, send string until response is seen. compare.\n");
	int r, w, fd, err, len = strlen(msg.c_str());
	char rbuf[STR_SIZE];
	memset(rbuf, 0, sizeof rbuf);

	if ((fd = SOCKET(AF_INET6, SOCK_DGRAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = FCNTL(fd, F_SETFL, O_NONBLOCK) < 0)) {
		std::cout << "error setting O_NONBLOCK (errno=" << strerror(errno) << ")" << std::endl;
		perror("fcntl");
		*passed = false;
		return;
	}
	DEBUG_TEST("[1] binding and sending UDP packets until I get a single response...");
	if ((err = BIND(fd, (struct sockaddr *)local_addr, sizeof(struct sockaddr_in6)) < 0)) {
		DEBUG_ERROR("error binding to interface (%d)", err);
		perror("bind");
		*passed = false;
		return;
	}

	// start sending UDP packets in the hopes that at least one will be picked up by the server
	struct sockaddr_storage saddr;
	while (true) {
		// tx
		if ((w = SENDTO(fd, msg.c_str(), len, 0, (struct sockaddr *)remote_addr, sizeof(*remote_addr))) < 0) {
			DEBUG_ERROR("error sending packet, err=%d", errno);
		}
		usleep(100000);
		memset(rbuf, 0, sizeof(rbuf));
		int serverlen = sizeof(struct sockaddr_storage);
		// rx
		r = RECVFROM(fd, rbuf, len, 0, (struct sockaddr *)&saddr, (socklen_t *)&serverlen);
		if (r == len) {
			DEBUG_TEST("[2] complete");
			sleep(ARTIFICIAL_SOCKET_LINGER);
			err = CLOSE(fd);
			DEBUG_TEST("%s, err=%d, r=%d, w=%d", testname.c_str(), err, r, w);
			sprintf(details, "%s, err=%d, r=%d, w=%d", testname.c_str(), err, r, w);
			DEBUG_TEST("Sent     : %s", msg.c_str());
			DEBUG_TEST("Received : %s", rbuf);
			*passed = (w == len && r == len && !err) && !strcmp(rbuf, msg.c_str());
			return;
		}
	}
}




// TEST-8
void udp_server_6(UDP_UNIT_TEST_SIG_6)
{
	std::string testname = "udp_server_6";
	std::string msg = "udp_cs_6";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "bind to interface with IPv6 address, read single string, send many responses. compare.\n");
	int r, w, fd, err, len = strlen(msg.c_str());
	char rbuf[STR_SIZE];
	memset(rbuf, 0, sizeof rbuf);

	if ((fd = SOCKET(AF_INET6, SOCK_DGRAM, 0)) < 0) {
		DEBUG_ERROR("error creating socket");
		perror("socket");
		*passed = false;
		return;
	}	
	if ((err = BIND(fd, (struct sockaddr *)local_addr, sizeof(struct sockaddr_in6)) < 0)) {
		DEBUG_ERROR("error binding to interface (%d)", err); 
		perror("bind");   
		*passed = false;
		return;
	}
	// rx
	DEBUG_TEST("[1/3] waiting for UDP packet to start test...");
	struct sockaddr_storage saddr;
	struct sockaddr_in6 *in6 = (struct sockaddr_in6*)&saddr;
	int serverlen = sizeof(saddr);
	memset(&saddr, 0, sizeof(saddr));
	if ((r = RECVFROM(fd, rbuf, len, 0, (struct sockaddr *)&saddr, (socklen_t *)&serverlen)) < 0) {
		perror("recvfrom");
		*passed = false;
		return;
	}
	char addrstr[INET6_ADDRSTRLEN], remote_addrstr[INET6_ADDRSTRLEN];
	inet_ntop(AF_INET6, &(in6->sin6_addr), addrstr, INET6_ADDRSTRLEN);
	inet_ntop(AF_INET6, &(remote_addr->sin6_addr), remote_addrstr, INET6_ADDRSTRLEN);
	DEBUG_TEST("[2/3] received DGRAM from %s : %d", addrstr, ntohs(in6->sin6_port));
	DEBUG_TEST("[2/3] sending DGRAM(s) to %s : %d", remote_addrstr, ntohs(remote_addr->sin6_port));
	// once we receive a UDP packet, spend 10 seconds sending responses in the hopes that the client will see
	// tx
	long int tx_ti = get_now_ts();
	while (true) {
		usleep(100000);
		//DEBUG_TEST("sending UDP packet");
		if ((w = SENDTO(fd, msg.c_str(), len, 0, (struct sockaddr *)remote_addr, sizeof(*remote_addr))) < 0) {
			DEBUG_ERROR("error sending packet, err=%d", errno);
		}
		if (get_now_ts() >= tx_ti + 10000) {
			// DEBUG_TEST("[3/4] get_now_ts()-tx_ti=%d", get_now_ts()-tx_ti);
			break;
		}
	}
	sleep(ARTIFICIAL_SOCKET_LINGER);
	err = CLOSE(fd);
	DEBUG_TEST("[3/3] complete, %s, err=%d, r=%d, w=%d", testname.c_str(), err, r, w);
	sprintf(details, "%s, err=%d, r=%d, w=%d", testname.c_str(), err, r, w);
	DEBUG_TEST("Sent     : %s", msg.c_str());
	DEBUG_TEST("Received : %s", rbuf);
	*passed = (w == len && r == len && !err) && !strcmp(rbuf, msg.c_str());
}






/****************************************************************************/
/* SUSTAINED                                                                */
/****************************************************************************/





void tcp_client_sustained_4(TCP_UNIT_TEST_SIG_4)
{
	std::string testname = "tcp_client_sustained_4";
	std::string msg = "tcp_sustained_4";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "connect to remote host with IPv4 address, exchange a sequence of packets, check order.\n");
	int n=0, w=0, r=0, fd, err;	
	char *rxbuf = (char*)malloc(cnt*sizeof(char));
	char *txbuf = (char*)malloc(cnt*sizeof(char));
	generate_random_data(txbuf, cnt, 0, 9);

	if ((fd = SOCKET(AF_INET, SOCK_STREAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = CONNECT(fd, (const struct sockaddr *)addr, sizeof(*addr))) < 0) {
		DEBUG_ERROR("error connecting to remote host (%d)", err);
		perror("connect");
		*passed = false;
		return;
	}
	if (op == TEST_OP_N_BYTES) {
		int wrem = cnt, rrem = cnt;
		// TX
		long int tx_ti = get_now_ts();	
		while (wrem) {
			int next_write = std::min(4096, wrem);
			signal(SIGPIPE, SIG_IGN);
			DEBUG_TEST("writing...");
			n = WRITE(fd, &txbuf[w], next_write);
			DEBUG_TEST("wrote=%d", n);
			if (n > 0) {
				w += n;
				wrem -= n;
				err = n;
				DEBUG_TEST("wrote=%d, w=%d, wrem=%d", n, w, wrem);
			}
		}
		long int tx_tf = get_now_ts();	
		DEBUG_TEST("wrote=%d, reading next...", w);
		// RX
		long int rx_ti = 0;	
		while (rrem) {
			n = READ(fd, &rxbuf[r], rrem);
			if (rx_ti == 0) { // wait for first message
				rx_ti = get_now_ts();	
			}
			if (n > 0) {
				r += n;
				rrem -= n;
				err = n;
			}
		}
		long int rx_tf = get_now_ts();	
		DEBUG_TEST("read=%d", r);
		sleep(ARTIFICIAL_SOCKET_LINGER);
		err = CLOSE(fd);
		// Compare RX and TX buffer and detect mismatches
		bool match = true;
		for (int i=0; i<cnt; i++) {
			if (rxbuf[i] != txbuf[i]) {
				DEBUG_ERROR("buffer mismatch found at idx=%d", i);
				match=false;
			}
		}
		// Compute time deltas and transfer rates
		float tx_dt = (tx_tf - tx_ti) / (float)1000;
		float rx_dt = (rx_tf - rx_ti) / (float)1000;
		float tx_rate = (float)cnt / (float)tx_dt;
		float rx_rate = (float)cnt / (float)rx_dt;
		sprintf(details, "%s, match=%d, n=%d, tx_dt=%.2f, rx_dt=%.2f, r=%d, w=%d, tx_rate=%.2f MB/s, rx_rate=%.2f MB/s", 
			testname.c_str(), match, cnt, tx_dt, rx_dt, r, w, (tx_rate / float(ONE_MEGABYTE) ), (rx_rate / float(ONE_MEGABYTE) ));	
		*passed = (r == cnt && w == cnt && match && err>=0);
	}
	free(rxbuf);
	free(txbuf);
}





void tcp_client_sustained_6(TCP_UNIT_TEST_SIG_6)
{
	std::string testname = "tcp_client_sustained_6";
	std::string msg = "tcp_sustained_6";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "connect to remote host with IPv6 address, exchange a sequence of packets, check order.\n");
	int n=0, w=0, r=0, fd, err;	
	char *rxbuf = (char*)malloc(cnt*sizeof(char));
	char *txbuf = (char*)malloc(cnt*sizeof(char));
	generate_random_data(txbuf, cnt, 0, 9);
	if ((fd = SOCKET(AF_INET6, SOCK_STREAM, 0)) < 0){
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = CONNECT(fd, (const struct sockaddr *)addr, sizeof(*addr))) < 0) {
		DEBUG_ERROR("error connecting to remote host (%d)", err);
		perror("connect");
		*passed = false;
		return;
	}

	if (op == TEST_OP_N_BYTES) {
		int wrem = cnt, rrem = cnt;
		// TX
		long int tx_ti = get_now_ts();	
		while (wrem) {
			int next_write = std::min(4096, wrem);
			n = WRITE(fd, &txbuf[w], next_write);
			if (n > 0) {
				w += n;
				wrem -= n;
				err = n;
			}
		}
		long int tx_tf = get_now_ts();	
		DEBUG_TEST("wrote=%d", w);
		// RX
		long int rx_ti = 0;	
		while (rrem) {
			n = READ(fd, &rxbuf[r], rrem);
			if (rx_ti == 0) { // wait for first message
				rx_ti = get_now_ts();	
			}
			if (n > 0) {
				r += n;
				rrem -= n;
				err = n;
			}
		}
		long int rx_tf = get_now_ts();	
		DEBUG_TEST("read=%d", r);
		sleep(ARTIFICIAL_SOCKET_LINGER);
		err = CLOSE(fd);
		// Compare RX and TX buffer and detect mismatches
		bool match = true;
		for (int i=0; i<cnt; i++) {
			if (rxbuf[i] != txbuf[i]) {
				DEBUG_ERROR("buffer mismatch found at idx=%d", i);
				match=false;
			}
		}
		// Compute time deltas and transfer rates
		float tx_dt = (tx_tf - tx_ti) / (float)1000;
		float rx_dt = (rx_tf - rx_ti) / (float)1000;
		float tx_rate = (float)cnt / (float)tx_dt;
		float rx_rate = (float)cnt / (float)rx_dt;
		sprintf(details, "%s, match=%d, n=%d, tx_dt=%.2f, rx_dt=%.2f, r=%d, w=%d, tx_rate=%.2f MB/s, rx_rate=%.2f MB/s", 
			testname.c_str(), match, cnt, tx_dt, rx_dt, r, w, (tx_rate / float(ONE_MEGABYTE) ), (rx_rate / float(ONE_MEGABYTE) ));	
		*passed = (r == cnt && w == cnt && match && err>=0);
	}
	free(rxbuf);
	free(txbuf);
}





void tcp_server_sustained_4(TCP_UNIT_TEST_SIG_4)
{
	std::string testname = "tcp_server_sustained_4";
	std::string msg = "tcp_sustained_4";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "accept connection from host with IPv4 address, exchange a sequence of packets, check order.\n");
	int n=0, w=0, r=0, fd, client_fd, err;
	char *rxbuf = (char*)malloc(cnt*sizeof(char));
	memset(rxbuf, 0, cnt);

	if ((fd = SOCKET(AF_INET, SOCK_STREAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = BIND(fd, (struct sockaddr *)addr, (socklen_t)sizeof(*addr)) < 0)) {
		DEBUG_ERROR("error binding to interface (%d)", err);
		perror("bind");
		*passed = false;
		return;
	}
	if ((err = LISTEN(fd, 1)) < 0) {
		DEBUG_ERROR("error placing socket in LISTENING state (%d)", err);
		perror("listen");
		*passed = false;
		return;
	}
	struct sockaddr_storage client;
	struct sockaddr_in *in4 = (struct sockaddr_in*)&client;
	socklen_t client_addrlen = sizeof(sockaddr_storage);
	if ((client_fd = ACCEPT(fd, (struct sockaddr *)in4, &client_addrlen)) < 0) {
		fprintf(stderr,"error accepting connection (%d)\n", err);
		perror("accept");
	}
	DEBUG_TEST("accepted connection from %s, on port %d", inet_ntoa(in4->sin_addr), ntohs(in4->sin_port));
	if (op == TEST_OP_N_BYTES) {
		int wrem = cnt, rrem = cnt;
		long int rx_ti = 0;
		while (rrem) {
			n = READ(client_fd, &rxbuf[r], rrem);
			if (n > 0) {
				if (rx_ti == 0) { // wait for first message
					rx_ti = get_now_ts();	
				}
				r += n;
				rrem -= n;
				err = n;
				DEBUG_TEST("read=%d, r=%d, rrem=%d", n, r, rrem);
			}
		}
		long int rx_tf = get_now_ts();	
		DEBUG_TEST("read=%d, writing next...", r);
		long int tx_ti = get_now_ts();	
		while (wrem) {
			int next_write = std::min(1024, wrem);
			n = WRITE(client_fd, &rxbuf[w], next_write);
			if (n > 0) {	
				w += n;
				wrem -= n;
				err = n;
			}
		}
		long int tx_tf = get_now_ts();	
		DEBUG_TEST("wrote=%d", w);
		sleep(ARTIFICIAL_SOCKET_LINGER);
		err = CLOSE(fd);
		err = CLOSE(client_fd);
		// Compute time deltas and transfer rates
		float tx_dt = (tx_tf - tx_ti) / (float)1000;
		float rx_dt = (rx_tf - rx_ti) / (float)1000;
		float tx_rate = (float)cnt / (float)tx_dt;
		float rx_rate = (float)cnt / (float)rx_dt;
		sprintf(details, "%s, n=%d, tx_dt=%.2f, rx_dt=%.2f, r=%d, w=%d, tx_rate=%.2f MB/s, rx_rate=%.2f MB/s", 
			testname.c_str(), cnt, tx_dt, rx_dt, r, w, (tx_rate / float(ONE_MEGABYTE) ), (rx_rate / float(ONE_MEGABYTE) ));
		*passed = (r == cnt && w == cnt && err>=0);
	}
	free(rxbuf);
}





void tcp_server_sustained_6(TCP_UNIT_TEST_SIG_6)
{
	std::string testname = "tcp_server_sustained_6";
	std::string msg = "tcp_sustained_6";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "accept connection from host with IPv6 address, exchange a sequence of packets, check order.\n");
	int n=0, w=0, r=0, fd, client_fd, err;
	char *rxbuf = (char*)malloc(cnt*sizeof(char));
	memset(rxbuf, 0, cnt);

	if ((fd = SOCKET(AF_INET6, SOCK_STREAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = BIND(fd, (struct sockaddr *)addr, (socklen_t)sizeof(struct sockaddr_in6)) < 0)) {
		DEBUG_ERROR("error binding to interface (%d)", err);
		perror("bind");
		*passed = false;
		return;
	}
	if ((err = LISTEN(fd, 1)) < 0) {
		DEBUG_ERROR("error placing socket in LISTENING state (%d)", err);
		perror("listen");
		*passed = false;
		return;
	}
	struct sockaddr_in6 client;
	socklen_t client_addrlen = sizeof(sockaddr_in6);
	if ((client_fd = ACCEPT(fd, (struct sockaddr *)&client, &client_addrlen)) < 0) {
		fprintf(stderr,"error accepting connection (%d)\n", err);
		perror("accept");
		*passed = false;
		return;
	}
	char ipstr[INET6_ADDRSTRLEN];
	inet_ntop(AF_INET6, &client.sin6_addr, ipstr, sizeof ipstr);
	DEBUG_TEST("accepted connection from %s, on port %d", ipstr, ntohs(client.sin6_port));

	if (op == TEST_OP_N_BYTES) {
		int wrem = cnt, rrem = cnt;
		long int rx_ti = 0;
		while (rrem) {
			n = READ(client_fd, &rxbuf[r], rrem);
			if (n > 0) {
				if (rx_ti == 0) { // wait for first message
					rx_ti = get_now_ts();	
				}
				r += n;
				rrem -= n;
				err = n;
			}
		}
		long int rx_tf = get_now_ts();	
		DEBUG_TEST("read=%d", r);
		long int tx_ti = get_now_ts();	
		while (wrem) {
			int next_write = std::min(1024, wrem);
			n = WRITE(client_fd, &rxbuf[w], next_write);
			if (n > 0) {	
				w += n;
				wrem -= n;
				err = n;
			}
		}
		long int tx_tf = get_now_ts();	
		DEBUG_TEST("wrote=%d", w);
		sleep(ARTIFICIAL_SOCKET_LINGER);
		err = CLOSE(fd);
		err = CLOSE(client_fd);
		// Compute time deltas and transfer rates
		float tx_dt = (tx_tf - tx_ti) / (float)1000;
		float rx_dt = (rx_tf - rx_ti) / (float)1000;
		float tx_rate = (float)cnt / (float)tx_dt;
		float rx_rate = (float)cnt / (float)rx_dt;
		sprintf(details, "%s, n=%d, tx_dt=%.2f, rx_dt=%.2f, r=%d, w=%d, tx_rate=%.2f MB/s, rx_rate=%.2f MB/s", 
			testname.c_str(), cnt, tx_dt, rx_dt, r, w, (tx_rate / float(ONE_MEGABYTE) ), (rx_rate / float(ONE_MEGABYTE) ));

		*passed = (r == cnt && w == cnt && err>=0);
	}
	free(rxbuf);
}





void udp_client_sustained_4(UDP_UNIT_TEST_SIG_4)
{
	std::string testname = "udp_client_sustained_4";
	std::string msg = "udp_sustained_4";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "bind to interface with IPv4 address, TX n-datagrams\n");
	int w, fd, err, len = strlen(msg.c_str());
	char rbuf[STR_SIZE];
	memset(rbuf, 0, sizeof rbuf);
	if ((fd = SOCKET(AF_INET, SOCK_DGRAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = FCNTL(fd, F_SETFL, O_NONBLOCK) < 0)) {
		fprintf(stderr, "error setting O_NONBLOCK (errno=%d)",  errno);
		perror("fcntl");
		*passed = false;
		return;
	}
	DEBUG_TEST("sending UDP packets until I get a single response...");
	if ((err = BIND(fd, (struct sockaddr *)local_addr, sizeof(struct sockaddr_in)) < 0)) {
		DEBUG_ERROR("error binding to interface (%d)", err);
		perror("bind");
		*passed = false;
		return;
	}
	int num_to_send = 10;
	for (int i=0; i<num_to_send; i++) {
		// tx
		if ((w = SENDTO(fd, msg.c_str(), strlen(msg.c_str()), 0, (struct sockaddr *)remote_addr, sizeof(*remote_addr))) < 0) {
			DEBUG_ERROR("error sending packet, err=%d", errno);
		}
	}
	sleep(ARTIFICIAL_SOCKET_LINGER);
	err = CLOSE(fd);
	DEBUG_TEST("%s, n=%d, err=%d, w=%d", testname.c_str(), cnt, err, w);
	sprintf(details, "%s, n=%d, err=%d, w=%d", testname.c_str(), cnt, err, w);
	DEBUG_TEST("Sent     : %s", msg.c_str());
	*passed = (w == len && !err);
	return;	
}





void udp_server_sustained_4(UDP_UNIT_TEST_SIG_4)
{
	std::string testname = "udp_server_sustained_4";
	std::string msg = "udp_sustained_4";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "bind to interface with IPv4 address, RX (n/x)-datagrams\n");
	int r, fd, err, len = strlen(msg.c_str());
	char rbuf[STR_SIZE];
	memset(rbuf, 0, sizeof rbuf);
	if ((fd = SOCKET(AF_INET, SOCK_DGRAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = BIND(fd, (struct sockaddr *)local_addr, sizeof(struct sockaddr_in)) < 0)) {
		DEBUG_ERROR("error binding to interface (%d)", err);    
		perror("bind");
		*passed = false;
		return;
	}
	int num_to_recv = 3;
	DEBUG_TEST("waiting for UDP packet...");
	for (int i=0; i<num_to_recv; i++) {
		// rx
		struct sockaddr_storage saddr;
		struct sockaddr_in *in4 = (struct sockaddr_in*)&saddr;
		int serverlen = sizeof(saddr);
		memset(&saddr, 0, sizeof(saddr));
		r = RECVFROM(fd, rbuf, STR_SIZE, 0, (struct sockaddr *)in4, (socklen_t *)&serverlen);
		char addrstr[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &(in4->sin_addr), addrstr, INET_ADDRSTRLEN);
		// once we receive a UDP packet, spend 10 seconds sending responses in the hopes that the client will see
		DEBUG_TEST("received DGRAM from %s : %d", inet_ntoa(in4->sin_addr), ntohs(in4->sin_port));
		DEBUG_TEST("sending DGRAM(s) to %s : %d", inet_ntoa(remote_addr->sin_addr), ntohs(remote_addr->sin_port));
	}
	sleep(ARTIFICIAL_SOCKET_LINGER);
	//err = CLOSE(fd);
	DEBUG_TEST("%s, n=%d, err=%d, r=%d", testname.c_str(), cnt, err, r);
	sprintf(details, "%s, n=%d, err=%d, r=%d", testname.c_str(), cnt, err, r);
	DEBUG_TEST("Received : %s", rbuf);
	*passed = (r == len && !err) && !strcmp(rbuf, msg.c_str());
}





void udp_client_sustained_6(UDP_UNIT_TEST_SIG_6)
{
	std::string testname = "udp_client_sustained_6";
	std::string msg = "udp_sustained_6";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "bind to interface with IPv6 address, TX n-datagrams\n");
	int w, fd, err, len = strlen(msg.c_str());
	char rbuf[STR_SIZE];
	memset(rbuf, 0, sizeof rbuf);
	if ((fd = SOCKET(AF_INET6, SOCK_DGRAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = FCNTL(fd, F_SETFL, O_NONBLOCK) < 0)) {
		fprintf(stderr, "error setting O_NONBLOCK (errno=%d)",  errno);
		perror("fcntl");
		*passed = false;
		return;
	}
	DEBUG_TEST("sending UDP packets until I get a single response...");
	if ((err = BIND(fd, (struct sockaddr *)local_addr, sizeof(struct sockaddr_in6)) < 0)) {
		DEBUG_ERROR("error binding to interface (%d)", err);
		perror("bind");
		*passed = false;
		return;
	}
	int num_to_send = 10;
	for (int i=0; i<num_to_send; i++) {
		sleep(1);
		// tx
		if ((w = SENDTO(fd, msg.c_str(), strlen(msg.c_str()), 0, (struct sockaddr *)remote_addr, sizeof(*remote_addr))) < 0) {
			DEBUG_ERROR("error sending packet, err=%d", errno);
		}
	}
	sleep(ARTIFICIAL_SOCKET_LINGER);
	err = CLOSE(fd);
	DEBUG_TEST("%s, n=%d, err=%d, w=%d", testname.c_str(), cnt, err, w);
	sprintf(details, "%s, n=%d, err=%d, w=%d", testname.c_str(), cnt, err, w);
	DEBUG_TEST("Sent     : %s", msg.c_str());
	*passed = (w == len && !err);
	return;	
}





void udp_server_sustained_6(UDP_UNIT_TEST_SIG_6)
{
	std::string testname = "udp_server_sustained_6";
	std::string msg = "udp_sustained_6";
	fprintf(stderr, "\n\n%s (ts=%lu)\n", testname.c_str(), get_now_ts);
	fprintf(stderr, "bind to interface with IPv6 address, RX (n/x)-datagrams\n");
	int r, fd, err, len = strlen(msg.c_str());
	char rbuf[STR_SIZE];
	memset(rbuf, 0, sizeof rbuf);
	if ((fd = SOCKET(AF_INET6, SOCK_DGRAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		perror("socket");
		*passed = false;
		return;
	}
	if ((err = BIND(fd, (struct sockaddr *)local_addr, sizeof(struct sockaddr_in6)) < 0)) {
		DEBUG_ERROR("error binding to interface (%d)", err);    
		perror("bind");
		*passed = false;
		return;
	}
	int num_to_recv = 3;
	DEBUG_TEST("waiting for UDP packet...");
	for (int i=0; i<num_to_recv; i++) {
		// rx
		struct sockaddr_storage saddr;
		struct sockaddr_in6 *in6 = (struct sockaddr_in6*)&saddr;
		int serverlen = sizeof(saddr);
		memset(&saddr, 0, sizeof(saddr));
		r = RECVFROM(fd, rbuf, STR_SIZE, 0, (struct sockaddr *)in6, (socklen_t *)&serverlen);
		char addrstr[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &(in6->sin6_addr), addrstr, INET6_ADDRSTRLEN);
		// once we receive a UDP packet, spend 10 seconds sending responses in the hopes that the client will see
		//DEBUG_TEST("received DGRAM from %s : %d", inet_ntoa(in6->sin6_addr), ntohs(in6->sin6_port));
		//DEBUG_TEST("sending DGRAM(s) to %s : %d", inet_ntoa(remote_addr->sin6_addr), ntohs(remote_addr->sin6_port));
	}
	sleep(ARTIFICIAL_SOCKET_LINGER);
	err = CLOSE(fd);
	DEBUG_TEST("%s, n=%d, err=%d, r=%d", testname.c_str(), cnt, err, r);
	sprintf(details, "%s, n=%d, err=%d, r=%d", testname.c_str(), cnt, err, r);
	DEBUG_TEST("Received : %s", rbuf);
	*passed = (r == len && !err) && !strcmp(rbuf, msg.c_str());
}

/****************************************************************************/
/* PERFORMANCE (between library instances)                                  */
/****************************************************************************/

// Maintain transfer for cnt OR cnt
void tcp_client_perf_4(TCP_UNIT_TEST_SIG_4)
{
	fprintf(stderr, "\n\n\ntcp_client_perf_4\n");
	/*
	int w=0, fd, err;
	int total_test_sz          = cnt;
	int arbitrary_chunk_sz_max = MAX_RX_BUF_SZ;
	int arbitrary_chunk_sz_min = 512;

	char rbuf[arbitrary_chunk_sz_max];

	for (int i=arbitrary_chunk_sz_min; (i*2) < arbitrary_chunk_sz_max; i*=2) {

		if ((fd = SOCKET(AF_INET, SOCK_STREAM, 0)) < 0)
			DEBUG_ERROR("error creating ZeroTier socket");
		if ((err = CONNECT(fd, (const struct sockaddr *)addr, sizeof(addr))) < 0)
			DEBUG_ERROR("error connecting to remote host (%d)", err);

		DEBUG_TEST("[TX] Testing (%d) byte chunks: ", i);

		int chunk_sz   = i;
		long int start_time = get_now_ts();
		w = 0;

		// TX
		while (w < total_test_sz)
			w += WRITE(fd, rbuf, chunk_sz);
		
		long int end_time = get_now_ts();
		float ts_delta = (end_time - start_time) / (float)1000;
		float rate = (float)total_test_sz / (float)ts_delta;
		sprintf(details, "tot=%d, dt=%.2f, rate=%.2f MB/s", w, ts_delta, (rate / float(ONE_MEGABYTE) ));
		CLOSE(fd);		
	}	
	*passed = (w == total_test_sz && !err) ? PASSED : FAILED;
	*/
}

// Maintain transfer for cnt OR cnt
void tcp_server_perf_4(TCP_UNIT_TEST_SIG_4)
{
	fprintf(stderr, "\n\n\ntcp_server_perf_4\n");
	/*
	int r=0, fd, client_fd, err;
	int total_test_sz          = cnt;
	int arbitrary_chunk_sz_max = MAX_RX_BUF_SZ;
	int arbitrary_chunk_sz_min = 512;

	char rbuf[arbitrary_chunk_sz_max];

	for (int i=arbitrary_chunk_sz_min; (i*2) < arbitrary_chunk_sz_max; i*=2) {
		DEBUG_ERROR("TESTING chunk size = %d", i);
		if ((fd = SOCKET(AF_INET, SOCK_STREAM, 0)) < 0)
			DEBUG_ERROR("error creating ZeroTier socket");
		if ((err = BIND(fd, (struct sockaddr *)addr, (socklen_t)sizeof(struct sockaddr_in)) < 0))
			DEBUG_ERROR("error binding to interface (%d)", err);
		if ((err = LISTEN(fd, 1)) < 0)
			DEBUG_ERROR("error placing socket in LISTENING state (%d)", err);
		if ((client_fd = ACCEPT(fd, (struct sockaddr *)&addr, (socklen_t *)sizeof(addr))) < 0)
			DEBUG_ERROR("error accepting connection (%d)", err);

		DEBUG_TEST("[RX] Testing (%d) byte chunks: ", i);

		int chunk_sz   = i;
		long int start_time = get_now_ts();
		r = 0;

		// RX
		while (r < total_test_sz)
			r += READ(client_fd, rbuf, chunk_sz);
		
		long int end_time = get_now_ts();

		float ts_delta = (end_time - start_time) / (float)1000;
		float rate = (float)total_test_sz / (float)ts_delta;

		sprintf(details, "tot=%d, dt=%.2f, rate=%.2f MB/s", r, ts_delta, (rate / float(ONE_MEGABYTE) ));		

		CLOSE(fd);
		CLOSE(client_fd);
	}
	*passed = (r == total_test_sz && !err) ? PASSED : FAILED;
	*/
}





/****************************************************************************/
/* PERFORMANCE (between library and native)                                 */
/****************************************************************************/

void tcp_perf_tx_echo_4(TCP_UNIT_TEST_SIG_4)
{
	std::string msg = "tcp_perf_tx_echo_4";
	fprintf(stderr, "\n\n%s\n\n", msg.c_str());

	int err   = 0;
	int tot   = 0;
	int w     = 0;
	int fd, mode;

	char pbuf[64]; // test parameter buffer
	char tbuf[MAX_TX_BUF_SZ];

	mode = ECHOTEST_MODE_TX;

	// connect to remote echotest host
	if ((fd = SOCKET(AF_INET, SOCK_STREAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		return;
	}
	if ((err = CONNECT(fd, (const struct sockaddr *)addr, sizeof(*addr))) < 0) {
		DEBUG_ERROR("error connecting to remote host (%d)", err);
		return;
	}

	DEBUG_TEST("copying test parameters to buffer");
	memset(pbuf, 0, sizeof pbuf);
	memcpy(pbuf, &mode, sizeof mode);
	memcpy(pbuf + sizeof mode, &cnt, sizeof cnt);

	DEBUG_TEST("sending test parameters to echotest");
	if ((w = WRITE(fd, pbuf, sizeof pbuf)) < 0) {
		DEBUG_ERROR("error while sending test parameters to echotest (err=%d)", w);
		return;
	}

	// begin
	DEBUG_TEST("beginning test, sending test byte stream...");
	while (tot < cnt) {
		if ((w = WRITE(fd, tbuf, sizeof tbuf)) < 0) {
			DEBUG_ERROR("error while sending test byte stream to echotest (err=%d)", w);
			return;
		}
		tot += w;
		DEBUG_TEST("tot=%d, sent=%d", tot, w);
	}
	// read results
	memset(pbuf, 0, sizeof pbuf);
	DEBUG_TEST("reading test results from echotest");
	if ((w = READ(fd, pbuf, sizeof tbuf)) < 0) {
		DEBUG_ERROR("error while reading results from echotest (err=%d)", w);
		return;
	}

	DEBUG_TEST("reading test results");
	long int start_time = 0, end_time = 0;
	memcpy(&start_time, pbuf, sizeof start_time);
	memcpy(&end_time, pbuf + sizeof start_time, sizeof end_time);

	float ts_delta = (end_time - start_time) / (float)1000;
	float rate = (float)tot / (float)ts_delta;
	sprintf(details, "%s, tot=%d, dt=%.2f, rate=%.2f MB/s", msg.c_str(), tot, ts_delta, (rate / float(ONE_MEGABYTE) ));

	sleep(ARTIFICIAL_SOCKET_LINGER);
	err = CLOSE(fd);
	*passed = (tot == cnt && !err) ? PASSED : FAILED;
}


void tcp_perf_rx_echo_4(TCP_UNIT_TEST_SIG_4)
{
	std::string msg = "tcp_perf_rx_echo_4";
	fprintf(stderr, "\n\n%s\n\n", msg.c_str());

	int err   = 0;
	int mode  = 0;
	int tot   = 0;
	int r     = 0;
	
	char pbuf[64]; // test parameter buffer
	char tbuf[MAX_TX_BUF_SZ];
	int fd;

	mode = ECHOTEST_MODE_RX;

	// connect to remote echotest host
	if ((fd = SOCKET(AF_INET, SOCK_STREAM, 0)) < 0) {
		DEBUG_ERROR("error creating ZeroTier socket");
		return;
	}
	if ((err = CONNECT(fd, (const struct sockaddr *)addr, sizeof(*addr))) < 0) {
		DEBUG_ERROR("error connecting to remote host (%d)", err);
		return;
	}

	DEBUG_TEST("copying test parameters to buffer");
	memset(pbuf, 0, sizeof pbuf);
	memcpy(pbuf, &mode, sizeof mode);
	memcpy(pbuf + sizeof mode, &cnt, sizeof cnt);

	DEBUG_TEST("sending test parameters to echotest");
	if ((r = WRITE(fd, pbuf, sizeof pbuf)) < 0) {
		DEBUG_ERROR("error while sending test parameters to echotest (err=%d)", r);
		return;
	}

	// begin
	DEBUG_TEST("beginning test, as soon as bytes are read we will start keeping time...");
	if ((r = read(fd, tbuf, sizeof tbuf)) < 0) {
		DEBUG_ERROR("there was an error reading the test stream. aborting (err=%d, errno=%s)", r, strerror(errno));
		return;
	}

	tot += r;

	long int start_time = get_now_ts();	
	DEBUG_TEST("Received first set of bytes in test stream. now keeping time");

	while (tot < cnt) {
		if ((r = read(fd, tbuf, sizeof tbuf)) < 0) {
			DEBUG_ERROR("there was an error reading the test stream. aborting (err=%d)", r);
			return;
		}
		tot += r;
		DEBUG_TEST("r=%d, tot=%d", r, tot);
	}
	long int end_time = get_now_ts();	
	float ts_delta = (end_time - start_time) / (float)1000;
	float rate = (float)tot / (float)ts_delta;
	sprintf(details, "%s, tot=%d, dt=%.2f, rate=%.2f MB/s", msg.c_str(), tot, ts_delta, (rate / float(ONE_MEGABYTE) ));		
	
	sleep(ARTIFICIAL_SOCKET_LINGER);
	err = CLOSE(fd);
	*passed = (tot == cnt && !err) ? PASSED : FAILED;
}




/****************************************************************************/
/* OBSCURE API CALL TESTS                                                   */
/****************************************************************************/

int obscure_api_test(bool *passed)
{
	int err = -1;
	fprintf(stderr, "\n\nobscure API test\n\n");	

	/*
	// ---
	// getpeername()
	int fd, client_fd;

	// after accept()
	if ((fd = SOCKET(AF_INET, SOCK_STREAM, 0)) < 0)
		DEBUG_ERROR("error creating ZeroTier socket");
	if ((err = BIND(fd, (struct sockaddr *)addr, sizeof(struct sockaddr_in)) < 0))
		DEBUG_ERROR("error binding to interface (%d)", err);
	if ((err = LISTEN(fd, 100)) < 0)
		printf("error placing socket in LISTENING state (%d)", err);
	// accept
	struct sockaddr_in client;
	socklen_t client_addrlen = sizeof(sockaddr_in);
	if ((client_fd = accept(fd, (struct sockaddr *)&client, &client_addrlen)) < 0)
		fprintf(stderr,"error accepting connection (%d)\n", err);
	fprintf(stderr, "accepted connection from %s, on port %d", inet_ntoa(client.sin_addr), ntohs(client.sin_port));
	// getpeername
	struct sockaddr_storage peer_addr;
	struct sockaddr_in *in4 = (struct sockaddr_in*)&peer_addr;
	socklen_t peer_addrlen = sizeof(peer_addr);
	GETPEERNAME(fd, (struct sockaddr*)&peer_addr, &peer_addrlen);
	DEBUG_TEST("getpeername() => %s : %d", inet_ntoa(in4->sin_addr), ntohs(in4->sin_port));
	// compate getpeername() result to address returned by accept()

	// after connect
	if ((fd = SOCKET(AF_INET, SOCK_STREAM, 0)) < 0)
		DEBUG_ERROR("error creating ZeroTier socket");
	if ((err = CONNECT(fd, (const struct sockaddr *)addr, sizeof(*addr))) < 0)
		DEBUG_ERROR("error connecting to remote host (%d)", err);
	// TODO: Put this test in the general API section
	struct sockaddr_storage peer_addr;
	struct sockaddr_in *in4 = (struct sockaddr_in*)&peer_addr;
	socklen_t peer_addrlen = sizeof(peer_addr);
	GETPEERNAME(fd, (struct sockaddr*)&peer_addr, &peer_addrlen);
	DEBUG_TEST("getpeername() => %s : %d", inet_ntoa(in4->sin_addr), ntohs(in4->sin_port));
	// compare result of getpeername to remote address

	// TODO: write an ipv6 version of the above ^^^
	*/
/*
int levels[] = {
		IPPROTO_TCP, 
		IPPROTO_UDP,
		IPPROTO_IP
	};
	int num_levels = sizeof(levels) / sizeof(int);

	int optnames[] = {
		TCP_NODELAY,
		SO_LINGER
	};
	int num_optnames = sizeof(optnames) / sizeof(int);


	for (int i=0; i<num_levels; i++) { // test all levels
		for (int j=0; j<num_optnames; j++) { // test all optnames

		// ---
		// Disable Nagle's Algorithm on a socket (TCP_NODELAY)
		int level = IPPROTO_TCP;
		int optname = TCP_NODELAY;
		int optval = 1;
		socklen_t flag_len = sizeof(optval);
		int fd = SOCKET(AF_INET, SOCK_STREAM, 0);
		DEBUG_TEST("setting level=%d, optname=%d, optval=%d...", level, optname, optval);
		err = SETSOCKOPT(fd, level, optname, (char *)&optval, sizeof(int));
		if (err < 0) {
			DEBUG_ERROR("error while setting optval on socket");
			*passed = false;
			err = -1;
		}
		optval = -99; // set junk value to test against
		if ((err = GETSOCKOPT(fd, level, optname, &optval, &flag_len)) < 0) {
			DEBUG_ERROR("error while getting the optval");
			*passed = false;
			err = -1;
		}
		DEBUG_TEST("flag_len=%d", flag_len);
		if (optval <= 0) {
			DEBUG_ERROR("incorrect optval=%d (from getsockopt)", optval);
			*passed = false;
			err = -1;
		} else {
			DEBUG_TEST("correctly read optval=%d, now reversing it", optval);
			if (optval > 0) { // TODO: what should be expected for each platform? Should this mirror them?
				optval = 0;
				DEBUG_TEST("setting level=%d, optname=%d, optval=%d...", level, optname, optval);
				if ((err = SETSOCKOPT(fd, level, optname, (char *) &optval, (socklen_t)sizeof(int))) < 0) {
					DEBUG_ERROR("error while setting on socket");
					*passed = false;
					err = -1;
				}
				else {
					DEBUG_TEST("success");
					*passed = true;
				}
			} else {
				DEBUG_ERROR("the optval wasn't set correctly");
				*passed = false;
				err = -1;
			}
		}
	}
	*/
	return err;
}

/****************************************************************************/
/* SLAM API (multiple of each api call and/or plausible call sequence)      */
/****************************************************************************/

#if defined(__SELFTEST__)

#define SLAM_NUMBER 16
#define SLAM_REPEAT 1

int slam_api_test()
{
	int err = 0;
	int results[SLAM_NUMBER*SLAM_REPEAT];

	struct hostent *server;
	struct sockaddr_in6 addr6;
	struct sockaddr_in addr;

	// int start_stack_timer_cnt = pico_ntimers(); // number of picoTCP timers allocated

	// TESTS:
	// socket()
	// close()
	if (false)
	{
		// open and close SLAM_NUMBER*SLAM_REPEAT sockets
		for (int j=0; j<SLAM_REPEAT; j++) {
			std::cout << "slamming " << j << " time(s)" << std::endl;
			usleep(SLAM_INTERVAL);
			// create sockets
			int fds[SLAM_NUMBER];
			for (int i = 0; i<SLAM_NUMBER; i++) {
				if ((err = SOCKET(AF_INET, SOCK_STREAM, 0)) < 0) {
					std::cout << "error creating socket (errno = " << strerror(errno) << ")" << std::endl;
					if (errno == EMFILE)
						break;
					else
						return -1;
				}
				else
					fds[i] = err;
				std::cout << "\tcreating " << i << " socket(s) fd = " << err << std::endl;

			}
			// close sockets
			for (int i = 0; i<SLAM_NUMBER; i++) {
				//std::cout << "\tclosing " << i << " socket(s)" << std::endl;
				if ((err = CLOSE(fds[i])) < 0) {
					std::cout << "error closing socket (errno = " << strerror(errno) << ")" << std::endl;
					//return -1;
				}
				else
					fds[i] = -1;
			}
		}
		//if (zts_num_active_virt_sockets() == 0)
		//	std::cout << "PASSED [slam open and close]" << std::endl;
		//else
		//	std::cout << "FAILED [slam open and close] - sockets left unclosed" << std::endl;
	}

	// ---

	// TESTS:
	// socket()
	// bind()
	// listen()
	// accept()
	// close()
	if (false) 
	{
		int sock = 0;
		std::vector<int> used_ports;

		for (int j=0; j<SLAM_REPEAT; j++) {
			std::cout << "slamming " << j << " time(s)" << std::endl;
			usleep(SLAM_INTERVAL);

			for (int i = 0; i<SLAM_NUMBER; i++) {
				if ((sock = SOCKET(AF_INET, SOCK_STREAM, 0)) < 0) {
					std::cout << "error creating socket (errno = " << strerror(errno) << ")" << std::endl;
					if (errno == EMFILE)
						break;
					else
						return -1;
				}
				std::cout << "socket() = " << sock << std::endl;
				usleep(SLAM_INTERVAL);

				int port;
				while ((std::find(used_ports.begin(),used_ports.end(),port) == used_ports.end()) == false) {
					port = MIN_PORT + (rand() % (int)(MAX_PORT - MIN_PORT + 1));
				}
				used_ports.push_back(port);
				std::cout << "port = " << port << std::endl;
				
				if (false) {
					server = gethostbyname2("::",AF_INET6);
					memset((char *) &addr6, 0, sizeof(addr6));
					addr6.sin6_flowinfo = 0;
					addr6.sin6_family = AF_INET6;
					addr6.sin6_port = htons(port);
					addr6.sin6_addr = in6addr_any;
					err = BIND(sock, (struct sockaddr *)&addr6, (socklen_t)(sizeof addr6));
				}

				if (true) {
					addr.sin_port = htons(port);
					addr.sin_addr.s_addr = inet_addr("10.9.9.50");
					//addr.sin_addr.s_addr = htons(INADDR_ANY);
					addr.sin_family = AF_INET;
					err = BIND(sock, (struct sockaddr *)&addr, (socklen_t)(sizeof addr));
				}
				if (err < 0) {
					std::cout << "error binding socket (errno = " << strerror(errno) << ")" << std::endl;
					return -1;
				}
				
				if (sock > 0) {
					if ((err = CLOSE(sock)) < 0) {
						std::cout << "error closing socket (errno = " << strerror(errno) << ")" << std::endl;
						//return -1;
					}
				}
			}
		}
		used_ports.clear();
		//if (zts_num_active_virt_sockets() == 0)
		//	std::cout << "PASSED [slam open, bind, listen, accept, close]" << std::endl;
		//else
		//	std::cout << "FAILED [slam open, bind, listen, accept, close]" << std::endl;
	}

	// TESTS:
	// (1) socket()
	// (2) connect()
	// (3) close()
	int num_times = 3;//zts_maxsockets(SOCK_STREAM);
	std::cout << "socket/connect/close - " << num_times << " times" << std::endl;
	for (int i=0;i<(SLAM_NUMBER*SLAM_REPEAT); i++) { results[i] = 0; }
	if (true) 
	{
		int port = 4545;
		
		// open, bind, listen, accept, close
		for (int j=0; j<num_times; j++) {
			int sock = 0;
			errno = 0;

			usleep(SLAM_INTERVAL);

			// socket()
			printf("creating socket... (%d)\n", j);
			if ((sock = SOCKET(AF_INET, SOCK_STREAM, 0)) < 0)
				std::cout << "error creating socket (errno = " << strerror(errno) << ")" << std::endl;
			results[j] = std::min(results[j], sock);
			
			// set O_NONBLOCK
			if ((err = FCNTL(sock, F_SETFL, O_NONBLOCK) < 0))
				std::cout << "error setting O_NONBLOCK (errno=" << strerror(errno) << ")" << std::endl;
			results[j] = std::min(results[j], err);

			// connect()
			if (false) {
				server = gethostbyname2("::",AF_INET6);
				memset((char *) &addr6, 0, sizeof(addr6));
				addr6.sin6_flowinfo = 0;
				addr6.sin6_family = AF_INET6;
				addr6.sin6_port = htons(port);
				addr6.sin6_addr = in6addr_any;
				err = CONNECT(sock, (struct sockaddr *)&addr6, (socklen_t)(sizeof addr6));
			}
			if (true) {
				addr.sin_port = htons(port);
				addr.sin_addr.s_addr = inet_addr("10.9.9.51");
				//addr.sin_addr.s_addr = htons(INADDR_ANY);
				addr.sin_family = AF_INET;
				err = CONNECT(sock, (struct sockaddr *)&addr, (socklen_t)(sizeof addr));
			}

			if (errno != EINPROGRESS) { // acceptable error for non-block mode
				if (err < 0)
					std::cout << "error connecting socket (errno = " << strerror(errno) << ")" << std::endl;
				results[j] = std::min(results[j], err);
			}

			// close()
			if ((err = CLOSE(sock)) < 0)
				std::cout << "error closing socket (errno = " << strerror(errno) << ")" << std::endl;
			results[j] = std::min(results[j], err);
		}

		displayResults(results, num_times);
		//if (zts_num_active_virt_sockets() == 0)
		//	std::cout << "PASSED [slam open, connect, close]" << std::endl;
		//else
		//	std::cout << "FAILED [slam open, connect, close]" << std::endl;
	}
	return 0;
}

/*
void get_network_routes(char *nwid)
{
	// Retreive managed routes for a given ZeroTier network
	std::vector<ZT_VirtualNetworkRoute> *routes = zts_get_network_routes(nwid);

	for (int i=0; i<routes->size(); i++) {
		struct sockaddr_in *target = (struct sockaddr_in*)&(routes->at(i).target);
		struct sockaddr_in *via = (struct sockaddr_in*)&(routes->at(i).via);
		char target_str[INET6_ADDRSTRLEN];
		memset(target_str, 0, INET6_ADDRSTRLEN);
		inet_ntop(AF_INET, (const void *)&((struct sockaddr_in *)target)->sin_addr.s_addr, target_str, INET_ADDRSTRLEN);
		char via_str[INET6_ADDRSTRLEN];
		memset(via_str, 0, INET6_ADDRSTRLEN);
		inet_ntop(AF_INET, (const void *)&((struct sockaddr_in *)via)->sin_addr.s_addr, via_str, INET_ADDRSTRLEN);
		DEBUG_TEST("<target=%s, via=%s, flags=%d>", target_str, via_str, routes->at(i).flags);
	}
}
*/

/****************************************************************************/
/* RANDOMIZED API TEST                                                      */
/****************************************************************************/

int random_api_test()
{
	// PASSED implies we didn't segfault or hang anywhere

	// variables which will be populated with random values
	/*
	int socket_family;
	int socket_type;
	int protocol;
	int fd;
	int len;
	int addrlen;
	int flags;

	struct sockaddr_storage;
	struct sockaddr_in addr;
	struct sockaddr_in6 addr6;
	*/
	
	/*
	int num_operations = 100;
	char *opbuf = (char*)malloc(num_operations*sizeof(char));
	generate_random_data(opbuf, num_operations, 0, 9);
	for (int i=0; i<num_operations; i++) {
		sleep(1);
		DEBUG_TEST("[i=%d, op=%d] calling X", i, opbuf[i]);

		// generate set of random arguments

		// addresses
	
		// buffers

		// buffer lengths

		// flags

		switch(opbuf[i])
		{
			case 0:
				SOCKET();
			case 1:
				CONNECT();
			case 2:
				LISTEN();
			case 3:
				BIND();
			case 4:
				ACCEPT();
			case 5:

		}
	}

	SOCKET()
	CONNECT()
	LISTEN()
	ACCEPT()
	BIND()
	GETSOCKOPT()
	SETSOCKOPT()
	FNCTL()
	CLOSE()
	SEND()
	RECV()
	SENDTO()
	RECVFROM()
	READ()
	WRITE()

	*/

	return PASSED;
}


/*
 For each API call, test the following:
  - All possible combinations of plausible system-defined arguments
  - Common values in innappropriate locations {-1, 0, 1}
  - Check for specific errno values for each function

*/
void test_bad_args()
{
// Protocol Family test set
	int proto_families[] = {
		AF_UNIX, 
		AF_LOCAL,
		AF_INET,
		AF_INET6,
		AF_IPX,
		PF_LOCAL,
		PF_UNIX,
		PF_INET,
		PF_ROUTE,
		PF_KEY,
		PF_INET6,
#if !defined(__linux__)
		PF_SYSTEM,
		PF_NDRV,
#endif
#if !defined(__APPLE__)
		AF_NETLINK,
		AF_X25,
		AF_AX25,
		AF_ATMPVC,
		AF_ALG,
		AF_PACKET,
#endif
		AF_APPLETALK
	};
	int num_proto_families = sizeof(proto_families) / sizeof(int);

// Socket Type test set
	int socket_types[] = {
		SOCK_STREAM,
		SOCK_DGRAM,
		SOCK_RAW
	};
	int num_socket_types = 3;


// Protocol test set

	// int min = -1;
	int max =  2;
	int err =  0;

	int min_protocol_family_value = 0;
	int max_protocol_family_value = 0;

	int min_socket_type_value = 0;
	int max_socket_type_value = 0;

	int min_protocol_value = 0;
	int max_protocol_value = 0;

	// socket()
	DEBUG_TEST("testing bad arguments for socket()");

	// Try all plausible argument combinations
	for (int i=0; i<num_proto_families; i++) {
		for (int j=0; j<num_socket_types; j++) {
			for (int k=0; k<max; k++) {

				int protocol_family = proto_families[i];
				int socket_type = socket_types[j];
				int protocol = -1;

				min_protocol_family_value = std::min(protocol_family, min_protocol_family_value); 
				max_protocol_family_value = std::max(protocol_family, max_protocol_family_value); 

				min_socket_type_value = std::min(socket_type, min_socket_type_value); 
				max_socket_type_value = std::max(socket_type, max_socket_type_value); 

				min_protocol_value = std::min(protocol, min_protocol_value); 
				max_protocol_value = std::max(protocol, max_protocol_value); 

				err = SOCKET(protocol_family, socket_type, protocol);
				usleep(100000);
				if (err < 0) {
					DEBUG_ERROR("SOCKET(%d, %d, %d) = %d, errno=%d (%s)", protocol_family, socket_type, protocol, err, errno, strerror(errno));
				}
				else {
					DEBUG_TEST("SOCKET(%d, %d, %d) = %d, errno=%d (%s)", protocol_family, socket_type, protocol, err, errno, strerror(errno));
				}
			}	
		}
	}

	DEBUG_TEST("min_protocol_family_value=%d",min_protocol_family_value);
	DEBUG_TEST("max_protocol_family_value=%d",max_protocol_family_value);

	DEBUG_TEST("min_socket_type_value=%d",min_socket_type_value);
	DEBUG_TEST("max_socket_type_value=%d",max_socket_type_value);

	DEBUG_TEST("min_protocol_value=%d",min_protocol_value);
	DEBUG_TEST("max_protocol_value=%d",max_protocol_value);

	DEBUG_TEST("AF_INET = %d", AF_INET);
	DEBUG_TEST("AF_INET6 = %d", AF_INET6);
	DEBUG_TEST("SOCK_STREAM = %d", SOCK_STREAM);
	DEBUG_TEST("SOCK_DGRAM = %d", SOCK_DGRAM);
}

void dns_test(struct sockaddr *addr)
{
	fprintf(stderr, "\n\ndns_test\n\n");
	zts_add_dns_nameserver(addr);
	// resolve
	zts_del_dns_nameserver(addr);
}

void close_while_writing_test()
{
	fprintf(stderr, "\n\nclose_while_writing_test\n\n");
	// TODO: Close a socket while another thread is writing to it or reading from it
}

/****************************************************************************/
/* test thread model, and locking                                           */
/****************************************************************************/

#define CONCURRENCY_LEVEL   8     // how many threads we want to test with
#define TIME_GRANULARITY    10000 // multiple in microseconds
#define TIME_MULTIPLIER_MIN 1     // 
#define TIME_MULTIPLIER_MAX 10    // 
#define WORKER_ITERATIONS   100   // number of times a worker shall do its task
#define MASTER_ITERATIONS   10    // number of times we will create a set of workers

// for passing info to worker threads
struct fd_addr_pair {
	int fd;
	struct sockaddr_in *remote_addr;
};

pthread_t tid[CONCURRENCY_LEVEL];

// over num_iterations, wait a random time, create a socket, wait a random time, and close the socket
void* worker_create_socket(void *arg)
{
	pthread_t id = pthread_self();
	int fd, rs, rc;
	// if (pthread_equal(id,tid[0])) { }
	for (int i=0; i<WORKER_ITERATIONS; i++) {
		rs = rand_in_range(TIME_MULTIPLIER_MIN, TIME_MULTIPLIER_MAX);
		rc = rand_in_range(TIME_MULTIPLIER_MIN, TIME_MULTIPLIER_MAX);
		fprintf(stderr, "id=%d, rs = %d, rc = %d\n", id, rs, rc);
		usleep(rs * TIME_GRANULARITY);
		fd = SOCKET(AF_INET, SOCK_STREAM, 0);
		usleep(rc * TIME_GRANULARITY);
		CLOSE(fd);
	}
	return NULL;
}

// test the core locking logic by creating large numbers of threads and performing random operations over an extended period of time
void multithread_test(int num_iterations, bool *passed)
{
	int err = 0;
	fprintf(stderr, "\n\nmultithread_socket_creation\n\n");
	// test zts_socket() and zts_close()
	for (int j=0; j<num_iterations; j++) {
		fprintf(stderr, "iteration=%d\n", j);
		// create threads
		for (int i=0; i<CONCURRENCY_LEVEL; i++) {
			fprintf(stderr,"creating thread [%d]\n", i);
			if ((err = pthread_create(&(tid[i]), NULL, &worker_create_socket, NULL)) < 0) {
				fprintf(stderr, "there was a problem while creating thread [%d]\n", i);
				*passed = false;
				return;
			}
		}
		// join all threads
		char *b;
		for (int i=0; i<CONCURRENCY_LEVEL; i++) {
			if ((err = pthread_join(tid[i],(void**)&b)) < 0) {
				fprintf(stderr, "error while joining thread [%d]\n", i);
				*passed = false;
				return;
			}
		}
	}
	*passed = true;
}

// write a simple string message to a SOCK_DGRAM socket
void* worker_write_to_udp_socket(void *arg) {
	fprintf(stderr, "\n\n\nwrite_to_udp_socket\n\n\n");
	struct fd_addr_pair *fdp = (struct fd_addr_pair*)arg;
	int fd = fdp->fd;
	struct sockaddr_in *remote_addr = fdp->remote_addr;
	//fprintf(stderr, "fd=%d\n", fd);
	int w = 0;
	for (int i=0; i<WORKER_ITERATIONS; i++) {
		int r = rand_in_range(TIME_MULTIPLIER_MIN, TIME_MULTIPLIER_MAX);
		usleep(r * TIME_GRANULARITY);
		if ((w = SENDTO(fd, "hello", 5, 0, (struct sockaddr *)remote_addr, sizeof(*remote_addr))) < 0) {
			DEBUG_ERROR("error sending packet, err=%d", errno);
		}
	}
	return NULL;
}

// create a single socket and many threads to write to that single socket
void multithread_udp_write(struct sockaddr_in *local_addr, struct sockaddr_in *remote_addr, bool *passed)
{
	fprintf(stderr, "\n\nmultithread_udp_broadcast\n\n");
	int fd, err;
	if((fd = SOCKET(AF_INET, SOCK_DGRAM, 0)) < 0) {
		DEBUG_ERROR("error while creating socket");
		*passed = false;
		return;
	}
	if ((err = BIND(fd, (struct sockaddr *)local_addr, sizeof(struct sockaddr_in)) < 0)) {
		DEBUG_ERROR("error binding to interface (%d)", err);
		perror("bind");
		*passed = false;
		return;
	}
	// params to send to new threads
	struct fd_addr_pair fdp;
	fdp.fd = fd;
	fdp.remote_addr = remote_addr;

	for (int i=0; i<CONCURRENCY_LEVEL; i++) {
		fprintf(stderr,"creating thread [%d]\n", i);
		if ((err = pthread_create(&(tid[i]), NULL, &worker_write_to_udp_socket, (void*)&fdp)) < 0) {
			fprintf(stderr, "there was a problem while creating thread [%d]\n", i);
			*passed = false;
			return;
		}
	}
	// join all threads
	char *b;
	for (int i=0; i<CONCURRENCY_LEVEL; i++) {
		if ((err = pthread_join(tid[i],(void**)&b)) < 0) {
			fprintf(stderr, "error while joining thread [%d]\n", i);
			*passed = false;
			return;
		}
	}
	CLOSE(fd);
}

void multithread_rw_server()
{
	fprintf(stderr, "\n\nmultithread_rw_server\n\n");
	// TODO: Test read/writes from multiple threads
}

void multithread_rw_client()
{
	fprintf(stderr, "\n\nmultithread_rw_client\n\n");
}

/****************************************************************************/
/* close()                                                                  */
/****************************************************************************/

// Tests rapid opening and closure of sockets
void close_test(struct sockaddr *bind_addr)
{
	fprintf(stderr, "\n\nclose_test\n\n");
	// BUG: While running an extended test of unassigned closures, the 
	// stack may crash at: `pico_check_timers at pico_stack.c:608, this appears
	// to be a bad pointer to a timer within the stack. 
	bool extended = false;
	int tries = !extended ? 8 : 1024;
	int err = 0;
	for (int i=0; i<tries; i++)
	{
		int fd;
		if ((fd = SOCKET(AF_INET, SOCK_STREAM, 0)) < 0) {
			DEBUG_ERROR("error creating socket. sleeping until timers are released");
			sleep(30);
		}
		if ((err = BIND(fd, (struct sockaddr *)bind_addr, sizeof(struct sockaddr_in)) < 0)) { 
			DEBUG_ERROR("error binding to interface (%d)", err);
		}
		usleep(100000);
		if ((err = CLOSE(fd)) < 0) {
			DEBUG_ERROR("error closing socket (%d)", err);
		}
		DEBUG_TEST("i=%d, close() = %d", i, err);
		((struct sockaddr_in *)bind_addr)->sin_port++;
	}
}

void bind_to_localhost_test(int port)
{
	fprintf(stderr, "\n\nbind_to_localhost_test\n\n");
	int fd, err = 0;
	// ipv4, 0.0.0.0
	struct sockaddr_storage bind_addr;
	DEBUG_TEST("binding to 0.0.0.0");
	str2addr("0.0.0.0", port, 4, (struct sockaddr *)&bind_addr);
	if ((fd = SOCKET(AF_INET, SOCK_STREAM, 0)) > 0) {
		if ((err = BIND(fd, (struct sockaddr *)&bind_addr, sizeof(struct sockaddr_in))) == 0) { 
			usleep(100000);
			if ((err = CLOSE(fd)) < 0) {
				DEBUG_ERROR("error closing socket (%d)", err);
			}
		}
		else{
			DEBUG_ERROR("error binding to interface (%d)", err);
		}
	}
	else {
		DEBUG_ERROR("error creating socket (%d)", err);
	}

	port++;

	/*
	// ipv4, 127.0.0.1
	DEBUG_TEST("binding to 127.0.0.1");
	str2addr("127.0.0.1", port, 4, (struct sockaddr *)&bind_addr);
	if ((fd = SOCKET(AF_INET, SOCK_STREAM, 0)) > 0) {
		if ((err = BIND(fd, (struct sockaddr *)&bind_addr, sizeof(struct sockaddr_in))) == 0) { 
			usleep(100000);
			if ((err = CLOSE(fd)) < 0) {
				DEBUG_ERROR("error closing socket (%d)", err);
			}
		}
		else{
			DEBUG_ERROR("error binding to interface (%d)", err);
		}
	}
	else {
		DEBUG_ERROR("error creating socket", err);
	}

	port++;
	*/

	// ipv6, [::]
	DEBUG_TEST("binding to [::]");
	str2addr("::", port, 6, (struct sockaddr *)&bind_addr);
	if ((fd = SOCKET(AF_INET6, SOCK_STREAM, 0)) > 0) {
		if ((err = BIND(fd, (struct sockaddr *)&bind_addr, sizeof(struct sockaddr_in))) == 0) { 
			usleep(100000);
			if ((err = CLOSE(fd)) < 0) {
				DEBUG_ERROR("error closing socket (%d)", err);
			}
		}
		else{
			DEBUG_ERROR("error binding to interface (%d)", err);
		}
	}
	else {
		DEBUG_ERROR("error creating socket (%d)", err);
	}
}

#endif // __SELFTEST__

/****************************************************************************/
/* main(), calls test_driver(...)                                           */
/****************************************************************************/

int main(int argc , char *argv[])
{
	if (argc < 6) {
		fprintf(stderr, "usage: selftest <num_repeats> <selftest.conf> <alice|bob|ted|carol> to <bob|alice|ted|carol>\n");
		fprintf(stderr, "e.g. : selftest 3 test/test.conf alice to bob\n");
		return 1;
	}

	int num_repeats = atoi(argv[1]);
	std::string path = argv[2];
	std::string from = argv[3];
	std::string   to = argv[5];
	std::string   me = from;
	std::vector<std::string> results;
	std::string remote_echo_ipv4, smode;

	std::string nwid, stype;
	std::string ipstr, ipstr6, local_ipstr, local_ipstr6, remote_ipstr, remote_ipstr6;

	int err         = 0;
	int mode        = 0;
	int port        = 0;
	int op          = 0;
	int start_port  = 0;
	int cnt         = 0;
	int ipv;
	// for timing
	// how long we expect the specific test to take
	int subtest_expected_duration;
	// (T+X), when we plan to start this test 
	int subtest_start_time_offset = 0;

	char details[128];
	memset(&details, 0, sizeof details);
	bool passed = 0; 
	struct sockaddr_storage local_addr;
	struct sockaddr_storage remote_addr;

	// load config file
	if (path.find(".conf") == std::string::npos) {
		fprintf(stderr, "Possibly invalid conf file. Exiting...\n");
		exit(0);
	}
	loadTestConfigFile(path);
	// get origin details
	local_ipstr      = testConf[me + ".ipv4"];
	local_ipstr6     = testConf[me + ".ipv6"];
	nwid             = testConf[me + ".nwid"];
	path             = testConf[me + ".path"];
	stype            = testConf[me + ".test"];
	smode            = testConf[me + ".mode"];
	start_port       = atoi(testConf[me + ".port"].c_str());
	remote_echo_ipv4 = testConf[to + ".echo_ipv4"];
	remote_ipstr     = testConf[to + ".ipv4"];
	remote_ipstr6    = testConf[to + ".ipv6"];

	if (strcmp(smode.c_str(), "server") == 0)
		mode = TEST_MODE_SERVER;
	else
		mode = TEST_MODE_CLIENT;

	fprintf(stderr, "\n\nORIGIN:\n\n");
	fprintf(stderr, "\tlocal_ipstr      = %s\n", local_ipstr.c_str());
	fprintf(stderr, "\tlocal_ipstr6     = %s\n", local_ipstr6.c_str());
	fprintf(stderr, "\tstart_port       = %d\n", start_port);
	fprintf(stderr, "\tpath             = %s\n", path.c_str());
	fprintf(stderr, "\tnwid             = %s\n", nwid.c_str());
	fprintf(stderr, "\ttype             = %s\n\n", stype.c_str());
	fprintf(stderr, "DESTINATION:\n\n");
	fprintf(stderr, "\tremote_ipstr     = %s\n", remote_ipstr.c_str());
	fprintf(stderr, "\tremote_ipstr6    = %s\n", remote_ipstr6.c_str());
	fprintf(stderr, "\tremote_echo_ipv4 = %s\n", remote_echo_ipv4.c_str());

#if defined(__SELFTEST__)
	// set start time here since we need to wait for both libzt instances to be online
	long int selftest_start_time = get_now_ts();
	subtest_expected_duration = 5;

	DEBUG_TEST("Waiting for libzt to come online...\n");
	zts_simple_start(path.c_str(), nwid.c_str());
	char device_id[ZT_ID_LEN];
	zts_get_device_id(device_id);
	DEBUG_TEST("I am %s, %s", device_id, me.c_str());
	if (mode == TEST_MODE_SERVER) {
		DEBUG_TEST("Ready. You should start selftest program on second host now...\n\n");
	}
	if (mode == TEST_MODE_CLIENT) {
		DEBUG_TEST("Ready. Contacting selftest program on first host.\n\n");
	}
#endif // __SELFTEST__

	// What follows is a long-form of zts_simple_start():
	/*
	 zts_start(path.c_str());
	 printf("waiting for service to start...\n");
	 while (zts_running() == false)
		sleep(1);
	 printf("joining network...\n");
	 zts_join(nwid.c_str());
	 printf("waiting for address assignment...\n");
	 while (zts_has_address(nwid.c_str()) == false)
		sleep(1);
	*/

for (int i=0; i<num_repeats; i++)
{
	DEBUG_TEST("\n\n\n --- COMPREHENSIVE TEST ITERATION: %d out of %d ---\n\n\n", i, num_repeats);

#if defined(__SELFTEST__)
	if (false) {
		port = 1000; 
		// closure test
		struct sockaddr_in in4;
		DEBUG_TEST("testing closures by binding to: %s", local_ipstr.c_str());
		str2addr(local_ipstr, port, 4, (struct sockaddr *)&in4);
		close_test((struct sockaddr*)&in4);
		port++;
	}

	// Test adding, resolving, and removing a DNS server

	//	ipv = 4;
	//	str2addr(remote_ipstr, port, ipv, (struct sockaddr *)&remote_addr);
	//	dns_test((struct sockaddr *)&remote_addr);

	//	close_while_writing_test();

	// localhost bind test

	//	bind_to_localhost_test(port);

	// Transmission Tests

	// RANDOM API TEST
			//random_api_test();

	// SLAM API TEST
			//slam_api_test();

	// BAD ARGS API TEST
			//test_bad_args();

	// OBSCURE API TEST
	if (false) {
		obscure_api_test(&passed);
	}

	// Spam a SOCK_DGRAM socket from many threads
	if (false) {
		ipv = 4;
		port = start_port;
		str2addr(local_ipstr, port, ipv, (struct sockaddr *)&local_addr);
		str2addr(remote_ipstr, port, ipv, (struct sockaddr *)&remote_addr);
		multithread_udp_write((struct sockaddr_in *)&local_addr, (struct sockaddr_in *)&remote_addr, &passed);
	}
	
	//
	if (false) {
		multithread_test(10, &passed);
	}

#endif // __SELFTEST__

	port = start_port+(100*i); // arbitrary
	cnt  = 1024*3;
	op   = TEST_OP_N_BYTES;

	/*
	// Deliberately create a bad read to trigger address sanitizer
	int stack_array[100];
	stack_array[1] = 0;
	return stack_array[argc + 100];  // BOOM

	int mybuf[10];
	mybuf[11] = 9;
	memcpy(mybuf, "what the hell is this", 55);
	*/

	// set start time here since we aren't waiting for libzt to come online in NATIVETEST mode
	#if defined(__NATIVETEST__)	
		long int selftest_start_time = get_now_ts();
		subtest_expected_duration = 20; // initial value, wait for other instance to come online
	#endif

	#if defined(LIBZT_IPV4)
		// UDP 4 client/server
		
		ipv = 4;
		subtest_start_time_offset += subtest_expected_duration;
		subtest_expected_duration = 30;

		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			udp_server_4((struct sockaddr_in *)&local_addr, (struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(local_ipstr, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			udp_client_4((struct sockaddr_in *)&local_addr, (struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		mode = mode == TEST_MODE_SERVER ? TEST_MODE_CLIENT : TEST_MODE_SERVER; // switch roles
		port++; // move up one port
		subtest_start_time_offset+=subtest_expected_duration;
		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			udp_server_4((struct sockaddr_in *)&local_addr, (struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(local_ipstr, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			udp_client_4((struct sockaddr_in *)&local_addr, (struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		port++;

	// UDP 4 sustained transfer

		ipv = 4;	
		subtest_start_time_offset+=subtest_expected_duration;
		subtest_expected_duration = 30;

		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			udp_server_sustained_4((struct sockaddr_in *)&local_addr, (struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(local_ipstr, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			udp_client_sustained_4((struct sockaddr_in *)&local_addr, (struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		mode = mode == TEST_MODE_SERVER ? TEST_MODE_CLIENT : TEST_MODE_SERVER; // switch roles
		port++; // move up one port
		subtest_start_time_offset+=subtest_expected_duration;
		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			udp_server_sustained_4((struct sockaddr_in *)&local_addr, (struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(local_ipstr, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			udp_client_sustained_4((struct sockaddr_in *)&local_addr, (struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		port++;

	// TCP 4 client/server

		ipv = 4;
		subtest_start_time_offset+=subtest_expected_duration;
		subtest_expected_duration = 30;

		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr, port, ipv, (struct sockaddr *)&local_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			tcp_server_4((struct sockaddr_in *)&local_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(remote_ipstr, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			tcp_client_4((struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		mode = mode == TEST_MODE_SERVER ? TEST_MODE_CLIENT : TEST_MODE_SERVER; // switch roles
		port++; // move up one port
		subtest_start_time_offset+=subtest_expected_duration;
		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr, port, ipv, (struct sockaddr *)&local_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			tcp_server_4((struct sockaddr_in *)&local_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(remote_ipstr, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			tcp_client_4((struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		port++;

	// TCP 4 sustained transfer
		
		ipv = 4;	
		subtest_start_time_offset+=subtest_expected_duration;
		subtest_expected_duration = 30;

		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr, port, ipv, (struct sockaddr *)&local_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			tcp_server_sustained_4((struct sockaddr_in *)&local_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(remote_ipstr, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			tcp_client_sustained_4((struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		mode = mode == TEST_MODE_SERVER ? TEST_MODE_CLIENT : TEST_MODE_SERVER; // switch roles
		port++;
		subtest_start_time_offset+=subtest_expected_duration;
		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr, port, ipv, (struct sockaddr *)&local_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			tcp_server_sustained_4((struct sockaddr_in *)&local_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(remote_ipstr, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			tcp_client_sustained_4((struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		port++;

		// PERFORMANCE (between this library instance and a native non library instance (echo) )
		// Client/Server mode isn't being tested here, so it isn't important, we'll just set it to client

		// ipv4 echo test (TCP)
		/*
		ipv = 4;	
		if (me == "alice" || me == "ted") {
			port=start_port+100; // e.g. 7100
			str2addr(remote_echo_ipv4, port, ipv, (struct sockaddr *)&remote_addr);
			tcp_perf_tx_echo_4((struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
			RECORD_RESULTS(passed, details, &results);
			tcp_perf_rx_echo_4((struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
			RECORD_RESULTS(passed, details, &results);
		}
		if (me == "bob" || me == "carol") {
			DEBUG_TEST("waiting (15s) for other selftest to complete before continuing...");
			port=start_port+101; // e.g. 7101
			str2addr(remote_echo_ipv4, port, ipv, (struct sockaddr *)&remote_addr);
			tcp_perf_rx_echo_4((struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
			RECORD_RESULTS(passed, details, &results);
			tcp_perf_tx_echo_4((struct sockaddr_in *)&remote_addr, op, cnt, details, &passed); 
			RECORD_RESULTS(passed, details, &results);
		}
		*/

	#endif // LIBZT_IPV4





	#if defined(LIBZT_IPV6)
		// UDP 6 client/server

		ipv = 6;
		subtest_start_time_offset+=subtest_expected_duration;
		subtest_expected_duration = 30;

		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr6, port, ipv, (struct sockaddr*)&local_addr);
			str2addr(remote_ipstr6, port, ipv, (struct sockaddr*)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			udp_server_6((struct sockaddr_in6 *)&local_addr, (struct sockaddr_in6 *)&remote_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(local_ipstr6, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr6, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			udp_client_6((struct sockaddr_in6 *)&local_addr, (struct sockaddr_in6 *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		mode = mode == TEST_MODE_SERVER ? TEST_MODE_CLIENT : TEST_MODE_SERVER; // switch roles
		port++; // move up one port
		subtest_start_time_offset+=subtest_expected_duration;
		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr6, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr6, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			udp_server_6((struct sockaddr_in6 *)&local_addr, (struct sockaddr_in6 *)&remote_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(local_ipstr6, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr6, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			udp_client_6((struct sockaddr_in6 *)&local_addr, (struct sockaddr_in6 *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		port++;



	// UDP 6 sustained transfer

		ipv = 6;	
		subtest_start_time_offset+=subtest_expected_duration;
		subtest_expected_duration = 30;

		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr6, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr6, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			udp_server_sustained_6((struct sockaddr_in6 *)&local_addr, (struct sockaddr_in6 *)&remote_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(local_ipstr6, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr6, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			udp_client_sustained_6((struct sockaddr_in6 *)&local_addr, (struct sockaddr_in6 *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		mode = mode == TEST_MODE_SERVER ? TEST_MODE_CLIENT : TEST_MODE_SERVER; // switch roles
		port++; // move up one port
		subtest_start_time_offset+=subtest_expected_duration;
		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr6, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr6, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			udp_server_sustained_6((struct sockaddr_in6 *)&local_addr, (struct sockaddr_in6 *)&remote_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(local_ipstr6, port, ipv, (struct sockaddr *)&local_addr);
			str2addr(remote_ipstr6, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			udp_client_sustained_6((struct sockaddr_in6 *)&local_addr, (struct sockaddr_in6 *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		port++;

	// TCP 6 client/server

		ipv = 6;
		subtest_start_time_offset+=subtest_expected_duration;
		subtest_expected_duration = 30;

		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr6, port, ipv, (struct sockaddr *)&local_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			tcp_server_6((struct sockaddr_in6 *)&local_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			DEBUG_TEST("waiting (15s) for other selftest to complete before continuing...");
			str2addr(remote_ipstr6, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			tcp_client_6((struct sockaddr_in6 *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		mode = mode == TEST_MODE_SERVER ? TEST_MODE_CLIENT : TEST_MODE_SERVER; // switch roles
		port++; // move up one port
		subtest_start_time_offset+=subtest_expected_duration;
		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr6, port, ipv, (struct sockaddr *)&local_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			tcp_server_6((struct sockaddr_in6 *)&local_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(remote_ipstr6, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			tcp_client_6((struct sockaddr_in6 *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		port++;

	// TCP 6 sustained transfer

		ipv = 6;	
		subtest_start_time_offset+=subtest_expected_duration;
		subtest_expected_duration = 30;

		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr6, port, ipv, (struct sockaddr *)&local_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			tcp_server_sustained_6((struct sockaddr_in6 *)&local_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(remote_ipstr6, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			tcp_client_sustained_6((struct sockaddr_in6 *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		mode = mode == TEST_MODE_SERVER ? TEST_MODE_CLIENT : TEST_MODE_SERVER; // switch roles
		port++;
		subtest_start_time_offset+=subtest_expected_duration;
		if (mode == TEST_MODE_SERVER) {
			str2addr(local_ipstr6, port, ipv, (struct sockaddr *)&local_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset);
			tcp_server_sustained_6((struct sockaddr_in6 *)&local_addr, op, cnt, details, &passed); 
		}
		else if (mode == TEST_MODE_CLIENT) {
			str2addr(remote_ipstr6, port, ipv, (struct sockaddr *)&remote_addr);
			wait_until_tplus_s(selftest_start_time, subtest_start_time_offset+5);
			tcp_client_sustained_6((struct sockaddr_in6 *)&remote_addr, op, cnt, details, &passed); 
		}
		RECORD_RESULTS(passed, details, &results);
		port++;
	#endif // LIBZT_IPV6

		// Print results of all tests
		printf("--------------------------------------------------------------------------------\n");
		for (int i=0;i<results.size(); i++) {
			fprintf(stderr, "%s\n", results[i].c_str());
		}
	}
	return err;
}

// FCNTL(client_fd, F_SETFL, O_NONBLOCK);

#include "gbn.h"

state_t s;
struct sockaddr serv;
struct sockaddr cli;
socklen_t serv_len;
socklen_t cli_len;
int* attempts;
int attempt = 0;
int* seqOnTheFly;
int numPackets;

uint16_t checksum(uint16_t *buf, int nwords)
{
	uint32_t sum;

	for (sum = 0; nwords > 0; nwords--) {
		sum += *buf++;
	}
	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	return ~sum;
}


void sig_handler(int signum){
	attempt ++;
	int i;
	for (i = 0; i < numPackets; i++) {
		if (seqOnTheFly[i]) attempts[i]++;
	}
	printf("Timeout has occurred\n");
	signal(SIGALRM, sig_handler);
	alarm(TIMEOUT);
}


void make_packet(gbnhdr* packet,uint8_t type, uint8_t seqnum, int isHeader, char *buffer, int datalen){
	packet->type = type;
	packet->seqnum = seqnum;

	if (isHeader == 0) packet->checksum = 0;
	else {
		memcpy(packet->data, buffer, datalen);
		packet->datalen = datalen;
		packet->checksum = checksum((uint16_t *) buffer, (1 + datalen) / 2);
	}
}


int is_timeout() {
	if (s.timed_out == 0) {
		s.timed_out = -1;
		return 0;
	}
	return -1;
}

/*
 * params:
 * 1.packet,
 * 2.expected type
 */
int check_packetType(gbnhdr *packet, int type) {
	if (packet->type != type) return -1;
	return 0;
}

/*
 * params: 1. packet, 2. expected expected number
 * rec_seqnum should be expected seqnum
 * seq for ACK should be last sent seqnum
 */
int check_seqnum(gbnhdr *packet, int expected) {
	if (packet->seqnum != expected) return -1;
	return 0;
}

ssize_t gbn_send(int sockfd, const void *buf, size_t len, int flags){
	/* split data into multiple packets */
	numPackets = (int) len / DATALEN;
	int lastPacketSize = len % DATALEN;
	if (len % DATALEN != 0) numPackets ++;
	printf("in send and ready to send %i packets\n", numPackets);
	int att[numPackets];
	memset(att, 0, numPackets * sizeof(int));
	attempts = att;
	int sOnTheFly[numPackets];
	memset(sOnTheFly, 0, numPackets * sizeof(int));
	seqOnTheFly = sOnTheFly;
	char * slicedBuf = malloc(DATALEN);
	int i = 0;
	signal(SIGALRM, sig_handler);

	while (i < numPackets) {
		int j = 0;

		while ( i < numPackets && j < s.mode) {
			printf("sending packet %i\n", i);
			if (attempts[i] >= MAX_ATTEMPT) {
				s.state = CLOSED;
				free(slicedBuf);
				return -1;
			}
			
			int currSize = DATALEN;
			if (i == numPackets -1) currSize = lastPacketSize;
			
			memset(slicedBuf, '\0', currSize);
			memcpy(slicedBuf, buf + i * DATALEN, currSize);

			gbnhdr *packet = malloc(sizeof(gbnhdr));
			make_packet(packet, DATA, s.send_seqnum, -1, slicedBuf, currSize);
			if (attempts[i] < MAX_ATTEMPT && sendto(sockfd, packet, sizeof(*packet), flags, s.senderServerAddr, s.senderSocklen) == -1) {
				attempts[i] ++;
				free(packet);
				continue;
			}
			seqOnTheFly[s.send_seqnum] = 1;
			if (j == 0) alarm(TIMEOUT);
			s.send_seqnum ++;
			j++;
			i++;
			free(packet);
		}

		int unACK = j;
		while (unACK > 0) {
			/* receive ack header */
			gbnhdr *rec_header = malloc(sizeof(gbnhdr));
			struct sockaddr tmp_sock;
			socklen_t tmp_sock_len;
			maybe_recvfrom(sockfd, (char *)rec_header, sizeof(rec_header), 0, &tmp_sock, &tmp_sock_len);
			/* verify there is no timeout, verify type = dataack and seqnum are expected */
			if (check_packetType(rec_header, DATAACK) == 0 && check_seqnum(rec_header, s.rec_seqnum) == 0) {
				printf("received dataack for seq %d successfully\n", s.rec_seqnum);
				s.mode = s.mode == SLOW ? MODERATE : FAST;
				seqOnTheFly[s.rec_seqnum] = 0;
				attempts[s.rec_seqnum++] = 0;
				unACK --;
				alarm(TIMEOUT); 
			} else {
				i -= s.send_seqnum - s.rec_seqnum;
				s.send_seqnum = s.rec_seqnum;
				s.mode = SLOW;
				free(rec_header);
				attempts[i] ++;
				break;
			}
			free(rec_header);
		}
	}
	free(slicedBuf);
	return 0;
}

ssize_t gbn_recv(int sockfd, void *buf, size_t len, int flags){
	/* receiver receive packet from sender and if valid, send DATAACK */
	printf ("in receive\n");

	gbnhdr * sender_packet = malloc(sizeof(gbnhdr));


RECV:
	if (maybe_recvfrom(sockfd, (char *)sender_packet, sizeof(gbnhdr), 0, s.receiverServerAddr, &s.receiverSocklen) == -1) {
		goto RECV;
	}

	/* if a data packet is received, check packet to verify its type */
	if (sender_packet->type == DATA) {
		alarm(TIMEOUT);
		/* check data validity */
		if (check_seqnum(sender_packet, s.rec_seqnum) == -1) {
			 printf("received an unexpected seqnum, discarding data...\n");
			goto RECV;
		}
		int sender_packet_size = sender_packet->datalen;
		if (checksum((uint16_t *)&sender_packet->data, (1 + sender_packet_size) / 2) == -1) {
			printf("data is corrupt\n");
			goto RECV;
		}
		memcpy(buf, sender_packet->data, sender_packet_size);
		/* receiver reply with DATAACK header with seqnum received */
		gbnhdr *rec_header = malloc(sizeof(gbnhdr));
		make_packet(rec_header, DATAACK, s.rec_seqnum, 0, NULL, 0);

		if (sendto(sockfd, rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) {
			printf ("error sending in gbn_recv\n");
			goto RECV;
		}
		printf("sent dataack with seqnum %i\n", s.rec_seqnum);
		free(rec_header);
		/* if successfully send ACK, expected next rec_seqnum ++ */
		s.rec_seqnum ++;
		return sender_packet_size;
	/* if a connection teardown request is received, reply with FINACK header */
	} else if (check_packetType(sender_packet, FIN) == 0) {
		printf("reply with FINACK header \n");
		gbnhdr *rec_header = malloc(sizeof(gbnhdr));
		make_packet(rec_header, FINACK, 0, 0, NULL, 0);
		if (sendto(sockfd, rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
		s.state = FIN_RCVD;
		return 0;
	} else {
		goto RECV;
	}

	return(-1);
}


int gbn_close(int sockfd){
	printf("in connection close\n");
    printf("state %i\n", s.state);
	/* sender initiate a FIN request and wait for FINACK */
    while (1) {
    	if (s.state == ESTABLISHED || s.state == SYN_SENT || s.state == SYN_RCVD) {
			printf("client send fin to server to close connection \n");
			gbnhdr * send_header = malloc(sizeof(gbnhdr));
			make_packet(send_header, FIN, 0, 0, NULL, 0);
			if (sendto(sockfd, send_header, sizeof(gbnhdr), 0, s.senderServerAddr, s.senderSocklen) == -1) return -1;
			if (sendto(sockfd, send_header, sizeof(gbnhdr), 0, s.senderServerAddr, s.senderSocklen) == -1) return -1;
			if (sendto(sockfd, send_header, sizeof(gbnhdr), 0, s.senderServerAddr, s.senderSocklen) == -1) return -1;
			if (sendto(sockfd, send_header, sizeof(gbnhdr), 0, s.senderServerAddr, s.senderSocklen) == -1) return -1;
			if (sendto(sockfd, send_header, sizeof(gbnhdr), 0, s.senderServerAddr, s.senderSocklen) == -1) return -1;
			if (sendto(sockfd, send_header, sizeof(gbnhdr), 0, s.senderServerAddr, s.senderSocklen) == -1) return -1;
			if (sendto(sockfd, send_header, sizeof(gbnhdr), 0, s.senderServerAddr, s.senderSocklen) == -1) return -1;
			if (sendto(sockfd, send_header, sizeof(gbnhdr), 0, s.senderServerAddr, s.senderSocklen) == -1) return -1;
			if (sendto(sockfd, send_header, sizeof(gbnhdr), 0, s.senderServerAddr, s.senderSocklen) == -1) return -1;
			if (sendto(sockfd, send_header, sizeof(gbnhdr), 0, s.senderServerAddr, s.senderSocklen) == -1) return -1;
			if (sendto(sockfd, send_header, sizeof(gbnhdr), 0, s.senderServerAddr, s.senderSocklen) == -1) return -1;
			if (sendto(sockfd, send_header, sizeof(gbnhdr), 0, s.senderServerAddr, s.senderSocklen) == -1) return -1;
			if (sendto(sockfd, send_header, sizeof(gbnhdr), 0, s.senderServerAddr, s.senderSocklen) == -1) return -1;
			if (sendto(sockfd, send_header, sizeof(gbnhdr), 0, s.senderServerAddr, s.senderSocklen) == -1) return -1;
			s.state = FIN_SENT;

		}
		else if (s.state == FIN_SENT) {
			gbnhdr *finack_packet = malloc(sizeof(gbnhdr));
			if (maybe_recvfrom(sockfd, (char *)finack_packet, sizeof(gbnhdr), 0, s.receiverServerAddr, &s.receiverSocklen) == -1) {
				continue;
			}
			if (finack_packet->type == FINACK) {
				printf("client close.\n");
				return 0;
			}
		/* if receiver sees a FIN header, reply with FINACK and close socket connection */
		} else if (s.state == FIN_RCVD) {
			printf("server send finack to client to close connection\n");
			gbnhdr * rec_header = malloc(sizeof(gbnhdr));
			make_packet(rec_header, FINACK, 0, 0, NULL, 0);
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			if (sendto(sockfd, &rec_header, sizeof(gbnhdr), 0, s.receiverServerAddr, s.receiverSocklen) == -1) return -1;
			printf("server close.\n");
			return 0;
		}
    }
	return(-1);
}

int gbn_connect(int sockfd, const struct sockaddr *server, socklen_t socklen){
	if (sockfd < 0) return -1;
	printf("in gbn connect\n");
	/* Define Global State */
	s.mode = SLOW;
	s.senderSocklen = socklen;
	serv_len = socklen;
	serv = *server;

	gbnhdr *send_header = malloc(sizeof(gbnhdr));
	make_packet(send_header, SYN, 0, 0, NULL, 0);

	signal(SIGALRM, sig_handler);

	/* send SYN and wait for SYNACK. after that, send a SYNACK back. */
	while (attempt < MAX_ATTEMPT) {
		if (sendto(sockfd, send_header, sizeof(send_header), 0, server, s.senderSocklen) == -1 ) {
			attempt ++;
			printf("sender send syn failed\n");
			continue;
		}
		s.senderServerAddr = (struct sockaddr *)server;
		s.receiverSocklen = socklen;
		s.state = SYN_SENT;
		alarm(TIMEOUT);
		/* waiting for receiving SYNACK */
		gbnhdr *rec_header = malloc(sizeof(gbnhdr));
		if (maybe_recvfrom(sockfd, (char *)rec_header, sizeof(rec_header), 0, s.receiverServerAddr, &s.receiverSocklen) == -1) {
			printf("sender error in recvfrom syn ack\n");
			attempt ++;
			continue;
		}
		/* check for timeout, check if header type is SYNACK */
		if (check_packetType(rec_header, SYNACK) == 0) {
			printf("sender received synack header\n");
			s.state = ESTABLISHED;
			printf("sender connection established\n");
			make_packet(send_header, SYNACK, 0, 0, NULL, 0);
			sendto(sockfd, send_header, sizeof(send_header), 0, s.senderServerAddr, s.senderSocklen);
			return 0;
		}
	}
	/* if reach max number of tries, close the connection */
	s.state = CLOSED;
	return(-1);
}

int gbn_listen(int sockfd, int backlog){
	printf("in listen\n");
	while(0) {
		gbnhdr *send_header = malloc(sizeof(gbnhdr));
		/* receiver receive from (listen to) header of the request to connect */
		if (maybe_recvfrom(sockfd, (char *)send_header, sizeof(gbnhdr), 0, s.receiverServerAddr, &s.receiverSocklen) == -1) {
			printf("error rec syn from sender\n");
			return -1;
		}

		if (check_packetType(send_header, SYN) == 0) {
			s.state = SYN_RCVD;
			printf("received syn header\n");
			return 0;
		}
	}

	return 0;
}

int gbn_bind(int sockfd, const struct sockaddr *server, socklen_t socklen){
	/* pointer to local struct on receiver server where sender address is to be stored */
	printf("in bind\n");
	s.receiverServerAddr = (struct sockaddr *)server;
	s.receiverSocklen = socklen;
	return bind(sockfd, server, socklen);
}	

int gbn_socket(int domain, int type, int protocol){
		
	/*----- Randomizing the seed. This is used by the rand() function -----*/
	printf("in gbn_socket\n");
	srand((unsigned)time(0));

	return socket(domain, type, protocol);
}

int gbn_accept(int sockfd, struct sockaddr *client, socklen_t *socklen){
	/*
	 * receiver send to sender either RST or SYNACK based on global state
	 * if accept, initialize the receiver sequence number to 0
	*/
	s.rec_seqnum = 0;
	gbnhdr * rec_header = malloc(sizeof(gbnhdr));
	printf("in accept\n");

	/* if connection teardown initiated, reject connection by sending RST */
	if (s.state == FIN_SENT) make_packet(rec_header, RST, 0, 0, NULL, 0);
	/* accept connection initiation by sending header with SYNACK */
	else make_packet(rec_header, SYNACK, 0, 0, NULL, 0);



	signal(SIGALRM, sig_handler);


	int attempt = 0;


	struct sockaddr t;
	struct sockaddr* tmp_sock = &t;
	socklen_t t_sock_len;
	socklen_t* tmp_sock_len = &t_sock_len;
	cli = *client;
	cli_len = *socklen;
	int syned = 0;
	/* wait for SYN, then send SYNACK and wait for SYNACK. */
	while (attempt < MAX_ATTEMPT) {
		gbnhdr *send_header_syn = malloc(sizeof(gbnhdr));
		if (!syned) {
			if (maybe_recvfrom(sockfd, (char *)send_header_syn, sizeof(send_header_syn), 0, tmp_sock, tmp_sock_len) == -1) {
				printf("error rec syn from sender\n");
				return -1;
			}

			if (check_packetType(send_header_syn, SYN) != 0) {
				printf("wrong type received. expect SYN\n");
				attempt ++;
				continue;
			}
			syned = 1;
			cli = *tmp_sock;
		}
		if (sendto(sockfd, rec_header, sizeof(gbnhdr), 0, tmp_sock, *tmp_sock_len) == -1 ) {
			attempt ++;
			printf("receiver send synack failed\n");
			continue;
		}
		s.senderServerAddr = tmp_sock;
		s.senderSocklen = *tmp_sock_len;
		alarm(TIMEOUT);
		/* waiting for receiving SYNACK */
		gbnhdr *send_header = malloc(sizeof(gbnhdr));
		if (maybe_recvfrom(sockfd, (char *)send_header, sizeof(send_header), 0, tmp_sock, tmp_sock_len) == -1) {
			printf("receiver error in maybe_recvfrom syn ack\n");
			attempt ++;
			continue;
		}
		/* check for timeout, check if header type is SYNACK */
		if (check_packetType(send_header, SYNACK) == 0) {
			printf("receiver received synack header\n");
			s.state = ESTABLISHED;
			printf("receiver connection established\n");
			free(rec_header);
			return sockfd;
		}
		printf("received non-synack\n");
		printf("recived type: %d\n",send_header->type);
		attempt ++;
	}

	free(rec_header);
	return -1;
}

ssize_t maybe_recvfrom(int  s, char *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen){

	/*----- Packet not lost -----*/
	if (rand() > LOSS_PROB*RAND_MAX){


		/*----- Receiving the packet -----*/
		int retval = recvfrom(s, buf, len, flags, from, fromlen);

		/*----- Packet corrupted -----*/
		if (rand() < CORR_PROB*RAND_MAX){
			/*----- Selecting a random byte inside the packet -----*/
			int index = (int)((len-1)*rand()/(RAND_MAX + 1.0));

			/*----- Inverting a bit -----*/
			char c = buf[index];
			if (c & 0x01)
				c &= 0xFE;
			else
				c |= 0x01;
			buf[index] = c;
		}

		return retval;
	}
	/*----- Packet lost -----*/
	return (len);  /* Simulate a success */
}
/*
 *  Copyright (c) 2016 Thierry Leconte
 *
 *   
 *   This code is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License version 2
 *   published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <netdb.h>
#include "vdlm2.h"
#include "crc.h"
#include "acars.h"
#include "cJSON.h"

extern int verbose;
extern char *idstation;

extern int jsonout;
extern char* jsonbuf;
extern int sockfd;
extern  void outjson(void);

extern int DecodeLabel(acarsmsg_t *msg,oooi_t *oooi);

static void printmsg(acarsmsg_t * msg)
{
	fprintf(logfd, "ACARS\n");

	if (msg->mode < 0x5d) {
		fprintf(logfd, "Aircraft reg: %s ", msg->addr);
		fprintf(logfd, "Flight id: %s", msg->fid);
		fprintf(logfd, "\n");
	}
	fprintf(logfd, "Mode: %1c ", msg->mode);
	fprintf(logfd, "Msg. label: %s\n", msg->label);
	fprintf(logfd, "Block id: %c ", msg->bid);
	fprintf(logfd, "Ack: %c\n", msg->ack);
	fprintf(logfd, "Msg. no: %s\n", msg->no);
	fprintf(logfd, "Message :\n%s\n", msg->txt);
	if (msg->be == 0x17)
		fprintf(logfd, "Block End\n");

	fflush(logfd);
}

static int buildjson(unsigned int vaddr,acarsmsg_t * msg, int chn, int freqb, struct timeval tv)
{

	oooi_t oooi;
	cJSON *json_obj;
	int ok = 0;
	char convert_tmp[8];
#if defined (WITH_RTL) || defined (WITH_AIR)
        float freq = freqb / 1000000.0;
#else
        float freq = 0;
#endif

	json_obj = cJSON_CreateObject();
	if (json_obj == NULL)
		return ok;

	double t = (double)tv.tv_sec + ((double)tv.tv_usec)/1e6;
	cJSON_AddNumberToObject(json_obj, "timestamp", t);
	cJSON_AddStringToObject(json_obj, "station_id", idstation);

	cJSON_AddNumberToObject(json_obj, "channel", chn);
        snprintf(convert_tmp, sizeof(convert_tmp), "%3.3f", freq);
        cJSON_AddRawToObject(json_obj, "freq", convert_tmp);

	cJSON_AddNumberToObject(json_obj, "icao", vaddr & 0xffffff);
	snprintf(convert_tmp, sizeof(convert_tmp), "%c", msg->mode);

	cJSON_AddStringToObject(json_obj, "mode", convert_tmp);
	cJSON_AddStringToObject(json_obj, "label", msg->label);

	if(msg->bid) {
		snprintf(convert_tmp, sizeof(convert_tmp), "%c", msg->bid);
		cJSON_AddStringToObject(json_obj, "block_id", convert_tmp);

		if(msg->ack == 0x15) {
			cJSON_AddFalseToObject(json_obj, "ack");
		} else {
			snprintf(convert_tmp, sizeof(convert_tmp), "%c", msg->ack);
			cJSON_AddStringToObject(json_obj, "ack", convert_tmp);
		}

		cJSON_AddStringToObject(json_obj, "tail", msg->addr);
		if(msg->mode <= 'Z') {
			cJSON_AddStringToObject(json_obj, "flight", msg->fid);
			cJSON_AddStringToObject(json_obj, "msgno", msg->no);
		}
	}
	if(msg->txt[0])
		cJSON_AddStringToObject(json_obj, "text", msg->txt);

	if (msg->be == 0x17)
		cJSON_AddTrueToObject(json_obj, "end");

	if(DecodeLabel(msg, &oooi)) {
		if(oooi.sa[0])
			cJSON_AddStringToObject(json_obj, "depa", oooi.sa);
		if(oooi.da[0])
			cJSON_AddStringToObject(json_obj, "dsta", oooi.da);
		if(oooi.eta[0])
			cJSON_AddStringToObject(json_obj, "eta", oooi.eta);
		if(oooi.gout[0])
			cJSON_AddStringToObject(json_obj, "gtout", oooi.gout);
		if(oooi.gin[0])
			cJSON_AddStringToObject(json_obj, "gtin", oooi.gin);
		if(oooi.woff[0])
			cJSON_AddStringToObject(json_obj, "wloff", oooi.woff);
		if(oooi.won[0])
			cJSON_AddStringToObject(json_obj, "wlin", oooi.won);
	}
	ok = cJSON_PrintPreallocated(json_obj, jsonbuf, JSONBUFLEN, 0);
	cJSON_Delete(json_obj);
	return ok;
}

extern int sockfd_native;

#define STATION_ID_LENGTH 8

typedef struct _acars_udp_message_header_t acars_udp_message_header_t;
struct _acars_udp_message_header_t {
    char station_id[STATION_ID_LENGTH];
    unsigned int fc;
    unsigned int timestamp;
};

typedef struct _acars_udp_message_t acars_udp_message_t;
struct _acars_udp_message_t {
    acars_udp_message_header_t header;
    char payload[8192];
};

void outraw(const msgblk_t * blk, const unsigned char *txt, int len, time_t tm)
{
	acars_udp_message_t msg;

	memset(&msg, 0, sizeof(msg));
	strncpy(&msg.header.station_id[0], &idstation[0], STATION_ID_LENGTH);
	msg.header.timestamp = htonl(tm);
	msg.header.fc = htonl((unsigned)(blk->Fr / 1000.0));
	memcpy(&msg.payload[0], &txt[0], len);

	write(sockfd_native, &msg, sizeof(msg.header) + len);
}

void outacars(unsigned int vaddr,msgblk_t * blk,unsigned char *txt, int len)
{
	acarsmsg_t msg;
	int i, k, j;
	unsigned int crc;

	crc = 0;
	/* test crc, set le and remove parity */
	for (i = 0; i < len - 1; i++) {
		update_crc(crc, txt[i]);
		txt[i] &= 0x7f;
	}
	if (crc) {
		fprintf(logfd, "crc error\n");
	}

	/* fill msg struct */

	k = 0;
	msg.mode = txt[k];
	k++;

	for (i = 0, j = 0; i < 7; i++, k++) {
		if (txt[k] != '.') {
			msg.addr[j] = txt[k];
			j++;
		}
	}
	msg.addr[j] = '\0';

	/* ACK/NAK */
	msg.ack = txt[k];
	if (msg.ack == 0x15)
		msg.ack = '!';
	k++;

	msg.label[0] = txt[k];
	k++;
	msg.label[1] = txt[k];
	if (msg.label[1] == 0x7f)
		msg.label[1] = 'd';
	k++;
	msg.label[2] = '\0';

	msg.bid = txt[k];
	if (msg.bid == 0)
		msg.bid = ' ';
	k++;

	/* txt start  */
	msg.bs = txt[k];
	k++;

	msg.no[0] = '\0';
	msg.fid[0] = '\0';
	msg.txt[0] = '\0';

	if (msg.bs != 0x03) {
		if (msg.mode <= 'Z' && msg.bid <= '9') {
			/* message no */
			for (i = 0; i < 4 && k < len - 4; i++, k++) {
				msg.no[i] = txt[k];
			}
			msg.no[i] = '\0';

			/* Flight id */
			for (i = 0; i < 6 && k < len - 4; i++, k++) {
				msg.fid[i] = txt[k];
			}
			msg.fid[i] = '\0';
		}

		/* Message txt */
		for (i = 0; (k < len - 4); i++, k++)
			msg.txt[i] = txt[k];
		msg.txt[i] = 0;
	}
	msg.be = txt[k];

	if (verbose) {
		printmsg(&msg);
	}

	// build the JSON buffer if needed
	if(jsonbuf)
		buildjson(vaddr, &msg, blk->chn, blk->Fr, blk->tv);

	if(jsonout) {
		fprintf(logfd, "%s\n", jsonbuf);
		fflush(logfd);
	}

	if (sockfd >= 0) {
		outjson();
	}
	if (sockfd_native >= 0) {
		outraw();
	}

}

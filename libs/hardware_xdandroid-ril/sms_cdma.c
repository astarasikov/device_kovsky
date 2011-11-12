/* //device/system/reference-ril/atchannel.c
**
** Copyright 2011, Howard Chu hyc@highlandsun.com
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/
/* Encode/Decode CDMA SMS PDUs for Android RIL
 */
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "telephony/ril_cdma_sms.h"

#ifdef TESTER
static int hex2int(char c) {
	return (c&0x0f) + ((c & 0x40) ? 9 : 0);
}
#else
extern int hex2int(char c);
#endif

static int msgid;

static void decode_cdma_address(unsigned char *p, int len, RIL_CDMA_SMS_Address *addr) {
	unsigned short c;
	int shift;
	unsigned char *end = p + len;
	unsigned char *w;
	int mask8, mask4l, mask4h, shifth;

	c = *p++;
	addr->digit_mode = (c & 0x80) != 0;
	addr->number_mode = (c & 0x40) != 0;
	c <<= 2;
	shift = 2;
	if (addr->digit_mode) {
		addr->number_type = (c & 0xe0) >> 5;
		shift += 3;
		c <<= 3;
		if (!addr->number_mode) {
			c <<= 3;
				c |= (*p++);
			addr->number_plan = (c & 0x780) >> 7;
			shift = 1;
		}
	}
	w = addr->digits;
	if (shift == 1) {
		c <<= 8;
		mask8  = 0x7f80;
		mask4l = 0x0780;
		mask4h = 0x7800;
		shifth = 11;
		shift  = 7;
	} else if (shift == 2) {
		c <<= 6;
		mask8  = 0x3fc0;
		mask4l = 0x03c0;
		mask4h = 0x3c00;
		shifth = 10;
		shift  = 6;
	} else {
		c <<= 3;
		mask8 =  0x7f8;
		mask4l = 0x078;
		mask4h = 0x780;
		shifth = 7;
		shift  = 3;
	}
	c |= *p++;
	addr->number_of_digits = (c & mask8) >> shift;
	if (addr->digit_mode) {
		while (p<=end) {
			c <<= 8;
			c |= *p++;
			*w++ = (c & mask8) >> shift;
		}
	} else {
		while (p<=end) {
			c <<= 8;
			c |= *p++;
			*w++ = (c & mask4h) >> shifth;
			*w++ = (c & mask4l) >> shift;
		}
	}
}

static void decode_cdma_subaddress(unsigned char *p, int len, RIL_CDMA_SMS_Subaddress *addr) {
	unsigned short c;
	int shift;
	unsigned char *end = p + len;
	unsigned char *w;
	int mask8, mask4l, mask4h, shifth;

	c = *p++;
	addr->subaddressType = (c & 0xe0) >> 5;
	addr->odd = (c & 0x10) != 0;
	c <<= 8;
	c |= *p++;
	addr->number_of_digits = (c & 0xff0) >> 4;

	w = addr->digits;
	while (p<=end) {
		c <<= 8;
		c |= *p++;
		*w++ = (c & 0xff0) >> 4;
	}
}

void decode_cdma_sms_to_ril(unsigned char *pdu, RIL_CDMA_SMS_Message *msg) {
	int i;
	unsigned char *ptr, c;
	int msgtype;
	int len = strlen((char *)pdu);
	int pid, plen;

	/* convert hex 2 binary */
	ptr = pdu;
	for (i=0; i<len; i+=2) {
		c = (hex2int(pdu[i]) << 4) | hex2int(pdu[i+1]);
		*ptr++ = c;
	}
	len /= 2;
	ptr = pdu;
	msgtype = *ptr++;	/* 0 = point-to-point, 1 = broadcast, 2 = ack */
						/* Android doesn't seem to care ... */

	while (ptr < pdu+len) {
		pid = *ptr++;
		plen = *ptr++;
		switch(pid) {
		case 0:	/* Teleservice ID */
			msg->uTeleserviceID = (ptr[0] << 8) | ptr[1];
			break;
		case 1: /* Service Category */
			msg->bIsServicePresent = 1;
			msg->uServicecategory = (ptr[0] << 8) | ptr[1];
			break;
		case 2: /* Originating Address */
			decode_cdma_address(ptr, plen, &msg->sAddress);
			break;
		case 3: /* Originating Subaddress */
			decode_cdma_subaddress(ptr, plen, &msg->sSubAddress);
			break;
		case 8: /* Bearer Data */
			msg->uBearerDataLen = plen;
			memcpy(msg->aBearerData, ptr, plen);
			break;
#ifdef TESTER
		case 4:	/* Destination Address */
			decode_cdma_address(ptr, plen, &msg->sAddress);
			break;
		case 5: /* Destination Subaddress */
			decode_cdma_subaddress(ptr, plen, &msg->sSubAddress);
			break;
#endif
		case 6: /* Bearer Reply Option */
		case 7: /* Cause Codes */
		default:
			break;
		}
		ptr += plen;
	}
}

static unsigned char *putdigit(unsigned char *ptr, int d) {
	d &= 0x0f;
	*ptr++ = d + ((d>9) ? 'A' - 10 : '0');
	return ptr;
}

static unsigned char *putbyte(unsigned char *ptr, int b) {
	ptr = putdigit(ptr, b >> 4);
	return putdigit(ptr, b);
}

static unsigned char *
encode_cdma_address(unsigned char *ptr, RIL_CDMA_SMS_Address *addr) {
	unsigned char *r, *end;
	int bits = 10, bytes, i, c;
	int shift, mask;

	if (addr->digit_mode) {
		bits += addr->number_of_digits * 8;
		bits += 3;	/* number_type */
		if (!addr->number_mode)
			bits += 4;	/* number_plan */
	} else {
		bits += addr->number_of_digits * 4;
	}
	/* Round off to 8 bit bytes */
	bytes = bits+7;
	bytes ^= bytes & 7;

	/* Number of pad bits at end */
	bits = bytes - bits;

	ptr = putbyte(ptr, bytes/8);	/* len */
	end = ptr + bytes/4;

	c = 0;
	r = addr->digits;
	if (addr->digit_mode)
		c |= 0x80;
	if (addr->number_mode)
		c |= 0x40;
	if (addr->digit_mode) {
		c |= (addr->number_type & 0x07) << 3;
		if (!addr->number_mode) {
			c |= (addr->number_plan >>1);
			ptr = putbyte(ptr, c);
			if (addr->number_plan & 1)
				c = 0x80;
			else
				c = 0;
			shift = 1;
			mask = 0x7f;
		} else {
			shift = 3;
			mask = 0x1f;
		}
	} else {
		shift = 2;
		mask = 0x3f;
	}

	c |= (addr->number_of_digits >> shift) & mask;
	ptr = putbyte(ptr, c);
	c = addr->number_of_digits << (8-shift);

	if (addr->digit_mode) {
		for (i=0; i<addr->number_of_digits; i++) {
			int b = *r++;
			c |= (b >> shift) & mask;
			ptr = putbyte(ptr, c);
			c = b << (8-shift);
		}
	} else {
		for (i=0; i<addr->number_of_digits; i+=2) {
			int b = *r++ << 4;
			b |= *r++;
			c |= (b >> shift) & mask;
			ptr = putbyte(ptr, c);
			c = b << (8-shift);
		}
	}
	if (ptr < end) {
		ptr = putbyte(ptr, c);
	}
	return ptr;
}

static unsigned char *
encode_cdma_subaddress(unsigned char *ptr, RIL_CDMA_SMS_Subaddress *addr) {
	unsigned char *r;
	int bytes, i, c;
	int shift, mask;

	ptr = putbyte(ptr, addr->number_of_digits + 2);	/* len */

	c = addr->subaddressType << 1;
	c |= (addr->odd != 0);
	ptr = putdigit(ptr, c);

	ptr = putbyte(ptr, addr->number_of_digits);

	for (i=0; i<addr->number_of_digits; i++)
		ptr = putbyte(ptr, addr->digits[i]);

	ptr = putdigit(ptr, 0);	/* reserved */

	return ptr;
}

/* IN: msg, buffer, size of buffer
 * OUT: PDU in buffer, return length of data in buffer
 * Note: since buffer is hex encoded, this is actually 2x the number of msg bytes
 */
int encode_cdma_sms_from_ril(RIL_CDMA_SMS_Message *msg, unsigned char *buf, int len) {
	unsigned char *ptr, *end;
	int service = msg->uTeleserviceID;

	end = buf+len;
	ptr = putbyte(buf, 0);	/* msgtype = 00, point-to-point */

	/* It seems like the wrong ID is getting set by Android.
	 * Actual MMS messages are sent via HTTP, not thru here.
	 */
	if (service == 4101)	/* WEMT */
		service = 4098;		/* WMT */

	ptr = putbyte(ptr, 0);	/* Teleservice ID */
	ptr = putbyte(ptr, 2);	/* 2 bytes */
	ptr = putbyte(ptr, service >> 8);
	ptr = putbyte(ptr, service);

	if (msg->bIsServicePresent) {
		ptr = putbyte(ptr, 1);	/* Service Category */
		ptr = putbyte(ptr, 2);	/* 2 bytes */
		ptr = putbyte(ptr, msg->uServicecategory >> 8);
		ptr = putbyte(ptr, msg->uServicecategory);
	}

	if (msg->sAddress.number_of_digits) {
		ptr = putbyte(ptr, 4);	/* Destination address */
		ptr = encode_cdma_address(ptr, &msg->sAddress);
	}

	if (msg->sSubAddress.number_of_digits) {
		ptr = putbyte(ptr, 5);	/* Destination subaddress */
		ptr = encode_cdma_subaddress(ptr, &msg->sSubAddress);
	}

	ptr = putbyte(ptr, 6);	/* Bearer reply option */
	ptr = putbyte(ptr, 1);	/* 1 byte */
	ptr = putbyte(ptr, (++msgid) << 2);	/* two low bits are reserved */

	if (msg->uBearerDataLen) {
		int i;
		ptr = putbyte(ptr, 8);	/* Bearer data */
		ptr = putbyte(ptr, msg->uBearerDataLen);
		for (i=0; i<msg->uBearerDataLen; i++) {
			ptr = putbyte(ptr, msg->aBearerData[i]);
			if (ptr >= end) break;
		}
	}
	*ptr ='\0';
	return ptr - buf;
}

#ifdef TESTER
char ibuf[512], mbuf[512], ebuf[512];
main(int argc, char *argv[]) {
	RIL_CDMA_SMS_Message msg = {0};
	int fd, len, elen;
	char *ptr;
	if (argc == 2) {
		fd = open(argv[1], O_RDONLY);
	} else {
		fd = 0;
	}
	len = read(fd, ibuf, sizeof(mbuf));
	if (fd) close(fd);
	if (ibuf[len-1] == '\n') {
		ibuf[len-1] = '\0';
	}
	len--;
	memcpy(mbuf, ibuf, len);
	decode_cdma_sms_to_ril(mbuf, &msg);
	elen = encode_cdma_sms_from_ril(&msg, ebuf, sizeof(ebuf));
	/* Results will always mismatch because address type changed */
	if (elen != len || strcmp(ibuf, ebuf)) {
		fprintf(stderr, "input and output don't match!\n");
	}
}
#endif

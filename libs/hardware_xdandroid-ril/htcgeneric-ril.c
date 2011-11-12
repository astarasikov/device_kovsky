/* //device/system/htcgeneric-ril/htcgeneric-ril.c
 **
 ** Copyright 2006, The Android Open Source Project
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

#include <telephony/ril.h>
#include <telephony/ril_cdma_sms.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <pthread.h>
#include <alloca.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include "gsm.h"
#include <getopt.h>
#include <sys/socket.h>
#include <cutils/sockets.h>
#include <cutils/properties.h>
#include <termios.h>

#define LOG_NDEBUG 0
#define LOG_TAG "RIL"
#include <utils/Log.h>

#define MAX_AT_RESPONSE 0x1000

#define RIL_REQUEST_SEND_SMS_EXTENDED 512

/* pathname returned from RIL_REQUEST_SETUP_DATA_CALL / RIL_REQUEST_SETUP_DEFAULT_PDP */
#define PPP_TTY_PATH "ppp0"
#define PPP_SYS_PATH "/sys/class/net/ppp0/operstate"
#define PPP_RETRY_COUNT 15

static int ppp_set_state(int state) {
	return property_set("ril.xda.data_ready", state ? "true" : "false");
}

#ifdef USE_TI_COMMANDS

// Enable workaround for bug in (TI-based) HTC stack
// 1) Make incoming call, do not answer
// 2) Hangup remote end
// Expected: call should disappear from CLCC line
// Actual: Call shows as "ACTIVE" before disappearing
#define WORKAROUND_ERRONEOUS_ANSWER 1

// Some varients of the TI stack do not support the +CGEV unsolicited
// response. However, they seem to send an unsolicited +CME ERROR: 150
#define WORKAROUND_FAKE_CGEV 1
#endif

/* valid registration states */
#define REG_HOME	1
#define REG_ROAM	5

typedef enum {
	SIM_ABSENT = 0,
	SIM_NOT_READY = 1,
	SIM_READY = 2, /* SIM_READY means the radio state is RADIO_STATE_SIM_READY */
	SIM_PIN = 3,
	SIM_PUK = 4,
	SIM_NETWORK_PERSONALIZATION = 5
} SIM_Status;

/* These are the known types, from ril.h */
typedef enum {
	RADIO_UNKNOWN = 0,
	RADIO_GPRS,
	RADIO_EDGE,
	RADIO_UMTS,
	RADIO_IS95A,
	RADIO_IS95B,
	RADIO_1xRTT,
	RADIO_EVDO_0,
	RADIO_EVDO_A,
	RADIO_HSDPA,
	RADIO_HSUPA,
	RADIO_HSPA,
	RADIO_EVDO_B
} RADIO_Types;

static void onRequest (int request, void *data, size_t datalen, RIL_Token t);
static RIL_RadioState currentState();
static int onSupports (int requestCode);
static void onCancel (RIL_Token t);
static const char *getVersion();
static int isRadioOn();
static SIM_Status getSIMStatus();
static int getCardStatus(RIL_CardStatus **pp_card_status);
static void freeCardStatus(RIL_CardStatus *p_card_status);
static void onDataCallListChanged(void *param);
static int killConn(char * cid);


extern const char * requestToString(int request);

/*** Static Variables ***/
static const RIL_RadioFunctions s_callbacks = {
	RIL_VERSION,
	onRequest,
	currentState,
	onSupports,
	onCancel,
	getVersion
};

#ifdef RIL_SHLIB
static const struct RIL_Env *s_rilenv;

#define RIL_onRequestComplete(t, e, response, responselen) s_rilenv->OnRequestComplete(t,e, response, responselen)
#define RIL_onUnsolicitedResponse(a,b,c) s_rilenv->OnUnsolicitedResponse(a,b,c)
#define RIL_requestTimedCallback(a,b,c) s_rilenv->RequestTimedCallback(a,b,c)
#endif

static RIL_RadioState sState = RADIO_STATE_UNAVAILABLE;

static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_state_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t s_data_mutex = PTHREAD_MUTEX_INITIALIZER;

static int slow_sim=0;
static int s_port = -1;
static const char * s_device_path = NULL;
static int          s_device_socket = 0;
static const char *smd7 = "";

/* trigger change to this with s_state_cond */
static int s_closed = 0;

static int sFD;     /* file desc of AT channel */
static char sATBuffer[MAX_AT_RESPONSE+1];
static char *sATBufferCur = NULL;
	/* Higher layers expect a NITZ string in this format:
	 *  08/10/28,19:08:37-20,1 (yy/mm/dd,hh:mm:ss(+/-)tz,dst)
	 */
static char sNITZtime[sizeof("08/10/28,19:08:37-20,1")+4];

static const struct timeval TIMEVAL_SIMPOLL = {1,0};
static const struct timeval TIMEVAL_CALLSTATEPOLL = {1,0};
static const struct timeval TIMEVAL_0 = {0,0};

#ifdef WORKAROUND_ERRONEOUS_ANSWER
// Max number of times we'll try to repoll when we think
// we have a AT+CLCC race condition
#define REPOLL_CALLS_COUNT_MAX 4

// Line index that was incoming or waiting at last poll, or -1 for none
static int s_incomingOrWaitingLine = -1;
// Number of times we've asked for a repoll of AT+CLCC
static int s_repollCallsCount = 0;
// Should we expect a call to be answered in the next CLCC?
static int s_expectAnswer = 0;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

static void pollSIMState (void *param);
static void setRadioState(RIL_RadioState newState);

#define MODE_GSM	1
#define MODE_CDMA	2

static int phone_has = 0;	/* Modes that the phone hardware supports */
static int phone_is  = 0;	/* Mode the phone is in */
static int world_phone = 0;
static int done_first = 0;	/* Have we done poweron once before? */
static int switch_req = 0;	/* When switching modes, ignore everything until this req */

static char erisystem[50];
static char erishort[50];
static char operid[12];
static int countValidCalls=0;
static int signalStrength[2];
static char eriPRL[4];
static int got_state_change=0;
static int regstate;
static int gsm_rtype=-1;
static int gsm_selmode=-1;
static RADIO_Types android_rtype=RADIO_UNKNOWN;
static char imei[16+4];
static char esn[9];
static char prl[8];
static char min[12];
static char operID[12];
static int audio_on = 0;	/* is the audio on? */

typedef enum {
	Data_Off = 0,
	Data_Terminated,
	Data_Dialing,
	Data_Connected,
} Data_State;

static Data_State data_state=Data_Off;

static RIL_RadioState Radio_READY = RADIO_STATE_SIM_READY;
static RIL_RadioState Radio_NOT_READY = RADIO_STATE_SIM_NOT_READY;

static void handle_cdma_ccwa (const char *s)
{
	int err;
	char *line, *tmp;

	line = tmp = strdup(s);
	RIL_CDMA_CallWaiting resp;
	err = at_tok_start(&tmp);
	if (err)
		goto out;
	err = at_tok_nextstr(&tmp, &resp.number);
	if (err)
		goto out;
	resp.numberPresentation = strcspn(resp.number, "+0123456789") != 0;
	resp.name = NULL;
	RIL_onUnsolicitedResponse ( RIL_UNSOL_CDMA_CALL_WAITING,
			&resp, sizeof(resp));
out:
	free(line);
}

extern char** cdma_to_gsmpdu(const char *);
extern char* gsm_to_cdmapdu(const char *);
extern int hex2int(const char);
extern void decode_cdma_sms_to_ril(char *pdu, RIL_CDMA_SMS_Message *msg);
extern int encode_cdma_sms_from_ril(RIL_CDMA_SMS_Message *msg, char *buf, int buflen);

static void HexStr_to_DecInt(char *strings, unsigned int *ints)
{
	int i = 0;
	int j = strlen(strings);
	int k = 0;
	for(i = 0, k = 0; i < j; i += 2, k++)
	{
		printf("%d, %d\n", i, k);
		if(strings[i] <= 57){
			*(ints + k) += (unsigned int)((strings[i] - 48) * 16);
		}
		else{
			*(ints+k) += (unsigned int)(((strings[i] - 97) + 10) * 16);
		}

		if(strings[i+1] <= 57){
			*(ints+k) += (unsigned int)(strings[i+1] - 48);
		}
		else{
			*(ints+k) += (unsigned int)((strings[i+1] - 97) + 10);
		}
	}
}

static int clccStateToRILState(int state, RIL_CallState *p_state)

{
	switch(state) {
		case 0: *p_state = RIL_CALL_ACTIVE;   return 0;
		case 1: *p_state = RIL_CALL_HOLDING;  return 0;
		case 2: *p_state = RIL_CALL_DIALING;  return 0;
		case 3: *p_state = RIL_CALL_ALERTING; return 0;
		case 4: *p_state = RIL_CALL_INCOMING; return 0;
		case 5: *p_state = RIL_CALL_WAITING;  return 0;
		default: return -1;
	}
}

static const char* networkStatusToRilString(int state)
{
	switch(state){
		case 0: return("unknown");   break;
		case 1: return("available"); break;
		case 2: return("current");   break;
		case 3: return("forbidden"); break;
		default: return NULL;
	}
}

// some phone functions are controlled by msm_proc_comm through /sys
void writesys(char *name, char *val) {
	FILE *fout;
	char filename[256];
	strcpy(filename,"/sys/class/vogue_hw/");
	strcat(filename,name);
	fout=fopen(filename,"w");
	if(!fout) return;
	fprintf(fout, "%s", val);
	fclose(fout);
}
/**
 * Note: directly modified line and has *p_call point directly into
 * modified line
 */
static int callFromCLCCLine(char *line, RIL_Call *p_call)
{
	//+CLCC: 1,0,2,0,0,\"+18005551212\",145
	//     index,isMT,state,mode,isMpty(,number,TOA)?

	int err;
	int state;
	int mode;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &(p_call->index));
	if (err < 0) goto error;

	err = at_tok_nextbool(&line, &(p_call->isMT));
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &state);
	if (err < 0) goto error;

	err = clccStateToRILState(state, &(p_call->state));
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &mode);
	if (err < 0) goto error;

	p_call->isVoice = (mode == 0);

	err = at_tok_nextbool(&line, &(p_call->isMpty));
	if (err < 0) goto error;

	if (at_tok_hasmore(&line)) {
		err = at_tok_nextstr(&line, &(p_call->number));

		/* tolerate null here */
		if (err < 0) return 0;

		// Some lame implementations return strings
		// like "NOT AVAILABLE" in the CLCC line
		if (p_call->number != NULL
				&& 0 == strspn(p_call->number, "+0123456789")
		   ) {
			p_call->number = NULL;
		}

		err = at_tok_nextint(&line, &p_call->toa);
		if (err < 0) goto error;
	}

	return 0;

error:
	LOGE("invalid CLCC line\n");
	return -1;
}

//returns the call number of the active data call, or -1 if no active call
static int dataCallNum()
{
	int err;
	ATResponse *p_response;
	ATLine *p_cur;
	int countCalls;
	RIL_Call *p_call;
	int i;
	int callNumber = -1;

	if(currentState() != Radio_READY){
		return -1;
	}

	err = at_send_command_multiline ("AT+CLCC", "+CLCC:", &p_response);

	if (err != 0 || p_response->success == 0) {
		return -1;
	}

	/* count the calls */
	for (countCalls = 0, p_cur = p_response->p_intermediates
			; p_cur != NULL
			; p_cur = p_cur->p_next
	    ) {
		countCalls++;
	}

	p_call = (RIL_Call *)alloca(sizeof(RIL_Call));
	memset (p_call, 0, sizeof(RIL_Call));

	for (i = 0, p_cur = p_response->p_intermediates
			; p_cur != NULL
			; p_cur = p_cur->p_next
	    ) {
		err = callFromCLCCLine(p_cur->line, p_call);

		if (err != 0) {
			continue;
		}

		if(p_call[0].isVoice == 0) // only count data calls
		{
			callNumber = i;
		}
	}
	return callNumber;
}

/** do post-AT+CFUN=1 initialization */
static void onRadioPowerOn()
{
#ifdef USE_TI_COMMANDS
	/*  Must be after CFUN=1 */
	/*  TI specific -- notifications for CPHS things such */
	/*  as CPHS message waiting indicator */

	at_send_command("AT%CPHS=1", NULL);

	/*  TI specific -- enable NITZ unsol notifs */
	at_send_command("AT%CTZV=1", NULL);
#endif

	if(phone_is == MODE_GSM)
	{
		if (!done_first) {
			at_send_command("AT+BANDSET=0", NULL);
			at_send_command("AT+CGAATT=2,2,0", NULL);
		}

//		at_send_command("AT+ALS=4294967295", NULL);

		at_send_command("AT+ODEN=112", NULL);
		at_send_command("AT+ODEN=911", NULL);
		at_send_command("AT+ODEN=000", NULL);
		at_send_command("AT+ODEN=08", NULL);
		at_send_command("AT+ODEN=110", NULL);
		at_send_command("AT+ODEN=118", NULL);
		at_send_command("AT+ODEN=119", NULL);

		pollSIMState(NULL);
	} else {
		if (!done_first) {
//			at_send_command("AT+PREV_CDMA=1", NULL);
			at_send_command("AT+CGAATT=2,1,6", NULL);	/* '6'=CDMA-only mode, '3'=GSM-only, '0'=world mode */
		}
		at_send_command("AT+HTC_GPSONE=4", NULL);
		at_send_command("AT+CLVL=102", NULL);

		setRadioState(Radio_READY);
	}
	done_first = 1;
}

/** do post- SIM ready initialization */
static void onRadioReady()
{
	/* Common initialization commands */

	/* Network registration */
	at_send_command("AT+COPS=0", NULL);

	if(phone_is == MODE_GSM) {
		/* Preferred RAT - UMTS Dualmode */
//		at_send_command("AT+XRAT=1,2", NULL);

		/*
		 * Always send SMS messages directly to the TE
		 *
		 * mode = 1 // discard when link is reserved (link should never be
		 *             reserved)
		 * mt = 2   // most messages routed to TE
		 * bm = 2   // new cell BM's routed to TE
		 * ds = 1   // Status reports routed to TE
		 * bfr = 1  // flush buffer
		 */
//		at_send_command("AT+CNMI=1,2,2,1,1", NULL);

		at_send_command("AT+CSCB=1", NULL);

		/*  Enable +CGEV GPRS event notifications, but don't buffer */
//		at_send_command("AT+CGEREP=1,0", NULL);

		/* Enable NITZ reporting - already done */
//		at_send_command("AT+CTZU=1", NULL);
//		at_send_command("AT+CTZR=1", NULL);
//		at_send_command("AT+HTCCTZR=1", NULL);

		/* Enable unsolizited RSSI reporting */
		at_send_command("AT@HTCCSQ=1", NULL);

		at_send_command_singleline("AT+CSMS=1", "+CSMS:", NULL);
	}
}

static void setPhoneMode()
{
	if (phone_is == MODE_CDMA) {
		Radio_READY = RADIO_STATE_NV_READY;
		Radio_NOT_READY = RADIO_STATE_NV_NOT_READY;
		LOGD("Using CDMA Phone");
	} else {
		Radio_READY = RADIO_STATE_SIM_READY;
		Radio_NOT_READY = RADIO_STATE_SIM_NOT_READY;
		LOGD("Using GSM Phone");
	}
	setRadioState(Radio_NOT_READY);
}

static void requestRadioPower(void *data, size_t datalen, RIL_Token t)
{
	int onOff;

	int err;
	ATResponse *p_response = NULL;

	assert (datalen >= sizeof(int *));
	onOff = ((int *)data)[0];

	if (onOff == 0 && sState != RADIO_STATE_OFF) {
		if((phone_has & MODE_GSM) || world_phone)
			err = at_send_command("AT+CFUN=0", &p_response);
		else
			err = at_send_command("AT+CFUN=66", &p_response);
		if (err < 0 || p_response->success == 0) goto error;
		setRadioState(RADIO_STATE_OFF);
	} else if (onOff > 0 && sState == RADIO_STATE_OFF) {
		char value[PROPERTY_VALUE_MAX];
		err = at_send_command("AT+CFUN=1", &p_response);
		if (err < 0|| p_response->success == 0) {
			// Some stacks return an error when there is no SIM,
			// but they really turn the RF portion on
			// So, if we get an error, let's check to see if it
			// turned on anyway

			if (isRadioOn() != 1) {
				goto error;
			}
		}
		property_get("gsm.current.phone-type", value, "1");
		if (value[0] == '2') {
			phone_is = MODE_CDMA;
		} else {
			phone_is = MODE_GSM;
		}
		/* did we get a mode the phone doesn't support? */
		if (!(phone_is & phone_has)) {
			phone_is = phone_has;
			if (phone_is == MODE_CDMA)
				switch_req = RIL_REQUEST_CDMA_SUBSCRIPTION;
			/* else what? Dunno what GSM should use */
		}
		setPhoneMode();
	}

	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;

error:
	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestOrSendDataCallList(RIL_Token *t);

static void onDataCallListChanged(void *param)
{
	requestOrSendDataCallList(NULL);
}

static void requestDataCallList(void *data, size_t datalen, RIL_Token t)
{
	requestOrSendDataCallList(&t);
}

static void requestOrSendDataCallList(RIL_Token *t)
{
	ATResponse *p_response;
	ATLine *p_cur;
	RIL_Data_Call_Response *responses;
	int err;
	char status[1];
	int n = 0;
	char *out;

	if (phone_is == MODE_GSM) {
		err = at_send_command_multiline ("AT+CGACT?", "+CGACT:", &p_response);
		if (err != 0 || p_response->success == 0) {
			if (t != NULL)
				RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
			else
				RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
						NULL, 0);
			return;
		}

		for (p_cur = p_response->p_intermediates; p_cur != NULL;
				p_cur = p_cur->p_next)
			n++;

		responses = alloca(n * sizeof(RIL_Data_Call_Response));

		int i;
		for (i = 0; i < n; i++) {
			responses[i].cid = -1;
			responses[i].active = -1;
			responses[i].type = "";
			responses[i].apn = "";
			responses[i].address = "";
		}

		RIL_Data_Call_Response *response = responses;
		for (p_cur = p_response->p_intermediates; p_cur != NULL;
				p_cur = p_cur->p_next) {
			char *line = p_cur->line;

			err = at_tok_start(&line);
			if (err < 0)
				goto error;

			err = at_tok_nextint(&line, &response->cid);
			if (err < 0)
				goto error;

			err = at_tok_nextint(&line, &response->active);
			if (err < 0)
				goto error;

			response++;
		}

		at_response_free(p_response);

		err = at_send_command_multiline ("AT+CGDCONT?", "+CGDCONT:", &p_response);
		if (err != 0 || p_response->success == 0) {
			if (t != NULL)
				RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
			else
				RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
						NULL, 0);
			return;
		}

		for (p_cur = p_response->p_intermediates; p_cur != NULL;
				p_cur = p_cur->p_next) {
			char *line = p_cur->line;
			int cid;
			char *type;
			char *apn;
			char *address;


			err = at_tok_start(&line);
			if (err < 0)
				goto error;

			err = at_tok_nextint(&line, &cid);
			if (err < 0)
				goto error;

			for (i = 0; i < n; i++) {
				if (responses[i].cid == cid)
					break;
			}

			if (i >= n) {
				/* details for a context we didn't hear about in the last request */
				continue;
			}

			err = at_tok_nextstr(&line, &out);
			if (err < 0)
				goto error;

			responses[i].type = alloca(strlen(out) + 1);
			strcpy(responses[i].type, out);

			err = at_tok_nextstr(&line, &out);
			if (err < 0)
				goto error;

			responses[i].apn = alloca(strlen(out) + 1);
			strcpy(responses[i].apn, out);

			err = at_tok_nextstr(&line, &out);
			if (err < 0)
				goto error;

			responses[i].address = alloca(strlen(out) + 1);
			strcpy(responses[i].address, out);
		}

		at_response_free(p_response);

	} else {
		//CDMA
		n = 1;
		responses = alloca(sizeof(RIL_Data_Call_Response));

		responses[0].cid = 1;
		if (dataCallNum() >= 0)
			responses[0].active = 1;
		else
			responses[0].active = 0;
		responses[0].type = "IP";
		responses[0].apn = "internet";
		responses[0].address = "";
	}

	// make sure pppd is still running, invalidate datacall if it isn't
	if (access(PPP_SYS_PATH, F_OK) || data_state < Data_Connected)
	{
		responses[0].active = 0;
	}

	if (t != NULL)
		RIL_onRequestComplete(*t, RIL_E_SUCCESS, responses,
				n * sizeof(RIL_Data_Call_Response));
	else
		RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
				responses,
				n * sizeof(RIL_Data_Call_Response));

	return;

error:
	at_response_free(p_response);
	if (t != NULL)
		RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
	else
		RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
				NULL, 0);
}

static void requestBasebandVersion(void *data, size_t datalen, RIL_Token t)
{
	int err;
	ATResponse *p_response = NULL;
	char* line = NULL;
	char response[256], *ptr;

	/* AMSS version */
	/* Old CDMA phones don't support RADIOVER command */
	if (phone_has == MODE_CDMA && !world_phone) {
		char *comma;
		int len = 128;
		err = at_send_command_singleline("AT+HTC_RSINFO=0", "+HTC_RSINFO:", &p_response);
		if (err != 0 || !p_response->success) goto error;

		line = p_response->p_intermediates->line;

		at_tok_start(&line);
		err = at_tok_nextstr(&line, &ptr);
		if (err < 0) goto error;
		comma = strchr(ptr, ',');
		if (comma)
			len = comma - ptr;
		strncpy(response, ptr, len);
		response[len] = '\0';
		at_response_free(p_response);
	} else {
		err = at_send_command_singleline("AT+RADIOVER", "+RADIOVER:", &p_response);
		if (err != 0 || !p_response->success) goto error;

		line = p_response->p_intermediates->line;

		at_tok_start(&line);
		err = at_tok_nextstr(&line, &ptr);
		if (err < 0) goto error;
		strncpy(response, ptr, 128);
		response[128] = '\0';
		at_response_free(p_response);
	}

	/* Radio version */
	err = at_send_command_singleline("AT@v", "", &p_response);
	if (err != 0 || !p_response->success) goto error;

	line = p_response->p_intermediates->line;
	err = at_tok_nextstr(&line, &ptr);
	if (err < 0) goto error;
	strcat(response, "_");
	strncat(response, ptr, 127);
	response[255] = '\0';
	at_response_free(p_response);

	RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(char *));
	return;

error:
	at_response_free(p_response);
	LOGE("ERROR: requestBasebandVersion failed\n");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

/* This command is GSM only */
static void requestQueryNetworkSelectionMode(
		void *data, size_t datalen, RIL_Token t)
{
	int err;
	ATResponse *p_response = NULL;
	int response = 0;
	char *line;

	if(regstate != REG_HOME && regstate != REG_ROAM) {
		RIL_onRequestComplete(t, RIL_E_OP_NOT_ALLOWED_BEFORE_REG_TO_NW, NULL, 0);
		return;
	}

	if(phone_is == MODE_GSM) { //this command conflicts with the network status command
		if (gsm_selmode == -1) {
			err = at_send_command_singleline("AT+COPS?", "+COPS:", &p_response);

			if (err < 0 || p_response->success == 0) {
				goto error;
			}

			line = p_response->p_intermediates->line;

			err = at_tok_start(&line);

			if (err < 0) {
				goto error;
			}

			err = at_tok_nextint(&line, &response);

			if (err < 0) {
				goto error;
			}
		} else {
			response = gsm_selmode;
		}
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
	at_response_free(p_response);
	return;

error:
	at_response_free(p_response);
	LOGE("requestQueryNetworkSelectionMode must never return error when radio is on");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestQueryAvailableNetworks(void *data, size_t datalen, RIL_Token t)
{
	/* We expect an answer on the following form:
	   +COPS: (2,"AT&T","AT&T","310410",0),(1,"T-Mobile ","TMO","310260",0)
	 */

	int err, operators, i, skip, status;
	ATResponse *p_response = NULL;
	char * c_skip, *line, *p = NULL;
	char ** response = NULL;

	err = at_send_command_singleline("AT+COPS=?", "+COPS:", &p_response);

	if (err != 0 || !p_response->success) goto error;

	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	/* Count number of '(' in the +COPS response to get number of operators*/
	operators = 0;
	for (p = line ; *p != '\0' ;p++) {
		if (*p == '(') operators++;
	}

	response = (char **)alloca(operators * 4 * sizeof(char *));

	for (i = 0 ; i < operators ; i++ )
	{
		err = at_tok_nextstr(&line, &c_skip);
		if (err < 0) goto error;
		if (strcmp(c_skip,"") == 0)
		{
			operators = i;
			continue;
		}
		status = atoi(&c_skip[1]);
		response[i*4+3] = (char*)networkStatusToRilString(status);

		err = at_tok_nextstr(&line, &(response[i*4+0]));
		if (err < 0) goto error;

		err = at_tok_nextstr(&line, &(response[i*4+1]));
		if (err < 0) goto error;

		err = at_tok_nextstr(&line, &(response[i*4+2]));
		if (err < 0) goto error;

		err = at_tok_nextstr(&line, &c_skip);

		if (err < 0) goto error;
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, response, (operators * 4 * sizeof(char *)));
	at_response_free(p_response);
	return;

error:
	at_response_free(p_response);
	LOGE("ERROR - requestQueryAvailableNetworks() failed");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestGetPreferredNetworkType(void *data, size_t datalen, RIL_Token t)
{
	int err;
	ATResponse *p_response = NULL;
	int response = 0, r2;
	char *line;

	err = at_send_command_singleline("AT+CGAATT?", "+CGAATT:", &p_response);

	if (err < 0 || p_response->success == 0) {
		goto error;
	}

	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);

	if (err < 0) {
		goto error;
	}

	// Get third int in response
	err = at_tok_nextint(&line, &response);
	if (err < 0) {
		goto error;
	}
	err = at_tok_nextint(&line, &r2);
	if (err < 0) {
		goto error;
	}
	err = at_tok_nextint(&line, &response);
	if (err < 0) {
		goto error;
	}
	response += r2 * 10;
	switch(response) {
	case 20: response = 0; break;
	case 11: response = 1; break;
	case 22: response = 2; break;
	case 13: response = 3; break;
	case 16: response = 4; break;
	case 14: response = 5; break;
	case 15: response = 6; break;
	case 10: response = 7; break;
	}
	RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
	at_response_free(p_response);
	return;

error:
	at_response_free(p_response);
	LOGE("ERROR: requestGetPreferredNetworkType() failed\n");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSetPreferredNetworkType(void *data, size_t datalen, RIL_Token t)
{
	int err, rat;
	ATResponse *p_response = NULL;
	char cmd[sizeof("AT+CGAATT=2,1,0")];
	const char *at_rat = NULL;
	int newmode;

	assert (datalen >= sizeof(int *));
	rat = ((int *)data)[0];

#ifdef USE_IDCC_MODEM
	switch (rat)
	{
		case 0: at_rat = "1,2"; break;/* Dual Mode - WCDMA preferred*/
		case 1: at_rat = "0"; break;  /* GSM only */
		case 2: at_rat = "2"; break;  /* WCDMA only */
	}

	/* Need to unregister from NW before changing preferred RAT */
	err = at_send_command("AT+COPS=2", NULL);
	if (err < 0) goto error;

	sprintf(cmd, "AT+XRAT=%s", at_rat);
#else
	LOGD("In requestSetPreferredNetworkType RAPH");
	switch (rat) {
		case 0:
		case 1:
		case 2:
		case 3:
			newmode = MODE_GSM; break;
		case 4:
		case 5:
		case 6:
			newmode = MODE_CDMA; break;
		case 7:
			newmode = phone_is; break;
		default:
			newmode = 0; break;	/* unknown mode */
	}
	if (!(newmode & phone_has)) {
		RIL_onRequestComplete(t, RIL_E_MODE_NOT_SUPPORTED, NULL, 0);
		return;
	}
	/* Note: the G1 ril uses these values. Don't know why case 0 and 2
	 * use '2' in the middle value.
	 */
	switch (rat) {
		case 0: at_rat = "2,2,0"; break;/* Dual Mode - WCDMA preferred*/
		case 1: at_rat = "2,1,1"; break;  /* GSM only */
		case 2: at_rat = "2,2,2"; break;  /* WCDMA only */
		case 3: at_rat = "2,1,3"; break;  /* GSM auto (PRL) */
		case 4: at_rat = "2,1,6"; break;  /* CDMA auto (PRL) */
		case 5: at_rat = "2,1,4"; break;  /* CDMA only */
		case 6: at_rat = "2,1,5"; break;  /* EvDO only */
		case 7: at_rat = "2,1,0"; break;  /* GSM/CDMA auto (PRL) */
	}

	/* For some reason, without the bandset command, the CGAATT
	one fails. [mdrobnak] */
	err = at_send_command("AT+BANDSET=0", NULL);
	if (err < 0) goto error;

	sprintf(cmd, "AT+CGAATT=%s", at_rat);

#endif /* USE_IDCC_MODEM */
	err = at_send_command(cmd, &p_response);

	if (err < 0|| p_response->success == 0) {
		goto error;
	}

	if (phone_is != newmode) {
		phone_is = newmode;
		setPhoneMode();
	} else {
		/* Register on the NW again */
		err = at_send_command("AT+COPS=0", NULL);
		if (err < 0) goto error;
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, sizeof(int));
	at_response_free(p_response);
	return;
error:
	at_response_free(p_response);
	LOGE("ERROR: requestSetPreferredNetworkType() failed\n");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}


static void requestQueryFacilityLock(void *data, size_t datalen, RIL_Token t)
{
	int err, rat, response;
	ATResponse *p_response = NULL;
	char * cmd = NULL;
	char * line = NULL;
	char * facility_string = NULL;
	char * facility_password = NULL;
	char * facility_class = NULL;

	LOGD("FACILITY");
	assert (datalen >=  (3 * sizeof(char **)));

	facility_string   = ((char **)data)[0];
	facility_password = ((char **)data)[1];
	facility_class    = ((char **)data)[2];


	asprintf(&cmd, "AT+CLCK=\"%s\",2,\"%s\",%s", facility_string, facility_password, facility_class);
	err = at_send_command_singleline(cmd,"+CLCK:", &p_response);
	free(cmd);
	if (err < 0 || p_response->success == 0){
		goto error;
	}

	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);

	if (err < 0) {
		goto error;
	}

	err = at_tok_nextint(&line, &response);

	if (err < 0) {
		goto error;
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
	at_response_free(p_response);
	return;

error:
	at_response_free(p_response);
	LOGE("ERROR: requestQueryFacilityLock() failed\n");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static int resentCallState;

static void sendCallStateChanged(void *param)
{
	RIL_onUnsolicitedResponse (
			RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
			NULL, 0);
	resentCallState = 2;	/* notification needs ack */
}

static void requestGetCurrentCalls(void *data, size_t datalen, RIL_Token t)
{
	int err,fd;
	ATResponse *p_response;
	ATLine *p_cur;
	int countCalls;
	RIL_Call *p_calls;
	RIL_Call **pp_calls;
	int i;
	char status[1];
	int needRepoll = 0;
	char fake_clcc[64];

#ifdef WORKAROUND_ERRONEOUS_ANSWER
	int prevIncomingOrWaitingLine;

	prevIncomingOrWaitingLine = s_incomingOrWaitingLine;
	s_incomingOrWaitingLine = -1;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

	if(currentState() != Radio_READY){
		/* Might be waiting for SIM PIN */
		RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
	}

	resentCallState = 0;	/* any pending notifications are ack'd now */

	err = at_send_command_multiline ("AT+CLCC", "+CLCC:", &p_response);

	if (err != 0 || p_response->success == 0) {
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
		return;
	}

	/* count the calls */
	for (countCalls = 0, p_cur = p_response->p_intermediates
			; p_cur != NULL
			; p_cur = p_cur->p_next
	    ) {
		countCalls++;
	}

	/* yes, there's an array of pointers and then an array of structures */

	pp_calls = (RIL_Call **)alloca(countCalls * sizeof(RIL_Call *));
	p_calls = (RIL_Call *)alloca(countCalls * sizeof(RIL_Call));
	memset (p_calls, 0, countCalls * sizeof(RIL_Call));

	/* init the pointer array */
	for(i = 0; i < countCalls ; i++) {
		pp_calls[i] = &(p_calls[i]);
	}

	for (countValidCalls = 0, p_cur = p_response->p_intermediates
			; p_cur != NULL
			; p_cur = p_cur->p_next
	    ) {
		err = callFromCLCCLine(p_cur->line, p_calls + countValidCalls);

		if (err != 0) {
			continue;
		}

#ifdef WORKAROUND_ERRONEOUS_ANSWER
		if (p_calls[countValidCalls].state == RIL_CALL_INCOMING
				|| p_calls[countValidCalls].state == RIL_CALL_WAITING
		   ) {
			s_incomingOrWaitingLine = p_calls[countValidCalls].index;
		}
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

		if (p_calls[countValidCalls].state != RIL_CALL_ACTIVE
				&& p_calls[countValidCalls].state != RIL_CALL_HOLDING
		   ) {
			needRepoll = 1;
		}
		if(p_calls[countValidCalls].isVoice) // only count voice calls
			countValidCalls++;
	}

#ifdef WORKAROUND_ERRONEOUS_ANSWER
	// Basically:
	// A call was incoming or waiting
	// Now it's marked as active
	// But we never answered it
	//
	// This is probably a bug, and the call will probably
	// disappear from the call list in the next poll
	if (prevIncomingOrWaitingLine >= 0
			&& s_incomingOrWaitingLine < 0
			&& s_expectAnswer == 0
	   ) {
		for (i = 0; i < countValidCalls ; i++) {

			if (p_calls[i].index == prevIncomingOrWaitingLine
					&& p_calls[i].state == RIL_CALL_ACTIVE
					&& s_repollCallsCount < REPOLL_CALLS_COUNT_MAX
			   ) {
				LOGI(
						"Hit WORKAROUND_ERRONOUS_ANSWER case."
						" Repoll count: %d\n", s_repollCallsCount);
				s_repollCallsCount++;
				goto error;
			}
		}
	}

	s_expectAnswer = 0;
	s_repollCallsCount = 0;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/
	LOGI("Calls=%d,Valid=%d\n",countCalls,countValidCalls);
	if(countValidCalls==0 && audio_on) { // close audio if no voice calls.
		LOGI("Audio Close\n");
		writesys("audio","5");
		audio_on = 0;
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, pp_calls,
			countValidCalls * sizeof (RIL_Call *));

	at_response_free(p_response);

#ifdef POLL_CALL_STATE
	if (countValidCalls)  // We don't seem to get a "NO CARRIER" message from
		// smd, so we're forced to poll until the call ends.
#else
	if (needRepoll)
#endif
	{
		if (!resentCallState) {
			resentCallState = 1;	/* notification is queued */
			RIL_requestTimedCallback (sendCallStateChanged, NULL, &TIMEVAL_CALLSTATEPOLL);
		}
	}
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	at_response_free(p_response);
}

static void requestDial(void *data, size_t datalen, RIL_Token t)
{
	RIL_Dial *p_dial;
	char *cmd;
	const char *clir;
	int ret;

	p_dial = (RIL_Dial *)data;

	switch (p_dial->clir) {
		case 1: clir = "I"; break;  /*invocation*/
		case 2: clir = "i"; break;  /*suppression*/
		default:
		case 0: clir = ""; break;   /*subscription default*/
	}
	writesys("audio","2");
	audio_on = 1;
	asprintf(&cmd, "ATD%s%s;", p_dial->address, clir);

	ret = at_send_command(cmd, NULL);

	free(cmd);

	/* success or failure is ignored by the upper layer here.
	   it will call GET_CURRENT_CALLS and determine success that way */
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestWriteSmsToSim(void *data, size_t datalen, RIL_Token t)
{
	RIL_SMS_WriteArgs *p_args;
	char *cmd;
	int length;
	int err;
	ATResponse *p_response = NULL;

	if(phone_has & MODE_GSM) {

		p_args = (RIL_SMS_WriteArgs *)data;

		length = strlen(p_args->pdu)/2;
		asprintf(&cmd, "AT+CMGW=%d,%d", length, p_args->status);

		err = at_send_command_sms(cmd, p_args->pdu, "+CMGW:", &p_response);

		free(cmd);

		if (err != 0 || p_response->success == 0) goto error;

		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
		at_response_free(p_response);
	} else {
		RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
	}
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	at_response_free(p_response);
}

static void requestHangup(void *data, size_t datalen, RIL_Token t)
{
	int *p_line;

	int ret;
	char *cmd;

	p_line = (int *)data;

	// 3GPP 22.030 6.5.5
	// "Releases a specific active call X"
	asprintf(&cmd, "AT+CHLD=1%d", p_line[0]);

	ret = at_send_command(cmd, NULL);

	free(cmd);
	//	writesys("audio","5");

	/* success or failure is ignored by the upper layer here.
	   it will call GET_CURRENT_CALLS and determine success that way */
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void resp2Strength(int *response, RIL_SignalStrength *rs)
{
	const int dbm_table[8] = {135,125,115,105,95,85,75,70};
	const int edbm_table[8] = {120,110,100,90,80,75,70,65};
	const int ecio_table[8] = {200,150,130,120,110,100,90,80};

	if (phone_is == MODE_GSM) {
		rs->GW_SignalStrength.signalStrength = response[0];
		rs->GW_SignalStrength.bitErrorRate = response[1];
	} else {
		/* 1st # is CDMA, 2nd is EVDO */
		rs->CDMA_SignalStrength.dbm = dbm_table[response[0]];
		rs->CDMA_SignalStrength.ecio = ecio_table[response[0]];
		rs->EVDO_SignalStrength.dbm = edbm_table[response[1]];
		rs->EVDO_SignalStrength.ecio = ecio_table[response[1]];
		rs->EVDO_SignalStrength.signalNoiseRatio = response[1];
	}
}

static void requestSignalStrength(void *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response = NULL;
	int err;
	int response[2];
	RIL_SignalStrength rs = {{99,99},{-1,-1},{-1,-1,-1}};
	char *line;

	/* If we have no recent report, ask */
	if(signalStrength[0] == 0 && signalStrength[1] == 0) {
		if(phone_is == MODE_GSM)
			err = at_send_command_singleline("AT+CSQ", "+CSQ:", &p_response);
		else
			err = at_send_command_singleline("AT+HTC_CSQ", "+HTC_CSQ:", &p_response);

		if (err < 0 || p_response->success == 0) {
			RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
			goto error;
		}

		line = p_response->p_intermediates->line;

		err = at_tok_start(&line);
		if (err < 0) goto error;

		err = at_tok_nextint(&line, &(response[0]));
		if (err < 0) goto error;

		err = at_tok_nextint(&line, &(response[1]));
		if (err < 0) goto error;

		at_response_free(p_response);
	} else {
		LOGD("Sending stored RSSI values to RIL");
		response[0] = signalStrength[0];
		response[1] = signalStrength[1];
		signalStrength[0] = 0;
		signalStrength[1] = 0;
	}

	resp2Strength(response, &rs);

	RIL_onRequestComplete(t, RIL_E_SUCCESS, &rs, sizeof(rs));
	return;

error:
	LOGE("requestSignalStrength must never return an error when radio is on");
	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static char lastDtmf;

/* CDMA doesn't use these */
static void requestDtmfStart(void *data, size_t datalen, RIL_Token t)
{
	int err;
	char cmd[sizeof("AT$VTS=*,1")];

	assert (datalen >= sizeof(char *));

	lastDtmf = ((char *)data)[0];
	if (phone_is == MODE_GSM)
		sprintf(cmd, "AT$VTS=%c,1", (int)lastDtmf);
	else
		sprintf(cmd, "AT+VTS=%c", (int)lastDtmf);

	err = at_send_command(cmd, NULL);
	if (err != 0) goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;

error:
	LOGE("ERROR: requestDtmfStart failed");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

}

static void requestDtmfStop(void *data, size_t datalen, RIL_Token t)
{
	int err;
	char cmd[sizeof("AT$VTS=*,0")];

	assert (datalen >= sizeof(char *));

	if (phone_is == MODE_GSM)
		sprintf(cmd, "AT$VTS=%c,0", lastDtmf);
	else
		sprintf(cmd, "AT");

	err = at_send_command(cmd, NULL);
	if (err != 0) goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;

error:
	LOGE("ERROR: requestDtmfStop failed");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

}

static void requestCdmaBurstDtmf(void *data, size_t datalen, RIL_Token t)
{
	int err, dur = 0;
	char cmd[sizeof("AT+VTS=*,999")], *c;

	assert (datalen >= sizeof(char *));

	/* There are actually 3 parameters, 3rd is off time.
	 * Not implemented here.
	 */
	if (datalen > sizeof(char *)) {
		c = ((char **)data)[1];
		if (sscanf(c, "%d", &dur) != 1)
			goto error;
	}
	c = ((char **)data)[0];
	while (*c) {
		if (dur)
			sprintf(cmd, "AT+VTS=%c,%d", *c, dur);
		else
			sprintf(cmd, "AT+VTS=%c", *c);

		err = at_send_command(cmd, NULL);
		if (err != 0) goto error;
		c++;
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;

error:
	LOGE("ERROR: requestCdmaBurstDtmf failed");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSetMute(void *data, size_t datalen, RIL_Token t)
{
	int err;
	char cmd[sizeof("AT+CMUT=1")];

	assert (datalen >= sizeof(int *));

	sprintf(cmd, "AT+CMUT=%d", ((int*)data)[0] != 0);

	err = at_send_command(cmd, NULL);

	if (err != 0) goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;

error:
	LOGE("ERROR: requestSetMute failed");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

}

static void requestGetMute(void *data, size_t datalen, RIL_Token t)
{
	int err;
	ATResponse *p_response = NULL;
	int response[1];
	char *line;

	if(phone_is == MODE_CDMA) {
		err = at_send_command_singleline("AT+CMUT?", "+CMUT:", &p_response);
	} else {
		err = at_send_command_singleline("AT+MUT", "+CMUT:", &p_response);
	}
	if (err < 0 || p_response->success == 0) {
		goto error;
	}

	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &response[0]);
	if (err < 0) goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(char*));
	at_response_free(p_response);

	return;

error:
	LOGE("ERROR: requestGetMute failed");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	at_response_free(p_response);
}

static void requestScreenState(void *data, size_t datalen, RIL_Token t)
{
	int err, screenState;

	assert (datalen >= sizeof(int *));
	screenState = ((int*)data)[0];

	if(screenState == 1)
	{
		if (phone_is == MODE_GSM) {
			/*
			 if (ppt)
			 	"AT+ENCSQ=1;\r"
			 else
				 "AT+ENCSQ=1;+CREG=2\r"

			requestRegistrationState

			 "AT+HTCCTZR=1\r"
			 "AT@HTCPDPFD=0\r"
			 */
			/* Screen is on - be sure to enable all unsolicited notifications again */
/*			err = at_send_command("AT+CREG=2", NULL);
			if (err < 0) goto error;
			err = at_send_command("AT+CGREG=2", NULL);
			if (err < 0) goto error;
			err = at_send_command("AT+CGEREP=1,0", NULL);
			if (err < 0) goto error;
			err = at_send_command("AT@HTCPDPFD=0", NULL);
			if (err < 0) goto error;
			err = at_send_command("AT+ENCSQ=1",NULL);
			if (err < 0) goto error;
			err = at_send_command("AT@HTCCSQ=1", NULL);
			if (err < 0) goto error;*/

			err = at_send_command("AT+ENCSQ=1", NULL);
			if (err < 0) goto error;
			err = at_send_command("AT+CREG=2", NULL);
			if (err < 0) goto error;
			err = at_send_command("AT+CSQ", NULL);
			if (err < 0) goto error;
//			err = at_send_command("AT+HTCCTZR=1", NULL);
//			if (err < 0) goto error;
			err = at_send_command("AT@HTCPDPFD=0", NULL);
			if (err < 0) goto error;
		} else {
			err = at_send_command("AT+ENCSQ=1", NULL);
			if (err < 0) goto error;
//			err = at_send_command("AT+HTCCTZR=1", NULL);
//			if (err < 0) goto error;
			err = at_send_command("AT@HTCPDPFD=0", NULL);
			if (err < 0) goto error;

		}
	} else if (screenState == 0) {
		if (phone_is == MODE_GSM) {
			/*
				"AT+HTCPDPIDLE\r"
			 	if (ppt)
					"AT+ENCSQ=0;\r"
				else
					"AT+ENCSQ=0;+CREG=1\r"
				"AT+HTCCTZR=2\r"
				"AT@HTCPDPFD=1"
			 */
			/* Screen is off - disable all unsolicited notifications */
			/*err = at_send_command("AT+CREG=0", NULL);
			if (err < 0) goto error;
			err = at_send_command("AT+CGREG=0", NULL);
			if (err < 0) goto error;
			err = at_send_command("AT+CGEREP=0,0", NULL);
			if (err < 0) goto error;
			err = at_send_command("AT@HTCPDPFD=1", NULL);
			if (err < 0) goto error;
			err = at_send_command("AT+ENCSQ=0",NULL);
			if (err < 0) goto error;
			err = at_send_command("AT@HTCCSQ=0", NULL);
			if (err < 0) goto error;*/

//			err = at_send_command("AT+HTCPDPIDLE", NULL);
//			if (err < 0) goto error;
			err = at_send_command("AT+ENCSQ=0", NULL);
			if (err < 0) goto error;
			err = at_send_command("AT+CREG=1", NULL);
			if (err < 0) goto error;
//			err = at_send_command("AT+HTCCTZR=2", NULL);
//			if (err < 0) goto error;
			err = at_send_command("AT@HTCPDPFD=1", NULL);
			if (err < 0) goto error;
		} else {
			err = at_send_command("AT+ENCSQ=0", NULL);
			if (err < 0) goto error;
//			err = at_send_command("AT+HTCCTZR=2", NULL);
//			if (err < 0) goto error;
			err = at_send_command("AT@HTCPDPFD=1", NULL);
			if (err < 0) goto error;

		}
	} else {
		/* Not a defined value - error */
		goto error;
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;

error:
	LOGE("ERROR: requestScreenState failed");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestRegistrationState(int request, void *data,
		size_t datalen, RIL_Token t)
{
	int err;
	ATResponse *p_response = NULL, *p_response_bs = NULL;
	const char *cmd;
	const char *prefix;
	char *line, *p;
	int commas;
	int skip;
	int i;
	int count = 4;
	int fd;
	int radiotype = -1;
	char sstate[3];
	char sradiotype[3];
	char *responseStr[14] = {sstate, NULL, NULL, sradiotype};

	got_state_change = 0;

	if(phone_is == MODE_GSM) {
		if (request == RIL_REQUEST_REGISTRATION_STATE) {
			cmd = "AT+CREG?";
			prefix = "+CREG:";
		} else if (request == RIL_REQUEST_GPRS_REGISTRATION_STATE) {
			cmd = "AT+CGREG?";
			prefix = "+CGREG:";
		} else {
			assert(0);
			goto error;
		}
	} else {
#if 0
		/* "Regular" CDMA (like Diam/Raph) uses AT+COPS, world phone CDMA uses AT+HTC_SRV_STATUS */
		if (is_world_cdma)
			cmd = "AT+HTC_SRV_STATUS?";
		else
			cmd = "AT+COPS?";

		prefix= "$HTC_SYSTYPE";
#else
		cmd = "AT+HTC_GETSYSTYPE=0";
		prefix= "+HTC_GETSYSTYPE:";
		err = at_send_command_singleline(cmd, prefix, &p_response);
		if(err==0 && p_response->success) {
			line = p_response->p_intermediates->line;
			err = at_tok_start(&line);
			if(err==0)
				err = at_tok_nextint(&line, &radiotype);
		}
		cmd = "AT+CREG?";
		prefix = "+CREG:";
#endif
	}
	err = 1;
	for (i=0;i<4 && err != 0;i++)
		err = at_send_command_singleline(cmd, prefix, &p_response);

	if (err != 0 || !p_response->success) {
		/* CGREG will return CREG if we're not in GSM */
		if (err != 0 && request == RIL_REQUEST_GPRS_REGISTRATION_STATE &&
			(phone_has & MODE_CDMA)) {
			phone_is = MODE_CDMA;
			setPhoneMode();
			switch_req = RIL_REQUEST_CDMA_SUBSCRIPTION;
		}
		goto error;
	}

	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;
	/* Ok you have to be careful here
	 * The solicited version of the CREG response is
	 * +CREG: n, stat, [lac, cid]
	 * and the unsolicited version is
	 * +CREG: stat, [lac, cid]
	 * The <n> parameter is basically "is unsolicited creg on?"
	 * which it should always be
	 *
	 * Now we should normally get the solicited version here,
	 * but the unsolicited version could have snuck in
	 * so we have to handle both
	 *
	 * Also since the LAC and CID are only reported when registered,
	 * we can have 1, 2, 3, or 4 arguments here
	 *
	 * finally, a +CGREG: answer may have a fifth value that corresponds
	 * to the network type, as in;
	 *
	 *   +CGREG: n, stat [,lac, cid [,networkType]]
	 */

	/* count number of commas */
	commas = 0;
	for (p = line ; *p != '\0' ;p++) {
		if (*p == ',') commas++;
	}

	switch (commas) {
		case 0: /* +CREG: <stat> */
			err = at_tok_nextint(&line, &regstate);
			if (err < 0) goto error;
			break;

		case 1: /* +CREG: <n>, <stat> */
			err = at_tok_nextint(&line, &skip);
			if (err < 0) goto error;
			err = at_tok_nextint(&line, &regstate);
			if (err < 0) goto error;
			break;

		case 2: /* +CREG: <stat>, <lac>, <cid> */
			err = at_tok_nextint(&line, &regstate);
			if (err < 0) goto error;
			p = line;
			err = at_tok_nexthexint(&line, &skip);
			if (err < 0) goto error;
			at_tok_nextstr(&p, &responseStr[1]);
			p = line;
			err = at_tok_nexthexint(&line, &skip);
			if (err < 0) goto error;
			at_tok_nextstr(&p, &responseStr[2]);
			break;
		case 3: /* +CREG: <n>, <stat>, <lac>, <cid> */
			err = at_tok_nextint(&line, &skip);
			if (err < 0) goto error;
			err = at_tok_nextint(&line, &regstate);
			if (err < 0) goto error;
			p = line;
			err = at_tok_nexthexint(&line, &skip);
			if (err < 0) goto error;
			at_tok_nextstr(&p, &responseStr[1]);
			p = line;
			err = at_tok_nexthexint(&line, &skip);
			if (err < 0) goto error;
			at_tok_nextstr(&p, &responseStr[2]);

			/* Hack for broken +CGREG responses which don't return the network type */
			if((regstate == REG_HOME || regstate == REG_ROAM) && gsm_rtype == -1) {
				ATResponse *p_response_op = NULL;
				err = at_send_command_singleline("AT+COPS?", "+COPS:", &p_response_op);
				if (err == 0 && p_response_op->success) {
					/* We need to get the 4th return param */
					int commas_op = 0;
					char *p_op, *line_op;
					line_op = p_response_op->p_intermediates->line;

					for (p_op = line_op ; *p_op != '\0' ;p_op++) {
						if (*p_op == ',') commas_op++;
					}

					if (commas_op == 3) {
						err = at_tok_start(&line_op);
						err = at_tok_nextint(&line_op, &skip);
						if (err < 0) goto error;
						err = at_tok_nextint(&line_op, &skip);
						if (err < 0) goto error;
						err = at_tok_nextint(&line_op, &skip);
						if (err < 0) goto error;
						err = at_tok_nextint(&line_op, &radiotype);
						if (err < 0) goto error;
						gsm_rtype = radiotype;
					}
				}

				at_response_free(p_response_op);
			}
			break;
			/* special case for CGREG, there is a fourth parameter
			 * that is the network type (unknown/gprs/edge/umts)
			 */
		case 4: /* +CGREG: <n>, <stat>, <lac>, <cid>, <networkType> */
			err = at_tok_nextint(&line, &skip);
			if (err < 0) goto error;
			err = at_tok_nextint(&line, &regstate);
			if (err < 0) goto error;
			p = line;
			err = at_tok_nexthexint(&line, &skip);
			if (err < 0) goto error;
			at_tok_nextstr(&p, &responseStr[1]);
			p = line;
			err = at_tok_nexthexint(&line, &skip);
			if (err < 0) goto error;
			at_tok_nextstr(&p, &responseStr[2]);
			err = at_tok_nexthexint(&line, &radiotype);
			if (err < 0) goto error;
			gsm_rtype = radiotype;
			break;
		default:
			goto error;
	}

	if (phone_is == MODE_GSM) {
		/* Now translate to 'Broken Android Speak' - can't follow the GSM spec */
		if (radiotype == -1 && (regstate == REG_HOME || regstate == REG_ROAM))
			radiotype = gsm_rtype;
		switch(radiotype) {
			/* GSM/GSM Compact - aka GPRS */
			case 0:
			case 1:
				radiotype = RADIO_GPRS;
				break;
				/* EGPRS - aka EDGE */
			case 3:
				radiotype = RADIO_EDGE;
				break;
				/* UTRAN - UMTS aka 3G */
			case 2:
			case 7:
				radiotype = RADIO_UMTS;
				break;
				/* UTRAN with HSDPA */
			case 4:
				radiotype = RADIO_HSDPA;
				break;
				/* UTRAN with HSUPA */
			case 5:
				radiotype = RADIO_HSUPA;
				break;
				/* UTRAN with HSPA (HSDPA and HSUPA) */
			case 6:
				radiotype = RADIO_HSPA;
				break;
			default:
				radiotype = RADIO_UNKNOWN;
		}
	} else {
		if (radiotype == 2)	/* 1xRTT */
			radiotype=RADIO_1xRTT;
		else if (radiotype == 3)	/* EvDO 0 */
			radiotype=RADIO_EVDO_0;
		else if (radiotype >= 4)	/* EvDO A */
			radiotype=RADIO_EVDO_A;
#if 0
		else if (radiotype == 5)	/* EvDO B? */
			radiotype = RADIO_EVDO_B;		/* Gingerbread only? */
#endif
		else if (radiotype == -1)	/* unknown */
			radiotype = RADIO_UNKNOWN;

		if (request != RIL_REQUEST_GPRS_REGISTRATION_STATE) {
			count = 14;
			for (i=4; i<14; i++)
				responseStr[i] = NULL;
		if (regstate == REG_HOME || regstate == REG_ROAM) {
			char *line_bs;

			/* returns SYSTYPE which we already have, ERIIND which
			 * will be parsed in the unsol handler, CSQ, and 3GIND
			 * We want the ERIIND...
			 */
			if (!eriPRL[0])
				at_send_command("AT+HTC_SRV_STATUS?", NULL);

			err = at_send_command_singleline("AT+HTC_BSINFO?", "+HTC_BSINFO:", &p_response_bs);
			if (err < 0 || !p_response->success)
				goto done;
			line_bs = p_response_bs->p_intermediates->line;
			err = at_tok_start(&line_bs);
			err = at_tok_nextstr(&line_bs, &responseStr[8]);
			if (err < 0) goto done;
			err = at_tok_nextstr(&line_bs, &responseStr[9]);
			if (err < 0) goto done;
			err = at_tok_nextstr(&line_bs, &responseStr[4]);
			if (err < 0) goto done;
			err = at_tok_nextstr(&line_bs, &responseStr[5]);
			if (err < 0) goto done;
			if (responseStr[5][0] == '+')
				responseStr[5]++;
			err = at_tok_nextstr(&line_bs, &responseStr[6]);
			if (err < 0) goto done;
			if (responseStr[6][0] == '+')
				responseStr[6]++;
			responseStr[7]  = "1";	/* FIXME: CDMA2000 Concurrent Services supported? */
			responseStr[10] = "64";	/* FIXME: Roaming Indicator */
			responseStr[11] = "1";	/* FIXME: current system in PRL? */
			responseStr[12] = eriPRL;	/* Default roaming indicator */
			responseStr[13] = "-1";
		}
		}
	}
done:
	sprintf(sstate, "%d", regstate);
	sprintf(sradiotype, "%d", radiotype);
	android_rtype = radiotype;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, count*sizeof(char*));
	at_response_free(p_response_bs);
	at_response_free(p_response);

	return;
error:
	LOGE("requestRegistrationState must never return an error when radio is on");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	at_response_free(p_response_bs);
	at_response_free(p_response);
}

static void requestOperator(void *data, size_t datalen, RIL_Token t)
{
	int err;
	int i;
	int skip;
	ATLine *p_cur;
	ATResponse *p_response = NULL;
	char *response[4];

	memset(response, 0, sizeof(response));

	if(regstate != REG_HOME && regstate != REG_ROAM) {
		RIL_onRequestComplete(t, RIL_E_OP_NOT_ALLOWED_BEFORE_REG_TO_NW, NULL, 0);
		return;
	}

	if(phone_is == MODE_GSM) {
		err = at_send_command_multiline(
				"AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS?",
				"+COPS:", &p_response);

		/* we expect 3 lines here:
		 * +COPS: 0,0,"T - Mobile"
		 * +COPS: 0,1,"TMO"
		 * +COPS: 0,2,"310170"
		 */

		if (err != 0 || !p_response->success) goto error;

		for (i = 0, p_cur = p_response->p_intermediates
				; p_cur != NULL
				; p_cur = p_cur->p_next, i++
		    ) {
			char *line = p_cur->line;

			err = at_tok_start(&line);
			if (err < 0) goto error;

			err = at_tok_nextint(&line, &gsm_selmode);
			if (err < 0) goto error;

			// If we're unregistered, we may just get
			// a "+COPS: 0" response
			if (!at_tok_hasmore(&line)) {
				response[i] = NULL;
				continue;
			}

			err = at_tok_nextint(&line, &skip);
			if (err < 0) goto error;

			// a "+COPS: 0, n" response is also possible
			if (!at_tok_hasmore(&line)) {
				response[i] = NULL;
				continue;
			}

			err = at_tok_nextstr(&line, &(response[i]));
			if (err < 0) goto error;

			if (at_tok_hasmore(&line)) {
				at_tok_nextint(&line, &gsm_rtype);
			}
		}

		if (i < 3) {
			goto error;
		}
		if (i == 3) {
			response[3] = '\0';
		}
	}
	else {
		response[0]=erisystem;
		response[1]=erishort;
		if (!operid[0]) {
			char *line, *p;
			err = at_send_command_singleline("AT+HTC_SRV_STATUS?", "+HTC_SRV_STATUS:",
				&p_response);
			if (err < 0 || !p_response->success) goto error;

			line = p_response->p_intermediates->line;

			/* 0,2,"0310000" */
			err = at_tok_start(&line);
			if (err < 0) goto error;

			err = at_tok_nextint(&line, &skip);
			if (err < 0) goto error;

			err = at_tok_nextint(&line, &skip);
			if (err < 0) goto error;

			err = at_tok_nextstr(&line, &p);
			if (err < 0) goto error;

			/* 3 digit MCC, 2 or 3 digit MNC
			 * Note: India has 5-digit MNCs
			 */
			if (p[0] == '0') p++;
			strncpy(operid, p, 3);
			p += 3;
			if (p[0] == '0') p++;
			strcpy(operid+3, p);
		}
		response[2] = operid;
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
	at_response_free(p_response);
	return;

error:
	LOGE("requestOperator must not return error when radio is on");
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	at_response_free(p_response);
}

static void requestCDMASendSMS(void *data, size_t datalen, RIL_Token t)
{
	int err, len;
	RIL_CDMA_SMS_Message *msg;
	RIL_SMS_Response response;
	char sendstr[512], cmd[sizeof("AT+CMGS=255")], *line;
	ATResponse *p_response = NULL;

	msg = data;
	len = encode_cdma_sms_from_ril(msg, sendstr, sizeof(sendstr));

	sprintf(cmd, "AT+CMGS=%d", len/2);
	err = at_send_command_sms(cmd, sendstr, "+CMGS:", &p_response);

	if (err != 0 || p_response->success == 0) goto error;

	memset(&response, 0, sizeof(response));

	/* FIXME fill in messageRef and ackPDU */
	line = p_response->p_intermediates->line;
	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &response.messageRef);
	if (err < 0) goto error;

	response.ackPDU = NULL;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
	at_response_free(p_response);
	return;

error:
	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSendSMS(void *data, size_t datalen, RIL_Token t, int request)
{
	int err;
	char smsc[30];
	const char *pdu;
	const char *testSmsc;
	int tpLayerLength,length,i,plus = 0;
	char *cmd1, *cmd2, *line, *temp;
//	bytes_t first;
	int tosca,curChar=0;
	RIL_SMS_Response response;
	ATResponse *p_response = NULL;
	ATResponse *p2_response = NULL;
	char * cdma=0;
	char sendstr[512];
	struct {
		RIL_SMS_Response resp;
		int result;
	} extendedResponse;

	testSmsc = ((char **)data)[0];
	pdu = ((const char **)data)[1];

	tpLayerLength = strlen(pdu)/2;
	LOGI("SMSC=%s  PDU=%s",testSmsc,pdu);
	// "NULL for default SMSC"
	if (testSmsc == NULL) {
		err = at_send_command_singleline("AT+CSCA?", "+CSCA:", &p2_response);

		if (err < 0 || p2_response->success == 0) {
			goto error;
		}

		line = p2_response->p_intermediates->line;

		err = at_tok_start(&line);
		if (err < 0) goto error;

		err = at_tok_nextstr(&line, &temp);
		if (err < 0) goto error;

		err = at_tok_nextint(&line, &tosca);
		if (err < 0) goto error;

		if(temp[0]=='+') {
			++temp;
			//plus = 1;
		}

		length = strlen(temp) - plus;
		sprintf(smsc,"%.2x%.2x",(length + 1) / 2 + 1, tosca);

		for (i = 0; curChar < length - 1; i+=2 ) {
			smsc[5+i] = temp[plus+curChar++];
			smsc[4+i] = temp[plus+curChar++];
		}

		if ( length % 2) {//One extra number
			smsc[4+length] = temp[curChar];
			smsc[3+length]='F';
			smsc[5+length]='\0';
		} else {
			smsc[4+length] = '\0';
		}
		//first = malloc(30*sizeof(byte_t));
		//length = 2 + (gsm_bcdnum_from_ascii(temp,strlen(temp),&first)) / 2;
		//sprintf(smsc,"%.2x%.2x%s",length,tosca,first);
		//free(first);
	}
	else
		strcpy(smsc,testSmsc);
	LOGI("SMSC=%s  PDU=%s",smsc,pdu);

	asprintf(&cmd1, "AT+CMGS=%d", tpLayerLength);
	asprintf(&cmd2, "%s%s", smsc, pdu);

	err = at_send_command_sms(cmd1, cmd2, "+CMGS:", &p_response);

	free(cmd1);
	free(cmd2);

	if (err != 0 || p_response->success == 0) goto error;

	memset(&response, 0, sizeof(response));

	/* FIXME fill in messageRef and ackPDU */
	line = p_response->p_intermediates->line;
	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &response.messageRef);
	if (err < 0) goto error;
	
	response.ackPDU = NULL;

	if (request == RIL_REQUEST_SEND_SMS_EXTENDED)
	{
		extendedResponse.resp = response;
		extendedResponse.result = 1;
		RIL_onRequestComplete(t, RIL_E_SUCCESS, &extendedResponse, sizeof(extendedResponse));
	}
	else
	{
		RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
	}
	at_response_free(p_response);
	at_response_free(p2_response);
	return;

error:
	at_response_free(p_response);
	at_response_free(p2_response);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSetupDataCall(char **data, size_t datalen, RIL_Token t)
{
	const char *apn;
	char *user = NULL;
	char *pass = NULL;
	char *cmd;
	int err;
	ATResponse *p_response = NULL;
	int fd, i;
	FILE *pppconfig;
	size_t cur = 0;
	ssize_t written, rlen;
	char status[32] = {0};
	char *buffer;
	long buffSize, len;
	char ipbuf[sizeof("255.255.255.255")];
	char ipbuf2[sizeof("255.255.255.255")];
	char *response[3] = { "1", PPP_TTY_PATH, ipbuf };
	int mypppstatus;

	apn = ((const char **)data)[2];
	user = ((char **)data)[3];
	if(user != NULL)
	{
		if (strlen(user)<2)
			user = "dummy";
	} else
		user = "dummy";

	pass = ((char **)data)[4];
	if(pass != NULL)
	{
		if (strlen(pass)<2)
			pass = "dummy";
	} else
		pass = "dummy";

	LOGD("requesting data connection to APN '%s'\n", apn);

	//Make sure there is no existing connection or pppd instance running
	if(killConn(response[0]) < 0)
		goto error;

	if(phone_is == MODE_GSM)
	{
		if (*data[0]=='0')
		LOGE("Android want us to connect as CDMA while we are a GSM phone !");
	} else
	{
		if(*data[0]=='1')
		LOGE("Android want us to connect as GSM while we are a CDMA phone !");
	}
	if(phone_is == MODE_GSM) {
		asprintf(&cmd, "AT+CGDCONT=1,\"IP\",\"%s\",,0,0", apn);
		//FIXME check for error here
		err = at_send_command(cmd, NULL);
		free(cmd);
		// Set required QoS params to default
		err = at_send_command("AT+CGQREQ=1", NULL);
		// Set minimum QoS params to default
		err = at_send_command("AT+CGQMIN=1", NULL);
		// packet-domain event reporting
		err = at_send_command("AT+CGEREP=1,0", NULL);
		// Hangup anything that's happening there now
		pthread_mutex_lock(&s_data_mutex);
		data_state = Data_Dialing;
		pthread_mutex_unlock(&s_data_mutex);
		err = at_send_command("AT+CGACT=0,1", NULL);
		// Start data on PDP context 1
		err = at_send_command("ATD*99***1#", &p_response);
		if (err < 0 || p_response->success == 0) {
			at_response_free(p_response);
			goto error;
		}
		at_response_free(p_response);
	} else {
		//CDMA
		pthread_mutex_lock(&s_data_mutex);
		data_state = Data_Dialing;
		pthread_mutex_unlock(&s_data_mutex);
		err = at_send_command("AT+HTC_DUN=0", NULL);
		err = at_send_command("ATH", NULL);
		err = at_send_command("ATDT#777", &p_response);
		if (err < 0 || p_response->success == 0) {
			at_response_free(p_response);
			goto error;
		}
		at_response_free(p_response);
	}

	property_set("net.gprs.local-ip", "0.0.0.0");
	property_set("net.ppp0.local-ip", "0.0.0.0");
	ppp_set_state(1);

	for (i=0; i<PPP_RETRY_COUNT; i++) {
		int ok;
		sleep(1); // allow time for ip-up to run
		pthread_mutex_lock(&s_data_mutex);
		ok = (data_state == Data_Dialing);
		pthread_mutex_unlock(&s_data_mutex);
		if (!ok)
			goto error;
		/* Return the IP address too */
		property_get("net.gprs.local-ip", ipbuf, "0.0.0.0");
		property_get("net.ppp0.local-ip", ipbuf, "0.0.0.0");
		if (strcmp(ipbuf, "0.0.0.0") || strcmp(ipbuf2, "0.0.0.0")
			break;
	}
	/* pppd started, but didn't get an IP address */
	if (i == PPP_RETRY_COUNT) {
		ppp_set_state(0);
		goto error;
	}

	pthread_mutex_lock(&s_data_mutex);
	i = (data_state == Data_Dialing);
	if (i)
		data_state = Data_Connected;
	pthread_mutex_unlock(&s_data_mutex);

	/* We started pppd successfully, but lost the connection */
	if (!i) {
		ppp_set_state(0);
		goto error;
	}
	RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	pthread_mutex_lock(&s_data_mutex);
	data_state = Data_Off;
	pthread_mutex_unlock(&s_data_mutex);
}

static int killConn(char *cid)
{
	int err;
	char * cmd;
	int i = 0;
	ATResponse *p_response = NULL;

	if (access(PPP_SYS_PATH, F_OK) == 0) {
		/* Did we already send a kill? */
		pthread_mutex_lock(&s_data_mutex);
		if (data_state > Data_Terminated) {
			i = 1;
			data_state = Data_Terminated;
		}
		pthread_mutex_unlock(&s_data_mutex);
		if (i) {
			ppp_set_state(0);
		}
		for (i=0; i<PPP_RETRY_COUNT; i++) {
			sleep(1);
			if (access(PPP_SYS_PATH, F_OK))
				break;
		}
		if (i == PPP_RETRY_COUNT)
			goto error;
	}
	if (phone_is == MODE_GSM) {
		asprintf(&cmd, "AT+CGACT=0,%s", cid);

		err = at_send_command(cmd, &p_response);
		free(cmd);

	} else {
		//CDMA
		if (dataCallNum() >= 0) {
			err = at_send_command("ATH", &p_response);
		} else {
			err = 0;
		}
	}
	/* NO CARRIER is success for a hangup command */
	if (p_response && !p_response->success) {
		err = strcmp(p_response->finalResponse, "3");
	}
	at_response_free(p_response);
	if (err)
		goto error;
	pthread_mutex_lock(&s_data_mutex);
	data_state = Data_Off;
	pthread_mutex_unlock(&s_data_mutex);
	return 0;

error:
	return -1;
}

static void requestDeactivateDataCall(void *data, size_t datalen, RIL_Token t)
{
	char *cid;

	cid = ((char **)data)[0];
	if (killConn(cid) < 0)
		goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSMSAcknowledge(void *data, size_t datalen, RIL_Token t)
{
	int ackSuccess;
	int err;

	ackSuccess = ((int *)data)[0];

	if (ackSuccess == 1) {
		err = at_send_command("AT+CNMA=1", NULL);
	} else if (ackSuccess == 0)  {
		err = at_send_command("AT+CNMA=2", NULL);
	} else {
		LOGE("unsupported arg to RIL_REQUEST_SMS_ACKNOWLEDGE\n");
		goto error;
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

}

static void  requestSIM_IO(void *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response = NULL;
	RIL_SIM_IO_Response sr;
	int err;
	char *cmd = NULL;
	RIL_SIM_IO *p_args;
	char *line;

	memset(&sr, 0, sizeof(sr));

	p_args = (RIL_SIM_IO *)data;
	if(slow_sim)
		usleep(slow_sim);

	/* FIXME handle pin2 */

	if (p_args->data == NULL) {
		asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d",
				p_args->command, p_args->fileid,
				p_args->p1, p_args->p2, p_args->p3);
	} else {
		asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d,%s",
				p_args->command, p_args->fileid,
				p_args->p1, p_args->p2, p_args->p3, p_args->data);
	}
	err = at_send_command_singleline(cmd, "+CRSM:", &p_response);
	free(cmd);

	if (err < 0 || p_response->success == 0) {
		goto error;
	}

	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &(sr.sw1));
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &(sr.sw2));
	if (err < 0) goto error;

	if (at_tok_hasmore(&line)) {
		err = at_tok_nextstr(&line, &(sr.simResponse));
		if (err < 0) goto error;
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
	at_response_free(p_response);
	return;

error:
	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void  requestEnterSimPin(void*  data, size_t  datalen, RIL_Token  t)
{
	ATResponse   *p_response = NULL;
	int           err;
	char*         cmd = NULL;
	const char**  strings = (const char**)data;;

	if ( datalen == sizeof(char*) ) {
		asprintf(&cmd, "AT+CPIN=\"%s\"", strings[0]);
	} else if ( datalen == 2*sizeof(char*) ) {
		asprintf(&cmd, "AT+CPIN=\"%s\",\"%s\"", strings[0], strings[1]);
	} else
		goto error;

	err = at_send_command_singleline(cmd, "+CREG:", &p_response);
	free(cmd);

	if (err < 0 || p_response->success == 0) {
error:
		RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, NULL, 0);
	} else {
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

		/* Notify that SIM is ready */
		setRadioState(RADIO_STATE_SIM_READY);
	}
	
	at_response_free(p_response);
}

static void  requestChangeSimPin(void*  data, size_t  datalen, RIL_Token  t)
{
	ATResponse   *p_response = NULL;
	int           err;
	char*         cmd = NULL;
	const char**  strings = (const char**)data;;

	if ( datalen == 2*sizeof(char*) )
		asprintf(&cmd, "AT+CPWD=\"SC\",\"%s\",\"%s\"", strings[0], strings[1]);
	else
		goto error;

	err = at_send_command(cmd, &p_response);
	free(cmd);

	if (err < 0 || p_response->success == 0) {
error:
		RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, NULL, 0);
	}
	else
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	
	at_response_free(p_response);
}


static void unsolicitedNitzTime(const char * s)
{
	int err;
	char * response = NULL;
	char * line, *origline;
	char * p = NULL;
	char * tz = NULL; /* Timezone */
	origline = line = strdup(s);

	/* Higher layers expect a NITZ string in this format:
	 *  08/10/28,19:08:37-20,1 (yy/mm/dd,hh:mm:ss(+/-)tz,dst)
	 */

	if(strStartsWith(s,"+CTZV:")){

		/* Get Time and Timezone data and store in static variable.
		 * Wait until DST is received to send response to upper layers
		 */
		at_tok_start(&line);

		err = at_tok_nextstr(&line, &tz);
		if (err < 0) goto error;

		err = at_tok_nextstr(&line, &response);
		if (err < 0) goto error;

		snprintf(sNITZtime, sizeof(sNITZtime), "%s%s", response, tz);

	}
	else if(strStartsWith(s,"+CTZDST:")){

		/* We got DST, now assemble the response and send to upper layers */
		at_tok_start(&line);

		err = at_tok_nextstr(&line, &tz);
		if (err < 0) goto error;

		asprintf(&response, "%s,%s", sNITZtime, tz);

		RIL_onUnsolicitedResponse(RIL_UNSOL_NITZ_TIME_RECEIVED, response, strlen(response));
		free(response);

	}
	else if(strStartsWith(s, "+HTCCTZV:")){
		at_tok_start(&line);

		err = at_tok_nextstr(&line, &response);
		if (err < 0) goto error;
		RIL_onUnsolicitedResponse(RIL_UNSOL_NITZ_TIME_RECEIVED, response, strlen(response));

	}
	free(origline);
	return;

error:
	LOGE("Invalid NITZ line %s\n", s);
}

static void unsolicitedRSSI(const char * s)
{
	int err;
	int response[2];
	const unsigned char asu_table[6]={0,3,5,8,12,19};
	char * line = NULL, *origline;
	RIL_SignalStrength rs = {{99,99},{-1,-1},{-1,-1,-1}};

	origline = strdup(s);
	line = origline;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &(response[0]));
	if (err < 0) goto error;

	if (at_tok_hasmore(&line)) {
		err = at_tok_nextint(&line, &(response[1]));
		if (err < 0) goto error;
	}
	if (phone_is == MODE_GSM) {
		if (s[0] == '@')
			response[0]=asu_table[response[0]%6];
	}

	signalStrength[0]=response[0];
	signalStrength[1]=response[1];
	free(origline);

	resp2Strength(response, &rs);

	RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH, &rs, sizeof(rs));
	return;

error:
	/* The notification was for a battery event - do not send a msg to upper layers */
	return;
}

static Data_State check_data() {
	Data_State ret;

	pthread_mutex_lock(&s_data_mutex);
	ret = data_state;
	pthread_mutex_unlock(&s_data_mutex);
	return ret;
}

static void unsolicitedCREG(const char * s)
{
	int i;
	char *colon;

	/* Format is +CREG: <state>,[<lac>,<cid>] */
	colon = strchr(s, ':');
	if (colon) {
		colon++;
		colon++;
		if (*colon == '0') {
			regstate = 0;
		} else {
			i = atoi(colon);
			if (i)
				regstate = i;
		}
	}
	gsm_rtype = -1;
	gsm_selmode = -1;
	if (regstate != REG_HOME && regstate != REG_ROAM) {
		i = check_data();
		if (i == Data_Connected)
			RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
	}
}

static void requestNotSupported(RIL_Token t, int request)
{
	LOGD("Request %d is unsupported", request);
	RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
	return;
}

static void requestOEMHookRaw(void *data, size_t datalen, RIL_Token t)
{
	RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
	return;
}

static void requestQueryCallWaiting(void *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response = NULL;
	int err;
	char *cmd = NULL;
	char *line;
	int class;
	int response[2];

	class = ((int *)data)[0];

	asprintf(&cmd, "AT+CCWA=1,2,%d",class);

	err = at_send_command_singleline(cmd, "+CCWA:", &p_response);

	if (err < 0 || p_response->success == 0) goto error;

	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &(response[0]));
	if (err < 0) goto error;

	if (at_tok_hasmore(&line)) {
		err = at_tok_nextint(&line, &(response[1]));
		if (err < 0) goto error;
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
	at_response_free(p_response);
	free(cmd);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	at_response_free(p_response);
	free(cmd);
}

static void requestSetCallWaiting(void *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response = NULL;
	int err;
	char *cmd = NULL;
	int enabled, class;

	if((datalen<2)||(data==NULL)) goto error;

	enabled = ((int *)data)[0];
	class = ((int *)data)[1];

	asprintf(&cmd, "AT+CCWA=0,%d,%d",enabled,class);

	err = at_send_command(cmd, NULL);

	if (err < 0 ) goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	at_response_free(p_response);
	free(cmd);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	at_response_free(p_response);
	free(cmd);
}

static void requestQueryCallForwardStatus(RIL_Token t)
{
	int err = 0;
	int i = 0;
	int n = 0;
	int tmp = 0;
	ATResponse *p_response = NULL;
	ATLine *p_cur;
	RIL_CallForwardInfo **responses = NULL;
	char *cmd;

	err = at_send_command_multiline("AT+CCFC=0,2", "+CCFC:", &p_response);

	if(err != 0 || p_response->success == 0)
		goto error;

	for (p_cur = p_response->p_intermediates; p_cur != NULL; p_cur = p_cur->p_next)
		n++;

	responses = alloca(n * sizeof(RIL_CallForwardInfo *));

	for(i = 0; i < n; i++) {
		responses[i] = alloca(sizeof(RIL_CallForwardInfo));
		responses[i]->status = 0;
		responses[i]->reason = 0;
		responses[i]->serviceClass = 0;
		responses[i]->toa = 0;
		responses[i]->number = "";
		responses[i]->timeSeconds = 0;
	}

	for (i = 0,p_cur = p_response->p_intermediates; p_cur != NULL; p_cur = p_cur->p_next, i++) {
		char *line = p_cur->line;

		err = at_tok_start(&line);
		if (err < 0) goto error;

		err = at_tok_nextint(&line, &(responses[i]->status));
		if (err < 0) goto error;

		err = at_tok_nextint(&line, &(responses[i]->serviceClass));
		if (err < 0) goto error;

		if(!at_tok_hasmore(&line)) continue;

		err = at_tok_nextstr(&line, &(responses[i]->number));
		if (err < 0) goto error;

		if(!at_tok_hasmore(&line)) continue;

		err = at_tok_nextint(&line, &(responses[i]->toa));
		if (err < 0) goto error;

		if(!at_tok_hasmore(&line)) continue;
		at_tok_nextint(&line,&tmp);

		if(!at_tok_hasmore(&line)) continue;
		at_tok_nextint(&line,&tmp);

		if(!at_tok_hasmore(&line)) continue;
		err = at_tok_nextint(&line, &(responses[i]->timeSeconds));
		if (err < 0) goto error;

	}

	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_SUCCESS, responses, sizeof(RIL_CallForwardInfo **));
	return;

error:
	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSetCallForward(void *data, RIL_Token t)
{
	int err = 0;
	char *cmd = NULL;
	RIL_CallForwardInfo *info = NULL;

	info = ((RIL_CallForwardInfo *) data);

	if(data == NULL) goto error;

	asprintf(&cmd, "AT+CCFC = %d, %d, \"%s\"",
			info->reason, info->status,
			info->number);

	err = at_send_command(cmd, NULL);

	if (err < 0 ) goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	free(cmd);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	free(cmd);
}

static void requestGetCLIR(void *data, size_t datalen, RIL_Token t)
{
	/* Even though data and datalen must be NULL and 0 respectively this
	 * condition is not verified
	 */
	ATResponse *p_response = NULL;
	int response[2];
	char *line = NULL;
	int err = 0;

	err = at_send_command_singleline("AT+CLIR?", "+CLIR:", &p_response);

	if (err < 0 || p_response->success == 0) goto error;

	line = p_response->p_intermediates->line;
	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &(response[0]));
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &(response[1]));
	if (err < 0) goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
	at_response_free(p_response);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	at_response_free(p_response);
}

static void requestSetCLIR(void *data, size_t datalen, RIL_Token t)
{
	char *cmd = NULL;
	int err = 0;

	asprintf(&cmd, "AT+CLIR=%d", ((int *)data)[0]);

	err = at_send_command(cmd, NULL);
	free(cmd);

	if (err < 0)
		RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, NULL, 0);
	else
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestSendSMSExpectMore(void *data, size_t datalen, RIL_Token t)
{
	char *cmd = NULL;
	asprintf(&cmd, "AT+CMMS=1");
	at_send_command(cmd, NULL);
	free(cmd);
	requestSendSMS(data, datalen, t, 0);
}

static void requestSendUSSD(void *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response = NULL;
	int err = 0;
	int len;
	cbytes_t ussdRequest;
	char *cmd;
	bytes_t temp;
	char *newUSSDRequest;

	ussdRequest = (cbytes_t)(data);
	temp = malloc(strlen((char *)ussdRequest)*sizeof(char)+1);
	len = utf8_to_gsm8(ussdRequest,strlen((char *)ussdRequest),temp);
	newUSSDRequest = malloc(2*len*sizeof(char)+1);
	gsm_hex_from_bytes(newUSSDRequest,temp, len);
	newUSSDRequest[2*len]='\0';
	asprintf(&cmd, "AT+CUSD=1,\"%s\",15", newUSSDRequest);
	free(newUSSDRequest);
	free(temp);
	err = at_send_command(cmd, &p_response);
	free(cmd);
	if (err < 0 || p_response->success == 0) {
		goto error;
	} else {
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	}
	at_response_free(p_response);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void  unsolicitedUSSD(const char *s)
{
	char *line, *linestart;
	int typeCode, count, err, len, encoding = 0;
	char *message;
	char *outputmessage, typecode[8];
	char *responseStr[2] = {typecode,NULL};

	LOGD("unsolicitedUSSD %s\n",s);

	linestart=line=strdup(s);
	err = at_tok_start(&line);
	if(err < 0) goto error;

	err = at_tok_nextint(&line, &typeCode);
	if(err < 0) goto error;

	if(at_tok_hasmore(&line)) {
		err = at_tok_nextstr(&line, &message);
		if(err < 0) goto error;

		if(at_tok_hasmore(&line)) {
			err = at_tok_nextint(&line, &encoding);
			if(err < 0) goto error;
		}

		len = strlen(message);
		outputmessage = malloc(len/2+1);
		gsm_hex_to_bytes((cbytes_t)message,len,(bytes_t)outputmessage);
		responseStr[1] = malloc(len+1);
		if ((encoding & 0xec) == 0x48) {
			len = ucs2_to_utf8((cbytes_t)outputmessage,len/4,(bytes_t)responseStr[1]);
		} else {
			len = utf8_from_gsm8((cbytes_t)outputmessage,len/2,(bytes_t)responseStr[1]);
		}
		responseStr[1][len]='\0';
		free(outputmessage);
		count = 2;
	} else {
		responseStr[1]=NULL;
		count = 1;
	}
	free(linestart);
	sprintf(responseStr[0], "%d", typeCode & 7);
	
	RIL_onUnsolicitedResponse (RIL_UNSOL_ON_USSD, responseStr, count*sizeof(char*));
	free(responseStr[1]);
	return;

error:
	free(linestart);
	LOGE("unexpectedUSSD error\n");
}

static void  unsolicitedERI(const char *s) {
	char *line, *origline;
	int temp;
	char *newEri;
	char olderi[50];

	origline = strdup(s);
	line = origline;
	at_tok_start(&line);

	/* 3,1,1,0,0,0,0,Sprint,1
	 * carrier ID, eri ID, icon_img ID,
	 * 4 unused int params
	 * operator name
	 * data support
	 */
	at_tok_nextint(&line, &temp);
	at_tok_nextint(&line, &temp);
	snprintf(eriPRL, sizeof(eriPRL), "%d", temp);
	at_tok_nextint(&line, &temp);
	at_tok_nextint(&line, &temp);
	at_tok_nextint(&line, &temp);
	at_tok_nextint(&line, &temp);
	at_tok_nextint(&line, &temp);
	at_tok_nextstr(&line, &newEri);

	strcpy(olderi, erisystem);
	if(strlen(newEri)<50)
		strcpy(erisystem,newEri);
	line = strchr(newEri, ' ');
	if (line && line - newEri < 50) {
		*line = '\0';
		strcpy(erishort,newEri);
	} else {
		strcpy(erishort, erisystem);
	}
	if (strcmp(olderi, erisystem))
		operid[0] = '\0';

	free(origline);
}

static void requestSetFacilityLock(void *data, size_t datalen, RIL_Token t)
{
	/* It must be tested if the Lock for a particular class can be set without
	 * modifing the values of the other class. If not, first must call
	 * requestQueryFacilityLock to obtain the previus value
	 */
	int err = 0;
	char *cmd = NULL;
	char *code = NULL;
	char *lock = NULL;
	char *password = NULL;
	char *class = NULL;

	assert (datalen >=  (4 * sizeof(char **)));

	code = ((char **)data)[0];
	lock = ((char **)data)[1];
	password = ((char **)data)[2];
	class = ((char **)data)[3];

	asprintf(&cmd, "AT+CLCK=\"%s\",%s,\"%s\",%s", code, lock, password, class);
	err = at_send_command(cmd, NULL);
	if (err < 0) goto error;

	free(cmd);
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestChangeBarringPassword(void *data, size_t datalen, RIL_Token t)
{
	int err = 0;
	char *cmd = NULL;
	char *string = NULL;
	char *old_password = NULL;
	char *new_password = NULL;

	assert (datalen >=  (3 * sizeof(char **)));

	string	   = ((char **)data)[0];
	old_password = ((char **)data)[1];
	new_password = ((char **)data)[2];

	asprintf(&cmd, "AT+CPWD=\"%s\",\"%s\",\"%s\"", string, old_password, new_password);
	err = at_send_command(cmd, NULL);

	free(cmd);

	if (err < 0) goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSetNetworkSelectionManual(void *data, size_t datalen, RIL_Token t)
{
	char *operator = NULL;
	char *cmd = NULL;
	int err = 0;
	ATResponse *p_response = NULL;

	operator = (char *)data;
	asprintf(&cmd, "AT+COPS=1,2,\"%s\"", operator);
	err = at_send_command(cmd, &p_response);
	if (err < 0 || p_response->success == 0){
		err = at_send_command("AT+COPS=0", NULL);
		if(err < 0) goto error;
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	at_response_free(p_response);
	free(cmd);
	return;

error:
	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestQueryCLIP(void *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response = NULL;
	int err;
	char *line;
	int response;

	err = at_send_command_singleline("AT+CLIP?","+CLIP:",&p_response);
	if(err < 0 || p_response->success == 0) goto error;

	line = p_response->p_intermediates->line;
	err = at_tok_start(&line);
	if(err < 0) goto error;

	/* The first number is discarded */
	err = at_tok_nextint(&line, &response);
	if(err < 0) goto error;

	err = at_tok_nextint(&line, &response);
	if(err < 0) goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
	at_response_free(p_response);
	return;

error:
	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestResetRadio(RIL_Token t)
{
	int err = 0;

	err = at_send_command("AT+CFUN=16", NULL);
	if(err < 0)
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	else
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

	return;
}

static void requestSetSuppSVCNotification(void *data, size_t datalen, RIL_Token t)
{
	int err = 0;
	int enabled = 0;
	char *cmd = NULL;
	enabled = ((int *)data)[0];

	asprintf(&cmd, "AT+CSSN=%d,%d", enabled, enabled);
	err = at_send_command(cmd,NULL);
	free(cmd);
	if(err < 0)
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	else
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestExplicitCallTransfer(RIL_Token t)
{
	int err = 0;
	err = at_send_command("AT+CHLD=4",NULL);
	if(err < 0)
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	else
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestSetLocationUpdates(void *data, size_t datalen, RIL_Token t)
{
	int err = 0;
	char cmd[sizeof("AT+CREG=1")];
	int updates = ((int *)data)[0] == 1;

	/* HTC ril never disables it here */
	sprintf(cmd, "AT%s", updates ? "+CREG=2" :"");

	err = at_send_command(cmd,NULL);
	if(err < 0)
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	else
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestSTKGetprofile(RIL_Token t)
{
	int err = 0;
	int responselen = 0;
	ATResponse *p_response = NULL;
	char *response = NULL;
	char *line = NULL;

	err = at_send_command_singleline("AT+STKPROF?", "+STKPROF:", &p_response);

	if(err < 0 || p_response->success == 0) goto error;

	line = p_response->p_intermediates->line;
	err = at_tok_start(&line);
	if(err < 0) goto error;

	err = at_tok_nextint(&line, &responselen);
	if(err < 0) goto error;

	err = at_tok_nextstr(&line, &response);
	if(err < 0) goto error;

	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_SUCCESS, response, responselen * sizeof(char));
	return;

error:
	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	return;
}

static void requestSTKSetProfile(void * data, size_t datalen, RIL_Token t)
{
	int err = 0;
	int length = 0;
	char *profile = NULL;
	char *cmd = NULL;

	profile = (char *)data;
	length = strlen(profile);
	asprintf(&cmd, "AT+STKPROF=%d,\"%s\"", length, profile);

	err = at_send_command(cmd, NULL);
	free(cmd);
	if(err < 0)
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	else
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;
}

static void requestLastDataFailCause(RIL_Token t)
{
	AT_CME_Error err = at_get_cme_error();
	int response;

	switch(err) {
	case CME_PHONE_LINK_RESERVED:
		response = PDP_FAIL_NSAPI_IN_USE; break;
	case CME_SERVICE_OPTION_NOT_SUPPORTED:
	case CME_OPERATION_NOT_SUPPORTED:
		response = PDP_FAIL_SERVICE_OPTION_NOT_SUPPORTED; break;
	case CME_SERVICE_OPTION_NOT_SUBSCRIBED:
		response = PDP_FAIL_SERVICE_OPTION_NOT_SUBSCRIBED; break;
	case CME_SERVICE_OPTION_OUT_OF_ORDER:
		response = PDP_FAIL_SERVICE_OPTION_OUT_OF_ORDER; break;
	case CME_OPERATION_NOT_ALLOWED:
	case CME_NETWORK_NOT_ALLOWED:
		response = PDP_FAIL_OPERATOR_BARRED; break;
	case CME_PDP_AUTHENTICATION_FAILED:
		response = PDP_FAIL_USER_AUTHENTICATION; break;
	case CME_ILLEGAL_MS:
	case CME_ILLEGAL_ME:
	case CME_GPRS_NOT_ALLOWED:
		response = PDP_FAIL_OPERATOR_BARRED; break;
	case CME_PLMN_NOT_ALLOWED:
	case CME_LOCATION_NOT_ALLOWED:
	case CME_ROAMING_NOT_ALLOWED:
		response = PDP_FAIL_ACTIVATION_REJECT_GGSN; break;
	case CME_TEMPORARY_NOT_ALLOWED:
		response = PDP_FAIL_ACTIVATION_REJECT_UNSPECIFIED; break;
	case CME_PHONE_FAILURE:
	case CME_PHONE_NO_CONNECTION:
	case CME_NO_NETWORK_SERVICE:
	case CME_NETWORK_TIMEOUT:
		response = PDP_FAIL_REGISTRATION_FAIL; break;
	case CME_MEMORY_FAILURE:
		response = PDP_FAIL_INSUFFICIENT_RESOURCES; break;
	case CME_INVALID_MOBILE_CLASS:
		response = PDP_FAIL_GPRS_REGISTRATION_FAIL; break;
	default:
		response = PDP_FAIL_ERROR_UNSPECIFIED; break;
	}
	RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
}

static void requestLastFailCause(RIL_Token t)
{
	AT_CME_Error err = at_get_cme_error();
	int response;

	switch(err) {
	case CME_PHONE_BUSY:
		response = CALL_FAIL_BUSY; break;
	case CME_NETWORK_TIMEOUT:
		response = CALL_FAIL_CONGESTION; break;
	case CME_OPERATION_NOT_ALLOWED:
	case CME_OPERATION_NOT_SUPPORTED:
	case CME_CALL_BARRED:
		response = CALL_FAIL_CALL_BARRED; break;
	case CME_PLMN_NOT_ALLOWED:
	case CME_LOCATION_NOT_ALLOWED:
	case CME_ROAMING_NOT_ALLOWED:
		response = CALL_FAIL_FDN_BLOCKED; break;
	case CME_NETWORK_NOT_ALLOWED:
		response = CALL_FAIL_CDMA_NOT_EMERGENCY; break;
	case CME_ILLEGAL_MS:
		response = CALL_FAIL_IMSI_UNKNOWN_IN_VLR; break;
	case CME_ILLEGAL_ME:
		response = CALL_FAIL_IMEI_NOT_ACCEPTED; break;
	default:
		response = CALL_FAIL_ERROR_UNSPECIFIED; break;
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
}

static void requestHangupWaitingOrBackground(RIL_Token t)
{
	// 3GPP 22.030 6.5.5
	// "Releases all held calls or sets User Determined User Busy
	//  (UDUB) for a waiting call."
	at_send_command("AT+CHLD=0", NULL);

	/* success or failure is ignored by the upper layer here.
	   it will call GET_CURRENT_CALLS and determine success that way */
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestHangupForegroundResumeBackground(RIL_Token t)
{
	// 3GPP 22.030 6.5.5
	// "Releases all active calls (if any exist) and accepts
	//  the other (held or waiting) call."
	at_send_command("AT+CHLD=1", NULL);
	// writesys("audio","5");

	/* success or failure is ignored by the upper layer here.
	   it will call GET_CURRENT_CALLS and determine success that way */
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestSwitchWaitingOrHoldingAndActive(RIL_Token t)
{
	// 3GPP 22.030 6.5.5
	// "Places all active calls (if any exist) on hold and accepts
	//  the other (held or waiting) call."
	if (phone_is == MODE_GSM)
		at_send_command("AT+CHLD=2", NULL);
	else
		at_send_command("AT+HTC_SENDFLASH", NULL);

#ifdef WORKAROUND_ERRONEOUS_ANSWER
	s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

	/* success or failure is ignored by the upper layer here.
	   it will call GET_CURRENT_CALLS and determine success that way */
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestAnswer(RIL_Token t)
{
	at_send_command("ATA", NULL);
	writesys("audio","2");
	audio_on = 1;

#ifdef WORKAROUND_ERRONEOUS_ANSWER
	s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

	/* success or failure is ignored by the upper layer here.
	   it will call GET_CURRENT_CALLS and determine success that way */
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestConference(RIL_Token t)
{
	// 3GPP 22.030 6.5.5
	// "Adds a held call to the conversation"
	at_send_command("AT+CHLD=3", NULL);

	/* success or failure is ignored by the upper layer here.
	   it will call GET_CURRENT_CALLS and determine success that way */
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestUDUB(RIL_Token t)
{
	/* user determined user busy */
	/* sometimes used: ATH */
	at_send_command("ATH", NULL);

	/* success or failure is ignored by the upper layer here.
	   it will call GET_CURRENT_CALLS and determine success that way */
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

}

static void requestSeparateConnection(void * data, size_t datalen, RIL_Token t)
{
	char cmd[12];
	int party = ((int*)data)[0];

	// Make sure that party is in a valid range.
	// (Note: The Telephony middle layer imposes a range of 1 to 7.
	// It's sufficient for us to just make sure it's single digit.)
	if (party > 0 && party < 10){
		sprintf(cmd, "AT+CHLD=2%d", party);
		at_send_command(cmd, NULL);
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	}
	else{
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	}
}

static void requestDTMF(void * data, size_t datalen, RIL_Token t)
{
	int err = 0;
	char cmd[sizeof("AT$VTS=*,1")];

	lastDtmf = ((char *)data)[0];
	sprintf(cmd, "AT$VTS=%c,1", (int)lastDtmf);

	err = at_send_command(cmd, NULL);
	if(err < 0){
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	}
	else{
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	}

	return;
}

static void requestGetIMSI(RIL_Token t)
{
	ATResponse *p_response = NULL;
	int err;

	err = at_send_command_numeric("AT+CIMI", &p_response);
	if (err < 0 || p_response->success == 0) {
		RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
	} else {
		RIL_onRequestComplete(t, RIL_E_SUCCESS,
			p_response->p_intermediates->line, sizeof(char *));
	}
	at_response_free(p_response);
}

static int getIMEISV()
{
	int err;
	ATResponse *p_response = NULL;
	if (!imei[0]) {
		err = at_send_command_numeric("AT+CGSN", &p_response);
		if (err < 0 || p_response->success == 0)
			return -1;
		strncpy(imei, p_response->p_intermediates->line, 17);
		imei[18] = imei[17];
		imei[17] = imei[16];
		imei[16] = imei[15];
		imei[15] = '\0';
	}
	at_response_free(p_response);
	return 0;
}

/* Return 1 if ESN, 0 if MEID, -1 on error */
static int getESNMEID(int munge)
{
	int err = 0;
	ATResponse *p_response = NULL;
	char *line;

	/* On world phone, AT+GSN returns the IMEI in decimal. This can be returned directly.
	 * On regular CDMA, AT+GSN returns the ESN in hex. This has to be converted to decimal.
	 */

	err = at_send_command_numeric("AT+GSN", &p_response);
	if (err < 0 || p_response->success == 0)
		return -1;

	line = p_response->p_intermediates->line;
	if (line[1] == 'x') {
		/* Hex ESN: regular CDMA */
		unsigned long int l;
		l = (unsigned long)hex2int(line[9]) + (unsigned long)16*hex2int(line[8])
			+ (unsigned long)256*hex2int(line[7]) + (unsigned long)4096*hex2int(line[6])
			+ (unsigned long)65536*hex2int(line[5]) + (unsigned long)1048576*hex2int(line[4])
			+ (unsigned long)16777216*hex2int(line[3]) + (unsigned long)268435456*hex2int(line[2]);
		sprintf(imei,"%015lu",l);
		imei[16] = '\0';
		err = 1;
	} else {
		/* Decimal IMEI: world phone in CDMA mode */
		strncpy(imei, line, 17);
		if (munge) {
			imei[18] = imei[17];
			imei[17] = imei[16];
			imei[16] = imei[15];
			imei[15] = '\0';
		}
		err = 0;
	}
	at_response_free(p_response);
	return err;
}

/* Only used on GSM */
static void requestGetIMEISV(int request, RIL_Token t)
{
	int err;
	char *resp;

	if(phone_has & MODE_CDMA)
		err = getESNMEID(1);
	else
		err = getIMEISV();

	if (err < 0) {
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	} else {
		if (request == RIL_REQUEST_GET_IMEI)
			resp = imei;
		else
			resp = imei+16;
		RIL_onRequestComplete(t, RIL_E_SUCCESS, resp, sizeof(resp));
	}
}

static void requestDeviceIdentity(RIL_Token t)
{
	int err;
	char *resp[4], *blank="";
	char meid[15];

	if (phone_is == MODE_GSM) {
		err = getIMEISV();
		if (err < 0)
			goto error;
		resp[0] = imei;
		resp[1] = imei+16;
		resp[2] = blank;
		resp[3] = blank;
	} else {
		err = getESNMEID(1);
		if (err < 0)
			goto error;
		resp[0] = blank;
		resp[1] = blank;
		if (err) {
			resp[2] = imei;
			resp[3] = blank;
		} else {
			resp[0] = imei;
			resp[1] = imei+16;
			if (esn[0])
				resp[2] = esn;
			else
				resp[2] = blank;
		    strncpy(meid, imei, 14);
			meid[14] = '\0';
			resp[3] = meid;
		}
	}
	RIL_onRequestComplete(t, RIL_E_SUCCESS, resp, sizeof(resp));
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCancelUSSD(RIL_Token t)
{
	int err = 0;
	ATResponse *p_response = NULL;

	err = at_send_command_numeric("AT+CUSD=2", &p_response);

	if (err < 0 || p_response->success == 0){
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	}
	else{
		RIL_onRequestComplete(t, RIL_E_SUCCESS,
				p_response->p_intermediates->line, sizeof(char *));
	}

	at_response_free(p_response);
	return;
}

static void requestSetNetworkSelectionAutomatic(RIL_Token t)
{
	int err = 0;

	err = at_send_command("AT+COPS=0", NULL);
	if(err < 0)
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	else
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

	return;
}

static void requestOEMHookStrings(void * data, size_t datalen, RIL_Token t)
{
	int i;
	const char ** cur=(const char **)data;
	char *send=NULL;
	char *startswith=NULL;
	int err=0;
	ATResponse *p_response = NULL;

	if(datalen==sizeof (char *)) {
		send=(char *)*cur;
		startswith=send+2;
		err = at_send_command_singleline(send, startswith, &p_response);
		if(err<0 || p_response->success == 0)
                        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
		else
			RIL_onRequestComplete(t, RIL_E_SUCCESS, &p_response->p_intermediates->line, sizeof (char **));
		at_response_free(p_response);
		return;
	}
	RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
	return;
}

static void requestDeleteSMSOnSIM(void * data, size_t datalen, RIL_Token t)
{
	int err = 0;
	char * cmd;
	ATResponse *p_response = NULL;

	if (phone_has & MODE_GSM) {
		asprintf(&cmd, "AT+CMGD=%d", ((int *)data)[0]);

		err = at_send_command(cmd, &p_response);
		if (err < 0 || p_response->success == 0){
			RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
		} else {
			RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
		}

		free(cmd);
		at_response_free(p_response);
	} else {
		RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
	}
	return;
}

static void requestSTKSendEnvelopeCommand(void * data, size_t datalen, RIL_Token t) {
	int err = 0;
	int len = 0;
	int envcmd = 0;
	int itemid = 0;
	int helpreq = 0;
	int eventlst = 0;
	int lang_cause = 0;
	char *hexdata = (char *)data;
	char *cmd = NULL;
	unsigned int *intdata = NULL;

	len = strlen(data);

	intdata = (unsigned int*)(alloca((len / 2) * sizeof(unsigned int)));

	HexStr_to_DecInt(data, intdata);

	envcmd = intdata[0];
	if(envcmd == 211){
		itemid = intdata[8];
		helpreq = intdata[9];
		asprintf(&cmd, "AT+STKENV=%d, %d, %d", envcmd, itemid, helpreq);
		err = at_send_command(cmd, NULL);
		if(err < 0)
			goto error;
	}
	else if(envcmd == 214){
		len = intdata[1];
		eventlst = intdata[4];
		if(len > 7) lang_cause = intdata[9];
		asprintf(&cmd, "AT+STKENV=%d, %d, %d", envcmd, eventlst, lang_cause);
		err = at_send_command(cmd, NULL);
		if(err < 0)
			goto error;

	}
	else{
		goto notsupported;
	}

	free(cmd);
	free(intdata);
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;

notsupported:
	free(cmd);
	free(intdata);
	RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
	return;

error:
	free(cmd);
	free(intdata);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	return;
}

static void requestSTKSendTerminalResponse(void * data, size_t datalen, RIL_Token t) {
	int err = 0;
	int len = 0;
	int command = 0;
	int result = 0;
	int additionalInfo = 0;
	int dcs = 0;
	int offset = 0;
	int optInfoLen = 0;
	char *optInfo = NULL;
	int i = 0;
	char *hexdata = (char *)data;
	char *cmd = NULL;
	unsigned int *intdata = NULL;

	len = strlen(data);
	intdata = (unsigned int*)(alloca((len / 2) * sizeof(unsigned int)));
	HexStr_to_DecInt(data, intdata);
	command = intdata[2];

	switch(command){
		case 21:
			command = 33;
			break;
		case 20:
			command = 32;
			break;
		case 15:
			command = 21;
			break;
		case 22:
			command = 34;
			break;
		case 23:
			command = 35;
			break;
		case 24:
			command = 36;
			break;
		default:
			goto notsupported;
			break;
	}

	switch(command){
		case 32:
		case 33:{
				result = intdata[11];
				if(intdata[10] > 1)
					additionalInfo = intdata[12];
				asprintf(&cmd, "AT+STKTR=%d, %d, %d", command, result, additionalInfo);
				err = at_send_command(cmd, NULL);
				if(err < 0)
					goto error;
				break;
			}
		case 21:{
				result = intdata[11];
				asprintf(&cmd, "AT+STKTR=%d, %d", command, result);
				err = at_send_command(cmd, NULL);
				if(err < 0)
					goto error;
				break;
			}
		case 34:
		case 35:
		case 36:{
				result = intdata[11];
				if(intdata[10] > 1){
					additionalInfo = intdata[12];
					offset = 1;
				}
				optInfoLen = (intdata[13] + offset) * 2;
				optInfo = alloca(optInfoLen * sizeof(char));
				for(i = 0; i < optInfoLen; i++)
					optInfo[i] = hexdata[15 + offset + i];

				asprintf(&cmd, "AT+STKTR=%d, %d, %d, 0, %d,\"%s\"", command, result, additionalInfo, intdata[14+offset], optInfo);

				err = at_send_command(cmd, NULL);
				if(err < 0)
					goto error;

				break;
			}
	}

	free(cmd);
	free(intdata);
	free(optInfo);
	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;

notsupported:
	free(cmd);
	free(intdata);
	free(optInfo);
	RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
	return;

error:
	free(cmd);
	free(intdata);
	free(optInfo);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaSetSubscription(void *data, size_t datalen, RIL_Token t) {
	RIL_onRequestComplete(t, RIL_E_SUCCESS, 0, 0);
}

static int getCDMAOperator() {
	int err, skip;
	ATResponse *p_response = NULL;
	char *line, *p;

	err = at_send_command_singleline("AT+HTC_RSINFO=0", "+HTC_RSINFO:", &p_response);
	if (err < 0 || p_response->success == 0)
		goto error;

	/* returns version,mobile-sw version,esn,prl_version,pre_only,IMSI,customer_id,pri_version */
	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextstr(&line, &p);
	if (err < 0) goto error;
	err = at_tok_nextstr(&line, &p);
	if (err < 0) goto error;
	err = at_tok_nextstr(&line, &p);	/* esn, we might want to keep this? */
	if (err < 0) goto error;
	strncpy(esn, p, sizeof(esn));
	esn[sizeof(esn)-1] = '\0';

	err = at_tok_nextstr(&line, &p);
	if (err < 0) goto error;
	strncpy(prl, p, sizeof(prl));
	prl[sizeof(prl)-1] = '\0';

	err = at_tok_nextstr(&line, &p);	/* pre_only */
	if (err < 0) goto error;

	err = at_tok_nextstr(&line, &p);	/* IMSI: operator ID + MIN */
	if (err < 0) goto error;
	skip = strlen(p);
	if (skip < 10) {
		err = -1;
		goto error;
	}
	strcpy(min, p + skip - 10);
	strncpy(operID, p, skip - 10);
	operID[skip-10] = '\0';

error:
	at_response_free(p_response);
	return err;
}

static void requestCdmaSubscription(RIL_Token t) {
	int err, skip;
	ATResponse *p_response = NULL;
	char *line, *p;
	char mdn[12];
	char h_sids[64];
	char h_nids[64];
	char *responseStr[5] = {mdn, h_sids, h_nids, min, prl};

	err = at_send_command_singleline("AT+HTC_NAM_SEL?", "+HTC_NAM_SEL:", &p_response);
	if (err != 0 || !p_response->success) goto error;

	/* returns x,x,"MDN" */
	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &skip);
	if (err < 0) goto error;
	err = at_tok_nextint(&line, &skip);
	if (err < 0) goto error;
	err = at_tok_nextstr(&line, &p);
	if (err < 0) goto error;

	strncpy(mdn, p, sizeof(mdn));
	mdn[sizeof(mdn)-1] = '\0';

	at_response_free(p_response);

	/* AT+HTC_DM=xxxx to retrieve H_SID/H_NID. XXXX obtained from CDMA Hero. */
	err = at_send_command_singleline("AT+HTC_DM=C826030100", "+HTC_DM:", &p_response);
	if (err < 0 || p_response->success == 0)
		goto error;

	/* returns long string of hex addresses and data */
	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	p = line + sizeof("C826030100");
	p[8] = '\0';
	LOGV("Hex SID/NID: %s\n", p);
	/* values are little endian */
	/* There's space here for quite a long list of values
	 * but we'll only parse the first, for now.
	 */
	skip = hex2int(p[1]) | (hex2int(p[0]) << 4) | (hex2int(p[3]) << 8) | (hex2int(p[2]) << 12);
	sprintf(h_sids, "%d", skip);
	skip = hex2int(p[5]) | (hex2int(p[4]) << 4) | (hex2int(p[7]) << 8) | (hex2int(p[6]) << 12);
	sprintf(h_nids, "%d", skip);

	at_response_free(p_response);
	p_response = NULL;

	err = getCDMAOperator();
	if (err) goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, sizeof(responseStr));
	return;

error:
	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaSetRoamingPref(void *data, size_t datalen, RIL_Token t) {
	int err;
	char cmd[sizeof("AT+HTC_PSS=255,3")], *str;
	int pref;

	pref = ((int *)data)[0];
	switch (pref) {
	case 0: str="1,2"; break;
	case 1: str="2,3"; break;
	case 2: str="255,3"; break;
	default: goto error;
	}

	sprintf(cmd, "AT+HTC_PSS=%s", str);
	err = at_send_command(cmd, NULL);
	if (err !=0) goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, 0, 0);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaQueryRoamingPref(RIL_Token t) {
	int err, pref;
	ATResponse *p_response = NULL;
	char *line, *p;

	err = at_send_command_singleline("AT+HTC_PSS?", "+HTC_PSS:", &p_response);
	if (err < 0 || p_response->success == 0)
		goto error;

	/* returns x,y */
	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	p = line+1;
	if (!strcmp(p, "255,3"))
		pref = 2;
	else if (!strcmp(p, "1,2"))
		pref = 0;
	else if (!strcmp(p, "2,3"))
		pref = 1;
	else goto error;	/* no idea */

	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_SUCCESS, &pref, sizeof(int *));
	return;

error:
	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaFlash(void *data, size_t datalen, RIL_Token t) {
	int err;
	char *cmd, *str;
	int pref, len;

	str = data;
	if (str && (len = strlen(str))) {
		cmd = alloca(sizeof("AT+HTC_SENDFLASH=") + len);
		sprintf(cmd, "AT+HTC_SENDFLASH=%s", str);
	} else {
		cmd = "AT+HTC_SENDFLASH";
	}
	err = at_send_command(cmd, NULL);
	if (err !=0) goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, 0, 0);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaSetVoicePriv(void *data, size_t datalen, RIL_Token t) {
	int err;
	char cmd[sizeof("AT+HTC_VPRIVACY=1")];
	int pref;

	pref = *(int *)data;
	/* HTC value is inverted, 1 is off, 0 is on */
	sprintf(cmd, "AT+HTC_VPRIVACY=%d", pref != 0);
	/* Modem sends a confirmation, echoing the value we sent. Don't care. */
	err = at_send_command_singleline(cmd, "+HTC_VPRIVACY:", NULL);
	if (err !=0) goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, 0, 0);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaGetVoicePriv(RIL_Token t) {
	int err;
	int pref;
	ATResponse *p_response = NULL;
	char *line;

	err = at_send_command_singleline("AT+HTC_VPRIVACY=4", "+HTC_VPRIVACY:", &p_response);
	if (err !=0 || !p_response->success) goto error;

	/* returns x */
	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &pref);
	if (err < 0) goto error;

	/* HTC value is inverted, 1 is off, 0 is on */
	pref = pref != 0;

	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_SUCCESS, &pref, sizeof(pref));
	return;

error:
	at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSetTTYMode(void *data, size_t datalen, RIL_Token t) {
	int err;
	char cmd[sizeof("AT+HTC_TTYMODE=1")];
	int pref;

	pref = *(int *)data;
	/* Map Android value to HTC */
	switch(pref) {
	case 0: pref = 3; break;
	case 1: pref = 0; break;
	case 2: pref = 2; break;
	case 3: pref = 1; break;
	default: pref= 3; break;
	}
	sprintf(cmd, "AT+HTC_TTYMODE=%d", pref);
	/* Modem sends a confirmation, echoing the value we sent. Don't care. */
	err = at_send_command_singleline(cmd, "+HTC_TTYMODE:", NULL);
	if (err !=0) goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, 0, 0);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestGetTTYMode(RIL_Token t) {
	int err;
	int pref;
	ATResponse *p_response = NULL;
	char *line;

	err = at_send_command_singleline("AT+HTC_TTYMODE=4", "+HTC_TTYMODE:", &p_response);
	if (err !=0 || !p_response->success) goto error;

	/* returns x */
	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &pref);
	if (err < 0) goto error;

	/* Map HTC value to Android */
	switch(pref) {
	case 0:	pref = 1; break;
	case 1: pref = 3; break;
	case 2: pref = 2; break;
	case 3: pref = 0; break;
	default: pref= 0; break;
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, &pref, sizeof(pref));
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void neighborRSSI(void *s) {
	unsolicitedRSSI(s);
	free(s);
}

static void requestNeighboringCellIds(void * data, size_t datalen, RIL_Token t) {
	int err=1;
	int response[2];
	ATResponse *p_response = NULL;
	char *line, *p;
	int commas;
	int skip;
	int i;
	int count = 0, len;
	int cur_cid;
	int is_2g = (gsm_rtype == 0 || gsm_rtype == 1 || gsm_rtype == 3);
	char cmd[] = "AT+Q3GNCELL";
	char prefix[] = "+Q3GNCELL:";

	RIL_NeighboringCell **pp_cellIds, *pp_dummy;

	if (is_2g) {
		cmd[4] = '2';
		prefix[2] = '2';
	}

	err = at_send_command_singleline(cmd, prefix, &p_response);
	if (err < 0 || p_response->success == 0)
		goto error;

	/* my cid, my rssi, # of neighbors [[,neighbor, rssi] ...] */
	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	/* CID,rssi */
	err = at_tok_nextstr(&line, &p);
	if (err < 0) goto error;
	err = at_tok_nextint(&line, &response[0]);
	if (err < 0) goto error;

	/* Dunno how the 3G units map to CSQ */
	if (is_2g) {
		signalStrength[0] = response[0];
		signalStrength[1] = 99;
	}

	err = at_tok_nextint(&line, &count);
	if (err < 0) goto error;

	/* No neighbors found */
	if (count < 1) {
		len = 0;
		pp_cellIds = &pp_dummy;
		pp_dummy = NULL;
		goto done;
	}

	pp_cellIds = (RIL_NeighboringCell **)alloca(count * sizeof(RIL_NeighboringCell *));

	for (i=0; i<count; i++) {
		int cid;
		pp_cellIds[i] = (RIL_NeighboringCell *)alloca(sizeof(RIL_NeighboringCell)+10);
		pp_cellIds[i]->cid = (char *)(pp_cellIds[i] + 1);
		if (is_2g)
			err = at_tok_nexthexint(&line, &cid);
		else
			err = at_tok_nextint(&line, &cid);
		if (err < 0) goto error;
		sprintf(pp_cellIds[i]->cid, "%x", cid);
		err = at_tok_nextint(&line, &pp_cellIds[i]->rssi);
		if (err < 0) goto error;
	}
	len = count * sizeof(*pp_cellIds);
#if 0
	/* Ok you have to be careful here
	 * The solicited version of the CREG response is
	 * +CREG: n, stat, [lac, cid]
	 * and the unsolicited version is
	 * +CREG: stat, [lac, cid]
	 * The <n> parameter is basically "is unsolicited creg on?"
	 * which it should always be
	 *
	 * Now we should normally get the solicited version here,
	 * but the unsolicited version could have snuck in
	 * so we have to handle both
	 *
	 * Also since the LAC and CID are only reported when registered,
	 * we can have 1, 2, 3, or 4 arguments here
	 * 
	 * finally, a +CGREG: answer may have a fifth value that corresponds
	 * to the network type, as in;
	 *
	 *   +CGREG: n, stat [,lac, cid [,networkType]]
	 */

	/* count number of commas */
	commas = 0;
	for (p = line ; *p != '\0' ;p++) {
		if (*p == ',') commas++;
	}
	switch (commas) {
		case 0: /* +CREG: <stat> */
		case 1: /* +CREG: <n>, <stat> */
			goto error;
			break;

		case 2: /* +CREG: <stat>, <lac>, <cid> */
			err = at_tok_nextint(&line, &response[0]);
			if (err < 0) goto error;
			err = at_tok_nexthexint(&line, &response[1]);
			if (err < 0) goto error;
			p = line;
			err = at_tok_nexthexint(&line, &cur_cid);
			if (err < 0) goto error;
			err = at_tok_nextstr(&p, &p_cellIds[0].cid);
			break;
		case 3: /* +CREG: <n>, <stat>, <lac>, <cid> */
			err = at_tok_nextint(&line, &skip);
			if (err < 0) goto error;
			err = at_tok_nextint(&line, &response[0]);
			if (err < 0) goto error;
			err = at_tok_nexthexint(&line, &response[1]);
			if (err < 0) goto error;
			p = line;
			err = at_tok_nexthexint(&line, &cur_cid);
			if (err < 0) goto error;
			p_cellIds[0].rssi=2;
			err = at_tok_nextstr(&p, &p_cellIds[0].cid);
			break;
		case 4: /* +CGREG: <n>, <stat>, <lac>, <cid>, <networkType> */
			err = at_tok_nextint(&line, &skip);
			if (err < 0) goto error;
			err = at_tok_nextint(&line, &response[0]);
			if (err < 0) goto error;
			err = at_tok_nexthexint(&line, &response[1]);
			if (err < 0) goto error;
			p = line;
			err = at_tok_nexthexint(&line, &cur_cid);
			if (err < 0) goto error;
			err = at_tok_nextstr(&p, &p_cellIds[0].cid);
			err = at_tok_nexthexint(&line, &response[3]);
			if (err < 0) goto error;
			count = 4;
			break;
		default:
			goto error;
	}
	len = sizeof(*pp_cellIds);
#endif

done:
	RIL_onRequestComplete(t, RIL_E_SUCCESS, pp_cellIds, len);
	at_response_free(p_response);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	at_response_free(p_response);
}


/*** Callback methods from the RIL library to us ***/

/**
 * Call from RIL to us to make a RIL_REQUEST
 *
 * Must be completed with a call to RIL_onRequestComplete()
 *
 * RIL_onRequestComplete() may be called from any thread, before or after
 * this function returns.
 *
 * Will always be called from the same thread, so returning here implies
 * that the radio is ready to process another command (whether or not
 * the previous command has completed).
 */
	static void
onRequest (int request, void *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response;
	int err;

	LOGD("onRequest: %s (%d)", requestToString(request), request);

	/* These requests are always valid */
	if (request == RIL_REQUEST_BASEBAND_VERSION ||
		request == RIL_REQUEST_OEM_HOOK_STRINGS)
		goto ok;

	/* Ignore all requests except RIL_REQUEST_GET_SIM_STATUS
	 * when RADIO_STATE_UNAVAILABLE.
	 */
	if (sState == RADIO_STATE_UNAVAILABLE
			&& !(request == RIL_REQUEST_GET_SIM_STATUS
				|| request == RIL_REQUEST_GET_IMEI
				|| request == RIL_REQUEST_GET_IMEISV
				|| request == RIL_REQUEST_DEVICE_IDENTITY)
	   ) {
		RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
		return;
	}

	/* Ignore all non-power requests when RADIO_STATE_OFF
	 * (except RIL_REQUEST_GET_SIM_STATUS)
	 */
	if (sState == RADIO_STATE_OFF
			&& !(request == RIL_REQUEST_RADIO_POWER
				|| request == RIL_REQUEST_GET_SIM_STATUS
				|| request == RIL_REQUEST_GET_IMEI
				|| request == RIL_REQUEST_GET_IMEISV
				|| request == RIL_REQUEST_DEVICE_IDENTITY)
	   ) {
		RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
		return;
	}

	/* In an Auto mode transition, ignore all commands until Android catches up */
	if (switch_req) {
		if (switch_req != request) {
			RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
			return;
		}
		switch_req = 0;
	}

ok:
	switch (request) {
		case RIL_REQUEST_GET_SIM_STATUS: {
			RIL_CardStatus *p_card_status;
			char *p_buffer;
			int buffer_size;
			
			int result = getCardStatus(&p_card_status);
			if (result == RIL_E_SUCCESS) {
				p_buffer = (char *)p_card_status;
				buffer_size = sizeof(*p_card_status);
			} else {
				p_buffer = NULL;
				buffer_size = 0;
			}
			RIL_onRequestComplete(t, result, p_buffer, buffer_size);
			freeCardStatus(p_card_status);
			break;
		}

		case RIL_REQUEST_GET_CURRENT_CALLS:
			requestGetCurrentCalls(data, datalen, t);
			break;

		case RIL_REQUEST_DIAL:
			requestDial(data, datalen, t);
			break;

		case RIL_REQUEST_HANGUP:
			requestHangup(data, datalen, t);
			break;

		case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
			requestHangupWaitingOrBackground(t);
			break;

		case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
			requestHangupForegroundResumeBackground(t);
			break;

		case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
			requestSwitchWaitingOrHoldingAndActive(t);
			break;
			
		case RIL_REQUEST_ANSWER:
			requestAnswer(t);
			break;

		case RIL_REQUEST_CONFERENCE:
			requestConference(t);
			break;

		case RIL_REQUEST_UDUB:
			requestUDUB(t);
			break;

		case RIL_REQUEST_SEPARATE_CONNECTION:
			requestSeparateConnection(data, datalen, t);
			break;

		case RIL_REQUEST_SIGNAL_STRENGTH:
			requestSignalStrength(data, datalen, t);
			break;

		case RIL_REQUEST_REGISTRATION_STATE:
		case RIL_REQUEST_GPRS_REGISTRATION_STATE:
			requestRegistrationState(request, data, datalen, t);
			break;

		case RIL_REQUEST_SET_MUTE:
			requestSetMute(data, datalen, t);
			break;

		case RIL_REQUEST_GET_MUTE:
			requestGetMute(data, datalen, t);
			break;

		case RIL_REQUEST_SCREEN_STATE:
			requestScreenState(data, datalen, t);
			break;

		case RIL_REQUEST_OPERATOR:
			requestOperator(data, datalen, t);
			break;

		case RIL_REQUEST_RADIO_POWER:
			requestRadioPower(data, datalen, t);
			break;

		case RIL_REQUEST_QUERY_FACILITY_LOCK:
			requestQueryFacilityLock(data, datalen, t);
			break;

		case RIL_REQUEST_DTMF:
			requestDTMF(data, datalen, t);
			break;

		case RIL_REQUEST_DTMF_STOP:
			requestDtmfStop(data, datalen, t);
			break;

		case RIL_REQUEST_DTMF_START:
			requestDtmfStart(data, datalen, t);
			break;

		case RIL_REQUEST_SEND_SMS_EXTENDED:
		case RIL_REQUEST_SEND_SMS:
			requestSendSMS(data, datalen, t, request);
			break;

		case RIL_REQUEST_SETUP_DATA_CALL:
			requestSetupDataCall(data, datalen, t);
			break;

		case RIL_REQUEST_DEACTIVATE_DATA_CALL:
			requestDeactivateDataCall(data, datalen, t);
			break;

		case RIL_REQUEST_SMS_ACKNOWLEDGE:
			requestSMSAcknowledge(data, datalen, t);
			break;

		case RIL_REQUEST_GET_IMSI:
			requestGetIMSI(t);
			break;

		case RIL_REQUEST_BASEBAND_VERSION:
			requestBasebandVersion(data, datalen, t);
			break;

		case RIL_REQUEST_GET_IMEI:
		case RIL_REQUEST_GET_IMEISV:
			requestGetIMEISV(request, t);
			break;

		case RIL_REQUEST_DEVICE_IDENTITY:
			requestDeviceIdentity(t);
			break;

		case RIL_REQUEST_SIM_IO:
			requestSIM_IO(data,datalen,t);
			break;

		case RIL_REQUEST_CANCEL_USSD:
			requestCancelUSSD(t);
			break;
			
		case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
			requestSetNetworkSelectionAutomatic(t);
			break;

		case RIL_REQUEST_DATA_CALL_LIST:
			requestDataCallList(data, datalen, t);
			break;

		case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
			requestQueryNetworkSelectionMode(data, datalen, t);
			break;

		case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS:
			requestQueryAvailableNetworks(data, datalen, t);
			break;

		case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
			requestGetPreferredNetworkType(data, datalen, t);
			break;

		case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
			requestSetPreferredNetworkType(data, datalen, t);
			break;

		case RIL_REQUEST_OEM_HOOK_RAW:
			// echo back data
			requestOEMHookRaw(data, datalen, t);
			break;

		case RIL_REQUEST_OEM_HOOK_STRINGS:
			requestOEMHookStrings(data, datalen, t);
			break;

		case RIL_REQUEST_WRITE_SMS_TO_SIM:
			requestWriteSmsToSim(data, datalen, t);
			break;

		case RIL_REQUEST_DELETE_SMS_ON_SIM:
			requestDeleteSMSOnSIM(data, datalen, t);
			break;

		case RIL_REQUEST_ENTER_SIM_PIN:
			requestEnterSimPin(data, datalen, t);
			break;
		case RIL_REQUEST_CHANGE_SIM_PIN:
			requestChangeSimPin(data, datalen, t);
			break;
		case RIL_REQUEST_ENTER_SIM_PUK:
		case RIL_REQUEST_ENTER_SIM_PIN2:
		case RIL_REQUEST_ENTER_SIM_PUK2:
		case RIL_REQUEST_CHANGE_SIM_PIN2:
			requestNotSupported(t, request);
			break;

		case RIL_REQUEST_QUERY_CALL_WAITING:
			requestQueryCallWaiting(data, datalen, t);
			break;

		case RIL_REQUEST_SET_CALL_WAITING:
			requestSetCallWaiting(data, datalen, t);
			break;

		case RIL_REQUEST_QUERY_CALL_FORWARD_STATUS:
			requestQueryCallForwardStatus(t);
			break;

		case RIL_REQUEST_SET_CALL_FORWARD:
			requestSetCallForward(data, t);
			break;

		case RIL_REQUEST_GET_CLIR:
			requestGetCLIR(data, datalen, t);
			break;

		case RIL_REQUEST_SET_CLIR:
			requestSetCLIR(data, datalen, t);
			break;

		case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
			requestSendSMSExpectMore(data, datalen, t);
			break;

		case RIL_REQUEST_SEND_USSD:
			requestSendUSSD(data, datalen, t);
			break;

		case RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION:
			// NOTE: There isn't an AT command with this capability
			requestNotSupported(t, request);
			break;

		case RIL_REQUEST_SET_FACILITY_LOCK:
			requestSetFacilityLock(data, datalen, t);
			break;

		case RIL_REQUEST_CHANGE_BARRING_PASSWORD:
			requestChangeBarringPassword(data, datalen, t);
			break;

		case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL:
			requestSetNetworkSelectionManual(data, datalen, t);
			break;

		case RIL_REQUEST_QUERY_CLIP:
			requestQueryCLIP(data, datalen, t);
			break;

		case RIL_REQUEST_RESET_RADIO:
			requestResetRadio(t);
			break;

		case RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION:
			requestSetSuppSVCNotification(data, datalen, t);
			break;

		case RIL_REQUEST_EXPLICIT_CALL_TRANSFER:
			requestExplicitCallTransfer(t);
			break;

		case RIL_REQUEST_SET_LOCATION_UPDATES:
			requestSetLocationUpdates(data, datalen, t);
			break;

		case RIL_REQUEST_STK_GET_PROFILE:
			requestSTKGetprofile(t);
			break;

		case RIL_REQUEST_STK_SET_PROFILE:
			requestSTKSetProfile(data, datalen, t);
			break;

		case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE:
			requestLastDataFailCause(t);
			break;

		case RIL_REQUEST_LAST_CALL_FAIL_CAUSE:
			requestLastFailCause(t);
			break;

		case RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND:
			requestSTKSendEnvelopeCommand(data, datalen, t);
			break;

		case RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE:
			requestSTKSendTerminalResponse(data, datalen, t);
			break;

		case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
			requestNeighboringCellIds(data, datalen, t);
			break;

		case RIL_REQUEST_CDMA_SET_SUBSCRIPTION:
			requestCdmaSetSubscription(data, datalen, t);
			break;

		case RIL_REQUEST_CDMA_SUBSCRIPTION:
			requestCdmaSubscription(t);
			break;

		case RIL_REQUEST_CDMA_SEND_SMS:
			requestCDMASendSMS(data, datalen, t);
			break;

		case RIL_REQUEST_CDMA_SMS_ACKNOWLEDGE:
			RIL_onRequestComplete(t, RIL_E_SUCCESS, 0, 0);
			break;

		case RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE:
			requestCdmaSetRoamingPref(data, datalen, t);
			break;

		case RIL_REQUEST_CDMA_QUERY_ROAMING_PREFERENCE:
			requestCdmaQueryRoamingPref(t);
			break;

		case RIL_REQUEST_CDMA_BURST_DTMF:
			requestCdmaBurstDtmf(data, datalen, t);
			break;

		case RIL_REQUEST_CDMA_FLASH:
			requestCdmaFlash(data, datalen, t);
			break;

		case RIL_REQUEST_CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE:
			requestCdmaSetVoicePriv(data, datalen, t);
			break;

		case RIL_REQUEST_CDMA_QUERY_PREFERRED_VOICE_PRIVACY_MODE:
			requestCdmaGetVoicePriv(t);
			break;

		case RIL_REQUEST_SET_TTY_MODE:
			requestSetTTYMode(data, datalen, t);
			break;

		case RIL_REQUEST_QUERY_TTY_MODE:
			requestGetTTYMode(t);
			break;

		case RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM:
			requestNotSupported(t, request);
			break;

		case RIL_REQUEST_SET_BAND_MODE:
			requestNotSupported(t, request);
			break;

		case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE:
			requestNotSupported(t, request);
			break;

		case RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG:
			requestNotSupported(t, request);
			break;

		case RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG:
			requestNotSupported(t, request);
			break;

		case RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION:
			requestNotSupported(t, request);
			break;

		default:
			requestNotSupported(t, request);
			break;
	}
}

/**
 * Synchronous call from the RIL to us to return current radio state.
 * RADIO_STATE_UNAVAILABLE should be the initial state.
 */
	static RIL_RadioState
currentState()
{
	return sState;
}
/**
 * Call from RIL to us to find out whether a specific request code
 * is supported by this implementation.
 *
 * Return 1 for "supported" and 0 for "unsupported"
 */

	static int
onSupports (int requestCode)
{
	//@@@ todo

	return 1;
}

static void onCancel (RIL_Token t)
{
	//@@@todo

}

static const char * getVersion(void)
{
	return "HTC Vogue Community RIL hyc "
#include "gitver.h"
	;
}

	static void
setRadioState(RIL_RadioState newState)
{
	RIL_RadioState oldState;

	pthread_mutex_lock(&s_state_mutex);

	oldState = sState;

	if (s_closed > 0) {
		// If we're closed, the only reasonable state is
		// RADIO_STATE_UNAVAILABLE
		// This is here because things on the main thread
		// may attempt to change the radio state after the closed
		// event happened in another thread
		newState = RADIO_STATE_UNAVAILABLE;
	}

	if (sState != newState || s_closed > 0) {
		sState = newState;

		pthread_cond_broadcast (&s_state_cond);
	}

	pthread_mutex_unlock(&s_state_mutex);

	/* do these outside of the mutex */
	if (sState != oldState) {
		RIL_onUnsolicitedResponse (RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
				NULL, 0);

		/* FIXME onSimReady() and onRadioPowerOn() cannot be called
		 * from the AT reader thread
		 * Currently, this doesn't happen, but if that changes then these
		 * will need to be dispatched on the request thread
		 */
		if (sState == Radio_READY) {
			onRadioReady();
		} else if (sState == Radio_NOT_READY) {
			onRadioPowerOn();
		}
	}
}

/** returns one of RIL_SIM_*. Returns RIL_SIM_NOT_READY on error */
	static SIM_Status
getSIMStatus()
{
	ATResponse *p_response = NULL;
	int err;
	int ret;
	char *cpinLine;
	char *cpinResult;

	if (sState == RADIO_STATE_OFF || sState == RADIO_STATE_UNAVAILABLE) {
		ret = SIM_NOT_READY;
		goto done;
	}

	err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);

	if (err != 0) {
		ret = SIM_NOT_READY;
		goto done;
	}

	if (!p_response->success) {
	    switch (at_get_cme_error()) {
		case CME_SIM_NOT_INSERTED:
			ret = SIM_ABSENT;
			goto done;

		default:
			ret = SIM_NOT_READY;
			goto done;
	    }
	}

	/* CPIN? has succeeded, now look at the result */

	cpinLine = p_response->p_intermediates->line;
	err = at_tok_start (&cpinLine);

	if (err < 0) {
		ret = SIM_NOT_READY;
		goto done;
	}

	err = at_tok_nextstr(&cpinLine, &cpinResult);

	if (err < 0) {
		ret = SIM_NOT_READY;
		goto done;
	}

	if (0 == strcmp (cpinResult, "SIM PIN")) {
		ret = SIM_PIN;
		goto done;
	} else if (0 == strcmp (cpinResult, "SIM PUK")) {
		ret = SIM_PUK;
		goto done;
	} else if (0 == strcmp (cpinResult, "PH-NET PIN")) {
		return SIM_NETWORK_PERSONALIZATION;
	} else if (0 != strcmp (cpinResult, "READY"))  {
		/* we're treating unsupported lock types as "sim absent" */
		ret = SIM_ABSENT;
		goto done;
	} else {
		/* Is it really ready? Not until it can tell us what
		 * its IMSI is
		 */
		ATResponse *resp2 = NULL;
		int i;
		for (i=0; i<10; i++) {
			err = at_send_command_numeric("AT+CIMI", &resp2);
			if (err < 0 || !resp2->success)
				err = -1;
			at_response_free(resp2);
			if (err >= 0)
				break;
			sleep(2);
		}
		if (i == 10) {
			ret = SIM_NOT_READY;
			goto done;
		}
	}

	at_response_free(p_response);
	p_response = NULL;
	cpinResult = NULL;

	//fall through if everything else succeeded
	ret = SIM_READY;

done:
	at_response_free(p_response);
	return ret;
}

/**
 * Get the current card status.
 *
 * This must be freed using freeCardStatus.
 * @return: On success returns RIL_E_SUCCESS
 */
static int getCardStatus(RIL_CardStatus **pp_card_status) {
    static RIL_AppStatus app_status_array[] = {
        // SIM_ABSENT = 0
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_NOT_READY = 1
        { RIL_APPTYPE_SIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_READY = 2
        { RIL_APPTYPE_SIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_PIN = 3
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // SIM_PUK = 4
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // SIM_NETWORK_PERSONALIZATION = 5
        { RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN }
    };
    RIL_CardState card_state;
    int num_apps;

    int sim_status = getSIMStatus();
    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 1;
    }

    // Allocate and initialize base card status.
    RIL_CardStatus *p_card_status = malloc(sizeof(RIL_CardStatus));
    p_card_status->card_state = card_state;
    p_card_status->universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->gsm_umts_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->cdma_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->num_applications = num_apps;

    // Initialize application status
    int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
        p_card_status->applications[i] = app_status_array[SIM_ABSENT];
    }

    // Pickup the appropriate application status
    // that reflects sim_status for gsm.
    if (num_apps != 0) {
        // Only support one app, gsm
        p_card_status->num_applications = 1;
        p_card_status->gsm_umts_subscription_app_index = 0;

        // Get the correct app status
        p_card_status->applications[0] = app_status_array[sim_status];
    }

    *pp_card_status = p_card_status;
    return RIL_E_SUCCESS;
}

/**
 * Free the card status returned by getCardStatus
 */
static void freeCardStatus(RIL_CardStatus *p_card_status) {
	free(p_card_status);
}

/**
 * SIM ready means any commands that access the SIM will work, including:
 *  AT+CPIN, AT+CSMS, AT+CNMI, AT+CRSM
 *  (all SMS-related commands)
 */

static void pollSIMState (void *param)
{
	ATResponse *p_response;
	int ret;

	if (sState != RADIO_STATE_SIM_NOT_READY) {
		// no longer valid to poll
		return;
	}

	switch(getSIMStatus()) {
		case SIM_ABSENT:
		case SIM_PIN:
		case SIM_PUK:
		case SIM_NETWORK_PERSONALIZATION:
		default:
			setRadioState(RADIO_STATE_SIM_LOCKED_OR_ABSENT);
			return;

		case SIM_NOT_READY:
			RIL_requestTimedCallback (pollSIMState, NULL, &TIMEVAL_SIMPOLL);
			return;

		case SIM_READY:
			setRadioState(RADIO_STATE_SIM_READY);
			return;
	}
}

/** returns 1 if on, 0 if off, and -1 on error */
static int isRadioOn()
{
	ATResponse *p_response = NULL;
	int err;
	char *line;
	char ret;

	err = at_send_command_singleline("AT+CFUN?", "+CFUN:", &p_response);

	if (err < 0 || p_response->success == 0) {
		// assume radio is off
		goto error;
	}

	line = p_response->p_intermediates->line;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextbool(&line, &ret);
	if (err < 0) goto error;

	at_response_free(p_response);

	return (int)ret;

error:

	at_response_free(p_response);
	return -1;
}

/**
 * Initialize everything that can be configured while we're still in
 * AT+CFUN=0. Leave mode-specific stuff until after PowerOn, since
 * the actual Android Phone type is only determined then.
 */
static void initializeCallback(void *param)
{
	ATResponse *p_response = NULL;
	int err;

	at_handshake();

	if (phone_has & MODE_GSM) {
		at_send_command("AT@HTCCSQ=0", &p_response);
		/* World phones have GSM radios but don't support all GSM commands */
		if (!p_response->success) {
			phone_has |= MODE_CDMA;
			world_phone = 1;
			LOGD("Running on a world phone\n");
		}
		at_response_free(p_response);
	}
	/* make sure the radio is off */
	if(phone_has & MODE_GSM)
		at_send_command("AT+CFUN=0", NULL);
	else {
		/* Sending 'AT+CFUN=0' to a CDMA diam/raph eventually causes a "FATAL_ERROR: SM_TM (oncrpc_cb.c:00596)"
		 * Send 'CFUN=66' first, if that errors then we know this is a world phone */
		at_send_command("AT+CFUN=66", NULL);
	}

	setRadioState (RADIO_STATE_OFF);

	strcpy(erisystem,"Android");

	/* note: we don't check errors here. Everything important will
	   be handled in onATTimeout and onATReaderClosed */

	/* Common initialization strings */

	/*  atchannel is tolerant of echo but it must */
	/*  reset and have numeric result codes
	 *	can't use text result codes, some commands
	 *	force numeric anyway
	 */
	at_send_command("ATZV0", NULL);

	/*  echo off */
	at_send_command("ATE0", NULL);

	/*  No auto-answer */
	at_send_command("ATS0=0", NULL);

	/*  send results */
	at_send_command("ATQ0", NULL);

	/*  check for busy, don't check for dialone */
	at_send_command("ATX3", NULL);

	/*  set DCD depending on service */
	at_send_command("AT&C1", NULL);

	/*  set DTR according to service */
	at_send_command("AT&D1", NULL);

	/*  Extended errors */
	at_send_command("AT+CMEE=1", NULL);

	/*  detailed rings, service reporting */
	at_send_command("AT+CRC=1;+CR=1", NULL);

	at_send_command("AT+FCLASS=0", NULL);

	/*  SMS PDU mode */
	at_send_command("AT+CMGF=0", NULL);

	/*  HEX character set */
	at_send_command("AT+CSCS=\"HEX\"", NULL);

	/*  +CSSU unsolicited supp service notifications */
	at_send_command("AT+CSSN=1,1", NULL);

	/*  No connected line identification */
	at_send_command("AT+COLP=0", NULL);

	/*  Call Waiting notifications */
	at_send_command("AT+CCWA=1", NULL);

	/*  Not muted */
	at_send_command("AT+CMUT=0", NULL);

	/*  don't hide outgoing callerID */
	at_send_command("AT+CLIR=0", NULL);

	at_send_command("AT+CNMI=1,2,2,2,0", NULL);

	/*  GPRS registration events */
	at_send_command("AT+CGREG=1", NULL);

	/*  USSD unsolicited */
	at_send_command("AT+CUSD=1", NULL);

	at_send_command("AT+CPPP=2", NULL);

	at_send_command("AT+ENCSQ=1", NULL);

	/* Disconnect notifications; ?? */
	at_send_command("AT@HTCDIS=1;@HTCSAP=1", NULL);

	/*  Network registration events */
	err = at_send_command("AT+CREG=2", &p_response);
	/* some handsets -- in tethered mode -- don't support CREG=2 */
	if (err < 0 || p_response->success == 0)
		at_send_command("AT+CREG=1", NULL);
	at_response_free(p_response);

	/*  dunno, magic... */
	at_send_command("AT+HTCmaskW1=4294967295,14449", NULL);

//	at_send_command("AT+HTCmaskW1=262143,162161", NULL);
//	at_send_command("AT@AGPSADDRESS=193,253,42,109,7275", NULL);

	at_send_command("AT+CHZ=0", NULL);
	at_send_command("AT+2GNCELL=0", NULL);
	at_send_command("AT+3GNCELL=0", NULL);
	at_send_command("AT+CGEQREQ=1,4,0,0,0,0,2,0,\"0E0\",\"0E0\",3,0,0", NULL);

	/* CNV=DTM, GPRSCLASS, HSDPA category [,HSUPA category] */
	at_send_command("AT+HTCNV=1,12,8,6", NULL);

	at_send_command("AT+HSDPA=2", NULL);
	at_send_command("AT+HTCCTZR=1", NULL);
	at_send_command("AT+HTCCNIV=0", NULL);
	at_send_command("AT@HTCDORMANCYSET=3", NULL);
	at_send_command("AT@HTCPDPFD=0", NULL);
	at_send_command("AT+HTCAGPS=2", NULL);

		/*enable ENS mode, okay to fail */
//		at_send_command("AT+HTCENS=1", NULL);

	/*  caller id = yes */
	at_send_command("AT+CLIP=1", NULL);

	/* Alternate Line Support? dual-sim etc.? */
	at_send_command("AT+ALS=0", NULL);

	/*  Alternating voice/data off */
	at_send_command("AT+CMOD=0", NULL);

	at_send_command("AT+GTKC=2", NULL);

	/* For worldphone, check for SIM card. If absent, just use CDMA. */
	if (phone_has == (MODE_GSM|MODE_CDMA)) {
		err = at_send_command_singleline("AT+SIMTYPE", "+SIMTYPE:", &p_response);
		if (err == 0 && p_response->success) {
			char *line = p_response->p_intermediates->line;
			int simtype;
			err = at_tok_start(&line);
			if (err < 0) goto no_gsm;
			err = at_tok_nextint(&line, &simtype);
			if (err < 0) goto no_gsm;
			if (!simtype) goto no_gsm;
			property_set("ro.ril.world_phone", "true");
		} else {
no_gsm:
			phone_has ^= MODE_GSM;
			LOGD("World phone has no SIM card installed, just using CDMA\n");
		}
	}

	/* For CDMA, find operator ID */
	if (phone_has & MODE_CDMA) {
		err = getCDMAOperator();
		if (!err) {
			property_set("ro.cdma.home.operator.numeric", operID);
		}
	}
#if 0
	/* Show battery strength */
	at_send_command("AT+CBC", NULL);
	/* List all supported commands */
	at_send_command("AT+CLAC", NULL);
#endif
}

static void waitForClose()
{
	pthread_mutex_lock(&s_state_mutex);

	while (s_closed == 0) {
		pthread_cond_wait(&s_state_cond, &s_state_mutex);
	}

	pthread_mutex_unlock(&s_state_mutex);
}

/**
 * Called by atchannel when an unsolicited line appears
 * This is called on atchannel's reader thread. AT commands may
 * not be issued here
 */
static void onUnsolicited (const char *s, const char *sms_pdu)
{
	char *line = NULL;
	int err;

	/* Ignore unsolicited responses until we're initialized.
	 * This is OK because the RIL library will poll for initial state
	 */
	if (sState == RADIO_STATE_UNAVAILABLE) {
		return;
	}

	if (strStartsWith(s, "%CTZV:")
			|| strStartsWith(s,"+CTZV:")
			|| strStartsWith(s,"+CTZDST:")
			|| strStartsWith(s,"+HTCCTZV:")) {
		unsolicitedNitzTime(s);
	} else if (strStartsWith(s,"+CRING:")
			|| !strcmp(s, "2") /* RING */
			|| !strcmp(s, "3") /* NO CARRIER */
			|| !strcmp(s, "+PCD: 1,0") /* NO CARRIER */
			|| strStartsWith(s,"+CCWA")
		  ) {
		if (strStartsWith(s,"+CCWA") && phone_is == MODE_CDMA) {
			/* Handle CCWA specially */
			handle_cdma_ccwa(s);
			return;
		}
		if (s[0] == '2' || strStartsWith(s, "+CRING:")) {
			RIL_onUnsolicitedResponse(
				RIL_UNSOL_CALL_RING, NULL, 0);
		}
		err = 0;
		if (s[0] == '3' || !strcmp(s, "+PCD: 1,0")) {
			err = check_data();
			if (err == Data_Connected)
				err = 1;
			else if (err == Data_Dialing)
				err = 2;
			/* +PCD is always data only */
			if (s[0] == '+' && err == 1) {
				RIL_onUnsolicitedResponse (
					RIL_UNSOL_DATA_CALL_LIST_CHANGED,
					NULL, 0);
				return;
			}
		}
		if (err < 2) {
			if (!resentCallState) {
				resentCallState = 1;
				RIL_onUnsolicitedResponse (
					RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
					NULL, 0);
			}
			if (err == 1)
				RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
		}
	} else if (strStartsWith(s,"+XCIEV:")
			|| strStartsWith(s,"$HTC_CSQ:")
			|| strStartsWith(s,"+CSQ:")
			|| strStartsWith(s,"@HTCCSQ:")) {
		unsolicitedRSSI(s);
	} else if (strStartsWith(s,"+CREG:")
			|| strStartsWith(s,"+CGREG:")
			|| strStartsWith(s,"$HTC_SYSTYPE:")) {
		if (!got_state_change) {
			got_state_change=1;
			if (s[0] == '+')
				unsolicitedCREG(s);
			RIL_onUnsolicitedResponse (
				RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED,
				NULL, 0);
		}
/*		RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL); */
	} else if (strStartsWith(s, "+CMT:")) {
		LOGD("GSM_PDU=%s\n",sms_pdu);
		if(phone_is == MODE_CDMA) {
			RIL_CDMA_SMS_Message msg;

			memset(&msg, 0, sizeof(msg));
			decode_cdma_sms_to_ril((char *)sms_pdu, &msg);
			RIL_onUnsolicitedResponse (
					RIL_UNSOL_RESPONSE_CDMA_NEW_SMS,
					&msg, sizeof(msg));
		} else
			RIL_onUnsolicitedResponse (
					RIL_UNSOL_RESPONSE_NEW_SMS,
					sms_pdu, strlen(sms_pdu));
	} else if (strStartsWith(s, "+CDS:")) {
		char new_pdu[512];
		int pdu_len = strlen(sms_pdu);
		/* SMSC seems to be omitted. */
		if (strncmp(sms_pdu, "07", 2)) {
			strcpy(new_pdu, "00");
			strcpy(new_pdu+2, sms_pdu);
			pdu_len += 2;
			sms_pdu = NULL;
		}
		RIL_onUnsolicitedResponse (
				RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT,
				sms_pdu ? sms_pdu : new_pdu, pdu_len);
	} else if (strStartsWith(s, "+CGEV:")) {
		/* Really, we can ignore NW CLASS and ME CLASS events here,
		 * but right now we don't since extranous
		 * RIL_UNSOL_DATA_CALL_LIST_CHANGED calls are tolerated
		 */
		/* can't issue AT commands here -- call on main thread */
		RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#ifdef WORKAROUND_FAKE_CGEV
	} else if (strStartsWith(s, "+CME ERROR: 150")) {
		RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#endif /* WORKAROUND_FAKE_CGEV */
	} else if (strStartsWith(s, "$HTC_ERIIND:")) {
		unsolicitedERI(s);
	} else if (strStartsWith(s, "+CUSD:")) {
		unsolicitedUSSD(s);
	}
}

/* Called on command or reader thread */
static void onATReaderClosed()
{
	LOGI("AT channel closed\n");
	at_close();
	s_closed = 1;

	setRadioState (RADIO_STATE_UNAVAILABLE);
}

#if 0
/* Called on command thread */
static void onATTimeout()
{
	LOGI("AT channel timeout; closing\n");
	at_close();

	s_closed = 1;

	/* FIXME cause a radio reset here */

	setRadioState (RADIO_STATE_UNAVAILABLE);
}
#endif

static void usage(char *s)
{
#ifdef RIL_SHLIB
	fprintf(stderr, "htcgeneric-ril requires: -p <tcp port> or -d /dev/tty_device\n");
#else
	fprintf(stderr, "usage: %s [-p <tcp port>] [-d /dev/tty_device]\n", s);
	exit(-1);
#endif
}

	static void *
mainLoop(void *param)
{
	int fd;
	int ret;

	AT_DUMP("== ", "entering mainLoop()", -1 );
	at_set_on_reader_closed(onATReaderClosed);
#if 0
	/* /dev/smd0 doesn't survive a close, can't be re-opened */
	at_set_on_timeout(onATTimeout);
#endif

	for (;;) {
		fd = -1;
		while  (fd < 0) {
			if (s_port > 0) {
				fd = socket_loopback_client(s_port, SOCK_STREAM);
			} else if (s_device_socket) {
				fd = socket_local_client( s_device_path,
						ANDROID_SOCKET_NAMESPACE_FILESYSTEM,
						SOCK_STREAM );
			} else if (s_device_path != NULL) {
				fd = open (s_device_path, O_RDWR);
				if ( fd >= 0 && !memcmp( s_device_path, "/dev/ttyS", 9 ) ) {

					/* disable echo on serial ports */
					struct termios  ios;
					tcgetattr( fd, &ios );
					ios.c_lflag = 0;  /* disable ECHO, ICANON, etc... */
					tcsetattr( fd, TCSANOW, &ios );
				}
			}

			if (fd < 0) {
				perror ("opening AT interface. retrying...");
				sleep(10);
				/* never returns */
			}
		}

		s_closed = 0;
		ret = at_open(fd, onUnsolicited);

		if (ret < 0) {
			LOGE ("AT error %d on at_open\n", ret);
			return 0;
		}

		RIL_requestTimedCallback(initializeCallback, NULL, &TIMEVAL_0);

		// Give initializeCallback a chance to dispatched, since
		// we don't presently have a cancellation mechanism
		sleep(1);

		waitForClose();
		LOGI("Re-opening after close");
	}
}

#ifdef RIL_SHLIB

pthread_t s_tid_mainloop;
#if 0
/* CDMA mode is now detected dynamically, North American Dialing
 * is handled by the Android CDMA phone framework, and slow_sim
 * is just going to be ignored for now
 */
void parse_cmdline() {
	int fd=open("/proc/cmdline", O_RDONLY);
	char buf[1024];
	char *ptr,*ptr2;
	int i;
	buf[1023]=0;
	if(fd<0)
		return;
	if(read(fd, buf, 1023)<0) {
		close(fd);
		return;
	}
	ptr=strstr(buf, "force_cdma=");
	if(ptr && (ptr==buf || ptr[-1]==' ')) {
		ptr+=strlen("force_cdma=");
		if(atoi(ptr))
			isgsm=0;
		else
			isgsm=1;
	}
	ptr=strstr(buf, "slow_sim=");
	if(ptr && (ptr==buf || ptr[-1]==' ')) {
		ptr+=strlen("slow_sim=");
		slow_sim=atoi(ptr);
	}
	ptr=strstr(buf, "north_am_dialing=");
	if(ptr && (ptr==buf || ptr[-1]==' ')) {
		ptr+=strlen("north_am_dialing=");
		cdma_north_american_dialing=atoi(ptr);
	}
}
#endif

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc, char **argv)
{
	int ret;
	int fd = -1;
	int opt;
	pthread_attr_t attr;
	char buffer[32];

	s_rilenv = env;

	fd=open("/sys/class/htc_hw/radio", O_RDONLY);
	read(fd, buffer, 32);
	if(strncmp(buffer, "GSM",3)==0) {
		phone_has = MODE_GSM;
		phone_is = MODE_GSM;
	} else {
		phone_has = MODE_CDMA;
		phone_is = MODE_CDMA;
	}

	/* If /dev/smd7 exists, use it for data, otherwise defaults to smd1 */
	if (access("/dev/smd7", F_OK) == 0) {
		smd7 = "/dev/smd7";
	}
#if 0
	parse_cmdline();
#endif

	while ( -1 != (opt = getopt(argc, argv, "p:d:s:"))) {
		switch (opt) {
			case 'p':
				s_port = atoi(optarg);
				if (s_port == 0) {
					usage(argv[0]);
					return NULL;
				}
				LOGI("Opening loopback port %d\n", s_port);
				break;

			case 'd':
				s_device_path   = optarg;
				LOGI("Opening tty device %s\n", s_device_path);
				break;

			case 's':
				s_device_path   = optarg;
				s_device_socket = 1;
				LOGI("Opening socket %s\n", s_device_path);
				break;

			default:
				usage(argv[0]);
				return NULL;
		}
	}

	if (s_port < 0 && s_device_path == NULL) {
		s_device_path = "/dev/smd0";
		LOGI("Opening tty device %s\n", s_device_path);
	}

	pthread_attr_init (&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	ret = pthread_create(&s_tid_mainloop, &attr, mainLoop, NULL);

	return &s_callbacks;
}
#else /* RIL_SHLIB */
int main (int argc, char **argv)
{
	int ret;
	int fd = -1;
	int opt;

	while ( -1 != (opt = getopt(argc, argv, "p:d:s:"))) {
		switch (opt) {
			case 'p':
				s_port = atoi(optarg);
				if (s_port == 0) {
					usage(argv[0]);
				}
				LOGI("Opening loopback port %d\n", s_port);
				break;

			case 'd':
				s_device_path = optarg;
				LOGI("Opening tty device %s\n", s_device_path);
				break;

			case 's':
				s_device_path   = optarg;
				s_device_socket = 1;
				LOGI("Opening socket %s\n", s_device_path);
				break;

			default:
				usage(argv[0]);
		}
	}

	if (s_port < 0 && s_device_path == NULL) {
		usage(argv[0]);
	}

	RIL_register(&s_callbacks);

	mainLoop(NULL);

	return 0;
}

#endif /* RIL_SHLIB */

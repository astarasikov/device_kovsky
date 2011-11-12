/* //device/system/reference-ril/atchannel.h
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

#ifndef ATCHANNEL_H
#define ATCHANNEL_H 1

#ifdef __cplusplus
extern "C" {
#endif

/* define AT_DEBUG to send AT traffic to /tmp/radio-at.log" */
#define AT_DEBUG  0

#if AT_DEBUG
extern void  AT_DUMP(const char* prefix, const char*  buff, int  len);
#else
#define  AT_DUMP(prefix,buff,len)  do{}while(0)
#endif

#define AT_ERROR_GENERIC -1
#define AT_ERROR_COMMAND_PENDING -2
#define AT_ERROR_CHANNEL_CLOSED -3
#define AT_ERROR_TIMEOUT -4
#define AT_ERROR_INVALID_THREAD -5 /* AT commands may not be issued from
                                       reader thread (or unsolicited response
                                       callback */
#define AT_ERROR_INVALID_RESPONSE -6 /* eg an at_send_command_singleline that
                                        did not get back an intermediate
                                        response */


typedef enum {
    NO_RESULT,   /* no intermediate response expected */
    NUMERIC,     /* a single intermediate response starting with a 0-9 */
    SINGLELINE,  /* a single intermediate response starting with a prefix */
    MULTILINE    /* multiple line intermediate response
                    starting with a prefix */
} ATCommandType;

/** a singly-lined list of intermediate responses */
typedef struct ATLine  {
    struct ATLine *p_next;
    char *line;
} ATLine;

/** Free this with at_response_free() */
typedef struct {
    int success;              /* true if final response indicates
                                    success (eg "OK") */
    char *finalResponse;      /* eg OK, ERROR */
    ATLine  *p_intermediates; /* any intermediate responses */
} ATResponse;

/**
 * a user-provided unsolicited response handler function
 * this will be called from the reader thread, so do not block
 * "s" is the line, and "sms_pdu" is either NULL or the PDU response
 * for multi-line TS 27.005 SMS PDU responses (eg +CMT:)
 */
typedef void (*ATUnsolHandler)(const char *s, const char *sms_pdu);

int at_open(int fd, ATUnsolHandler h);
void at_close();

/* This callback is invoked on the command thread.
   You should reset or handshake here to avoid getting out of sync */
void at_set_on_timeout(void (*onTimeout)(void));
/* This callback is invoked on the reader thread (like ATUnsolHandler)
   when the input stream closes before you call at_close
   (not when you call at_close())
   You should still call at_close()
   It may also be invoked immediately from the current thread if the read
   channel is already closed */
void at_set_on_reader_closed(void (*onClose)(void));

int at_send_command_singleline (const char *command,
                                const char *responsePrefix,
                                 ATResponse **pp_outResponse);

int at_send_command_numeric (const char *command,
                                 ATResponse **pp_outResponse);

int at_send_command_multiline (const char *command,
                                const char *responsePrefix,
                                 ATResponse **pp_outResponse);


int at_handshake();

int at_send_command (const char *command, ATResponse **pp_outResponse);

int at_send_command_sms (const char *command, const char *pdu,
                            const char *responsePrefix,
                            ATResponse **pp_outResponse);

void at_response_free(ATResponse *p_response);

char *at_get_last_error();

typedef enum {
	CME_NO_ERROR = -1,
	CME_PHONE_FAILURE = 0,
	CME_PHONE_NO_CONNECTION,
	CME_PHONE_LINK_RESERVED,
	CME_OPERATION_NOT_ALLOWED,
	CME_OPERATION_NOT_SUPPORTED,
	CME_PH_SIM_PIN_REQD,
	CME_PH_FSIM_PIN_REQD,
	CME_PH_FSIM_PUK_REQD,
	CME_SIM_NOT_INSERTED=10,
	CME_SIM_PIN_REQD,
	CME_SIM_PUK_REQD,
	CME_SIM_FAILURE,
	CME_SIM_BUSY,
	CME_SIM_WRONG,
	CME_INCORRECT_PASSWORD,
	CME_SIM_PIN2_REQD,
	CME_SIM_PUK2_REQD,
	CME_MEMORY_FULL=20,
	CME_INVALID_INDEX,
	CME_NOT_FOUND,
	CME_MEMORY_FAILURE,
	CME_TEXT_STRING_TOO_LONG,
	CME_INVALID_CHARS_IN_TEXT,
	CME_DIAL_STRING_TOO_LONG,
	CME_INVALID_CHARS_IN_DIAL,
	CME_NO_NETWORK_SERVICE=30,
	CME_NETWORK_TIMEOUT,
	CME_NETWORK_NOT_ALLOWED,
	CME_NETWORK_PERS_PIN_REQD=40,
	CME_NETWORK_PERS_PUK_REQD,
	CME_NETWORK_SUBSET_PERS_PIN_REQD,
	CME_NETWORK_SUBSET_PERS_PUK_REQD,
	CME_PROVIDER_PERS_PIN_REQD,
	CME_PROVIDER_PERS_PUK_REQD,
	CME_CORP_PERS_PIN_REQD,
	CME_CORP_PERS_PUK_REQD,
	CME_PH_SIM_PUK_REQD,
	CME_UNKNOWN_ERROR=100,
	CME_ILLEGAL_MS=103,
	CME_ILLEGAL_ME=106,
	CME_GPRS_NOT_ALLOWED,
	CME_PLMN_NOT_ALLOWED=111,
	CME_LOCATION_NOT_ALLOWED,
	CME_ROAMING_NOT_ALLOWED,
	CME_TEMPORARY_NOT_ALLOWED=126,
	CME_SERVICE_OPTION_NOT_SUPPORTED=132,
	CME_SERVICE_OPTION_NOT_SUBSCRIBED,
	CME_SERVICE_OPTION_OUT_OF_ORDER,
	CME_UNSPECIFIED_GPRS_ERROR=148,
	CME_PDP_AUTHENTICATION_FAILED,
	CME_INVALID_MOBILE_CLASS,
	CME_OPERATION_TEMP_NOT_ALLOWED=256,
	CME_CALL_BARRED,
	CME_PHONE_BUSY,
	CME_USER_ABORT,
	CME_INVALID_DIAL_STRING,
	CME_SS_NOT_EXECUTED,
	CME_SIM_BLOCKED,
	CME_INVALID_BLOCK,
	CME_SIM_POWERED_DOWN=772
} AT_CME_Error;

AT_CME_Error at_get_cme_error();

#ifdef __cplusplus
}
#endif

#endif /*ATCHANNEL_H*/

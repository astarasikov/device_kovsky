/*
 * Author: Jbruneaux
 *
 * Description : provide interface between userland and kernel for the 
 * acoustic management
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define LOG_NDEBUG 0
#define LOG_TAG "Libacoustic-wince"
#include <cutils/log.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <assert.h>
#include <stdlib.h>
#include <ctype.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdbool.h>

#include <linux/msm_audio.h>

#include "libacoustic.h"

#define AUDIO_PARA_CUSTOM_FILENAME        "/sdcard/AudioPara3.csv"
#define AUDIO_PARA_DEFAULT_FILENAME       "/system/etc/AudioPara3.csv"

#define AUDIO_FILTER_CUSTOM_FILENAME      "/sdcard/AudioFilterTable.csv"
#define AUDIO_FILTER_DEFAULT_FILENAME     "/system/etc/AudioFilterTable.csv"

#define AUDIO_PREPROCESS_CUSTOM_FILENAME  "/sdcard/AudioPreProcessTable.csv"
#define AUDIO_PREPROCESS_DEFAULT_FILENAME "/system/etc/AudioPreProcessTable.csv"

#define PCM_OUT_DEVICE      "/dev/msm_pcm_out"
#define PCM_IN_DEVICE       "/dev/msm_pcm_in"
#define PCM_CTL_DEVICE      "/dev/msm_pcm_ctl"
#define PREPROC_CTL_DEVICE  "/dev/msm_audpre"

struct au_table_s {
    union {
        struct au_table_st table;
        char   array[0x80];
    };
};

struct be_table_s {
    union {
        struct be_table_st table;
        uint16_t array[0x80];
    };
};

struct d_table_s {
    union {
        struct d_table_st table;
        uint16_t array[0xB];
    };
};

struct fg_table_s {
    union {
        struct fg_table_st table;
        uint16_t array[0xA0];
    };
};

struct c_table_s {
    union {
        struct fg_table_st table;
        uint16_t array[0x50];  
    };
};

/***********************************************************************************
 *
 *  Global variables
 *
 ***********************************************************************************/
static struct au_table_s*    Audio_Path_Table = NULL;              /* a_table ('A') */
static uint8_t APT_max_index   = 0;
static struct au_table_s*    Audio_Path_Uplink_Table = NULL;       /* u_table ('U') */
static uint8_t APUT_max_index  = 0;
static struct fg_table_s*    Phone_Acoustic_Table = NULL;          /* f_table ('F' + 'B') */
static uint8_t PAT_max_index   = 0;
static struct fg_table_s*    BT_Phone_Acoustic_Table = NULL;       /* g_table ('E' + 'G') */
static uint8_t BTPAT_max_index = 0;
static struct d_table_s*     HTC_VOC_CAL_CODEC_TABLE_Table = NULL; /* d_table ('D') */
static uint8_t HVCCT_max_index = 0;
static struct c_table_s*     CE_Acoustic_Table = NULL;             /* c_table ('C') */
static uint8_t CEAT_max_index  = 0;

/* Communication with kernel */
static int acousticfd = 0;
static int mNumSndEndpoints;
static struct msm_snd_endpoint *mSndEndpoints;
static struct msm_acoustic_capabilities device_capabilities;
static int TPA2016fd;

static bool mInit = false;

static struct rx_iir_filter iir_cfg[1];
static struct adrc_filter adrc_cfg[1];
static struct eqalizer eqalizer[1];
static uint16_t adrc_flag[1];
static uint16_t eq_flag[1];
static uint16_t rx_iir_flag[1];
static bool audpp_filter_inited = false;
static bool audpre_filter_inited = false;
static bool adrc_filter_exists[1];

static struct tx_iir tx_iir_cfg[18];    // Normal + Full DUplex
static struct ns ns_cfg[9];
static bool audpre_ns_cfg_exist = false;
static struct tx_agc tx_agc_cfg[9];
static bool audpre_tx_agc_cfg_exist = false;

// Current TPA2016 registers value initialized with default values
static uint8_t tpa2016d2_regs[7] = { 0xC3, 0x05, 0x0B, 0x00, 0x06, 0x3A, 0xC2 };
/* Values from TIAGC.csv */
static const uint8_t tpa2016d2_regs_audio[7] = { 0xC2, 0x20, 0x01, 0x00, 0x10, 0x19, 0xC0 };
static const uint8_t tpa2016d2_regs_voice[7] = { 0xC3, 0x3F, 0x01, 0x00, 0x14, 0x7A, 0xC0 };

static int SND_DEVICE_CURRENT;
static int SND_DEVICE_HANDSET;
static int SND_DEVICE_SPEAKER;
static int SND_DEVICE_HEADSET;
static int SND_DEVICE_SPEAKER_MIC=1;
static int SND_DEVICE_BT;
static int SND_DEVICE_CARKIT;
static int SND_DEVICE_TTY_FULL;
static int SND_DEVICE_TTY_VCO;
static int SND_DEVICE_TTY_HCO;
static int SND_DEVICE_NO_MIC_HEADSET;
static int SND_DEVICE_FM_HEADSET;
static int SND_DEVICE_HEADSET_AND_SPEAKER;
static int SND_DEVICE_FM_SPEAKER;
static int SND_DEVICE_BT_EC_OFF;
static int SND_DEVICE_IDLE;

/* Specific pass-through device for special ops */
#define BT_CUSTOM_DEVICES_ID_OFFSET     300     // This will be used for bluetooth custom devices

static int SND_DEVICE_REC_INC_MIC = 252;
static int SND_DEVICE_PLAYBACK_HANDSFREE = 253;
static int SND_DEVICE_PLAYBACK_HEADSET = 254;

static int mCurrentSndDevice = -1;
static int mCurrentVolume = 0;
static int mCurrent_Adie_PGA_Gain = 1;


static bool bCurrentAudioUplinkState = 0;
static bool bCurrentEnableHSSDState = 0;
static bool bCurrentAUXBypassReqState = 0;

/***********************************************************************************
 *
 *  Privates functions
 *
 ***********************************************************************************/
static int openacousticfd(void)
{
    acousticfd = open(MSM_HTC_ACOUSTIC_WINCE, O_RDWR);
    if ( acousticfd < 0 ) {
        LOGE("Error opening dev %s (fd = %d). Error %s (%d)", MSM_HTC_ACOUSTIC_WINCE, acousticfd,
                    strerror(errno), errno);
        return -1;
    } 
    return 0;
}

static int get_device_capabilities(void)
{
    int bOn;
    if ( ioctl(acousticfd, ACOUSTIC_GET_CAPABILITIES, &device_capabilities) < 0) {
        LOGE("ACOUSTIC_GET_CAPABILITIES error.");
        return -EIO;
    } 
    LOGV("Device capabilities :");
    LOGV("- Htc voc cal fields per params : %d", device_capabilities.htc_voc_cal_fields_per_param);
    LOGV("- Dual mic supported : %s", (device_capabilities.bDualMicSupported)?"true":"false");

    /* Test for TPA2016 */
    TPA2016fd = open(MSM_TPA2016D2_DEV, O_RDWR);
    if ( TPA2016fd < 0 ) {
        LOGE("Error opening dev %s (fd = %d). Error %s (%d)", MSM_TPA2016D2_DEV, acousticfd,
                    strerror(errno), errno);
    } else {
        /* Power on amplifier */
        bOn = 1;
        if (ioctl(TPA2016fd, TPA2016_SET_POWER, &bOn ) < 0) {
            LOGE("TPA2016_SET_POWER error.");
            return -EIO;
        }

        /* Read current device configuration */
        if (ioctl(TPA2016fd, TPA2016_READ_CONFIG, &tpa2016d2_regs ) < 0) {
            LOGE("TPA2016_READ_CONFIG error.");
            return -EIO;
        }  

        /* Power off amplifier */
        bOn = 0;
        if (ioctl(TPA2016fd, TPA2016_SET_POWER, &bOn ) < 0) {
            LOGE("TPA2016_SET_POWER error.");
            return -EIO;
        }
    } 
    LOGV("- Use TPA2016 Amplifier : %s", (TPA2016fd >= 0)?"true":"false");
    return 0;
}

static int get_sound_endpoints(void)
{
    int cnt, rc = -EIO;
    int m7xsnddriverfd;
    struct msm_snd_endpoint *ept;

    m7xsnddriverfd = open("/dev/msm_snd", O_RDWR);
    if (m7xsnddriverfd >= 0) {
        rc = ioctl(m7xsnddriverfd, SND_GET_NUM_ENDPOINTS, &mNumSndEndpoints);
        if (rc >= 0) {
            mSndEndpoints = malloc(mNumSndEndpoints * sizeof(struct msm_snd_endpoint));
            mInit = true;
            LOGV("constructed (%d SND endpoints)", mNumSndEndpoints);
            struct msm_snd_endpoint *ept = mSndEndpoints;
            for (cnt = 0; cnt < mNumSndEndpoints; cnt++, ept++) {
                ept->id = cnt;
                ioctl(m7xsnddriverfd, SND_GET_ENDPOINT, ept);
                LOGV("cnt = %d ept->name = %s ept->id = %d\n", cnt, ept->name, ept->id);
#define CHECK_FOR(desc) if (!strcmp(ept->name, #desc)) SND_DEVICE_##desc = ept->id;
                CHECK_FOR(CURRENT)
                CHECK_FOR(HANDSET)
                CHECK_FOR(SPEAKER)
                CHECK_FOR(SPEAKER_MIC)
                CHECK_FOR(BT)
                CHECK_FOR(BT_EC_OFF)
                CHECK_FOR(HEADSET)
                CHECK_FOR(CARKIT)
                CHECK_FOR(TTY_FULL)
                CHECK_FOR(TTY_VCO)
                CHECK_FOR(TTY_HCO)
                CHECK_FOR(NO_MIC_HEADSET)
                CHECK_FOR(FM_HEADSET)
                CHECK_FOR(FM_SPEAKER)
                CHECK_FOR(HEADSET_AND_SPEAKER)
                CHECK_FOR(IDLE)
#undef CHECK_FOR
            }
        }
        else LOGE("Could not retrieve number of MSM SND endpoints.");

        // Close snd
        close(m7xsnddriverfd);
    }
	else LOGE("Could not open MSM SND driver.");

    return rc;
}


static int get_pga_gain_for_fm_profile(int profile, int current_pga_gain)
{
    // FM HEADSET index
    if ( (profile == 18) | (profile == 19) ) {
        if ( current_pga_gain < 0xB ) {
            return ( 0x34 - (current_pga_gain * 4) );
        }
        return 0xFF; 
    }
    // FM SPEAKER index
    else if ( (profile == 22) | (profile == 23) ) {
        if ( current_pga_gain < 0xB ) {
            return ( 0x28 - (current_pga_gain * 4) );
        }
        return 0xFF; 
    }
    return 0;
}

static int UpdateAudioAdieTable(bool bAudioUplinkReq, int paramR1, bool bEnableHSSD, bool bAUXBypassReq, bool bForceUpdate)
{
    struct adie_table table;
    int table_num = 0;
    int tab_byte_idx;

    char temp_table[0x80];
    uint16_t pcom_update[1];

    LOGV("UpdateAudioAdieTable(bAudioUplinkReq %d,bAUXBypassReq %d, bEnableHSSD=%d, bForceUpdate = %d)\n",
            bAudioUplinkReq, bEnableHSSD, bAUXBypassReq, bForceUpdate);

    if ( (bCurrentAudioUplinkState  == bAudioUplinkReq) && 
         (bCurrentEnableHSSDState   == bEnableHSSD ) &&
         (bCurrentAUXBypassReqState == bAUXBypassReq) && 
         (bForceUpdate == false) ) {
        LOGV("Update not required");
        return 0;
    }

    do 
    {
        memset(temp_table, 0, 0x80);
        tab_byte_idx = 0;
        
        do
        {
            temp_table[tab_byte_idx] = Audio_Path_Table[table_num].array[tab_byte_idx];
            temp_table[tab_byte_idx + 1] = (temp_table[tab_byte_idx + 1] | Audio_Path_Table[table_num].array[tab_byte_idx + 1]);

            if ( (!(table_num & 1)) && ( bAudioUplinkReq != 0 ) ) {
                temp_table[tab_byte_idx + 1] = (Audio_Path_Uplink_Table[table_num].array[tab_byte_idx + 1] 
                                                     | Audio_Path_Table[table_num].array[tab_byte_idx + 1]);
            }

            if ( paramR1 == 0 ) {
                if ( bEnableHSSD != 0 ) {
                    if ( Audio_Path_Table[table_num].array[tab_byte_idx] == 0x37 ) {
                        temp_table[tab_byte_idx + 1] |= 0x80;
                    }
                    if ( Audio_Path_Table[table_num].array[tab_byte_idx] == 0x48 ) {
                        temp_table[tab_byte_idx + 1] |= 0xC0;
                    }
                }
            } else {
                if ( Audio_Path_Table[table_num].array[tab_byte_idx] == 0x3E ) {
                    temp_table[tab_byte_idx + 1] = (temp_table[tab_byte_idx + 1] & 0xE7) | 0x10;           
                }
            }

            if ( (bAUXBypassReq != 0) && (Audio_Path_Table[table_num].array[tab_byte_idx] == 0x42) ) {
                temp_table[tab_byte_idx + 1] = get_pga_gain_for_fm_profile(table_num, mCurrent_Adie_PGA_Gain);
            }

            tab_byte_idx += 2;
        }
        while( tab_byte_idx < 0x80);
        
        /* Send table to kernel for update */
        table.table_num = table_num;
        table.pcArray = temp_table;
        if ( ioctl(acousticfd, ACOUSTIC_UPDATE_ADIE_TABLE, &table) < 0) {
            LOGE("ACOUSTIC_UPDATE_ADIE_TABLE error.");
            return -EIO;
        }    

        table_num += 1;
    }
    while ( table_num < APT_max_index );

    /* Generate PCOM_UPDATE_AUDIO 0x1 */
    struct audio_update_req req = {.type = PCOM_UPDATE_REQ, .value = 0x1};
    if ( ioctl(acousticfd, ACOUSTIC_UPDATE_AUDIO_SETTINGS, &req) < 0) {
        LOGE("ACOUSTIC_UPDATE_AUDIO_SETTINGS error.");
        return -EIO;
    }     

    bCurrentAudioUplinkState  = bAudioUplinkReq;
    bCurrentEnableHSSDState   = bEnableHSSD;
    bCurrentAUXBypassReqState = bAUXBypassReq;

    return 0;
}


static int ParseAudioParaLine(char* line, int len)
{
    char *token, *ps;
    int table_num;
    int field_count = 0;

    /* Parse the first field */
    token = strtok(line, ",");
    switch ( token[0] ) {
        case 'A':
            table_num = strtol(token + 1, &ps, 10);
            if ( table_num > 31) {
                return -EINVAL;
            }
            if ( APT_max_index < (table_num+1) ) APT_max_index = table_num+1;
            //LOGV("Audio Path Table: %d\n", table_num);
            /* Skip the mode name string field */
            strtok(NULL, ",");
            while ( (token = strtok(NULL, ",")) ) {
                Audio_Path_Table[table_num].array[field_count++] = strtol(token, &ps, 16);
            };
            break;

        case 'B':
        case 'F':
            table_num = strtol(token + 1, &ps, 10);
            if ( table_num > 99) {
                return -EINVAL;
            }
            if ( PAT_max_index < (table_num+1) ) PAT_max_index = table_num+1;
            //LOGV("Phone Acoustic Table: %d\n", table_num);
            /* Skip the mode name string field */
            strtok(NULL, ",");
            while ( (token = strtok(NULL, ",")) ) {
                Phone_Acoustic_Table[table_num].array[field_count++] = strtol(token, &ps, 16);
            };
            break;

        case 'C':
            table_num = strtol(token + 1, &ps, 10);
            if ( table_num > 14) {
                return -EINVAL;
            }
            if ( CEAT_max_index < (table_num+1) ) CEAT_max_index = table_num+1;
            //LOGV("CE Acoustic Table: %d\n", table_num);
            /* Skip the mode name string field */
            strtok(NULL, ",");
            while ( (token = strtok(NULL, ",")) ) {
                CE_Acoustic_Table[table_num].array[field_count++] = strtol(token, &ps, 16);
            };
            break;

        case 'D':
            table_num = strtol(token + 1, &ps, 10);
            if ( table_num > 31) {
                return -EINVAL;
            }
            if ( HVCCT_max_index < (table_num+1) ) HVCCT_max_index = table_num+1;
            //LOGV("HTC_VOC_CAL_CODEC_TABLE Table: %d\n", table_num);
            /* Skip the mode name string field */
            strtok(NULL, ",");
            while ( (token = strtok(NULL, ",")) ) {
                HTC_VOC_CAL_CODEC_TABLE_Table[table_num].array[field_count++] = strtol(token, &ps, 16);
            };
            break;

        case 'E':
        case 'G':
#if 0
            table_num = strtol(token + 1, &ps, 10);
            if ( table_num > 100) {
                return -EINVAL;
            }
            if ( BTPAT_max_index < (table_num+1) ) BTPAT_max_index = table_num+1;
            //LOGV("BT Phone Acoustic Table: %d\n", table_num);
            /* Skip the mode name string field */
            strtok(NULL, ",");
            while ( (token = strtok(NULL, ",")) ) {
                BT_Phone_Acoustic_Table[table_num].array[field_count++] = strtol(token, &ps, 16);
            };
#endif
            /* Skip the table number field */
            token = strtok(NULL, ",");
            if ( BTPAT_max_index < 100 ) {  
                strncpy(BT_Phone_Acoustic_Table[BTPAT_max_index].table.name, token, MAX_MODE_NAME_LENGTH);          
                LOGV("BT Phone Acoustic Table: %s\n", BT_Phone_Acoustic_Table[BTPAT_max_index].table.name);
                while ( (token = strtok(NULL, ",")) ) {
                    BT_Phone_Acoustic_Table[BTPAT_max_index].array[field_count++] = strtol(token, &ps, 16);
                };
                BTPAT_max_index++;
            }
            break;

        case 'H':
            //LOGV("It's just a header line, skip it\n");
            return 0;

        case 'U':
            table_num = strtol(token + 1, &ps, 10);
            if ( table_num > 31) {
                return -EINVAL;
            }
            if ( APUT_max_index < (table_num+1) ) APUT_max_index = table_num+1;
            //LOGV("Audio Path Table Uplink: %d\n", table_num);
            /* Skip the mode name string field */
            strtok(NULL, ",");
            while ( (token = strtok(NULL, ",")) ) {
                Audio_Path_Uplink_Table[table_num].array[field_count++] = strtol(token, &ps, 16);
            };
            break;

        default:
            LOGE("Unknown parameter field %c\n", token[0]);
            return -1;
    }
    
    return 0;

}

static int ReadAudioParaFromFile(void)
{
    struct stat st;
    char *read_buf;
    char *next_str, *current_str;
    int csvfd;
    struct htc_voc_cal_table htc_voc_cal_tbl;
    uint16_t* htc_voc_cal_tbl_conv = NULL;

    static const char *path =
        AUDIO_PARA_CUSTOM_FILENAME;

    csvfd = open(path, O_RDONLY);
    if (csvfd < 0) {
        /* Failed to open custom parameters file, fallback to the default file ... */        
        LOGE("Failed to open %s. Error %s (%d)\n",
             path, strerror(errno), errno);

        LOGE("Trying with default file");
        path = AUDIO_PARA_DEFAULT_FILENAME;
        csvfd = open(path, O_RDONLY);
        if (csvfd < 0) {
            LOGE("Failed to open %s. Error %s (%d)\n",
                 path, strerror(errno), errno);
            return -1;
        }
    }
    
    LOGE("Successfully opened %s\n", path);

    if (fstat(csvfd, &st) < 0) {
        LOGE("Failed to stat %s: %s (%d)\n",
             path, strerror(errno), errno);
        close(csvfd);
        return -1;
    }

    read_buf = (char *) mmap(0, st.st_size,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE,
                    csvfd, 0);

    if (read_buf == MAP_FAILED) {
        LOGE("Failed to mmap parameters file: %s (%d)\n",
             strerror(errno), errno);
        close(csvfd);
        return -1;
    }
   

    if ( Audio_Path_Table == NULL ) {
        Audio_Path_Table = (struct au_table_s*) malloc(32 * sizeof(struct au_table_s) );  // 0x1000
        if (Audio_Path_Table == NULL) {
            LOGE("Failed to malloc Audio_Path_Table\n");
            return -1;
        }
    }

    if ( Audio_Path_Uplink_Table == NULL ) {
        Audio_Path_Uplink_Table = (struct au_table_s*) malloc(32 * sizeof(struct au_table_s) ); // 0x1000
        if (Audio_Path_Uplink_Table == NULL) {
            LOGE("Failed to malloc Audio_Path_Uplink_Table\n");
            return -1;
        }
    }

    if ( Phone_Acoustic_Table == NULL ) {
        Phone_Acoustic_Table = (struct fg_table_s*) malloc(100 * sizeof(struct fg_table_s) ); // 0x3C00
        if (Phone_Acoustic_Table == NULL) {
            LOGE("Failed to malloc Phone_Acoustic_Table\n");
            return -1;
        }
    }

    if ( BT_Phone_Acoustic_Table == NULL ) {
        BT_Phone_Acoustic_Table = (struct fg_table_s*) malloc(100 * sizeof(struct fg_table_s) );  // 0x7800
        if (BT_Phone_Acoustic_Table == NULL) {
            LOGE("Failed to malloc BT_Phone_Acoustic_Table\n");
            return -1;
        }
    }

    if ( HTC_VOC_CAL_CODEC_TABLE_Table == NULL ) {
        HTC_VOC_CAL_CODEC_TABLE_Table = (struct d_table_s*) malloc(32 * sizeof(struct d_table_s));
        if (HTC_VOC_CAL_CODEC_TABLE_Table == NULL) {
            LOGE("Failed to malloc HTC_VOC_CAL_CODEC_TABLE_Table\n");
            return -1;
        }
    }

    if ( CE_Acoustic_Table == NULL ) {
        CE_Acoustic_Table = (struct c_table_s*) malloc(15 * sizeof(struct c_table_s) );   // 0x960
        if (CE_Acoustic_Table == NULL) {
            LOGE("Failed to malloc CE_Acoustic_Table\n");
            return -1;
        }
    }

    current_str = read_buf;

    while (1) {
        int len;
        next_str = strchr(current_str, '\n');
        if (!next_str)
           break;
        len = next_str - current_str;
        *next_str++ = '\0';
        if ( ParseAudioParaLine(current_str, len) < 0 ) {
            break;
        }

        current_str = next_str;
    }

    munmap(read_buf, st.st_size);
    close(csvfd);

    LOGI("Readed :");
    LOGI("%d Audio_Path_Table entries", APT_max_index);
    LOGI("%d Audio_Path_Uplink_Table entries", APUT_max_index);
    LOGI("%d Phone_Acoustic_Table entries", PAT_max_index);
    LOGI("%d BT_Phone_Acoustic_Table entries", BTPAT_max_index);
    LOGI("%d HTC_VOC_CAL_CODEC_TABLE_Table entries", HVCCT_max_index);
    LOGI("%d CE_Acoustic_Table entries", CEAT_max_index);

    // initialise audio table with uplink off
    UpdateAudioAdieTable(0, 0, 0, 0, true);

    /* Table might need to be converted (on some devices, 1 setting is 8 params long,
     * whereas on some others, it's 0xB params long)
     * Note that this is the only one field size that varies between those devices.
     * All other field types are same size.
     */
    if ( device_capabilities.htc_voc_cal_fields_per_param < 0xB ) {
        int field;
        uint16_t* htc_voc_cal_tbl_conv_field;
        /* Convert table to required field size */
        htc_voc_cal_tbl_conv = (uint16_t*) malloc(HVCCT_max_index * device_capabilities.htc_voc_cal_fields_per_param * sizeof(uint16_t));
        if ( htc_voc_cal_tbl_conv == NULL ) {
            goto exit;
        }

        for (field=0; field<HVCCT_max_index; field++) {
            htc_voc_cal_tbl_conv_field = &htc_voc_cal_tbl_conv[field * device_capabilities.htc_voc_cal_fields_per_param];
            memcpy((void*) htc_voc_cal_tbl_conv_field,
                           HTC_VOC_CAL_CODEC_TABLE_Table[field].array,
                           device_capabilities.htc_voc_cal_fields_per_param * sizeof(uint16_t));
        }
        /* Fill the stucture with converted table that will be passed to kernel for update */
        htc_voc_cal_tbl.size = HVCCT_max_index * device_capabilities.htc_voc_cal_fields_per_param * sizeof(uint16_t);
        htc_voc_cal_tbl.pArray = htc_voc_cal_tbl_conv;
    } else {
        /* Fill the stucture with table that will be passed to kernel for update */
        htc_voc_cal_tbl.size = HVCCT_max_index * sizeof(struct d_table_s);
        htc_voc_cal_tbl.pArray = HTC_VOC_CAL_CODEC_TABLE_Table->array;
    }

    if (ioctl(acousticfd, ACOUSTIC_UPDATE_HTC_VOC_CAL_CODEC_TABLE,
                         &htc_voc_cal_tbl ) < 0) {
        LOGE("ACOUSTIC_UPDATE_HTC_VOC_CAL_CODEC_TABLE error.");
        return -EIO;
    }

    if ( htc_voc_cal_tbl_conv ) {
        free(htc_voc_cal_tbl_conv);
    }

exit:
/*
    if ( BT_Phone_Acoustic_Table[0] == 0 ) {
        memcpy(&BT_Phone_Acoustic_Table[0x40], &f_table[0x1680], 0x140);
    }
*/

    return 0;
}



/* Imports from codeaurora (check_and_set_audpp_parameters), adaptated/splitted for wince devices */
static int check_and_set_audpre_parameters(char *buf, int size)
{
    char *p, *ps;
    static const char *const seps = ",";
    int i, j;
    int device_id = 0;
    int samp_index = 0;
    int fd;

    if (buf[0] == 'A')  {
        /* TX_IIR filter */
        if (!(p = strtok(buf, ","))){
            goto token_err;}

        /* Table header */
        samp_index = strtol(p + 1, &ps, 10);
        LOGV("Found TX_IIR filter %d", samp_index); 
        /* Index range = 0..17 */
        if ( samp_index > 17 ) {
            return -EINVAL;
        }

        if (!(p = strtok(NULL, seps))){
            goto token_err;}
        /* Table description */
        if (!(p = strtok(NULL, seps))){
            goto token_err;}

        for (i = 0; i < 48; i++) {
            j = (i >= 40)? i : ((i % 2)? (i - 1) : (i + 1));
            tx_iir_cfg[samp_index].iir_params[j] = (uint16_t)strtol(p, &ps, 16);
            if (!(p = strtok(NULL, seps))){
                goto token_err;}
        }

        tx_iir_cfg[samp_index].active_flag = (uint16_t)strtol(p, &ps, 16);
        if (!(p = strtok(NULL, seps))){
            goto token_err;}

        tx_iir_cfg[samp_index].num_bands = (uint16_t)strtol(p, &ps, 16);
    } else if(buf[0] == 'B')  {
        /* AGC filter */
        if (!(p = strtok(buf, ",")))
            goto token_err;

        /* Table header */
        samp_index = strtol(p + 1, &ps, 10);
        LOGV("Found AGC filter %d", samp_index); 
        /* Index range = 0..8 */
        if ( samp_index > 8 ) {
            return -EINVAL;
        }

        if (!(p = strtok(NULL, seps)))
            goto token_err;
        /* Table description */
        if (!(p = strtok(NULL, seps)))
            goto token_err;
        /* Enable */
        if (!(p = strtok(NULL, seps)))
            goto token_err;

        tx_agc_cfg[samp_index].static_gain = (uint16_t)strtol(p, &ps, 16);
        if (!(p = strtok(NULL, seps)))
            goto token_err;

        tx_agc_cfg[samp_index].adaptive_gain_flag = (uint16_t)strtol(p, &ps, 16);
        if (!(p = strtok(NULL, seps)))
            goto token_err;

        for (i = 0; i < 18; i++) {
            tx_agc_cfg[samp_index].agc_params[i] = (uint16_t)strtol(p, &ps, 16);
            if (!(p = strtok(NULL, seps)))
                goto token_err;
            }

        audpre_tx_agc_cfg_exist = true;

    } else if ((buf[0] == 'C')) {       
        /* This is the NS record we are looking for.  Tokenize it */
        if (!(p = strtok(buf, ",")))
            goto token_err;

        /* Table header */
        samp_index = strtol(p + 1, &ps, 10);
        LOGV("Found NS record %d", samp_index); 
        /* Index range = 0..8 */
        if ( samp_index > 8 ) {
            return -EINVAL;
        }

        if (!(p = strtok(NULL, seps)))
            goto token_err;

        /* Table description */
        if (!(p = strtok(NULL, seps)))
            goto token_err;
        /* Enable */
        if (!(p = strtok(NULL, seps)))
            goto token_err;
        ns_cfg[samp_index].dens_gamma_n = (uint16_t)strtol(p, &ps, 16);

        if (!(p = strtok(NULL, seps)))
            goto token_err;
        ns_cfg[samp_index].dens_nfe_block_size = (uint16_t)strtol(p, &ps, 16);

        if (!(p = strtok(NULL, seps)))
            goto token_err;
        ns_cfg[samp_index].dens_limit_ns = (uint16_t)strtol(p, &ps, 16);

        if (!(p = strtok(NULL, seps)))
            goto token_err;
        ns_cfg[samp_index].dens_limit_ns_d = (uint16_t)strtol(p, &ps, 16);

        if (!(p = strtok(NULL, seps)))
            goto token_err;
        ns_cfg[samp_index].wb_gamma_e = (uint16_t)strtol(p, &ps, 16);

        if (!(p = strtok(NULL, seps)))
            goto token_err;
        ns_cfg[samp_index].wb_gamma_n = (uint16_t)strtol(p, &ps, 16);

        audpre_ns_cfg_exist = true;
    }
    return 0;

token_err:
    LOGE("malformatted preprocessor control buffer");
    return -EINVAL;
}

static int get_audpre_table(void)
{
    struct stat st;
    char *read_buf;
    char *next_str, *current_str;
    int csvfd;

    LOGI("get_audpre_table");
    static const char *path =
            AUDIO_PREPROCESS_CUSTOM_FILENAME;

    csvfd = open(path, O_RDONLY);
    if (csvfd < 0) {
        /* Failed to open custom parameters file, fallback to the default file ... */        
        LOGE("Failed to open AUDIO_NORMAL_PREPROCESS %s. Error %s (%d)\n",
             path, strerror(errno), errno);

        LOGE("Trying with default file");
        path = AUDIO_PREPROCESS_DEFAULT_FILENAME;
        csvfd = open(path, O_RDONLY);
        if (csvfd < 0) {
            LOGE("Failed to open %s. Error %s (%d)\n",
                 path, strerror(errno), errno);
            return -1;
        }
    }
    
    LOGE("Successfully opened %s\n", path);

    if (fstat(csvfd, &st) < 0) {
        LOGE("failed to stat %s: %s (%d).",
             path, strerror(errno), errno);
        close(csvfd);
        return -1;
    }

    read_buf = (char *) mmap(0, st.st_size,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE,
                    csvfd, 0);

    if (read_buf == MAP_FAILED) {
        LOGE("failed to mmap parameters file: %s (%d)",
             strerror(errno), errno);
        close(csvfd);
        return -1;
    }

    current_str = read_buf;

    while (1) {
        int len;
        next_str = strchr(current_str, '\n');
        if (!next_str)
           break;
        len = next_str - current_str;
        *next_str++ = '\0';
        if (check_and_set_audpre_parameters(current_str, len)) {
            LOGI("failed to set audpre parameters, exiting.");
            munmap(read_buf, st.st_size);
            close(csvfd);
            return -1;
        }
        current_str = next_str;
    }

    munmap(read_buf, st.st_size);
    close(csvfd);
    return 0;
}

static int check_and_set_audpp_parameters(char *buf, int size)
{
    char *p, *ps;
    static const char *const seps = ",";
    int table_num;
    int i, j;
    int samp_index = 0;
    struct eq_filter_type eq[12];
    int fd;
    void *audioeq;
    void *(*eq_cal)(int32_t, int32_t, int32_t, uint16_t, int32_t, int32_t *, int32_t *, uint16_t *);
    uint16_t numerator[6];
    uint16_t denominator[4];
    uint16_t shift[2];

    if ( (buf[0] == 'A') && (buf[1] == '1') ) {
        LOGV("Found IIR filter");
        /* IIR filter */
        if (!(p = strtok(buf, ",")))
            goto token_err;

        /* Table header */
        table_num = strtol(p + 1, &ps, 10);
        if (!(p = strtok(NULL, seps)))
            goto token_err;
        /* Table description */
        if (!(p = strtok(NULL, seps)))
            goto token_err;

        for (i = 0; i < 48; i++) {
            j = (i >= 40)? i : ((i % 2)? (i - 1) : (i + 1));
            iir_cfg[0].iir_params[j] = (uint16_t)strtol(p, &ps, 16);
            if (!(p = strtok(NULL, seps)))
                goto token_err;
        }
        rx_iir_flag[0] = (uint16_t)strtol(p, &ps, 16);
        if (!(p = strtok(NULL, seps)))
            goto token_err;
        iir_cfg[0].num_bands = (uint16_t)strtol(p, &ps, 16);

    } else if ( (buf[0] == 'B') && (buf[1] == '1') ) {
        LOGV("Found ADRC record");
        /* This is the ADRC record we are looking for.  Tokenize it */
        adrc_filter_exists[0] = true;
        if (!(p = strtok(buf, ",")))
            goto token_err;

        /* Table header */
        table_num = strtol(p + 1, &ps, 10);
        if (!(p = strtok(NULL, seps)))
            goto token_err;

        /* Table description */
        if (!(p = strtok(NULL, seps)))
            goto token_err;
        adrc_flag[0] = (uint16_t)strtol(p, &ps, 16);

        if (!(p = strtok(NULL, seps)))
            goto token_err;
        adrc_cfg[0].adrc_params[0] = (uint16_t)strtol(p, &ps, 16);

        if (!(p = strtok(NULL, seps)))
            goto token_err;
        adrc_cfg[0].adrc_params[1] = (uint16_t)strtol(p, &ps, 16);

        if (!(p = strtok(NULL, seps)))
            goto token_err;
        adrc_cfg[0].adrc_params[2] = (uint16_t)strtol(p, &ps, 16);

        if (!(p = strtok(NULL, seps)))
            goto token_err;
        adrc_cfg[0].adrc_params[3] = (uint16_t)strtol(p, &ps, 16);

        if (!(p = strtok(NULL, seps)))
            goto token_err;
        adrc_cfg[0].adrc_params[4] = (uint16_t)strtol(p, &ps, 16);

        if (!(p = strtok(NULL, seps)))
            goto token_err;
        adrc_cfg[0].adrc_params[5] = (uint16_t)strtol(p, &ps, 16);

        if (!(p = strtok(NULL, seps)))
            goto token_err;
        adrc_cfg[0].adrc_params[6] = (uint16_t)strtol(p, &ps, 16);

        if (!(p = strtok(NULL, seps)))
            goto token_err;
        adrc_cfg[0].adrc_params[7] = (uint16_t)strtol(p, &ps, 16);

        if (!(p = strtok(NULL, seps)))
            goto token_err;

    } else if ( (buf[0] == 'C') && (buf[1] == '1') ) {
        LOGV("Found EQ record");
        /* This is the EQ record we are looking for.  Tokenize it */
        if (!(p = strtok(buf, ",")))
            goto token_err;

        /* Table header */
        table_num = strtol(p + 1, &ps, 10);
        if (!(p = strtok(NULL, seps)))
            goto token_err;
        /* Table description */
        if (!(p = strtok(NULL, seps)))
            goto token_err;

        eq_flag[0] = (uint16_t)strtol(p, &ps, 16);
        if (!(p = strtok(NULL, seps)))
            goto token_err;
        LOGI("EQ flag = %02x.", eq_flag[0]);

        audioeq = dlopen("/system/lib/libaudioeq.so", RTLD_NOW);
        if (audioeq == NULL) {
            LOGE("audioeq library open failure");
            return -1;
        }
        eq_cal = (void *(*) (int32_t, int32_t, int32_t, uint16_t, int32_t, int32_t *, int32_t *, uint16_t *)) dlsym(audioeq, "audioeq_calccoefs");
        memset(&eqalizer[0], 0, sizeof(eqalizer));

        /* Temp add the bands here */
        eqalizer[0].bands = 8;
        for (i = 0; i < eqalizer[0].bands; i++) {

            eq[i].gain = (uint16_t)strtol(p, &ps, 16);

            if (!(p = strtok(NULL, seps)))
                goto token_err;
            eq[i].freq = (uint16_t)strtol(p, &ps, 16);

            if (!(p = strtok(NULL, seps)))
                goto token_err;
            eq[i].type = (uint16_t)strtol(p, &ps, 16);

            if (!(p = strtok(NULL, seps)))
                goto token_err;
            eq[i].qf = (uint16_t)strtol(p, &ps, 16);

            if (!(p = strtok(NULL, seps)))
                goto token_err;

            eq_cal(eq[i].gain, eq[i].freq, 48000, eq[i].type, eq[i].qf, (int32_t*)numerator, (int32_t *)denominator, shift);
            for (j = 0; j < 6; j++) {
                eqalizer[0].params[ ( i * 6) + j] = numerator[j];
            }
            for (j = 0; j < 4; j++) {
                eqalizer[0].params[(eqalizer[0].bands * 6) + (i * 4) + j] = denominator[j];
            }
            eqalizer[0].params[(eqalizer[0].bands * 10) + i] = shift[0];
        }
        dlclose(audioeq);

    }
    return 0;

token_err:
    LOGE("malformatted pcm control buffer");
    return -EINVAL;
}

static int get_audpp_filter(void)
{
    struct stat st;
    char *read_buf;
    char *next_str, *current_str;
    int csvfd;

    LOGI("get_audpp_filter");
    static const char *path =
            AUDIO_FILTER_CUSTOM_FILENAME;

    csvfd = open(path, O_RDONLY);
    if (csvfd < 0) {
        /* Failed to open custom parameters file, fallback to the default file ... */        
        LOGE("Failed to open AUDIO_NORMAL_FILTER %s. Error %s (%d)\n",
             path, strerror(errno), errno);

        LOGE("Trying with default file");
        path = AUDIO_FILTER_DEFAULT_FILENAME;
        csvfd = open(path, O_RDONLY);
        if (csvfd < 0) {
            LOGE("Failed to open %s. Error %s (%d)\n",
                 path, strerror(errno), errno);
            return -1;
        }
    }
    
    LOGE("Successfully opened %s\n", path);

    if (fstat(csvfd, &st) < 0) {
        LOGE("failed to stat %s: %s (%d).",
             path, strerror(errno), errno);
        close(csvfd);
        return -1;
    }

    read_buf = (char *) mmap(0, st.st_size,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE,
                    csvfd, 0);

    if (read_buf == MAP_FAILED) {
        LOGE("failed to mmap parameters file: %s (%d)",
             strerror(errno), errno);
        close(csvfd);
        return -1;
    }

    current_str = read_buf;

    while (1) {
        int len;
        next_str = strchr(current_str, '\n');
        if (!next_str)
           break;
        len = next_str - current_str;
        *next_str++ = '\0';
        if (check_and_set_audpp_parameters(current_str, len)) {
            LOGI("failed to set audpp parameters, exiting.");
            munmap(read_buf, st.st_size);
            close(csvfd);
            return -1;
        }
        current_str = next_str;
    }

    munmap(read_buf, st.st_size);
    close(csvfd);
    return 0;
}

/***********************************************************************************
 *
 *  Interfaces
 *
 ***********************************************************************************/
int htc_acoustic_init(void)
{
    int rc = 0;

    LOGV("Running custom built libhtc_acoustic.so");

    /* Open the acoustic driver */
    rc = openacousticfd();
    if ( rc < 0 ) {
        return rc;
    }

    /* Get device capabilities */
    rc = get_device_capabilities();
    if ( rc < 0 ) {
        return rc;
    }

    /* Read parameters from csv file */
    rc = ReadAudioParaFromFile();
    if ( rc < 0 ) {
        return rc;
    }

    /* Read filter tables */
    rc = get_audpp_filter();
    if ( rc == 0 ) {
        audpp_filter_inited = true;
    }
    rc = get_audpre_table();
    if ( rc == 0 ) {
        audpre_filter_inited = true;
    }
    /* TODO : AGC for TI A2026 from csv file ? 
     * Values are almost all the same, except for voice call.
     * Values hardcoded here, might be readed from file if required in the future
     */

    /* Retrieve available sound endpoints IDs from kernel */
    rc = get_sound_endpoints();
    
    return rc;
}

int snd_get_num_endpoints(void)
{
    return mNumSndEndpoints;
}

int snd_get_endpoint(int cnt, struct msm_snd_endpoint *p_ept)
{
    int icnt;
    struct msm_snd_endpoint *ept = mSndEndpoints;

    *p_ept = ept[cnt];

    return 0;
}

int htc_acoustic_deinit(void)
{
    int rc = 0;

    /* Close the acoustic driver */
    close(acousticfd);

    /* Close TPA2016 */
    if ( TPA2016fd >= 0 ) {
        close(TPA2016fd);
    }

    /* Free the memory */
    if ( Audio_Path_Table != NULL )
        free(Audio_Path_Table);
    if ( Audio_Path_Uplink_Table != NULL )
        free(Audio_Path_Uplink_Table);
    if ( Phone_Acoustic_Table != NULL )
        free(Phone_Acoustic_Table);
    if ( BT_Phone_Acoustic_Table != NULL )
        free(BT_Phone_Acoustic_Table);
    if ( HTC_VOC_CAL_CODEC_TABLE_Table != NULL )
        free(HTC_VOC_CAL_CODEC_TABLE_Table);
    if ( mSndEndpoints != NULL )
        free(mSndEndpoints);

    return rc;
}

// TODO : See how EQ and filters are derived from the initial settings
int msm72xx_enable_audpp(int enable_mask)
{
    int fd;
    int device_id=0;

    if (!audpp_filter_inited) return -EINVAL;

    fd = open(PCM_CTL_DEVICE, O_RDWR);
    if (fd < 0) {
        LOGE("Cannot open PCM Ctl device");
        return -EPERM;
    }

    if (adrc_filter_exists[device_id])
    {
        if (adrc_flag[device_id] == 0 && (enable_mask & ADRC_ENABLE))
            enable_mask &= ~ADRC_ENABLE;
        else if(enable_mask & ADRC_ENABLE)
        {
            LOGI("ADRC Filter ADRC FLAG = %02x.", adrc_flag[device_id]);
            LOGI("ADRC Filter COMP THRESHOLD = %02x.", adrc_cfg[device_id].adrc_params[0]);
            LOGI("ADRC Filter COMP SLOPE = %02x.", adrc_cfg[device_id].adrc_params[1]);
            LOGI("ADRC Filter COMP RMS TIME = %02x.", adrc_cfg[device_id].adrc_params[2]);
            LOGI("ADRC Filter COMP ATTACK[0] = %02x.", adrc_cfg[device_id].adrc_params[3]);
            LOGI("ADRC Filter COMP ATTACK[1] = %02x.", adrc_cfg[device_id].adrc_params[4]);
            LOGI("ADRC Filter COMP RELEASE[0] = %02x.", adrc_cfg[device_id].adrc_params[5]);
            LOGI("ADRC Filter COMP RELEASE[1] = %02x.", adrc_cfg[device_id].adrc_params[6]);
            LOGI("ADRC Filter COMP DELAY = %02x.", adrc_cfg[device_id].adrc_params[7]);
            if (ioctl(fd, AUDIO_SET_ADRC, &adrc_cfg[device_id]) < 0)
            {
                LOGE("set adrc filter error.");
            }
        }
    }

    if (eq_flag[device_id] == 0 && (enable_mask & EQ_ENABLE))
        enable_mask &= ~EQ_ENABLE;
    else if (enable_mask & EQ_ENABLE)
    {
	    LOGI("Setting EQ Filter");
        if (ioctl(fd, AUDIO_SET_EQ, &eqalizer[device_id]) < 0) {
            LOGE("set Equalizer error.");
        }
    }

    if (rx_iir_flag[device_id] == 0 && (enable_mask & RX_IIR_ENABLE))
        enable_mask &= ~RX_IIR_ENABLE;
    else if (enable_mask & RX_IIR_ENABLE)
    {
        LOGI("IIR Filter FLAG = %02x.", rx_iir_flag[device_id]);
        LOGI("IIR NUMBER OF BANDS = %02x.", iir_cfg[device_id].num_bands);
        LOGI("IIR Filter N1 = %02x.", iir_cfg[device_id].iir_params[0]);
        LOGI("IIR Filter N2 = %02x.",  iir_cfg[device_id].iir_params[1]);
        LOGI("IIR Filter N3 = %02x.",  iir_cfg[device_id].iir_params[2]);
        LOGI("IIR Filter N4 = %02x.",  iir_cfg[device_id].iir_params[3]);
        LOGI("IIR FILTER M1 = %02x.",  iir_cfg[device_id].iir_params[24]);
        LOGI("IIR FILTER M2 = %02x.", iir_cfg[device_id].iir_params[25]);
        LOGI("IIR FILTER M3 = %02x.",  iir_cfg[device_id].iir_params[26]);
        LOGI("IIR FILTER M4 = %02x.",  iir_cfg[device_id].iir_params[27]);
        LOGI("IIR FILTER M16 = %02x.",  iir_cfg[device_id].iir_params[39]);
        LOGI("IIR FILTER SF1 = %02x.",  iir_cfg[device_id].iir_params[40]);
        if (ioctl(fd, AUDIO_SET_RX_IIR, &iir_cfg[device_id]) < 0)
        {
            LOGE("set rx iir filter error.");
        }
    }

    LOGE("msm72xx_enable_audpp: 0x%04x", enable_mask);
    if (ioctl(fd, AUDIO_ENABLE_AUDPP, &enable_mask) < 0) {
        LOGE("enable audpp error");
        close(fd);
        return -EPERM;
    }

    close(fd);
    return 0;
}


int msm72xx_set_audpre_params(int audpre_index, int tx_iir_index)
{
    if (audpre_filter_inited)
    {
        int fd;

        fd = open(PREPROC_CTL_DEVICE, O_RDWR);
        if (fd < 0) {
             LOGE("Cannot open PreProc Ctl device");
             return -EPERM;
        }

        if ( audpre_tx_agc_cfg_exist ) {
             /* Setting AGC Params */
            LOGI("AGC Filter Param4= %02x.", tx_agc_cfg[audpre_index].static_gain);
            LOGI("AGC Filter Param5= %02x.", tx_agc_cfg[audpre_index].adaptive_gain_flag);
            LOGI("AGC Filter Param6= %02x.", tx_agc_cfg[audpre_index].agc_params[0]);
            LOGI("AGC Filter Param7= %02x.", tx_agc_cfg[audpre_index].agc_params[16]);
            if (ioctl(fd, AUDIO_SET_AGC, &tx_agc_cfg[audpre_index]) < 0)
            {
                LOGE("set AGC filter error.");
            }
        }

        if ( audpre_ns_cfg_exist ) {
             /* Setting NS Params */
            LOGI("NS Filter Param3= %02x.", ns_cfg[audpre_index].dens_gamma_n);
            LOGI("NS Filter Param4= %02x.", ns_cfg[audpre_index].dens_nfe_block_size);
            LOGI("NS Filter Param5= %02x.", ns_cfg[audpre_index].dens_limit_ns);
            LOGI("NS Filter Param6= %02x.", ns_cfg[audpre_index].dens_limit_ns_d);
            LOGI("NS Filter Param7= %02x.", ns_cfg[audpre_index].wb_gamma_e);
            LOGI("NS Filter Param8= %02x.", ns_cfg[audpre_index].wb_gamma_n);
            if (ioctl(fd, AUDIO_SET_NS, &ns_cfg[audpre_index]) < 0)
            {
                LOGE("set NS filter error.");
            }
        }

        /* Setting TX_IIR Params */
        LOGI("TX_IIR Filter Param2= %02x.", tx_iir_cfg[audpre_index].active_flag);
        LOGI("TX_IIR Filter Param3= %02x.", tx_iir_cfg[audpre_index].num_bands);
        LOGI("TX_IIR Filter Param4= %02x.", tx_iir_cfg[audpre_index].iir_params[0]);
        LOGI("TX_IIR Filter Param5= %02x.", tx_iir_cfg[audpre_index].iir_params[1]);
        LOGI("TX_IIR Filter Param6 %02x.", tx_iir_cfg[audpre_index].iir_params[47]);
        if (ioctl(fd, AUDIO_SET_TX_IIR, &tx_iir_cfg[audpre_index]) < 0)
        {
           LOGE("set TX IIR filter error.");
        }

	    close(fd);
        return 0;
    }

    return -1;
}

int msm72xx_enable_audpre(int acoustic_flags, int audpre_index, int tx_iir_index)
{
    int fd;

    fd = open(PREPROC_CTL_DEVICE, O_RDWR);
    if (fd < 0) {
         LOGE("Cannot open PreProc Ctl device");
         return -EPERM;
    }

    /* Remove the unused flags for device that don't have correct config parameters
     * Flags values are token from enum audio_in_acoustics in AudioSystem.h
     */
    if ( !audpre_ns_cfg_exist ) {
        acoustic_flags &= ~0x02;
    }
    if ( !audpre_tx_agc_cfg_exist ) {
        acoustic_flags &= ~0x01;
    }

    /*Setting AUDPRE_ENABLE*/
    LOGE("msm72xx_enable_audpre: 0x%04x", acoustic_flags);
    if (ioctl(fd, AUDIO_ENABLE_AUDPRE, &acoustic_flags) < 0)
    {
       LOGE("set AUDPRE_ENABLE error.");
    }
	close(fd);

    return 0;
}

int msm72xx_update_audio_method(int method)
{
    LOGV("msm72xx_update_audio_method %d", method);

    struct audio_update_req req = {.type = ADIE_UPDATE_AUDIO_METHOD, .value = method};
    if ( ioctl(acousticfd, ACOUSTIC_UPDATE_AUDIO_SETTINGS, &req) < 0) {
        LOGE("ACOUSTIC_UPDATE_AUDIO_SETTINGS error.");
        return -EIO;
    }  
    return 0;
}

#define SND_METHOD_AUDIO 1
#define SND_METHOD_NONE  -1

int msm72xx_set_acoustic_table(int device, int volume)
{
    struct fg_table_s* table = NULL;
    struct c_table_s*  ce_table = NULL;
    int out_path = device;
    int out_path_method = SND_METHOD_VOICE;

    LOGV("msm72xx_set_acoustic_table %d %d", device, volume);

    if ( (mCurrentVolume == volume) && 
         ((mCurrentSndDevice == device) || (device == SND_DEVICE_CURRENT)) ) {
        LOGV("Update not required");
        return 0;
    }

    if ( volume > 5 ) {
        return -EIO;
    }

    if ( device == SND_DEVICE_CURRENT ) {
       device = mCurrentSndDevice;
       LOGV("Use current device %d", device);
    }

    if( device == SND_DEVICE_HANDSET ) {
        LOGV("Acoustic profile : EARCUPLE");
        out_path = EARCUPLE;
    } else if( (device == SND_DEVICE_SPEAKER) || (device == SND_DEVICE_SPEAKER_MIC) ) {
        LOGV("Acoustic profile : HANDSFREE");
        out_path = HANDSFREE;
    } else if( device == SND_DEVICE_HEADSET ) {
        LOGV("Acoustic profile : HEADSET");
        out_path = HEADSET;
    } else if( (device == SND_DEVICE_BT) || (device == SND_DEVICE_BT_EC_OFF) ) {
        LOGV("Acoustic profile : BTHEADSET");
        out_path = BTHEADSET;
    } else if( device == SND_DEVICE_CARKIT ) {
        LOGV("Acoustic profile : CARKIT");
        out_path = CARKIT;
    } else if( device == SND_DEVICE_TTY_FULL ) {
        LOGV("Acoustic profile : TTY_FULL");
        out_path = TTY_FULL;
    } else if( device == SND_DEVICE_TTY_VCO ) {
        LOGV("Acoustic profile : TTY_VCO");
        out_path = TTY_VCO;
    } else if( device == SND_DEVICE_TTY_HCO ) {
        LOGV("Acoustic profile : TTY_HCO");
        out_path = TTY_HCO;
    } else if( device == SND_DEVICE_PLAYBACK_HEADSET ) {
        LOGV("Acoustic profile : PLAYBACK_HEADSET");
        out_path = PLAYBACK_HEADSET;
    } else if( device == SND_DEVICE_PLAYBACK_HANDSFREE ) {
        LOGV("Acoustic profile : PLAYBACK_HANDSFREE");
        out_path = PLAYBACK_HANDSFREE;
    } else if( device == SND_DEVICE_REC_INC_MIC ) {
        LOGV("Acoustic profile : REC_INC_MIC");
        out_path = REC_INC_MIC;
    } else if( device == SND_DEVICE_IDLE ) {
        LOGV("Acoustic profile : EARCUPLE");
        out_path = EARCUPLE;
    } else if ( device >= BT_CUSTOM_DEVICES_ID_OFFSET ) {
        LOGV("Acoustic profile : CUSTOM_BTHEADSET");
        out_path = CUSTOM_BTHEADSET;
    }

    // TODO : See UpdateVolumeTable from CE for device = 3
    switch ( out_path )
    {
        case HEADSET:
        case HANDSFREE:
        case EARCUPLE:
            table = &Phone_Acoustic_Table[(out_path*6) + volume];
        break;

        case BTHEADSET:
            table = &Phone_Acoustic_Table[18];
        break;

        case CUSTOM_BTHEADSET:
            table = &BT_Phone_Acoustic_Table[device - BT_CUSTOM_DEVICES_ID_OFFSET];
            out_path = BTHEADSET;
        break;

        case CARKIT:
            table = &Phone_Acoustic_Table[19];
        break;

        case TTY_FULL:
            table = &Phone_Acoustic_Table[20];
            out_path_method = SND_METHOD_NONE;
        break;

        case TTY_VCO:
            table = &Phone_Acoustic_Table[21];
            out_path_method = SND_METHOD_NONE;
        break;

        case TTY_HCO:
            table = &Phone_Acoustic_Table[22];
            out_path_method = SND_METHOD_NONE;
        break;

        case REC_INC_MIC:
            table = &Phone_Acoustic_Table[23];
            out_path_method = SND_METHOD_AUDIO;
        break;

        case REC_EXT_MIC:
            table = &Phone_Acoustic_Table[24];
            out_path_method = SND_METHOD_AUDIO;
        break;

        case PLAYBACK_HEADSET:
            table = &Phone_Acoustic_Table[25];
            out_path_method = SND_METHOD_AUDIO;
        break;

        case PLAYBACK_HANDSFREE:
            table = &Phone_Acoustic_Table[26];
            out_path_method = SND_METHOD_AUDIO;
        break;

        default:
            LOGE("Unknown out_path");
        break;
    }

    if ( table ) {
        if (ioctl(acousticfd, ACOUSTIC_UPDATE_VOLUME_TABLE, &(table->array) ) < 0) {
            LOGE("ACOUSTIC_UPDATE_VOLUME_TABLE error.");
            return -EIO;
        }

        /* TODO : Look at UpdateCeTable from CE dll
         * TODO : Is it really usefull as the table for all devices is filled with 0's
         */
        if ( out_path < SYS ) {
            ce_table = &CE_Acoustic_Table[out_path];
            if (ioctl(acousticfd, ACOUSTIC_UPDATE_CE_TABLE, &(ce_table->array) ) < 0) {
                LOGE("ACOUSTIC_UPDATE_CE_TABLE error.");
                return -EIO;
            }
        }

        /* Set TPA2016 specific parameters if existing */
        if ( (TPA2016fd >= 0) &&
                ((out_path == HANDSFREE) || (out_path == PLAYBACK_HANDSFREE)) ) {
            if ( out_path_method == SND_METHOD_NONE ) {
                goto exit;
            } else if ( out_path_method == SND_METHOD_AUDIO ) {
                /* Set default audio settings */
                memcpy(tpa2016d2_regs, tpa2016d2_regs_audio, 7);
            } else if ( out_path_method == SND_METHOD_VOICE ) {
                /* Set default voice settings */
                memcpy(tpa2016d2_regs, tpa2016d2_regs_voice, 7);
            }
            /* Set volume */
            tpa2016d2_regs[FIXED_GAIN_REG-1] = volume * 6;
            if (ioctl(TPA2016fd, TPA2016_SET_CONFIG, &tpa2016d2_regs ) < 0) {
                LOGE("TPA2016_SET_CONFIG error.");
                return -EIO;
            }  
        }  
#if 0
        if ( (out_path_method == SND_METHOD_VOICE) ||
                (out_path_method == SND_METHOD_AUDIO) ) {
            msm72xx_update_audio_method(out_path_method);
        }
#endif
exit:
        mCurrentSndDevice = device;
    }

    mCurrentVolume = volume;

    return out_path_method;
}

int msm72xx_start_acoustic_setting(void)
{
    LOGV("msm72xx_start_acoustic_setting");
    struct audio_update_req req = {.type = ADIE_FORCE_ADIE_UPDATE_REQ, .value = 1};
    if ( ioctl(acousticfd, ACOUSTIC_UPDATE_AUDIO_SETTINGS, &req) < 0) {
        LOGE("ACOUSTIC_UPDATE_AUDIO_SETTINGS error.");
        return -EIO;
    }  
    return 0;
}

int msm72xx_set_acoustic_done(void)
{
    LOGV("msm72xx_set_acoustic_done");
    if (ioctl(acousticfd, ACOUSTIC_ARM11_DONE, NULL ) < 0) {
        LOGE("ACOUSTIC_ARM11_DONE error.");
        return -EIO;
    }

    struct audio_update_req req = {.type = ADIE_FORCE_ADIE_UPDATE_REQ, .value = 0};
    if ( ioctl(acousticfd, ACOUSTIC_UPDATE_AUDIO_SETTINGS, &req) < 0) {
        LOGE("ACOUSTIC_UPDATE_AUDIO_SETTINGS error.");
        return -EIO;
    }  

    return 0;
}

int msm72xx_set_audio_path(bool bEnableMic, bool bEnableDualMic,
                           int device_out, bool bEnableOut)
{
    struct msm_audio_path audio_path = {.bEnableMic = bEnableMic, .bEnableDualMic = bEnableDualMic};

    if ( bEnableOut ) {
        if ( (device_out == SND_DEVICE_SPEAKER) || (device_out == SND_DEVICE_PLAYBACK_HANDSFREE) ) {
            audio_path.bEnableSpeaker = true;
        } else {
            audio_path.bEnableSpeaker = false;
        }

        if ( (device_out == SND_DEVICE_HEADSET) || (device_out == SND_DEVICE_PLAYBACK_HEADSET) ) {
            audio_path.bEnableHeadset = true;
        } else {
            audio_path.bEnableHeadset = false;
        }
    } else {
        audio_path.bEnableSpeaker = false;
        audio_path.bEnableHeadset = false;
    }

    LOGV("msm72xx_set_audio_path: device=%d Mic = %d, DualMic = %d, Speaker = %d, Headset = %d",
            device_out,
			bEnableMic, bEnableDualMic, audio_path.bEnableSpeaker, audio_path.bEnableHeadset);

    UpdateAudioAdieTable(bEnableMic, 0, 0, 0, false);
    
    if (ioctl(acousticfd, ACOUSTIC_SET_HW_AUDIO_PATH, &audio_path ) < 0) {
        LOGE("ACOUSTIC_SET_HW_AUDIO_PATH error.");
        return -EIO;
    } 

    if ( TPA2016fd >= 0 ) {
        if (ioctl(TPA2016fd, TPA2016_SET_POWER, &audio_path.bEnableSpeaker ) < 0) {
            LOGE("TPA2016_SET_POWER error.");
            return -EIO;
        } 
        
        if ( bEnableOut ) {
            /* Enable both outputs */
            tpa2016d2_regs[IC_REG-1] |= (SPK_EN_L | SPK_EN_R);
            if (ioctl(TPA2016fd, TPA2016_SET_CONFIG, &tpa2016d2_regs ) < 0) {
                LOGE("TPA2016_SET_CONFIG error.");
                return -EIO;
            }        
        }
    }

    return 0;
}

int msm72xx_get_bluetooth_hs_id(const char* BT_Name)
{
    int i;
    
    for (i = 0; i < BTPAT_max_index; i++) {
        if (!strcasecmp(BT_Name, BT_Phone_Acoustic_Table[i].table.name)) {
            LOGI("Found custom acoustic parameters for %s", BT_Name);
            break;
        }
    }

    if (i == BTPAT_max_index) {
        LOGI("Couldn't find custom acoustic parameters for %s, using default", BT_Name);
        i = 0;
    }

    return i + BT_CUSTOM_DEVICES_ID_OFFSET;
}


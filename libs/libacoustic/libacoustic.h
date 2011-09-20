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

#ifndef ANDROID_HARDWARE_LIB_HTC_ACOUSTIC_H
#define ANDROID_HARDWARE_LIB_HTC_ACOUSTIC_H

#include <utils/threads.h>
#include <stdint.h>

#define MSM_HTC_ACOUSTIC_WINCE "/dev/htc_acoustic_wince"
#define MSM_TPA2016D2_DEV      "/dev/tpa2016d2"

#define MAX_MODE_NAME_LENGTH 32

struct msm_acoustic_capabilities {
    char htc_voc_cal_fields_per_param;    /* Specifies the number of fields per parameter */
    bool bDualMicSupported;
    /* TODO : keep up-to-date with new fields in kernel */
};

struct msm_audio_path {
    bool bEnableMic;
    bool bEnableDualMic;
    bool bEnableSpeaker;
    bool bEnableHeadset;
};

struct adie_table {
    int table_num;
    char* pcArray;
};

struct htc_voc_cal_table {
    uint16_t* pArray;
    int       size;
};

enum {
    PCOM_UPDATE_REQ = 0,
    ADIE_FORCE8K_REQ,
    ADIE_FORCE_ADIE_AWAKE_REQ,
    ADIE_FORCE_ADIE_UPDATE_REQ,
    ADIE_UPDATE_AUDIO_METHOD,
    
} AUDIO_UPDATE_REQ_TYPE;

struct audio_update_req {
    int type;       /* one of the AUDIO_UPDATE_REQ_TYPE */
    int value;      /* For PCOM_UPDATE, dex data. For ADIE updates, value of the setting */
};

/* Acoustic IOCTLs */
#define ACOUSTIC_IOCTL_MAGIC 'p'
#define ACOUSTIC_ARM11_DONE	                    _IOW(ACOUSTIC_IOCTL_MAGIC, 22, unsigned int)

#define ACOUSTIC_UPDATE_ADIE_TABLE              _IOW(ACOUSTIC_IOCTL_MAGIC,  1, struct adie_table* )
#define ACOUSTIC_UPDATE_VOLUME_TABLE            _IOW(ACOUSTIC_IOCTL_MAGIC,  2, uint16_t* )
#define ACOUSTIC_UPDATE_CE_TABLE                _IOW(ACOUSTIC_IOCTL_MAGIC,  3, uint16_t* )
#define ACOUSTIC_UPDATE_AUDIO_PATH_TABLE        _IOW(ACOUSTIC_IOCTL_MAGIC,  4, uint16_t* )
#define ACOUSTIC_UPDATE_AUDIO_SETTINGS          _IOW(ACOUSTIC_IOCTL_MAGIC,  5, struct audio_update_req* )
#define ACOUSTIC_UPDATE_HTC_VOC_CAL_CODEC_TABLE _IOW(ACOUSTIC_IOCTL_MAGIC,  6, struct htc_voc_cal_table* )
#define ACOUSTIC_GET_CAPABILITIES               _IOW(ACOUSTIC_IOCTL_MAGIC,  8, struct msm_acoustic_capabilities* )

#define ACOUSTIC_SET_HW_AUDIO_PATH          _IOW(ACOUSTIC_IOCTL_MAGIC,  10, struct msm_audio_path* )

/* TPA2016 IOCTLs */
#define TPA2016_IOCTL_MAGIC 'a'
#define TPA2016_SET_POWER                       _IOW(TPA2016_IOCTL_MAGIC, 0x01,	unsigned)
#define TPA2016_SET_CONFIG                      _IOW(TPA2016_IOCTL_MAGIC, 0x02,	unsigned char*)
#define TPA2016_READ_CONFIG                     _IOW(TPA2016_IOCTL_MAGIC, 0x03, unsigned char*)

/* TPA2016 registers */
#define IC_REG          0x1
#define ATK_REG         0x2
#define REL_REG         0x3
#define HOLD_REG        0x4
#define FIXED_GAIN_REG	0x5
#define AGC_REG1        0x6
#define AGC_REG2        0x7


/* Registers map
 *
 *  Address | Name                  | Bit 7      | Bit 6      | Bit 5      | Bit 4      | Bit 3      | Bit 2      | Bit 1      | Bit 0      |
 *  --------+-----------------------+------------+------------+------------+------------+------------+------------+------------+------------+
 *  0x1     | IC FUNCTION CONTROL   | SPKR_EN_R  | SPKR_EN_L  | SWS        | FAULT_R    | FAULT_L    | Thermal    | UNUSED     | NG_EN      |
 *  0x2     | AGC ATTACK CONTROL    | Unused     | Unused     | ATK_time[5]| ATK_time[4]| ATK_time[3]| ATK_time[2]| ATK_time[1]| ATK_time[0]|
 *  0x3     | AGC RELEASE CONTROL   | Unused     | Unused     | REL_time[5]| REL_time[4]| REL_time[3]| REL_time[2]| REL_time[1]| REL_time[0]|
 *  0x4     | AGC HOLD TIME CONTROL | Unused     | Unused     | HLD_time[5]| HLD_time[4]| HLD_time[3]| HLD_time[2]| HLD_time[1]| HLD_time[0]|
 *  0x5     | AGC FIXED GAIN CONTROL| Unused     | Unused     | Fix_gain[5]| Fix_gain[4]| Fix_gain[3]| Fix_gain[2]| Fix_gain[1]| Fix_gain[0]|
 *  0x6     | AGC CONTROL 1         | Out Lim Dis| NoisGateTh1| NoisGateTh0| Out limlev4| Out limlev3| Out limlev2| Out limlev1| Out limlev0|
 *  0x7     | AGC CONTROL 2         | Max Gain[3]| Max Gain[2]| Max Gain[1]| Max Gain[0]| Unused     | Unused     | CompRatio1 | CompRatio0 |
 *
 */

#define SPK_EN_L                    1<<6
#define SPK_EN_R                    1<<7
#define SWS_BIT                     0x40
#define NOISE_GATE_ENABLE_BIT       0x01


#define AGC_ATTACK_BITS             0x3F
#define AGC_RELEASE_BITS            0x3F
#define AGC_HOLDTIME_BITS           0x3F
#define AGC_FIXEDGAIN_BITS          0x3F
/* AGC CONTROL 1 */
#define OUT_LIMITER_DISABLE         0x80
#define NOISE_GATE_THRESOLD_BITS    0x60
#define OUT_LIMITER_LEVEL_BITS      0x1F
/* AGC CONTROL 2 */
#define MAX_GAIN_BITS               0xF0
#define COMPRESSION_RATIO_BITS      0x03




#define TRUE 1
#define FALSE 0

#define SAMP_RATE_INDX_8000	0
#define SAMP_RATE_INDX_11025	1
#define SAMP_RATE_INDX_12000	2
#define SAMP_RATE_INDX_16000	3
#define SAMP_RATE_INDX_22050	4
#define SAMP_RATE_INDX_24000	5
#define SAMP_RATE_INDX_32000	6
#define SAMP_RATE_INDX_44100	7
#define SAMP_RATE_INDX_48000	8

#define AUDIO_HW_IN_SAMPLERATE  8000                 // Default audio input sample rate

/* enable_mask bits */
#define ADRC_ENABLE  0x0001
#define ADRC_DISABLE 0x0000
#define EQ_ENABLE    0x0002
#define EQ_DISABLE   0x0000
#define RX_IIR_ENABLE   0x0004
#define RX_IIR_DISABLE  0x0000

#define AGC_ENABLE     0x0001
#define NS_ENABLE      0x0002
#define TX_IIR_ENABLE  0x0004

#if __cplusplus
extern "C" {
#endif

#include <linux/msm_audio.h>

struct eq_filter_type {
    int16_t gain;
    uint16_t freq;
    uint16_t type;
    uint16_t qf;
};

struct eqalizer {
    uint16_t bands;
    uint16_t params[132];
};

struct rx_iir_filter {
    uint16_t num_bands;
    uint16_t iir_params[48];
};

struct tx_iir {
    uint16_t num_bands;
    uint16_t iir_params[48];
    uint16_t active_flag;
};

struct adrc_filter {
	uint16_t adrc_params[8];
};

struct ns {
        uint16_t  dens_gamma_n;
        uint16_t  dens_nfe_block_size;
        uint16_t  dens_limit_ns;
        uint16_t  dens_limit_ns_d;
        uint16_t  wb_gamma_e;
        uint16_t  wb_gamma_n;
};

struct tx_agc {
        uint16_t  static_gain;
        int16_t   adaptive_gain_flag;
        uint16_t  agc_params[17];
};

struct adrc_config {
    uint16_t adrc_band_params[10];
};

struct adrc_ext_buf {
    int16_t buff[196];
};

struct mbadrc_filter {
    uint16_t num_bands;
    uint16_t down_samp_level;
    uint16_t adrc_delay;
    uint16_t ext_buf_size;
    uint16_t ext_partition;
    uint16_t ext_buf_msw;
    uint16_t ext_buf_lsw;
    struct adrc_config adrc_band[5];
    struct adrc_ext_buf  ext_buf;
};

enum CE_audio_devices {
    HEADSET = 0,
    HANDSFREE,
    EARCUPLE,
    BTHEADSET,
    CARKIT,
    TTY_FULL,   
    TTY_VCO,
    TTY_HCO,
    REC_INC_MIC,
    REC_EXT_MIC,
    PLAYBACK_HEADSET,
    PLAYBACK_HANDSFREE,
    CUSTOM_BTHEADSET,
    SYS
};

/* CSV lines desc */
struct header_s {
    uint8_t Header;
    char Mode[MAX_MODE_NAME_LENGTH];
};

struct register_table_s {
    uint8_t Address;
    uint8_t Reg;
};

struct fg_table_st {
    char     name[MAX_MODE_NAME_LENGTH];
    uint16_t volume_level;
    uint16_t codecTxGain;
    uint16_t codecRxGain;
    uint16_t codecSTGain;
    uint16_t txVolume;
    uint16_t rxVolume;
    uint16_t rxAgcEnableFlag;
    uint16_t compFlinkStaticGain;
    uint16_t compFlinkAIGFlag;
    uint16_t expFlinkThreshold;
    uint16_t expFlinkSlope;
    uint16_t compFlinkThreshold;
    uint16_t compFlinkSlope;
    uint16_t comFlinkRmsTav;
    uint16_t compFlinkReleaseK;
    uint16_t compFlinkAIGMin;
    uint16_t compFlinkAIGMax;
    uint16_t rxAvcEnableFlag;
    uint16_t avcRlinkSensitivityOffset;
    uint16_t avcFlinkHeadroom;
    uint16_t txAgcEnableFlag;
    uint16_t compRlinkStaticGain;
    uint16_t compRlinkAIGFlag;
    uint16_t expRlinkThreshold;
    uint16_t expRlinkSlope;
    uint16_t compRlinkThreshold;
    uint16_t compRlinkSlope;
    uint16_t comRlinkRmsTav;
    uint16_t compRlinkReleaseK;
    uint16_t compRlinkAIGMin;
    uint16_t compRlinkAIGMax;
    uint16_t NLPP_limit;
    uint16_t NLPP_gain;
    uint16_t AF_limit;
    uint16_t HS_mode;
    uint16_t Tuning_mode;
    uint16_t echo_path_delay;
    uint16_t OutputGain;
    uint16_t InputGain;
    uint16_t AF_twoalpha;
    uint16_t AF_erl;
    uint16_t AF_taps;
    uint16_t AF_present_coefs;
    uint16_t AF_offset;
    uint16_t AF_erl_bg;
    uint16_t AF_taps_bg;
    uint16_t PCD_threshold;
    uint16_t minimum_erl;
    uint16_t erl_step;
    uint16_t max_noise_floor;
    uint16_t Det_threshold;
    uint16_t SPDET_Far;
    uint16_t SPDET_mic;
    uint16_t SPDET_xclip;
    uint16_t DENS_tail_alpha;
    uint16_t DENS_tail_portion;
    uint16_t DENS_gamma_e_alpha;
    uint16_t DENS_gamma_e_dt;
    uint16_t DENS_gamma_e_low;
    uint16_t DENS_gamma_e_rescue;
    uint16_t DENS_gamma_e_high;
    uint16_t DENS_spdet_near;
    uint16_t DENS_spdet_act;
    uint16_t DENS_gamma_n;
    uint16_t DENS_NFE_blocksize;
    uint16_t DENS_limit_NS;
    uint16_t DENS_NL_atten;
    uint16_t DENS_CNI_Level;
    uint16_t WB_echo_ratio;
    uint16_t rxPcmFiltEnableFlag;
    uint16_t rxPcmFiltCoeff[7];
    uint16_t txPcmFiltEnableFlag;
    uint16_t txPcmFiltCoeff[7];
    uint16_t rxiirFiltNumCoeff[18];
    uint16_t rxiirFiltDenCoeff[12];
    uint16_t rxiirFiltNumShiftFactor[4];
    uint16_t txiirFiltNumCoeff[18];
    uint16_t txiirFiltDenCoeff[12];
    uint16_t txiirFiltNumShiftFactor[4];
    uint16_t ecparameterupdated;
};

struct d_table_st {
    uint16_t routing_mode_config;
    uint16_t internal_codec_config;
    uint16_t external_codec_config;
    uint16_t pcm_ctrl;
    uint16_t codec_intf_ctrl;
    uint16_t dma_path_ctrl;
    uint16_t eight_khz_int_mode;
    uint16_t rx_codec_stereo_config;
    uint16_t tx_codec_stereo_config;
    uint16_t ECNS;
    uint16_t unk0;
};

struct be_table_st {
    uint16_t Operator_ID;
    uint16_t Vocpath;
    uint16_t volume_level;
    uint16_t codecTxGain;
    uint16_t codecRxGain;
    uint16_t codecSTGain;
    uint16_t txVolume;
    uint16_t rxVolume;
    uint16_t pcmFormatCtrl;
    uint16_t ecSwitch;
    uint16_t ecMode;
    uint16_t ecStartupMuteHangoverThres;
    uint16_t ecFarendHangoverThres;
    uint16_t esecDoubletalkHangoverThres;
    uint16_t hecDoubletalkHangoverThres;
    uint16_t aecDoubletalkHangoverThres;
    uint16_t ecStartupMuteMode;
    uint16_t ecMuteOverride;
    uint16_t ecStartupErleThres;
    uint16_t ecForceHalfDuplex;
    uint16_t esecResetThres;
    uint16_t hecResetThres;
    uint16_t aecResetThres;
    uint16_t ecInputSampOffset;
    uint16_t rxAgcEnableFlag;
    uint16_t compFlinkStaticGain;
    uint16_t compFlinkAIGFlag;
    uint16_t expFlinkThreshold;
    uint16_t expFlinkSlope;
    uint16_t compFlinkThreshold;
    uint16_t compFlinkSlope;
    uint16_t rxAvcEnableFlag;
    uint16_t avcRlinkSensitivityOffset;
    uint16_t avcFlinkHeadroom;
    uint16_t txAgcEnableFlag;
    uint16_t compRlinkStaticGain;
    uint16_t compRlinkAIGFlag;
    uint16_t expRlinkThreshold;
    uint16_t expRlinkSlope;
    uint16_t compRlinkThreshold;
    uint16_t compRlinkSlope;
    uint16_t nsSwitch;
    uint16_t nsMinGain;
    uint16_t nsSlope;
    uint16_t nsSNRThreshold;
    uint16_t rxPcmFiltCoeff[6];
    uint16_t txPcmFiltCoeff[6];
    uint16_t ec_reset_flag;
    uint16_t Reserved[5];
    uint16_t MCC_Val;
    uint16_t MNC_Val; 
};

struct au_table_st {    
    struct register_table_s register_table[26]; /* Add 1 .. Add 26 */   
    uint8_t Total_number;
    uint8_t Delay_number;
    uint8_t Delay_time;
    uint8_t extra[12];
};

/***********************************************************************************
 *
 *  Interfaces
 *
 ***********************************************************************************/
int htc_acoustic_init(void);
int htc_acoustic_deinit(void);

int msm72xx_enable_audpp(int enable_mask);
int msm72xx_set_audpre_params(int audpre_index, int tx_iir_index);
int msm72xx_enable_audpre(int acoustic_flags, int audpre_index, int tx_iir_index);
int snd_get_num_endpoints(void);
int snd_get_endpoint(int, struct msm_snd_endpoint *);

int msm72xx_start_acoustic_setting(void);
int msm72xx_set_acoustic_table(int device, int volume);
int msm72xx_set_acoustic_done(void);
int msm72xx_set_audio_path(bool bEnableMic, bool bEnableDualMic,
                           int device_out, bool bEnableOut);
int msm72xx_update_audio_method(int method);
int msm72xx_get_bluetooth_hs_id(const char* BT_Name);

#if __cplusplus
} // extern "C"
#endif

#endif

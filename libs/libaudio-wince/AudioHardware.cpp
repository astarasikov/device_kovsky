/*
** Copyright 2008, The Android Open-Source Project
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

#include <math.h>

//#define LOG_NDEBUG 0
#define LOG_TAG "AudioHardwareMSM72XX_wince"
#include <utils/Log.h>
#include <utils/String8.h>

#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <fcntl.h>

// hardware specific functions

#include "AudioHardware.h"
#include <media/AudioRecord.h>

extern "C" {
#include "a1010.h"

#define AUDIENCE_A1010_DEV      "/dev/audience_A1010"
}

#define LOG_SND_RPC 0  // Set to 1 to log sound RPC's

namespace android {
static int audpre_index, tx_iir_index;
static void * acoustic;
const uint32_t AudioHardware::inputSamplingRates[] = {
        8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000
};

/****************************************************************************
 * Function prototypes for external libhtc-acoustic
 ****************************************************************************/
int (*htc_acoustic_init)(void);
int (*htc_acoustic_deinit)(void);
int (*msm72xx_set_acoustic_table)(int device, int volume);
int (*msm72xx_start_acoustic_setting)(void);
int (*msm72xx_set_acoustic_done)(void);
int (*msm72xx_set_audio_path)(bool bEnableMic, bool bEnableDualMic, int device_out, bool bEnableOut);
int (*msm72xx_update_audio_method)(int method);
int (*msm72xx_get_bluetooth_hs_id)(const char* BT_Name);

/****************************************************************************
 * Local Function prototypes
 ****************************************************************************/
static int set_initial_audio_volume(void);
static int get_master_volume(void);
static int get_current_stream(void);

/****************************************************************************
 * Sound devices ids
 ****************************************************************************/
/* Default device for backward compatibility if libhtc_acoustic is not found */
static int SND_DEVICE_CURRENT=256;
static int SND_DEVICE_HANDSET=0;
static int SND_DEVICE_SPEAKER=1;
static int SND_DEVICE_SPEAKER_MIC=1;
static int SND_DEVICE_HEADSET=2;

static int SND_DEVICE_BT=-1;
static int SND_DEVICE_BT_EC_OFF=-1;
static int SND_DEVICE_HEADSET_AND_SPEAKER=-1;
static int SND_DEVICE_IN_S_SADC_OUT_HANDSET=-1;
static int SND_DEVICE_IN_S_SADC_OUT_SPEAKER_PHONE=-1;
static int SND_DEVICE_TTY_HEADSET=-1;
static int SND_DEVICE_TTY_HCO=-1;
static int SND_DEVICE_TTY_VCO=-1;
static int SND_DEVICE_TTY_FULL=-1;
static int SND_DEVICE_CARKIT=-1;
static int SND_DEVICE_FM_SPEAKER=-1;
static int SND_DEVICE_FM_HEADSET=-1;
static int SND_DEVICE_HANDSET_ALL=-1;
static int SND_DEVICE_NO_MIC_HEADSET=-1;
static int SND_DEVICE_IDLE=-1;

/* Specific pass-through device for special ops in libacoustic */
#define BT_CUSTOM_DEVICES_ID_OFFSET     300     // This will be used for bluetooth custom devices

static int SND_DEVICE_REC_INC_MIC = 252;
static int SND_DEVICE_PLAYBACK_HANDSFREE = 253;
static int SND_DEVICE_PLAYBACK_HEADSET = 254;

static bool mUseAcoustic = false;
static int bCurrentOutStream = AudioSystem::DEFAULT;

static int support_a1010 = 0;
static int a1010_fd = -1;
static int old_pathid = -1;
static int new_pathid = -1;

// ----------------------------------------------------------------------------

AudioHardware::AudioHardware() :
    mInit(false), mMicMute(true), mBluetoothNrec(true), mBluetoothId(0),
    mOutput(0), mSndEndpoints(NULL), mCurSndDevice(-1)
{

    int (*snd_get_num)() = NULL;
    int (*snd_get_endpoint)(int, msm_snd_endpoint *) = NULL;
#if 0 /* See comment bellow */
    int (*set_acoustic_parameters)();
#endif

    struct msm_snd_endpoint *ept;

    acoustic = ::dlopen("/system/lib/libhtc_acoustic.so", RTLD_NOW);
    if (acoustic == NULL ) {
        LOGE("Could not open libhtc_acoustic.so");
        /* this is not really an error on non-htc devices... */
        mNumSndEndpoints = 0;
        mInit = true;
        return;
    }

#if 0	/* The function in the original libhtc_acoustic parse the parameters
		 * from csv files and write the params in the shared memory.
		 * We can't use it because our AMSS does not work the same way.
		 * Plus, it won't be used as the htc-acoustic device required in 
		 * the kernel is disabled (to avoid the original parameters to be screwed).
		 * Our AMSS need only the current audio parameters to be written in
		 * shared memory at a fixed offset.
		 * The "custom" libhtc_acoustic will do this for us, with the help
		 * of a new htc-acousctic_wince device
		 */
    set_acoustic_parameters = (int (*)(void))::dlsym(acoustic, "set_acoustic_parameters");
    if ((*set_acoustic_parameters) == 0 ) {
        LOGE("Could not open set_acoustic_parameters()");
        return;
    }

    int rc = set_acoustic_parameters();
    if (rc < 0) {
        LOGE("Could not set acoustic parameters to share memory: %d", rc);
//        return;
    }
#endif

	htc_acoustic_init = (int (*)(void))::dlsym(acoustic, "htc_acoustic_init");
    if ((*htc_acoustic_init) == 0 ) {
        LOGE("Could not link htc_acoustic_init()");
    }
    
    if ( htc_acoustic_init != 0 ) {
        /* If acoustic init was ok, then continue linking other functions
         * otherwise, don't link them so that the rest of the hardware
         * can still work
         */
        if ( htc_acoustic_init() != 0 ) {
            LOGE("Failed to initialize htc acoutic system. Using basic hardware.");
        } else {
            mUseAcoustic = true;
            htc_acoustic_deinit = (int (*)(void))::dlsym(acoustic, "htc_acoustic_deinit");
            if ((*htc_acoustic_deinit) == 0 ) {
                LOGE("Could not link htc_acoustic_deinit()");
            }

            msm72xx_set_acoustic_table = (int (*)(int, int))::dlsym(acoustic, "msm72xx_set_acoustic_table");
            if ((*msm72xx_set_acoustic_table) == 0 ) {
                LOGE("Could not link msm72xx_set_acoustic_table()");
            }

            msm72xx_start_acoustic_setting = (int (*)(void))::dlsym(acoustic, "msm72xx_start_acoustic_setting");
            if ((*msm72xx_start_acoustic_setting) == 0 ) {
                LOGE("Could not link msm72xx_start_acoustic_setting()");
            }

            msm72xx_set_acoustic_done = (int (*)(void))::dlsym(acoustic, "msm72xx_set_acoustic_done");
            if ((*msm72xx_set_acoustic_done) == 0 ) {
                LOGE("Could not link msm72xx_set_acoustic_done()");
            }

            msm72xx_set_audio_path = (int (*)(bool, bool, int, bool))::dlsym(acoustic, "msm72xx_set_audio_path");
            if ((*msm72xx_set_audio_path) == 0 ) {
                LOGE("Could not link msm72xx_set_audio_path()");
            }

            msm72xx_update_audio_method = (int (*)(int))::dlsym(acoustic, "msm72xx_update_audio_method");
            if ((*msm72xx_update_audio_method) == 0 ) {
                LOGE("Could not link msm72xx_update_audio_method()");
            }

            msm72xx_get_bluetooth_hs_id =(int (*)(const char*))::dlsym(acoustic, "msm72xx_get_bluetooth_hs_id");
            if ((*msm72xx_get_bluetooth_hs_id) == 0 ) {
                LOGE("Could not link msm72xx_get_bluetooth_hs_id()");
            }
    
            /* Test for audience a1010 presence (rhodium devices only) */
            a1010_fd = open(AUDIENCE_A1010_DEV, O_RDWR);
            if ( a1010_fd < 0 ) {
                LOGE("Error opening dev %s (fd = %d). Error %s (%d)", AUDIENCE_A1010_DEV, a1010_fd,
                            strerror(errno), errno);
                support_a1010 = 0;
            } else {
                support_a1010 = 1;
                close(a1010_fd);
            }
    	}
	}	

    snd_get_num = (int (*)(void))::dlsym(acoustic, "snd_get_num_endpoints");
    if ((*snd_get_num) == 0 ) {
        LOGE("Could not link snd_get_num()");
    }

    snd_get_endpoint = (int (*)(int, msm_snd_endpoint *))::dlsym(acoustic, "snd_get_endpoint");
    if ((*snd_get_endpoint) == 0 ) {
        LOGE("Could not link snd_get_endpoint()");
        return;
    }

    if ( snd_get_num != NULL ) {
        mNumSndEndpoints = snd_get_num();
        LOGD("mNumSndEndpoints = %d", mNumSndEndpoints);
        mSndEndpoints = new msm_snd_endpoint[mNumSndEndpoints];
        mInit = true;
        LOGV("constructed %d SND endpoints", mNumSndEndpoints);
        ept = mSndEndpoints;
        if ( snd_get_endpoint != NULL ) {
            for (int cnt = 0; cnt < mNumSndEndpoints; cnt++, ept++) {
                ept->id = cnt;
                snd_get_endpoint(cnt, ept);
                LOGV("cnt = %d ept->name = %s ept->id = %d\n", cnt, ept->name, ept->id);
#define CHECK_FOR(desc) \
                if (!strcmp(ept->name, #desc)) { \
                    SND_DEVICE_##desc = ept->id; \
                } else
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
                CHECK_FOR(IDLE) {}
#undef CHECK_FOR
            }
        }
    } else {
        mNumSndEndpoints = 4;
        LOGV("constructed %d default SND endpoints", mNumSndEndpoints);
        mInit = true;
    }

    if ( mUseAcoustic ) {
        if ( SND_DEVICE_IDLE != -1 ) { 
            mCurSndDevice = SND_DEVICE_IDLE;
        }
        set_initial_audio_volume();
    }

    LOGV("AudioHardware::AudioHardware Initialized\n");
}

AudioHardware::~AudioHardware()
{
    for (size_t index = 0; index < mInputs.size(); index++) {
        closeInputStream((AudioStreamIn*)mInputs[index]);
    }
    mInputs.clear();
    closeOutputStream((AudioStreamOut*)mOutput);
    delete [] mSndEndpoints;

    /* In case we could initialize new acoustic library, then deinit to free memory */
    if ( (htc_acoustic_deinit != NULL) && mUseAcoustic ) {
        htc_acoustic_deinit();
    }

    if (acoustic) {
        ::dlclose(acoustic);
        acoustic = 0;
    }
    mInit = false;
}

status_t AudioHardware::initCheck()
{
    return mInit ? NO_ERROR : NO_INIT;
}

AudioStreamOut* AudioHardware::openOutputStream(
        uint32_t devices, int *format, uint32_t *channels, uint32_t *sampleRate, status_t *status)
{
    { // scope for the lock
        Mutex::Autolock lock(mLock);

        // only one output stream allowed
        if (mOutput) {
            if (status) {
                *status = INVALID_OPERATION;
            }
            return 0;
        }

        // create new output stream
        AudioStreamOutMSM72xx* out = new AudioStreamOutMSM72xx();
        status_t lStatus = out->set(this, devices, format, channels, sampleRate);
        if (status) {
            *status = lStatus;
        }
        if (lStatus == NO_ERROR) {
            mOutput = out;
        } else {
            delete out;
        }
    }
    return mOutput;
}

void AudioHardware::closeOutputStream(AudioStreamOut* out) {
    Mutex::Autolock lock(mLock);
    if (mOutput == 0 || mOutput != out) {
        LOGW("Attempt to close invalid output stream");
    }
    else {
        delete mOutput;
        mOutput = 0;
    }
}

AudioStreamIn* AudioHardware::openInputStream(
        uint32_t devices, int *format, uint32_t *channels, uint32_t *sampleRate, status_t *status,
        AudioSystem::audio_in_acoustics acoustic_flags)
{
    // check for valid input source
    if (!AudioSystem::isInputDevice((AudioSystem::audio_devices)devices)) {
        return 0;
    }

    mLock.lock();

    AudioStreamInMSM72xx* in = new AudioStreamInMSM72xx();
    status_t lStatus = in->set(this, devices, format, channels, sampleRate, acoustic_flags);
    if (status) {
        *status = lStatus;
    }
    if (lStatus != NO_ERROR) {
        mLock.unlock();
        delete in;
        return 0;
    }

    mInputs.add(in);
    mLock.unlock();

    return in;
}

void AudioHardware::closeInputStream(AudioStreamIn* in) {
    Mutex::Autolock lock(mLock);

    ssize_t index = mInputs.indexOf((AudioStreamInMSM72xx *)in);
    if (index < 0) {
        LOGW("Attempt to close invalid input stream");
    } else {
        mLock.unlock();
        delete mInputs[index];
        mLock.lock();
        mInputs.removeAt(index);
    }
}

status_t AudioHardware::setMode(int mode)
{
    status_t status = AudioHardwareBase::setMode(mode);
    if (status == NO_ERROR) {
        // make sure that doAudioRouteOrMute() is called by doRouting()
        // even if the new device selected is the same as current one.
        clearCurDevice();
    }
    return status;
}

bool AudioHardware::checkOutputStandby()
{
    if (mOutput)
        if (!mOutput->checkStandby())
            return false;

    return true;
}

status_t AudioHardware::setMicMute(bool state)
{
    Mutex::Autolock lock(mLock);
    return setMicMute_nosync(state);
}

// always call with mutex held
status_t AudioHardware::setMicMute_nosync(bool state)
{
    if (mMicMute != state) {
        mMicMute = state;
        return doAudioRouteOrMute(SND_DEVICE_CURRENT);
    }
    return NO_ERROR;
}

status_t AudioHardware::getMicMute(bool* state)
{
    *state = mMicMute;
    return NO_ERROR;
}

status_t AudioHardware::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 value;
    String8 key;
    const char BT_NREC_KEY[] = "bt_headset_nrec";
    const char BT_NAME_KEY[] = "bt_headset_name";
    const char BT_NREC_VALUE_ON[] = "on";


    LOGV("setParameters() %s", keyValuePairs.string());

    if (keyValuePairs.length() == 0) return BAD_VALUE;

    key = String8(BT_NREC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == BT_NREC_VALUE_ON) {
            mBluetoothNrec = true;
        } else {
            mBluetoothNrec = false;
            LOGI("Turning noise reduction and echo cancellation off for BT "
                 "headset");
        }
    }
    key = String8(BT_NAME_KEY);
    if (param.get(key, value) == NO_ERROR) {
        mBluetoothId = 0;
        if ( mUseAcoustic ) {
            mBluetoothId = msm72xx_get_bluetooth_hs_id(value.string());
        } else {
            for (int i = 0; i < mNumSndEndpoints; i++) {
                if (!strcasecmp(value.string(), mSndEndpoints[i].name)) {
                    mBluetoothId = mSndEndpoints[i].id;
                    LOGI("Using custom acoustic parameters for %s", value.string());
                    break;
                }
            }
        }
        if (mBluetoothId == 0) {
            LOGI("Using default acoustic parameters "
                 "(%s not in acoustic database)", value.string());
            doRouting();
        }
    }
    return NO_ERROR;
}

String8 AudioHardware::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    return param.toString();
}


static unsigned calculate_audpre_table_index(unsigned index)
{
    switch (index) {
        case 48000:    return SAMP_RATE_INDX_48000;
        case 44100:    return SAMP_RATE_INDX_44100;
        case 32000:    return SAMP_RATE_INDX_32000;
        case 24000:    return SAMP_RATE_INDX_24000;
        case 22050:    return SAMP_RATE_INDX_22050;
        case 16000:    return SAMP_RATE_INDX_16000;
        case 12000:    return SAMP_RATE_INDX_12000;
        case 11025:    return SAMP_RATE_INDX_11025;
        case 8000:    return SAMP_RATE_INDX_8000;
        default:     return -1;
    }
}
size_t AudioHardware::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    if (format != AudioSystem::PCM_16_BIT) {
        LOGW("getInputBufferSize bad format: %d", format);
        return 0;
    }
    if (channelCount < 1 || channelCount > 2) {
        LOGW("getInputBufferSize bad channel count: %d", channelCount);
        return 0;
    }

    return 2048*channelCount;
}

/* This function will be called when volume change is done. It will apply new
 * parameters in tables and manage the method used for audio volume control
 */
static status_t doAcousticVolumeUpdate(struct msm_snd_volume_config* args,
                                      uint32_t fd)
{
    int device_method = SND_METHOD_NONE;
    int device = args->device;

    LOGV("doAcousticVolumeUpdate %d %d %d", args->device, args->method, args->volume);

    if ( device != SND_DEVICE_IDLE ) {
         if ( msm72xx_set_acoustic_table != NULL ) {
             device_method = msm72xx_set_acoustic_table(device, args->volume);
         }

         /* Some devices do not require/support volume setting */
         if ( device_method != SND_METHOD_NONE ) {
             /* Some devices only accept volume level 5 */
             if ( device_method == SND_METHOD_AUDIO ) {
                LOGV("call snd_set_volume audio 5");
                args->device = SND_DEVICE_IDLE;
                args->method = SND_METHOD_AUDIO;
                args->volume = 5;
             } else {
                LOGV("call snd_set_volume voice %d", args->volume);
            }

#if LOG_SND_RPC
             LOGD("rpc snd_set_volume(%d, %d, %d)\n", args->device, args->method, args->volume);
#endif
             if (ioctl(fd, SND_SET_VOLUME, args) < 0) {
                 LOGE("snd_set_volume error.");
                 close(fd);
                 return -EIO;
             }
         }
    } else {
#if LOG_SND_RPC
         LOGD("rpc snd_set_volume(%d, %d, %d)\n", args->device, args->method, args->volume);
#endif
         if (ioctl(fd, SND_SET_VOLUME, args) < 0) {
             LOGE("snd_set_volume error.");
             close(fd);
             return -EIO;
         }
    }

    return NO_ERROR;
}

static status_t set_volume_rpc(uint32_t device,
                               uint32_t method,
                               uint32_t volume)
{
    int fd;
#if LOG_SND_RPC
    /* If acoustic library is used, debug will be done in doAcousticVolumeUpdate */
    if ( !mUseAcoustic ) {
        LOGD("rpc_snd_set_volume(%d, %d, %d)\n", device, method, volume);
    }
#endif

    if (device == -1UL) return NO_ERROR;

    fd = open("/dev/msm_snd", O_RDWR);
    if (fd < 0) {
        LOGE("Can not open snd device");
        return -EPERM;
    }
    /* rpc_snd_set_volume(
     *     device,            # Any hardware device enum, including
     *                        # SND_DEVICE_CURRENT
     *     method,            # must be SND_METHOD_VOICE to do anything useful
     *     volume,            # integer volume level, in range [0,5].
     *                        # note that 0 is audible (not quite muted)
     *  )
     * rpc_snd_set_volume only works for in-call sound volume.
     */
     struct msm_snd_volume_config args;
     args.device = device;
     args.method = method;
     args.volume = volume;

     if ( mUseAcoustic ) {
        doAcousticVolumeUpdate(&args, fd);
     } else {
         if (ioctl(fd, SND_SET_VOLUME, &args) < 0) {
             LOGE("snd_set_volume error.");
             close(fd);
             return -EIO;
         }
     }
     close(fd);
     return NO_ERROR;
}

status_t AudioHardware::setVoiceVolume(float v)
{
    if (v < 0.0) {
        LOGW("setVoiceVolume(%f) under 0.0, assuming 0.0\n", v);
        v = 0.0;
    } else if (v > 1.0) {
        LOGW("setVoiceVolume(%f) over 1.0, assuming 1.0\n", v);
        v = 1.0;
    }

    int vol = lrint(v * 5.0);
    LOGD("setVoiceVolume(%f)\n", v);
    LOGI("Setting in-call volume to %d (available range is 0 to 5)\n", vol);

    Mutex::Autolock lock(mLock);
    set_volume_rpc(SND_DEVICE_CURRENT, SND_METHOD_VOICE, vol);
    return NO_ERROR;
}

status_t AudioHardware::setMasterVolume(float v)
{
    Mutex::Autolock lock(mLock);
    int vol = ceil(v * 5.0);
    LOGI("Set master volume to %d.\n", vol);
    /*
    set_volume_rpc(SND_DEVICE_HANDSET, SND_METHOD_VOICE, vol);
    set_volume_rpc(SND_DEVICE_SPEAKER, SND_METHOD_VOICE, vol);
    set_volume_rpc(SND_DEVICE_BT,      SND_METHOD_VOICE, vol);
    set_volume_rpc(SND_DEVICE_HEADSET, SND_METHOD_VOICE, vol);
    */
    // We return an error code here to let the audioflinger do in-software
    // volume on top of the maximum volume that we set through the SND API.
    // return error - software mixer will handle it
    return -1;
}

/* This function will be called on device change to sets up the volume 
 * on the new device and to update the audio settings accordling to the
 * new device and new volume
 */
status_t AudioHardware::doUpdateVolume(uint32_t inputDevice)
{
    /* Update volume in case of device change */
    int volume = get_master_volume();

    LOGV("AudioHardware::doUpdateVolume %d", mCurSndDevice);
    
    if ( msm72xx_start_acoustic_setting != NULL ) {
        msm72xx_start_acoustic_setting();
    } 

	bool in_call = mMode == AudioSystem::MODE_IN_CALL;
	bool use_mic = (inputDevice & AudioSystem::DEVICE_IN_BUILTIN_MIC);
	use_mic |= (inputDevice & AudioSystem::DEVICE_IN_BACK_MIC);

    /* When in call, use the METHOD_VOICE to set the volume */
    if (in_call) {
		LOGI("updating volume method=VOICE");
		set_volume_rpc(mCurSndDevice, SND_METHOD_VOICE, volume);
    }
	else if (use_mic) {
		LOGI("updating recording volume method=AUDIO");
        set_volume_rpc(SND_DEVICE_REC_INC_MIC, SND_METHOD_AUDIO, volume);
	}
	else {
		LOGI("updating volume method=AUDIO");
		set_volume_rpc(mCurSndDevice, SND_METHOD_AUDIO, volume);
	}

    /* Tell the audio acoustic controller that we have processed the new settings */
    if ( msm72xx_set_acoustic_done != NULL ) {
        msm72xx_set_acoustic_done();
    }

    return NO_ERROR;
}

/* This functions configures the A1010 audience controller to use the correct settings */
status_t AudioHardware::doAudience_A1010_Control(void)
{
    int rc = 0;

    if (a1010_fd < 0) {
        a1010_fd = open(AUDIENCE_A1010_DEV, O_RDWR);
        if (a1010_fd < 0) {
            LOGE("Cannot open audience_A1010 device (%d)\n", a1010_fd);
            return -1;
    	}
    }

    if ( mCurSndDevice == SND_DEVICE_SPEAKER_MIC ) {
        new_pathid = A1010_PATH_SPEAKER;
    }
    else {
        new_pathid = A1010_PATH_SUSPEND;
    }

    if (old_pathid != new_pathid) {
        LOGI("A1010: do ioctl(A1010_SET_CONFIG) to %d\n", new_pathid);
	rc = ioctl(a1010_fd, A1010_SET_CONFIG, &new_pathid);
	if (!rc)
		old_pathid = new_pathid;
	else
	    goto Error;
    }

Error:
    close(a1010_fd);
    a1010_fd = -1;

    return rc;
}

/* This function will be called on device change, so that hardware and software changes
 * will be done by the new acoustic library. It will enable outputs, set new tables,
 * handle special modes (like music playback, enable mic while recording), ...
 */
status_t AudioHardware::doAcousticAudioDeviceChange(struct msm_snd_device_config* args)
{
    int fd;
    uint32_t inputDevice = 0;

    LOGV("AudioHardware::update_device %d %d %d", args->device, args->ear_mute, args->mic_mute);

    if ( msm72xx_start_acoustic_setting != NULL ) {
        msm72xx_start_acoustic_setting();
    }
	
	bool in_call = mMode == AudioSystem::MODE_IN_CALL;

    /* If the current device is speaker, then lower the volume before 
     * switching to the new device.
     * On some device, it can cause power collapse if speaker is not powered off
     * (i.e : on diamond)
     */
    if ( mCurSndDevice != SND_DEVICE_IDLE ) {
        if (in_call) {
			LOGI("disabling volume method=VOICE");
			set_volume_rpc(SND_DEVICE_CURRENT, SND_METHOD_VOICE, 0);
		}
		else {
			LOGI("disabling volume method=AUDIO");
			set_volume_rpc(SND_DEVICE_CURRENT, SND_METHOD_AUDIO, 0);
    	}
	}

    /* Microphone should be un-muted when recording or during voice call */
    AudioStreamInMSM72xx *input = getActiveInput_l();
    if ( input == NULL ) 
    {
        /* To be removed if the "send_mic_mute_to_AudioManager" param is set
         * to true in the phone app config file (/packages/apps/Phone/res/values/config.xml)
         */
        if ( mMode != AudioSystem::MODE_IN_CALL ) {
            args->mic_mute = true;  
        } else {
            args->mic_mute = false;
        }
    } else {
        inputDevice = input->devices();
        if (inputDevice & AudioSystem::DEVICE_IN_ALL) {
			LOGI("enabling mic recording");
            args->mic_mute = false;  
        } else {
			LOGI("disabling mic recording");
            args->mic_mute = true;
        }
    }
    
	/* Redirect output to correct device for specials devices */
    if ( (int)args->device == SND_DEVICE_PLAYBACK_HANDSFREE ) {
		LOGI("redirecting output to SPEAKER");
        args->device = SND_DEVICE_SPEAKER;
    } else if ( (int)args->device == SND_DEVICE_PLAYBACK_HEADSET ) {
		LOGI("redirecting output to HEADSET");
        args->device = SND_DEVICE_HEADSET;
    } else if ( (int)args->device >= BT_CUSTOM_DEVICES_ID_OFFSET ) {
		LOGI("redirecting output to BT");
        args->device = SND_DEVICE_BT;
    }
 
    /* Do not use SND_DEVICE_CURRENT */
    if ( args->device == (unsigned int)SND_DEVICE_CURRENT ) {
		LOGI("patching device to mCurSndDevice(%d)", mCurSndDevice);
        args->device = mCurSndDevice;
    }

    if ( msm72xx_set_audio_path != NULL ) {
        bool bEnableOut = false;
        /* XXX: Can't be used. @ boot time, isStreamActive blocks media service.
        bool bMusic;
        AudioSystem::isStreamActive(AudioSystem::MUSIC, &bMusic);*/
        if (in_call || (bCurrentOutStream != AudioSystem::DEFAULT)) {
            bEnableOut = true;    
        }
        /* If recording while speaker is in use, then enable dual mic */
		bool use_mic = inputDevice & AudioSystem::DEVICE_IN_BUILTIN_MIC;
		bool rear_mic = inputDevice & AudioSystem::DEVICE_IN_BACK_MIC;
		bool use_spk = ((int)args->device) == SND_DEVICE_SPEAKER;

		if ((in_call || use_mic || rear_mic) && use_spk) {
            msm72xx_set_audio_path(!args->mic_mute, 1, args->device, bEnableOut );
            mCurSndDevice = SND_DEVICE_SPEAKER_MIC;
			args->device = SND_DEVICE_SPEAKER_MIC;
			LOGI("mCurSndDevice <- SPEAKER_MIC");
        } else {
            msm72xx_set_audio_path(!args->mic_mute, 0, args->device, bEnableOut );
        }
    }

    // TODO : switch on/off leds as done in msm_setup_audio() ? 
    if ( msm72xx_set_acoustic_table != NULL ) {
        msm72xx_set_acoustic_table(args->device, get_master_volume());
    }


    /* Currently only used for the SPEAKER_MIC device but might be expanded to other devices
     * if dual mic selection is supported by android
     */
    if ( support_a1010 ) {
        doAudience_A1010_Control();
    }

    LOGV("call snd_set_device %d", args->device);

    /* Open msm_snd to set device */
    fd = open("/dev/msm_snd", O_RDWR);
    if (fd < 0) {
        LOGE("Can not open snd device");
        if ( msm72xx_set_acoustic_done != NULL ) {
            msm72xx_set_acoustic_done();
        }
        return -EPERM;
    }

#if LOG_SND_RPC
    LOGD("rpc snd_set_device(%d, %d, %d)\n", args->device, args->ear_mute, args->mic_mute);
#endif
    if (ioctl(fd, SND_SET_DEVICE, args) < 0) {
        LOGE("snd_set_device error.");
        close(fd);
        if ( msm72xx_set_acoustic_done != NULL ) {
            msm72xx_set_acoustic_done();
        }
        return -EIO;
    }

    if ( msm72xx_set_acoustic_done != NULL ) {
        msm72xx_set_acoustic_done();
    }

    close(fd);
    return NO_ERROR;
}

status_t AudioHardware::do_route_audio_rpc(uint32_t device,
                                   bool ear_mute, bool mic_mute)
{
    if (device == -1UL)
        return NO_ERROR;

    int fd;
#if LOG_SND_RPC
    if ( !mUseAcoustic ) {
        LOGD("rpc_snd_set_device(%d, %d, %d)\n", device, ear_mute, mic_mute);
    }
#endif

    if ( !mUseAcoustic ) {
        fd = open("/dev/msm_snd", O_RDWR);
        if (fd < 0) {
            LOGE("Can not open snd device");
            return -EPERM;
        }
    }
    // RPC call to switch audio path
    /* rpc_snd_set_device(
     *     device,            # Hardware device enum to use
     *     ear_mute,          # Set mute for outgoing voice audio
     *                        # this should only be unmuted when in-call
     *     mic_mute,          # Set mute for incoming voice audio
     *                        # this should only be unmuted when in-call or
     *                        # recording.
     *  )
     */
    struct msm_snd_device_config args;
    args.device = device;
    args.ear_mute = ear_mute ? SND_MUTE_MUTED : SND_MUTE_UNMUTED;
    args.mic_mute = mic_mute ? SND_MUTE_MUTED : SND_MUTE_UNMUTED;

    if ( mUseAcoustic ) {
       doAcousticAudioDeviceChange(&args);
    } else {
        if (ioctl(fd, SND_SET_DEVICE, &args) < 0) {
            LOGE("snd_set_device error.");
            close(fd);
            return -EIO;
        }
        close(fd);
    }

    mCurSndDevice = args.device;

    return NO_ERROR;
}

// always call with mutex held
status_t AudioHardware::doAudioRouteOrMute(uint32_t device)
{
    if (device == (uint32_t)SND_DEVICE_BT || device == (uint32_t)SND_DEVICE_CARKIT) {
        if (mBluetoothId) {
            device = mBluetoothId;
        } else if (!mBluetoothNrec) {
            device = SND_DEVICE_BT_EC_OFF;
        }
    }
    LOGV("doAudioRouteOrMute() device %x, mMode %d, mMicMute %d", device, mMode, mMicMute);
    return do_route_audio_rpc(device,
                              mMode != AudioSystem::MODE_IN_CALL, mMicMute);
}

status_t AudioHardware::doRouting()
{
    /* currently this code doesn't work without the htc libacoustic */
    if (!acoustic)
        return 0;

    Mutex::Autolock lock(mLock);
    uint32_t outputDevices = mOutput->devices();
    status_t ret = NO_ERROR;
    int (*msm72xx_enable_audpp)(int);
    msm72xx_enable_audpp = (int (*)(int))::dlsym(acoustic, "msm72xx_enable_audpp");
    int audProcess = (ADRC_DISABLE | EQ_DISABLE | RX_IIR_DISABLE);
    AudioStreamInMSM72xx *input = getActiveInput_l();
    uint32_t inputDevice = (input == NULL) ? 0 : input->devices();
    int sndDevice = -1;

    // Ignore routing device information when we start recording in voice
    // call. Recording will happen through currently active tx device
    if(inputDevice == AudioSystem::DEVICE_IN_VOICE_CALL)
        return NO_ERROR;
    if (inputDevice != 0) {
        LOGI("do input routing device %x\n", inputDevice);
 
#if 0
        /* Audio recording seems to need some special tweaks to work when used with speakerphone
         * so default to headset to make it work
         */
        if ( (inputDevice & AudioSystem::DEVICE_IN_BUILTIN_MIC) ||
             (inputDevice & AudioSystem::DEVICE_IN_BACK_MIC) ) {
            LOGI("Routing audio to Headset\n");
            sndDevice = SND_DEVICE_HANDSET;
        } else 
#endif
        if (inputDevice & AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
            LOGI("Routing audio to Bluetooth PCM\n");
            sndDevice = SND_DEVICE_BT;
        } else if (inputDevice & AudioSystem::DEVICE_IN_WIRED_HEADSET) {
            if ((outputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) &&
                (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER)) {
                LOGI("Routing audio to Wired Headset and Speaker\n");
                sndDevice = SND_DEVICE_HEADSET_AND_SPEAKER;
                audProcess = (ADRC_ENABLE | EQ_ENABLE | RX_IIR_ENABLE);
            } else {
                LOGI("Routing audio to Wired Headset\n");
                sndDevice = SND_DEVICE_HEADSET;
            }
        } else {
            if (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) {
                LOGI("Routing audio to Speakerphone\n");
                sndDevice = SND_DEVICE_SPEAKER;
                audProcess = (ADRC_ENABLE | EQ_ENABLE | RX_IIR_ENABLE);
            } else {
                LOGI("Routing audio to Handset\n");
                sndDevice = SND_DEVICE_HANDSET;
            }
        }
    }
    // if inputDevice == 0, restore output routing

    if (sndDevice == -1) {
        if (outputDevices & (outputDevices - 1)) {
            if ((outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) == 0) {
                LOGW("Hardware does not support requested route combination (%#X),"
                     " picking closest possible route...", outputDevices);
            }
        }

        if (outputDevices &
            (AudioSystem::DEVICE_OUT_BLUETOOTH_SCO | AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET)) {
            LOGI("Routing audio to Bluetooth PCM\n");
            sndDevice = SND_DEVICE_BT;
        } else if (outputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT) {
            LOGI("Routing audio to Bluetooth PCM\n");
            sndDevice = SND_DEVICE_CARKIT;
        } else if ((outputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) &&
                   (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER)) {
            LOGI("Routing audio to Wired Headset and Speaker\n");
            sndDevice = SND_DEVICE_HEADSET_AND_SPEAKER;
            audProcess = (ADRC_ENABLE | EQ_ENABLE | RX_IIR_ENABLE);
        } else if (outputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) {
            if (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) {
                LOGI("Routing audio to No microphone Wired Headset and Speaker (%d,%x)\n", mMode, outputDevices);
                sndDevice = SND_DEVICE_HEADSET_AND_SPEAKER;
                audProcess = (ADRC_ENABLE | EQ_ENABLE | RX_IIR_ENABLE);
            } else {
                LOGI("Routing audio to No microphone Wired Headset (%d,%x)\n", mMode, outputDevices);
                sndDevice = SND_DEVICE_NO_MIC_HEADSET;
            }
        } else if (outputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) {
            LOGI("Routing audio to Wired Headset\n");
            sndDevice = SND_DEVICE_HEADSET;
        } else if (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) {
            LOGI("Routing audio to Speakerphone\n");
            sndDevice = SND_DEVICE_SPEAKER;
            audProcess = (ADRC_ENABLE | EQ_ENABLE | RX_IIR_ENABLE);
        } else {
            LOGI("Routing audio to Handset\n");
            sndDevice = SND_DEVICE_HANDSET;
        }
    }

    if (sndDevice != -1 && sndDevice != mCurSndDevice) {
        ret = doAudioRouteOrMute(sndDevice);
        if ((*msm72xx_enable_audpp) == 0 ) {
            LOGE("Could not open msm72xx_enable_audpp()");
        } else {
            msm72xx_enable_audpp(audProcess);
        }

        if ( mUseAcoustic ) {
            /* Update the acoustic hardware with new device settings */
            doUpdateVolume(mCurSndDevice);
        }
    }

    return ret;
}

status_t AudioHardware::checkMicMute()
{
    Mutex::Autolock lock(mLock);
    if (mMode != AudioSystem::MODE_IN_CALL) {
        setMicMute_nosync(true);
    }

    return NO_ERROR;
}

status_t AudioHardware::dumpInternals(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append("AudioHardware::dumpInternals\n");
    snprintf(buffer, SIZE, "\tmInit: %s\n", mInit? "true": "false");
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmMicMute: %s\n", mMicMute? "true": "false");
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmBluetoothNrec: %s\n", mBluetoothNrec? "true": "false");
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmBluetoothId: %d\n", mBluetoothId);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

status_t AudioHardware::dump(int fd, const Vector<String16>& args)
{
    dumpInternals(fd, args);
    for (size_t index = 0; index < mInputs.size(); index++) {
        mInputs[index]->dump(fd, args);
    }

    if (mOutput) {
        mOutput->dump(fd, args);
    }
    return NO_ERROR;
}

uint32_t AudioHardware::getInputSampleRate(uint32_t sampleRate)
{
    uint32_t i;
    uint32_t prevDelta;
    uint32_t delta;

    for (i = 0, prevDelta = 0xFFFFFFFF; i < sizeof(inputSamplingRates)/sizeof(uint32_t); i++, prevDelta = delta) {
        delta = abs(sampleRate - inputSamplingRates[i]);
        if (delta > prevDelta) break;
    }
    // i is always > 0 here
    return inputSamplingRates[i-1];
}

// getActiveInput_l() must be called with mLock held
AudioHardware::AudioStreamInMSM72xx *AudioHardware::getActiveInput_l()
{
    for (size_t i = 0; i < mInputs.size(); i++) {
        // return first input found not being in standby mode
        // as only one input can be in this state
        if (mInputs[i]->state() > AudioStreamInMSM72xx::AUDIO_INPUT_CLOSED) {
            return mInputs[i];
        }
    }

    return NULL;
}
// ----------------------------------------------------------------------------

AudioHardware::AudioStreamOutMSM72xx::AudioStreamOutMSM72xx() :
    mHardware(0), mFd(-1), mStartCount(0), mRetryCount(0), mStandby(true), mDevices(0)
{
}

status_t AudioHardware::AudioStreamOutMSM72xx::set(
        AudioHardware* hw, uint32_t devices, int *pFormat, uint32_t *pChannels, uint32_t *pRate)
{
    int lFormat = pFormat ? *pFormat : 0;
    uint32_t lChannels = pChannels ? *pChannels : 0;
    uint32_t lRate = pRate ? *pRate : 0;

    mHardware = hw;

    // fix up defaults
    if (lFormat == 0) lFormat = format();
    if (lChannels == 0) lChannels = channels();
    if (lRate == 0) lRate = sampleRate();

    // check values
    if ((lFormat != format()) ||
        (lChannels != channels()) ||
        (lRate != sampleRate())) {
        if (pFormat) *pFormat = format();
        if (pChannels) *pChannels = channels();
        if (pRate) *pRate = sampleRate();
        return BAD_VALUE;
    }

    if (pFormat) *pFormat = lFormat;
    if (pChannels) *pChannels = lChannels;
    if (pRate) *pRate = lRate;

    mDevices = devices;

    return NO_ERROR;
}

AudioHardware::AudioStreamOutMSM72xx::~AudioStreamOutMSM72xx()
{
    if ( mUseAcoustic ) {
        /* Reset output stream to default and apply setting to acoustic device */
        bCurrentOutStream = AudioSystem::DEFAULT;
        mHardware->doAudioRouteOrMute(SND_DEVICE_CURRENT);
    }
    if (mFd >= 0) close(mFd);
}

ssize_t AudioHardware::AudioStreamOutMSM72xx::write(const void* buffer, size_t bytes)
{
    // LOGD("AudioStreamOutMSM72xx::write(%p, %u)", buffer, bytes);
    status_t status = NO_INIT;
    size_t count = bytes;
    const uint8_t* p = static_cast<const uint8_t*>(buffer);

    if (mStandby) {

        // open driver
        LOGV("open driver");
        status = ::open("/dev/msm_pcm_out", O_RDWR);
        if (status < 0) {
            LOGE("Cannot open /dev/msm_pcm_out errno: %d", errno);
            goto Error;
        }
        mFd = status;

        // configuration
        LOGV("get config");
        struct msm_audio_config config;
        status = ioctl(mFd, AUDIO_GET_CONFIG, &config);
        if (status < 0) {
            LOGE("Cannot read config");
            goto Error;
        }

        LOGV("set config");
        config.channel_count = AudioSystem::popCount(channels());
        config.sample_rate = sampleRate();
        config.buffer_size = bufferSize();
        config.buffer_count = AUDIO_HW_NUM_OUT_BUF;
        config.codec_type = CODEC_TYPE_PCM;
        status = ioctl(mFd, AUDIO_SET_CONFIG, &config);
        if (status < 0) {
            LOGE("Cannot set config");
            goto Error;
        }

        LOGV("buffer_size: %u", config.buffer_size);
        LOGV("buffer_count: %u", config.buffer_count);
        LOGV("channel_count: %u", config.channel_count);
        LOGV("sample_rate: %u", config.sample_rate);

        // fill 2 buffers before AUDIO_START
        mStartCount = AUDIO_HW_NUM_OUT_BUF;
        mStandby = false;
    }

    while (count) {
        ssize_t written = ::write(mFd, p, count);
        if (written >= 0) {
            count -= written;
            p += written;
        } else {
            if (errno != EAGAIN) return written;
            mRetryCount++;
            LOGW("EAGAIN - retry");
        }
    }

    // start audio after we fill 2 buffers
    if (mStartCount) {
        if (--mStartCount == 0) {
            ioctl(mFd, AUDIO_START, 0);
     
            if ( mUseAcoustic ) {
                /* Get the current output stream type */
                bCurrentOutStream = get_current_stream();
                /* Sets up acoustic hardware */
                if ( mHardware->mCurSndDevice == SND_DEVICE_SPEAKER ) {
                    mHardware->doAudioRouteOrMute(SND_DEVICE_PLAYBACK_HANDSFREE);
                } else if ( mHardware->mCurSndDevice == SND_DEVICE_HEADSET ) {
                    mHardware->doAudioRouteOrMute(SND_DEVICE_PLAYBACK_HEADSET);
                } else {
                    /* Let the device be choosen by actual settings */
                    mHardware->doRouting();
                }
            }
        }
    }
    return bytes;

Error:
    if (mFd >= 0) {
        if ( mUseAcoustic ) {
            /* Reset output stream to default and apply setting to acoustic device */
            bCurrentOutStream = AudioSystem::DEFAULT;
            mHardware->doAudioRouteOrMute(SND_DEVICE_CURRENT);
        }
        ::close(mFd);
        mFd = -1;
    }
    // Simulate audio output timing in case of error
    usleep(bytes * 1000000 / frameSize() / sampleRate());

    return status;
}

status_t AudioHardware::AudioStreamOutMSM72xx::standby()
{
    status_t status = NO_ERROR;
    if (!mStandby && mFd >= 0) {
        if ( mUseAcoustic ) {
            /* Reset output stream to default and apply setting to acoustic device */
            bCurrentOutStream = AudioSystem::DEFAULT;
            mHardware->doAudioRouteOrMute(SND_DEVICE_CURRENT);
        }
        ::close(mFd);
        mFd = -1;
    }
    mStandby = true;
    return status;
}

status_t AudioHardware::AudioStreamOutMSM72xx::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append("AudioStreamOutMSM72xx::dump\n");
    snprintf(buffer, SIZE, "\tsample rate: %d\n", sampleRate());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tbuffer size: %d\n", bufferSize());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tchannels: %d\n", channels());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tformat: %d\n", format());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmHardware: %p\n", mHardware);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmFd: %d\n", mFd);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmStartCount: %d\n", mStartCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmRetryCount: %d\n", mRetryCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmStandby: %s\n", mStandby? "true": "false");
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

bool AudioHardware::AudioStreamOutMSM72xx::checkStandby()
{
    return mStandby;
}


status_t AudioHardware::AudioStreamOutMSM72xx::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting);
    status_t status = NO_ERROR;
    int device;
    LOGV("AudioStreamOutMSM72xx::setParameters() %s", keyValuePairs.string());

    if (param.getInt(key, device) == NO_ERROR) {
        mDevices = device;
        LOGV("set output routing %x", mDevices);
        status = mHardware->doRouting();
        param.remove(key);
    }

    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}

String8 AudioHardware::AudioStreamOutMSM72xx::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);

    if (param.get(key, value) == NO_ERROR) {
        LOGV("get routing %x", mDevices);
        param.addInt(key, (int)mDevices);
    }

    LOGV("AudioStreamOutMSM72xx::getParameters() %s", param.toString().string());
    return param.toString();
}

status_t AudioHardware::AudioStreamOutMSM72xx::getRenderPosition(uint32_t *dspFrames)
{
    //TODO: enable when supported by driver
    return INVALID_OPERATION;
}

// ----------------------------------------------------------------------------

AudioHardware::AudioStreamInMSM72xx::AudioStreamInMSM72xx() :
    mHardware(0), mFd(-1), mState(AUDIO_INPUT_CLOSED), mRetryCount(0),
    mFormat(AUDIO_HW_IN_FORMAT), mChannels(AUDIO_HW_IN_CHANNELS),
    mSampleRate(AUDIO_HW_IN_SAMPLERATE), mBufferSize(AUDIO_HW_IN_BUFFERSIZE),
    mAcoustics((AudioSystem::audio_in_acoustics)0), mDevices(0)
{
}

status_t AudioHardware::AudioStreamInMSM72xx::set(
        AudioHardware* hw, uint32_t devices, int *pFormat, uint32_t *pChannels, uint32_t *pRate,
        AudioSystem::audio_in_acoustics acoustic_flags)
{
    if (pFormat == 0 || *pFormat != AUDIO_HW_IN_FORMAT) {
        *pFormat = AUDIO_HW_IN_FORMAT;
        return BAD_VALUE;
    }
    if (pRate == 0) {
        return BAD_VALUE;
    }
    uint32_t rate = hw->getInputSampleRate(*pRate);
    if (rate != *pRate) {
        *pRate = rate;
        return BAD_VALUE;
    }

    if (pChannels == 0 || (*pChannels != AudioSystem::CHANNEL_IN_MONO &&
        *pChannels != AudioSystem::CHANNEL_IN_STEREO)) {
        *pChannels = AUDIO_HW_IN_CHANNELS;
        return BAD_VALUE;
    }

    mHardware = hw;

    LOGV("AudioStreamInMSM72xx::set(%d, %d, %u)", *pFormat, *pChannels, *pRate);
    if (mFd >= 0) {
        LOGE("Audio record already open");
        return -EPERM;
    }

    // open audio input device
    status_t status = ::open("/dev/msm_pcm_in", O_RDWR);
    if (status < 0) {
        LOGE("Cannot open /dev/msm_pcm_in errno: %d", errno);
        goto Error;
    }
    mFd = status;

    // configuration
    LOGV("get config");
    struct msm_audio_config config;
    status = ioctl(mFd, AUDIO_GET_CONFIG, &config);
    if (status < 0) {
        LOGE("Cannot read config");
        goto Error;
    }

    LOGV("set config");
    config.channel_count = AudioSystem::popCount(*pChannels);
    config.sample_rate = *pRate;
    config.buffer_size = bufferSize();
    config.buffer_count = 2;
    config.codec_type = CODEC_TYPE_PCM;
    status = ioctl(mFd, AUDIO_SET_CONFIG, &config);
    if (status < 0) {
        LOGE("Cannot set config");
        if (ioctl(mFd, AUDIO_GET_CONFIG, &config) == 0) {
            if (config.channel_count == 1) {
                *pChannels = AudioSystem::CHANNEL_IN_MONO;
            } else {
                *pChannels = AudioSystem::CHANNEL_IN_STEREO;
            }
            *pRate = config.sample_rate;
        }
        goto Error;
    }

    LOGV("confirm config");
    status = ioctl(mFd, AUDIO_GET_CONFIG, &config);
    if (status < 0) {
        LOGE("Cannot read config");
        goto Error;
    }
    LOGV("buffer_size: %u", config.buffer_size);
    LOGV("buffer_count: %u", config.buffer_count);
    LOGV("channel_count: %u", config.channel_count);
    LOGV("sample_rate: %u", config.sample_rate);

    mDevices = devices;
    mFormat = AUDIO_HW_IN_FORMAT;
    mChannels = *pChannels;
    mSampleRate = config.sample_rate;
    mBufferSize = config.buffer_size;

    //mHardware->setMicMute_nosync(false);
    mState = AUDIO_INPUT_OPENED;

    if (!acoustic)
        return NO_ERROR;

    audpre_index = calculate_audpre_table_index(mSampleRate);
    tx_iir_index = (audpre_index * 2) + (hw->checkOutputStandby() ? 0 : 1);
    LOGD("audpre_index = %d, tx_iir_index = %d\n", audpre_index, tx_iir_index);

    /**
     * If audio-preprocessing failed, we should not block record.
     */
    int (*msm72xx_set_audpre_params)(int, int);
    msm72xx_set_audpre_params = (int (*)(int, int))::dlsym(acoustic, "msm72xx_set_audpre_params");
    if ( msm72xx_set_audpre_params != NULL ) {
        status = msm72xx_set_audpre_params(audpre_index, tx_iir_index);
        if (status < 0)
            LOGE("Cannot set audpre parameters");
    }

    int (*msm72xx_enable_audpre)(int, int, int);
    msm72xx_enable_audpre = (int (*)(int, int, int))::dlsym(acoustic, "msm72xx_enable_audpre");
    mAcoustics = acoustic_flags;
    if ( msm72xx_enable_audpre != NULL ) {
        status = msm72xx_enable_audpre((int)acoustic_flags, audpre_index, tx_iir_index);
        if (status < 0)
            LOGE("Cannot enable audpre");
    }

    return NO_ERROR;

Error:
    if (mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }
    return status;
}

AudioHardware::AudioStreamInMSM72xx::~AudioStreamInMSM72xx()
{
    LOGV("AudioStreamInMSM72xx destructor");
    standby();
}

ssize_t AudioHardware::AudioStreamInMSM72xx::read( void* buffer, ssize_t bytes)
{
    LOGV("AudioStreamInMSM72xx::read(%p, %ld)", buffer, bytes);
    if (!mHardware) return -1;

    size_t count = bytes;
    uint8_t* p = static_cast<uint8_t*>(buffer);

    if (mState < AUDIO_INPUT_OPENED) {
        Mutex::Autolock lock(mHardware->mLock);
        if (set(mHardware, mDevices, &mFormat, &mChannels, &mSampleRate, mAcoustics) != NO_ERROR) {
            return -1;
        }
    }

    if (mState < AUDIO_INPUT_STARTED) {
        mState = AUDIO_INPUT_STARTED;
        // force routing to input device
        mHardware->clearCurDevice();
        mHardware->doRouting();
        if (ioctl(mFd, AUDIO_START, 0)) {
            LOGE("Error starting record");
            standby();
            return -1;
        }
    }

    while (count) {
        ssize_t bytesRead = ::read(mFd, buffer, count);
        if (bytesRead >= 0) {
            count -= bytesRead;
            p += bytesRead;
        } else {
            if (errno != EAGAIN) return bytesRead;
            mRetryCount++;
            LOGW("EAGAIN - retrying");
        }
    }
    return bytes;
}

status_t AudioHardware::AudioStreamInMSM72xx::standby()
{
    if (mState > AUDIO_INPUT_CLOSED) {
        if (mFd >= 0) {
            ::close(mFd);
            mFd = -1;
        }
        mState = AUDIO_INPUT_CLOSED;
    }
    if (!mHardware) return -1;
    // restore output routing if necessary
    mHardware->clearCurDevice();
    mHardware->doRouting();
    return NO_ERROR;
}

status_t AudioHardware::AudioStreamInMSM72xx::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append("AudioStreamInMSM72xx::dump\n");
    snprintf(buffer, SIZE, "\tsample rate: %d\n", sampleRate());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tbuffer size: %d\n", bufferSize());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tchannels: %d\n", channels());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tformat: %d\n", format());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmHardware: %p\n", mHardware);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmFd count: %d\n", mFd);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmState: %d\n", mState);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmRetryCount: %d\n", mRetryCount);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

status_t AudioHardware::AudioStreamInMSM72xx::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting);
    status_t status = NO_ERROR;
    int device;
    LOGV("AudioStreamInMSM72xx::setParameters() %s", keyValuePairs.string());

    if (param.getInt(key, device) == NO_ERROR) {
        LOGV("set input routing %x", device);
        if (device & (device - 1)) {
            status = BAD_VALUE;
        } else {
            mDevices = device;
            status = mHardware->doRouting();
        }
        param.remove(key);
    }

    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}

String8 AudioHardware::AudioStreamInMSM72xx::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);

    if (param.get(key, value) == NO_ERROR) {
        LOGV("get routing %x", mDevices);
        param.addInt(key, (int)mDevices);
    }

    LOGV("AudioStreamInMSM72xx::getParameters() %s", param.toString().string());
    return param.toString();
}

// ----------------------------------------------------------------------------

extern "C" AudioHardwareInterface* createAudioHardware(void) {
    return new AudioHardware();
}

/****************************************************************************
 * Local Function definition
 ****************************************************************************/
static int set_initial_audio_volume(void) 
{
    /* Winmo call this while initializing the audio device */
    if ( SND_DEVICE_IDLE != -1) {
        set_volume_rpc(SND_DEVICE_IDLE, 1, 5);
        LOGV("Initial audio volume setting done");
    } else {
        LOGE("Can't set initial volume, SND_DEVICE_IDLE not defined");
        return -EIO;
    }
    return NO_ERROR;
}

static int get_master_volume(void) 
{
    float volume;
    AudioSystem::getMasterVolume(&volume);
    volume = ceil(volume * 5.0);
    return volume;
}

static int get_current_stream(void) 
{
    bool bStreamIsActive;
    int stream;
    
    for (stream = AudioSystem::VOICE_CALL; stream<AudioSystem::NUM_STREAM_TYPES; stream++) {
        AudioSystem::isStreamActive(stream, &bStreamIsActive);
        //LOGV("Stream %d status : %d", stream, bStreamIsActive);
        if ( bStreamIsActive ) {
            return stream;
        }
    }

    return AudioSystem::DEFAULT;
}

}; // namespace android

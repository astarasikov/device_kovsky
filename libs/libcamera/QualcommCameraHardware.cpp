/*
** Copyright 2008, Google Inc.
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

#define LOG_NDEBUG 0
//#define DEBUG_CFGCTRL
#define LOG_TAG "QualcommCameraHardware"
#include <utils/Log.h>

#include "QualcommCameraHardware.h"

#include <utils/threads.h>
#include <binder/MemoryHeapPmem.h>
#include <utils/String16.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#if HAVE_ANDROID_OS
#include <linux/android_pmem.h>
#endif
#include <linux/ioctl.h>

#define LIKELY(exp)   __builtin_expect(!!(exp), 1)
#define UNLIKELY(exp) __builtin_expect(!!(exp), 0)

extern "C" {

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
#include <stdlib.h>

#include <media/msm_camera.h>

#define DEFAULT_PICTURE_WIDTH  2048 // 1280
#define DEFAULT_PICTURE_HEIGHT 1536 // 768

#define DEFAULT_THUMBNAIL_SETTING 2
#define DEFAULT_PREVIEW_SETTING 2 // HVGA
#define PREVIEW_SIZE_COUNT (sizeof(preview_sizes)/sizeof(camera_size_type))

#define NOT_FOUND -1

/* TAG JB 01/20/2010 : Dual library support */
#define BASIC_LIB   0
#define NEW_LIB     1
static int liboemcamera_version = BASIC_LIB;

// Number of video buffers held by kernal (initially 1,2 &3)
// TODO : Check kernel video buffers
#define ACTIVE_VIDEO_BUFFERS 3

static int dstOffset = 0;   // Will be used for mdp zoom
/* End of TAG */

#if DLOPEN_LIBMMCAMERA
#include <dlfcn.h>

void* (*LINK_cam_conf)(void *data);
void* (*LINK_cam_frame)(void *data);
bool  (*LINK_jpeg_encoder_init)();
void  (*LINK_jpeg_encoder_join)();
bool  (*LINK_jpeg_encoder_encode)(const cam_ctrl_dimension_t *dimen,
                                  const uint8_t *thumbnailbuf, int thumbnailfd,
                                  const uint8_t *snapshotbuf, int snapshotfd,
                                  common_crop_t *scaling_parms, exif_tags_info_t *exif_data,
                                  int exif_table_numEntries, int jpegPadding);
/* TAG JB 01/20/2010 : Dual library support */
bool  (*LINK_jpeg_encoder_encode_basic)(const cam_ctrl_dimension_t_basic *dimen,
                                  const uint8_t *thumbnailbuf, int thumbnailfd,
                                  const uint8_t *snapshotbuf, int snapshotfd,
                                  common_crop_t *scaling_parms, exif_tags_info_t *exif_data,
                                  int exif_table_numEntries, int jpegPadding);
/* End of TAG */
int  (*LINK_camframe_terminate)(void);
int8_t (*LINK_jpeg_encoder_setMainImageQuality)(uint32_t quality);
int8_t (*LINK_jpeg_encoder_setThumbnailQuality)(uint32_t quality);
int8_t (*LINK_jpeg_encoder_setRotation)(uint32_t rotation);
int8_t (*LINK_jpeg_encoder_setLocation)(const camera_position_type *location);
const struct camera_size_type *(*LINK_default_sensor_get_snapshot_sizes)(int *len);
int (*LINK_launch_cam_conf_thread)(void);
int (*LINK_release_cam_conf_thread)(void);
// callbacks
void  (**LINK_mmcamera_camframe_callback)(struct msm_frame *frame);
void  (**LINK_mmcamera_jpegfragment_callback)(uint8_t *buff_ptr,
                                              uint32_t buff_size);
void  (**LINK_mmcamera_jpeg_callback)(jpeg_event_t status);
void  (**LINK_mmcamera_shutter_callback)();
/* TAG JB 02/24/2010 : New lib + Zoom */
void  (**LINK_mmcamera_shutter_callback_new)(common_crop_t *crop);
/* End of TAG */

/* TAG JB 01/20/2010 : Dual library support */

/* this LINK_xxx is used to detect the library compatibility.
 * If it can be linked, then the legend library is used (1.5Mb),
 * otherwise, it's the dream/sapphire library (307Kb)
 */
void  (**LINK_mt9p012_process_start)(uint32_t *buff_ptr);

/* Needed by the legend library. Don't know yet what should be done in it */
void  (**LINK_mmcamera_vfe_stop_ack_callback)(void* data);
/* End of TAG */
#ifdef DEBUG_CFGCTRL
char*   LINK_cfgctrl;
#endif

#else
#define LINK_cam_conf cam_conf
#define LINK_cam_frame cam_frame
#define LINK_jpeg_encoder_init jpeg_encoder_init
#define LINK_jpeg_encoder_join jpeg_encoder_join
#define LINK_jpeg_encoder_encode jpeg_encoder_encode
#define LINK_camframe_terminate camframe_terminate
#define LINK_jpeg_encoder_setMainImageQuality jpeg_encoder_setMainImageQuality
#define LINK_jpeg_encoder_setThumbnailQuality jpeg_encoder_setThumbnailQuality
#define LINK_jpeg_encoder_setRotation jpeg_encoder_setRotation
#define LINK_jpeg_encoder_setLocation jpeg_encoder_setLocation
#define LINK_default_sensor_get_snapshot_sizes default_sensor_get_snapshot_sizes
#define LINK_launch_cam_conf_thread launch_cam_conf_thread
#define LINK_release_cam_conf_thread release_cam_conf_thread
/* TAG JB 01/20/2010 : Dual library support */
#define LINK_mt9p012_process_start
/* End of TAG */
extern void (*mmcamera_camframe_callback)(struct msm_frame *frame);
extern void (*mmcamera_jpegfragment_callback)(uint8_t *buff_ptr,
                                      uint32_t buff_size);
extern void (*mmcamera_jpeg_callback)(jpeg_event_t status);
extern void (*mmcamera_shutter_callback)();
#endif

} // extern "C"

/* TAG JB 01/20/2010 : Dual library support */
typedef struct crop_info_struct {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} zoom_crop_info;
/* End of TAG */

/* TAG JB 01/21/2010 : Sensor dependant parameters */

struct camera_size_type {
    int width;
    int height;
};

static camera_size_type preview_sizes[] = {
    { 800, 480 }, // WVGA
    { 640, 480 }, // VGA
    { 480, 320 }, // HVGA
    { 384, 288 },
    { 352, 288 }, // CIF
    { 320, 240 }, // QVGA
    { 240, 160 }, // SQVGA
    { 176, 144 }, // QCIF
};

//static const camera_size_type* picture_sizes;
//static int PICTURE_SIZE_COUNT;
/*       TODO
 * Ideally this should be a populated by lower layers.
 * But currently this is no API to do that at lower layer.
 * Hence populating with default sizes for now. This needs
 * to be changed once the API is supported.
 */
//sorted on column basis
static const camera_size_type picture_sizes[] = {
    { 2592, 1944 }, // 5MP
    { 2048, 1536 }, // 3MP QXGA
    //{ 1920, 1080 }, //HD1080
    { 1600, 1200 }, // 2MP UXGA
    //{ 1280, 768 }, //WXGA
    //{ 1280, 720 }, //HD720
    { 1024, 768}, // 1MP XGA
    //{ 800, 600 }, //SVGA
    //{ 800, 480 }, // WVGA
    { 640, 480 }, // VGA
    //{ 352, 288 }, //CIF
    //{ 320, 240 }, // QVGA
    //{ 176, 144 } // QCIF
};
//for 3M chcek
static const camera_size_type picture_sizes_3m[] = {
    { 2048, 1536 }, // 3MP QXGA
    //{ 1920, 1080 }, //HD1080
    { 1600, 1200 }, // 2MP UXGA
    //{ 1280, 768 }, //WXGA
    //{ 1280, 720 }, //HD720
    { 1024, 768}, // 1MP XGA
    //{ 800, 600 }, //SVGA
    //{ 800, 480 }, // WVGA
    { 640, 480 }, // VGA
    //{ 352, 288 }, //CIF
    //{ 320, 240 }, // QVGA
    //{ 176, 144 } // QCIF
};
static int PICTURE_SIZE_COUNT = sizeof(picture_sizes)/sizeof(camera_size_type);
static int PICTURE_SIZE_COUNT_3M = sizeof(picture_sizes_3m)/sizeof(camera_size_type);
static const camera_size_type * picture_sizes_ptr;
static int supportedPictureSizesCount;

typedef struct {
    uint32_t aspect_ratio;
    uint32_t width;
    uint32_t height;
} thumbnail_size_type;

static thumbnail_size_type thumbnail_sizes[] = {
    { 7281, 512, 288 }, //1.777778
    { 6826, 480, 288 }, //1.666667
    { 6144, 432, 288 }, //1.5
    { 5461, 512, 384 }, //1.333333
    { 5006, 352, 288 }, //1.222222
};
#define THUMBNAIL_SIZE_COUNT (sizeof(thumbnail_sizes)/sizeof(thumbnail_size_type))

#define THUMBNAIL_WIDTH_STR "512"
#define THUMBNAIL_HEIGHT_STR "384"
#define THUMBNAIL_SMALL_HEIGHT 144
static camera_size_type jpeg_thumbnail_sizes[]  = {
    { 512, 288 },
    { 480, 288 },
    { 432, 288 },
    { 512, 384 },
    { 352, 288 },
    {0,0}
};

#define JPEG_THUMBNAIL_SIZE_COUNT (sizeof(jpeg_thumbnail_sizes)/sizeof(camera_size_type))

#ifdef Q12
#undef Q12
#endif

#define Q12 4096
/* End of TAG */

static int attr_lookup(const str_map arr[], int len, const char *name)
{
    if (name) {
        for (int i = 0; i < len; i++) {
            if (!strcmp(arr[i].desc, name))
                return arr[i].val;
        }
    }
    return NOT_FOUND;
}

#define INIT_VALUES_FOR(parm) do {                               \
    if (!parm##_values) {                                        \
        parm##_values = (char *)malloc(sizeof(parm)/             \
                                       sizeof(parm[0])*30);      \
        char *ptr = parm##_values;                               \
        const str_map *trav;                                     \
        for (trav = parm; trav->desc; trav++) {                  \
            int len = strlen(trav->desc);                        \
            strcpy(ptr, trav->desc);                             \
            ptr += len;                                          \
            *ptr++ = ',';                                        \
        }                                                        \
        *--ptr = 0;                                              \
    }                                                            \
} while(0)

// round to the next power of two
static inline unsigned clp2(unsigned x)
{
    x = x - 1;
    x = x | (x >> 1);
    x = x | (x >> 2);
    x = x | (x >> 4);
    x = x | (x >> 8);
    x = x | (x >>16);
    return x + 1;
}

static int exif_table_numEntries = 0;
#define MAX_EXIF_TABLE_ENTRIES 11
exif_tags_info_t exif_data[MAX_EXIF_TABLE_ENTRIES];

namespace android {

static Mutex singleton_lock;
static bool singleton_releasing;
static nsecs_t singleton_releasing_start_time;
static const nsecs_t SINGLETON_RELEASING_WAIT_TIME = seconds_to_nanoseconds(5);
static const nsecs_t SINGLETON_RELEASING_RECHECK_TIMEOUT = seconds_to_nanoseconds(1);
static Condition singleton_wait;

static void receive_camframe_callback(struct msm_frame *frame);
static void receive_jpeg_fragment_callback(uint8_t *buff_ptr, uint32_t buff_size);
static void receive_jpeg_callback(jpeg_event_t status);
static void receive_shutter_callback();
/* TAG JB 02/24/2010 : New lib + Zoom */
static void receive_shutter_callback_new(common_crop_t *crop);
/* End of TAG */

/* TAG JB 01/21/2010 : Sensor dependant parameters */

// from aeecamera.h
static const str_map whitebalance[] = {
    { CameraParameters::WHITE_BALANCE_AUTO,            CAMERA_WB_AUTO },
    { CameraParameters::WHITE_BALANCE_INCANDESCENT,    CAMERA_WB_INCANDESCENT },
    { CameraParameters::WHITE_BALANCE_FLUORESCENT,     CAMERA_WB_FLUORESCENT },
    { CameraParameters::WHITE_BALANCE_DAYLIGHT,        CAMERA_WB_DAYLIGHT },
    { CameraParameters::WHITE_BALANCE_CLOUDY_DAYLIGHT, CAMERA_WB_CLOUDY_DAYLIGHT },
    { CameraParameters::WHITE_BALANCE_TWILIGHT,        CAMERA_WB_TWILIGHT },
    { CameraParameters::WHITE_BALANCE_SHADE,           CAMERA_WB_SHADE },
};

// from camera_effect_t. This list must match aeecamera.h
static const str_map effects[] = {
    { CameraParameters::EFFECT_NONE,       CAMERA_EFFECT_OFF },
    { CameraParameters::EFFECT_MONO,       CAMERA_EFFECT_MONO },
    { CameraParameters::EFFECT_NEGATIVE,   CAMERA_EFFECT_NEGATIVE },
    { CameraParameters::EFFECT_SOLARIZE,   CAMERA_EFFECT_SOLARIZE },
    { CameraParameters::EFFECT_SEPIA,      CAMERA_EFFECT_SEPIA },
    { CameraParameters::EFFECT_POSTERIZE,  CAMERA_EFFECT_POSTERIZE },
    { CameraParameters::EFFECT_WHITEBOARD, CAMERA_EFFECT_WHITEBOARD },
    { CameraParameters::EFFECT_BLACKBOARD, CAMERA_EFFECT_BLACKBOARD },
    { CameraParameters::EFFECT_AQUA,       CAMERA_EFFECT_AQUA }
};

// from qcamera/common/camera.h
static const str_map antibanding[] = {
    { CameraParameters::ANTIBANDING_OFF,  CAMERA_ANTIBANDING_OFF },
    { CameraParameters::ANTIBANDING_50HZ, CAMERA_ANTIBANDING_50HZ },
    { CameraParameters::ANTIBANDING_60HZ, CAMERA_ANTIBANDING_60HZ },
    { CameraParameters::ANTIBANDING_AUTO, CAMERA_ANTIBANDING_AUTO }
};

static const str_map antibanding_3m[] = {
    { CameraParameters::ANTIBANDING_OFF,  CAMERA_ANTIBANDING_OFF },
    { CameraParameters::ANTIBANDING_50HZ, CAMERA_ANTIBANDING_50HZ },
    { CameraParameters::ANTIBANDING_60HZ, CAMERA_ANTIBANDING_60HZ },
    //{ CameraParameters::ANTIBANDING_AUTO, CAMERA_ANTIBANDING_AUTO }
};

// from camera.h, led_mode_t
static const str_map flash[] = {
    { CameraParameters::FLASH_MODE_OFF,  LED_MODE_OFF },
    { CameraParameters::FLASH_MODE_AUTO, LED_MODE_AUTO },
    { CameraParameters::FLASH_MODE_ON, LED_MODE_ON },
/*
    { CameraParameters::FLASH_MODE_TORCH, LED_MODE_TORCH}
*/
};

static const str_map focus_modes[] = {
    { CameraParameters::FOCUS_MODE_AUTO,     AF_MODE_AUTO},
    //{ CameraParameters::FOCUS_MODE_INFINITY, DONT_CARE },
    //{ CameraParameters::FOCUS_MODE_NORMAL,   AF_MODE_NORMAL },
    //{ CameraParameters::FOCUS_MODE_MACRO,    AF_MODE_MACRO }
};

struct SensorType {
    const char *name;
    int rawPictureWidth;
    int rawPictureHeight;
    bool hasAutoFocusSupport;
    int max_supported_snapshot_width;
    int max_supported_snapshot_height;
    int bitMask;
};

static SensorType sensorTypes[] = {
        { "5mp", 2608, 1960, true,  2592, 1944,0x00000fff },
        { "3mp", 2064, 1544, false, 2048, 1536,0x000007ff },
        { "2mp", 3200, 1200, false, 1600, 1200,0x000007ff }
};

static SensorType * sensorType;

static bool parameter_string_initialized = false;
static String8 preview_size_values;
static String8 picture_size_values;
static String8 antibanding_values;
static String8 effect_values;
static String8 whitebalance_values;
static String8 flash_values;
static String8 focus_mode_values;
static String8 zoom_ratio_values;

static String8 create_sizes_str(const camera_size_type *sizes, int len) {
    String8 str;
    char buffer[32];

    if (len > 0) {
        sprintf(buffer, "%dx%d", sizes[0].width, sizes[0].height);
        str.append(buffer);
    }
    for (int i = 1; i < len; i++) {
        sprintf(buffer, ",%dx%d", sizes[i].width, sizes[i].height);
        str.append(buffer);
    }
    return str;
}

static String8 create_values_str(const str_map *values, int len) {
    String8 str;

    if (len > 0) {
        str.append(values[0].desc);
    }
    for (int i = 1; i < len; i++) {
        str.append(",");
        str.append(values[i].desc);
    }
    return str;
}

static String8 create_str(int16_t *arr, int length){
    String8 str;
    char buffer[32];

    if(length > 0){
        snprintf(buffer, sizeof(buffer), "%d", arr[0]);
        str.append(buffer);
    }

    for (int i =1;i<length;i++){
        snprintf(buffer, sizeof(buffer), ",%d",arr[i]);
        str.append(buffer);
    }
    return str;
}

/* end of TAG */

/* TAG JB 01/21/2010 : Zoom */
static int32_t mMaxZoom = 0;
static bool zoomSupported = false;
static bool native_get_maxzoom(int camfd, void *pZm);
/* End of TAG */

/* TAG JB 01/21/2010 : enhancement */
static int camerafd;
pthread_t w_thread;
/* End of TAG */

/* TAG JB 01/20/2010 : Dual library support */
static void receive_vfe_stop_ack_callback(void* data);

static void cam_ctrl_dimension_convert(
        cam_ctrl_dimension_t mDimensionIn, cam_ctrl_dimension_t_basic mDimensionOut){
    mDimensionOut.picture_width = mDimensionIn.picture_width;
    mDimensionOut.picture_height = mDimensionIn.picture_height;
    mDimensionOut.display_width = mDimensionIn.display_width;
    mDimensionOut.display_height = mDimensionIn.display_height;
    mDimensionOut.orig_picture_dx = mDimensionIn.orig_picture_dx;
    mDimensionOut.orig_picture_dy = mDimensionIn.orig_picture_dy;
    mDimensionOut.ui_thumbnail_width = mDimensionIn.ui_thumbnail_width;
    mDimensionOut.ui_thumbnail_height = mDimensionIn.ui_thumbnail_height;
    mDimensionOut.thumbnail_width = mDimensionIn.thumbnail_width;
    mDimensionOut.thumbnail_height = mDimensionIn.thumbnail_height;
    mDimensionOut.raw_picture_height = mDimensionIn.raw_picture_height;
    mDimensionOut.raw_picture_width = mDimensionIn.raw_picture_width;
    mDimensionOut.filler7 = mDimensionIn.filler7;
    mDimensionOut.filler8 = mDimensionIn.filler8;
}
/* End of TAG */

void *opencamerafd(void *data) {
    camerafd = open(MSM_CAMERA_CONTROL, O_RDWR);
    return NULL;
}

QualcommCameraHardware::QualcommCameraHardware()
    : mParameters(),
      mPreviewHeight(-1),
      mPreviewWidth(-1),
      mRawHeight(-1),
      mRawWidth(-1),
      mCameraRunning(false),
      mPreviewInitialized(false),
      mFrameThreadRunning(false),
      mSnapshotThreadRunning(false),
      mReleasedRecordingFrame(false),
      mNotifyCb(0),
      mDataCb(0),
      mDataCbTimestamp(0),
      mCallbackCookie(0),
      mMsgEnabled(0),
      mPreviewFrameSize(0),
      mRawSize(0),
      mCameraControlFd(-1),
      mAutoFocusThreadRunning(false),
      mAutoFocusFd(-1),
      mInPreviewCallback(false),
      mCameraRecording(false)
{
    /* TAG JB 01/21/2010 : Enhancements */
    // Start opening camera device in a separate thread/ Since this
    // initializes the sensor hardware, this can take a long time. So,
    // start the process here so it will be ready by the time it's
    // needed.
    if ((pthread_create(&w_thread, NULL, opencamerafd, NULL)) != 0) {
        LOGE("Camera open thread creation failed");
    }
    /* End of TAG */

    memset(&mDimension, 0, sizeof(mDimension));
    memset(&mCrop, 0, sizeof(mCrop));

    /* TAG JB 01/20/2010 : New memory management */
    kPreviewBufferCountActual = kPreviewBufferCount;
    /* End of TAG */
    LOGV("constructor EX");
}

//filter Picture sizes based on max width and height
void QualcommCameraHardware::filterPictureSizes(){
    int i;
    LOGE("filterPictureSizes enter~!!");

    if(!strcmp(mSensorInfo.name, "mt9t013"))
    {
        LOGE("filterPictureSizes MT9T013!!");
        for(i=0;i<PICTURE_SIZE_COUNT_3M;i++)
        {
            if(((picture_sizes_3m[i].width <=
                sensorType->max_supported_snapshot_width) &&
                (picture_sizes_3m[i].height <=
                sensorType->max_supported_snapshot_height)))
            {
                picture_sizes_ptr = picture_sizes_3m + i;
                supportedPictureSizesCount = PICTURE_SIZE_COUNT_3M - i  ;
                return ;
            }
        }
    } else if(!strcmp(mSensorInfo.name, "mt9p012")) {
        LOGE("filterPictureSizes MT9P012!!");
        for(i=0;i<PICTURE_SIZE_COUNT;i++)
        {
            if(((picture_sizes[i].width <=
                sensorType->max_supported_snapshot_width) &&
                (picture_sizes[i].height <=
                sensorType->max_supported_snapshot_height)))
            {
                picture_sizes_ptr = picture_sizes + i;
                supportedPictureSizesCount = PICTURE_SIZE_COUNT - i  ;
                return ;
            }
        }
    }
    LOGE("filterPictureSizes exit~!!");
}

void QualcommCameraHardware::initDefaultParameters()
{
    LOGV("initDefaultParameters E");

    /* TAG JB 01/21/2010 : Sensor dependant parameters */
    findSensorType();

    // Initialize constant parameter strings. This will happen only once in the
    // lifetime of the mediaserver process.
    if (!parameter_string_initialized)
    {
	    if(!strcmp(mSensorInfo.name, "mt9t013"))
       	{
            LOGV("mt9t013 sensor found\n");

            // Filter picture size based on sensor resolution
            filterPictureSizes();
            antibanding_values = create_values_str(
                antibanding_3m, sizeof(antibanding_3m) / sizeof(str_map));
            /* Flash supported modes are initalized but will only be activated if 
             * board camera declaration specifies that flash is present (see test bellow)
             */ 
            flash_values = create_values_str(
                flash, sizeof(flash) / sizeof(str_map));

            // Enable autofocus support
            sensorType->hasAutoFocusSupport = true;

            // TODO : retrieve zoom info from sensor
            parameter_string_initialized = true;

        } else if(!strcmp(mSensorInfo.name, "mt9p012")) {
            // Disable autofocus support
            //sensorType->hasAutoFocusSupport = false;
            filterPictureSizes();
            /* Flash supported modes are initalized but will only be activated if 
             * board camera declaration specifies that flash is present (see test bellow)
             */ 
            flash_values = create_values_str(
                flash, sizeof(flash) / sizeof(str_map));

            // TODO : retrieve zoom info from sensor
            parameter_string_initialized = true;

        } else {
            LOGV("Unknown sensor. Default parameters will be used\n");
            // TODO : initialize default parameters

            /* Picture sizes defaulted to 3M sensor type */
            picture_sizes_ptr = picture_sizes_3m;
            supportedPictureSizesCount = PICTURE_SIZE_COUNT_3M;
            /* Antibanding not supported */
            antibanding_values.append("off");

            parameter_string_initialized = true;
        }
    }

    /* Common parameters initialization (some of those parameters might be moved to sensor
     * specific initialization code if they don't support default value)
     */
    /* Setting available preview sizes */
    preview_size_values = create_sizes_str(
            preview_sizes, sizeof(preview_sizes) / sizeof(camera_size_type));
    LOGV("preview_size_values.string() = %s \n", preview_size_values.string());
    mParameters.set(CameraParameters::KEY_SUPPORTED_PREVIEW_SIZES,
                    preview_size_values.string());
    /* Setting available picture sizes */
    picture_size_values = create_sizes_str(
            picture_sizes_ptr, supportedPictureSizesCount);
    LOGV("picture_size_values.string() = %s \n", picture_size_values.string());
    mParameters.set(CameraParameters::KEY_SUPPORTED_PICTURE_SIZES,
                    picture_size_values.string());
    /* Setting available jpeg thumbnail sizes */
    String8 valuesStr = create_sizes_str(jpeg_thumbnail_sizes, JPEG_THUMBNAIL_SIZE_COUNT);
    mParameters.set(CameraParameters::KEY_SUPPORTED_JPEG_THUMBNAIL_SIZES,
                valuesStr.string());
    /* Setting available antibanding modes */
    LOGV("antibanding_values.string() = %s \n", antibanding_values.string());
    mParameters.set(CameraParameters::KEY_SUPPORTED_ANTIBANDING,
                    antibanding_values);
    /* Setting available whitebalance mode */
    whitebalance_values = create_values_str(
            whitebalance, sizeof(whitebalance) / sizeof(str_map));
    LOGV("whitebalance_values.string() = %s \n", whitebalance_values.string());
    mParameters.set(CameraParameters::KEY_SUPPORTED_WHITE_BALANCE,
                    whitebalance_values);
    /* Setting available effects */
    effect_values = create_values_str(
        effects, sizeof(effects) / sizeof(str_map));
    LOGV("effect_values.string() = %s \n", effect_values.string());
    mParameters.set(CameraParameters::KEY_SUPPORTED_EFFECTS,
                    effect_values);

    /* Setting autofocus modes if supported */
    // TODO : add support for auto focus in the driver
    if(sensorType->hasAutoFocusSupport){
        /* TODO : the focus modes might bze moved to the sensor dependant section */
        focus_mode_values = create_values_str(
                focus_modes, sizeof(focus_modes) / sizeof(str_map));
        mParameters.set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES,
                    focus_mode_values);
        mParameters.set(CameraParameters::KEY_FOCUS_MODE,
                    CameraParameters::FOCUS_MODE_AUTO);
    } else {
        mParameters.set(CameraParameters::KEY_FOCUS_MODE,
                    CameraParameters::FOCUS_MODE_INFINITY);
    }

    /* Flash only activated if board support it */
    if (mSensorInfo.flash_enabled) {
        LOGV("Flash sensor support enabled. Values = %s", 
                        flash_values.string());
        mParameters.set(CameraParameters::KEY_FLASH_MODE,
                        CameraParameters::FLASH_MODE_OFF);
        mParameters.set(CameraParameters::KEY_SUPPORTED_FLASH_MODES,
                        flash_values);
    }

    /* TAG JB 01/21/2010 : Zoom fix */
    /* Try to get maximal supported zoom. If this fails, then zoom is not available */
    if(native_get_maxzoom(mCameraControlFd, (void *)&mMaxZoom) == true){
        LOGD("Maximum zoom value is %d", mMaxZoom);
        zoomSupported = true;
        if((mMaxZoom > 0) && (liboemcamera_version != BASIC_LIB) ){
            //TODO : if max zoom is available find the zoom ratios
            int16_t * zoomRatios = new int16_t[mMaxZoom+1];
            for (int i=0; i<(mMaxZoom+1); i++) {    
                zoomRatios[i] = i*10;
                zoom_ratio_values = create_str(zoomRatios, mMaxZoom + 1);
            }
            delete zoomRatios;
        } else {
            zoomSupported = false;
            LOGE("Failed to get maximum zoom value...setting max "
                    "zoom to zero");
            mMaxZoom = 0;
        }
    } else {
        zoomSupported = false;
        LOGE("Failed to get maximum zoom value...setting max "
                "zoom to zero");
        mMaxZoom = 0;
    }

    /* Enable zoom if sensor supports it */
    if(zoomSupported){
        LOGV("zoom_ratio_values = %s\n", zoom_ratio_values.string());

        mParameters.set(CameraParameters::KEY_ZOOM_SUPPORTED, "true");       
        mParameters.set(CameraParameters::KEY_MAX_ZOOM, mMaxZoom);
        mParameters.set(CameraParameters::KEY_ZOOM_RATIOS, zoom_ratio_values);
        mParameters.set(CameraParameters::KEY_ZOOM, 0);
    } else {
        mParameters.set(CameraParameters::KEY_MAX_ZOOM, 0);
        mParameters.set(CameraParameters::KEY_ZOOM_SUPPORTED, "false");
    }
    /* End of TAG zoom */
    /* end of TAG */

    /* Initialize camera with default parameters */
    camera_size_type *ps = &preview_sizes[DEFAULT_PREVIEW_SETTING];
    mParameters.setPreviewSize(ps->width, ps->height);
    mParameters.setPreviewFrameRate(15);
    mParameters.setPreviewFormat("yuv420sp"); // informative
    mParameters.setPictureFormat("jpeg"); // informative

    mParameters.set(CameraParameters::KEY_JPEG_QUALITY, "100"); // maximum quality
    mParameters.set(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH,  THUMBNAIL_WIDTH_STR); // informative
    mParameters.set(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT, THUMBNAIL_HEIGHT_STR); // informative
    mParameters.set(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY, "90");

    mParameters.setPictureSize(DEFAULT_PICTURE_WIDTH, DEFAULT_PICTURE_HEIGHT);
    mParameters.set(CameraParameters::KEY_ANTIBANDING,   "off");
    mParameters.set(CameraParameters::KEY_EFFECT,        "none");
    mParameters.set(CameraParameters::KEY_WHITE_BALANCE, "auto");
   
    if (setParameters(mParameters) != NO_ERROR) {
        LOGE("Failed to set default parameters?!");
    }

/* Tag JB 02/25/2001 : zoom postview image fix */
    jpegPadding = 0;
/* End of TAG */

    LOGV("initDefaultParameters X");
}

/* TAG JB 01/21/2010 : Sensor dependant parameters */
void QualcommCameraHardware::findSensorType(){
    bool ret;
    mDimension.picture_width = DEFAULT_PICTURE_WIDTH;
    mDimension.picture_height = DEFAULT_PICTURE_HEIGHT;

    if ( liboemcamera_version == BASIC_LIB ) {  
         ret = native_set_parm(CAMERA_SET_PARM_DIMENSION,
                    sizeof(cam_ctrl_dimension_t_basic), &mDimension.picture_width);
    } else {
         ret = native_set_parm(CAMERA_SET_PARM_DIMENSION,
                        sizeof(cam_ctrl_dimension_t), &mDimension);
    }

    if (ret) {
        unsigned int i;
        for (i = 0; i < sizeof(sensorTypes) / sizeof(SensorType); i++) {
            if (sensorTypes[i].rawPictureHeight
                    == mDimension.raw_picture_height) {
                sensorType = sensorTypes + i;
                LOGV("findSensorType : sensorType = %s \n", sensorType->name);
                return;
            }
        }
    }
    //default to 5 mp
    sensorType = sensorTypes;
    LOGV("findSensorType : sensorType = %s \n", sensorType->name);
    return;
}
/* End of TAG */

void QualcommCameraHardware::setCallbacks(notify_callback notify_cb,
                                      data_callback data_cb,
                                      data_callback_timestamp data_cb_timestamp,
                                      void* user)
{
    Mutex::Autolock lock(mLock);
    mNotifyCb = notify_cb;
    mDataCb = data_cb;
    mDataCbTimestamp = data_cb_timestamp;
    mCallbackCookie = user;
}

void QualcommCameraHardware::enableMsgType(int32_t msgType)
{
    Mutex::Autolock lock(mLock);
    mMsgEnabled |= msgType;
}

void QualcommCameraHardware::disableMsgType(int32_t msgType)
{
    Mutex::Autolock lock(mLock);
    mMsgEnabled &= ~msgType;
}

bool QualcommCameraHardware::msgTypeEnabled(int32_t msgType)
{
    Mutex::Autolock lock(mLock);
    return (mMsgEnabled & msgType);
}


extern "C" int HAL_getNumberOfCameras()
{
    //return sizeof(sCameraInfo) / sizeof(sCameraInfo[0]);
    return 1;
}

extern "C" void HAL_getCameraInfo(int cameraId, struct CameraInfo* cameraInfo)
{
    static CameraInfo sCameraInfo[] = {
        {
            CAMERA_FACING_BACK,
            90,  /* orientation */
        }
    };
    memcpy(cameraInfo, &sCameraInfo[cameraId], sizeof(CameraInfo));
}

extern "C" sp<CameraHardwareInterface> HAL_openCameraHardware(int cameraId)
{
    LOGV("openCameraHardware: call createInstance");
    return QualcommCameraHardware::createInstance();
}

#define ROUND_TO_PAGE(x)  (((x)+0xfff)&~0xfff)

bool QualcommCameraHardware::startCamera()
{
    LOGV("startCamera E");
#if DLOPEN_LIBMMCAMERA
    libmmcamera = ::dlopen("liboemcamera.so", RTLD_NOW);
    LOGV("loading liboemcamera at %p", libmmcamera);
    if (!libmmcamera) {
        LOGE("FATAL ERROR: could not dlopen liboemcamera.so: %s", dlerror());
        return false;
    }

    /* TAG JB 01/20/2010 : Dual library support */
    /* See comment of LINK_mt9p012_process_start declaration */
    LOGV("Detecting liboemcamera \"version\"");
    LINK_mt9p012_process_start = NULL;
    *(void **)&LINK_mt9p012_process_start =
        ::dlsym(libmmcamera, "mt9p012_process_start");
    if ( LINK_mt9p012_process_start == NULL ) {
        liboemcamera_version = BASIC_LIB;
    } else {
        liboemcamera_version = NEW_LIB;
    }
    LOGV("liboemcamera type used : %s", (liboemcamera_version == NEW_LIB) ? "NEW_LIB" : "BASIC_LIB");

#ifdef DEBUG_CFGCTRL
    *(void **)&LINK_cfgctrl = 
        ::dlsym(libmmcamera, "cfgctrl");
    if ( LINK_cfgctrl == NULL ) {
        LOGE("Error getting cfgctrl address");
    } else {
        LOGV("cfgctrl address = 0x%08x", LINK_cfgctrl);
    }
#endif
    /* End of TAG */

    *(void **)&LINK_cam_frame =
        ::dlsym(libmmcamera, "cam_frame");
    *(void **)&LINK_camframe_terminate =
        ::dlsym(libmmcamera, "camframe_terminate");

    *(void **)&LINK_jpeg_encoder_init =
        ::dlsym(libmmcamera, "jpeg_encoder_init");
    /* TAG JB 01/20/2010 : Dual library support */
    *(void **)&LINK_jpeg_encoder_encode_basic =
        ::dlsym(libmmcamera, "jpeg_encoder_encode");

    if ( liboemcamera_version == NEW_LIB ) {
        *(void **)&LINK_jpeg_encoder_encode =
            ::dlsym(libmmcamera, "jpeg_encoder_encode");
    }
    /* End of TAG */

    *(void **)&LINK_jpeg_encoder_join =
        ::dlsym(libmmcamera, "jpeg_encoder_join");

    *(void **)&LINK_mmcamera_camframe_callback =
        ::dlsym(libmmcamera, "mmcamera_camframe_callback");

    *LINK_mmcamera_camframe_callback = receive_camframe_callback;

    *(void **)&LINK_mmcamera_jpegfragment_callback =
        ::dlsym(libmmcamera, "mmcamera_jpegfragment_callback");

    *LINK_mmcamera_jpegfragment_callback = receive_jpeg_fragment_callback;

    *(void **)&LINK_mmcamera_jpeg_callback =
        ::dlsym(libmmcamera, "mmcamera_jpeg_callback");

    *LINK_mmcamera_jpeg_callback = receive_jpeg_callback;

    if ( liboemcamera_version == NEW_LIB ) {
        *(void **)&LINK_mmcamera_shutter_callback_new =
            ::dlsym(libmmcamera, "mmcamera_shutter_callback");
        *LINK_mmcamera_shutter_callback_new = receive_shutter_callback_new;
    } else {
        *(void **)&LINK_mmcamera_shutter_callback =
            ::dlsym(libmmcamera, "mmcamera_shutter_callback");
        *LINK_mmcamera_shutter_callback = receive_shutter_callback;
    }

    *(void**)&LINK_jpeg_encoder_setMainImageQuality =
        ::dlsym(libmmcamera, "jpeg_encoder_setMainImageQuality");

    *(void**)&LINK_jpeg_encoder_setThumbnailQuality =
        ::dlsym(libmmcamera, "jpeg_encoder_setThumbnailQuality");

    *(void**)&LINK_jpeg_encoder_setRotation =
        ::dlsym(libmmcamera, "jpeg_encoder_setRotation");

    *(void**)&LINK_jpeg_encoder_setLocation =
        ::dlsym(libmmcamera, "jpeg_encoder_setLocation");

    *(void **)&LINK_cam_conf =
        ::dlsym(libmmcamera, "cam_conf");

/* Disabling until support is available.
    *(void **)&LINK_default_sensor_get_snapshot_sizes =
        ::dlsym(libmmcamera, "default_sensor_get_snapshot_sizes");
*/

    /* TAG JB 01/20/2010 : Dual library support */
    if ( liboemcamera_version == NEW_LIB ) {
        *(void **)&LINK_mmcamera_vfe_stop_ack_callback =
            ::dlsym(libmmcamera, "mmcamera_vfe_stop_ack_callback");
        *LINK_mmcamera_vfe_stop_ack_callback = receive_vfe_stop_ack_callback;
    }
    /* End of TAG */


    /* TAG JB 01/21/2010 : Enhancements */
    *(void **)&LINK_launch_cam_conf_thread =
        ::dlsym(libmmcamera, "launch_cam_conf_thread");

    *(void **)&LINK_release_cam_conf_thread =
        ::dlsym(libmmcamera, "release_cam_conf_thread");
    /* End of tag */
    
#else
    mmcamera_camframe_callback = receive_camframe_callback;
    mmcamera_jpegfragment_callback = receive_jpeg_fragment_callback;
    mmcamera_jpeg_callback = receive_jpeg_callback;
    mmcamera_shutter_callback = receive_shutter_callback;
#endif // DLOPEN_LIBMMCAMERA

    /* TAG JB 01/21/2010 : Enhancements */
    /* The control thread is in libcamera itself. */
    if (pthread_join(w_thread, NULL) != 0) {
        LOGE("Camera open thread exit failed");
        return false;
    }
    mCameraControlFd = camerafd;
    /* End of TAG */

    if (mCameraControlFd < 0) {
        LOGE("startCamera X: %s open failed: %s!",
             MSM_CAMERA_CONTROL,
             strerror(errno));
        return false;
    }

    /* TAG JB 01/21/2010 : Enhancements */
    /* This will block until the control thread is launched. After that, sensor
     * information becomes available.
     */

    if (LINK_launch_cam_conf_thread()) {
        LOGE("failed to launch the camera config thread");
        return false;
    }
    /* End of TAG */

    /* TAG JB 01/21/2010 : Sensor dependant parameters */
    memset(&mSensorInfo, 0, sizeof(mSensorInfo));
    if (ioctl(mCameraControlFd,
              MSM_CAM_IOCTL_GET_SENSOR_INFO,
              &mSensorInfo) < 0)
        LOGW("%s: cannot retrieve sensor info!", __FUNCTION__);
    else
        LOGI("%s: camsensor name %s, flash %d", __FUNCTION__,
             mSensorInfo.name, mSensorInfo.flash_enabled);
    /* End of TAG */

    /* Disabling until support is available. */
#if 0
    // TODO : Check it
    if ( liboemcamera_version == NEW_LIB ) {
        picture_sizes_test = LINK_default_sensor_get_snapshot_sizes(&PICTURE_SIZE_COUNT);
        if (!picture_sizes_test || !PICTURE_SIZE_COUNT) {
            LOGE("startCamera X: could not get snapshot sizes");
            return false;
        }
    }
#endif

    LOGV("startCamera X");
    return true;
}

status_t QualcommCameraHardware::dump(int fd,
                                      const Vector<String16>& args) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    // Dump internal primitives.
    result.append("QualcommCameraHardware::dump");
    snprintf(buffer, 255, "preview width(%d) x height (%d)\n",
             mPreviewWidth, mPreviewHeight);
    result.append(buffer);
    snprintf(buffer, 255, "raw width(%d) x height (%d)\n",
             mRawWidth, mRawHeight);
    result.append(buffer);
    snprintf(buffer, 255,
             "preview frame size(%d), raw size (%d), jpeg size (%d) "
             "and jpeg max size (%d)\n", mPreviewFrameSize, mRawSize,
             mJpegSize, mJpegMaxSize);
    result.append(buffer);
    write(fd, result.string(), result.size());

    // Dump internal objects.
    if (mPreviewHeap != 0) {
        mPreviewHeap->dump(fd, args);
    }
    if (mRawHeap != 0) {
        mRawHeap->dump(fd, args);
    }
    if (mJpegHeap != 0) {
        mJpegHeap->dump(fd, args);
    }
    mParameters.dump(fd, args);
    return NO_ERROR;
}

static bool native_get_maxzoom(int camfd, void *pZm)
{
    LOGV("native_get_maxzoom E");

    struct msm_ctrl_cmd ctrlCmd;
    int32_t *pZoom = (int32_t *)pZm;

    ctrlCmd.type       = CAMERA_GET_PARM_MAXZOOM;
    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.length     = sizeof(int32_t);
    ctrlCmd.value      = pZoom;
    ctrlCmd.resp_fd    = camfd;

    if (ioctl(camfd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd) < 0) {
        LOGE("native_get_maxzoom: ioctl fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }
    LOGD("ctrlCmd.value = %d", *(int32_t *)ctrlCmd.value);
    memcpy(pZoom, (int32_t *)ctrlCmd.value, sizeof(int32_t));

    LOGV("native_get_maxzoom X");
    return true;
}

static bool native_set_afmode(int camfd, isp3a_af_mode_t af_type)
{
    int rc;
    struct msm_ctrl_cmd ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type = CAMERA_SET_PARM_AUTO_FOCUS;
    ctrlCmd.length = sizeof(af_type);
    ctrlCmd.value = &af_type;
    ctrlCmd.resp_fd = camfd; // FIXME: this will be put in by the kernel

    if ((rc = ioctl(camfd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd)) < 0)
        LOGE("native_set_afmode: ioctl fd %d error %s\n",
             camfd,
             strerror(errno));

    LOGV("native_set_afmode: ctrlCmd.status == %d\n", ctrlCmd.status);
    return rc >= 0 && ctrlCmd.status == CAMERA_EXIT_CB_DONE;
}

static bool native_cancel_afmode(int camfd, int af_fd)
{
    int rc;
    struct msm_ctrl_cmd ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type = CAMERA_AUTO_FOCUS_CANCEL;
    ctrlCmd.length = 0;
    ctrlCmd.resp_fd = af_fd;

    if ((rc = ioctl(camfd, MSM_CAM_IOCTL_CTRL_COMMAND_2, &ctrlCmd)) < 0)
        LOGE("native_cancel_afmode: ioctl fd %d error %s\n",
             camfd,
             strerror(errno));
    return rc >= 0;
}

static bool native_start_preview(int camfd)
{
    struct msm_ctrl_cmd ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type       = CAMERA_START_PREVIEW;
    ctrlCmd.length     = 0;
    ctrlCmd.resp_fd    = camfd; // FIXME: this will be put in by the kernel

    if (ioctl(camfd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd) < 0) {
        LOGE("native_start_preview: MSM_CAM_IOCTL_CTRL_COMMAND fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }

    return true;
}

static bool native_get_picture (int camfd, common_crop_t *crop)
{
    struct msm_ctrl_cmd ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.length     = sizeof(common_crop_t);
    ctrlCmd.value      = crop;

    if(ioctl(camfd, MSM_CAM_IOCTL_GET_PICTURE, &ctrlCmd) < 0) {
        LOGE("native_get_picture: MSM_CAM_IOCTL_GET_PICTURE fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }

    LOGV("crop: in1_w %d", crop->in1_w);
    LOGV("crop: in1_h %d", crop->in1_h);
    LOGV("crop: out1_w %d", crop->out1_w);
    LOGV("crop: out1_h %d", crop->out1_h);

    LOGV("crop: in2_w %d", crop->in2_w);
    LOGV("crop: in2_h %d", crop->in2_h);
    LOGV("crop: out2_w %d", crop->out2_w);
    LOGV("crop: out2_h %d", crop->out2_h);

    LOGV("crop: update %d", crop->update_flag);


    return true;
}

static bool native_stop_preview(int camfd)
{
    struct msm_ctrl_cmd ctrlCmd;
    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type       = CAMERA_STOP_PREVIEW;
    ctrlCmd.length     = 0;
    ctrlCmd.resp_fd    = camfd; // FIXME: this will be put in by the kernel

    if(ioctl(camfd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd) < 0) {
        LOGE("native_stop_preview: ioctl fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }

    return true;
}

static bool native_start_snapshot(int camfd)
{
    struct msm_ctrl_cmd ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type       = CAMERA_START_SNAPSHOT;
    ctrlCmd.length     = 0;
    ctrlCmd.resp_fd    = camfd; // FIXME: this will be put in by the kernel

    if(ioctl(camfd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd) < 0) {
        LOGE("native_start_snapshot: ioctl fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }

    return true;
}

static bool native_stop_snapshot (int camfd)
{
    struct msm_ctrl_cmd ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type       = CAMERA_STOP_SNAPSHOT;
    ctrlCmd.length     = 0;
    ctrlCmd.resp_fd    = camfd; // FIXME: this will be put in by the kernel

    if (ioctl(camfd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd) < 0) {
        LOGE("native_stop_snapshot: ioctl fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }

    return true;
}

bool QualcommCameraHardware::native_jpeg_encode(void)
{
    int jpeg_quality = mParameters.getInt("jpeg-quality");
    if (jpeg_quality >= 0) {
        LOGV("native_jpeg_encode, current jpeg main img quality =%d",
             jpeg_quality);
        if(!LINK_jpeg_encoder_setMainImageQuality(jpeg_quality)) {
            LOGE("native_jpeg_encode set jpeg-quality failed");
            return false;
        }
    }

    int thumbnail_quality = mParameters.getInt("jpeg-thumbnail-quality");
    if (thumbnail_quality >= 0) {
        LOGV("native_jpeg_encode, current jpeg thumbnail quality =%d",
             thumbnail_quality);
        if(!LINK_jpeg_encoder_setThumbnailQuality(thumbnail_quality)) {
            LOGE("native_jpeg_encode set thumbnail-quality failed");
            return false;
        }
    }

    int rotation = mParameters.getInt("rotation");
    if (rotation >= 0) {
        LOGV("native_jpeg_encode, rotation = %d", rotation);
        if(!LINK_jpeg_encoder_setRotation(rotation)) {
            LOGE("native_jpeg_encode set rotation failed");
            return false;
        }
    }

    jpeg_set_location();

/* TAG JB 01/20/2010 : Dual library support */
    uint8_t * thumbnailHeap = NULL;
    int thumbfd = -1;
    
    int width = mParameters.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH);
    int height = mParameters.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT);

    LOGV("width %d and height %d", width , height);

    if(width != 0 && height != 0){
        thumbnailHeap = (uint8_t *)mThumbnailHeap->mHeap->base();
        thumbfd =  mThumbnailHeap->mHeap->getHeapID();
    }else {
        thumbnailHeap = NULL;
        thumbfd = 0;
    }

    if ( liboemcamera_version == BASIC_LIB ) {
        cam_ctrl_dimension_t_basic mDimensionBasic;
        cam_ctrl_dimension_convert(mDimension, mDimensionBasic);
        if (!LINK_jpeg_encoder_encode_basic(&mDimensionBasic,
                                      thumbnailHeap,
                                      thumbfd,
                                      (uint8_t *)mRawHeap->mHeap->base(),
                                      mRawHeap->mHeap->getHeapID(),
                                      &mCrop, exif_data, exif_table_numEntries,
                                      jpegPadding/2)) {
            LOGE("native_jpeg_encode: jpeg_encoder_encode failed.");
            return false;
        }    
    } else {
        if (!LINK_jpeg_encoder_encode(&mDimension,
                                      thumbnailHeap,
                                      thumbfd,
                                      (uint8_t *)mRawHeap->mHeap->base(),
                                      mRawHeap->mHeap->getHeapID(),
                                      &mCrop, exif_data, exif_table_numEntries,
                                      jpegPadding/2)) {
            LOGE("native_jpeg_encode: jpeg_encoder_encode failed.");
            return false;
        }
    }
/* End of TAG */
    return true;
}

bool QualcommCameraHardware::native_set_dimension(cam_ctrl_dimension_t *value)
{
    bool ret;
    if ( liboemcamera_version == BASIC_LIB ) {
        LOGE("native_set_dimension: length: %d.", sizeof(cam_ctrl_dimension_t_basic));
        ret = native_set_parm(CAMERA_SET_PARM_DIMENSION,
                    sizeof(cam_ctrl_dimension_t_basic), &mDimension.picture_width);
    } else {
        LOGE("native_set_dimension: length: %d.", sizeof(cam_ctrl_dimension_t));
        ret = native_set_parm(CAMERA_SET_PARM_DIMENSION,
                                   sizeof(cam_ctrl_dimension_t), &mDimension);
    }
    return ret;
}

bool QualcommCameraHardware::native_set_parm(
    cam_ctrl_type type, uint16_t length, void *value)
{
    int rc = true;
    struct msm_ctrl_cmd ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type       = (uint16_t)type;
    ctrlCmd.length     = length;
    // FIXME: this will be put in by the kernel
    ctrlCmd.resp_fd    = mCameraControlFd;
    ctrlCmd.value = value;

    LOGV("native_set_parm. camfd=%d, type=%d, length=%d",
         mCameraControlFd, type, length);
    rc = ioctl(mCameraControlFd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd);
    if(rc < 0 || ctrlCmd.status != CAM_CTRL_SUCCESS) {
        LOGE("ioctl error. camfd=%d, type=%d, length=%d, rc=%d, ctrlCmd.status=%d, %s",
             mCameraControlFd, type, length, rc, ctrlCmd.status, strerror(errno));
        return false;
    }
    return true;
}

void QualcommCameraHardware::jpeg_set_location()
{
    bool encode_location = true;
    camera_position_type pt;

#define PARSE_LOCATION(what,type,fmt,desc) do {                                \
        pt.what = 0;                                                           \
        const char *what##_str = mParameters.get("gps-"#what);                 \
        LOGV("GPS PARM %s --> [%s]", "gps-"#what, what##_str);                 \
        if (what##_str) {                                                      \
            type what = 0;                                                     \
            if (sscanf(what##_str, fmt, &what) == 1)                           \
                pt.what = what;                                                \
            else {                                                             \
                LOGE("GPS " #what " %s could not"                              \
                     " be parsed as a " #desc, what##_str);                    \
                encode_location = false;                                       \
            }                                                                  \
        }                                                                      \
        else {                                                                 \
            LOGV("GPS " #what " not specified: "                               \
                 "defaulting to zero in EXIF header.");                        \
            encode_location = false;                                           \
       }                                                                       \
    } while(0)

    PARSE_LOCATION(timestamp, long, "%ld", "long");
    if (!pt.timestamp) pt.timestamp = time(NULL);
    PARSE_LOCATION(altitude, short, "%hd", "short");
    PARSE_LOCATION(latitude, double, "%lf", "double float");
    PARSE_LOCATION(longitude, double, "%lf", "double float");

#undef PARSE_LOCATION

    if (encode_location) {
        LOGD("setting image location ALT %d LAT %lf LON %lf",
             pt.altitude, pt.latitude, pt.longitude);
        if (!LINK_jpeg_encoder_setLocation(&pt)) {
            LOGE("jpeg_set_location: LINK_jpeg_encoder_setLocation failed.");
        }
    }
    else LOGV("not setting image location");
}

void QualcommCameraHardware::runFrameThread(void *data)
{
    LOGV("runFrameThread E");

    int cnt;

#if DLOPEN_LIBMMCAMERA
    // We need to maintain a reference to liboemcamera.so for the duration of the
    // frame thread, because we do not know when it will exit relative to the
    // lifetime of this object.  We do not want to dlclose() liboemcamera while
    // LINK_cam_frame is still running.
    void *libhandle = ::dlopen("liboemcamera.so", RTLD_NOW);
    LOGV("FRAME: loading liboemcamera at %p", libhandle);
    if (!libhandle) {
        LOGE("FATAL ERROR: could not dlopen liboemcamera.so: %s", dlerror());
    }
    if (libhandle)
#endif
    {
        LINK_cam_frame(data);
    }

    mPreviewHeap.clear();

#if DLOPEN_LIBMMCAMERA
    if (libhandle) {
        ::dlclose(libhandle);
        LOGV("FRAME: dlclose(liboemcamera)");
    }
#endif

    mFrameThreadWaitLock.lock();
    mFrameThreadRunning = false;
    mFrameThreadWait.signal();
    mFrameThreadWaitLock.unlock();

    LOGV("runFrameThread X");
}

void *frame_thread(void *user)
{
    LOGD("frame_thread E");
    sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->runFrameThread(user);
    }
    else LOGW("not starting frame thread: the object went away!");
    LOGD("frame_thread X");
    return NULL;
}

bool QualcommCameraHardware::initPreview()
{
    // See comments in deinitPreview() for why we have to wait for the frame
    // thread here, and why we can't use pthread_join().
    mParameters.getPreviewSize(&mPreviewWidth, &mPreviewHeight);

    LOGI("initPreview E: preview size=%dx%d", mPreviewWidth, mPreviewHeight);
    LOGV("running custom built libcamera.so");
    mFrameThreadWaitLock.lock();
    while (mFrameThreadRunning) {
        LOGV("initPreview: waiting for old frame thread to complete.");
        mFrameThreadWait.wait(mFrameThreadWaitLock);
        LOGV("initPreview: old frame thread completed.");
    }
    mFrameThreadWaitLock.unlock();

    mSnapshotThreadWaitLock.lock();
    while (mSnapshotThreadRunning) {
        LOGV("initPreview: waiting for old snapshot thread to complete.");
        mSnapshotThreadWait.wait(mSnapshotThreadWaitLock);
        LOGV("initPreview: old snapshot thread completed.");
    }
    mSnapshotThreadWaitLock.unlock();

/* TAG JB 01/20/2010 : Memory allocation fails if not rounded to page size */
    int cnt = 0;
    mPreviewFrameSize = mPreviewWidth * mPreviewHeight * 3/2;
    int CbCrOffset = PAD_TO_WORD(mPreviewWidth * mPreviewHeight);
    dstOffset = 0;
    mPreviewHeap = new PmemPool("/dev/pmem_adsp",
                                MemoryHeapBase::READ_ONLY | MemoryHeapBase::NO_CACHING,
                                mCameraControlFd,
                                MSM_PMEM_OUTPUT2 /* MSM_PMEM_PREVIEW */,
                                mPreviewFrameSize,
                                kPreviewBufferCountActual,
                                mPreviewFrameSize,
                                CbCrOffset,
                                0,
                                "preview");
/* End of TAG */

    if (!mPreviewHeap->initialized()) {
        mPreviewHeap.clear();
        LOGE("initPreview X: could not initialize preview heap.");
        return false;
    }

    //mDimension.picture_width  = DEFAULT_PICTURE_WIDTH;
    //mDimension.picture_height = DEFAULT_PICTURE_HEIGHT;

    bool ret = native_set_dimension(&mDimension);

    if (ret) {
/* TAG JB 01/20/2010 : New memory allocation */
        for (cnt = 0; cnt < kPreviewBufferCount; cnt++) {
            frames[cnt].fd = mPreviewHeap->mHeap->getHeapID();
            frames[cnt].buffer =
                (uint32_t)mPreviewHeap->mHeap->base() + mPreviewHeap->mAlignedBufferSize * cnt;
            frames[cnt].y_off = 0;
            frames[cnt].cbcr_off = CbCrOffset;
            frames[cnt].path = MSM_FRAME_ENC;
        }
/* end of TAG */
        mFrameThreadWaitLock.lock();
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        mFrameThreadRunning = !pthread_create(&mFrameThread,
                                              &attr,
                                              frame_thread,
                                              &frames[kPreviewBufferCount-1]);
        ret = mFrameThreadRunning;
        mFrameThreadWaitLock.unlock();
    }

    LOGV("initPreview X: %d", ret);
    return ret;
}

void QualcommCameraHardware::deinitPreview(void)
{
    LOGI("deinitPreview E");

    // When we call deinitPreview(), we signal to the frame thread that it
    // needs to exit, but we DO NOT WAIT for it to complete here.  The problem
    // is that deinitPreview is sometimes called from the frame-thread's
    // callback, when the refcount on the Camera client reaches zero.  If we
    // called pthread_join(), we would deadlock.  So, we just call
    // LINK_camframe_terminate() in deinitPreview(), which makes sure that
    // after the preview callback returns, the camframe thread will exit.  We
    // could call pthread_join() in initPreview() to join the last frame
    // thread.  However, we would also have to call pthread_join() in release
    // as well, shortly before we destoy the object; this would cause the same
    // deadlock, since release(), like deinitPreview(), may also be called from
    // the frame-thread's callback.  This we have to make the frame thread
    // detached, and use a separate mechanism to wait for it to complete.

    if (LINK_camframe_terminate() < 0)
        LOGE("failed to stop the camframe thread: %s",
             strerror(errno));
    LOGI("deinitPreview X");
}

bool QualcommCameraHardware::initRaw(bool initJpegHeap)
{
    mParameters.getPictureSize(&mRawWidth, &mRawHeight);

    LOGV("initRaw E: picture size=%dx%d",
         mRawWidth, mRawHeight);

    /* TAG JB 01/20/2010 : New memory allocation routines + enhancement */
    int thumbnailBufferSize;

    //Thumbnail height should be smaller than Picture height
    if (mRawHeight > (int)thumbnail_sizes[DEFAULT_THUMBNAIL_SETTING].height){
        mDimension.ui_thumbnail_width =
                thumbnail_sizes[DEFAULT_THUMBNAIL_SETTING].width;
        mDimension.ui_thumbnail_height =
                thumbnail_sizes[DEFAULT_THUMBNAIL_SETTING].height;
        uint32_t pictureAspectRatio = (uint32_t)((mRawWidth * Q12) / mRawHeight);
        uint32_t i;
        for(i = 0; i < THUMBNAIL_SIZE_COUNT; i++ )
        {
            if(thumbnail_sizes[i].aspect_ratio == pictureAspectRatio)
            {
                mDimension.ui_thumbnail_width = thumbnail_sizes[i].width;
                mDimension.ui_thumbnail_height = thumbnail_sizes[i].height;
                break;
            }
        }
    }
    else{
        mDimension.ui_thumbnail_height = THUMBNAIL_SMALL_HEIGHT;
        mDimension.ui_thumbnail_width =
                (THUMBNAIL_SMALL_HEIGHT * mRawWidth)/ mRawHeight;
    }

    LOGV("Thumbnail Size Width %d Height %d",
            mDimension.ui_thumbnail_width,
            mDimension.ui_thumbnail_height);

    thumbnailBufferSize = mDimension.ui_thumbnail_width *
                          mDimension.ui_thumbnail_height * 3 / 2;
    int CbCrOffsetThumb = PAD_TO_WORD(mDimension.ui_thumbnail_width *
                          mDimension.ui_thumbnail_height);
    /* End of TAG */
    mDimension.picture_width   = mRawWidth;
    mDimension.picture_height  = mRawHeight;
    mRawSize = mRawWidth * mRawHeight * 3 / 2;
    mJpegMaxSize = mRawWidth * mRawHeight * 3 / 2;

    if(!native_set_dimension(&mDimension)) {
        LOGE("initRaw X: failed to set dimension");
        return false;
    }

    if (mJpegHeap != NULL) {
        LOGV("initRaw: clearing old mJpegHeap.");
        mJpegHeap.clear();
    }

    // Snapshot
/* TAG JB 01/20/2010 : New memory allocation routines */
    mRawSize = mRawWidth * mRawHeight * 3 / 2;
    int CbCrOffsetRaw = PAD_TO_WORD(mRawWidth * mRawHeight);

    mJpegMaxSize = mRawWidth * mRawHeight * 3 / 2;

    //For offline jpeg hw encoder, jpeg encoder will provide us the
    //required offsets and buffer size depending on the rotation.
    int yOffset = 0;
    LOGV("initRaw: initializing mRawHeap.");

    mRawHeap =
        new PmemPool("/dev/pmem_camera",
                     MemoryHeapBase::READ_ONLY | MemoryHeapBase::NO_CACHING,
                     mCameraControlFd,
                     MSM_PMEM_MAINIMG,
                     mJpegMaxSize,
                     kRawBufferCount,
                     mRawSize,
                     CbCrOffsetRaw,
                     yOffset,
                     "snapshot camera");

    if (!mRawHeap->initialized()) {
        LOGE("initRaw X failed with pmem_camera, trying with pmem_adsp");
        mRawHeap =
            new PmemPool("/dev/pmem_adsp",
                         MemoryHeapBase::READ_ONLY | MemoryHeapBase::NO_CACHING,
                         mCameraControlFd,
                         MSM_PMEM_MAINIMG,
                         mJpegMaxSize,
                         kRawBufferCount,
                         mRawSize,
                         CbCrOffsetRaw,
                         yOffset,
                         "snapshot camera");
        if (!mRawHeap->initialized()) {
            mRawHeap.clear();
            LOGE("initRaw X: error initializing mRawHeap");
            return false;
        }
    }
/* End of TAG */

    LOGV("do_mmap snapshot pbuf = %p, pmem_fd = %d",
         (uint8_t *)mRawHeap->mHeap->base(), mRawHeap->mHeap->getHeapID());

    // Jpeg

    if (initJpegHeap) {
        LOGV("initRaw: initializing mJpegHeap.");
/* TAG JB 01/20/2010 : New memory allocation routines */
        mJpegHeap =
            new AshmemPool(mJpegMaxSize,
                           kJpegBufferCount,
                           0, // we do not know how big the picture will be
                           "jpeg");
/* End of TAG */

        if (!mJpegHeap->initialized()) {
            mJpegHeap.clear();
            mRawHeap.clear();
            LOGE("initRaw X failed: error initializing mJpegHeap.");
            return false;
        }

        // Thumbnails
/* TAG JB 01/20/2010 : New memory allocation routines */
        mThumbnailHeap =
            new PmemPool("/dev/pmem_adsp",
                         MemoryHeapBase::READ_ONLY | MemoryHeapBase::NO_CACHING,
                         mCameraControlFd,
                         MSM_PMEM_THUMBNAIL,
                         thumbnailBufferSize,
                         1,
                         thumbnailBufferSize,
                         CbCrOffsetThumb,
                         0,
                         "thumbnail");
/* End of TAG */

        if (!mThumbnailHeap->initialized()) {
            mThumbnailHeap.clear();
            mJpegHeap.clear();
            mRawHeap.clear();
            LOGE("initRaw X failed: error initializing mThumbnailHeap.");
            return false;
        }
    }

    LOGV("initRaw X");
    return true;
}

void QualcommCameraHardware::deinitRaw()
{
    LOGV("deinitRaw E");

    mThumbnailHeap.clear();
    mJpegHeap.clear();
    mRawHeap.clear();
    mDisplayHeap.clear();

    LOGV("deinitRaw X");
}

void QualcommCameraHardware::release()
{
    LOGD("release E");
    Mutex::Autolock l(&mLock);

#if DLOPEN_LIBMMCAMERA
    if (libmmcamera == NULL) {
        LOGE("ERROR: multiple release!");
        return;
    }
#else
#warning "Cannot detect multiple release when not dlopen()ing liboemcamera!"
#endif

    int cnt, rc;
    struct msm_ctrl_cmd ctrlCmd;

    if (mCameraRunning) {
        if(mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {
            mRecordFrameLock.lock();
            mReleasedRecordingFrame = true;
            mRecordWait.signal();
            mRecordFrameLock.unlock();
        }
        stopPreviewInternal();
    }

    LINK_jpeg_encoder_join();
/* TAG JB 02/24/2011 : New lib + Zoom */
    if ( liboemcamera_version == NEW_LIB ) {
        Mutex::Autolock l (&mRawPictureHeapLock);
    }
/* Enod of TAG */
    deinitRaw();

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.length = 0;
    ctrlCmd.type = (uint16_t)CAMERA_EXIT;
    ctrlCmd.resp_fd = mCameraControlFd; // FIXME: this will be put in by the kernel
    if (ioctl(mCameraControlFd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd) < 0)
        LOGE("ioctl CAMERA_EXIT fd %d error %s",
             mCameraControlFd, strerror(errno));

    LINK_release_cam_conf_thread();
    close(mCameraControlFd);
    mCameraControlFd = -1;
#if DLOPEN_LIBMMCAMERA
    if (libmmcamera) {
        ::dlclose(libmmcamera);
        LOGV("dlclose(liboemcamera)");
        libmmcamera = NULL;
    }
#endif

    singleton_lock.lock();
    singleton_releasing = true;
    singleton_releasing_start_time = systemTime();
    singleton_lock.unlock();

    LOGD("release X");
}

QualcommCameraHardware::~QualcommCameraHardware()
{
    LOGD("~QualcommCameraHardware E");
    singleton_lock.lock();

    singleton.clear();
    singleton_releasing = false;
    singleton_releasing_start_time = 0;
    singleton_wait.signal();
    singleton_lock.unlock();
    LOGD("~QualcommCameraHardware X");
}

sp<IMemoryHeap> QualcommCameraHardware::getRawHeap() const
{
    LOGV("getRawHeap");
/* Tag JB 02/25/2001 : zoom postview image fix */
    return mDisplayHeap != NULL ? mDisplayHeap->mHeap : NULL;
/* End of TAG */
//    return mRawHeap != NULL ? mRawHeap->mHeap : NULL;
}

sp<IMemoryHeap> QualcommCameraHardware::getPreviewHeap() const
{
    LOGV("getPreviewHeap");
    return mPreviewHeap != NULL ? mPreviewHeap->mHeap : NULL;
}

status_t QualcommCameraHardware::startPreviewInternal()
{
    if(mCameraRunning) {
        LOGV("startPreview X: preview already running.");
        return NO_ERROR;
    }

    if (!mPreviewInitialized) {
        mPreviewInitialized = initPreview();
        if (!mPreviewInitialized) {
            LOGE("startPreview X initPreview failed.  Not starting preview.");
            return UNKNOWN_ERROR;
        }
    }

    mCameraRunning = native_start_preview(mCameraControlFd);
    if(!mCameraRunning) {
        deinitPreview();
        mPreviewInitialized = false;
        LOGE("startPreview X: native_start_preview failed!");
        return UNKNOWN_ERROR;
    }

    LOGV("startPreview X");
    return NO_ERROR;
}

status_t QualcommCameraHardware::startPreview()
{
    LOGV("startPreview E");
    Mutex::Autolock l(&mLock);

    return startPreviewInternal();
}

void QualcommCameraHardware::stopPreviewInternal()
{
    LOGV("stopPreviewInternal E: %d", mCameraRunning);
    if (mCameraRunning) {
        // Cancel auto focus.
        if (mMsgEnabled & CAMERA_MSG_FOCUS) {
            LOGV("canceling autofocus");
            cancelAutoFocus();
        }

        LOGV("Stopping preview");
        mCameraRunning = !native_stop_preview(mCameraControlFd);
        if (!mCameraRunning && mPreviewInitialized) {
            deinitPreview();
            mPreviewInitialized = false;
        }
        else LOGE("stopPreviewInternal: failed to stop preview");
    }
    LOGV("stopPreviewInternal X: %d", mCameraRunning);
}

void QualcommCameraHardware::stopPreview()
{
    LOGV("stopPreview: E");
    Mutex::Autolock l(&mLock);

    if(mMsgEnabled & CAMERA_MSG_VIDEO_FRAME)
           return;

    stopPreviewInternal();

    LOGV("stopPreview: X");
}

void QualcommCameraHardware::runAutoFocus()
{
    mAutoFocusThreadLock.lock();
    mAutoFocusFd = open(MSM_CAMERA_CONTROL, O_RDWR);
    if (mAutoFocusFd < 0) {
        LOGE("autofocus: cannot open %s: %s",
             MSM_CAMERA_CONTROL,
             strerror(errno));
        mAutoFocusThreadRunning = false;
        mAutoFocusThreadLock.unlock();
        return;
    }

#if DLOPEN_LIBMMCAMERA
    // We need to maintain a reference to liboemcamera.so for the duration of the
    // AF thread, because we do not know when it will exit relative to the
    // lifetime of this object.  We do not want to dlclose() liboemcamera while
    // LINK_cam_frame is still running.
    void *libhandle = ::dlopen("liboemcamera.so", RTLD_NOW);
    LOGV("AF: loading liboemcamera at %p", libhandle);
    if (!libhandle) {
        LOGE("FATAL ERROR: could not dlopen liboemcamera.so: %s", dlerror());
        close(mAutoFocusFd);
        mAutoFocusFd = -1;
        mAutoFocusThreadRunning = false;
        mAutoFocusThreadLock.unlock();
        return;
    }
#endif

    /* This will block until either AF completes or is cancelled. */
    LOGV("af start (fd %d)", mAutoFocusFd);
    bool status = native_set_afmode(mAutoFocusFd, AF_MODE_AUTO);
    LOGV("af done: %d", (int)status);
    mAutoFocusThreadRunning = false;
    close(mAutoFocusFd);
    mAutoFocusFd = -1;
    mAutoFocusThreadLock.unlock();

    if (mMsgEnabled & CAMERA_MSG_FOCUS)
        mNotifyCb(CAMERA_MSG_FOCUS, status, 0, mCallbackCookie);

#if DLOPEN_LIBMMCAMERA
    if (libhandle) {
        ::dlclose(libhandle);
        LOGV("AF: dlclose(liboemcamera)");
    }
#endif
}

status_t QualcommCameraHardware::cancelAutoFocus()
{
    LOGV("cancelAutoFocus E");
    native_cancel_afmode(mCameraControlFd, mAutoFocusFd);
    LOGV("cancelAutoFocus X");

    /* Needed for eclair camera PAI */
    return NO_ERROR;
}

void *auto_focus_thread(void *user)
{
    LOGV("auto_focus_thread E");
    sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->runAutoFocus();
    }
    else LOGW("not starting autofocus: the object went away!");
    LOGV("auto_focus_thread X");
    return NULL;
}

status_t QualcommCameraHardware::autoFocus()
{
    LOGV("autoFocus E");
    Mutex::Autolock l(&mLock);

    if (mCameraControlFd < 0) {
        LOGE("not starting autofocus: main control fd %d", mCameraControlFd);
        return UNKNOWN_ERROR;
    }

    /* Not sure this is still needed with new APIs .. 
    if (mMsgEnabled & CAMERA_MSG_FOCUS) {
        LOGW("Auto focus is already in progress");
        return NO_ERROR;
        // No idea how to rewrite this
        //return mAutoFocusCallback == af_cb ? NO_ERROR : INVALID_OPERATION;
    }*/

    {
        mAutoFocusThreadLock.lock();
        if (!mAutoFocusThreadRunning) {
            // Create a detatched thread here so that we don't have to wait
            // for it when we cancel AF.
            pthread_t thr;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            mAutoFocusThreadRunning =
                !pthread_create(&thr, &attr,
                                auto_focus_thread, NULL);
            if (!mAutoFocusThreadRunning) {
                LOGE("failed to start autofocus thread");
                mAutoFocusThreadLock.unlock();
                return UNKNOWN_ERROR;
            }
        }
        mAutoFocusThreadLock.unlock();
    }

    LOGV("autoFocus X");
    return NO_ERROR;
}

void QualcommCameraHardware::runSnapshotThread(void *data)
{
    LOGV("runSnapshotThread E");
    if (native_start_snapshot(mCameraControlFd))
        receiveRawPicture();
    else
        LOGE("main: native_start_snapshot failed!");

    mSnapshotThreadWaitLock.lock();
    mSnapshotThreadRunning = false;
    mSnapshotThreadWait.signal();
    mSnapshotThreadWaitLock.unlock();

    LOGV("runSnapshotThread X");
}

void *snapshot_thread(void *user)
{
    LOGD("snapshot_thread E");
    sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->runSnapshotThread(user);
    }
    else LOGW("not starting snapshot thread: the object went away!");
    LOGD("snapshot_thread X");
    return NULL;
}

status_t QualcommCameraHardware::takePicture()
{
    LOGV("takePicture: E");
    Mutex::Autolock l(&mLock);

    // Wait for old snapshot thread to complete.
    mSnapshotThreadWaitLock.lock();
    while (mSnapshotThreadRunning) {
        LOGV("takePicture: waiting for old snapshot thread to complete.");
        mSnapshotThreadWait.wait(mSnapshotThreadWaitLock);
        LOGV("takePicture: old snapshot thread completed.");
    }

/* Tag JB 02/25/2001 : zoom postview image fix */
    mDisplayHeap = mThumbnailHeap;
/* End of TAG */

    stopPreviewInternal();

    if (!initRaw(mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE)) { /* not sure if this is right */
        LOGE("initRaw failed.  Not taking picture.");
        return UNKNOWN_ERROR;
    }

    mShutterLock.lock();
    mShutterPending = true;
    mShutterLock.unlock();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    mSnapshotThreadRunning = !pthread_create(&mSnapshotThread,
                                             &attr,
                                             snapshot_thread,
                                             NULL);
    mSnapshotThreadWaitLock.unlock();

    LOGV("takePicture: X");
    return mSnapshotThreadRunning ? NO_ERROR : UNKNOWN_ERROR;
}

status_t QualcommCameraHardware::cancelPicture()
{
    LOGV("cancelPicture: EX");

    return NO_ERROR;
}

status_t QualcommCameraHardware::
setParameters(const CameraParameters & params) {
	LOGE("setParameters: E params = %p", &params);

	Mutex::Autolock l(&mLock);
	/* TAG JB 01/21/2010 : Enhancement */
	status_t rc, final_rc = NO_ERROR;

	if ((rc = setPreviewSize(params))){
		final_rc = rc;
		LOGE("setPreviewSize rc %d", rc);
	}
	if ((rc = setPictureSize(params))){
		final_rc = rc;
		LOGE("setPictureSize rc %d", rc);
	}
	if ((rc = setJpegThumbnailSize(params))){
		final_rc = rc;
		LOGE("setJpegThumbnailSize rc %d", rc);
	}
	if ((rc = setAntibanding(params))){
		final_rc = rc;
		LOGE("setAntibanding rc %d", rc);
	}
	if ((rc = setEffect(params))){
		final_rc = rc;
		LOGE("setEffect rc %d", rc);
	}
	if ((rc = setWhiteBalance(params))){
		final_rc = rc;
		LOGE("setWhiteBalance rc %d", rc);
	}
	if ((rc = setFlash(params))){
		final_rc = rc;
		LOGE("setFlash rc %d", rc);
	}

	// FIXME: set nightshot and luma adaptatiom
	mParameters = params;

	/* TAG JB 01/21/2010 : Zoom */
	//setting zoom may fail but it is not critical
	if ((rc = setZoom(params))) {
		LOGE("setZoom rc %d", rc);
	}

	LOGE("setParameters: X");
	return final_rc;
}

CameraParameters QualcommCameraHardware::getParameters() const
{
    LOGV("getParameters: EX");
    return mParameters;
}

extern "C" sp<CameraHardwareInterface> openCameraHardware()
{
    LOGV("openCameraHardware: call createInstance");
    return QualcommCameraHardware::createInstance();
}

wp<QualcommCameraHardware> QualcommCameraHardware::singleton;

// If the hardware already exists, return a strong pointer to the current
// object. If not, create a new hardware object, put it in the singleton,
// and return it.
sp<CameraHardwareInterface> QualcommCameraHardware::createInstance()
{
    LOGD("createInstance: E");

    //Mutex::Autolock lock(&singleton_lock);
    singleton_lock.lock();

    // Wait until the previous release is done.
    while (singleton_releasing) {
        if((singleton_releasing_start_time != 0) &&
                (systemTime() - singleton_releasing_start_time) > SINGLETON_RELEASING_WAIT_TIME){
            LOGV("in createinstance system time is %lld %lld %lld ",
                    systemTime(), singleton_releasing_start_time, SINGLETON_RELEASING_WAIT_TIME);
            singleton_lock.unlock();
            LOGE("Previous singleton is busy and time out exceeded. Returning null");
            return NULL;
        }
        LOGI("Wait for previous release.");
        singleton_wait.waitRelative(singleton_lock, SINGLETON_RELEASING_RECHECK_TIMEOUT);
        LOGI("out of Wait for previous release.");
    }

    if (singleton != 0) {
        sp<CameraHardwareInterface> hardware = singleton.promote();
        if (hardware != 0) {
            LOGD("createInstance: X return existing hardware=%p", &(*hardware));
            singleton_lock.unlock();
            return hardware;
        }
    }

    {
        struct stat st;
        int rc = stat("/dev/oncrpc", &st);
        if (rc < 0) {
            LOGD("createInstance: X failed to create hardware: %s", strerror(errno));
            singleton_lock.unlock();
            return NULL;
        }
    }

    QualcommCameraHardware *cam = new QualcommCameraHardware();
    sp<QualcommCameraHardware> hardware(cam);
    singleton = hardware;

    if (!cam->startCamera()) {
        LOGE("%s: startCamera failed!", __FUNCTION__);
        singleton_lock.unlock();
        return NULL;
    }

    cam->initDefaultParameters();
    LOGD("createInstance: X created hardware=%p", &(*hardware));
    singleton_lock.unlock();
    return hardware;
}

// For internal use only, hence the strong pointer to the derived type.
sp<QualcommCameraHardware> QualcommCameraHardware::getInstance()
{
    sp<CameraHardwareInterface> hardware = singleton.promote();
    if (hardware != 0) {
        //    LOGV("getInstance: X old instance of hardware");
        return sp<QualcommCameraHardware>(static_cast<QualcommCameraHardware*>(hardware.get()));
    } else {
        LOGV("getInstance: X new instance of hardware");
        return sp<QualcommCameraHardware>();
    }
}

void QualcommCameraHardware::receivePreviewFrame(struct msm_frame *frame)
{
//    LOGV("receivePreviewFrame E");

    if (!mCameraRunning) {
        LOGE("ignoring preview callback--camera has been stopped");
        return;
    }

    // Why is this here?
    /*mCallbackLock.lock();
    preview_callback pcb = mPreviewCallback;
    void *pdata = mPreviewCallbackCookie;
    recording_callback rcb = mRecordingCallback;
    void *rdata = mRecordingCallbackCookie;
    mCallbackLock.unlock(); */

    // Find the offset within the heap of the current buffer.
    ssize_t offset =
        (ssize_t)frame->buffer - (ssize_t)mPreviewHeap->mHeap->base();
    offset /= mPreviewFrameSize;

    //LOGV("%d\n", offset);

    mInPreviewCallback = true;
    if (mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME)
        mDataCb(CAMERA_MSG_PREVIEW_FRAME, mPreviewHeap->mBuffers[offset], mCallbackCookie);

    if (mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {
        //Mutex::Autolock rLock(&mRecordFrameLock);
        mDataCbTimestamp(systemTime(), CAMERA_MSG_VIDEO_FRAME, mPreviewHeap->mBuffers[offset], mCallbackCookie); /* guess? */
        Mutex::Autolock rLock(&mRecordFrameLock);
        //mDataCb(CAMERA_MSG_VIDEO_FRAME, mPreviewHeap->mBuffers[offset], mCallbackCookie);

        if (mReleasedRecordingFrame != true) {
            LOGV("block for release frame request/command");
            mRecordWait.wait(mRecordFrameLock);
            LOGV("frame released, continuing");
        }
        mReleasedRecordingFrame = false;
    }

    /*if(mMsgEnabled & CAMERA_MSG_VIDEO_IMAGE) {
        Mutex::Autolock rLock(&mRecordFrameLock);
        rcb(systemTime(), mPreviewHeap->mBuffers[offset], rdata);
        if (mReleasedRecordingFrame != true) {
            LOGV("block for release frame request/command");
            mRecordWait.wait(mRecordFrameLock);
        }
        mReleasedRecordingFrame = false;
    }*/
    mInPreviewCallback = false;

//    LOGV("receivePreviewFrame X");
}

status_t QualcommCameraHardware::startRecording()
{
    LOGV("startRecording E");
    Mutex::Autolock l(&mLock);

    mReleasedRecordingFrame = false;
    mCameraRecording = true;

    return startPreviewInternal();
}

void QualcommCameraHardware::stopRecording()
{
    LOGV("stopRecording: E");
    Mutex::Autolock l(&mLock);

    {
        mRecordFrameLock.lock();
        mReleasedRecordingFrame = true;
        mRecordWait.signal();
        mRecordFrameLock.unlock();

        mCameraRecording = false;

        if(mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME) {
            LOGV("stopRecording: X, preview still in progress");
            return;
        }
    }

    stopPreviewInternal();
    LOGV("stopRecording: X");
}

void QualcommCameraHardware::releaseRecordingFrame(
       const sp<IMemory>& mem __attribute__((unused)))
{
    LOGV("releaseRecordingFrame E");
    Mutex::Autolock l(&mLock);
    Mutex::Autolock rLock(&mRecordFrameLock);
    mReleasedRecordingFrame = true;
    mRecordWait.signal();
    LOGV("releaseRecordingFrame X");
}

bool QualcommCameraHardware::recordingEnabled()
{
    return (mCameraRunning && mCameraRecording);
}

void QualcommCameraHardware::notifyShutter()
{
    /* make sure we get into the correct notifyShutter function */
    if ( liboemcamera_version == NEW_LIB ) {
        notifyShutter_new(&mCrop, FALSE);
    } else {
        mShutterLock.lock();
        if (mShutterPending && (mMsgEnabled & CAMERA_MSG_SHUTTER)) {
            mNotifyCb(CAMERA_MSG_SHUTTER, 0, 0, mCallbackCookie);
            mShutterPending = false;
        }
        mShutterLock.unlock();
    }
}

static void receive_shutter_callback()
{
    LOGV("receive_shutter_callback: E");
    sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->notifyShutter();
    }
    LOGV("receive_shutter_callback: X");
}

/* TAG 02/24/2011 : New lib + Zoom */
void QualcommCameraHardware::notifyShutter_new(common_crop_t *crop, bool mPlayShutterSoundOnly)
{
    mShutterLock.lock();
    image_rect_type size;

    LOGV("notifyShutter_new mPlayShutterSoundOnly %d, mShutterPending %d, mMsgEnabled & CAMERA_MSG_SHUTTER %d", 
            mPlayShutterSoundOnly, mShutterPending, mMsgEnabled & CAMERA_MSG_SHUTTER);
   
    if(mPlayShutterSoundOnly) {
        /* At this point, invoke Notify Callback to play shutter sound only.
         * We want to call notify callback again when we have the
         * yuv picture ready. This is to reduce blanking at the time
         * of displaying postview frame. Using ext2 to indicate whether
         * to play shutter sound only or register the postview buffers.
         */
        mNotifyCb(CAMERA_MSG_SHUTTER, 0, mPlayShutterSoundOnly,
                            mCallbackCookie);
        mShutterLock.unlock();
        return;
    }

    if (mShutterPending && mNotifyCb && (mMsgEnabled & CAMERA_MSG_SHUTTER)) {
        LOGV("out2_w=%d, out2_h=%d, in2_w=%d, in2_h=%d",
             crop->out2_w, crop->out2_h, crop->in2_w, crop->in2_h);
        LOGV("out1_w=%d, out1_h=%d, in1_w=%d, in1_h=%d",
             crop->out1_w, crop->out1_h, crop->in1_w, crop->in1_h);

        // To workaround a bug in MDP which happens if either
        // dimension > 2048, we display the thumbnail instead.

        mDisplayHeap = mRawHeap;

        if (crop->in1_w == 0 || crop->in1_h == 0) {
            // Full size
            size.width = mDimension.picture_width;
            size.height = mDimension.picture_height;
            if (size.width > 2048 || size.height > 2048) {
                size.width = mDimension.ui_thumbnail_width;
                size.height = mDimension.ui_thumbnail_height;
                mDisplayHeap = mThumbnailHeap;
            }
        } else {
            // Cropped
            size.width = (crop->in2_w + jpegPadding) & ~1;
            size.height = (crop->in2_h + jpegPadding) & ~1;
            if (size.width > 2048 || size.height > 2048) {
                size.width = (crop->in1_w + jpegPadding) & ~1;
                size.height = (crop->in1_h + jpegPadding) & ~1;
                mDisplayHeap = mThumbnailHeap;
            }
        }
        /* Now, invoke Notify Callback to unregister preview buffer
         * and register postview buffer with surface flinger. Set ext2
         * as 0 to indicate not to play shutter sound.
         */
        mNotifyCb(CAMERA_MSG_SHUTTER, (int32_t)&size, 0,
                        mCallbackCookie);
        mShutterPending = false;
    }
    mShutterLock.unlock();
}

static void receive_shutter_callback_new(common_crop_t *crop)
{
    LOGV("receive_shutter_callback_new: E");
    sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        /* Just play shutter sound at this time */
        obj->notifyShutter_new(crop, TRUE);
    }
    LOGV("receive_shutter_callback_new: X");
}


// Crop the picture in place.
static void crop_yuv420(uint32_t width, uint32_t height,
                 uint32_t cropped_width, uint32_t cropped_height,
                 uint8_t *image)
{
    uint32_t i, x, y;
    uint8_t* chroma_src, *chroma_dst;

    // Calculate the start position of the cropped area.
    x = (width - cropped_width) / 2;
    y = (height - cropped_height) / 2;
    x &= ~1;
    y &= ~1;

    // Copy luma component.
    for(i = 0; i < cropped_height; i++)
        memcpy(image + i * cropped_width,
               image + width * (y + i) + x,
               cropped_width);

    chroma_src = image + width * height;
    chroma_dst = image + cropped_width * cropped_height;

    // Copy chroma components.
    cropped_height /= 2;
    y /= 2;
    for(i = 0; i < cropped_height; i++)
        memcpy(chroma_dst + i * cropped_width,
               chroma_src + width * (y + i) + x,
               cropped_width);

}
/* End of TAG */

void QualcommCameraHardware::receiveRawPicture()
{
    LOGV("receiveRawPicture: E");

    if (mMsgEnabled & CAMERA_MSG_RAW_IMAGE) {
        if(native_get_picture(mCameraControlFd, &mCrop) == false) {
            LOGE("getPicture failed!");
            return;
        }

/* TAG JB 02/24/2011 : New lib + Zoom */
        if ( liboemcamera_version == NEW_LIB ) {
            mCrop.in1_w &= ~1;
            mCrop.in1_h &= ~1;
            mCrop.in2_w &= ~1;
            mCrop.in2_h &= ~1;

            LOGV("jpegPadding %d", jpegPadding);

            // Crop the image if zoomed.
            if (mCrop.in2_w != 0 && mCrop.in2_h != 0 &&
                    ((mCrop.in2_w + jpegPadding) < mCrop.out2_w) &&
                    ((mCrop.in2_h + jpegPadding) < mCrop.out2_h) &&
                    ((mCrop.in1_w + jpegPadding) < mCrop.out1_w)  &&
                    ((mCrop.in1_h + jpegPadding) < mCrop.out1_h) ) {
                LOGV("Image zoomed. Crop it");
                // By the time native_get_picture returns, picture is taken. Call
                // shutter callback if cam config thread has not done that.
                notifyShutter_new(&mCrop, FALSE);
                {
                    Mutex::Autolock l(&mRawPictureHeapLock);
                    if(mRawHeap != NULL)
                        crop_yuv420(mCrop.out2_w, mCrop.out2_h, (mCrop.in2_w + jpegPadding), (mCrop.in2_h + jpegPadding),
                                (uint8_t *)mRawHeap->mHeap->base());
                    if(mThumbnailHeap != NULL) {
                        crop_yuv420(mCrop.out1_w, mCrop.out1_h, (mCrop.in1_w + jpegPadding), (mCrop.in1_h + jpegPadding),
                                (uint8_t *)mThumbnailHeap->mHeap->base());
                    }
                }

                // We do not need jpeg encoder to upscale the image. Set the new
                // dimension for encoder.
                mDimension.orig_picture_dx = mCrop.in2_w + jpegPadding;
                mDimension.orig_picture_dy = mCrop.in2_h + jpegPadding;
                mDimension.thumbnail_width = mCrop.in1_w + jpegPadding;
                mDimension.thumbnail_height = mCrop.in1_h + jpegPadding;
                memset(&mCrop, 0, sizeof(mCrop));
            }else {
                LOGV("Image not zoomed. Don't crop it");
                memset(&mCrop, 0 ,sizeof(mCrop));
                // By the time native_get_picture returns, picture is taken. Call
                // shutter callback if cam config thread has not done that.
                notifyShutter_new(&mCrop, FALSE);
            }

            if (mDataCb && (mMsgEnabled & CAMERA_MSG_RAW_IMAGE)) {
                mDataCb(CAMERA_MSG_RAW_IMAGE, mRawHeap->mBuffers[0],
                    mCallbackCookie);
            }
        } else {
            // By the time native_get_picture returns, picture is taken. Call
            // shutter callback if cam config thread has not done that.
            notifyShutter();
            mDataCb(CAMERA_MSG_RAW_IMAGE, mRawHeap->mBuffers[0], mCallbackCookie);
        }
/* End of TAG */
    }
    else LOGV("Raw-picture callback was canceled--skipping.");

    if (mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE) {
        mJpegSize = 0;
        if (LINK_jpeg_encoder_init()) {
            if(native_jpeg_encode()) {
                LOGV("receiveRawPicture: X (success)");
                return;
            }
            LOGE("jpeg encoding failed");
        }
        else LOGE("receiveRawPicture X: jpeg_encoder_init failed.");
    }
    else LOGV("JPEG callback is NULL, not encoding image.");
    deinitRaw();
    LOGV("receiveRawPicture: X");
}

void QualcommCameraHardware::receiveJpegPictureFragment(
    uint8_t *buff_ptr, uint32_t buff_size)
{
    uint32_t remaining = mJpegHeap->mHeap->virtualSize();
    remaining -= mJpegSize;
    uint8_t *base = (uint8_t *)mJpegHeap->mHeap->base();

    LOGV("receiveJpegPictureFragment size %d", buff_size);
    if (buff_size > remaining) {
        LOGE("receiveJpegPictureFragment: size %d exceeds what "
             "remains in JPEG heap (%d), truncating",
             buff_size,
             remaining);
        buff_size = remaining;
    }
    memcpy(base + mJpegSize, buff_ptr, buff_size);
    mJpegSize += buff_size;
}

void QualcommCameraHardware::receiveJpegPicture(void)
{
    LOGV("receiveJpegPicture: E image (%d uint8_ts out of %d)",
         mJpegSize, mJpegHeap->mBufferSize);

    int index = 0, rc;

    if (mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE) {
        // The reason we do not allocate into mJpegHeap->mBuffers[offset] is
        // that the JPEG image's size will probably change from one snapshot
        // to the next, so we cannot reuse the MemoryBase object.
/* TAG JB 01/20/2010 : New memory allocation routines */
        sp<MemoryBase> buffer = new
            MemoryBase(mJpegHeap->mHeap,
                       index * mJpegHeap->mBufferSize +
                       0,
                       mJpegSize);
/* End of TAG */

        mDataCb(CAMERA_MSG_COMPRESSED_IMAGE, buffer, mCallbackCookie);
        buffer = NULL;
    }
    else LOGV("JPEG callback was cancelled--not delivering image.");

    LINK_jpeg_encoder_join();
    deinitRaw();
    LOGV("receiveJpegPicture: X callback done.");
}

bool QualcommCameraHardware::previewEnabled()
{
    Mutex::Autolock l(&mLock);
    return (mCameraRunning && (mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME));
}

/* TAG JB 01/21/2010 : Enhancement */ 
status_t QualcommCameraHardware::setPreviewSize(const CameraParameters& params)
{
    int width, height;
    params.getPreviewSize(&width, &height);
    LOGV("requested preview size %d x %d", width, height);

    // Validate the preview size
    for (size_t i = 0; i < sizeof(preview_sizes) / sizeof(camera_size_type); ++i) {
        if (width == preview_sizes[i].width
           && height == preview_sizes[i].height) {
            mDimension.display_width = width;
            mDimension.display_height= height;
            mParameters.setPreviewSize(width, height);
            return NO_ERROR;
        }
    }
    LOGE("Invalid preview size requested: %dx%d", width, height);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setJpegThumbnailSize(const CameraParameters& params){
    int width = params.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH);
    int height = params.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT);
    LOGV("requested jpeg thumbnail size %d x %d", width, height);

    // Validate the picture size
    for (unsigned int i = 0; i < JPEG_THUMBNAIL_SIZE_COUNT; ++i) {
       if (width == jpeg_thumbnail_sizes[i].width
         && height == jpeg_thumbnail_sizes[i].height) {
           mParameters.set(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH, width);
           mParameters.set(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT, height);
           return NO_ERROR;
       }
    }
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setPictureSize(const CameraParameters& params)
{
    int width, height;
    params.getPictureSize(&width, &height);
    LOGV("requested picture size %d x %d", width, height);

    // Validate the picture size
    for (int i = 0; i < supportedPictureSizesCount; ++i) {
        if (width == picture_sizes_ptr[i].width
          && height == picture_sizes_ptr[i].height) {
            mParameters.setPictureSize(width, height);
            mDimension.picture_width = width;
            mDimension.picture_height = height;
            return NO_ERROR;
        }
    }
    /* Dimension not among the ones in the list. Check if
     * its a valid dimension, if it is, then configure the
     * camera accordingly. else reject it.
     */
    if( isValidDimension(width, height) ) {
        mParameters.setPictureSize(width, height);
        mDimension.picture_width = width;
        mDimension.picture_height = height;
        return NO_ERROR;
    } else
        LOGE("Invalid picture size requested: %dx%d", width, height);
    return BAD_VALUE;
}

/* Tag JB 02/25/2001 : zoom postview image fix */
bool QualcommCameraHardware::isValidDimension(int width, int height) {
    bool retVal = FALSE;
    /* This function checks if a given resolution is valid or not.
     * A particular resolution is considered valid if it satisfies
     * the following conditions:
     * 1. width & height should be multiple of 16.
     * 2. width & height should be less than/equal to the dimensions
     *    supported by the camera sensor.
     * 3. the aspect ratio is a valid aspect ratio and is among the
     *    commonly used aspect ratio as determined by the thumbnail_sizes
     *    data structure.
     */

    if( ((unsigned int)width == CEILING16(width)) && ((unsigned int)height == CEILING16(height))
     && (width <= sensorType->max_supported_snapshot_width)
     && (height <= sensorType->max_supported_snapshot_height) )
    {
        uint32_t pictureAspectRatio = (uint32_t)((width * Q12)/height);
        for(uint32_t i = 0; i < THUMBNAIL_SIZE_COUNT; i++ ) {
            if(thumbnail_sizes[i].aspect_ratio == pictureAspectRatio) {
                retVal = TRUE;
                break;
            }
        }
    }
    return retVal;
}
/* end of TAG */

status_t QualcommCameraHardware::setEffect(const CameraParameters& params)
{
    const char *str_wb = mParameters.get(CameraParameters::KEY_WHITE_BALANCE);
    int32_t value_wb = attr_lookup(whitebalance, sizeof(whitebalance) / sizeof(str_map), str_wb);
    const char *str = params.get(CameraParameters::KEY_EFFECT);

	LOGV("setEffect");

    if (str != NULL) {
        int32_t value = attr_lookup(effects, sizeof(effects) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            if(!strcmp(sensorType->name, "2mp") && (value != CAMERA_EFFECT_OFF)
               &&(value != CAMERA_EFFECT_MONO) && (value != CAMERA_EFFECT_NEGATIVE)
               &&(value != CAMERA_EFFECT_SOLARIZE) && (value != CAMERA_EFFECT_SEPIA)) {
                LOGE("Special effect parameter is not supported for this sensor");
                return NO_ERROR;
            }

           if(((value == CAMERA_EFFECT_MONO) || (value == CAMERA_EFFECT_NEGATIVE)
           || (value == CAMERA_EFFECT_AQUA) || (value == CAMERA_EFFECT_SEPIA))
               && (value_wb != CAMERA_WB_AUTO)) {
               LOGE("Color Effect value will not be set " \
               "when the whitebalance selected is %s", str_wb);
               return NO_ERROR;
           }
           else {
		       LOGV("setEffect : CAMERA_SET_PARM_EFFECT");
               mParameters.set(CameraParameters::KEY_EFFECT, str);
               bool ret = native_set_parm(CAMERA_SET_PARM_EFFECT, sizeof(value),
                                           (void *)&value);
               return ret ? NO_ERROR : UNKNOWN_ERROR;
          }
        }
    }
    LOGE("Invalid effect value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setWhiteBalance(const CameraParameters& params)
{
    const char *str_effect = mParameters.get(CameraParameters::KEY_EFFECT);
    int32_t value_effect = attr_lookup(effects, sizeof(effects) / sizeof(str_map), str_effect);
    int32_t  value;

    if( (value_effect != CAMERA_EFFECT_MONO) && (value_effect != CAMERA_EFFECT_NEGATIVE)
    && (value_effect != CAMERA_EFFECT_AQUA) && (value_effect != CAMERA_EFFECT_SEPIA)) {
        const char *str = params.get(CameraParameters::KEY_WHITE_BALANCE);

        if (str != NULL) {
            value = attr_lookup(whitebalance, sizeof(whitebalance) / sizeof(str_map), str);

            if (value != NOT_FOUND) {
                mParameters.set(CameraParameters::KEY_WHITE_BALANCE, str);
                bool ret = native_set_parm(CAMERA_SET_PARM_WB, sizeof(value),
                                            (void *)&value);
                return ret ? NO_ERROR : UNKNOWN_ERROR;
            }
        }
        LOGE("Invalid whitebalance value: %s", (str == NULL) ? "NULL" : str);
        return BAD_VALUE;
    } else {
            LOGE("Whitebalance value will not be set " \
            "when the effect selected is %s", str_effect);
            return NO_ERROR;
    }
}

status_t QualcommCameraHardware::setFlash(const CameraParameters& params)
{
    if (!mSensorInfo.flash_enabled) {
        LOGV("%s: flash not supported", __FUNCTION__);
        return NO_ERROR;
    }

    LOGE("setFlash");
    const char *str = params.get(CameraParameters::KEY_FLASH_MODE);
    if (str != NULL) {
        int32_t value = attr_lookup(flash, sizeof(flash) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            mParameters.set(CameraParameters::KEY_FLASH_MODE, str);
			
            bool ret = native_set_parm(CAMERA_SET_PARM_LED_MODE,
                                       sizeof(value), (void *)&value);
            return ret ? NO_ERROR : UNKNOWN_ERROR;
            
            //return NO_ERROR;
        }
    }
    LOGE("Invalid flash mode value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setAntibanding(const CameraParameters& params)
{
    const char *str = params.get(CameraParameters::KEY_ANTIBANDING);
    if (str != NULL) {
        int value;
        if(!strcmp(sensorType->name, "3mp")) {
             value = (camera_antibanding_type)attr_lookup(
              antibanding_3m, sizeof(antibanding_3m) / sizeof(str_map), str);
        } else {
             value = (camera_antibanding_type)attr_lookup(
              antibanding, sizeof(antibanding) / sizeof(str_map), str);
        }

        LOGV("Antibanding value : %d", value);

        if (value != NOT_FOUND) {
            camera_antibanding_type temp = (camera_antibanding_type) value;
            mParameters.set(CameraParameters::KEY_ANTIBANDING, str);
            bool ret;
            LOGV("setAntibanding : CAMERA_SET_PARM_ANTIBANDING");
            ret = native_set_parm(CAMERA_SET_PARM_ANTIBANDING,
                            sizeof(camera_antibanding_type), (void *)&temp);
            return ret ? NO_ERROR : UNKNOWN_ERROR;
        }
    }
    LOGE("Invalid antibanding value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

/* TAG JB 01/21/2010 : Zoom */
status_t QualcommCameraHardware::setZoom(const CameraParameters& params)
{
    status_t rc = NO_ERROR;
    // No matter how many different zoom values the driver can provide, HAL
    // provides applictations the same number of zoom levels. The maximum driver
    // zoom value depends on sensor output (VFE input) and preview size (VFE
    // output) because VFE can only crop and cannot upscale. If the preview size
    // is bigger, the maximum zoom ratio is smaller. However, we want the
    // zoom ratio of each zoom level is always the same whatever the preview
    // size is. Ex: zoom level 1 is always 1.2x, zoom level 2 is 1.44x, etc. So,
    // we need to have a fixed maximum zoom value and do read it from the
    // driver.
    static const int ZOOM_STEP = 1;
    int32_t zoom_level = params.getInt("zoom");

    LOGV("Set zoom=%d", zoom_level);
    if(zoom_level >= 0 && zoom_level <= mMaxZoom) {
        mParameters.set("zoom", zoom_level);
        int32_t zoom_value = ZOOM_STEP * zoom_level;
        bool ret = native_set_parm(CAMERA_SET_PARM_ZOOM,
            sizeof(zoom_value), (void *)&zoom_value);
        rc = ret ? NO_ERROR : UNKNOWN_ERROR;
    } else {
        rc = BAD_VALUE;
    }

    return rc;
}
/* end of TAG */

/* TAG JB 01/20/2010 : Memory allocation fails if not rounded to page size
 * Using the new memory management as per found in codeaurora latest libcamera 2
 * (https://www.codeaurora.org/git/projects/qrd-gb-ssss-7225/repository/revisions/master/show/android/vendor/qcom/android-open/libcamera2)
 */
QualcommCameraHardware::MemPool::MemPool(int buffer_size, int num_buffers,
                                         int frame_size,
                                         const char *name) :
    mBufferSize(buffer_size),
    mNumBuffers(num_buffers),
    mFrameSize(frame_size),
    mBuffers(NULL), mName(name)
{
    int page_size_minus_1 = getpagesize() - 1;
    mAlignedBufferSize = (buffer_size + page_size_minus_1) & (~page_size_minus_1);
}

void QualcommCameraHardware::MemPool::completeInitialization()
{
    // If we do not know how big the frame will be, we wait to allocate
    // the buffers describing the individual frames until we do know their
    // size.

    if (mFrameSize > 0) {
        mBuffers = new sp<MemoryBase>[mNumBuffers];
        for (int i = 0; i < mNumBuffers; i++) {
            mBuffers[i] = new
                MemoryBase(mHeap,
                           i * mAlignedBufferSize,
                           mFrameSize);
        }
    }
}

QualcommCameraHardware::AshmemPool::AshmemPool(int buffer_size, int num_buffers,
                                               int frame_size,
                                               const char *name) :
    QualcommCameraHardware::MemPool(buffer_size,
                                    num_buffers,
                                    frame_size,
                                    name)
{
    LOGV("constructing MemPool %s backed by ashmem: "
         "%d frames @ %d uint8_ts, "
         "buffer size %d",
         mName,
         num_buffers, frame_size, buffer_size);

    int page_mask = getpagesize() - 1;
    int ashmem_size = buffer_size * num_buffers;
    ashmem_size += page_mask;
    ashmem_size &= ~page_mask;

    mHeap = new MemoryHeapBase(ashmem_size);

    completeInitialization();
}

static bool register_buf(int camfd,
                         int size,
                         int frame_size,
                         int cbcr_offset,
                         int yoffset,
                         int pmempreviewfd,
                         uint32_t offset,
                         uint8_t *buf,
                         int pmem_type,
                         bool vfe_can_write,
                         bool register_buffer = true);

QualcommCameraHardware::PmemPool::PmemPool(const char *pmem_pool,
                                           int flags,
                                           int camera_control_fd,
                                           int pmem_type,
                                           int buffer_size, int num_buffers,
                                           int frame_size, int cbcr_offset,
                                           int yOffset, const char *name) :
    QualcommCameraHardware::MemPool(buffer_size,
                                    num_buffers,
                                    frame_size,
                                    name),
    mPmemType(pmem_type),
    mCbCrOffset(cbcr_offset),
    myOffset(yOffset),
    mCameraControlFd(dup(camera_control_fd))
{
    LOGV("constructing MemPool %s backed by pmem pool %s: "
         "%d frames @ %d bytes, buffer size %d",
         mName,
         pmem_pool, num_buffers, frame_size,
         buffer_size);

    LOGV("%s: duplicating control fd %d --> %d",
         __FUNCTION__,
         camera_control_fd, mCameraControlFd);

    // Make a new mmap'ed heap that can be shared across processes.
    // mAlignedBufferSize is already in 4k aligned. (do we need total size necessary to be in power of 2??)
    mAlignedSize = mAlignedBufferSize * num_buffers;

    sp<MemoryHeapBase> masterHeap =
        new MemoryHeapBase(pmem_pool, mAlignedSize, flags);

    if (masterHeap->getHeapID() < 0) {
        LOGE("failed to construct master heap for pmem pool %s", pmem_pool);
        masterHeap.clear();
        return;
    }

    sp<MemoryHeapPmem> pmemHeap = new MemoryHeapPmem(masterHeap, flags);
    if (pmemHeap->getHeapID() >= 0) {
        pmemHeap->slap();
        masterHeap.clear();
        mHeap = pmemHeap;
        pmemHeap.clear();

        mFd = mHeap->getHeapID();
        if (::ioctl(mFd, PMEM_GET_SIZE, &mSize)) {
            LOGE("pmem pool %s ioctl(PMEM_GET_SIZE) error %s (%d)",
                 pmem_pool,
                 ::strerror(errno), errno);
            mHeap.clear();
            return;
        }

        LOGV("pmem pool %s ioctl(fd = %d, PMEM_GET_SIZE) is %ld",
             pmem_pool,
             mFd,
             mSize.len);
        LOGD("mBufferSize=%d, mAlignedBufferSize=%d\n", mBufferSize, mAlignedBufferSize);
        // Unregister preview buffers with the camera drivers.  Allow the VFE to write
        // to all preview buffers except for the last one.
        // Only Register the preview, snapshot and thumbnail buffers with the kernel.
        if( (strcmp("postview", mName) != 0) ){
            int num_buf = num_buffers;
            if(!strcmp("preview", mName)) num_buf = kPreviewBufferCount;
            LOGD("num_buffers = %d", num_buf);
            for (int cnt = 0; cnt < num_buf; ++cnt) {
                int active = 1;
                if(pmem_type == MSM_PMEM_OUTPUT1 /* MSM_PMEM_VIDEO */){
                     active = (cnt<ACTIVE_VIDEO_BUFFERS);
                     LOGV(" pmempool creating video buffers : active %d ", active);
                }
                else if (pmem_type == MSM_PMEM_OUTPUT2 /* MSM_PMEM_PREVIEW */){
                     active = (cnt < (num_buf-1));
                }
                register_buf(mCameraControlFd,
                         mBufferSize,
                         mFrameSize, mCbCrOffset, myOffset,
                         mHeap->getHeapID(),
                         mAlignedBufferSize * cnt,
                         (uint8_t *)mHeap->base() + mAlignedBufferSize * cnt,
                         pmem_type,
                         active);
            }
        }

        completeInitialization();
    }
    else LOGE("pmem pool %s error: could not create master heap!",
              pmem_pool);
}

QualcommCameraHardware::PmemPool::~PmemPool()
{
    LOGV("%s: %s E", __FUNCTION__, mName);
    if (mHeap != NULL) {
        // Unregister preview buffers with the camera drivers.
        //  Only Unregister the preview, snapshot and thumbnail
        //  buffers with the kernel.
        if( (strcmp("postview", mName) != 0) ){
            int num_buffers = mNumBuffers;
            if(!strcmp("preview", mName)) num_buffers = kPreviewBufferCount;
            for (int cnt = 0; cnt < num_buffers; ++cnt) {
                register_buf(mCameraControlFd,
                         mBufferSize,
                         mFrameSize,
                         mCbCrOffset,
                         myOffset,
                         mHeap->getHeapID(),
                         mAlignedBufferSize * cnt,
                         (uint8_t *)mHeap->base() + mAlignedBufferSize * cnt,
                         mPmemType,
                         false,
                         false /* unregister */);
            }
        }
    }
    LOGV("destroying PmemPool %s: closing control fd %d",
         mName,
         mCameraControlFd);
    close(mCameraControlFd);
    LOGV("%s: %s X", __FUNCTION__, mName);
}

QualcommCameraHardware::MemPool::~MemPool()
{
    LOGV("destroying MemPool %s", mName);
    if (mFrameSize > 0)
        delete [] mBuffers;
    mHeap.clear();
    LOGV("destroying MemPool %s completed", mName);
}

static bool register_buf(int camfd,
                         int size,
                         int frame_size,
                         int cbcr_offset,
                         int yoffset,
                         int pmempreviewfd,
                         uint32_t offset,
                         uint8_t *buf,
                         int pmem_type,
                         bool vfe_can_write,
                         bool register_buffer)
{
    struct msm_pmem_info pmemBuf;

    pmemBuf.type     = pmem_type;
    pmemBuf.fd       = pmempreviewfd;
    pmemBuf.offset   = offset;
    pmemBuf.len      = size;
    pmemBuf.vaddr    = buf;
    pmemBuf.y_off    = yoffset;
    pmemBuf.cbcr_off = cbcr_offset;

    pmemBuf.vfe_can_write   = vfe_can_write;

    LOGV("register_buf: camfd = %d, reg = %d buffer = %p, vfe_can_write = %d",
         camfd, !register_buffer, buf, vfe_can_write);
    if (ioctl(camfd,
              register_buffer ?
              MSM_CAM_IOCTL_REGISTER_PMEM :
              MSM_CAM_IOCTL_UNREGISTER_PMEM,
              &pmemBuf) < 0) {
        LOGE("register_buf: MSM_CAM_IOCTL_(UN)REGISTER_PMEM fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }
    return true;
}

status_t QualcommCameraHardware::MemPool::dump(int fd, const Vector<String16>& args) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    snprintf(buffer, 255, "QualcommCameraHardware::AshmemPool::dump\n");
    result.append(buffer);
    if (mName) {
        snprintf(buffer, 255, "mem pool name (%s)\n", mName);
        result.append(buffer);
    }
    if (mHeap != 0) {
        snprintf(buffer, 255, "heap base(%p), size(%d), flags(%d), device(%s)\n",
                 mHeap->getBase(), mHeap->getSize(),
                 mHeap->getFlags(), mHeap->getDevice());
        result.append(buffer);
    }
    snprintf(buffer, 255,
             "buffer size (%d), number of buffers (%d), frame size(%d)",
             mBufferSize, mNumBuffers, mFrameSize);
    result.append(buffer);
    write(fd, result.string(), result.size());
    return NO_ERROR;
}
/* End of TAG */

static void receive_camframe_callback(struct msm_frame *frame)
{
    sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->receivePreviewFrame(frame);
    }
}

static void receive_jpeg_fragment_callback(uint8_t *buff_ptr, uint32_t buff_size)
{
    LOGV("receive_jpeg_fragment_callback E");
    sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->receiveJpegPictureFragment(buff_ptr, buff_size);
    }
    LOGV("receive_jpeg_fragment_callback X");
}

static void receive_jpeg_callback(jpeg_event_t status)
{
    LOGV("receive_jpeg_callback E (completion status %d)", status);
    if (status == JPEG_EVENT_DONE) {
        sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
        if (obj != 0) {
            obj->receiveJpegPicture();
        }
    }
    LOGV("receive_jpeg_callback X");
}

status_t QualcommCameraHardware::sendCommand(int32_t command, int32_t arg1,
                                             int32_t arg2)
{
    LOGV("sendCommand: EX");
    return BAD_VALUE;
}

/* TAG JB 01/20/2010 : Dual library support */
static void receive_vfe_stop_ack_callback(void* data)
{
    // TODO : What should we do here ?? 
    LOGV("receive_vfe_stop_ack_callback EX");
}
/* End of TAG */

}; // namespace android

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

#ifndef ANDROID_HARDWARE_QUALCOMM_CAMERA_HARDWARE_H
#define ANDROID_HARDWARE_QUALCOMM_CAMERA_HARDWARE_H

#include <camera/CameraHardwareInterface.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <utils/threads.h>
#include <stdint.h>

extern "C" {
#include <linux/android_pmem.h>
#include <media/msm_camera.h>
}

#define MSM_CAMERA_CONTROL "/dev/msm_camera/control0"
#define JPEG_EVENT_DONE 0 /* guess */

#define TRUE 1
#define FALSE 0

#define CAM_CTRL_SUCCESS 1

/* TAG JB 01/20/2010 : From the disassembly of both drem/sapphire + legend camera libraries */
#define CAMERA_SET_PARM_DIMENSION           1
#define CAMERA_SET_PARM_ZOOM                2
#define CAMERA_SET_PARM_SENSOR_POSITION     3   // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_FOCUS_RECT          4   // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_LUMA_ADAPTATION     5   // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_CONTRAST            6
#define CAMERA_SET_PARM_BRIGHTNESS          7
#define CAMERA_SET_PARM_EXPOSURE_COMPENSATION   8   // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_SHARPNESS           9   // (4) from liboemcamera.so disassembly
#define CAMERA_SET_PARM_HUE                 10  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_SATURATION          11
#define CAMERA_SET_PARM_EXPOSURE            12
#define CAMERA_SET_PARM_AUTO_FOCUS          13
#define CAMERA_SET_PARM_WB                  14
#define CAMERA_SET_PARM_EFFECT              15
#define CAMERA_SET_PARM_FPS                 16  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_FLASH               17  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_NIGHTSHOT_MODE      18  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_REFLECT             19  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_PREVIEW_MODE        20  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_ANTIBANDING         21
#define CAMERA_SET_PARM_RED_EYE_REDUCTION   22  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_FOCUS_STEP          23  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_EXPOSURE_METERING   24  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_AUTO_EXPOSURE_MODE  25  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_ISO                 26
#define CAMERA_SET_PARM_BESTSHOT_MODE       27  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_PREVIEW_FPS         29  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_AF_MODE             30  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_HISTOGRAM           31  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_FLASH_STATE         32  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_FRAME_TIMESTAMP     33  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_STROBE_FLASH        34  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_FPS_LIST            35  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_HJR                 36
#define CAMERA_SET_PARM_ROLLOFF             37
#define CAMERA_STOP_PREVIEW                 38
#define CAMERA_START_PREVIEW                39
#define CAMERA_START_SNAPSHOT               40
#define CAMERA_START_RAW_SNAPSHOT           41
#define CAMERA_STOP_SNAPSHOT                42
#define CAMERA_EXIT                         43
#define CAMERA_GET_PARM_ZOOM                46  // from liboemcamera.so (307Kb version) disassembly
#define CAMERA_GET_PARM_MAXZOOM             47
#define CAMERA_GET_PARM_AF_SHARPNESS        48  // from liboemcamera.so disassembly
#define CAMERA_SET_PARM_LED_MODE            49
#define CAMERA_SET_MOTION_ISO               50  // from liboemcamera.so disassembly
#define CAMERA_AUTO_FOCUS_CANCEL            51  // (38) from liboemcamera.so disassembly
#define CAMERA_GET_PARM_FOCUS_STEP          52  // from liboemcamera.so (1535Kb version) disassembly
#define CAMERA_ENABLE_AFD                   53  // from liboemcamera.so (1535Kb version) disassembly
#define CAMERA_PREPARE_SNAPSHOT             54
#define CAMERA_SET_PARM_COORDINATE          55  // from liboemcamera.so (1535Kb version) disassembly
#define CAMERA_SET_AWB_CALIBRATION          56  // from liboemcamera.so (1535Kb version) disassembly
#define CAMERA_SET_PARM_LA_MODE             57  // from liboemcamera.so (1535Kb version) disassembly
#define CAMERA_SET_PARM_AE_COORDINATE       58  // from liboemcamera.so (1535Kb version) disassembly
#define CAMERA_GET_PARM_FOCAL_LENGTH        59  // from liboemcamera.so (1535Kb version) disassembly
#define CAMERA_GET_PARM_HORIZONTAL_VIEW_ANGLE 60  // from liboemcamera.so (1535Kb version) disassembly
#define CAMERA_GET_PARM_VERTICAL_VIEW_ANGLE 61  // from liboemcamera.so (1535Kb version) disassembly
#define CAMERA_GET_PARM_ISO                 62
#define CAMERA_SET_PARM_FRONT_CAMERA_MODE   63  // from liboemcamera.so (1535Kb version) disassembly

#define CAMERA_START_VIDEO                  56
#define CAMERA_STOP_VIDEO                   57
#define CAMERA_START_RECORDING              58
#define CAMERA_STOP_RECORDING               59
/* End of TAG */

#define PAD_TO_WORD(x) ((x&1) ? x+1 : x)
#define AF_MODE_AUTO 2
//#define CAMERA_AUTO_FOCUS_CANCEL 1 //204

typedef enum
{
    CAMERA_WB_MIN_MINUS_1,
    CAMERA_WB_AUTO = 1,  /* This list must match aeecamera.h */
    CAMERA_WB_CUSTOM,
    CAMERA_WB_INCANDESCENT,
    CAMERA_WB_FLUORESCENT,
    CAMERA_WB_DAYLIGHT,
    CAMERA_WB_CLOUDY_DAYLIGHT,
    CAMERA_WB_TWILIGHT,
    CAMERA_WB_SHADE,
    CAMERA_WB_MAX_PLUS_1
} camera_wb_type;

typedef enum
{
    CAMERA_RSP_CB_SUCCESS,    /* Function is accepted         */
    CAMERA_EXIT_CB_DONE,      /* Function is executed         */
    CAMERA_EXIT_CB_FAILED,    /* Execution failed or rejected */
    CAMERA_EXIT_CB_DSP_IDLE,  /* DSP is in idle state         */
    CAMERA_EXIT_CB_DSP_ABORT, /* Abort due to DSP failure     */
    CAMERA_EXIT_CB_ABORT,     /* Function aborted             */
    CAMERA_EXIT_CB_ERROR,     /* Failed due to resource       */
    CAMERA_EVT_CB_FRAME,      /* Preview or video frame ready */
    CAMERA_EVT_CB_PICTURE,    /* Picture frame ready for multi-shot */
    CAMERA_STATUS_CB,         /* Status updated               */
    CAMERA_EXIT_CB_FILE_SIZE_EXCEEDED, /* Specified file size not achieved,
                                          encoded file written & returned anyway */
    CAMERA_EXIT_CB_BUFFER,    /* A buffer is returned         */
    CAMERA_EVT_CB_SNAPSHOT_DONE,/*  Snapshot updated               */
    CAMERA_CB_MAX
} camera_cb_type;

typedef enum
{
    CAMERA_ANTIBANDING_OFF,
    CAMERA_ANTIBANDING_60HZ,
    CAMERA_ANTIBANDING_50HZ,
    CAMERA_ANTIBANDING_AUTO,
    CAMERA_MAX_ANTIBANDING,
} camera_antibanding_type;

typedef enum
{
	LED_MODE_OFF,
	LED_MODE_ON,
	LED_MODE_AUTO,
} flash_led_type;

typedef struct
{
    uint32_t timestamp;  /* seconds since 1/6/1980          */
    double   latitude;   /* degrees, WGS ellipsoid */
    double   longitude;  /* degrees                */
    int16_t  altitude;   /* meters                          */
} camera_position_type;

typedef struct
{
    unsigned int in1_h;
    unsigned int out1_h;
    unsigned int in1_w;
    unsigned int out1_w;
    unsigned int in2_h;
    unsigned int out2_w;
    unsigned int in2_w;
    unsigned int out2_h;
#if 0
	unsigned int in1_w;
	unsigned int in1_h;
	unsigned int out1_w;
	unsigned int out1_h;
	unsigned int in2_w;
	unsigned int in2_h;
	unsigned int out2_w;
	unsigned int out2_h;
#endif
    uint8_t update_flag; 
} common_crop_t;

/* TAG JB 01/20/2010 : Dual library support */
typedef unsigned int exif_tag_id_t;
enum {
	EXIFTAGID_GPS_LATITUDE	= 0x20002,
	EXIFTAGID_GPS_LONGITUDE	= 0x40004,
};
#define EXIF_RATIONAL 5
#define EXIF_ASCII 2
#define EXIF_BYTE 1
typedef unsigned int exif_tag_type_t;
typedef struct {
	//24 bytes = 6 ints
	exif_tag_type_t type;
	uint32_t count;
	uint32_t copy;
	uint32_t junk1;
	uint32_t junk2;
	uint32_t junk3;
} exif_tag_entry_t;

typedef struct {
	exif_tag_id_t tagid;
	exif_tag_entry_t tag_entry;
} exif_tags_info_t;
/* end of TAG */

#define CEILING16(x) (x&0xfffffff0)

typedef struct {
	//Size: 0x20 bytes = 32 bytes = 16 short
	unsigned short video_width;//0x2da0
	unsigned short video_height;//0x2da2
	unsigned short picture_width; //0x2da4
	unsigned short picture_height;//0x2da6
	unsigned short display_width; //0x2da8
	unsigned short display_height; //0x2daa
	unsigned short orig_picture_dx;  //0x2dac
	unsigned short orig_picture_dy; //0x2dae
	unsigned short ui_thumbnail_width; //0x2db0
	unsigned short ui_thumbnail_height; //0x2db2
	unsigned short thumbnail_width;  //0x2db4
	unsigned short thumbnail_height;  //0x2db6
	unsigned short raw_picture_height; //0x2db8
	unsigned short raw_picture_width;  //0x2dba
	unsigned short filler7;   ///0x2dbc
	unsigned short filler8;   //0x2dbe
} cam_ctrl_dimension_t;

/* TAG JB 01/20/2010 : Dual library support */
typedef struct
{
union {
	unsigned short video_width;//0x2df8
	unsigned short picture_width; //0x2df8
};
union {
	unsigned short video_height;//0x2dfa
	unsigned short picture_height;//0x2dfa
};
	unsigned short display_width; //0x2dfc
	unsigned short display_height; //0x2e00
	unsigned short orig_picture_dx;  //0x2e02
	unsigned short orig_picture_dy; //0x2e04
	unsigned short ui_thumbnail_width; //0x2e06
	unsigned short ui_thumbnail_height; //0x2e08
	unsigned short thumbnail_width;  //0x2e0a
	unsigned short thumbnail_height;  //0x2e0c
	unsigned short raw_picture_height; //0x2e0e
	unsigned short raw_picture_width;  //0x2e10
	unsigned short filler7;   ///0x2e12
	unsigned short filler8;   //0x2e14
} cam_ctrl_dimension_t_basic;
/* End of TAG */

typedef uint8_t cam_ctrl_type;
typedef uint8_t jpeg_event_t;
typedef unsigned int isp3a_af_mode_t;

struct str_map {
    const char *const desc;
    int val;
};

namespace android {

class QualcommCameraHardware : public CameraHardwareInterface {
public:

    virtual sp<IMemoryHeap> getPreviewHeap() const;
    virtual sp<IMemoryHeap> getRawHeap() const;
    virtual void setCallbacks(notify_callback notify_cb,
                              data_callback data_cb,
                              data_callback_timestamp data_cb_timestamp,
                              void* user);

    virtual void enableMsgType(int32_t msgType);
    virtual void disableMsgType(int32_t msgType);
    virtual bool msgTypeEnabled(int32_t msgType);

    virtual status_t dump(int fd, const Vector<String16>& args) const;
    virtual status_t startPreview();
    virtual void stopPreview();
    virtual bool previewEnabled();
    virtual status_t startRecording();
    virtual void stopRecording();
    virtual bool recordingEnabled();
    virtual void releaseRecordingFrame(const sp<IMemory>& mem);
    virtual status_t autoFocus();
    virtual status_t takePicture();
    virtual status_t cancelPicture();
    virtual status_t setParameters(const CameraParameters& params);
    virtual CameraParameters getParameters() const;
    virtual status_t sendCommand(int32_t command, int32_t arg1, int32_t arg2);
    virtual void release();
    virtual status_t cancelAutoFocus();

    static sp<CameraHardwareInterface> createInstance();
    static sp<QualcommCameraHardware> getInstance();

    void receivePreviewFrame(struct msm_frame *frame);
    void receiveJpegPicture(void);
    void jpeg_set_location();
    void receiveJpegPictureFragment(uint8_t *buf, uint32_t size);
    void notifyShutter();
    void notifyShutter_new(common_crop_t *crop, bool mPlayShutterSoundOnly);

private:
    QualcommCameraHardware();
    virtual ~QualcommCameraHardware();
    status_t startPreviewInternal();
    void stopPreviewInternal();
    friend void *auto_focus_thread(void *user);
    void runAutoFocus();
    bool native_set_dimension (int camfd);
    bool native_jpeg_encode (void);
    bool native_set_parm(cam_ctrl_type type, uint16_t length, void *value);
    bool native_set_dimension(cam_ctrl_dimension_t *value);
    int getParm(const char *parm_str, const str_map *parm_map);

    static wp<QualcommCameraHardware> singleton;

    /* These constants reflect the number of buffers that libmmcamera requires
       for preview and raw, and need to be updated when libmmcamera
       changes.
    */
    static const int kPreviewBufferCount = 4;
    static const int kRawBufferCount = 1;
    static const int kJpegBufferCount = 1;

    int jpegPadding;

    //TODO: put the picture dimensions in the CameraParameters object;
    CameraParameters mParameters;
    int mPreviewHeight;
    int mPreviewWidth;
    int mRawHeight;
    int mRawWidth;
    unsigned int frame_size;
    bool mCameraRunning;
    bool mPreviewInitialized;

    // This class represents a heap which maintains several contiguous
    // buffers.  The heap may be backed by pmem (when pmem_pool contains
    // the name of a /dev/pmem* file), or by ashmem (when pmem_pool == NULL).

    struct MemPool : public RefBase {
        MemPool(int buffer_size, int num_buffers,
                int frame_size,
                const char *name);

        virtual ~MemPool() = 0;

        void completeInitialization();
        bool initialized() const {
            return mHeap != NULL && mHeap->base() != MAP_FAILED;
        }

        virtual status_t dump(int fd, const Vector<String16>& args) const;

        int mBufferSize;
        int mAlignedBufferSize;
        int mNumBuffers;
        int mFrameSize;
        sp<MemoryHeapBase> mHeap;
        sp<MemoryBase> *mBuffers;

        const char *mName;
    };

    struct AshmemPool : public MemPool {
        AshmemPool(int buffer_size, int num_buffers,
                   int frame_size,
                   const char *name);
    };

    struct PmemPool : public MemPool {
        PmemPool(const char *pmem_pool,
                 int control_camera_fd, int flags, int pmem_type,
                 int buffer_size, int num_buffers,
                 int frame_size, int cbcr_offset,
                 int yoffset, const char *name);
        virtual ~PmemPool();
        int mFd;
        int mPmemType;
        int mCbCrOffset;
        int myOffset;
        int mCameraControlFd;
        uint32_t mAlignedSize;
        struct pmem_region mSize;
    };

    sp<PmemPool> mPreviewHeap;
    sp<PmemPool> mRecordHeap;
    sp<PmemPool> mThumbnailHeap;
    sp<PmemPool> mRawHeap;
    sp<PmemPool> mDisplayHeap;
    sp<AshmemPool> mJpegHeap;
    sp<PmemPool> mRawSnapShotPmemHeap;
    sp<AshmemPool> mRawSnapshotAshmemHeap;

    bool startCamera();
    bool initPreview();
    void deinitPreview();
    bool initRaw(bool initJpegHeap);
    void deinitRaw();

    bool mFrameThreadRunning;
    Mutex mFrameThreadWaitLock;
    Condition mFrameThreadWait;
    friend void *frame_thread(void *user);
    void runFrameThread(void *data);

    bool mShutterPending;
    Mutex mShutterLock;

    bool mSnapshotThreadRunning;
    Mutex mSnapshotThreadWaitLock;
    Condition mSnapshotThreadWait;
    friend void *snapshot_thread(void *user);
    void runSnapshotThread(void *data);
    Mutex mRawPictureHeapLock;

    void initDefaultParameters();
    /* TAG JB 01/21/2010 : Sensor dependant parameters */
    void filterPictureSizes();
    void findSensorType();
    /* End of TAG */

    /* TAG JB 01/21/2010 : Enhancement */    
    status_t setPreviewSize(const CameraParameters& params);
    status_t setJpegThumbnailSize(const CameraParameters& params);
    status_t setPictureSize(const CameraParameters& params);
    status_t setAntibanding(const CameraParameters& params);
    status_t setEffect(const CameraParameters& params);
    status_t setWhiteBalance(const CameraParameters& params);
    status_t setFlash(const CameraParameters& params);
    bool isValidDimension(int w, int h);
    /* End of TAG */
    
    /* TAG JB 01/21/2010 : Zoom */
    void storePreviewFrameForPostview();
    status_t setZoom(const CameraParameters& params);
    /* End of TAG */

    Mutex mLock;
    bool mReleasedRecordingFrame;

    void receiveRawPicture(void);


    Mutex mRecordLock;
    Mutex mRecordFrameLock;
    Condition mRecordWait;
    Condition mStateWait;

    /* mJpegSize keeps track of the size of the accumulated JPEG.  We clear it
       when we are about to take a picture, so at any time it contains either
       zero, or the size of the last JPEG picture taken.
    */
    uint32_t mJpegSize;

    notify_callback    mNotifyCb;
    data_callback      mDataCb;
    data_callback_timestamp mDataCbTimestamp;
    void               *mCallbackCookie;

    int32_t             mMsgEnabled;

    unsigned int        mPreviewFrameSize;
    int                 mRawSize;
    int                 mJpegMaxSize;

#if DLOPEN_LIBMMCAMERA
    void *libmmcamera;
#endif

    int mCameraControlFd;
    struct msm_camsensor_info mSensorInfo;
    cam_ctrl_dimension_t mDimension;
    bool mAutoFocusThreadRunning;
    Mutex mAutoFocusThreadLock;
    int mAutoFocusFd;


    pthread_t mFrameThread;
    pthread_t mSnapshotThread;

    common_crop_t mCrop;

    struct msm_frame frames[kPreviewBufferCount];
    bool mInPreviewCallback;
    bool mCameraRecording;

    /* TAG JB 01/20/2010 : New memory management + mdp zoom */
    int kPreviewBufferCountActual;
    /* End of TAG */
};

}; // namespace android

#endif

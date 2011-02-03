/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


//#define LOG_NDEBUG 0
#define LOG_TAG "SuperExtractor"
#include <utils/Log.h>
#include "include/SuperExtractor.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <media/stagefright/DataSource.h>
#include "include/ESDS.h"
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>


#define NOTSET_U8 ((OMX_U8)0xDE)
static OMX_VERSIONTYPE vOMX;
#define INIT_PARAM(_X_)  (memset(&(_X_), NOTSET_U8, sizeof(_X_)), ((_X_).nSize = sizeof (_X_)), (_X_).nVersion = vOMX)


#define SF_CHK_ERR(expr) {               \
            err = (expr);            \
            if ((err != OK))   \
            {                       \
            LOGV("error occured %x:[%s(%d)]\n",err,__FILE__, __LINE__); \
                goto cleanup;     \
            }                       \
        }


namespace android {

class SuperSource : public MediaSource {
public:
    // Caller retains ownership of both "dataSource" and "Parser Handle".
    SuperSource(const sp<MetaData> &format,
        const sp<DataSource> &dataSource,
        uint32_t mTrackCount, size_t &index,
        SuperExtractorData **AudExtractor);


    virtual status_t start(MetaData *params = NULL);
    virtual status_t stop();

    virtual sp<MetaData> getFormat();

    virtual status_t read(
        MediaBuffer **buffer, const ReadOptions *options = NULL);

protected:
    virtual ~SuperSource();

private:
    sp<MetaData> mFormat;
    sp<DataSource> mDataSource;
    SuperExtractorData *m_hExtractor;
    bool mStarted;
    bool mWait;
    size_t mFlagEnable;
    MediaBufferGroup *mGroup;
    MediaBuffer *mBuffer;
    int mEOS;
    OMX_ERRORTYPE eError;
    OMX_INDEXTYPE eIndex;
    NvxTrackInfo oInfo;
    NVX_CONFIG_TRACKLIST oTrackList;
    NVX_CONFIG_TRACKLIST_TRACK oAddTrack;
    OMX_CALLBACKTYPE pCallbacks;
    NVX_CONFIG_VIDEOHEADER mVidHd;
    NVX_CONFIG_AUDIOHEADER mAudHd;
    NVX_CONFIG_MP3TSENABLE oMp3Enable;
    NVX_CONFIG_DISABLEBUFFCONFIG oDisableFLag;
    OMX_INDEXTYPE eParam;
    bool mFirstBuffer;

    SuperSource(const SuperSource &);
    SuperSource &operator=(const SuperSource &);
};


struct OMXParserObserver : public BnOMXObserver {
    OMXParserObserver() {
        Sdata = NULL;
    }
    void setCallback( SuperExtractorData *hExtractor) ;

    virtual void onMessage(const omx_message &msg)
    {
        SuperExtractorData *hExtractor = (SuperExtractorData *)Sdata;
        if(hExtractor && hExtractor->msgCallback)
        {
            hExtractor->msgCallback(hExtractor, msg);
        }
    }

protected:
    virtual ~OMXParserObserver() {}

private:
    void* Sdata;
    OMXParserObserver(const OMXParserObserver &);
    OMXParserObserver&operator=(const OMXParserObserver &);
};

void OMXParserObserver::setCallback( SuperExtractorData *hExtractor) {
        Sdata = (void *)hExtractor;
    }

static void on_message(void *pAppData, const omx_message &msg) {
    SuperExtractorData * hExtractor = NULL;
    hExtractor = (SuperExtractorData *)pAppData;
    status_t err = OK;

switch (msg.type)
{
case omx_message::EVENT:
    {
        LOGV ("messege data event type %d ",msg.u.event_data.event);
        switch (msg.u.event_data.event)
        {
        case OMX_EventCmdComplete:
            {
                if (msg.u.event_data.data1 == OMX_CommandFlush)
                {
                    LOGV ("NvOsSemaphoreSignal  for video/Audio fulsh");
                    hExtractor->ParserFlushSema.broadcast();
                }
            }
            break;

        case OMX_EventBufferFlag:
            LOGV("Got OMX_EventBufferFlag event\n");
            hExtractor->EOS = true;
            hExtractor->hvideosignal.broadcast();
            hExtractor->haudiosignal.broadcast();
            break;
        }
        break;
    }

case omx_message::EMPTY_BUFFER_DONE:
    {
        //We are doing nothing for now.
        LOGV ("empty buffer done:We are doing nothing for now");
        break;
    }

case omx_message::FILL_BUFFER_DONE:
    {
        IOMX::buffer_id buffer = msg.u.extended_buffer_data.buffer;
        OMX_U32 flags = msg.u.extended_buffer_data.flags;
        LOGV("FILL_BUFFER_DONE(buffer: %p, size: %ld, flags: 0x%08lx, timestamp: %lld us (%.2f secs))",
            buffer,
            msg.u.extended_buffer_data.range_length,
            flags,
            msg.u.extended_buffer_data.timestamp,
            msg.u.extended_buffer_data.timestamp / 1E6);

        OMX_BUFFERHEADERTYPE* pBuffer= (  OMX_BUFFERHEADERTYPE *)buffer;

        if (0 == pBuffer->nOutputPortIndex)
        {
            if (hExtractor->bFilledVideoMsgQ)
            {
                LOGV ("ENQUEUING INTO  ----video msg Q and signlaling sema");
                SF_CHK_ERR(hExtractor->FilledVideoMsgQ.sfQueueEnQ(&pBuffer, 0));
                hExtractor->hvideosignal.broadcast();
                hExtractor->Count ++;
            }
        }

        if (1 == pBuffer->nOutputPortIndex)
        {
            if (hExtractor->bFilledAudioMsgQ)
            {
                SF_CHK_ERR(hExtractor->FilledAudioMsgQ.sfQueueEnQ(&pBuffer, 0));
                hExtractor->haudiosignal.broadcast();
            }
        }
        break;
    }

default:
    {
        CHECK(!"should not be here.");
        break;
    }
}
cleanup:
    LOGV(" Do Nothing");

}

SuperExtractor :: SuperExtractor (const sp<DataSource> &source)
        : mDataSource(source),
        mHaveMetadata(false),
        mHasVideo(false),
        mFileMetaData(new MetaData),
        AudExtractor(NULL),
        Extractor(NULL)
{

    char * component = "OMX.Nvidia.reader";
    OMX_ERRORTYPE eError;
    OMX_INDEXTYPE eIndex;
    status_t err = OK;
    NVX_CONFIG_DISABLEBUFFCONFIG oDisableFLag;

    NVX_CONFIG_TRACKLIST oTrackList;
    NVX_CONFIG_TRACKLIST_TRACK oAddTrack;
    OMX_CALLBACKTYPE pCallbacks;

    // Set OpenMAX version
    vOMX.s.nVersionMajor = 1;
    vOMX.s.nVersionMinor = 1;
    vOMX.s.nRevision = 0;
    vOMX.s.nStep = 0;

    Extractor = new SuperExtractorData;
    memset(Extractor,0,sizeof(SuperExtractorData));

    Extractor->msgCallback = on_message;
    LOGV(" In super extractor");
    sp<OMXParserObserver> observer1 = new OMXParserObserver();
    Extractor->hobserver = (void*)&observer1;
    observer1->setCallback(Extractor);
    Extractor->node = 0;
    CHECK_EQ(Extractor->mClient.connect(), OK);
    Extractor->sOMX = Extractor->mClient.interface();

    SF_CHK_ERR( Extractor->sOMX->allocateNode(
                                 component,
                                 observer1,
                                 &(Extractor->node)));

    SF_CHK_ERR(Extractor->sOMX->getExtensionIndex(
                                Extractor->node,
                                NVX_INDEX_CONFIG_TRACKLIST,
                                &eIndex));

    INIT_PARAM(oTrackList);
    SF_CHK_ERR(Extractor->sOMX->getConfig(
                                Extractor->node,
                                eIndex,
                                &oTrackList,
                                sizeof(NVX_CONFIG_TRACKLIST)));

    SF_CHK_ERR(Extractor->sOMX->getExtensionIndex(
                                Extractor->node,
                                NVX_INDEX_CONFIG_TRACKLIST_TRACK,
                                &eIndex));

    INIT_PARAM(oAddTrack);
    oAddTrack.uIndex = 0;
    oInfo.eTrackFlag = NvxTrack_AutoAdvance;
    oInfo.pClientPrivate = NULL;
    oInfo.uSize = 128;
    oInfo.pPath = new OMX_U8[oInfo.uSize];
    SFOsSnprintf((char *)oInfo.pPath, 128, "stagefright://%x",
                                          (mDataSource.get ()));
    oAddTrack.pTrackInfo = &oInfo;
    //Create Track list
    SF_CHK_ERR(Extractor->sOMX->setConfig(
                                Extractor->node,
                                eIndex,
                                &oAddTrack,
                                sizeof(NVX_CONFIG_TRACKLIST_TRACK)));

    // Disable bufferconfig
    SF_CHK_ERR(Extractor->sOMX->getExtensionIndex(
                                Extractor->node,
                                NVX_INDEX_CONFIG_DISABLEBUFFCONFIG,
                                &eIndex));
    INIT_PARAM(oDisableFLag);
    oDisableFLag.bDisableBuffConfig = OMX_TRUE;
    SF_CHK_ERR(Extractor->sOMX->setConfig(
                                Extractor->node,
                                eIndex,
                                &oDisableFLag,
                                sizeof(NVX_CONFIG_DISABLEBUFFCONFIG)));

    Extractor->VideoIndex  = 0; // initially set Index
    Extractor->AudioIndex  =  1;


cleanup:
    if (err != OK)
    {
        LOGV("Error in SuperExtractor Constructor");
        delete [ ] oInfo.pPath;
        oInfo.pPath = NULL;
        if (pBuffer)
        {
            delete [ ]pBuffer;
            pBuffer = NULL;
        }
        err = Extractor->sOMX->freeNode(Extractor->node);
        CHECK_EQ(err,OK);
        delete Extractor;
        Extractor = NULL;
    }
    else
    {
        LOGV ("exiting with error %d",err);
    }
}


SuperExtractor::~SuperExtractor() {
    status_t err = OK;
    LOGV("SuperExtractor::~SuperExtractor ");
    delete [ ]oInfo.pPath;
    oInfo.pPath = NULL;
    err = Extractor->sOMX->freeNode(Extractor->node);
    CHECK_EQ(err,OK);
    LOGV("SuperExtractor freeing ");
    delete Extractor;
    Extractor = NULL;

}

sp<MetaData> SuperExtractor::getMetaData() {

    NVX_CONFIG_QUERYMETADATA md;
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_INDEXTYPE eIndex;
    OMX_U32 len = 0;
    const char *pName = NULL;
    pBuffer = NULL;
    status_t err = OK;

    LOGV ("entered SuperExtractor GetMetaData");
    SF_CHK_ERR(Extractor->sOMX->getExtensionIndex(
                                Extractor->node,
                                NVX_INDEX_CONFIG_QUERYMETADATA,
                                &eIndex));
    INIT_PARAM(md);
    md.sValueStr = NULL;
    md.nValueLen = 0;
    md.eType = NvxMetadata_CoverArt;
    err = Extractor->sOMX->getConfig(
                           Extractor->node,
                           eIndex, &md,
                           sizeof(NVX_CONFIG_QUERYMETADATA));

    if (NO_MEMORY == err)
    {
        len = md.nValueLen;
        if (len == 0)
        {
            LOGV ("NO COVER ART");
            return mFileMetaData;
        }

        pBuffer = new OMX_U8[len +1];
        if (!pBuffer)
        {
            LOGV ("return NULL2");
            return NULL;
        }

        memset(pBuffer, 0, len);
        md.sValueStr = (char *)pBuffer;
        md.nValueLen = len +1;
        err = Extractor->sOMX->getConfig(
                               Extractor->node,
                               eIndex,
                               &md,
                               sizeof(NVX_CONFIG_QUERYMETADATA));
    }

    if (OK != err ||  md.nValueLen == 0)
    {
        delete [ ]pBuffer;
        pBuffer = NULL;
        return mFileMetaData;
    }

    LOGV ("Setting CoverArt album");
    mFileMetaData->setData(
                   kKeyAlbumArt, MetaData::TYPE_NONE,
                   pBuffer, len+1);

cleanup:
    return mFileMetaData;
}

size_t SuperExtractor::countTracks() {
    NVX_PARAM_STREAMCOUNT ostreamcount;
    OMX_ERRORTYPE eError;
    OMX_INDEXTYPE eParam;
    status_t err =OK;

    LOGV("In Count Tracks");
    SF_CHK_ERR(Extractor->sOMX->getExtensionIndex(
                                 Extractor->node,
                                 NVX_INDEX_PARAM_STREAMCOUNT,
                                 &eParam));
    INIT_PARAM(ostreamcount);
    SF_CHK_ERR(Extractor->sOMX->getParameter(
                                 Extractor->node,
                                 eParam,&ostreamcount,
                                 sizeof(NVX_PARAM_STREAMCOUNT)));

    LOGV ("NUMBER OF TRACKS IN SUPEREXTRACTOR %d",ostreamcount.StreamCount);


cleanup:
    if (err == OK)
    {
        mTrackCount = ostreamcount.StreamCount;
        Extractor->TrackCount = mTrackCount;
        return mTrackCount;
    }
    else
        return 0;
}
sp<MetaData> SuperExtractor::getTrackMetaData(
    size_t index, uint32_t flags) {

        NVX_PARAM_STREAMTYPE oStreamType;
        NVX_PARAM_AUDIOPARAMS oAudParams;
        OMX_PARAM_PORTDEFINITIONTYPE oPortDef;
        OMX_ERRORTYPE eError;
        OMX_INDEXTYPE eParam, eAudioIndex;
        NVX_PARAM_DURATION oDuration;
        int i;
        status_t err = OK;
        LOGV("get track metadata ");
        SF_CHK_ERR(Extractor->sOMX->getExtensionIndex(
                                    Extractor->node,
                                    NVX_INDEX_PARAM_DURATION,
                                    &eParam));
        INIT_PARAM(oDuration);
        INIT_PARAM(oStreamType);
        INIT_PARAM(oAudParams);
        INIT_PARAM(oPortDef);
        SF_CHK_ERR(Extractor->sOMX->getParameter(
                                    Extractor->node,
                                    eParam,
                                    &oDuration,
                                    sizeof(NVX_PARAM_DURATION)));
        SF_CHK_ERR(Extractor->sOMX->getExtensionIndex(
                                    Extractor->node,
                                    NVX_INDEX_PARAM_STREAMTYPE,
                                    &eParam));
        if (index == 0)
        {
            for (i =0; i< 2;i++)
            {
                oStreamType.nPort = i;
                SF_CHK_ERR(Extractor->sOMX->getParameter(
                                            Extractor->node,
                                            eParam, &oStreamType,
                                            sizeof(NVX_PARAM_STREAMTYPE)));

                switch (oStreamType.eStreamType)
                {
                case NvxStreamType_MPEG4:
                case NvxStreamType_MPEG4Ext:
                case NvxStreamType_H263:
                case NvxStreamType_WMV:
                case NvxStreamType_H264:
                case NvxStreamType_H264Ext:
                    {
                        LOGV ("video is avaliable ");
                        IsVideo = true;
                        Extractor->IsVideo = true;
                        break;
                    }
                case NvxStreamType_MP2:
                case NvxStreamType_MP3:
                case NvxStreamType_AAC:
                case NvxStreamType_AACSBR:
                case NvxStreamType_WMA:
                case NvxStreamType_WMAPro:
                case NvxStreamType_WMALossless:
                case NvxStreamType_AMRWB:
                case NvxStreamType_AMRNB:
                    {
                        LOGV ("AUDIO  is avaliable ");
                        IsAudio = true;
                        Extractor->IsAudio = true;
                        break;
                    }
                default:
                    break;
                }
            }


            if ( !IsVideo && IsAudio)
            {
                Extractor->VideoIndex = -1;
                Extractor->AudioIndex = 0;
                SF_CHK_ERR(Extractor->sOMX->sendCommand(
                                            Extractor->node,
                                            OMX_CommandPortDisable, 0));
            }
            else if (IsVideo && !IsAudio)
            {
                Extractor->VideoIndex = 0;
                Extractor->AudioIndex = -1;
                SF_CHK_ERR(Extractor->sOMX->sendCommand(
                                            Extractor->node,
                                            OMX_CommandPortDisable, 1));
            }
        }

        if ( index == Extractor->VideoIndex)
        {
            oStreamType.nPort = 0; //ON Port 0 for video
            SF_CHK_ERR(Extractor->sOMX->getParameter(
                                        Extractor->node,
                                        eParam, &oStreamType,
                                        sizeof(NVX_PARAM_STREAMTYPE)));
            // Stream has video hence set it as Video
            mFileMetaData->setCString(kKeyMIMEType, "video/");
            // creat new video track and its meta structure to fill required
            mTracks[index].meta = new  MetaData;
            mTracks[index].includes_expensive_metadata = false;
            mTracks[index].timescale = 0;
            mTracks[index].meta->setCString(kKeyMIMEType, "application/octet-stream");

            switch (oStreamType.eStreamType)
            {
            case NvxStreamType_MPEG4:
            case NvxStreamType_MPEG4Ext:
                mTracks[index].meta->setCString(
                                      kKeyMIMEType,
                                      MEDIA_MIMETYPE_VIDEO_MPEG4);
                break;
            case NvxStreamType_H263:
                mTracks[index].meta->setCString(
                                     kKeyMIMEType,
                                     MEDIA_MIMETYPE_VIDEO_H263);
                break;
            case NvxStreamType_WMV:
                mTracks[index].meta->setCString(
                                     kKeyMIMEType,
                                     MEDIA_MIMETYPE_VIDEO_WMV);
                break;
            case NvxStreamType_H264:
            case NvxStreamType_H264Ext:
                mTracks[index].meta->setCString(
                                     kKeyMIMEType,
                                     MEDIA_MIMETYPE_VIDEO_AVC);
                break;
            default:
                break;
            }

            oPortDef.nPortIndex = 0; //Videos
            SF_CHK_ERR(Extractor->sOMX->getParameter(
                                        Extractor->node,
                                        OMX_IndexParamPortDefinition,
                                        &oPortDef,
                                        sizeof(OMX_PARAM_PORTDEFINITIONTYPE)));
            int MaxInputSize = 0;
            mTracks[index].meta->setInt32(
                                 kKeyWidth, oPortDef.format.video.nFrameWidth);
            mTracks[index].meta->setInt32(
                                 kKeyHeight, oPortDef.format.video.nFrameHeight);
            mTracks[index].meta->setInt32(
                                 kKeyBitRate,oPortDef.format.video.nBitrate);
            mTracks[index].meta->setInt64(
                                 kKeyDuration, oDuration.nDuration );

            if ((oPortDef.format.video.nFrameWidth > 320) &&
                            (oPortDef.format.video.nFrameHeight > 240))
            {
                MaxInputSize = (oPortDef.format.video.nFrameWidth *
                                         oPortDef.format.video.nFrameHeight * 3) >> 2;
            }
            /* for less than QVGA size buffers, its better to allocate YUV sized buffes,
            as the input buffer for intra frames might be large */
            else
            {
                MaxInputSize = (oPortDef.format.video.nFrameWidth *
                                     oPortDef.format.video.nFrameHeight * 3) >> 1;

            }
            mTracks[index].meta->setInt32(kKeyMaxInputSize, MaxInputSize);

        } // end of video
        else  if (index == Extractor->AudioIndex)
        {
            SF_CHK_ERR(Extractor->sOMX->getExtensionIndex(
                                        Extractor->node,
                                        NVX_INDEX_PARAM_AUDIOPARAMS,
                                        &eAudioIndex));
            oStreamType.nPort = 1; // Audio port
            oPortDef.nPortIndex = 1; //Audio port
            SF_CHK_ERR(Extractor->sOMX->getParameter(
                                        Extractor->node,
                                        eParam,
                                        &oStreamType,
                                        sizeof(NVX_PARAM_STREAMTYPE)));

            mFileMetaData->setCString(kKeyMIMEType, "audio/");
            mTracks[index].meta = new  MetaData;
            mTracks[index].includes_expensive_metadata = false;
            mTracks[index].timescale = 0;
            mTracks[index].meta->setCString(
                                 kKeyMIMEType, "application/octet-stream");
            LOGV ("setting streamtype for mtracks index %d",index);
            switch (oStreamType.eStreamType)
            {
            case NvxStreamType_MP2:
            case NvxStreamType_MP3:
                mTracks[index].meta->setCString(
                                    kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG);
                break;
            case NvxStreamType_AAC:
            case NvxStreamType_AACSBR:
                mTracks[index].meta->setCString(
                                     kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AAC);
                break;
            case NvxStreamType_WMA:
            case NvxStreamType_WMAPro:
            case NvxStreamType_WMALossless:
                mTracks[index].meta->setCString(
                                     kKeyMIMEType,
                                     MEDIA_MIMETYPE_AUDIO_WMA);
                break;
            case NvxStreamType_AMRWB:
                mTracks[index].meta->setCString(
                                     kKeyMIMEType,
                                     MEDIA_MIMETYPE_AUDIO_AMR_WB);
                break;
            case NvxStreamType_AMRNB:
                mTracks[index].meta->setCString(
                                     kKeyMIMEType,
                                     MEDIA_MIMETYPE_AUDIO_AMR_NB);
                break;
            default: break;
            }

            oAudParams.nPort = 1;
            SF_CHK_ERR(Extractor->sOMX->getParameter(
                                        Extractor->node,
                                        eAudioIndex, &oAudParams,
                                        sizeof(NVX_PARAM_AUDIOPARAMS)));

            mTracks[index].meta->setInt32(
                                 kKeySampleRate, oAudParams.nSampleRate);
            mTracks[index].meta->setInt32(
                                 kKeyChannelCount, oAudParams.nChannels);
            mTracks[index].meta->setInt32(
                                 kKeyBitRate, oAudParams.nBitRate);
            mTracks[index].meta->setInt32(
                                 kKeyMaxInputSize, COMMON_MAX_INPUT_BUFFER_SIZE);
            mTracks[index].meta->setInt64(
                                 kKeyDuration, oDuration.nDuration );
            LOGV ("setting streamtype duration  %ld",oDuration.nDuration );
        }// end of auido
        LOGV(" end of Get track metadata");

cleanup:

        if (err == OK)
        {
            LOGV("END OF GETTRACKMETADATA");
            return mFileMetaData;
        }
        else
        {
            return NULL;
        }
}

sp<MediaSource> SuperExtractor::getTrack(size_t index) {

    Track  track;
    track = mTracks[index];
    LOGV("In get Track");
    return new SuperSource(
        track.meta, mDataSource, mTrackCount,index,&AudExtractor);
    LOGV(" end of Get Track");
}

    ////////////////////////////////////////////////////////////////////////////////
SuperSource::SuperSource(
        const sp<MetaData> &format,
        const sp<DataSource> &dataSource,
        uint32_t mTrackCount,size_t &index,SuperExtractorData **AudExtractor)
        : mFormat(format),
        mDataSource(dataSource),
        mStarted(false),
        mWait(false) ,
        mGroup(NULL),
        mFirstBuffer(true ),
        mBuffer(NULL),
        mFlagEnable(index),
        mEOS(0)
{

    status_t err = OK;
    OMX_ERRORTYPE eError = OMX_ErrorNone;

    if(*AudExtractor)
        m_hExtractor=*AudExtractor;

    LOGV(" In Super Source");
    if (index == 0)
    {
        int i =0;
        NVX_PARAM_STREAMTYPE oStreamType;
        OMX_INDEXTYPE eParam;
        INIT_PARAM(oStreamType);
        char * component = "OMX.Nvidia.reader";
        // Set OpenMAX version
        vOMX.s.nVersionMajor = 1;
        vOMX.s.nVersionMinor = 1;
        vOMX.s.nRevision = 0;
        vOMX.s.nStep = 0;
        m_hExtractor = new SuperExtractorData;
        memset(m_hExtractor,0,sizeof(SuperExtractorData));
        sp<OMXParserObserver> observer1 = new OMXParserObserver();
        m_hExtractor->hobserver = (void*)&observer1;
        observer1->setCallback(m_hExtractor);
        m_hExtractor->node = 0;
        CHECK_EQ(m_hExtractor->mClient.connect(), OK);
        m_hExtractor->sOMX = m_hExtractor->mClient.interface();
        SF_CHK_ERR(m_hExtractor->sOMX->allocateNode(
                                       component,observer1,
                                       &(m_hExtractor->node)));
        m_hExtractor->TrackCount = mTrackCount;
        m_hExtractor->StopCnt=0;
        mVidHd.nBuffer = NULL;
        mAudHd.nBuffer = NULL;
        m_hExtractor->msgCallback = on_message;

        SF_CHK_ERR(m_hExtractor->sOMX->getExtensionIndex(
                                       m_hExtractor->node,
                                       NVX_INDEX_CONFIG_TRACKLIST,
                                       &eIndex));
        INIT_PARAM(oTrackList);
        SF_CHK_ERR(m_hExtractor->sOMX->getConfig(
                                       m_hExtractor->node,
                                       eIndex, &oTrackList,
                                       sizeof(NVX_CONFIG_TRACKLIST)));
        SF_CHK_ERR(m_hExtractor->sOMX->getExtensionIndex(
                                       m_hExtractor->node,
                                       NVX_INDEX_CONFIG_TRACKLIST_TRACK,
                                       &eIndex));
        INIT_PARAM(oAddTrack);
        oAddTrack.uIndex = 0;
        oInfo.eTrackFlag = NvxTrack_AutoAdvance;
        oInfo.pClientPrivate = NULL;
        oInfo.uSize = 128;
        oInfo.pPath = new OMX_U8[oInfo.uSize];
         SFOsSnprintf((char *)oInfo.pPath, 128, "stagefright://%x",
                                          (mDataSource.get ()));
        oAddTrack.pTrackInfo = &oInfo;
        //Create Track list
        SF_CHK_ERR(m_hExtractor->sOMX->setConfig(
                                       m_hExtractor->node,
                                       eIndex, &oAddTrack,
                                       sizeof(NVX_CONFIG_TRACKLIST_TRACK)));
        m_hExtractor->VideoIndex = 0; // initially set Index
        m_hExtractor->AudioIndex  =   1;
        SF_CHK_ERR(m_hExtractor->sOMX->getExtensionIndex(
                                       m_hExtractor->node,
                                       NVX_INDEX_PARAM_STREAMTYPE,
                                       &eParam));

        for ( i =0; i< 2;i++)
        {
            oStreamType.nPort = i;
            SF_CHK_ERR(m_hExtractor->sOMX->getParameter(
                                           m_hExtractor->node,
                                           eParam, &oStreamType,
                                           sizeof(NVX_PARAM_STREAMTYPE)));
            switch (oStreamType.eStreamType)
            {
            case NvxStreamType_MPEG4:
            case NvxStreamType_MPEG4Ext:
            case NvxStreamType_H263:
            case NvxStreamType_WMV:
            case NvxStreamType_H264:
            case NvxStreamType_H264Ext:
                {
                    m_hExtractor->IsVideo = true;
                    break;
                }
            case NvxStreamType_MP2:
            case NvxStreamType_MP3:
            case NvxStreamType_AAC:
            case NvxStreamType_AACSBR:
            case NvxStreamType_WMA:
            case NvxStreamType_WMAPro:
            case NvxStreamType_WMALossless:
            case NvxStreamType_AMRWB:
            case NvxStreamType_AMRNB:
                {
                    m_hExtractor->IsAudio = true;
                    break;
                }

            }
        }
        *AudExtractor = m_hExtractor;

        if ( !m_hExtractor->IsVideo && m_hExtractor->IsAudio)
        {
            m_hExtractor->VideoIndex = -1;
            m_hExtractor->AudioIndex = 0;
            SF_CHK_ERR(m_hExtractor->sOMX->sendCommand(
                                           m_hExtractor->node,
                                           OMX_CommandPortDisable,0));
            *AudExtractor = m_hExtractor;
        }
        else if (m_hExtractor->IsVideo && !m_hExtractor->IsAudio)
        {
            m_hExtractor->VideoIndex = 0;
            m_hExtractor->AudioIndex = -1;
            SF_CHK_ERR(m_hExtractor->sOMX->sendCommand(
                                           m_hExtractor->node,
                                           OMX_CommandPortDisable,1));
            *AudExtractor = NULL;
        }


        //SuperExtractor constructor  Ends Here
        if (m_hExtractor->IsVideo)
        {
            sp<MetaData> meta = mFormat;
            SF_CHK_ERR(m_hExtractor->sOMX->getExtensionIndex(
                                           m_hExtractor->node,
                                           NVX_INDEX_CONFIG_VIDEOHEADER,
                                           &eParam));
            INIT_PARAM(mVidHd);
            mVidHd.nBufferlen = 1024;
            mVidHd.nBuffer = new char[ mVidHd.nBufferlen];
            SF_CHK_ERR(m_hExtractor->sOMX->getConfig(
                                           m_hExtractor->node,
                                           eParam, &mVidHd,
                                           sizeof(NVX_CONFIG_VIDEOHEADER)));
            LOGV ("setting video header data");
            meta->setData(kKeyHeader, kTypeHeader,
                                mVidHd.nBuffer,mVidHd.nBufferlen);
        }

    }
    if (index == m_hExtractor->AudioIndex)
    {

        if (m_hExtractor->IsAudio)
        {
            const char *mime;
            sp<MetaData> meta =mFormat;
            CHECK(meta->findCString(kKeyMIMEType, &mime));

            if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC))
            {
                SF_CHK_ERR(m_hExtractor->sOMX->getExtensionIndex(
                                               m_hExtractor->node,
                                               NVX_INDEX_CONFIG_AUDIOHEADER,
                                               &eParam));
            }
            else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_WMA))
            {
                LOGV ("GettiGEng streamtype for mtracks index MEDIA_MIMETYPE_AUDIO_WMA ",index);
                SF_CHK_ERR(m_hExtractor->sOMX->getExtensionIndex(
                                                m_hExtractor->node,
                                                NVX_INDEX_CONFIG_WMAHEADER,
                                                &eParam));
                // Disable bufferconfig
                SF_CHK_ERR(m_hExtractor->sOMX->getExtensionIndex(
                                                m_hExtractor->node,
                                                NVX_INDEX_CONFIG_DISABLEBUFFCONFIG,
                                                &eIndex));
                INIT_PARAM(oDisableFLag);
                oDisableFLag.bDisableBuffConfig = OMX_TRUE;
                LOGV ("calling disable buf config ");
                SF_CHK_ERR(m_hExtractor->sOMX->setConfig(
                                                m_hExtractor->node,
                                                eIndex,
                                                &oDisableFLag,
                                                sizeof(NVX_CONFIG_DISABLEBUFFCONFIG)));
            }
            if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC)
                            || !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_WMA))
            {
                sp<MetaData> meta =mFormat;
                INIT_PARAM(mAudHd);
                mAudHd.nBufferlen = 1024;
                mAudHd.nBuffer = new char[mAudHd.nBufferlen];
                SF_CHK_ERR(m_hExtractor->sOMX->getConfig(
                                                m_hExtractor->node,
                                                eParam,&mAudHd,
                                                sizeof(NVX_CONFIG_AUDIOHEADER)));

                LOGV ("setting audio header data %d",mAudHd.nBufferlen);
                meta->setData(kKeyHeader, kTypeHeader,
                                            mAudHd.nBuffer,mAudHd.nBufferlen);
            }
            if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG))
            {

                // enable/Disable mp3TS
                SF_CHK_ERR(m_hExtractor->sOMX->getExtensionIndex(
                                               m_hExtractor->node,
                                               NVX_INDEX_CONFIG_MP3TSENABLE,
                                               &eIndex));
                LOGV ("calling enable/disable MP3TS ");
                INIT_PARAM(oMp3Enable);
                oMp3Enable.bMp3Enable = OMX_TRUE;
                SF_CHK_ERR(m_hExtractor->sOMX->setConfig(
                                               m_hExtractor->node,
                                               eIndex,&oMp3Enable,
                                               sizeof(NVX_CONFIG_MP3TSENABLE)));
            }
           
        }

    }

cleanup:
    if (err != OK)
    {
        if (mFlagEnable == 0)
        {
            delete  [ ] mVidHd.nBuffer;
            mVidHd.nBuffer = NULL;
            delete [ ]oInfo.pPath;
            oInfo.pPath = NULL;
            delete [ ] mAudHd.nBuffer;
            mAudHd.nBuffer = NULL;
        }
    }
    LOGV(" Super Source End");
}
    SuperSource::~SuperSource() {
        if (mStarted) {
            stop();
        }
}
int  FillThisBuffer(void* pArgContext)
{
    OMX_BUFFERHEADERTYPE  *pBuffer = NULL;
    OMX_ERRORTYPE Err = OMX_ErrorNone;
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    SuperExtractorData *m_hExtractor = (SuperExtractorData *)pArgContext;
    uint32_t AudioEntries =0;
    uint32_t VideoEntries = 0;
    status_t err;
    LOGV ("entered  the thread Fill this buffer ");

    while (!m_hExtractor->mStopped)
    {
        if (m_hExtractor->seeking)
        {
            Mutex::Autolock autoLock(m_hExtractor->mMutex);
            m_hExtractor->hseek.wait( m_hExtractor->mMutex);
        }

        if (  m_hExtractor->IsVideo)
        {
            if ( (VideoEntries = m_hExtractor->EmptyVideoMsgQ.
                                           sfQueueGetNumEntries()) > 0)

            {
                //LOGV("entries in the EmptyVideoMsgQ & sending to parser");
                err = m_hExtractor->EmptyVideoMsgQ.sfQueueDeQ(&pBuffer);

                err = m_hExtractor->sOMX->fillBuffer(
                                          m_hExtractor->node,
                                          (void *) pBuffer);
                if (err != OK)
                {
                    err = m_hExtractor->EmptyVideoMsgQ.sfQueueEnQ(&pBuffer,0);
                }
            }
        }
        if (m_hExtractor->IsAudio)
        {
            if ((AudioEntries = m_hExtractor->EmptyAudioMsgQ.
                                              sfQueueGetNumEntries()) > 0)
            {
                //LOGV("some entries present in the EmptyAudioMsgQ and sending to parser");
                err = m_hExtractor->EmptyAudioMsgQ.sfQueueDeQ(&pBuffer);
                err = m_hExtractor->sOMX->fillBuffer(
                                          m_hExtractor->node,
                                          (void *) pBuffer);
                if (err != OK)
                {
                    err = m_hExtractor->EmptyAudioMsgQ.sfQueueEnQ(&pBuffer,0);
                }
            }
        }

        if ( (!AudioEntries) && (!VideoEntries))
        {
            Mutex::Autolock autoLock(m_hExtractor->mMutex);
            m_hExtractor->fillwait = true;
            m_hExtractor->hsema.wait( m_hExtractor->mMutex);
            m_hExtractor->fillwait = false;
        }
    } //End ofwhile
    return 1;
} //Function:fillthisbuffer

status_t SuperSource::start(MetaData *params) {

    OMX_ERRORTYPE eError = OMX_ErrorNone;
    int i =0;
    int32_t max_size;
    NVX_CONFIG_VIDEOHEADER mVidHd;
    OMX_INDEXTYPE eParam;
    OMX_INDEXTYPE eIndex;
    NVX_CONFIG_MP3TSENABLE oMp3Enable;
    status_t err = OK;
    LOGV ("enterd SuperSource start");
    CHECK(!mStarted);
    mGroup = new MediaBufferGroup;

    CHECK(mFormat->findInt32(kKeyMaxInputSize, &max_size));

    if (m_hExtractor == NULL)
    {
        LOGV(" extractor need to allocate first ");
        goto cleanup;
    }

    mGroup->add_buffer(new MediaBuffer(max_size));
    m_hExtractor->seeking = false;
    m_hExtractor->mStopped = false;
    m_hExtractor->EOS = false;
    m_hExtractor->fillwait = false;

    if (mFlagEnable == 0)
    {
        INIT_PARAM(m_hExtractor->decInputPortDef);

        if (m_hExtractor->IsAudio)
        {
            m_hExtractor->decInputPortDef.nPortIndex = 1;
            SF_CHK_ERR(m_hExtractor->sOMX->getParameter(
                                           m_hExtractor->node,
                                           OMX_IndexParamPortDefinition,
                                           &m_hExtractor->decInputPortDef,
                                           sizeof(OMX_PARAM_PORTDEFINITIONTYPE)));

            m_hExtractor->decInputPortDef.nBufferCountActual =
                                    m_hExtractor->decInputPortDef.nBufferCountMin;
            m_hExtractor->NoOfAudioBuffers =
                                  m_hExtractor->decInputPortDef.nBufferCountActual;

            err = m_hExtractor->EmptyAudioMsgQ.sfQueueCreate(
                   (MAX_INPUT_BUFFERS > m_hExtractor->decInputPortDef.nBufferCountActual)?
                    MAX_INPUT_BUFFERS:m_hExtractor->decInputPortDef.nBufferCountActual,
                    sizeof(OMX_BUFFERHEADERTYPE *));

            if (err == OK)
                m_hExtractor->bEmptyAudioMsgQ = true;

            err = m_hExtractor->FilledAudioMsgQ.sfQueueCreate(
                     (MAX_INPUT_BUFFERS > m_hExtractor->decInputPortDef.nBufferCountActual)?
                      MAX_INPUT_BUFFERS:m_hExtractor->decInputPortDef.nBufferCountActual,
                      sizeof(OMX_BUFFERHEADERTYPE *));

            if (err == OK)
                m_hExtractor->bFilledAudioMsgQ = true;

            LOGV("Allocating Audio InputPort Buffers : %d",
                                      m_hExtractor->decInputPortDef.nBufferCountActual);

            for (i = 0; i < m_hExtractor->decInputPortDef.nBufferCountActual; i++)
            {
                void  *mAData = NULL;
                err = m_hExtractor->sOMX->allocateBuffer(
                                          m_hExtractor->node,
                                          m_hExtractor->decInputPortDef.nPortIndex,
                                          m_hExtractor->decInputPortDef.nBufferSize,
                                          (void **)(&m_hExtractor->AudioinputBuffer[i]),
                                          &mAData );
                if (err == OK)
                    SF_CHK_ERR(m_hExtractor->EmptyAudioMsgQ.sfQueueEnQ(
                                                 &m_hExtractor->AudioinputBuffer[i], 0));
            }
        }

        if (m_hExtractor->IsVideo)
        {
            m_hExtractor->decInputPortDef.nPortIndex = 0;
            SF_CHK_ERR(m_hExtractor->sOMX->getParameter(
                                           m_hExtractor->node,
                                           OMX_IndexParamPortDefinition,
                                           &m_hExtractor->decInputPortDef,
                                           sizeof(OMX_PARAM_PORTDEFINITIONTYPE)));

            m_hExtractor->decInputPortDef.nBufferCountActual =
                                        m_hExtractor->decInputPortDef.nBufferCountMin;
            m_hExtractor->NoOfVideoBuffers =
                                        m_hExtractor->decInputPortDef.nBufferCountActual;
            LOGV("Allocating Video InputPort Buffers : %d",
                                     m_hExtractor->decInputPortDef.nBufferCountActual);

            err = m_hExtractor->EmptyVideoMsgQ.sfQueueCreate(
                         (MAX_INPUT_BUFFERS > m_hExtractor->decInputPortDef.nBufferCountActual)?
                          MAX_INPUT_BUFFERS:m_hExtractor->decInputPortDef.nBufferCountActual,
                          sizeof(OMX_BUFFERHEADERTYPE *));

            if (err == OK)
                m_hExtractor->bEmptyVideoMsgQ = true;

            err = m_hExtractor->FilledVideoMsgQ.sfQueueCreate(
                      (MAX_INPUT_BUFFERS > m_hExtractor->decInputPortDef.nBufferCountActual)?
                       MAX_INPUT_BUFFERS:m_hExtractor->decInputPortDef.nBufferCountActual,
                       sizeof(OMX_BUFFERHEADERTYPE *));

            if (err == OK)
                m_hExtractor->bFilledVideoMsgQ = true;

            for (i = 0; i < m_hExtractor->decInputPortDef.nBufferCountActual; i++)
            {
                void *AData = NULL;
                err = m_hExtractor->sOMX->allocateBuffer(
                                          m_hExtractor->node,
                                          m_hExtractor->decInputPortDef.nPortIndex,
                                          m_hExtractor->decInputPortDef.nBufferSize,
                                          (void **)(&m_hExtractor->VideoinputBuffer[i]),&AData );
                if (err == OK)
                    SF_CHK_ERR(m_hExtractor->EmptyVideoMsgQ.sfQueueEnQ(
                                                &m_hExtractor->VideoinputBuffer[i], 0));
            }
        }
        SF_CHK_ERR(m_hExtractor->sOMX->sendCommand(
                                       m_hExtractor->node,
                                       OMX_CommandStateSet,
                                       OMX_StateIdle));
        createThreadEtc(FillThisBuffer, (void *)m_hExtractor,
                                         "FillThisBufferThread");
        SF_CHK_ERR(m_hExtractor->sOMX->sendCommand(
                                       m_hExtractor->node,
                                       OMX_CommandStateSet,
                                       OMX_StateExecuting));
    }

cleanup:
    if (err == OK)
    {
        LOGV ("parser component  is into execute");
        mStarted = true;
        return OK;
    }
    else
    {
        mStarted = false;
        return UNKNOWN_ERROR;
    }
}


status_t SuperSource::stop() {
    OMX_ERRORTYPE eError  = OMX_ErrorNone;
    int i;
    LOGV(" SuperSource Stop--------");
    status_t err = OK;
    CHECK(mStarted);
    mStarted = false;

    if (mBuffer != NULL) {
        mBuffer->release();
        mBuffer = NULL;
    }
    delete mGroup;
    mGroup = NULL;

    m_hExtractor->mStopped = true;
    // Signal the child thread to exit
    m_hExtractor->hsema.broadcast();
    if  ((mFlagEnable == m_hExtractor->VideoIndex) && (m_hExtractor->IsVideo))
    {

        delete [ ] mVidHd.nBuffer;
        mVidHd.nBuffer = NULL;


        err = m_hExtractor->sOMX->sendCommand(
                                m_hExtractor->node,
                                OMX_CommandFlush,0);

        // waiting for parser flushsema
        {
            Mutex::Autolock autoLock(m_hExtractor->mMutex);
            m_hExtractor->ParserFlushSema.wait( m_hExtractor->mMutex);
        }


        LOGV("Freeing All Video InputPort Buffers : %d",
            m_hExtractor->NoOfVideoBuffers);
        for (i = 0; i < m_hExtractor->NoOfVideoBuffers; i++)
        {
            err = m_hExtractor->sOMX->freeBuffer(
                                    m_hExtractor->node,
                                    0,
                                    m_hExtractor->VideoinputBuffer[i]);
        }
        m_hExtractor->EmptyVideoMsgQ.sfQueueDestroy();
        m_hExtractor->FilledVideoMsgQ.sfQueueDestroy();
        m_hExtractor->bVidDone = true;

    }
    if  ((mFlagEnable == m_hExtractor->AudioIndex) && (m_hExtractor->IsAudio))
    {
        delete [ ] mAudHd.nBuffer;
        mAudHd.nBuffer = NULL;


        err = m_hExtractor->sOMX->sendCommand(
                                m_hExtractor->node,
                                OMX_CommandFlush,1);

        // waiting for parser flushsema
        {
            Mutex::Autolock autoLock(m_hExtractor->mMutex);
            m_hExtractor->ParserFlushSema.wait( m_hExtractor->mMutex);
        }

        LOGV("Freeing All Audio InputPort Buffers : %d",
            m_hExtractor->NoOfAudioBuffers);
        for (i = 0; i < m_hExtractor->NoOfAudioBuffers; i++)
        {
            err = m_hExtractor->sOMX->freeBuffer(
                                    m_hExtractor->node,
                                    1,
                                    m_hExtractor->AudioinputBuffer[i]);
        }
        m_hExtractor->EmptyAudioMsgQ.sfQueueDestroy();
        m_hExtractor->FilledAudioMsgQ.sfQueueDestroy();
        m_hExtractor->bAudDone = true;

    }
    if (( m_hExtractor->VideoIndex == -1 && m_hExtractor->bAudDone) ||
        ( m_hExtractor->AudioIndex == -1 && m_hExtractor->bVidDone) ||
        ( m_hExtractor->bVidDone && m_hExtractor->bAudDone))
    {

        delete [ ]oInfo.pPath;
        oInfo.pPath = NULL;


        err = m_hExtractor->sOMX->sendCommand(
                                m_hExtractor->node,
                                OMX_CommandStateSet,
                                OMX_StateIdle);
        err = m_hExtractor->sOMX->freeNode(m_hExtractor->node);

        LOGV ("Freeing extractor memroy");
        delete m_hExtractor;
        m_hExtractor = NULL;
    }


cleanup:
    LOGV("returning from source stop");
    if (err == OK)
        return OK;
    else
    {
        LOGV("Error in Stop");
        return UNKNOWN_ERROR;
    }
}

    sp<MetaData> SuperSource::getFormat() {

        return mFormat;
    }

status_t SuperSource::read(
    MediaBuffer **out, const ReadOptions *options) {

        OMX_ERRORTYPE eError = OMX_ErrorNone;
        OMX_BUFFERHEADERTYPE  *pBuffer = NULL;
        status_t err = OK ;
        OMX_S64 seekTimeUs = 0;
        OMX_TIME_CONFIG_TIMESTAMPTYPE TimeStamp;
        ReadOptions::SeekMode mode;

        if (options && options->getSeekTo(&seekTimeUs,&mode) &&
            (mFlagEnable == m_hExtractor->VideoIndex || ( !(m_hExtractor->IsVideo))) )
        {
            m_hExtractor->seeking = true;
            LOGV("SEEKED  to position %lld",seekTimeUs);
            TimeStamp.nTimestamp = (OMX_TICKS)(seekTimeUs);
            TimeStamp.nPortIndex = 0;
            LOGV("Flipping state Exec->Pause");
            SF_CHK_ERR(m_hExtractor->sOMX->sendCommand(
                                           m_hExtractor->node,
                                           OMX_CommandStateSet,
                                           OMX_StatePause));
            LOGV("GET THE CLOSEST SAMPLE");
            err = m_hExtractor->sOMX->setConfig(
                                      m_hExtractor->node,
                                      OMX_IndexConfigTimePosition,
                                      &TimeStamp,
                                      sizeof(OMX_TIME_CONFIG_TIMESTAMPTYPE));
            if(err != OK)
            {
                LOGV("error EOF while seeking");
                m_hExtractor->mStopped = true;
                return ERROR_END_OF_STREAM;
            }
            if ((seekTimeUs == 0) && (m_hExtractor->IsAudio))
            {
                LOGV ("Audio is seeking to 0 hence send the header buffer again ");
                mFormat->setData(kKeyHeader, kTypeHeader,
                                   mAudHd.nBuffer,mAudHd.nBufferlen);
            }
            //Draining buffers in filled msgqueue
            if (m_hExtractor->IsVideo)
            {
                while (m_hExtractor->FilledVideoMsgQ.sfQueueGetNumEntries() >0)
                {
                    LOGV("processing video buffers, and set semaphore count--");
                    SF_CHK_ERR(m_hExtractor->FilledVideoMsgQ.sfQueueDeQ(&pBuffer));
                    SF_CHK_ERR(m_hExtractor->EmptyVideoMsgQ.sfQueueEnQ(&pBuffer,0));
                }
            }
            if (m_hExtractor->IsAudio)
            {
                while (m_hExtractor->FilledAudioMsgQ.sfQueueGetNumEntries() >0)
                {
                    LOGV("processing audio buffers, with semaphore count--");
                    SF_CHK_ERR(m_hExtractor->FilledAudioMsgQ.sfQueueDeQ(&pBuffer));
                    SF_CHK_ERR(m_hExtractor->EmptyAudioMsgQ.sfQueueEnQ(&pBuffer,0));
                }
            }
            //flip the state to executing
            LOGV("Flipping the state Pause->exec");
            SF_CHK_ERR(m_hExtractor->sOMX->sendCommand(
                                           m_hExtractor->node,
                                           OMX_CommandStateSet,
                                           OMX_StateExecuting));

            if (m_hExtractor->fillwait)
            {
                m_hExtractor->hsema.broadcast();
            }
            else
            {
                m_hExtractor->hseek.broadcast();
            }
            m_hExtractor->seeking = false;
            //Buffer mgmt
            if (mBuffer != NULL) {
                mBuffer->release();
                mBuffer = NULL;
            }
        }
        //NORMAL START
        CHECK(mStarted);
        *out = NULL;
        if (mFlagEnable == m_hExtractor->VideoIndex)
        {
            while(m_hExtractor->FilledVideoMsgQ.sfQueueGetNumEntries() < 1)
            {
                LOGV ("wait for video semaphore");
                Mutex::Autolock autoLock(m_hExtractor->mMutex);
                m_hExtractor->hvideosignal.wait( m_hExtractor->mMutex);
            }
            mWait = true;
        }
        else  if (mFlagEnable == m_hExtractor->AudioIndex)
        {
            while(m_hExtractor->FilledAudioMsgQ.sfQueueGetNumEntries() < 1)
            {
                LOGV ("wait for audio semaphore ");
                Mutex::Autolock autoLock(m_hExtractor->mMutex);
                m_hExtractor->haudiosignal.wait( m_hExtractor->mMutex);
            }
            mWait = true;
        }

        if (m_hExtractor->EOS)
        {
            mEOS = 1;
            m_hExtractor->mStopped = true;
            m_hExtractor->StopCnt++;
            m_hExtractor->hsema.broadcast();
            return ERROR_END_OF_STREAM;
        }

        err = mGroup->acquire_buffer(&mBuffer);
        LOGV ("acquire buffer return status %x",err );

        if (err != OK) {
            CHECK_EQ(mBuffer, NULL);
            return err;
        }
        if (mFlagEnable == m_hExtractor->VideoIndex) //0 for video
        {
            if (m_hExtractor->bFilledVideoMsgQ &&
                (m_hExtractor->FilledVideoMsgQ.sfQueueGetNumEntries() > 0) ){

                    m_hExtractor->FilledVideoMsgQ.sfQueueDeQ(&pBuffer);
                    LOGV ("dequeued the video buffer %x and memcopying %d into SF buffer flag %d",
                        pBuffer->pBuffer,pBuffer->nFilledLen,pBuffer->nFlags);
                    uint8_t * temp = (uint8_t *)mBuffer->data();

                    if (temp!= NULL )
                    {
                        if ((mFirstBuffer || (pBuffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG)) )
                        {
                            if (pBuffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG)
                                LOGV ("first buffer is header ");
                            pBuffer->nFilledLen = 0;
                            pBuffer->nTimeStamp = 0;
                            pBuffer->nFlags = 0;
                            SF_CHK_ERR(m_hExtractor->EmptyVideoMsgQ.sfQueueEnQ(&pBuffer,0));
                            m_hExtractor->hsema.broadcast();
                            LOGV("skipped video buffer and waiting");
                            while(m_hExtractor->FilledVideoMsgQ.sfQueueGetNumEntries() < 1)
                            {
                                LOGV ("wait for video semaphore");
                                Mutex::Autolock autoLock(m_hExtractor->mMutex);
                                m_hExtractor->hvideosignal.wait( m_hExtractor->mMutex);
                            }
                            SF_CHK_ERR(m_hExtractor->FilledVideoMsgQ.sfQueueDeQ(&pBuffer));
                            mFirstBuffer = false;
                            LOGV ("skip the first video buffer");
                        }
                        else
                        {
                            if (pBuffer->nFilledLen == 0)
                            {
                                m_hExtractor->mStopped =true;
                                LOGV (" read is returing zero sized buffer ");
                                return ERROR_END_OF_STREAM;
                            }
                        }

                        memcpy (temp,pBuffer->pBuffer,pBuffer->nFilledLen);
                        LOGV ("setting range with video length %d",pBuffer->nFilledLen);
                        mBuffer->set_range(0, pBuffer->nFilledLen);
                        mBuffer->meta_data()->clear();
                        LOGV ("setting metadata nTimeStamp value %lld",pBuffer->nTimeStamp);
                        mBuffer->meta_data()->setInt64(
                            kKeyTime, pBuffer->nTimeStamp);
                        // reset few fields
                        pBuffer->nFilledLen = 0;
                        pBuffer->nTimeStamp = 0;
                        pBuffer->nFlags = 0;
                        SF_CHK_ERR(m_hExtractor->EmptyVideoMsgQ.sfQueueEnQ(&pBuffer,0));
                        m_hExtractor->hsema.broadcast();
                    }
            }
            else
            {
                m_hExtractor->mStopped =true;
                LOGV ("read is returing error ");
                return UNKNOWN_ERROR;
            }
        }
        else  if (mFlagEnable == m_hExtractor->AudioIndex)
        {
            const char *mime;

            CHECK(mFormat->findCString(kKeyMIMEType, &mime));
            if (m_hExtractor->bFilledAudioMsgQ &&
                           (m_hExtractor->FilledAudioMsgQ.sfQueueGetNumEntries() > 0) )
            {
                SF_CHK_ERR(m_hExtractor->FilledAudioMsgQ.sfQueueDeQ(&pBuffer));
                LOGV ("dequeued the audio buffer %xand memcopying %dinto SF buffer",pBuffer->pBuffer,pBuffer->nFilledLen);
                uint8_t * temp = (uint8_t *)mBuffer->data();
                if (temp!= NULL )
                {
                    if ((mFirstBuffer || (pBuffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG)) )
                    {
                        if (pBuffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG)
                            LOGV ("first buffer is header ");
                        pBuffer->nFilledLen = 0;
                        pBuffer->nTimeStamp = 0;
                        pBuffer->nFlags = 0;
                        SF_CHK_ERR(m_hExtractor->EmptyAudioMsgQ.sfQueueEnQ(&pBuffer,0));
                        m_hExtractor->hsema.broadcast();
                        LOGV("skipped audio buffer and waiting");
                        while(m_hExtractor->FilledAudioMsgQ.sfQueueGetNumEntries() < 1)
                        {
                            LOGV ("wait for video semaphore");
                            Mutex::Autolock autoLock(m_hExtractor->mMutex);
                            m_hExtractor->haudiosignal.wait( m_hExtractor->mMutex);
                        }
                        SF_CHK_ERR(m_hExtractor->FilledAudioMsgQ.sfQueueDeQ(&pBuffer));
                        mFirstBuffer = false;
                        LOGV ("skip the first audio buffer");
                    }
                    memcpy (temp,pBuffer->pBuffer,pBuffer->nFilledLen);
                    mBuffer->set_range(0, pBuffer->nFilledLen);
                    mBuffer->meta_data()->clear();
                    mBuffer->meta_data()->setInt64(
                        kKeyTime, pBuffer->nTimeStamp);
                    SF_CHK_ERR(m_hExtractor->EmptyAudioMsgQ.sfQueueEnQ(&pBuffer,0));
                    m_hExtractor->hsema.broadcast();
                }
            }

            else
            {
                m_hExtractor->mStopped = true;
                LOGV ("read is returing errrorrrrrrrrr ");
                return UNKNOWN_ERROR;
            }
        }

cleanup:
        if( err == OK)
        {
            *out = mBuffer;
            mBuffer = NULL;
            return OK;
        }
        else
        {
            *out = NULL;
            return UNKNOWN_ERROR;
        }

}
    //////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////////////////////////////////

bool SniffSuper (
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *meta)
    {

        uint8_t header[16];

        ssize_t n = source->readAt(0, header, sizeof(header));
        if (n < (ssize_t)sizeof(header)) {
            return false;
        }
        LOGV ("entered sniff super ");

        if ((!memcmp(header, "RIFF", 4)))
        {
            if ((!memcmp(header+8, "AVI ", 4)) ||
                (!memcmp(header +8, "AVIX", 4)))
            {
                *mimeType = MEDIA_MIMETYPE_CONTAINER_AVI;
                *confidence = 0.1;
                LOGV ("avi is identified /////");
                return true;
            }
        }
        else if (!memcmp(header, ASF_Header_GUID, 16))
        {
            *mimeType = MEDIA_MIMETYPE_CONTAINER_ASF;
            *confidence = 0.1;
            LOGV ("asf is identified /////");
            return true;
        }

        return false;
    }
static int SFOsSnprintf( char *str, size_t size, const char *format, ... )
{
    int n;
    va_list ap;

    va_start( ap, format );
    n = vsnprintf( str, size, format, ap );
    va_end( ap );

    return n;
}


}  // namespace android

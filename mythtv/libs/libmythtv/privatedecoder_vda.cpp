// Based upon CDVDVideoCodecVDA from the xbmc project, originally written by
// Scott Davilla (davilla@xbmc.org) and released under the GPLv2

#include "mythlogging.h"
#define LOC QString("VDADec: ")
#define ERR QString("VDADec error: ")

#include "frame.h"
#include "myth_imgconvert.h"
#include "util-osx-cocoa.h"
#include "privatedecoder_vda.h"
#ifdef USING_QUARTZ_VIDEO
#undef CodecType
#import  "QuickTime/ImageCompression.h"
#endif

extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
extern const uint8_t *ff_find_start_code(const uint8_t * p,
                                         const uint8_t *end,
                                         uint32_t * state);
}
VDALibrary *gVDALib = NULL;

VDALibrary* VDALibrary::GetVDALibrary(void)
{
    static QMutex vda_create_lock;
    QMutexLocker locker(&vda_create_lock);
    if (gVDALib)
        return gVDALib;
    gVDALib = new VDALibrary();
    if (gVDALib)
    {
        if (gVDALib->IsValid())
            return gVDALib;
        delete gVDALib;
    }
    gVDALib = NULL;
    return gVDALib;
}

VDALibrary::VDALibrary(void)
  : decoderCreate(NULL),  decoderDecode(NULL), decoderFlush(NULL),
    decoderDestroy(NULL), decoderConfigWidth(NULL),
    decoderConfigHeight(NULL), decoderConfigSourceFmt(NULL),
    decoderConfigAVCCData(NULL), m_lib(NULL), m_valid(false)
{
    m_lib = new QLibrary(VDA_DECODER_PATH);
    if (m_lib)
    {
        decoderCreate  = (MYTH_VDADECODERCREATE)
                            m_lib->resolve("VDADecoderCreate");
        decoderDecode  = (MYTH_VDADECODERDECODE)
                            m_lib->resolve("VDADecoderDecode");
        decoderFlush   = (MYTH_VDADECODERFLUSH)
                            m_lib->resolve("VDADecoderFlush");
        decoderDestroy = (MYTH_VDADECODERDESTROY)
                            m_lib->resolve("VDADecoderDestroy");
        decoderConfigWidth = (CFStringRef*)
                            m_lib->resolve("kVDADecoderConfiguration_Width");
        decoderConfigHeight = (CFStringRef*)
                            m_lib->resolve("kVDADecoderConfiguration_Height");
        decoderConfigSourceFmt = (CFStringRef*)
                            m_lib->resolve("kVDADecoderConfiguration_SourceFormat");
        decoderConfigAVCCData = (CFStringRef*)
                            m_lib->resolve("kVDADecoderConfiguration_avcCData");
    }

    if (decoderCreate && decoderDecode && decoderFlush && decoderDestroy &&
        decoderConfigHeight && decoderConfigWidth && decoderConfigSourceFmt &&
        decoderConfigAVCCData && m_lib->isLoaded())
    {
        m_valid = true;
        LOG(VB_PLAYBACK, LOG_INFO, LOC +
            "Loaded VideoDecodeAcceleration library.");
    }
    else
        LOG(VB_GENERAL, LOG_ERR, LOC +
            "Failed to load VideoDecodeAcceleration library.");
}

#define INIT_ST OSStatus vda_st; bool ok = true
#define CHECK_ST \
    ok &= (vda_st == kVDADecoderNoErr); \
    if (!ok) \
        LOG(VB_GENERAL, LOG_ERR, LOC + QString("Error at %1:%2 (#%3, %4)") \
              .arg(__FILE__).arg( __LINE__).arg(vda_st) \
              .arg(vda_err_to_string(vda_st)))

QString vda_err_to_string(OSStatus err)
{
    switch (err)
    {
        case kVDADecoderHardwareNotSupportedErr:
            return "Hardware not supported";
        case kVDADecoderFormatNotSupportedErr:
            return "Format not supported";
        case kVDADecoderConfigurationError:
            return "Configuration error";
        case kVDADecoderDecoderFailedErr:
            return "Decoder failed";
        case paramErr:
            return "Parameter error";
    }
    return "Unknown error";
}

void PrivateDecoderVDA::GetDecoders(render_opts &opts)
{
    opts.decoders->append("vda");
    (*opts.equiv_decoders)["vda"].append("nuppel");
    (*opts.equiv_decoders)["vda"].append("ffmpeg");
    (*opts.equiv_decoders)["vda"].append("dummy");
}

PrivateDecoderVDA::PrivateDecoderVDA()
  : PrivateDecoder(), m_lib(NULL), m_decoder(NULL), m_size(QSize()),
    m_frame_lock(QMutex::Recursive), m_frames_decoded(0), m_num_ref_frames(0),
    m_annexb(false), m_slice_count(0)
{        
}

PrivateDecoderVDA::~PrivateDecoderVDA()
{
    Reset();
    if (m_decoder)
    {
        INIT_ST;
        vda_st = m_lib->decoderDestroy((VDADecoder)m_decoder);
        CHECK_ST;
    }
    m_decoder = NULL;
}

bool PrivateDecoderVDA::Init(const QString &decoder,
                             PlayerFlags flags,
                             AVCodecContext *avctx)
{
    if ((decoder != "vda") || (avctx->codec_id != CODEC_ID_H264) ||
        !(flags & kDecodeAllowEXT) || !avctx)
        return false;

    m_lib = VDALibrary::GetVDALibrary();
    if (!m_lib)
        return false;

    bool valid_extradata = true;
    uint8_t *extradata = avctx->extradata;
    int extradata_size = avctx->extradata_size;
    if (!extradata || extradata_size < 7)
        valid_extradata = false;

    CFDataRef avc_cdata = NULL;
    if (valid_extradata && extradata[0] != 1)
    {
        if (!RewriteAvcc(&extradata, extradata_size, avc_cdata))
        {
            LOG(VB_GENERAL, LOG_ERR, LOC + "Failed to re-write avcc...");
            valid_extradata = false;
        }
        else
        {
            m_annexb = true;
        }
    }
    else
    {
        avc_cdata = CFDataCreate(kCFAllocatorDefault, (const uint8_t*)extradata,
                                 extradata_size);   
    }

    if (!valid_extradata)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "Invalid avcC atom data");
        return false;
    }

    OSType format = 'avc1';
    int32_t width  = avctx->coded_width;
    int32_t height = avctx->coded_height;
    m_size = QSize(width, height);
    m_num_ref_frames = avctx->refs;
    m_slice_count    = avctx->slice_count;

    int mbs = ceil((double)width / 16.0f);
    if (((mbs == 49)  || (mbs == 54 ) || (mbs == 59 ) || (mbs == 64) ||
         (mbs == 113) || (mbs == 118) || (mbs == 123) || (mbs == 128)))
    {
        LOG(VB_PLAYBACK, LOG_WARNING, LOC +
            QString("Warning: VDA decoding may not be supported for this "
                    "video stream (width %1)").arg(width));
    }

    CFMutableDictionaryRef destinationImageBufferAttributes =
        CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
                                  &kCFTypeDictionaryKeyCallBacks,
                                  &kCFTypeDictionaryValueCallBacks);
#ifdef USING_QUARTZ_VIDEO
    OSType cvPixelFormatType = k422YpCbCr8PixelFormat;
#else
    OSType cvPixelFormatType = kCVPixelFormatType_422YpCbCr8;
#endif
    CFNumberRef pixelFormat  = CFNumberCreate(kCFAllocatorDefault,
                                              kCFNumberSInt32Type,
                                              &cvPixelFormatType);
    CFDictionarySetValue(destinationImageBufferAttributes,
                         kCVPixelBufferPixelFormatTypeKey,
                         pixelFormat);

    //CFDictionaryRef emptyDictionary =
    //    CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0,
    //                       &kCFTypeDictionaryKeyCallBacks,
    //                       &kCFTypeDictionaryValueCallBacks);
    //CFDictionarySetValue(destinationImageBufferAttributes,
    //                     kCVPixelBufferIOSurfacePropertiesKey,
    //                     emptyDictionary);

    CFMutableDictionaryRef decoderConfig =
                            CFDictionaryCreateMutable(
                                            kCFAllocatorDefault, 4,
                                            &kCFTypeDictionaryKeyCallBacks,
                                            &kCFTypeDictionaryValueCallBacks);
    CFNumberRef avc_width  = CFNumberCreate(kCFAllocatorDefault,
                                            kCFNumberSInt32Type, &width);
    CFNumberRef avc_height = CFNumberCreate(kCFAllocatorDefault,
                                            kCFNumberSInt32Type, &height);
    CFNumberRef avc_format = CFNumberCreate(kCFAllocatorDefault,
                                            kCFNumberSInt32Type, &format);

    CFDictionarySetValue(decoderConfig, *m_lib->decoderConfigHeight,    avc_height);
    CFDictionarySetValue(decoderConfig, *m_lib->decoderConfigWidth,     avc_width);
    CFDictionarySetValue(decoderConfig, *m_lib->decoderConfigSourceFmt, avc_format);
    CFDictionarySetValue(decoderConfig, *m_lib->decoderConfigAVCCData,  avc_cdata);
    CFRelease(avc_width);
    CFRelease(avc_height);
    CFRelease(avc_format);
    CFRelease(avc_cdata);

    INIT_ST;
    vda_st = m_lib->decoderCreate(decoderConfig, destinationImageBufferAttributes,
                                  (VDADecoderOutputCallback*)VDADecoderCallback,
                                  this, (VDADecoder*)&m_decoder);
    CHECK_ST;
    CFRelease(decoderConfig);
    CFRelease(destinationImageBufferAttributes);
    //CFRelease(emptyDictionary);
    if (ok)
    {
        LOG(VB_PLAYBACK, LOG_INFO, LOC +
            QString("Created VDA decoder: Size %1x%2 Ref Frames %3 "
                    "Slices %4 AnnexB %5")
            .arg(width).arg(height).arg(m_num_ref_frames).arg(m_slice_count)
            .arg(m_annexb ? "Yes" : "No"));
    }
    return ok;
}

bool PrivateDecoderVDA::Reset(void)
{
    if (m_lib && m_decoder)
        m_lib->decoderFlush((VDADecoder)m_decoder, 0 /*dont emit*/);

    m_frames_decoded = 0;
    m_frame_lock.lock();
    while (!m_decoded_frames.empty())
        PopDecodedFrame();
    m_frame_lock.unlock();
    return true;
}

void PrivateDecoderVDA::PopDecodedFrame(void)
{
    QMutexLocker lock(&m_frame_lock);
    if (m_decoded_frames.isEmpty())
        return;
    CVPixelBufferRelease(m_decoded_frames.last().buffer);    
    m_decoded_frames.removeLast();
}

bool PrivateDecoderVDA::HasBufferedFrames(void)
{
    m_frame_lock.lock();
    bool result = m_decoded_frames.size() > 0;
    m_frame_lock.unlock();
    return result;
}

int  PrivateDecoderVDA::GetFrame(AVStream *stream,
                                 AVFrame *picture,
                                 int *got_picture_ptr,
                                 AVPacket *pkt)
{
    if (!pkt)
        
    CocoaAutoReleasePool pool;
    int result = -1;
    if (!m_lib || !stream)
        return result;

    AVCodecContext *avctx = stream->codec;
    if (!avctx)
        return result;

    if (pkt)
    {
        CFDataRef data;
        CFDictionaryRef params;
        if (m_annexb)
        {
            if (!RewritePacket(pkt->data, pkt->size, data))
                return -1;
        }
        else
            data = CFDataCreate(kCFAllocatorDefault, pkt->data, pkt->size);

        CFStringRef keys[4] = { CFSTR("FRAME_PTS"),
                                CFSTR("FRAME_INTERLACED"), CFSTR("FRAME_TFF"),
                                CFSTR("FRAME_REPEAT") };
        CFNumberRef values[5];
        values[0] = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type,
                                   &pkt->pts);
        values[1] = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt8Type,
                                   &picture->interlaced_frame);
        values[2] = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt8Type,
                                   &picture->top_field_first);
        values[3] = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt8Type,
                                   &picture->repeat_pict);
        params = CFDictionaryCreate(kCFAllocatorDefault, (const void **)&keys,
                                    (const void **)&values, 4,
                                    &kCFTypeDictionaryKeyCallBacks,
                                    &kCFTypeDictionaryValueCallBacks);

        INIT_ST;
        vda_st = m_lib->decoderDecode((VDADecoder)m_decoder, 0, data, params);
        CHECK_ST;
        if (vda_st == kVDADecoderNoErr)
            result = pkt->size;
        CFRelease(data);
        CFRelease(params);
    }

    if (m_decoded_frames.size() < m_num_ref_frames)
        return result;

    *got_picture_ptr = 1;
    m_frame_lock.lock();
    VDAFrame vdaframe = m_decoded_frames.takeLast();
    m_frame_lock.unlock();

    if (avctx->get_buffer(avctx, picture) < 0)
        return -1;

    picture->reordered_opaque = vdaframe.pts;
    picture->interlaced_frame = vdaframe.interlaced_frame;
    picture->top_field_first  = vdaframe.top_field_first;
    picture->repeat_pict      = vdaframe.repeat_pict;
    VideoFrame *frame         = (VideoFrame*)picture->opaque;

    PixelFormat in_fmt  = PIX_FMT_NONE;
    PixelFormat out_fmt = PIX_FMT_NONE;
    if (vdaframe.format == 'BGRA')
        in_fmt = PIX_FMT_BGRA;
    else if (vdaframe.format == '2vuy')
        in_fmt = PIX_FMT_UYVY422;

    if (frame->codec == FMT_YV12)
        out_fmt = PIX_FMT_YUV420P;

    if (out_fmt != PIX_FMT_NONE && in_fmt != PIX_FMT_NONE && frame->buf)
    {
        CVPixelBufferLockBaseAddress(vdaframe.buffer, 0);
        uint8_t* base = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(vdaframe.buffer, 0);
        AVPicture img_in, img_out;
        avpicture_fill(&img_out, (uint8_t *)frame->buf, out_fmt,
                       frame->width, frame->height);
        avpicture_fill(&img_in, base, in_fmt,
                       frame->width, frame->height);
        myth_sws_img_convert(&img_out, out_fmt, &img_in, in_fmt,
                       frame->width, frame->height);
        CVPixelBufferUnlockBaseAddress(vdaframe.buffer, 0);
    }
    else
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "Failed to convert decoded frame.");
    }

    CVPixelBufferRelease(vdaframe.buffer);
    return result;
}

void PrivateDecoderVDA::VDADecoderCallback(void *decompressionOutputRefCon,
                                           CFDictionaryRef frameInfo,
                                           OSStatus status,
                                           uint32_t infoFlags,
                                           CVImageBufferRef imageBuffer)
{
    CocoaAutoReleasePool pool;
    PrivateDecoderVDA *decoder = (PrivateDecoderVDA*)decompressionOutputRefCon;

    if (kVDADecodeInfo_FrameDropped & infoFlags)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "Callback: Decoder dropped frame");
        return;
    }
    
    if (!imageBuffer)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC +
            "Callback: decoder returned empty buffer.");
        return;
    }

    INIT_ST;
    vda_st = status;
    CHECK_ST;

    OSType format_type = CVPixelBufferGetPixelFormatType(imageBuffer);
    if ((format_type != '2vuy') && (format_type != 'BGRA'))
    {
        LOG(VB_GENERAL, LOG_ERR, LOC +
            QString("Callback: image buffer format unknown (%1)")
                .arg(format_type));
        return;
    }

    int64_t pts = AV_NOPTS_VALUE;
    int8_t interlaced = 0;
    int8_t topfirst   = 0;
    int8_t repeatpic  = 0;
    CFNumberRef ptsref = (CFNumberRef)CFDictionaryGetValue(frameInfo,
                                                   CFSTR("FRAME_PTS"));
    CFNumberRef intref = (CFNumberRef)CFDictionaryGetValue(frameInfo,
                                                   CFSTR("FRAME_INTERLACED"));
    CFNumberRef topref = (CFNumberRef)CFDictionaryGetValue(frameInfo,
                                                   CFSTR("FRAME_TFF"));
    CFNumberRef repref = (CFNumberRef)CFDictionaryGetValue(frameInfo,
                                                   CFSTR("FRAME_REPEAT"));

    if (ptsref)
    {
        CFNumberGetValue(ptsref, kCFNumberSInt64Type, &pts);
        CFRelease(ptsref);
    }
    if (intref)
    {
        CFNumberGetValue(intref, kCFNumberSInt8Type, &interlaced);
        CFRelease(intref);
    }
    if (topref)
    {
        CFNumberGetValue(topref, kCFNumberSInt8Type, &topfirst);
        CFRelease(topref);
    }
    if (repref)
    {
        CFNumberGetValue(repref, kCFNumberSInt8Type, &repeatpic);
        CFRelease(repref);
    }

    int64_t time =  (pts != (int64_t)AV_NOPTS_VALUE) ? pts : 0;
    {
        QMutexLocker lock(&decoder->m_frame_lock);
        bool found = false;
        int i = 0;
        for (; i < decoder->m_decoded_frames.size(); i++)
        {
            int64_t pts = decoder->m_decoded_frames[i].pts;
            if (pts != (int64_t)AV_NOPTS_VALUE && time > pts)
            {
                found = true;
                break;
            }
        }

        VDAFrame frame(CVPixelBufferRetain(imageBuffer), format_type,
                       pts, interlaced, topfirst, repeatpic);
        if (!found)
            i = decoder->m_decoded_frames.size();
        decoder->m_decoded_frames.insert(i, frame);
        decoder->m_frames_decoded++;
    }
}

bool PrivateDecoderVDA::RewriteAvcc(uint8_t **data, int &len,
                                    CFDataRef &data_out)
{
    if (len < 7)
        return false;

    uint32_t sync = 0xffffffff;
    const uint8_t *next  = NULL;
    const uint8_t *begin = *data;
    const uint8_t *end   = begin + len;
    const uint8_t *sps = NULL, *spsend = NULL;
    const uint8_t *pps = NULL, *ppsend = NULL;
    uint8_t this_nal = 0;

    while (begin < end)
    {
        begin = ff_find_start_code(begin, end, &sync);
        if ((sync & 0xffffff00) != 0x00000100)
            continue;

        this_nal = *(begin -1) & 0x1f;
        next = ff_find_start_code(begin, end, &sync);

        if (this_nal == 7 && next)
        {
            sps = begin - 1;
            spsend = next;
        }
        if (this_nal == 8 && next)
        {
            pps = begin - 1;
            ppsend = next;
        }
    }

    if (!sps || !spsend)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "Failed to find sps.");
        return false;
    }

    int spssize = spsend - sps;
    int ppssize = pps ? ppsend - pps : 0;
    ByteIOContext *pb;
    if (url_open_dyn_buf(&pb) < 0)
        return false;

    put_byte(pb, 1);      /* version */
    put_byte(pb, sps[1]); /* profile */
    put_byte(pb, sps[2]); /* profile compat */
    put_byte(pb, sps[3]); /* level */
    put_byte(pb, 0xff);   /* 6 bits reserved (111111) + 2 bits nal size length - 1 (11) */
    put_byte(pb, 0xe1);   /* 3 bits reserved (111) + 5 bits number of sps (00001) */

    put_be16(pb, spssize);
    put_buffer(pb, sps, spssize);
    if (pps)
    {
        put_byte(pb, 1); /* number of pps */
        put_be16(pb, ppssize);
        put_buffer(pb, pps, ppssize);
    }
    *data = NULL;
    len = url_close_dyn_buf(pb, data);
    data_out = CFDataCreate(kCFAllocatorDefault,
                            (const uint8_t*)*data, len);
    av_free(*data);
    return true;
}

bool PrivateDecoderVDA::RewritePacket(uint8_t *data, int len,
                                      CFDataRef &data_out)
{
    ByteIOContext *pb;
    if (url_open_dyn_buf(&pb) < 0)
        return false;

    uint32_t sync = 0xffffffff;
    const uint8_t *next  = NULL;
    const uint8_t *begin = data;
    const uint8_t *end   = begin + len;
    begin = ff_find_start_code(begin, end, &sync);
    while (begin < end)
    {
        const uint8_t* this_start = begin - 1;
        next = ff_find_start_code(begin, end, &sync);
        const uint8_t* this_end = next;
        if (this_end != end)
        {
            while (((this_end - data) > 4) && *(this_end - 5) == 0)
                this_end--;
            this_end -= 4;
        }
        int size = this_end - this_start;
        put_be32(pb, size);
        put_buffer(pb, this_start, size);
        begin = next;
    }
    uint8_t *newpkt;
    int size = url_close_dyn_buf(pb, &newpkt);
    data_out = CFDataCreate(kCFAllocatorDefault, newpkt, size);
    av_free(newpkt);
    return true;
}

#ifndef PRIVATEDECODER_VDA_H
#define PRIVATEDECODER_VDA_H

#include <QSize>
#include <QLibrary>
#include <QList>

#import  "CoreFoundation/CoreFoundation.h"
#ifdef USING_QUARTZ_VIDEO
#import  "QuartzCore/CoreVideo.h"
#else
#import  "CoreVideo/CoreVideo.h"
#endif
#include "privatedecoder_vda_defs.h"
#include "privatedecoder.h"

class VDALibrary
{
  public:
    static VDALibrary* GetVDALibrary(void);
    VDALibrary();
    bool IsValid(void) const { return m_valid; }

    MYTH_VDADECODERCREATE  decoderCreate;
    MYTH_VDADECODERDECODE  decoderDecode;
    MYTH_VDADECODERFLUSH   decoderFlush;
    MYTH_VDADECODERDESTROY decoderDestroy;
    CFStringRef           *decoderConfigWidth;
    CFStringRef           *decoderConfigHeight;
    CFStringRef           *decoderConfigSourceFmt;
    CFStringRef           *decoderConfigAVCCData;
    QLibrary *m_lib;
    bool      m_valid;
};

class VDAFrame
{
  public:
    VDAFrame(CVPixelBufferRef buf, FourCharCode fmt, int64_t pres,
             int8_t interlaced, int8_t top_field, int8_t repeat)
      : buffer(buf), format(fmt), pts(pres),
        interlaced_frame(interlaced), top_field_first(top_field),
        repeat_pict(repeat) { }

    CVPixelBufferRef buffer;
    FourCharCode     format;
    int64_t          pts;
    int8_t           interlaced_frame;
    int8_t           top_field_first;
    int8_t           repeat_pict;
};

class PrivateDecoderVDA : public PrivateDecoder
{
  public:
    static void GetDecoders(render_opts &opts);
    PrivateDecoderVDA();
    virtual ~PrivateDecoderVDA();
    virtual QString GetName(void) { return "vda"; }
    virtual bool Init(const QString &decoder,
                      PlayerFlags flags,
                      AVCodecContext *avctx);
    virtual bool Reset(void);
    virtual int  GetFrame(AVStream *stream,
                          AVFrame *picture,
                          int *got_picture_ptr,
                          AVPacket *pkt);
    virtual bool HasBufferedFrames(void);
    virtual bool NeedsReorderedPTS(void) { return true; }

    static void VDADecoderCallback(void *decompressionOutputRefCon,
                                   CFDictionaryRef frameInfo,
                                   OSStatus status,
                                   uint32_t infoFlags,
                                   CVImageBufferRef imageBuffer);

  protected:
    void PopDecodedFrame(void);
    bool RewriteAvcc(uint8_t **data, int &len, CFDataRef &data_out);
    bool RewritePacket(uint8_t *data, int len, CFDataRef &data_out);

    VDALibrary     *m_lib;
    VDADecoder     *m_decoder;
    QSize           m_size;
    QMutex          m_frame_lock;
    int32_t         m_frames_decoded;
    int32_t         m_num_ref_frames;
    QList<VDAFrame> m_decoded_frames;
    bool            m_annexb;
    uint32_t        m_slice_count;
};

#endif // PRIVATEDECODER_VDA_H

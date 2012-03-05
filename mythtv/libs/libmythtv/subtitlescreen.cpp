#include <QFontMetrics>

#include "mythlogging.h"
#include "mythfontproperties.h"
#include "mythuisimpletext.h"
#include "mythuishape.h"
#include "mythuiimage.h"
#include "mythpainter.h"
#include "subtitlescreen.h"
#include "bdringbuffer.h"

#define LOC      QString("Subtitles: ")
#define LOC_WARN QString("Subtitles Warning: ")
static const float PAD_WIDTH  = 0.5;
static const float PAD_HEIGHT = 0.04;

// Normal font height is designed to be 1/20 of safe area height, with
// extra blank space between lines to make 17 lines within the safe
// area.
static const float LINE_SPACING = (20.0 / 17.0);

SubtitleScreen::SubtitleScreen(MythPlayer *player, const char * name,
                               int fontStretch) :
    MythScreenType((MythScreenType*)NULL, name),
    m_player(player),  m_subreader(NULL),   m_608reader(NULL),
    m_708reader(NULL), m_safeArea(QRect()), m_useBackground(false),
    m_removeHTML(QRegExp("</?.+>")),        m_subtitleType(kDisplayNone),
    m_textFontZoom(100),                    m_refreshArea(false),
    m_fontStretch(fontStretch),
    m_fontsAreInitialized(false)
{
    m_removeHTML.setMinimal(true);

#ifdef USING_LIBASS
    m_assLibrary   = NULL;
    m_assRenderer  = NULL;
    m_assTrackNum  = -1;
    m_assTrack     = NULL;
    m_assFontCount = 0;
#endif
}

SubtitleScreen::~SubtitleScreen(void)
{
    ClearAllSubtitles();
#ifdef USING_LIBASS
    CleanupAssLibrary();
#endif
}

void SubtitleScreen::EnableSubtitles(int type, bool forced_only)
{
    if (forced_only)
    {
        SetVisible(true);
        SetArea(MythRect());
        return;
    }

    m_subtitleType = type;
    if (m_subreader)
    {
        m_subreader->EnableAVSubtitles(kDisplayAVSubtitle == m_subtitleType);
        m_subreader->EnableTextSubtitles(kDisplayTextSubtitle == m_subtitleType);
        m_subreader->EnableRawTextSubtitles(kDisplayRawTextSubtitle == m_subtitleType);
    }
    if (m_608reader)
        m_608reader->SetEnabled(kDisplayCC608 == m_subtitleType);
    if (m_708reader)
        m_708reader->SetEnabled(kDisplayCC708 == m_subtitleType);
    ClearAllSubtitles();
    SetVisible(m_subtitleType != kDisplayNone);
    SetArea(MythRect());
}

void SubtitleScreen::DisableForcedSubtitles(void)
{
    if (kDisplayNone != m_subtitleType)
        return;
    ClearAllSubtitles();
    SetVisible(false);
    SetArea(MythRect());
}

bool SubtitleScreen::Create(void)
{
    if (!m_player)
        return false;

    m_subreader = m_player->GetSubReader();
    m_608reader = m_player->GetCC608Reader();
    m_708reader = m_player->GetCC708Reader();
    if (!m_subreader)
        LOG(VB_GENERAL, LOG_WARNING, LOC + "Failed to get subtitle reader.");
    if (!m_608reader)
        LOG(VB_GENERAL, LOG_WARNING, LOC + "Failed to get CEA-608 reader.");
    if (!m_708reader)
        LOG(VB_GENERAL, LOG_WARNING, LOC + "Failed to get CEA-708 reader.");
    m_useBackground = (bool)gCoreContext->GetNumSetting("CCBackground", 0);
    m_textFontZoom  = gCoreContext->GetNumSetting("OSDCC708TextZoom", 100);

    QString defaultFont =
        gCoreContext->GetSetting("DefaultSubtitleFont", "FreeMono");
    m_fontNames.append(defaultFont);       // default
    m_fontNames.append("FreeMono");        // mono serif
    m_fontNames.append("DejaVu Serif");    // prop serif
    m_fontNames.append("Droid Sans Mono"); // mono sans
    m_fontNames.append("Liberation Sans"); // prop sans
    m_fontNames.append("Purisa");          // casual
    m_fontNames.append("URW Chancery L");  // cursive
    m_fontNames.append("Impact");          // capitals

    return true;
}

void SubtitleScreen::Pulse(void)
{
    ExpireSubtitles();

    DisplayAVSubtitles(); // allow forced subtitles to work

    if (kDisplayTextSubtitle == m_subtitleType)
        DisplayTextSubtitles();
    else if (kDisplayCC608 == m_subtitleType)
        DisplayCC608Subtitles();
    else if (kDisplayCC708 == m_subtitleType)
        DisplayCC708Subtitles();
    else if (kDisplayRawTextSubtitle == m_subtitleType)
        DisplayRawTextSubtitles();

    OptimiseDisplayedArea();
    m_refreshArea = false;
}

void SubtitleScreen::ClearAllSubtitles(void)
{
    ClearNonDisplayedSubtitles();
    ClearDisplayedSubtitles();
#ifdef USING_LIBASS
    if (m_assTrack)
        ass_flush_events(m_assTrack);
#endif
}

void SubtitleScreen::ClearNonDisplayedSubtitles(void)
{
    if (m_subreader && (kDisplayAVSubtitle == m_subtitleType))
        m_subreader->ClearAVSubtitles();
    if (m_subreader && (kDisplayRawTextSubtitle == m_subtitleType))
        m_subreader->ClearRawTextSubtitles();
    if (m_608reader && (kDisplayCC608 == m_subtitleType))
        m_608reader->ClearBuffers(true, true);
    if (m_708reader && (kDisplayCC708 == m_subtitleType))
        m_708reader->ClearBuffers();
}

void SubtitleScreen::ClearDisplayedSubtitles(void)
{
    for (int i = 0; i < 8; i++)
        Clear708Cache(i);
    DeleteAllChildren();
    m_expireTimes.clear();
    SetRedraw();
}

void SubtitleScreen::ExpireSubtitles(void)
{
    VideoOutput    *videoOut = m_player->GetVideoOutput();
    VideoFrame *currentFrame = videoOut ? videoOut->GetLastShownFrame() : NULL;
    long long now = currentFrame ? currentFrame->timecode : LLONG_MAX;
    QMutableHashIterator<MythUIType*, long long> it(m_expireTimes);
    while (it.hasNext())
    {
        it.next();
        if (it.value() < now)
        {
            DeleteChild(it.key());
            it.remove();
            SetRedraw();
        }
    }
}

void SubtitleScreen::OptimiseDisplayedArea(void)
{
    if (!m_refreshArea)
        return;

    QRegion visible;
    QListIterator<MythUIType *> i(m_ChildrenList);
    while (i.hasNext())
    {
        MythUIType *img = i.next();
        visible = visible.united(img->GetArea());
    }

    if (visible.isEmpty())
        return;

    QRect bounding  = visible.boundingRect();
    bounding = bounding.translated(m_safeArea.topLeft());
    bounding = m_safeArea.intersected(bounding);
    int left = m_safeArea.left() - bounding.left();
    int top  = m_safeArea.top()  - bounding.top();
    SetArea(MythRect(bounding));

    i.toFront();
    while (i.hasNext())
    {
        MythUIType *img = i.next();
        img->SetArea(img->GetArea().translated(left, top));
    }
}

void SubtitleScreen::DisplayAVSubtitles(void)
{
    if (!m_player || !m_subreader)
        return;

    AVSubtitles* subs = m_subreader->GetAVSubtitles();
    QMutexLocker lock(&(subs->lock));
    if (subs->buffers.empty() && (kDisplayAVSubtitle != m_subtitleType))
        return;

    VideoOutput    *videoOut = m_player->GetVideoOutput();
    VideoFrame *currentFrame = videoOut ? videoOut->GetLastShownFrame() : NULL;

    if (!currentFrame || !videoOut)
        return;

    float tmp = 0.0;
    QRect dummy;
    videoOut->GetOSDBounds(dummy, m_safeArea, tmp, tmp, tmp);

    while (!subs->buffers.empty())
    {
        const AVSubtitle subtitle = subs->buffers.front();
        if (subtitle.start_display_time > currentFrame->timecode)
            break;

        ClearDisplayedSubtitles();
        subs->buffers.pop_front();
        for (std::size_t i = 0; i < subtitle.num_rects; ++i)
        {
            AVSubtitleRect* rect = subtitle.rects[i];

            bool displaysub = true;
            if (subs->buffers.size() > 0 &&
                subs->buffers.front().end_display_time <
                currentFrame->timecode)
            {
                displaysub = false;
            }

            if (displaysub && rect->type == SUBTITLE_BITMAP)
            {
                // AVSubtitleRect's image data's not guaranteed to be 4 byte
                // aligned.

                QSize img_size(rect->w, rect->h);
                QRect img_rect(rect->x, rect->y, rect->w, rect->h);
                QRect display(rect->display_x, rect->display_y,
                              rect->display_w, rect->display_h);

                // XSUB and some DVD/DVB subs are based on the original video
                // size before the video was converted. We need to guess the
                // original size and allow for the difference

                int right  = rect->x + rect->w;
                int bottom = rect->y + rect->h;
                if (subs->fixPosition || (currentFrame->height < bottom) ||
                   (currentFrame->width  < right))
                {
                    int sd_height = 576;
                    if ((m_player->GetFrameRate() > 26.0f) && bottom <= 480)
                        sd_height = 480;
                    int height = ((currentFrame->height <= sd_height) &&
                                  (bottom <= sd_height)) ? sd_height :
                                 ((currentFrame->height <= 720) && bottom <= 720)
                                   ? 720 : 1080;
                    int width  = ((currentFrame->width  <= 720) &&
                                  (right <= 720)) ? 720 :
                                 ((currentFrame->width  <= 1280) &&
                                  (right <= 1280)) ? 1280 : 1920;
                    display = QRect(0, 0, width, height);
                }

                QRect scaled = videoOut->GetImageRect(img_rect, &display);
                QImage qImage(img_size, QImage::Format_ARGB32);
                for (int y = 0; y < rect->h; ++y)
                {
                    for (int x = 0; x < rect->w; ++x)
                    {
                        const uint8_t color = rect->pict.data[0][y*rect->pict.linesize[0] + x];
                        const uint32_t pixel = *((uint32_t*)rect->pict.data[1]+color);
                        qImage.setPixel(x, y, pixel);
                    }
                }

                if (scaled.size() != img_size)
                {
                    qImage = qImage.scaled(scaled.width(), scaled.height(),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                }

                MythPainter *osd_painter = videoOut->GetOSDPainter();
                MythImage* image = NULL;
                if (osd_painter)
                   image = osd_painter->GetFormatImage();

                long long displayfor = subtitle.end_display_time -
                                       subtitle.start_display_time;
                if (displayfor == 0)
                    displayfor = 60000;
                displayfor = (displayfor < 50) ? 50 : displayfor;
                long long late = currentFrame->timecode -
                                 subtitle.start_display_time;
                MythUIImage *uiimage = NULL;
                if (image)
                {
                    image->Assign(qImage);
                    QString name = QString("avsub%1").arg(i);
                    uiimage = new MythUIImage(this, name);
                    if (uiimage)
                    {
                        m_refreshArea = true;
                        uiimage->SetImage(image);
                        uiimage->SetArea(MythRect(scaled));
                        m_expireTimes.insert(uiimage,
                                     currentFrame->timecode + displayfor);
                    }
                }
                if (uiimage)
                {
                    LOG(VB_PLAYBACK, LOG_INFO, LOC +
                        QString("Display %1AV subtitle for %2ms")
                        .arg(subtitle.forced ? "FORCED " : "")
                        .arg(displayfor));
                    if (late > 50)
                        LOG(VB_PLAYBACK, LOG_INFO, LOC +
                            QString("AV Sub was %1ms late").arg(late));
                }
            }
#ifdef USING_LIBASS
            else if (displaysub && rect->type == SUBTITLE_ASS)
            {
                InitialiseAssTrack(m_player->GetDecoder()->GetTrack(kTrackTypeSubtitle));
                AddAssEvent(rect->ass);
            }
#endif
        }
        m_subreader->FreeAVSubtitle(subtitle);
    }
#ifdef USING_LIBASS
    RenderAssTrack(currentFrame->timecode);
#endif
}

void SubtitleScreen::DisplayTextSubtitles(void)
{
    if (!m_player || !m_subreader)
        return;

    bool changed = false;
    VideoOutput *vo = m_player->GetVideoOutput();
    if (vo)
    {
        QRect oldsafe = m_safeArea;
        m_safeArea = vo->GetSafeRect();
        changed = (oldsafe != m_safeArea);
        if (!InitializeFonts(changed))
            return;
    }
    else
    {
        return;
    }

    VideoFrame *currentFrame = vo->GetLastShownFrame();
    if (!currentFrame)
        return;

    TextSubtitles *subs = m_subreader->GetTextSubtitles();
    subs->Lock();
    uint64_t playPos = 0;
    if (subs->IsFrameBasedTiming())
    {
        // frame based subtitles get out of synch after running mythcommflag
        // for the file, i.e., the following number is wrong and does not
        // match the subtitle frame numbers:
        playPos = currentFrame->frameNumber;
    }
    else
    {
        // Use timecodes for time based SRT subtitles. Feeding this into
        // NormalizeVideoTimecode() should adjust for non-zero start times
        // and wraps. For MPEG, wraps will occur just once every 26.5 hours
        // and other formats less frequently so this should be sufficient.
        // Note: timecodes should now always be valid even in the case
        // when a frame doesn't have a valid timestamp. If an exception is
        // found where this is not true then we need to use the frameNumber
        // when timecode is not defined by uncommenting the following lines.
        //if (currentFrame->timecode == 0)
        //    playPos = (uint64_t)
        //        ((currentFrame->frameNumber / video_frame_rate) * 1000);
        //else
        playPos = m_player->GetDecoder()->NormalizeVideoTimecode(currentFrame->timecode);
    }
    if (playPos != 0)
        changed |= subs->HasSubtitleChanged(playPos);
    if (!changed)
    {
        subs->Unlock();
        return;
    }

    DeleteAllChildren();
    SetRedraw();
    if (playPos == 0)
    {
        subs->Unlock();
        return;
    }

    QStringList rawsubs = subs->GetSubtitles(playPos);
    if (rawsubs.empty())
    {
        subs->Unlock();
        return;
    }

    subs->Unlock();
    DrawTextSubtitles(rawsubs, 0, 0);
}

void SubtitleScreen::DisplayRawTextSubtitles(void)
{
    if (!m_player || !m_subreader)
        return;

    uint64_t duration;
    QStringList subs = m_subreader->GetRawTextSubtitles(duration);
    if (subs.empty())
        return;

    VideoOutput *vo = m_player->GetVideoOutput();
    if (!vo)
        return;

    VideoFrame *currentFrame = vo->GetLastShownFrame();
    if (!currentFrame)
        return;

    bool changed = false;
    QRect oldsafe = m_safeArea;
    m_safeArea = vo->GetSafeRect();
    changed = (oldsafe != m_safeArea);
    if (!InitializeFonts(changed))
        return;

    // delete old subs that may still be on screen
    DeleteAllChildren();
    DrawTextSubtitles(subs, currentFrame->timecode, duration);
}

void SubtitleScreen::DrawTextSubtitles(QStringList &wrappedsubs,
                                       uint64_t start, uint64_t duration)
{
    FormattedTextSubtitle fsub(m_safeArea, m_useBackground, this);
    fsub.InitFromSRT(wrappedsubs, m_textFontZoom);
    fsub.WrapLongLines();
    fsub.Layout();
    m_refreshArea = fsub.Draw(0, start, duration) || m_refreshArea;
}

void SubtitleScreen::DisplayDVDButton(AVSubtitle* dvdButton, QRect &buttonPos)
{
    if (!dvdButton || !m_player)
        return;

    VideoOutput *vo = m_player->GetVideoOutput();
    if (!vo)
        return;

    DeleteAllChildren();
    SetRedraw();

    float tmp = 0.0;
    QRect dummy;
    vo->GetOSDBounds(dummy, m_safeArea, tmp, tmp, tmp);

    AVSubtitleRect *hl_button = dvdButton->rects[0];
    uint h = hl_button->h;
    uint w = hl_button->w;
    QRect rect = QRect(hl_button->x, hl_button->y, w, h);
    QImage bg_image(hl_button->pict.data[0], w, h, w, QImage::Format_Indexed8);
    uint32_t *bgpalette = (uint32_t *)(hl_button->pict.data[1]);

    QVector<uint32_t> bg_palette(4);
    for (int i = 0; i < 4; i++)
        bg_palette[i] = bgpalette[i];
    bg_image.setColorTable(bg_palette);

    // copy button region of background image
    const QRect fg_rect(buttonPos.translated(-hl_button->x, -hl_button->y));
    QImage fg_image = bg_image.copy(fg_rect);
    QVector<uint32_t> fg_palette(4);
    uint32_t *fgpalette = (uint32_t *)(dvdButton->rects[1]->pict.data[1]);
    if (fgpalette)
    {
        for (int i = 0; i < 4; i++)
            fg_palette[i] = fgpalette[i];
        fg_image.setColorTable(fg_palette);
    }

    bg_image = bg_image.convertToFormat(QImage::Format_ARGB32);
    fg_image = fg_image.convertToFormat(QImage::Format_ARGB32);

    // set pixel of highlight area to highlight color
    for (int x=fg_rect.x(); x < fg_rect.x()+fg_rect.width(); ++x)
    {
        if ((x < 0) || (x > hl_button->w))
            continue;
        for (int y=fg_rect.y(); y < fg_rect.y()+fg_rect.height(); ++y)
        {
            if ((y < 0) || (y > hl_button->h))
                continue;
            bg_image.setPixel(x, y, fg_image.pixel(x-fg_rect.x(),y-fg_rect.y()));
        }
    }

    AddScaledImage(bg_image, rect);
}

/// Extract everything from the text buffer up until the next format
/// control character.  Return that substring, and remove it from the
/// input string.  Bogus control characters are left unchanged.  If the
/// buffer starts with a valid control character, the output parameters
/// are corresondingly updated (and the control character is stripped).
static QString extract_cc608(
    QString &text, bool teletextmode, int &color,
    bool &isItalic, bool &isUnderline)
{
    QString result;
    QString orig(text);

    if (teletextmode)
    {
        result = text;
        text = QString::null;
        return result;
    }

    // Handle an initial control sequence.
    if (text.length() >= 1 && text[0] >= 0x7000)
    {
        int op = text[0].unicode() - 0x7000;
        isUnderline = (op & 0x1);
        switch (op & ~1)
        {
        case 0x0e:
            // color unchanged
            isItalic = true;
            break;
        case 0x1e:
            color = op >> 1;
            isItalic = true;
            break;
        case 0x20:
            // color unchanged
            // italics unchanged
            break;
        default:
            color = (op & 0xf) >> 1;
            isItalic = false;
            break;
        }
        text = text.mid(1);
    }

    // Copy the string into the result, up to the next control
    // character.
    int nextControl = text.indexOf(QRegExp("[\\x7000-\\x7fff]"));
    if (nextControl < 0)
    {
        result = text;
        text = QString::null;
    }
    else
    {
        result = text.left(nextControl);
        // Print the space character before handling the next control
        // character, otherwise the space character will be lost due
        // to the text.trimmed() operation in the MythUISimpleText
        // constructor, combined with the left-justification of
        // captions.
        if (text[nextControl] < (0x7000 + 0x10))
            result += " ";
        text = text.mid(nextControl);
    }

    return result;
}

void SubtitleScreen::DisplayCC608Subtitles(void)
{
    if (!m_608reader)
        return;

    bool changed = false;

    if (m_player && m_player->GetVideoOutput())
    {
        QRect oldsafe = m_safeArea;
        m_safeArea = m_player->GetVideoOutput()->GetSafeRect();
        if (oldsafe != m_safeArea)
            changed = true;
    }
    else
    {
        return;
    }
    if (!InitializeFonts(changed))
        return;

    CC608Buffer* textlist = m_608reader->GetOutputText(changed);
    if (!changed)
        return;

    if (textlist)
        textlist->lock.lock();

    DeleteAllChildren();

    if (!textlist)
        return;

    if (textlist->buffers.empty())
    {
        SetRedraw();
        textlist->lock.unlock();
        return;
    }

    FormattedTextSubtitle fsub(m_safeArea, m_useBackground, this);
    fsub.InitFromCC608(textlist->buffers);
    fsub.Layout608();
    fsub.Layout();
    m_refreshArea = fsub.Draw() || m_refreshArea;
    textlist->lock.unlock();
}

void SubtitleScreen::DisplayCC708Subtitles(void)
{
    if (!m_708reader)
        return;

    CC708Service *cc708service = m_708reader->GetCurrentService();
    float video_aspect = 1.77777f;
    bool changed = false;
    if (m_player && m_player->GetVideoOutput())
    {
        video_aspect = m_player->GetVideoAspect();
        QRect oldsafe = m_safeArea;
        m_safeArea = m_player->GetVideoOutput()->GetSafeRect();
        changed = (oldsafe != m_safeArea);
        if (changed)
        {
            for (uint i = 0; i < 8; i++)
                cc708service->windows[i].changed = true;
        }
    }
    else
    {
        return;
    }

    if (!InitializeFonts(changed))
        return;

    for (uint i = 0; i < 8; i++)
    {
        CC708Window &win = cc708service->windows[i];
        if (win.exists && win.visible && !win.changed)
            continue;

        Clear708Cache(i);
        if (!win.exists || !win.visible)
            continue;

        QMutexLocker locker(&win.lock);
        vector<CC708String*> list = win.GetStrings();
        if (!list.empty())
        {
            FormattedTextSubtitle fsub(m_safeArea, m_useBackground, this);
            fsub.InitFromCC708(win, i, list, video_aspect, m_textFontZoom);
            fsub.Layout();
            // Draw the window background after calculating bounding
            // rectangle in Layout()
            if (win.GetFillAlpha()) // TODO border?
            {
                QBrush fill(win.GetFillColor(), Qt::SolidPattern);
                MythUIShape *shape =
                    new MythUIShape(this, QString("cc708bg%1").arg(i));
                shape->SetFillBrush(fill);
                shape->SetArea(MythRect(fsub.m_bounds));
                m_708imageCache[i].append(shape);
                m_refreshArea = true;
            }
            m_refreshArea =
                fsub.Draw(&m_708imageCache[i]) || m_refreshArea;
        }
        for (uint j = 0; j < list.size(); j++)
            delete list[j];
        win.changed = false;
    }
}

void SubtitleScreen::Clear708Cache(int num)
{
    if (!m_708imageCache[num].isEmpty())
    {
        foreach(MythUIType* image, m_708imageCache[num])
            DeleteChild(image);
        m_708imageCache[num].clear();
    }
}

void SubtitleScreen::AddScaledImage(QImage &img, QRect &pos)
{
    VideoOutput *vo = m_player->GetVideoOutput();
    if (!vo)
        return;

    QRect scaled = vo->GetImageRect(pos);
    if (scaled.size() != pos.size())
    {
        img = img.scaled(scaled.width(), scaled.height(),
                         Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    MythPainter *osd_painter = vo->GetOSDPainter();
    MythImage* image = NULL;
    if (osd_painter)
         image = osd_painter->GetFormatImage();

    if (image)
    {
        image->Assign(img);
        MythUIImage *uiimage = new MythUIImage(this, "dvd_button");
        if (uiimage)
        {
            m_refreshArea = true;
            uiimage->SetImage(image);
            uiimage->SetArea(MythRect(scaled));
        }
    }
}

bool SubtitleScreen::InitializeFonts(bool wasResized)
{
    bool success = true;

    if (!m_fontsAreInitialized)
    {
        LOG(VB_GENERAL, LOG_INFO, "InitializeFonts()");

        int count = 0;
        foreach(QString font, m_fontNames)
        {
            MythFontProperties *mythfont = new MythFontProperties();
            if (mythfont)
            {
                QFont newfont(font);
                newfont.setStretch(m_fontStretch);
                font.detach();
                mythfont->SetFace(newfont);
                m_fontSet.insert(count, mythfont);
                count++;
            }
        }
        success = count > 0;
        LOG(VB_PLAYBACK, LOG_INFO, LOC +
            QString("Loaded %1 CEA-708 fonts").arg(count));
    }

    if (wasResized || !m_fontsAreInitialized)
    {
        foreach(MythFontProperties* font, m_fontSet)
            font->face().setStretch(m_fontStretch);
        // XXX reset font sizes
    }

    m_fontsAreInitialized = true;
    return success;
}

MythFontProperties* SubtitleScreen::Get708Font(CC708CharacterAttribute attr)
    const
{
    MythFontProperties *mythfont = m_fontSet[attr.font_tag & 0x7];
    if (!mythfont)
        return NULL;

    mythfont->GetFace()->setItalic(attr.italics);
    mythfont->GetFace()->setPixelSize(m_708fontSizes[attr.pen_size & 0x3]);
    mythfont->GetFace()->setUnderline(attr.underline);
    mythfont->GetFace()->setBold(attr.boldface);
    mythfont->SetColor(attr.GetFGColor());

    int off = m_708fontSizes[attr.pen_size & 0x3] / 20;
    QPoint shadowsz(off, off);
    QColor colour = attr.GetEdgeColor();
    int alpha     = attr.GetFGAlpha();
    bool outline = false;
    bool shadow  = false;

    if (attr.edge_type == k708AttrEdgeLeftDropShadow)
    {
        shadow = true;
        shadowsz.setX(-off);
    }
    else if (attr.edge_type == k708AttrEdgeRightDropShadow)
    {
        shadow = true;
    }
    else if (attr.edge_type == k708AttrEdgeUniform ||
             attr.edge_type == k708AttrEdgeRaised  ||
             attr.edge_type == k708AttrEdgeDepressed)
    {
        outline = true;
    }

    mythfont->SetOutline(outline, colour, off, alpha);
    mythfont->SetShadow(shadow, shadowsz, colour, alpha);

    return mythfont;
}

static QString srtColorString(QColor color)
{
    return QString("#%1%2%3")
        .arg(color.red(),   2, 16, QLatin1Char('0'))
        .arg(color.green(), 2, 16, QLatin1Char('0'))
        .arg(color.blue(),  2, 16, QLatin1Char('0'));
}

void FormattedTextSubtitle::InitFromCC608(vector<CC608Text*> &buffers)
{
    static const QColor clr[8] =
    {
        Qt::white,   Qt::green,   Qt::blue,    Qt::cyan,
        Qt::red,     Qt::yellow,  Qt::magenta, Qt::white,
    };

    if (buffers.empty())
        return;
    vector<CC608Text*>::iterator i = buffers.begin();
    bool teletextmode = (*i)->teletextmode;
    bool useBackground = m_useBackground && !teletextmode;
    //int xscale = teletextmode ? 40 : 36;
    int yscale = teletextmode ? 25 : 17;
    int pixelSize = m_safeArea.height() / (yscale * LINE_SPACING) *
        (parent ? parent->m_textFontZoom : 100) / 100;
    if (parent)
        parent->SetFontSizes(pixelSize, pixelSize, pixelSize);

    for (; i != buffers.end(); ++i)
    {
        CC608Text *cc = (*i);
        int color = 0;
        bool isItalic = false, isUnderline = false;
        const bool isBold = false;
        QString text(cc->text);

        int orig_x = teletextmode ? cc->y : (cc->x + 3);
        int orig_y = teletextmode ? cc->x : cc->y;
        int y = (int)(((float)orig_y / (float)yscale) *
                      (float)m_safeArea.height());
        FormattedTextLine line(0, y, orig_x, orig_y);
        // Indented lines are handled as initial strings of space
        // characters, to improve vertical alignment.  Monospace fonts
        // are assumed.
        if (orig_x > 0)
        {
            CC708CharacterAttribute attr(false, false, false,
                                         Qt::white, useBackground);
            FormattedTextChunk chunk(QString(orig_x, ' '), attr, parent);
            line.chunks += chunk;
        }
        while (!text.isNull())
        {
            QString captionText =
                extract_cc608(text, cc->teletextmode,
                              color, isItalic, isUnderline);
            CC708CharacterAttribute attr(isItalic, isBold, isUnderline,
                                         clr[min(max(0, color), 7)],
                                         useBackground);
            FormattedTextChunk chunk(captionText, attr, parent);
            line.chunks += chunk;
            LOG(VB_VBI, LOG_INFO,
                QString("Adding cc608 chunk (%1,%2): %3")
                .arg(cc->x).arg(cc->y).arg(chunk.ToLogString()));
        }
        m_lines += line;
    }
}

void FormattedTextSubtitle::InitFromCC708(const CC708Window &win, int num,
                                          const vector<CC708String*> &list,
                                          float aspect,
                                          int textFontZoom)
{
    LOG(VB_VBI, LOG_INFO,LOC +
        QString("Display Win %1, Anchor_id %2, x_anch %3, y_anch %4, "
                "relative %5")
            .arg(num).arg(win.anchor_point).arg(win.anchor_horizontal)
            .arg(win.anchor_vertical).arg(win.relative_pos));
    int pixelSize = (m_safeArea.height() * textFontZoom) / 2000;
    if (parent)
        parent->SetFontSizes(pixelSize * 32 / 42, pixelSize, pixelSize * 42 / 32);

    float xrange  = win.relative_pos ? 100.0f :
                    (aspect > 1.4f) ? 210.0f : 160.0f;
    float yrange  = win.relative_pos ? 100.0f : 75.0f;
    float xmult   = (float)m_safeArea.width() / xrange;
    float ymult   = (float)m_safeArea.height() / yrange;
    uint anchor_x = (uint)(xmult * (float)win.anchor_horizontal);
    uint anchor_y = (uint)(ymult * (float)win.anchor_vertical);
    m_xAnchorPoint = win.anchor_point % 3;
    m_yAnchorPoint = win.anchor_point / 3;
    m_xAnchor = anchor_x;
    m_yAnchor = anchor_y;

    for (uint i = 0; i < list.size(); i++)
    {
        if (list[i]->y >= (uint)m_lines.size())
            m_lines.resize(list[i]->y + 1);
        if (m_useBackground)
        {
            list[i]->attr.bg_color = k708AttrColorBlack;
            list[i]->attr.bg_opacity = k708AttrOpacitySolid;
        }
        FormattedTextChunk chunk(list[i]->str, list[i]->attr, parent);
        m_lines[list[i]->y].chunks += chunk;
        LOG(VB_VBI, LOG_INFO, QString("Adding cc708 chunk: win %1 row %2: %3")
            .arg(num).arg(i).arg(chunk.ToLogString()));
    }
}

void FormattedTextSubtitle::InitFromSRT(QStringList &subs, int textFontZoom)
{
    // Does a simplistic parsing of HTML tags from the strings.
    // Nesting is not implemented.  Stray whitespace may cause
    // legitimate tags to be ignored.  All other HTML tags are
    // stripped and ignored.
    //
    // <i> - enable italics
    // </i> - disable italics
    // <b> - enable boldface
    // </b> - disable boldface
    // <u> - enable underline
    // </u> - disable underline
    // <font color="#xxyyzz"> - change font color
    // </font> - reset font color to white

    int pixelSize = (m_safeArea.height() * textFontZoom) / 2000;
    if (parent)
        parent->SetFontSizes(pixelSize, pixelSize, pixelSize);
    m_xAnchorPoint = 1; // center
    m_yAnchorPoint = 2; // bottom
    m_xAnchor = m_safeArea.width() / 2;
    m_yAnchor = m_safeArea.height();

    bool isItalic = false;
    bool isBold = false;
    bool isUnderline = false;
    QColor color(Qt::white);
    QRegExp htmlTag("</?.+>");
    QString htmlPrefix("<font color=\""), htmlSuffix("\">");
    htmlTag.setMinimal(true);
    foreach (QString subtitle, subs)
    {
        FormattedTextLine line;
        QString text(subtitle);
        while (!text.isEmpty())
        {
            int pos = text.indexOf(htmlTag);
            if (pos != 0) // don't add a zero-length string
            {
                CC708CharacterAttribute attr(isItalic, isBold, isUnderline,
                                             color, m_useBackground);
                FormattedTextChunk chunk(text.left(pos), attr, parent);
                line.chunks += chunk;
                text = (pos < 0 ? "" : text.mid(pos));
                LOG(VB_VBI, LOG_INFO, QString("Adding SRT chunk: %1")
                    .arg(chunk.ToLogString()));
            }
            if (pos >= 0)
            {
                int htmlLen = htmlTag.matchedLength();
                QString html = text.left(htmlLen).toLower();
                text = text.mid(htmlLen);
                if (html == "<i>")
                    isItalic = true;
                else if (html == "</i>")
                    isItalic = false;
                else if (html.startsWith(htmlPrefix) &&
                         html.endsWith(htmlSuffix))
                {
                    int colorLen = html.length() -
                        (htmlPrefix.length() + htmlSuffix.length());
                    QString colorString(
                        html.mid(htmlPrefix.length(), colorLen));
                    QColor newColor(colorString);
                    if (newColor.isValid())
                    {
                        color = newColor;
                    }
                    else
                    {
                        LOG(VB_VBI, LOG_INFO,
                            QString("Ignoring invalid SRT color specification: "
                                    "'%1'").arg(colorString));
                    }
                }
                else if (html == "</font>")
                    color = Qt::white;
                else if (html == "<b>")
                    isBold = true;
                else if (html == "</b>")
                    isBold = false;
                else if (html == "<u>")
                    isUnderline = true;
                else if (html == "</u>")
                    isUnderline = false;
                else
                {
                    LOG(VB_VBI, LOG_INFO,
                        QString("Ignoring unknown SRT formatting: '%1'")
                        .arg(html));
                }

                LOG(VB_VBI, LOG_INFO,
                    QString("SRT formatting change '%1', "
                            "new ital=%2 bold=%3 uline=%4 color=%5)")
                    .arg(html).arg(isItalic).arg(isBold).arg(isUnderline)
                    .arg(srtColorString(color)));
            }
        }
        m_lines += line;
    }
}

bool FormattedTextChunk::Split(FormattedTextChunk &newChunk)
{
    LOG(VB_VBI, LOG_INFO,
        QString("Attempting to split chunk '%1'").arg(text));
    int lastSpace = text.lastIndexOf(' ', -2); // -2 to ignore trailing space
    if (lastSpace < 0)
    {
        LOG(VB_VBI, LOG_INFO,
            QString("Failed to split chunk '%1'").arg(text));
        return false;
    }
    newChunk.format = format;
    newChunk.text = text.mid(lastSpace + 1).trimmed() + ' ';
    text = text.left(lastSpace).trimmed();
    LOG(VB_VBI, LOG_INFO,
        QString("Split chunk into '%1' + '%2'").arg(text).arg(newChunk.text));
    return true;
}

QString FormattedTextChunk::ToLogString(void) const
{
    QString str("");
    str += QString("fg=%1.%2 ")
        .arg(srtColorString(format.GetFGColor()))
        .arg(format.GetFGAlpha());
    str += QString("bg=%1.%2 ")
        .arg(srtColorString(format.GetBGColor()))
        .arg(format.GetBGAlpha());
    str += QString("edge=%1.%2 ")
        .arg(srtColorString(format.GetEdgeColor()))
        .arg(format.edge_type);
    str += QString("off=%1 pensize=%2 ")
        .arg(format.offset)
        .arg(format.pen_size);
    str += QString("it=%1 ul=%2 bf=%3 ")
        .arg(format.italics)
        .arg(format.underline)
        .arg(format.boldface);
    str += QString("font=%1 ").arg(format.font_tag);
    str += QString(" text='%1'").arg(text);
    return str;
}

void FormattedTextSubtitle::WrapLongLines(void)
{
    int maxWidth = m_safeArea.width();
    for (int i = 0; i < m_lines.size(); i++)
    {
        int width = m_lines[i].CalcSize().width();
        // Move entire chunks to the next line as necessary.  Leave at
        // least one chunk on the current line.
        while (width > maxWidth && m_lines[i].chunks.size() > 1)
        {
            width -= m_lines[i].chunks.back().CalcSize().width();
            // Make sure there's a next line to wrap into.
            if (m_lines.size() == i + 1)
                m_lines += FormattedTextLine(m_lines[i].x_indent,
                                             m_lines[i].y_indent);
            m_lines[i+1].chunks.prepend(m_lines[i].chunks.takeLast());
            LOG(VB_VBI, LOG_INFO,
                QString("Wrapping chunk to next line: '%1'")
                .arg(m_lines[i+1].chunks[0].text));
        }
        // Split the last chunk until width is small enough or until
        // no more splits are possible.
        bool isSplitPossible = true;
        while (width > maxWidth && isSplitPossible)
        {
            FormattedTextChunk newChunk;
            isSplitPossible = m_lines[i].chunks.back().Split(newChunk);
            if (isSplitPossible)
            {
                // Make sure there's a next line to split into.
                if (m_lines.size() == i + 1)
                    m_lines += FormattedTextLine(m_lines[i].x_indent,
                                                 m_lines[i].y_indent);
                m_lines[i+1].chunks.prepend(newChunk);
                width = m_lines[i].CalcSize().width();
            }
        }
    }
}

// Adjusts the Y coordinates to avoid overlap, which could happen as a
// result of a large text zoom factor.  Then, if the total height
// exceeds the safe area, compresses each piece of vertical blank
// space proportionally to make it fit.
void FormattedTextSubtitle::Layout608(void)
{
    int i;
    int totalHeight = 0;
    int totalSpace = 0;
    int firstY = 0;
    int prevY = 0;
    QVector<int> heights(m_lines.size());
    QVector<int> spaceBefore(m_lines.size());
    // Calculate totalHeight and totalSpace
    for (i = 0; i < m_lines.size(); i++)
    {
        m_lines[i].y_indent = max(m_lines[i].y_indent, prevY); // avoid overlap
        int y = m_lines[i].y_indent;
        if (i == 0)
            firstY = prevY = y;
        int height = m_lines[i].CalcSize().height();
        heights[i] = height;
        spaceBefore[i] = y - prevY;
        totalSpace += (y - prevY);
        prevY = y + height;
        totalHeight += height;
    }
    int safeHeight = m_safeArea.height();
    int overage = min(totalHeight - safeHeight, totalSpace);

    // Recalculate Y coordinates, applying the shrink factor to space
    // between each line.
    if (overage > 0 && totalSpace > 0)
    {
        float shrink = (totalSpace - overage) / (float)totalSpace;
        prevY = firstY;
        for (i = 0; i < m_lines.size(); i++)
        {
            m_lines[i].y_indent = prevY + spaceBefore[i] * shrink;
            prevY = m_lines[i].y_indent + heights[i];
        }
    }

    // Shift Y coordinates back up into the safe area.
    int shift = min(firstY, max(0, prevY - safeHeight));
    for (i = 0; i < m_lines.size(); i++)
        m_lines[i].y_indent -= shift;
}

// Resolves any TBD x_indent and y_indent values in FormattedTextLine
// objects.  Calculates m_bounds.  Prunes most leading and all
// trailing whitespace from each line so that displaying with a black
// background doesn't look clumsy.
void FormattedTextSubtitle::Layout(void)
{
    // Calculate dimensions of bounding rectangle
    int anchor_width = 0;
    int anchor_height = 0;
    for (int i = 0; i < m_lines.size(); i++)
    {
        QSize sz = m_lines[i].CalcSize(LINE_SPACING);
        anchor_width = max(anchor_width, sz.width());
        anchor_height += sz.height();
    }

    // Adjust the anchor point according to actual width and height
    int anchor_x = m_xAnchor;
    int anchor_y = m_yAnchor;
    if (m_xAnchorPoint == 1)
        anchor_x -= anchor_width / 2;
    else if (m_xAnchorPoint == 2)
        anchor_x -= anchor_width;
    if (m_yAnchorPoint == 1)
        anchor_y -= anchor_height / 2;
    else if (m_yAnchorPoint == 2)
        anchor_y -= anchor_height;

    // Shift the anchor point back into the safe area if necessary/possible.
    anchor_y = max(0, min(anchor_y, m_safeArea.height() - anchor_height));
    anchor_x = max(0, min(anchor_x, m_safeArea.width() - anchor_width));

    m_bounds = QRect(anchor_x, anchor_y, anchor_width, anchor_height);

    // Fill in missing coordinates
    int y = anchor_y;
    for (int i = 0; i < m_lines.size(); i++)
    {
        if (m_lines[i].x_indent < 0)
            m_lines[i].x_indent = anchor_x;
        if (m_lines[i].y_indent < 0)
            m_lines[i].y_indent = y;
        y += m_lines[i].CalcSize(LINE_SPACING).height();
        // Prune leading all-whitespace chunks.
        while (!m_lines[i].chunks.isEmpty() &&
               m_lines[i].chunks.first().text.trimmed().isEmpty())
        {
            m_lines[i].x_indent +=
                m_lines[i].chunks.first().CalcSize().width();
            m_lines[i].chunks.removeFirst();
        }
        // Prune trailing all-whitespace chunks.
        while (!m_lines[i].chunks.isEmpty() &&
               m_lines[i].chunks.last().text.trimmed().isEmpty())
        {
            m_lines[i].chunks.removeLast();
        }
        // Trim trailing whitespace from last chunk.  (Trimming
        // leading whitespace from all chunks is handled in the Draw()
        // routine.)
        if (!m_lines[i].chunks.isEmpty())
        {
            QString *str = &m_lines[i].chunks.last().text;
            int idx = str->length() - 1;
            while (idx >= 0 && str->at(idx) == ' ')
                --idx;
            str->truncate(idx + 1);
        }
    }
}

// Returns true if anything new was drawn, false if not.  The caller
// should call SubtitleScreen::OptimiseDisplayedArea() if true is
// returned.
bool FormattedTextSubtitle::Draw(QList<MythUIType*> *imageCache,
                                 uint64_t start, uint64_t duration) const
{
    bool result = false;
    QVector<MythUISimpleText *> bringToFront;

    for (int i = 0; i < m_lines.size(); i++)
    {
        int x = m_lines[i].x_indent;
        int y = m_lines[i].y_indent;
        int height = m_lines[i].CalcSize().height();
        QList<FormattedTextChunk>::const_iterator chunk;
        bool first = true;
        for (chunk = m_lines[i].chunks.constBegin();
             chunk != m_lines[i].chunks.constEnd();
             ++chunk)
        {
            MythFontProperties *mythfont = parent->Get708Font((*chunk).format);
            if (!mythfont)
                continue;
            QFontMetrics font(*(mythfont->GetFace()));
            // If the chunk starts with whitespace, the leading
            // whitespace ultimately gets lost due to the
            // text.trimmed() operation in the MythUISimpleText
            // constructor.  To compensate, we manually indent the
            // chunk accordingly.
            int count = 0;
            while (count < (*chunk).text.length() &&
                   (*chunk).text.at(count) == ' ')
            {
                ++count;
            }
            int x_adjust = count * font.width(" ");
            int padding = (*chunk).CalcPadding();
            // Account for extra padding before the first chunk.
            if (first)
                x += padding;
            QSize chunk_sz = (*chunk).CalcSize();
            if ((*chunk).format.GetBGAlpha())
            {
                QBrush bgfill = QBrush((*chunk).format.GetBGColor());
                QRect bgrect(x - padding, y,
                             chunk_sz.width() + 2 * padding, height);
                // Don't draw a background behind leading spaces.
                if (first)
                    bgrect.setLeft(bgrect.left() + x_adjust);
                MythUIShape *bgshape = new MythUIShape(parent,
                        QString("subbg%1x%2@%3,%4")
                                     .arg(chunk_sz.width())
                                     .arg(height)
                                     .arg(x).arg(y));
                bgshape->SetFillBrush(bgfill);
                bgshape->SetArea(MythRect(bgrect));
                if (imageCache)
                    imageCache->append(bgshape);
                if (duration > 0)
                    parent->RegisterExpiration(bgshape, start + duration);
                result = true;
            }
            // Shift to the right to account for leading spaces that
            // are removed by the MythUISimpleText constructor.  Also
            // add in padding at the end to avoid clipping.
            QRect rect(x + x_adjust, y,
                       chunk_sz.width() - x_adjust + padding, height);

            MythUISimpleText *text =
                new MythUISimpleText((*chunk).text, *mythfont, rect,
                                     Qt::AlignLeft, (MythUIType*)parent,
                                     QString("subtxt%1x%2@%3,%4")
                                     .arg(chunk_sz.width())
                                     .arg(height)
                                     .arg(x).arg(y));
            bringToFront.append(text);
            if (imageCache)
                imageCache->append(text);
            if (duration > 0)
                parent->RegisterExpiration(text, start + duration);
            result = true;

            LOG(VB_VBI, LOG_INFO,
                QString("Drawing chunk at (%1,%2): %3")
                .arg(x).arg(y).arg((*chunk).ToLogString()));

            x += chunk_sz.width();
            first = false;
        }
    }
    // Move each chunk of text to the front so that it isn't clipped
    // by the preceding chunk's background.
    for (int i = 0; i < bringToFront.size(); i++)
        bringToFront.at(i)->MoveToTop();
    return result;
}

QStringList FormattedTextSubtitle::ToSRT(void) const
{
    QStringList result;
    for (int i = 0; i < m_lines.size(); i++)
    {
        QString line;
        if (m_lines[i].orig_x > 0)
            line.fill(' ', m_lines[i].orig_x);
        QList<FormattedTextChunk>::const_iterator chunk;
        for (chunk = m_lines[i].chunks.constBegin();
             chunk != m_lines[i].chunks.constEnd();
             ++chunk)
        {
            const QString &text = (*chunk).text;
            const CC708CharacterAttribute &attr = (*chunk).format;
            bool isBlank = !attr.underline && text.trimmed().isEmpty();
            if (!isBlank)
            {
                if (attr.boldface)
                    line += "<b>";
                if (attr.italics)
                    line += "<i>";
                if (attr.underline)
                    line += "<u>";
                if (attr.GetFGColor() != Qt::white)
                    line += QString("<font color=\"%1\">")
                        .arg(srtColorString(attr.GetFGColor()));
            }
            line += text;
            if (!isBlank)
            {
                if (attr.GetFGColor() != Qt::white)
                    line += QString("</font>");
                if (attr.underline)
                    line += "</u>";
                if (attr.italics)
                    line += "</i>";
                if (attr.boldface)
                    line += "</b>";
            }
        }
        if (!line.trimmed().isEmpty())
            result += line;
    }
    return result;
}

void SubtitleScreen::SetFontSizes(int nSmall, int nMedium, int nLarge)
{
    m_708fontSizes[k708AttrSizeSmall]    = nSmall;
    m_708fontSizes[k708AttrSizeStandard] = nMedium;
    m_708fontSizes[k708AttrSizeLarge]    = nLarge;
}

QSize SubtitleScreen::CalcTextSize(const QString &text,
                                   const CC708CharacterAttribute &format,
                                   float layoutSpacing) const
{
    QFont *font = Get708Font(format)->GetFace();
    QFontMetrics fm(*font);
    int width = fm.width(text);
    int height = fm.height() * (1 + PAD_HEIGHT);
    if (layoutSpacing > 0 && !text.trimmed().isEmpty())
        height = max(height, (int)(font->pixelSize() * layoutSpacing));
    return QSize(width, height);
}

int SubtitleScreen::CalcPadding(const CC708CharacterAttribute &format) const
{
    QFont *font = Get708Font(format)->GetFace();
    QFontMetrics fm(*font);
    return fm.maxWidth() * PAD_WIDTH;
}

#ifdef USING_LIBASS
static void myth_libass_log(int level, const char *fmt, va_list vl, void *ctx)
{
    static QString full_line("libass:");
    static const int msg_len = 255;
    static QMutex string_lock;
    uint64_t verbose_mask = VB_GENERAL;
    LogLevel_t verbose_level = LOG_INFO;

    switch (level)
    {
        case 0: //MSGL_FATAL
            verbose_level = LOG_EMERG;
            break;
        case 1: //MSGL_ERR
            verbose_level = LOG_ERR;
            break;
        case 2: //MSGL_WARN
            verbose_level = LOG_WARNING;
            break;
        case 4: //MSGL_INFO
            verbose_level = LOG_INFO;
            break;
        case 6: //MSGL_V
        case 7: //MSGL_DBG2
            verbose_level = LOG_DEBUG;
            break;
        default:
            return;
    }

    if (!VERBOSE_LEVEL_CHECK(verbose_mask, verbose_level))
        return;

    string_lock.lock();

    char str[msg_len+1];
    int bytes = vsnprintf(str, msg_len+1, fmt, vl);
    // check for truncated messages and fix them
    if (bytes > msg_len)
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("libASS log output truncated %1 of %2 bytes written")
                .arg(msg_len).arg(bytes));
        str[msg_len-1] = '\n';
    }

    full_line += QString(str);
    if (full_line.endsWith("\n"))
    {
        LOG(verbose_mask, verbose_level, full_line.trimmed());
        full_line.truncate(0);
    }
    string_lock.unlock();
}

bool SubtitleScreen::InitialiseAssLibrary(void)
{
    if (m_assLibrary && m_assRenderer)
        return true;

    if (!m_assLibrary)
    {
        m_assLibrary = ass_library_init();
        if (!m_assLibrary)
            return false;

        ass_set_message_cb(m_assLibrary, myth_libass_log, NULL);
        ass_set_extract_fonts(m_assLibrary, true);
        LOG(VB_PLAYBACK, LOG_INFO, LOC + "Initialised libass object.");
    }

    LoadAssFonts();

    if (!m_assRenderer)
    {
        m_assRenderer = ass_renderer_init(m_assLibrary);
        if (!m_assRenderer)
            return false;

        ass_set_fonts(m_assRenderer, NULL, "sans-serif", 1, NULL, 1);
        ass_set_hinting(m_assRenderer, ASS_HINTING_LIGHT);
        LOG(VB_PLAYBACK, LOG_INFO, LOC + "Initialised libass renderer.");
    }

    return true;
}

void SubtitleScreen::LoadAssFonts(void)
{
    if (!m_assLibrary || !m_player)
        return;

    uint count = m_player->GetDecoder()->GetTrackCount(kTrackTypeAttachment);
    if (m_assFontCount == count)
        return;

    ass_clear_fonts(m_assLibrary);
    m_assFontCount = 0;

    // TODO these need checking and/or reinitialising after a stream change
    for (uint i = 0; i < count; ++i)
    {
        QByteArray filename;
        QByteArray font;
        m_player->GetDecoder()->GetAttachmentData(i, filename, font);
        ass_add_font(m_assLibrary, filename.data(), font.data(), font.size());
        LOG(VB_PLAYBACK, LOG_INFO, LOC + QString("Retrieved font '%1'")
            .arg(filename.constData()));
        m_assFontCount++;
    }
}

void SubtitleScreen::CleanupAssLibrary(void)
{
    CleanupAssTrack();

    if (m_assRenderer)
        ass_renderer_done(m_assRenderer);
    m_assRenderer = NULL;

    if (m_assLibrary)
    {
        ass_clear_fonts(m_assLibrary);
        m_assFontCount = 0;
        ass_library_done(m_assLibrary);
    }
    m_assLibrary = NULL;
}

void SubtitleScreen::InitialiseAssTrack(int tracknum)
{
    if (!InitialiseAssLibrary() || !m_player)
        return;

    if (tracknum == m_assTrackNum && m_assTrack)
        return;

    LoadAssFonts();
    CleanupAssTrack();
    m_assTrack = ass_new_track(m_assLibrary);
    m_assTrackNum = tracknum;

    QByteArray header = m_player->GetDecoder()->GetSubHeader(tracknum);
    if (!header.isNull())
        ass_process_codec_private(m_assTrack, header.data(), header.size());

    m_safeArea = m_player->GetVideoOutput()->GetMHEGBounds();
    ResizeAssRenderer();
}

void SubtitleScreen::CleanupAssTrack(void)
{
    if (m_assTrack)
        ass_free_track(m_assTrack);
    m_assTrack = NULL;
}

void SubtitleScreen::AddAssEvent(char *event)
{
    if (m_assTrack && event)
        ass_process_data(m_assTrack, event, strlen(event));
}

void SubtitleScreen::ResizeAssRenderer(void)
{
    // TODO this probably won't work properly for anamorphic content and XVideo
    ass_set_frame_size(m_assRenderer, m_safeArea.width(), m_safeArea.height());
    ass_set_margins(m_assRenderer, 0, 0, 0, 0);
    ass_set_use_margins(m_assRenderer, true);
    ass_set_font_scale(m_assRenderer, 1.0);
}

void SubtitleScreen::RenderAssTrack(uint64_t timecode)
{
    if (!m_player || !m_assRenderer || !m_assTrack)
        return;

    VideoOutput *vo = m_player->GetVideoOutput();
    if (!vo )
        return;

    QRect oldscreen = m_safeArea;
    m_safeArea = vo->GetMHEGBounds();
    if (oldscreen != m_safeArea)
        ResizeAssRenderer();

    int changed = 0;
    ASS_Image *images = ass_render_frame(m_assRenderer, m_assTrack,
                                         timecode, &changed);
    if (!changed)
        return;

    MythPainter *osd_painter = vo->GetOSDPainter();
    if (!osd_painter)
        return;

    int count = 0;
    DeleteAllChildren();
    SetRedraw();
    while (images)
    {
        if (images->w == 0 || images->h == 0)
        {
            images = images->next;
            continue;
        }

        uint8_t alpha = images->color & 0xFF;
        uint8_t blue = images->color >> 8 & 0xFF;
        uint8_t green = images->color >> 16 & 0xFF;
        uint8_t red = images->color >> 24 & 0xFF;

        if (alpha == 255)
        {
            images = images->next;
            continue;
        }

        QSize img_size(images->w, images->h);
        QRect img_rect(images->dst_x,images->dst_y,
                       images->w, images->h);
        QImage qImage(img_size, QImage::Format_ARGB32);
        qImage.fill(0x00000000);

        unsigned char *src = images->bitmap;
        for (int y = 0; y < images->h; ++y)
        {
            for (int x = 0; x < images->w; ++x)
            {
                uint8_t value = src[x];
                if (value)
                {
                    uint32_t pixel = (value * (255 - alpha) / 255 << 24) |
                                     (red << 16) | (green << 8) | blue;
                    qImage.setPixel(x, y, pixel);
                }
            }
            src += images->stride;
        }

        MythImage* image = NULL;
        MythUIImage *uiimage = NULL;

        if (osd_painter)
            image = osd_painter->GetFormatImage();

        if (image)
        {
            image->Assign(qImage);
            QString name = QString("asssub%1").arg(count);
            uiimage = new MythUIImage(this, name);
            if (uiimage)
            {
                m_refreshArea = true;
                uiimage->SetImage(image);
                uiimage->SetArea(MythRect(img_rect));
            }
        }
        images = images->next;
        count++;
    }
}
#endif // USING_LIBASS

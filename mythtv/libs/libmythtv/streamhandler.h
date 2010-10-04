// -*- Mode: c++ -*-

#ifndef _STREAM_HANDLER_H_
#define _STREAM_HANDLER_H_

#include <vector>
using namespace std;

#include <QWaitCondition>
#include <QString>
#include <QThread>
#include <QMutex>
#include <QMap>

#include "DeviceReadBuffer.h" // for ReaderPausedCB
#include "mpegstreamdata.h" // for PIDPriority
#include "util.h"

//#define DEBUG_PID_FILTERS

class PIDInfo
{
  public:
    PIDInfo() :
        _pid(0xffffffff), filter_fd(-1), streamType(0), pesType(-1) {;}
    PIDInfo(uint pid) :
        _pid(pid),        filter_fd(-1), streamType(0), pesType(-1) {;}
    PIDInfo(uint pid, uint stream_type, int pes_type) :
        _pid(pid),                       filter_fd(-1),
        streamType(stream_type),         pesType(pes_type) {;}

    virtual bool Open(const QString &dev, bool use_section_reader)
        { return false; }
    virtual bool Close(const QString &dev) { return false; }
    bool IsOpen(void) const { return filter_fd >= 0; }

    uint        _pid;
    int         filter_fd;         ///< Input filter file descriptor
    uint        streamType;        ///< StreamID
    int         pesType;           ///< PESStreamID
};
typedef QMap<uint,PIDInfo*> PIDInfoMap;

// locking order
// _pid_lock -> _listener_lock -> _start_stop_lock

class StreamHandler : protected QThread, public DeviceReaderCB
{
  public:
    virtual void AddListener(MPEGStreamData *data,
                             bool allow_section_reader = false,
                             bool needs_drb            = false,
                             QString output_file       = QString());
    virtual void RemoveListener(MPEGStreamData *data);
    bool IsRunning(void) const;

  protected:
    StreamHandler(const QString &device);
    ~StreamHandler();

    void Start(void);
    void Stop(void);

    void SetRunning(bool running,
                    bool using_buffering,
                    bool using_section_reader);

    bool AddPIDFilter(PIDInfo *info);
    bool RemovePIDFilter(uint pid);
    bool RemoveAllPIDFilters(void);

    void UpdateListeningForEIT(void);
    bool UpdateFiltersFromStreamData(void);
    virtual bool UpdateFilters(void) { return true; }
    virtual void CycleFiltersByPriority() {}

    PIDPriority GetPIDPriority(uint pid) const;

    // DeviceReaderCB
    virtual void ReaderPaused(int fd) { (void) fd; }
    virtual void PriorityEvent(int fd) { (void) fd; }

    PIDInfo *CreatePIDInfo(uint pid, uint stream_type, int pes_type)
        { return new PIDInfo(pid, stream_type, pes_type); }

  protected:
    /// Called with _listener_lock locked just after adding new output file.
    virtual void AddNamedOutputFile(const QString &filename) {}
    /// Called with _listener_lock locked just before removing old output file.
    virtual void RemoveNamedOutputFile(const QString &filename) {}

  protected:
    QString           _device;
    bool              _needs_buffering;
    bool              _allow_section_reader;

    mutable QMutex    _start_stop_lock;
    volatile bool     _running_desired;
    volatile bool     _error;
    bool              _running;
    bool              _using_buffering;
    bool              _using_section_reader;
    QWaitCondition    _running_state_changed;

    mutable QMutex    _pid_lock;
    vector<uint>      _eit_pids;
    PIDInfoMap        _pid_info;
    uint              _open_pid_filters;
    MythTimer         _cycle_timer;

    typedef QMap<MPEGStreamData*,QString> StreamDataList;
    mutable QMutex    _listener_lock;
    StreamDataList    _stream_data_list;
};

#endif // _STREAM_HANDLER_H_
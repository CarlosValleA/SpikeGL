#ifndef SpikeGLHandlerThread_H
#define SpikeGLHandlerThread_H

#include "../FG_SpikeGL/FG_SpikeGL/XtCmd.h"
#include <QThread>
#include <QMutex>
#include <list>
#include <vector>

class SpikeGLHandlerThread : public QThread
{
public:
    volatile bool pleaseStop;
    int maxCommandQ;

    SpikeGLHandlerThread() : pleaseStop(false), maxCommandQ(3072), nCmd(0) {}
    virtual ~SpikeGLHandlerThread();

    bool pushCmd(const XtCmd *c, int timeout_ms = -1 /*INFINITE*/);
    XtCmd * popCmd(std::vector<unsigned char> &outBuf, int timeout_ms = -1 /*INFINITE*/);
    int cmdQSize() const;

    void kill() { QThread::terminate(); }

protected:
    void tryToStop();
    virtual void threadFunc() = 0;
    void run() { threadFunc(); }
    typedef std::list<std::vector<unsigned char> > CmdList;
    CmdList cmds;
    volatile int nCmd;
    mutable QMutex mut;
};

class SpikeGLOutThread : public SpikeGLHandlerThread
{
public:
    SpikeGLOutThread()  {}
    ~SpikeGLOutThread();

    bool pushConsoleMsg(const std::string & msg, int mtype = XtCmdConsoleMsg::Normal);
    bool pushConsoleDebug(const std::string & msg) { return pushConsoleMsg(msg, XtCmdConsoleMsg::Debug); }
    bool pushConsoleError(const std::string & msg) { return pushConsoleMsg(msg, XtCmdConsoleMsg::Error); }
    bool pushConsoleWarning(const std::string & msg) { return pushConsoleMsg(msg, XtCmdConsoleMsg::Warning); }

protected:
    void threadFunc();
};

class SpikeGLInputThread : public SpikeGLHandlerThread
{
public:
    SpikeGLInputThread()  {}
    ~SpikeGLInputThread();
protected:
    void threadFunc();
};

#endif

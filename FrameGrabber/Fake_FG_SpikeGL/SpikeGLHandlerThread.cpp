#include "SpikeGLHandlerThread.h"
#include <stdio.h>
#include <sys/stat.h>
#include <stdio.h>
#ifdef Q_OS_WIN
#include <io.h>
#endif
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

SpikeGLHandlerThread::~SpikeGLHandlerThread() { }
SpikeGLOutThread::~SpikeGLOutThread() { 
    tryToStop(); 
    if (isRunning()) kill();
}
SpikeGLInputThread::~SpikeGLInputThread() { 
    tryToStop(); 
    if (isRunning()) kill();
}

void SpikeGLHandlerThread::tryToStop() {
    if (isRunning()) { pleaseStop = true; wait(300); }
}

bool SpikeGLHandlerThread::pushCmd(const XtCmd *c, int timeout_ms)
{
    if (mut.tryLock(timeout_ms)) {
        if (nCmd >= maxCommandQ) {
            // todo.. handle command q overflow here!
            mut.unlock();
            return false;
        }
        cmds.push_back(std::vector<unsigned char>());
        ++nCmd;
        std::vector<unsigned char> & v(cmds.back());
        v.resize(c->len + (((char *)c->data) - (char *)c));
        ::memcpy(&v[0], c, v.size());
        mut.unlock();
        return true;
    }
    return false;
}

XtCmd * SpikeGLHandlerThread::popCmd(std::vector<unsigned char> &outBuf, int timeout_ms)
{
    XtCmd *ret = 0;
    if (mut.tryLock(timeout_ms)) {
        if (nCmd) {
            std::vector<unsigned char> & v(cmds.front());
            outBuf.swap(v);
            cmds.pop_front();
            --nCmd;
            if (outBuf.size() >= sizeof(XtCmd)) {
                ret = (XtCmd *)&outBuf[0];
            }
        }
        mut.unlock();
    }
    return ret;
}

int SpikeGLHandlerThread::cmdQSize() const
{
    int ret = -1;
    if (mut.tryLock(-1)) {
        ret = nCmd;
        mut.unlock();
    }
    return ret;
}

bool SpikeGLOutThread::pushConsoleMsg(const std::string & str, int mtype)
{
    XtCmdConsoleMsg *xt = XtCmdConsoleMsg::allocInit(str, mtype);
    bool ret = pushCmd(xt);
    free(xt);
    return ret;
}

void SpikeGLOutThread::threadFunc()
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdout), O_BINARY);
#endif
    freopen(0, "wb", stdout);
    while (!pleaseStop) {
        int ct = 0;
        if (mut.tryLock(100)) {
            CmdList my;
            my.splice(my.begin(), cmds);
            nCmd = 0;
            mut.unlock();
            for (CmdList::iterator it = my.begin(); it != my.end(); ++it) {
                XtCmd *c = (XtCmd *)&((*it)[0]);
                if (!c->write(stdout)) {
                    // todo.. handle error here...
                }
                ++ct;
            }
            fflush(stdout);
            if (!ct) msleep(10);
        }
    }
}

void SpikeGLInputThread::threadFunc()
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), O_BINARY);
#endif
    freopen(0,"rb",stdin);
    std::vector<unsigned char> buf;
    XtCmd *xt = 0;
    while (!pleaseStop) {
        if ((xt = XtCmd::read(buf, stdin))) {
            if (!pushCmd(xt)) {
                // todo.. handle error here...
            }
        }
    }
}

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
    FILE *outf = 0;
#ifdef Q_OS_WIN
    int fd = _fileno(stdout);
    _setmode(fd, O_BINARY);
    outf = _fdopen(fd, "wb");
#else
    outf = freopen(0, "wb", stdout);
#endif
    if (!outf) return;
    while (!pleaseStop) {
        int ct = 0;
        if (mut.tryLock(100)) {
            CmdList my;
            my.splice(my.begin(), cmds);
            nCmd = 0;
            mut.unlock();
            for (CmdList::iterator it = my.begin(); it != my.end(); ++it) {
                XtCmd *c = (XtCmd *)&((*it)[0]);
                if (!c->write(outf)) {
                    // todo.. handle error here...
                }
                ++ct;
            }
            fflush(outf);
            if (!ct) msleep(10);
        }
    }
}

void SpikeGLInputThread::threadFunc()
{
    FILE *inpf = 0;
#ifdef Q_OS_WIN
    int fd = _fileno(stdin);
    _setmode(fd, O_BINARY);
    inpf = _fdopen(fd, "rb");
#else
    inpf = freopen(0,"rb",stdin);
#endif
    if (!inpf) return;
    std::vector<unsigned char> buf;
    XtCmd *xt = 0;
    while (!pleaseStop) {
        if ((xt = XtCmd::read(buf, inpf))) {
            if (!pushCmd(xt)) {
                // todo.. handle error here...
            }
        }
    }
}

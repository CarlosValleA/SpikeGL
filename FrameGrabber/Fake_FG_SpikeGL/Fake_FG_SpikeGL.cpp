// Fake_FG_SpikeGL.cpp : Defines the entry point for the console application.
//
#include "GlowBalls.h"
#include "../FG_SpikeGL/FG_SpikeGL/XtCmd.h"
#include "../../PagedRingbuffer.h"
#include <QTimer>
#include <QDateTime>
#include <QSharedMemory>
#ifdef Q_OS_WINDOWS
#include <varargs.h>
#include <windows.h>
#else
#include <stdarg.h>
#endif
#include <math.h>

SpikeGLOutThread    *spikeGL = 0;
SpikeGLInputThread  *spikeGLIn = 0;
QTimer *timer = 0;
bool gotFirstXferCallback = false, gotFirstStartFrameCallback = false;
int bpp=0, pitch=0, width=0, height=0;
static volatile bool acq = false; ///< true iff acquisition is running

int desiredWidth = 144;
int desiredHeight = 32;
unsigned long long tsCounter = 0, frameNum = 0;

std::string shmName("COMES_FROM_XtCmd");
unsigned shmSize(0), shmPageSize(0), shmMetaSize(0);
std::vector<char> metaBuffer; unsigned long long *metaPtr = 0; int metaIdx = 0, metaMaxIdx = 0;
std::vector<int> chanMapping;

QSharedMemory *qsm = 0;
void *sharedMemory = 0;
PagedScanWriter *writer = 0;
unsigned nChansPerScan = 0;

class XferEmu;
XferEmu *xfer = 0;


class XferEmu : public QThread
{
public:
    volatile bool pleaseStop;
    XferEmu(QObject *parent);
    ~XferEmu();

    void abort() { pleaseStop = true; }

protected:
    void run();
};

// some static functions..
static void probeHardware();
static double getTime();

static inline void metaPtrInc() { if (++metaIdx >= metaMaxIdx) metaIdx = 0; }
static inline unsigned long long & metaPtrCur() { 
    static unsigned long long dummy = 0; 
    if (metaPtr && metaIdx < metaMaxIdx) return metaPtr[metaIdx];
    return dummy;
}

static int PSWDbgFunc(const char *fmt, ...) 
{
    char buf[1024];
    va_list l;
    int ret = 0;
    va_start(l, fmt);
    ret = vsnprintf_s(buf, sizeof(buf), fmt, l);
    spikeGL->pushConsoleDebug(buf);
    va_end(l);
    return ret;
}
static int PSWErrFunc(const char *fmt, ...)
{
    char buf[1024];
    va_list l;
    int ret = 0;
    va_start(l, fmt);
    ret = vsnprintf_s(buf, sizeof(buf), fmt, l);
    spikeGL->pushConsoleError(buf);
    va_end(l);
    return ret;
}

void genFakeFrame(char *buf, int pitch, int height, unsigned long long  & ctr, unsigned long long & fnum) {
    float v = (int(fnum)%52000)/52000.0f;
    v = v * M_PI;
    for (int r = 0; r < height; ++r) {
        char *l0 = &buf[r*pitch], *pc = (char *)&ctr;
        l0[4] = pc[0]; l0[3] = pc[1]; l0[2] = pc[2]; l0[0] = pc[3];
        l0[7] = pc[4]; l0[6] = pc[5]; l0[5] = pc[6]; l0[4] = pc[7];
        for (int c = 8; c < pitch; c+=2) {
           short *s = (short *)&buf[r*pitch + c];
           float cfrac = (float(c)/152.f);
           if (c%2) *s = ::sinf(v * (4.0f * cfrac) + cfrac*0.1) * 32768;
           else     *s = ::cosf(v * (4.0f * cfrac) + cfrac*0.1) * 32767;
        }
        ctr += 88ULL;
    }
    // end of frame counter update...
    ctr += 88ULL*2ULL;
    ++fnum;
}

static void writeFakeFrame()
{
    if (!gotFirstXferCallback)
        spikeGL->pushConsoleDebug("acqCallback called at least once! Yay!"), gotFirstXferCallback = true;
    if (!width) {
        bpp = 1;		// bpp:		get number of bytes required to store a single image
        pitch = desiredWidth+8;				// pitch:	get number of bytes between two consecutive lines of all the buffer resource
        width = desiredWidth;				// width:	get the width (in pixel) of the image
        height = desiredHeight;				// Height:	get the height of the image
    }
    int w = width, h = height;
    if (w < desiredWidth || h < desiredHeight) {
        char tmp[512];
        _snprintf_c(tmp, sizeof(tmp), "acqCallback got a frame of size %dx%d, but expected a frame of size %dx%d", w, h, desiredWidth, desiredHeight);
        spikeGL->pushConsoleError(tmp);
        xfer->abort();
        return;
    }
    if (w > desiredWidth) w = desiredWidth;
    if (h > desiredHeight) h = desiredHeight;
    size_t len = w*h;

    const size_t oneScanBytes = nChansPerScan * sizeof(short);

    if (!sharedMemory) {
        spikeGL->pushConsoleError("INTERNAL ERROR.. not attached to shm, cannot send frames!");
        xfer->abort();
        return;
    }
    if (!writer->scansPerPage()) {
        spikeGL->pushConsoleError("INTERNAL ERROR.. shm page, cannot fit at least 1 scan! FIXME!");
        xfer->abort();
        return;
    }

    size_t nScansInFrame = len / oneScanBytes;

    if (!nScansInFrame) {
        spikeGL->pushConsoleError("Frame must contain at least 1 full scan! FIXME!");
        xfer->abort();
        return;
    }

    char frame[pitch*height];

    genFakeFrame(frame,pitch,height,tsCounter,frameNum);

    const short *pData = (short *)frame;

#define DUMP_FRAMES 0
#if     DUMP_FRAMES
    double t0write = getTime(); /// XXX
	static double tLastPrt = 0.; /// XXX
#endif

    if (pitch != w) {
        // WARNING:
        // THIS IS A HACK TO SUPPORT THE FPGA FRAMEGRABBER FORMAT AND IS VERY SENSITIVE TO THE EXACT
        // LAYOUT OF DATA IN THE FRAMEGRABBER!! EDIT THIS CODE IF THE LAYOUT CHANGES AND/OR WE NEED SOMETHING
        // MORE GENERIC!

        // pith != w, so write scan by taking each valid row of the frame... using our new writer->writePartial() method
        // note we only support basically frames where pitch is exactly 8 bytes larger than width, because these are from the FPGA
        // and we take the ENTIRE line (all the way up to pitch bytes) for data.  The first 8 bytes in the line is the timestamp
        // See emails form Jim Chen in March 2016
        if (pitch - w != 8) {
            spikeGL->pushConsoleError("Unsupported frame format! We are expecting a frame where pitch is 8 bytes larger than width!");
            xfer->abort();
            return;
        }
        if (nScansInFrame != 1) {
            spikeGL->pushConsoleError("Unsupported frame format! We are expecting a frame where there is exactly one complete scan per frame!");
            xfer->abort();
            return;
        }

        const char *pc = reinterpret_cast<const char *>(pData);
#if DUMP_FRAMES
		// HACK TESTING HACK
		static FILE *f = 0; static bool tryfdump = true;
		if ( tryfdump ) {
                        if (!f) f = fopen("c:\\frame.bin", "wb");
			if (!f) {
				spikeGL->pushConsoleError("Could not open d:\\frame.bin");
				tryfdump = false;
			}
			else {
				size_t s = fwrite(pc, pitch*height, bpp, f);
				if (!s) {
					spikeGL->pushConsoleError("Error writing frame to d:\\frmae.bin");
				}
				else if (t0write - tLastPrt > 1.0) {
					spikeGL->pushConsoleDebug("Appended frame to d:\\frame.bin");
					fflush(f);
					tLastPrt = t0write;
				}
			}
		}
		// /HACK
#endif
        writer->writePartialBegin();
        for (int line = 0; line < h; ++line) {
                if (line + 1 == h) {
                    metaPtrCur() = *reinterpret_cast<const unsigned long long *>(pc + line*pitch);
                    metaPtrInc();
                }
                if (!writer->writePartial(pc + /* <HACK> */ 8 /* </HACK> */ + (line*pitch), w, metaPtr)) {
                    spikeGL->pushConsoleError("PagedScanWriter::writePartial() returned false!");
                    writer->writePartialEnd();
                    xfer->abort();
                    return;
                }
        }

        if (!writer->writePartialEnd()) {
            spikeGL->pushConsoleError("PagedScanWriter::writePartialEnd() returned false!");
            xfer->abort();
            return;
        }
    } else {
        // NOTE: this case is calin's test code.. we fudge the metadata .. even though it makes no sense at all.. to make sure SpikeGL is reading it properly
        metaIdx = metaMaxIdx - 1;
        metaPtrCur() = frameNum;  // HACK!

        if (!writer->write(pData, unsigned(nScansInFrame),metaPtr)) {
            spikeGL->pushConsoleError("PagedScanWriter::write returned false!");
            xfer->abort();
            return;
        }
    }

    /// FPS RATE CODE
    static double lastTS = 0., lastPrt = -1.0;
    static int ctr = 0;
    ++ctr;

    double now = getTime();
    if (frameNum > 1 && now - lastPrt >= 1.0) {
        double rate = 1.0 / ((now - lastPrt)/ctr);
        /// xxx
        //char tmp[512];
        //_snprintf_c(tmp, sizeof(tmp), "DEBUG: frame %u - avg.twrite: %2.6f ms (avg of %d frames)", (unsigned)frameNum, tWriteSum/double(ctr) * 1e3, ctr);
        //spikeGL->pushConsoleDebug(tmp);
        //tWriteSum = 0.;
        /// /XXX
        lastPrt = now;
        ctr = 0;
        XtCmdFPS fps;
        fps.init(rate);
        spikeGL->pushCmd(&fps);
    }
    lastTS = now;    
}

static void startFrameCallback()
{ 
    if (!gotFirstStartFrameCallback)
        spikeGL->pushConsoleDebug("'startFrameCallback' called at least once! Yay!"), gotFirstStartFrameCallback = true;
}

static void freeHandles()
{
    if (xfer) delete xfer, xfer = 0;
    acq = false;
    bpp = pitch = width = height = 0;
    gotFirstStartFrameCallback = gotFirstXferCallback = false;
    delete qsm; qsm = 0; sharedMemory = 0;
}

XferEmu::XferEmu(QObject *parent) : QThread(parent), pleaseStop(false)
{
}

XferEmu::~XferEmu() {
    pleaseStop = true;
    wait();
}

void XferEmu::run() {
    acq = true;
    while (!pleaseStop) {
        startFrameCallback();
        writeFakeFrame();
        usleep(37); // sampling rate ~= 26khz
    }
    acq = false;
}

static bool setupAndStartAcq()
{
    freeHandles();

    metaBuffer.resize(shmMetaSize,0);
    metaPtr = shmMetaSize ? reinterpret_cast<unsigned long long *>(&metaBuffer[0]) : 0;
    metaIdx = 0; metaMaxIdx = shmMetaSize / sizeof(unsigned long long);

    if (!sharedMemory) {
        char tmp[512];

        if (qsm) delete qsm, qsm = 0;
        qsm = new QSharedMemory(shmName.c_str(), qApp);
        if (!qsm->isAttached() && !qsm->attach()) {
            delete qsm, qsm = 0;
            _snprintf_c(tmp, sizeof(tmp), "Could not attach to shared memory segment \"%s\"", shmName.c_str());
            spikeGL->pushConsoleError(tmp);
            return false;
        }
        sharedMemory = (void *)(qsm->data());
        if (qsm->size() < (int)shmSize) {
            _snprintf_c(tmp, sizeof(tmp), "Shm \"%s\" size %d != %u", shmName.c_str(), qsm->size(), shmSize);
            spikeGL->pushConsoleError(tmp);
            delete qsm; qsm = 0;
            sharedMemory = 0;
            return false;
        }

        if (writer) delete writer;
        writer = new PagedScanWriter(nChansPerScan, shmMetaSize, sharedMemory, shmSize, shmPageSize, chanMapping);
        writer->ErrFunc = &PSWErrFunc; writer->DbgFunc = &PSWDbgFunc;
        _snprintf_c(tmp, sizeof(tmp), "Connected to shared memory \"%s\" size: %u  pagesize: %u metadatasize: %u", shmName.c_str(), shmSize, shmPageSize, shmMetaSize);
        spikeGL->pushConsoleDebug(tmp);
    }
    xfer = new XferEmu(qApp);
    xfer->start(QThread::HighPriority);
    return true;
}

static void handleSpikeGLCommand(XtCmd *xt)
{
    (void)xt;
    switch (xt->cmd) {
    case XtCmd_Exit:
        spikeGL->pushConsoleDebug("Got exit command.. exiting gracefully...");
        exit(0);
        break;
    case XtCmd_Test:
        spikeGL->pushConsoleDebug("Got 'TEST' command, replying with this debug message!");
        break;
    case XtCmd_GrabFrames: {
        XtCmdGrabFrames *x = (XtCmdGrabFrames *)xt;
        spikeGL->pushConsoleDebug("Got 'GrabFrames' command");
        if (x->frameH > 0) desiredHeight = x->frameH; 
        if (x->frameW > 0) desiredWidth = x->frameW;
        if (x->numChansPerScan > 0) nChansPerScan = x->numChansPerScan;
        else {
            nChansPerScan = 1;
            spikeGL->pushConsoleWarning("FIXME: nChansPerScan was not specified in XtCmd_GrabFrames!");
        }
        if (x->shmPageSize <= 0 || x->shmSize <= 0 || !x->shmName[0]) {
            spikeGL->pushConsoleError("FIXME: shmPageSize,shmName,and shmSize need to be specified in XtCmd_GrabFrames!");
            break;
        }
        if (x->shmPageSize > x->shmSize) {
            spikeGL->pushConsoleError("FIXME: shmPageSize cannot be > shmSize in XtCmd_GrabFrames!");
            break;
        }
        shmPageSize = x->shmPageSize;
        shmSize = x->shmSize;
        shmName = x->shmName;
        shmMetaSize = x->shmMetaSize;
        chanMapping.clear();
        if (x->use_map) {
            chanMapping.resize(nChansPerScan);
            const int maxsize = sizeof(x->mapping) / sizeof(*x->mapping);
            if (chanMapping.size() > maxsize) chanMapping.resize(maxsize);
            memcpy(&chanMapping[0], x->mapping, chanMapping.size()*sizeof(int));
        }
        if (!setupAndStartAcq())
            spikeGL->pushConsoleWarning("Failed to start acquisition.");
        break;
    }
    case XtCmd_FPGAProto: {
        XtCmdFPGAProto *x = (XtCmdFPGAProto *)xt;
        if (x->len >= 16) {
            spikeGL->pushConsoleDebug("Got 'FPGAProto' command");
 //           fpga->protocol_Write(x->cmd_code, x->value1, x->value2);
        }
    }
        break;
    case XtCmd_OpenPort: {
        int p[6];
        XtCmdOpenPort *x = (XtCmdOpenPort *)xt;
        x->getParms(p);
        char buf[512];
        _snprintf_c(buf, sizeof(buf), "Got 'OpenPort' command with params: %d,%d,%d,%d,%d,%d", p[0], p[1], p[2], p[3], p[4], p[5]);
        spikeGL->pushConsoleDebug(buf);
        spikeGL->pushConsoleMsg("*FAKE* port opened ok... since, well, this is an emulated device!");
    }
        break;
    case XtCmd_ServerResource: {
        XtCmdServerResource *x = (XtCmdServerResource *)xt;
        spikeGL->pushConsoleDebug("Got 'ServerResource' command");
        if (x->serverIndex < 0 || x->resourceIndex < 0) {
            probeHardware();
        } else {
            int serverIndex = x->serverIndex;
            int resourceIndex = x->resourceIndex;
            char buf[64];
            _snprintf_c(buf, sizeof(buf), "Setting serverIndex=%d resourceIndex=%d", serverIndex, resourceIndex);
            spikeGL->pushConsoleDebug(buf);
        }
    }
        break;
    default: // ignore....?
        break;
    }
}

static void tellSpikeGLAboutSignalStatus()
{
    if (!acq) return;
    static double tLast = 0.;

    double tNow = getTime();
    if (tNow - tLast > 0.25) {
        XtCmdClkSignals cmd;
        cmd.init(true, true, true, true, true);
        spikeGL->pushCmd(&cmd);
        tLast = tNow;
    }
}

static void timerFunc()
{
    if (!spikeGLIn) return;
	int fail = 0;

	while (spikeGLIn->cmdQSize() && fail < 10) {
        std::vector<BYTE> buf;
        XtCmd *xt;
        if ((xt = spikeGLIn->popCmd(buf, 10))) {
            // todo.. handle commands here...
            handleSpikeGLCommand(xt);
        }
        else ++fail;

    }
    if (spikeGL) tellSpikeGLAboutSignalStatus();
}

void Callback::doCallback() { if (f) f(); }

static void setupTimerFunc(QObject *parent)
{
    Callback *cb = new Callback(timerFunc, parent);
    delete timer; timer = new QTimer(parent);
    QObject::connect(timer, SIGNAL(timeout()), cb, SLOT(doCallback()));
    timer->setSingleShot(false);
    timer->start(100);
    if (!timer || !timer->isActive()) {
        spikeGL->pushConsoleError("Could not create timer at starup!");
    }
}

static void probeHardware()
{
    if (spikeGL) {
        XtCmdServerResource r;
        r.init("FakeServer", "FakeFrameGrabber", 0, 0, 1, true);
        spikeGL->pushCmd(&r);
    }
}

void baseNameify(char *e)
{
    const char *s = e;
    for (const char *t = s; (t = strchr(s, '\\')); ++s) {}
    if (e != s) memmove(e, s, strlen(s) + 1);
}

class MyApp : public QCoreApplication
{
public:
    static MyApp *instance;
    MyApp(int argc, char **argv);
    ~MyApp();
};

MyApp *MyApp::instance = 0;

MyApp::MyApp(int argc, char **argv) : QCoreApplication(argc, argv) {
    if (!instance) {
        spikeGL = new SpikeGLOutThread;
        spikeGLIn = new SpikeGLInputThread;

        setupTimerFunc(this);

        spikeGL->start();

        spikeGL->pushConsoleMsg("Fake_FG slave process started.");

        spikeGLIn->start();
    }
    instance = this;
}

MyApp::~MyApp()
{
    delete spikeGL; delete spikeGLIn;
    spikeGL = 0; spikeGLIn = 0;
    timer->stop(); delete timer; timer = 0;
    instance = 0;
}

int main(int argc, char **argv)
{
    MyApp app(argc,argv);

    return app.exec();
}

#ifdef Q_OS_WINDOWS
static double getTime()
{
    static __int64 freq = 0;
    static __int64 t0 = 0;
    __int64 ct;

    if (!freq) {
        QueryPerformanceFrequency((LARGE_INTEGER *)&freq);
    }
    QueryPerformanceCounter((LARGE_INTEGER *)&ct);   // reads the current time (in system units)    
    if (!t0) {
        t0 = ct;
    }
    return double(ct - t0) / double(freq);
}
#else
static double getTime()
{
    static qint64 base = 0;
    if (!base) base = QDateTime::currentMSecsSinceEpoch();
    return double( (QDateTime::currentMSecsSinceEpoch()-base) / 1e3 );
}
#endif

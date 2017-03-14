// FG_SpikeGL.cpp : Defines the entry point for the console application.
//
#include "stdafx.h"
#include "GlowBalls.h"
#include "XtCmd.h"
#include "FPGA.h"
#include "../../../PagedRingbuffer.h"
#include <varargs.h>

SapAcquisition *acq = 0;
SapBuffer      *buffers = 0;
SapTransfer    *xfer = 0;
std::string configFilename("J_2000+_Electrode_8tap_8bit.ccf");
int serverIndex = -1, resourceIndex = -1;

SpikeGLOutThread    *spikeGL = 0;
SpikeGLInputThread  *spikeGLIn = 0;
UINT_PTR timerId = 0;
bool gotFirstXferCallback = false, gotFirstStartFrameCallback = false;
int bpp, pitch, width=0, height=0;

FPGA *fpga = 0;

int desiredWidth = 144;
int desiredHeight = 32;
unsigned long long frameNum = 0;

std::string shmName("COMES_FROM_XtCmd");
unsigned shmSize(0), shmPageSize(0), shmMetaSize(0);
std::vector<char> metaBuffer; unsigned int *metaPtr = 0; int metaIdx = 0, metaMaxIdx = 0;
std::vector<int> chanMapping;

void *sharedMemory = 0;
PagedScanWriter *writer = 0;
unsigned nChansPerScan = 0;
bool extraAI = false;

static HANDLE hShm = 0;

// some static functions..
static void probeHardware();
static double getTime();

static inline void metaPtrInc() { if (++metaIdx >= metaMaxIdx) metaIdx = 0; }
static inline unsigned int & metaPtrCur() { 
    static unsigned int dummy = 0; 
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

static void acqCallback(SapXferCallbackInfo *info)
{
    (void)info;

    if (!gotFirstXferCallback)
        spikeGL->pushConsoleDebug("acqCallback called at least once! Yay!"), gotFirstXferCallback = true;
    if (!buffers) {
        spikeGL->pushConsoleDebug("INTERNAL ERROR... acqCallback called with 'buffers' pointer NULL!");
        return;
    }
    if (!width) {
        bpp = buffers->GetBytesPerPixel();		// bpp:		get number of bytes required to store a single image
        pitch = buffers->GetPitch();				// pitch:	get number of bytes between two consecutive lines of all the buffer resource
        width = buffers->GetWidth();				// width:	get the width (in pixel) of the image
        height = buffers->GetHeight();				// Height:	get the height of the image
    }
    int w = width, h = height;
    if (w < desiredWidth || h < desiredHeight) {
        char tmp[512];
        _snprintf_c(tmp, sizeof(tmp), "acqCallback got a frame of size %dx%d, but expected a frame of size %dx%d", w, h, desiredWidth, desiredHeight);
        spikeGL->pushConsoleError(tmp);
        xfer->Abort();
        return;
    }
    if (w > desiredWidth) w = desiredWidth;
    if (h > desiredHeight) h = desiredHeight;
    size_t len = w*h;

    const size_t oneScanBytes = nChansPerScan * sizeof(short);

    if (!sharedMemory) {
        spikeGL->pushConsoleError("INTERNAL ERROR.. not attached to shm, cannot send frames!");
        xfer->Abort();
        return;
    }
    if (!writer->scansPerPage()) {
        spikeGL->pushConsoleError("INTERNAL ERROR.. shm page, cannot fit at least 1 scan! FIXME!");
        xfer->Abort();
        return;
    }

	size_t nScansInFrame = (len + (extraAI ? 4 : 0)) / oneScanBytes;

    if (!nScansInFrame) {
        spikeGL->pushConsoleError("Frame must contain at least 1 full scan! FIXME!");
        xfer->Abort();
        return;
    }

    const short *pData = 0;

    buffers->GetAddress((void **)(&pData));			// Get image buffer start memory address.

    if (!pData) {
        spikeGL->pushConsoleError("SapBuffers::GetAddress() returned a NULL pointer!");
        xfer->Abort();
        return;
    }
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
            buffers->ReleaseAddress((void *)pData);
            xfer->Abort();
            return;
        }
        if (nScansInFrame != 1) {
            spikeGL->pushConsoleError("Unsupported frame format! We are expecting a frame where there is exactly one complete scan per frame!");
            buffers->ReleaseAddress((void *)pData);
            xfer->Abort();
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
//            if (1) { // no swab
                bool lastIter = false;
                if ((lastIter = (line + 1 == h))) {
                    metaPtrCur() = *reinterpret_cast<const unsigned int *>(pc + line*pitch);
                    metaPtrInc();
                }
                if (!writer->writePartial(pc + /* <HACK> */ 8 /* </HACK> */ + (line*pitch), w, metaPtr)) {
                    spikeGL->pushConsoleError("PagedScanWriter::writePartial() returned false!");
                    buffers->ReleaseAddress((void *)pData);
                    writer->writePartialEnd();
                    xfer->Abort();
                    return;
                }
                if (extraAI && lastIter) {
                    const unsigned short *ais_in = (const unsigned short *)(pc + 4 + line*pitch);
                    short ais[2];
                    ais[0] = static_cast<short>(int(ais_in[0]) - 32768); // convert unsigned to signed 16 bit
                    ais[1] = static_cast<short>(int(ais_in[1]) - 32768); // ditto
                    if (!writer->writePartial((const char *)ais, 4, metaPtr)) {
                        spikeGL->pushConsoleError("PagedScanWriter::writePartial() for AI chans returned false!");
                        writer->writePartialEnd();
                        xfer->Abort();
                        return;
                    }
                }
 /*           } else { // swab
                if (line + 1 == h) {
                    unsigned long long mts = *reinterpret_cast<const unsigned long long *>(pc + line*pitch);
                    mts = ((mts >> 56) & 0x00000000000000ff) | ((mts >> 40) & 0x000000000000ff00) | ((mts >> 24) & 0x0000000000ff0000) | ((mts >> 8) & 0x00000000ff000000) | ((mts << 8) & 0x000000ff00000000) | ((mts << 24) & 0x0000ff0000000000) | ((mts << 40) & 0x00ff000000000000) | ((mts << 56) & 0xff000000000000);
                    metaPtrCur() = mts;
                    metaPtrInc();
                }
                static char btmp[262144]; // testing hacky buffer
                for (int i = 0; i < w; i += 2) {
                    btmp[i] = pc[8 + (line*pitch) + i+1];
                    btmp[i+1] = pc[8 + (line*pitch) + i];
                }
                if (!writer->writePartial(btmp, w, metaPtr)) {
                    spikeGL->pushConsoleError("PagedScanWriter::writePartial() returned false!");
                    buffers->ReleaseAddress((void *)pData);
                    writer->writePartialEnd();
                    xfer->Abort();
                    return;
                }
            }
*/
        }

        if (!writer->writePartialEnd()) {
            spikeGL->pushConsoleError("PagedScanWriter::writePartialEnd() returned false!");
            buffers->ReleaseAddress((void *)pData);
            xfer->Abort();
            return;
        }
    } else {
        // NOTE: this case is calin's test code.. we fudge the metadata .. even though it makes no sense at all.. to make sure SpikeGL is reading it properly
        metaIdx = metaMaxIdx - 1;
        metaPtrCur() = static_cast<unsigned int>(frameNum);  // HACK!

        /*
        // test channel reordering here..
        short *scan = (short *)pData;
        while ( (scan - ((short *)pData))/nChansPerScan < (int)nScansInFrame) {
            for (int i = 0; i < (int)nChansPerScan; ++i)
                scan[i] = i + 1;
            scan += nChansPerScan;
        }
        // end testing..
        */

        if (!writer->write(pData, unsigned(nScansInFrame),metaPtr)) {
            spikeGL->pushConsoleError("PagedScanWriter::write returned false!");
            buffers->ReleaseAddress((void *)pData); // Need to release it to return it to the hardware!
            xfer->Abort();
            return;
        }
    }

    //double tfwrite = getTime(); /// XXX

    buffers->ReleaseAddress((void *)pData); // Need to release it to return it to the hardware!
    

    /// FPS RATE CODE
    static double lastTS = 0., lastPrt = -1.0;
    static int ctr = 0;
    ++ctr;
    ++frameNum;
    //static double tWriteSum = 0.; /// XXX
    //tWriteSum += tfwrite - t0write; /// XXX

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

static void startFrameCallback(SapAcqCallbackInfo *info) 
{ 
    (void)info;
    if (!gotFirstStartFrameCallback)
        spikeGL->pushConsoleDebug("'startFrameCallback' called at least once! Yay!"), gotFirstStartFrameCallback = true;
}

static void freeSapHandles()
{
    if (xfer && *xfer) xfer->Destroy();
    if (buffers && *buffers) buffers->Destroy();
    if (acq && *acq) acq->Destroy();
    if (xfer) delete xfer, xfer = 0;
    if (buffers) delete buffers, buffers = 0;
    if (acq) delete acq, acq = 0;
    bpp = pitch = width = height = 0;
    gotFirstStartFrameCallback = gotFirstXferCallback = false;
}

static void sapAcqCallback(SapAcqCallbackInfo *p)
{
    if (!p) return;
    SapAcquisition::EventType t = p->GetEventType();
	if (spikeGL) {
		char buf[2048];
		_snprintf_c(buf, sizeof(buf), "SAP ACQ CALLBACK CALLED WITH eventtype=%d", (int)t);
		spikeGL->pushConsoleDebug(buf);
	}
    std::string msg = "";
    //SapAcquisition::EventNoPixelClk|SapAcquisition::EventFrameLost|SapAcquisition::EventPixelClk|SapAcquisition::EventDataOverflow
	if (t & SapAcquisition::EventPixelClk) msg = msg + " Pixel Clock Resumed!";
	if (t & SapAcquisition::EventNoPixelClk) msg = msg + " No Pixel Clock!";
    if (t & SapAcquisition::EventFrameLost) msg = msg + " Frame Lost!";
    if (t & SapAcquisition::EventDataOverflow) msg = msg + " Data Overflow!";
    if (!msg.size()) return;
    if (spikeGL) {
        spikeGL->pushConsoleError(std::string("(SAP Acq Event) ") + msg);
    } else
        fprintf(stderr, "(SAP Acq Event) %s\n", msg.c_str());
}

static void sapStatusCallback(SapManCallbackInfo *p)
{
    if (p->GetErrorMessage() && *(p->GetErrorMessage())) {
        if (spikeGL) {
            spikeGL->pushConsoleDebug(std::string("(SAP Status) ") + p->GetErrorMessage());
        } else {
            fprintf(stderr, "(SAP Status) %s\n", p->GetErrorMessage());
        }
	}
	else {
		int t = int(p->GetEventType());
		std::string msg = "";
		//SapAcquisition::EventNoPixelClk|SapAcquisition::EventFrameLost|SapAcquisition::EventPixelClk|SapAcquisition::EventDataOverflow
		if (t & SapAcquisition::EventPixelClk) msg = msg + " Pixel Clock Resumed!";
		if (t & SapAcquisition::EventNoPixelClk) msg = msg + " No Pixel Clock!";
		if (t & SapAcquisition::EventFrameLost) msg = msg + " Frame Lost!";
		if (t & SapAcquisition::EventDataOverflow) msg = msg + " Data Overflow!";
		if (!msg.size()) return;
		if (spikeGL) {
			spikeGL->pushConsoleError(std::string("(SAP Acq Event) ") + msg);
		}
		else
			fprintf(stderr, "(SAP Acq Event) %s\n", msg.c_str());
	}
}

// Resets server.. as per Jim Chen's recommendation 2/18/2016 email
static void resetHardware(int serverIndex, int timeout_ms = 3500 /* default 3.5 sec reset timeout */)
{    
    char tmp[512];
    SapManager::SetResetTimeout(timeout_ms); 
    if (!SapManager::ResetServer(serverIndex, 1)) {
        _snprintf_c(tmp, sizeof(tmp), "Reset hardware device %d: FAIL (Sapera Error: '%s')", serverIndex, SapManager::GetLastStatus());
        spikeGL->pushConsoleWarning(tmp);
    } else {
        _snprintf_c(tmp, sizeof(tmp), "Reset hardware device %d: success", serverIndex);
        spikeGL->pushConsoleMsg(tmp);
    }
}

static bool setupAndStartAcq()
{
    SapManager::SetDisplayStatusMode(SapManager::StatusCallback, sapStatusCallback, 0); // so we get errors reported properly from SAP

    freeSapHandles();

    metaBuffer.resize(shmMetaSize,0);
    metaPtr = shmMetaSize ? reinterpret_cast<unsigned int *>(&metaBuffer[0]) : 0;
    metaIdx = 0; metaMaxIdx = shmMetaSize / sizeof(unsigned int);

    if (!sharedMemory) {
        char tmp[512];

        hShm = OpenFileMapping(
            FILE_MAP_ALL_ACCESS,   // read/write access
            FALSE,                 // do not inherit the name
            shmName.c_str());               // name of mapping object


        if (!hShm ) {
            _snprintf_c(tmp, sizeof(tmp), "Could not open shared memory (%d).", GetLastError());
            spikeGL->pushConsoleError(tmp);
            return false;
        }

        sharedMemory = (void *)MapViewOfFile(
            hShm,
            FILE_MAP_ALL_ACCESS,  // read/write permission
            0,
            0,
            shmSize);

        if (!sharedMemory) {
            _snprintf_c(tmp, sizeof(tmp), "Could not map shared memory (%d).", GetLastError());
            spikeGL->pushConsoleError(tmp);
            CloseHandle(hShm); hShm = 0;
            return false;
        }
        if (writer) delete writer;
        writer = new PagedScanWriter(nChansPerScan, shmMetaSize, sharedMemory, shmSize, shmPageSize, chanMapping);
        writer->ErrFunc = &PSWErrFunc; writer->DbgFunc = &PSWDbgFunc;
        _snprintf_c(tmp, sizeof(tmp), "Connected to shared memory \"%s\" size: %u  pagesize: %u metadatasize: %u", shmName.c_str(), shmSize, shmPageSize, shmMetaSize);
        spikeGL->pushConsoleDebug(tmp);
    }

    if (serverIndex < 0) serverIndex = 1;
    if (resourceIndex < 0) resourceIndex = 0;

    char acqServerName[128], acqResName[128];
    char tmp[512];

    SapLocation loc(serverIndex, resourceIndex);
    SapManager::GetServerName(serverIndex, acqServerName, sizeof(acqServerName));
    SapManager::GetResourceName(loc, SapManager::ResourceAcq, acqResName, sizeof(acqResName));
    _snprintf_c(tmp, sizeof(tmp), "Server name: %s   Resource name: %s  ConfigFile: %s", acqServerName, acqResName, configFilename.c_str());
    spikeGL->pushConsoleDebug(tmp);

    if (SapManager::GetResourceCount(acqServerName, SapManager::ResourceAcq) > 0)
    {
        int nbufs = NUM_BUFFERS(); if (nbufs < 2) nbufs = 2;

        acq = new SapAcquisition(loc, configFilename.c_str(),
                                 (SapAcquisition::EventType)SapAcquisition::EventNoPixelClk|SapAcquisition::EventFrameLost|SapAcquisition::EventPixelClk|SapAcquisition::EventDataOverflow/*|SapAcquisition::EventCameraBufferOverrun|SapAcquisition::EventCameraMissedTrigger|SapAcquisition::EventExternalTriggerIgnored|SapAcquisition::EventExtLineTriggerTooSlow|SapAcquisition::EventExternalTriggerTooSlow|SapAcquisition::EventLineTriggerTooFast|SapAcquisition::EventVerticalTimeout*/, 
								 sapAcqCallback);
        buffers = new SapBufferWithTrash(nbufs, acq);
        xfer = new SapAcqToBuf(acq, buffers, acqCallback, 0);

        _snprintf_c(tmp, sizeof(tmp), "Will use buffer memory: %dMB in %d sapbufs", BUFFER_MEMORY_MB, nbufs);
        spikeGL->pushConsoleDebug(tmp);

        // Create acquisition object
        if (acq && !*acq && !acq->Create()) {
            spikeGL->pushConsoleError("Failed to Create() acquisition object");
            freeSapHandles();
            return false;
        }
    } else  {
        spikeGL->pushConsoleError("GetResourceCount() returned <= 0");
        freeSapHandles();
        return false;
    }

    //register an acquisition callback
    if (acq) 
        acq->RegisterCallback(SapAcquisition::EventStartOfFrame, startFrameCallback, 0);

    // Create buffer object
    if (buffers && !*buffers && !buffers->Create()) {
        spikeGL->pushConsoleError("Failed to Create() buffers object");
        freeSapHandles();
        return false;
    }

    // Create transfer object
    if (xfer && !*xfer && !xfer->Create()) {
        spikeGL->pushConsoleError("Failed to Create() xfer object");
        freeSapHandles();
        return false;
    }


    // Start continous grab
    return !!(xfer->Grab());
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
        if (x->ccfFile[0]) configFilename = x->ccfFile;
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
        extraAI = x->use_extra_ai;
        if (!setupAndStartAcq())
            spikeGL->pushConsoleWarning("Failed to start acquisition.");
        break;
    }
    case XtCmd_FPGAProto: {
        XtCmdFPGAProto *x = (XtCmdFPGAProto *)xt;
        if (x->len >= 16) {
            spikeGL->pushConsoleDebug("Got 'FPGAProto' command");
            fpga->protocol_Write(x->cmd_code, x->value1, x->value2);
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
        if (fpga) delete fpga;
        fpga = new FPGA(p);

        if (fpga->isOk())
            spikeGL->pushConsoleMsg(fpga->port() + " opened ok; FPGA communications enabled");
        else
            spikeGL->pushConsoleError("COM port error -- failed to start FPGA communications!");
    }
        break;
    case XtCmd_ServerResource: {
        XtCmdServerResource *x = (XtCmdServerResource *)xt;
        spikeGL->pushConsoleDebug("Got 'ServerResource' command");
            if (x->serverIndex < 0 || x->resourceIndex < 0) {
            probeHardware();
        } else {
            serverIndex = x->serverIndex;
            resourceIndex = x->resourceIndex;
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

    if (getTime() - tLast > 0.25) {
        BOOL PixelCLKSignal1, PixelCLKSignal2, PixelCLKSignal3, HSyncSignal, VSyncSignal;
        if (
            acq->GetSignalStatus(SapAcquisition::SignalPixelClk1Present, &PixelCLKSignal1)
            && acq->GetSignalStatus(SapAcquisition::SignalPixelClk2Present, &PixelCLKSignal2)
            && acq->GetSignalStatus(SapAcquisition::SignalPixelClk3Present, &PixelCLKSignal3)
            && acq->GetSignalStatus(SapAcquisition::SignalHSyncPresent, &HSyncSignal)
            && acq->GetSignalStatus(SapAcquisition::SignalVSyncPresent, &VSyncSignal)
            )
        {

            XtCmdClkSignals cmd;
            cmd.init(!!PixelCLKSignal1, !!PixelCLKSignal2, !!PixelCLKSignal3, !!HSyncSignal, !!VSyncSignal);
            spikeGL->pushCmd(&cmd);
        }
        tLast = getTime();
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
/*
#define TESTING_SPIKEGL_INTEGRATION
#ifdef TESTING_SPIKEGL_INTEGRATION
    if (acq)
        for (int iter = 0; iter < 2000; ++iter) {
            // for testing.. put fake frames 
            char buf[144 * 32 + sizeof(XtCmdImg) + 128];
            XtCmdImg *xt = (XtCmdImg *)buf;
            //::memset(xt->img, (iter % 2 ? 0x4f : 0), 144 * 32);
            xt->init(144, 32);
            for (int i = 0; i < 72 * 32; ++i) ((short *)xt->img)[i] = (short)(sinf(((iter + i) % 2560) / 2560.0f)*32768.f);
            spikeGL->pushCmd(xt);
        }
#endif
*/
}

static VOID CALLBACK timerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
    (void)hwnd; (void)uMsg; (void)idEvent; (void)dwTime;
    timerFunc();
}

static void handleSpikeGLEnvParms()
{
    const char *envstr = (char *)getenv("SPIKEGL_CCF");

    if (envstr && *envstr) 
        configFilename = envstr;
}

static void setupTimerFunc()
{
    if (!timerId) {
        timerId = ::SetTimer(NULL, NULL, 100, timerProc);
        if (!timerId) {
            spikeGL->pushConsoleError("Could not create timer at starup!");
        }
    }
}

static void probeHardware()
{
    char buf[512];
    SapManager::SetDisplayStatusMode(SapManager::StatusCallback, sapStatusCallback, 0); // so we get errors reported properly from SAP
    int nServers = SapManager::GetServerCount();
    _snprintf_c(buf, sizeof(buf), "ServerCount: %d", nServers);
    if (spikeGL) spikeGL->pushConsoleDebug(buf);
    else fprintf(stderr, "%s\n", buf);
    for (int i = 0; i < nServers; ++i) {
        resetHardware(i);
        int nRes;
        if ((nRes = SapManager::GetResourceCount(i, SapManager::ResourceAcq)) > 0) {
            char sname[64]; int type; BOOL accessible = SapManager::IsServerAccessible(i);
            if (!SapManager::GetServerName(i, sname)) continue;
            type = SapManager::GetServerType(i);
            for (int j = 0; j < nRes; ++j) {
                char rname[64];
                if (!SapManager::GetResourceName(i, SapManager::ResourceAcq, j, rname, sizeof(rname))) continue;
                _snprintf_c(buf, sizeof(buf), "#%d,%d \"%s\" - \"%s\", type %d accessible: %s",i,j,sname,rname,type,accessible ? "yes" : "no");
                if (spikeGL) spikeGL->pushConsoleDebug(buf);
                else fprintf(stderr, "%s\n", buf);
                if (spikeGL) {
                    XtCmdServerResource r;
                    r.init(sname, rname, i, j, type, !!accessible);
                    spikeGL->pushCmd(&r);
                }
            }
        }
    }
}

void baseNameify(char *e)
{
    const char *s = e;
    for (const char *t = s; t = strchr(s, '\\'); ++s) {}
    if (e != s) memmove(e, s, strlen(s) + 1);
}

int killAllOtherInstances()
{
    HANDLE pseudo = GetCurrentProcess();
    char myExe[MAX_PATH];
    DWORD myPid = GetCurrentProcessId();
    GetProcessImageFileName(pseudo, myExe, sizeof(myExe));
    baseNameify(myExe);

    DWORD pids[16384];
    DWORD npids;

    // get the process by name
    if (!EnumProcesses(pids, sizeof(pids), &npids))
        return -1;

    // convert from bytes to processes
    npids = npids / sizeof(DWORD);
    int ct = 0;
    // loop through all processes
    for (DWORD i = 0; i < npids; ++i) {
        if (pids[i] == myPid) continue; // skip self, obviously!
        // get a handle to the process
        HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pids[i]);
        if (h == INVALID_HANDLE_VALUE) continue;
        char exe[MAX_PATH];
        // get the process name
        if (GetProcessImageFileName(h, exe, sizeof(exe))) {
            baseNameify(exe);
            // terminate all pocesses that contain the name
            if (0 == strcmp(exe, myExe)) {
                TerminateProcess(h, 0);
                ++ct;
            }
        }
        CloseHandle(h);
    }

    if (ct && spikeGL) {
        char buf[256];
        _snprintf_c(buf, sizeof(buf), "killAllOtherInstances() -- killed %d other instances of %s", ct, myExe);
        spikeGL->pushConsoleDebug(buf);
    }

    return ct;
}

int main(int argc, const char* argv[])
{
    // NB: it's vital these two objects get constructed before any other calls.. since other code assumes they are valid and may call these objects' methods
    spikeGL = new SpikeGLOutThread;
    spikeGLIn = new SpikeGLInputThread;

    killAllOtherInstances();

    setupTimerFunc();
    handleSpikeGLEnvParms();

    spikeGL->start();

    spikeGL->pushConsoleMsg("FG_SpikeGL.exe slave process started.");

    spikeGLIn->start();

    // message pump...
    MSG msg;
    BOOL bRet;
    while ((bRet = GetMessage(&msg, NULL, 0, 0)) != 0)  {
        if (bRet == -1)  {
            // Handle Error
        } else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return (int)msg.wParam;
}


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

// FG_SpikeGL.cpp : Defines the entry point for the console application.
//
#include "stdafx.h"
#include "GlowBalls.h"
#include "XtCmd.h"
#include "FPGA.h"

SapAcquisition *acq = 0;
SapBuffer      *buffers = 0;
SapTransfer    *xfer = 0;
SapView        *view = 0;
//    std::string configFilename ("c:\\users\\calin\\Desktop\\Src\\SpikeGL\\Framegrabber\\J_2000+_Electrode_8tap_8bit.ccf");
std::string configFilename("J_2000+_Electrode_8tap_8bit.ccf");

SpikeGLOutThread    *spikeGL = 0;
SpikeGLInputThread  *spikeGLIn = 0;
UINT_PTR timerId = 0;
bool gotFirstXferCallback = false;
std::vector<BYTE> spikeGLFrameBuf;

FPGA *fpga = 0;

static void acqCallback(SapXferCallbackInfo *info) 
{ 
    SapView *v = (SapView *)info;
    //spikeGL->pushConsoleDebug("acqCallback called");
    v->Show();

    if (buffers) {
        XtCmdImg *xt = 0;
        BYTE *pXt = 0;
        BYTE *pData = 0;

        int bpp = buffers->GetBytesPerPixel();		// bpp:		get number of bytes required to store a single image
        int pitch = buffers->GetPitch();				// pitch:	get number of bytes between two consecutive lines of all the buffer resource
        int width = buffers->GetWidth();				// width:	get the width (in pixel) of the image
        int height = buffers->GetHeight();				// Height:	get the height of the image

        if (!gotFirstXferCallback) 
            spikeGL->pushConsoleDebug("acqCallback called at least once! Yay!"), gotFirstXferCallback = true;
        size_t len = (sizeof(XtCmdImg) - 1) + width*height;
        if (size_t(spikeGLFrameBuf.size()) < len) spikeGLFrameBuf.resize(len);
        if (size_t(spikeGLFrameBuf.size()) >= len) {
            xt = (XtCmdImg *)&(spikeGLFrameBuf[0]);
            xt->init(width, height);
            pXt = xt->img;
        }

        if (!pXt) {
            spikeGL->pushConsoleError("INTERNAL ERROR.. pXt is NULL!");
            return;
        }

        buffers->GetAddress((void **)(&pData));			// Get image buffer start memory address.

        if (!pData) {
            spikeGL->pushConsoleError("SapBuffers::GetAddress() returned a NULL pointer!");
            return;
        }

        if (pitch == width) {
            memcpy(pXt, pData, width*height);
        } else {
            // copy each row (line) of pixels.  Note the pitch parameter used to skip lines in the source image..
            for (int i = 0; i < height; ++i)
                memcpy(pXt + i*width, pData + i*pitch, width);
        }

        buffers->ReleaseAddress(pData); // Need to release it to return it to the hardware!

        // for SpikeGL
        if (xt) {
            if (!spikeGL->pushCmd(xt)) { /* todo:.. handle error here!*/ }
        }
    }
}

static void startFrameCallback(SapAcqCallbackInfo *info) 
{ 
    (void)info;
    spikeGL->pushConsoleDebug("startFrameCallback called");
}

static void freeSapHandles()
{
    if (xfer && *xfer) xfer->Destroy();
    if (view && *view) view->Destroy();
    if (buffers && *buffers) buffers->Destroy();
    if (acq && *acq) acq->Destroy();
    if (xfer) delete xfer, xfer = 0;
    if (view) delete view, view = 0;
    if (buffers) delete buffers, buffers = 0;
    if (acq) delete acq, acq = 0;
}

static void sapStatusCallback(SapManCallbackInfo *p)
{
    if (spikeGL && p->GetErrorMessage() && *(p->GetErrorMessage())) {
        spikeGL->pushConsoleDebug(std::string("(SAP Status) ") + p->GetErrorMessage());
    }
}

static bool setupAndStartAcq()
{
    SapManager::SetDisplayStatusMode(SapManager::StatusCallback, sapStatusCallback, 0); // so we get errors reported properly from SAP

    freeSapHandles();

    UINT32 acqDeviceNumber = 0;
    char acqServerName[128], acqResName[128];

    SapLocation loc(1, acqDeviceNumber);
    SapManager::GetServerName(1, acqServerName, sizeof(acqServerName));
    SapManager::GetResourceName(loc, SapManager::ResourceAcq, acqResName, sizeof(acqResName));
    char tmp[512];
    _snprintf_c(tmp, sizeof(tmp), "Server name: %s   Resource name: %s  ConfigFile: %s", acqServerName, acqResName, configFilename.c_str());
    spikeGL->pushConsoleDebug(tmp);

    if (SapManager::GetResourceCount(acqServerName, SapManager::ResourceAcq) > 0)
    {
        acq = new SapAcquisition(loc, configFilename.c_str());
        buffers = new SapBufferWithTrash(NUM_BUFFERS+1, acq);
        view = new SapView(buffers, SapHwndAutomatic);
        xfer = new SapAcqToBuf(acq, buffers, acqCallback, view);

        // Create acquisition object
        if (acq && !*acq && !acq->Create()) {
            spikeGL->pushConsoleError("Failed to Create() acquisition object");
            freeSapHandles();
            return false;
        }
    } else  {
        spikeGL->pushConsoleError("GetResrouceCount() returned <= 0");
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

    // Create view object
    if (view && !*view && !view->Create()) {
        spikeGL->pushConsoleError("Failed to Create() view object");
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
    case XtCmd_GrabFrames:
        spikeGL->pushConsoleDebug("Got 'GrabFrames' command");
        if (!setupAndStartAcq())
            spikeGL->pushConsoleWarning("Failed to start acquisition.");
        break;
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
    default: // ignore....?
        break;
    }
}

static void tellSpikeGLAboutSignalStatus()
{
    if (!acq) return;
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

int main(int argc, const char* argv[])
{
    // NB: it's vital these two objects get constructed before any other calls.. since other code assumes they are valid and may call these objects' methods
    spikeGL = new SpikeGLOutThread;
    spikeGLIn = new SpikeGLInputThread;

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


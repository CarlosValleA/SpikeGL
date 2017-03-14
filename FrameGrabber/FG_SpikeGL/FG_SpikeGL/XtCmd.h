#ifndef XTCMD_H
#define XTCMD_H

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

#define XT_CMD_MAGIC 0xf33d0a76

enum XtCmds {
    XtCmd_Null = 0, XtCmd_Noop = XtCmd_Null,
    XtCmd_Test, // test command. does nothing but used to test communication
    XtCmd_Img, // got image from subprocess -> SpikeGL
    XtCmd_ConsoleMessage, // basically ends up in SpikeGL's console
    XtCmd_Exit, // sent from SpikeGL -> slave app to tell it to exit gracefully...
    XtCmd_GrabFrames, // sent from SpikeGL-> slave app, tell it to start grabbing frames..
    XtCmd_FPGAProto, // sent from SpikeGL -> slave app to do low-level FPGA protocol commands
    XtCmd_ClkSignals, // sent from slave app -> SpikeGL to update GUI
    XtCmd_OpenPort, // sent from SpikeGL -> slave app to start an acquisition
    XtCmd_ServerResource, // sent SpikeGL <-> slave app (both directions) to either list the available server resources or set the active server
    XtCmd_FPS, ///< sent from slave process -> SpikeGL to report FPS (frames per second) coming in from the camera
    XtCmd_N // num commands in enum
};

struct XtCmd {
    int magic; ///< should always equal XT_CMD_MAGIC
    int cmd; ///< one of XtCmds enum above
    int len; ///< in bytes, of the data array below!
    union {
        unsigned char data[1]; ///< the size of this array varies depending on the command in question!
        int param; ///< generic param plus also acts as padding... to make subclass members 32-bit word-aligned..
    };

    void init() { magic = XT_CMD_MAGIC; len = sizeof(int); cmd = XtCmd_Noop; param = 0; }
    XtCmd() { init();  }

    bool write(FILE *f) const { 
        size_t n = len + offsetof(XtCmd, data);
        size_t r = fwrite(this, n, 1, f);
        return r==1;
    }

    /// read data from file f and put it in output buffer "buf".  If buf is too small to fit the incoming data, buf is grown as needed to accomodate the incoming data (but is never shrunk!)
    /// returns a valid pointer into "buf" (type cast correctly) on success or NULL if there was an error.  Buf may be modified in either case!
    static XtCmd * read(std::vector<unsigned char> & buf, FILE *f) {
        int fields[3];
        size_t r, n = sizeof(int) * 3;
        r = ::fread(&fields, n, 1, f);
        XtCmd *xt = 0;
        if (r == 1 && fields[2] >= 0 && fields[0] == int(XT_CMD_MAGIC) && fields[2] <= (1024 * 1024 * 10)) { // make sure everything is kosher with the input, and that len is less than 10MB (hard limit to prevent program crashes)
            if (size_t(buf.size()) < size_t(fields[2] + n)) buf.resize(fields[2] + n);
            xt = (XtCmd *)&buf[0];
            xt->magic = fields[0]; xt->cmd = fields[1]; xt->len = fields[2]; xt->param = 0;
            if ( xt->len > 0 && ::fread(xt->data, xt->len, 1, f) != 1 ) 
                 xt = 0; // error on read, make returned pointer null
        }
        return xt;
    }

    static const XtCmd *parseBuf(const unsigned char *buf, int bufsz, int & num_consumed) {
        num_consumed = 0;
        for (int i = 0; int(i+sizeof(int)) <= bufsz; ++i) {
            int *bufi = (int *)&buf[i];
            int nrem = bufsz-i;
            if (*bufi == int(XT_CMD_MAGIC)) {
                if (nrem < (int)sizeof(XtCmd)) return 0; // don't have a full struct's full of data yet, return out of this safely
                XtCmd *xt = (XtCmd *)bufi;
                nrem -= offsetof(XtCmd,data);
                if (xt->len <= nrem) { // make sure we have all the data in the buffer, and if so, update num_consumed and return the pointer to the data
                   nrem -= xt->len;
                   num_consumed = bufsz-nrem;
                   return xt;
                }
            }
        }
        return 0; // if this is reached, num_consumed is 0 and the caller should call this function again later when more data arrives
    }

};

/*
struct XtCmdImg : public XtCmd {
    unsigned long long frameNum; ///< first frame is 1.  Keeps getting incremented.
    int w, h; ///< in px, the image is always 8bit (grayscale) 
    union {
        unsigned char img[1];
        int imgPadding;
    };

    void init(int width, int height, unsigned long long frame = 0) {
        XtCmd::init(); 
        cmd = XtCmd_Img; 
        w = width;
        h = height;
        frameNum = frame;
        len = (sizeof(*this) - sizeof(XtCmd)) + w*h - 1;
        imgPadding = 0;
    }
    XtCmdImg() { init(0,0);  }
};
*/

struct XtCmdConsoleMsg : public XtCmd {
	enum MsgType { Normal = 0, Debug, Warning, Error, N_MsgType };
	int msgType;
	union {
		char msg[1];
		int padding2;
	};
	
    static XtCmdConsoleMsg *allocInit(const std::string & message, int mtype) {
        void *mem = malloc(sizeof(XtCmdConsoleMsg)+message.length()+1);
        XtCmdConsoleMsg *ret = (XtCmdConsoleMsg *)mem;
        if (ret) ret->init(message,mtype);
        return ret;
    }

    void init(const std::string & message, int mtype=Normal) {
        XtCmd::init();
		msgType = mtype;
        cmd = XtCmd_ConsoleMessage;
        len = static_cast<int>(message.length()+1+(sizeof(XtCmdConsoleMsg)-sizeof(XtCmd)));
        ::memcpy(msg, &message[0], message.length());
        msg[message.length()] = 0;
    }
};

struct XtCmdFPGAProto : public XtCmd {
    int cmd_code, value1, value2;

    void init(int cmdCode, int v1, int v2) {
        XtCmd::init();
        cmd = XtCmd_FPGAProto;
        len = static_cast<int>((sizeof(XtCmdFPGAProto) - sizeof(XtCmd)) + sizeof(int));
        cmd_code = cmdCode; value1 = v1; value2 = v2;
    }
};

struct XtCmdClkSignals : public XtCmd {
    void init(bool pxclk1, bool pxclk2, bool pxclk3, bool hsync, bool vsync) {
        XtCmd::init();
        cmd = XtCmd_ClkSignals;
        param = ((pxclk1 ? 0x1 : 0) << 0) | ((pxclk2 ? 0x1 : 0) << 1) | ((pxclk3 ? 0x1 : 0) << 2) | ((hsync ? 0x1 : 0) << 3) | ((vsync ? 0x1 : 0) << 4);
    }
    bool isPxClk1() const { return !!(param&(0x1 << 0)); }
    bool isPxClk2() const { return !!(param&(0x1 << 1)); }
    bool isPxClk3() const { return !!(param&(0x1 << 2)); }
    bool isHSync() const { return !!(param&(0x1 << 3)); }
    bool isVSync() const { return !!(param&(0x1 << 4)); }
};

struct XtCmdFPS : public XtCmd {
    void init(double fps) {
        XtCmd::init();
        cmd = XtCmd_FPS;
        if (fps < 0.) fps = 0.;
        param = int(fps + 0.5);
    }
};

struct XtCmdOpenPort : public XtCmd {
    void init(const int parms[6]) {
        XtCmd::init();
        cmd = XtCmd_OpenPort;
        param = 0;
        for (int i = 0; i < 6; ++i) param |= parms[i] << (i * 4);
    }
    void getParms(int parms[6]) {
        for (int i = 0; i < 6; ++i) {
            parms[i] = (param >> (i * 4)) & 0xf;
        }
    }
};

struct XtCmdServerResource : public XtCmd {
    char serverName[64];
    char resourceName[64];
    int serverIndex, resourceIndex, serverType; ///< set serverIndex to -1 if communicating from SpikeGL->slave app to get a list of all servers and resources of type SapManager::ResourceAcq
    bool accessible;

    void init(const std::string &snam, const std::string & rnam, int sidx, int ridx, int typ, bool ac) {
        XtCmd::init();
        cmd = XtCmd_ServerResource;
        strncpy(serverName, snam.c_str(), sizeof(serverName) - 1); serverName[sizeof(serverName) - 1] = 0;
        strncpy(resourceName, rnam.c_str(), sizeof(resourceName) - 1); resourceName[sizeof(resourceName) - 1] = 0;
        serverIndex = sidx;
        resourceIndex = ridx;
        serverType = typ;
        accessible = ac;
        len = static_cast<int>((sizeof(XtCmdServerResource) - sizeof(XtCmd)) + sizeof(int));
    }
};

struct XtCmdGrabFrames : public XtCmd {
    char ccfFile[256]; ///< which .ccf file should sapera use?
    char shmName[256];
    int shmSize, shmPageSize, shmMetaSize;
    int frameW, frameH; ///< in 8-bit pixels
    int numChansPerScan; ///< size of 1 full scan, in 16-bit samples.  If the incoming frame is larger than 1 scan, it will be chopped up into multiple frames, 1 per scan, when sent back to spikegl
    int use_map;
    int mapping[8192];
    bool use_extra_ai;

    void init(const std::string & nam, unsigned shmSz, unsigned pageSz, unsigned metaSz, const std::string &ccf, unsigned w, unsigned h, unsigned scanSize, const int *map = 0, bool extraAI = false) {
        XtCmd::init();
        cmd = XtCmd_GrabFrames;
        strncpy(ccfFile, ccf.c_str(), sizeof(ccfFile) - 1);  ccfFile[sizeof(ccfFile)-1] = 0;
        strncpy(shmName, nam.c_str(), sizeof(shmName) - 1);  shmName[sizeof(shmName)-1] = 0;
        shmSize = (int)shmSz;
        shmPageSize = (int)pageSz;
        shmMetaSize = (int)metaSz;
        frameW = int(w); frameH = int(h);
        numChansPerScan = scanSize;
        len = static_cast<int>((sizeof(XtCmdGrabFrames) - sizeof(XtCmd)) + sizeof(int));
        if ((use_map = !!map)) {
            int nbytes = sizeof(*map) * scanSize;
            if (nbytes > (int)sizeof(mapping)) nbytes = sizeof(mapping);
            memcpy(mapping, map, nbytes);
        }
        use_extra_ai = extraAI;
    }
};


#endif

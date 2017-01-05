#include <QCoreApplication>
#include <QThread>
#include <QString>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <list>
#include <string>

#ifndef Q_OS_WIN
#define _snprintf_c snprintf
#define _vsnprintf_s vsnprintf
typedef unsigned char BYTE;
#endif

#include "SpikeGLHandlerThread.h"

// Sapera-related
#define BUFFER_MEMORY_MB 16
extern int desiredHeight; // 32
extern int desiredWidth; // 144
#define NUM_BUFFERS()  ((BUFFER_MEMORY_MB*1024*1024) / (desiredHeight*desiredWidth) )
extern unsigned long long frameNum; // starts at 0, first frame sent is 1

extern std::string shmName;
extern unsigned shmSize, shmPageSize;

// SpikeGL communication related
extern SpikeGLOutThread    *spikeGL;
extern SpikeGLInputThread  *spikeGLIn;

/// Misc
extern std::vector<unsigned char> spikeGLFrameBuf;
extern bool gotFirstXferCallback, gotFirstStartFrameCallback;
extern int bpp, pitch, width, height;

class Callback : public QObject
{
    Q_OBJECT
public:

    typedef void (*Func)(void);

    Callback(Func cb, QObject *parent = 0) : QObject(parent), f(cb) {}
public slots:
    void doCallback();
protected:
    Func f;
};

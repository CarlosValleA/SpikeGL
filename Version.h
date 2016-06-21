#ifndef Version_H
#define Version_H

#define VERSION 0x20162106
#ifdef WIN64
#  define VERSION_STR "SpikeGL Win64 v.20162106"
#elif defined(MACX)
#  define VERSION_STR "SpikeGL OSX v.20162106"
#else
#  define VERSION_STR "SpikeGL v.20162106"
#endif


#endif

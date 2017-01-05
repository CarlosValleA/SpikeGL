#ifndef Version_H
#define Version_H

#define VERSION 0x20170105
#ifdef WIN64
#  define VERSION_STR "SpikeGL Win64 v.20170105"
#elif defined(MACX)
#  define VERSION_STR "SpikeGL OSX v.20170105"
#else
#  define VERSION_STR "SpikeGL v.20170105"
#endif


#endif

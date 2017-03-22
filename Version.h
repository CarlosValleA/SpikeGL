#ifndef Version_H
#define Version_H

#define VERSION 0x20170322
#ifdef WIN64
#  define VERSION_STR "SpikeGL Win64 v.20170322"
#elif defined(MACX)
#  define VERSION_STR "SpikeGL OSX v.20170322"
#else
#  define VERSION_STR "SpikeGL v.20170322"
#endif


#endif

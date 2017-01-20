#ifndef Version_H
#define Version_H

#define VERSION 0x20170120
#ifdef WIN64
#  define VERSION_STR "SpikeGL Win64 v.20170120"
#elif defined(MACX)
#  define VERSION_STR "SpikeGL OSX v.20170120"
#else
#  define VERSION_STR "SpikeGL v.20170120"
#endif


#endif

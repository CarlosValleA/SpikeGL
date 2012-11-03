#include "SpikeGL.h"
#include "DAQ.h"
#ifdef HAVE_NIDAQmx
#  include "NI/NIDAQmx.h"
#  include "AOWriteThread.h"
#else
#  define FAKEDAQ
#ifndef Q_OS_WIN
#  warning Not a real NI platform.  All acquisition related functions are emulated!
#endif
#endif

#include <string.h>
#include <QString>
#include <QFile>
#include <QMessageBox>
#include <QApplication>
#include <QRegExp>
#include <QThread>
#include <QPair>
#include <QSet>
#include <QMutexLocker>
#include <math.h>
#include "SampleBufQ.h"

#define DAQmxErrChk(functionCall) do { if( DAQmxFailed(error=(functionCall)) ) { callStr = STR(functionCall); goto Error_Out; } } while (0)

namespace DAQ 
{
    static bool noDaqErrPrint = false;

	const unsigned ModeNumChansPerIntan[N_Modes] = {
		15, 0, 15, 16, 16, 32
	};
	
	const unsigned ModeNumIntans[N_Modes] = {
		4, 0, 8, 2, 8, 8
	};
	
    /// if empty map returned, no devices with AI!
    DeviceRangeMap ProbeAllAIRanges() 
    {
        DeviceRangeMap ret;
        Range r;
#ifdef HAVE_NIDAQmx
        double myDoubleArray[512];
        for (int devnum = 1; devnum <= 16; ++devnum) {
            memset(myDoubleArray, 0, sizeof(myDoubleArray));
            QString dev( QString("Dev%1").arg(devnum) );
            if (!DAQmxFailed(DAQmxGetDevAIVoltageRngs(dev.toUtf8().constData(), myDoubleArray, 512))) {
                for (int i=0; i<512; i=i+2) {
                    r.min = myDoubleArray[i];
                    r.max = myDoubleArray[i+1];
                    if (r.min == r.max) break;
                    ret.insert(dev, r);
                }
            }
        }
#else // !WINDOWS, emulate
        r.min = -2.5;
        r.max = 2.5;
        ret.insert("Dev1", r);
        r.min = -5.;
        r.max = 5.;
        ret.insert("Dev1", r);
#endif
        return ret;
    }
    /// if empty map returned, no devices with AO!
    DeviceRangeMap ProbeAllAORanges()
    {
        DeviceRangeMap ret;
        Range r;
#ifdef HAVE_NIDAQmx
        double myDoubleArray[512];
        for (int devnum = 1; devnum <= 16; ++devnum) {
            memset(myDoubleArray, 0, sizeof(myDoubleArray));
            QString dev( QString("Dev%1").arg(devnum) );
            if (!DAQmxFailed(DAQmxGetDevAOVoltageRngs(dev.toUtf8().constData(), myDoubleArray, 512))) {
                for (int i=0; i<512; i=i+2) {
                    r.min = myDoubleArray[i];
                    r.max = myDoubleArray[i+1];
                    if (r.min == r.max) break;
                    ret.insert(dev, r);
                }
            }
        }
#else // !WINDOWS, emulate
        r.min = -2.5;
        r.max = 2.5;
        ret.insert("Dev1", r);
        r.min = -5.;
        r.max = 5.;
        ret.insert("Dev1", r);
#endif
        return ret;
    }

    DeviceChanMap ProbeAllAIChannels() {
        bool savedPrt = noDaqErrPrint;
        noDaqErrPrint = true;
        DeviceChanMap ret;
        for (int devnum = 1; devnum <= 16; ++devnum) {
            QString dev( QString("Dev%1").arg(devnum) );
            QStringList l = GetAIChans(dev);
            if (!l.empty()) {
                ret[dev] = l;
            }
        }
        noDaqErrPrint = savedPrt;
        return ret;
    }

    DeviceChanMap ProbeAllAOChannels() {
        bool savedPrt = noDaqErrPrint;
        noDaqErrPrint = true;
        DeviceChanMap ret;
        for (int devnum = 1; devnum <= 16; ++devnum) {
            QString dev( QString("Dev%1").arg(devnum) );
            QStringList l = GetAOChans(dev);
            if (!l.empty()) {
                ret[dev] = l;
            }
        }
        noDaqErrPrint = savedPrt;
        return ret;
    }
    

#if HAVE_NIDAQmx
    typedef int32 (__CFUNC *QueryFunc_t)(const char [], char *, uInt32);
    
    static QStringList GetPhysChans(const QString &devname, QueryFunc_t queryFunc, const QString & fn = "") 
    {
        int error;
        const char *callStr = "";
        char errBuff[2048];        
        char buf[65536] = "";
        QString funcName = fn;

        if (!funcName.length()) {
            funcName = "??";
        }

        DAQmxErrChk(queryFunc(devname.toUtf8().constData(), buf, sizeof(buf)));
        return QString(buf).split(QRegExp("\\s*,\\s*"), QString::SkipEmptyParts);
        
    Error_Out:
        if( DAQmxFailed(error) )
            DAQmxGetExtendedErrorInfo(errBuff,2048);
        if( DAQmxFailed(error) ) {
            if (!noDaqErrPrint) {
                Error() << "DAQmx Error: " << errBuff;
                Error() << "DAQMxBase Call: " << funcName << "(" << devname << ",buf," << sizeof(buf) << ")";
            }
        }
        
        return QStringList();         
    }
#endif

    QStringList GetDOChans(const QString & devname) 
    {
#ifdef HAVE_NIDAQmx
        return GetPhysChans(devname, DAQmxGetDevDOLines, "DAQmxGetDevDOLines");
#else // !HAVE_NIDAQmx, emulated, 1 chan
        return QStringList(QString("%1/port0/line0").arg(devname));
#endif
    }

    QStringList GetAIChans(const QString & devname)
    {
#ifdef HAVE_NIDAQmx
        return GetPhysChans(devname, DAQmxGetDevAIPhysicalChans, "DAQmxGetDevAIPhysicalChans");
#else // !HAVE_NIDAQmx, emulated, 60 chans
        QStringList ret;
        if (devname == "Dev1") {
            for (int i = 0; i < 60; ++i) {
                ret.push_back(QString("%1/ai%2").arg(devname).arg(i));
            }
        }
        return ret;
#endif
    }

    QStringList GetAOChans(const QString & devname)
    {
#ifdef HAVE_NIDAQmx
        return GetPhysChans(devname, DAQmxGetDevAOPhysicalChans, "DAQmxGetDevAOPhysicalChans");
#else // !HAVE_NIDAQmx, emulated, 2 chans
        QStringList ret;
        if (devname == "Dev1") {
            for (int i = 0; i < 2; ++i) {
                ret.push_back(QString("%1/ao%2").arg(devname).arg(i));
            }
        }
        return ret;
#endif
    }

    /// returns the number of physical channels in the AI subdevice for this device, or 0 if AI not supported on this device
    unsigned GetNAIChans(const QString & devname)
    {
        return GetAIChans(devname).count();
    }
    /// returns the number of physical channels in the AO subdevice for this device, or 0 if AO not supported on this device
    unsigned GetNAOChans(const QString & devname)
    {
        return GetAOChans(devname).count();
    }


    /// returns true iff the device supports AI simultaneous sampling
    bool     SupportsAISimultaneousSampling(const QString & devname)
    {
#ifdef HAVE_NIDAQmx
        bool32 ret = false;
        if (DAQmxFailed(DAQmxGetDevAISimultaneousSamplingSupported(devname.toUtf8().constData(), &ret))) {
            Error() << "Failed to query whether dev " << devname << " AI supports simultaneous sampling.";
        }
        return ret;
#else // !HAVE_NIDAQmx, emulated
        (void)devname;
        return true;
#endif
    }

    double   MaximumSampleRate(const QString & dev, int nChans) 
    {
        double ret = 1e6;
        (void)dev; (void)nChans;
#ifdef HAVE_NIDAQmx
        float64 val;
        int32 e;
        if (nChans <= 0) nChans = 1;
        if (nChans == 1)
            e = DAQmxGetDevAIMaxSingleChanRate(dev.toUtf8().constData(), &val);
        else
            e = DAQmxGetDevAIMaxMultiChanRate(dev.toUtf8().constData(), &val);
        if (DAQmxFailed(e)) {
            Error() << "Failed to query maximum sample rate for dev " << dev << ".";
        } else {
            ret = val;
            if (nChans > 1 && !SupportsAISimultaneousSampling(dev)) {
                ret = ret / nChans;
            }
        }
#endif
        return ret;        
    }

    double   MinimumSampleRate(const QString & dev)
    {
        double ret = 10.;
        (void)dev;
#ifdef HAVE_NIDAQmx
        float64 val;
        if (DAQmxFailed(DAQmxGetDevAIMinRate(dev.toUtf8().constData(), &val))) {
            Error() << "Failed to query minimum sample rate for dev " << dev << ".";
            
        } else {
            ret = val;
        }
#endif
        return ret;
    }

    QString  GetProductName(const QString &dev)
    {
#ifdef HAVE_NIDAQmx
        char buf[65536] = "Unknown";
        if (DAQmxFailed(DAQmxGetDevProductType(dev.toUtf8().constData(), buf, sizeof(buf)))) {
            Error() << "Failed to query product name for dev " << dev << ".";
        } 
        // else..
        return buf;
#else
        (void)dev;
        return "FakeDAQ";
#endif
    }
	
    Task::Task(const Params & acqParams, QObject *p) 
        : QThread(p), SampleBufQ(128), pleaseStop(false), params(acqParams), 
          fast_settle(0), muxMode(false), totalRead(0LL)
    {
		setDO(false); // assert DO is low when stopped...
    }

    Task::~Task()
    {
        stop();
    }


    void Task::stop() 
    {
        if (isRunning() && !pleaseStop) {
            pleaseStop = true;
            wait();
            pleaseStop = false;
        }
		setDO(false); // assert DO low when stopped...
    }

    void Task::run()
    {
        daqThr();
    }

    void Task::overflowWarning() 
    {
        Warning() << "DAQ Task sample buffer overflow!  Queue has " << dataQueueMaxSize << " buffers in it!  Dropping a buffer!";
        emit(bufferOverrun());
#ifdef FAKEDAQ
        static int overflowct = 0;
    
        if (++overflowct == 5) {
            emit(daqError("Overflow limit exceeded"));
        }
#endif
    }

    /* static */
    int Task::computeTaskReadFreq(double srate_in) {
        int srate = ceil(srate_in); (void)srate;
        return DEF_TASK_READ_FREQ_HZ;
        /*
        // try and figure out how often to run the task read function.  defaults to 10Hz
        // but if that isn't groovy with the sample rate, try a freq that is more groovy
        int task_freq = DEF_TASK_READ_FREQ_HZ;
        while (task_freq >= 3 && (srate % task_freq)) 
            --task_freq;
        if (task_freq < 3) {
            for (task_freq = DEF_TASK_READ_FREQ_HZ; (srate % task_freq) && task_freq <= DEF_TASK_READ_FREQ_HZ*2; 
                 ++task_freq)
                ;
            if (srate % task_freq)
                task_freq = 1;// give up and use 1Hz!
        }    
        Debug() << "Using task read freq: " << task_freq << "Hz.";        
        return task_freq;
         */
    }
    
#ifdef FAKEDAQ
}// end namespace DAQ

#include <stdlib.h>

namespace DAQ 
{
    void Task::daqThr()
    {
        static QString fname("fakedaqdata.bin");
        char *e;
        if ((e=getenv("FAKEDAQ"))) {
            fname = e;
        } else {
            Warning() << "FAKEDAQ env var not found, using " << fname << " as filename instead";
        }
        QFile f(fname);
        if (!f.open(QIODevice::ReadOnly)) {
            QString err = QString("Could not open %1!").arg(fname);
            Error() << err;
            emit daqError(err);        
            return;
        }
        std::vector<int16> data;
        const double onePd = int(params.srate/double(computeTaskReadFreq(params.srate)));
        while (!pleaseStop) {
            data.resize(unsigned(params.nVAIChans*onePd));
            qint64 nread = f.read((char *)&data[0], data.size()*sizeof(int16));
            if (nread != data.size()*sizeof(int16)) {
                f.seek(0);
            } else if (nread > 0) {
                nread /= sizeof(int16);
                data.resize(nread);
                if (!totalRead) emit(gotFirstScan());
                enqueueBuffer(data, totalRead);
      
                totalRead += nread;
            }
            usleep(int((1e6/params.srate)*onePd));
        }
    }

    void Task::setDO(bool onoff)
    {
        Warning() << "setDO(" << (onoff ? "on" : "off") << ") called (unimplemented in FAKEDAQ mode)";
    }


    void Task::requestFastSettle() 
    {
        Warning() << "requestFastSettle() unimplemented for FAKEDAQ mode!";
        emit(fastSettleCompleted());
    }

#else // !FAKEDAQ

    void AOWriteThread::run()
    {
        const float64     aoTimeout = DAQ_TIMEOUT*2.;
        unsigned aoWriteCt = 0;
        pleaseStop = false;
        int32       error = 0;
        char        errBuff[2048]={'\0'};
        const char *callStr = "";
        const Params & p(params);
        const int32 aoChansSize = p.aoChannels.size();
        const int32 aoSamplesPerChan = aoBufferSize/aoChansSize;
        std::vector<int16> leftOver;
        while (!pleaseStop) {
            double t0 = getTime();
            std::vector<int16> samps;
            u64 sampCount;
            dequeueBuffer(samps, sampCount);
            samps.insert(samps.begin(), leftOver.begin(), leftOver.end());
            leftOver.clear();
            unsigned sampIdx = 0;
            while (sampIdx < samps.size()) {
                int32 nScansToWrite = (samps.size()-sampIdx)/aoChansSize;
                int32 nScansWritten = 0;
                if (nScansToWrite > aoSamplesPerChan)
                    nScansToWrite = aoSamplesPerChan;
                else if (nScansToWrite < aoSamplesPerChan) {
                    leftOver.insert(leftOver.end(), samps.begin()+sampIdx, samps.end());
                    break;
                }

                DAQmxErrChk(DAQmxWriteBinaryI16(taskHandle, nScansToWrite, 1, aoTimeout, DAQmx_Val_GroupByScanNumber, &samps[sampIdx], &nScansWritten, NULL));
                if (nScansWritten != nScansToWrite) {
                    Error() << "nScansWritten (" << nScansWritten << ") != nScansToWrite (" << nScansToWrite << ")";
                    break;
                }    
                const double tWrite(getTime()-t0);
                if (tWrite > 0.250) {
                    Debug() << "AOWrite #" << aoWriteCt << " " << nScansWritten << " scans " << aoChansSize << " chans" << " (bufsize=" << aoBufferSize << ") took: " << tWrite << " secs";
                }
                ++aoWriteCt;
                sampIdx += nScansWritten*aoChansSize;
            }
        }

    Error_Out:
        if ( DAQmxFailed(error) )
            DAQmxGetExtendedErrorInfo(errBuff,2048);
        if ( taskHandle != 0) {
            DAQmxStopTask (taskHandle);
        }
        if( DAQmxFailed(error) ) {
            QString e;
            e.sprintf("DAQmx Error: %s\nDAQMxBase Call: %s",errBuff, callStr);
            if (!noDaqErrPrint) {
                Error() << e;
            }
            emit daqError(e);
        }        
    }

    /* static */
    void Task::recomputeAOAITab(QVector<QPair<int,int> > & aoAITab,
                                QString & aoChan,
                                const Params & p)
    {
        aoAITab.clear();
        aoChan.clear();
        if (p.aoPassthru) {
            const QVector<QString> aoChanStrings ((ProbeAllAOChannels()[p.aoDev]).toVector());
            const int aoNChans = aoChanStrings.size();
            QSet<int> seenAO;
            aoAITab.reserve(aoNChans > 0 ? aoNChans : 0); ///< map of AO chan id to virtual AI chan id
            for (int i = 0; i < aoNChans; ++i) {
                if (p.aoPassthruMap.contains(i) && !seenAO.contains(i)) { 
                    aoAITab.push_back(QPair<int, int>(i,p.aoPassthruMap[i]));
                    seenAO.insert(i);
                    //build chanspec string for AI
                    aoChan.append(QString("%1%2").arg(aoChan.length() ? ", " : "").arg(aoChanStrings[i]));
                }
            }
        }
    }
	
	static inline int mapNewChanIdToPreJuly2011ChanId(int c, DAQ::Mode m, bool dualDevMode) {
		const int intan = c/DAQ::ModeNumChansPerIntan[m], chan = c % DAQ::ModeNumChansPerIntan[m];
		return chan*(DAQ::ModeNumIntans[m] * (dualDevMode ? 2 : 1)) + intan;
	}
    
    void Task::daqThr()
    {
        // Task parameters
        int32       error = 0;
        TaskHandle  taskHandle = 0, taskHandle2 = 0, aoTaskHandle = 0;
        char        errBuff[2048]={'\0'};
        const char *callStr = "";
        double      startTime;
        const Params & p (params);

        // Channel spec string for NI driver
        QString chan = "", chan2 = "", aoChan = "";
      	
        {
            const QVector<QString> aiChanStrings ((ProbeAllAIChannels()[p.dev]).toVector());
            //build chanspec string for aiChanStrings..
            for (QVector<unsigned>::const_iterator it = p.aiChannels.begin(); it != p.aiChannels.end(); ++it) 
			{
                chan.append(QString("%1%2").arg(chan.length() ? ", " : "").arg(aiChanStrings[*it]));
            }
            
        }
        if (p.dualDevMode) {
            const QVector<QString> aiChanStrings2 ((ProbeAllAIChannels()[p.dev2]).toVector());
            //build chanspec string for aiChanStrings..
            for (QVector<unsigned>::const_iterator it = p.aiChannels2.begin(); it != p.aiChannels2.end(); ++it) 
			{
                chan2.append(QString("%1%2").arg(chan2.length() ? ", " : "").arg(aiChanStrings2[*it]));
            }
            
        }
        
        const int nChans = p.aiChannels.size(), nChans2 = p.dualDevMode ? p.aiChannels2.size() : 0;
        const float64     min = p.range.min;
        const float64     max = p.range.max;
        const int nExtraChans1 = p.nExtraChans1, nExtraChans2 = p.dualDevMode ? p.nExtraChans2 : 0;
        

        // Params dependent on mode and DAQ::Params, etc
        const char *clockSource = p.extClock ? "PFI2" : "OnboardClock"; ///< TODO: make extClock possibly be something other than PFI2
        const char * const aoClockSource = "OnboardClock";
        float64 sampleRate = p.srate;
        const float64 aoSampleRate = p.srate;
        const float64     timeout = DAQ_TIMEOUT;
        const int NCHANS1 = p.nVAIChans1, NCHANS2 = p.dualDevMode ? p.nVAIChans2 : 0; 
        muxMode =  (p.mode != AIRegular);
        int nscans_per_mux_scan = 1;
        AOWriteThread * aoWriteThr = 0;

        if (muxMode) {
            const int mux_chans_per_phys = ModeNumChansPerIntan[p.mode];
            sampleRate *= double(mux_chans_per_phys);
            nscans_per_mux_scan = mux_chans_per_phys;
            /*if (!p.extClock) {
                /// Aieeeee!  Need ext clock for demux mode!
                QString e("Aieeeee!  Need to use an EXTERNAL clock for DEMUX mode!");
                Error() << e;
                emit daqError(e);
                return;
            }
			 */
        }

        QVector<QPair<int,int> > aoAITab;

        recomputeAOAITab(aoAITab, aoChan, p);
        
        const int task_read_freq_hz = computeTaskReadFreq(p.srate);
        
        int fudged_srate = ceil(sampleRate);
		while ((fudged_srate/task_read_freq_hz) % 2) // samples per chan needs to be a multiple of 2
			++fudged_srate;
        u64 bufferSize = u64(fudged_srate*nChans), bufferSize2 = u64(fudged_srate*nChans2);
		if (p.lowLatency)
			bufferSize /= (task_read_freq_hz); ///< 1/10th sec per read
		else 
			bufferSize *= double(p.aiBufferSizeCS) / 100.0; ///< otherwise just use user spec..
		if (p.dualDevMode) {
			if (p.lowLatency)
				bufferSize2 /= (task_read_freq_hz); ///< 1/10th sec per read
			else 
				bufferSize2 *= double(p.aiBufferSizeCS) / 100.0; ///< otherwise just use user spec..			
		}
		
        if (bufferSize < NCHANS1) bufferSize = NCHANS1;
		if (bufferSize2 < NCHANS2) bufferSize2 = NCHANS2;
/*        if (bufferSize * task_read_freq_hz != u64(fudged_srate*nChans)) // make sure buffersize is on scan boundary?
            bufferSize += task_read_freq_hz - u64(fudged_srate*nChans)%task_read_freq_hz; */
        if (bufferSize % nChans) // make sure buffer is on a scan boundary!
            bufferSize += nChans - (bufferSize%nChans);
        if (p.dualDevMode && bufferSize2 % nChans2) // make sure buffer is on a scan boundary!
            bufferSize2 += nChans2 - (bufferSize2%nChans2);
		
        //const u64 dmaBufSize = p.lowLatency ? u64(100000) : u64(1000000); /// 1000000 sample DMA buffer per chan?
        const u64 samplesPerChan = bufferSize/nChans, samplesPerChan2 = (nChans2 ? bufferSize2/nChans2 : 0);
        u64 aoBufferSize = 0; /* needs to be set below!!! */

        // Timing parameters
        int32       pointsRead=0, pointsRead2=0;
        const int32 pointsToRead = bufferSize, pointsToRead2 = bufferSize2;
        std::vector<int16> data, data2, leftOver, leftOver2, aoData;       

        QMap<unsigned, unsigned> saved_aoPassthruMap = p.aoPassthruMap;
		QString saved_aoDev = p.aoDev;
		Range saved_aoRange = p.aoRange;


		if (muxMode) {
			setDO(false);// set DO line low to reset external MUX
			msleep(1000); // keep it low for 1 second
		}

        DAQmxErrChk (DAQmxCreateTask("",&taskHandle)); 
        DAQmxErrChk (DAQmxCreateAIVoltageChan(taskHandle,chan.toUtf8().constData(),"",(int)p.aiTerm,min,max,DAQmx_Val_Volts,NULL)); 
        DAQmxErrChk (DAQmxCfgSampClkTiming(taskHandle,clockSource,sampleRate,DAQmx_Val_Rising,DAQmx_Val_ContSamps,bufferSize)); 
        //DAQmxErrChk (DAQmxCfgInputBuffer(taskHandle,dmaBufSize));  //use a 1,000,000 sample DMA buffer per channel
        //DAQmxErrChk (DAQmxRegisterEveryNSamplesEvent (taskHandle, DAQmx_Val_Acquired_Into_Buffer, everyNSamples, 0, DAQPvt::everyNSamples_func, this)); 
        if (p.dualDevMode) {
            DAQmxErrChk (DAQmxCreateTask((QString("")+QString::number(qrand())).toUtf8(),&taskHandle2)); 
			DAQmxErrChk (DAQmxCreateAIVoltageChan(taskHandle2,chan2.toUtf8().constData(),"",(int)p.aiTerm,min,max,DAQmx_Val_Volts,NULL)); 
			const char * clockSource2 = clockSource;//*/"PFI2";
//#ifdef DEBUG
//			clockSource2 = clockSource;
//#endif
			DAQmxErrChk (DAQmxCfgSampClkTiming(taskHandle2,clockSource2,sampleRate,DAQmx_Val_Rising,DAQmx_Val_ContSamps,bufferSize2)); 
			
		}
		
		const int task_write_freq_hz = p.lowLatency ? TASK_WRITE_FREQ_HZ*3 : TASK_WRITE_FREQ_HZ;

        if (p.aoPassthru && aoAITab.size()) {
            const float64     aoMin = p.aoRange.min;
            const float64     aoMax = p.aoRange.max;
            const int32 aoSamplesPerChan = /*aoSampleRate * (double(p.aiBufferSizeCS) / 100.0);//*/(aoSampleRate/TASK_WRITE_FREQ_HZ > 0) ? int(aoSampleRate/task_write_freq_hz) : 1;
            aoBufferSize = u64(aoSamplesPerChan) * aoAITab.size() * sizeof(int16);
            DAQmxErrChk (DAQmxCreateTask("",&aoTaskHandle));
            DAQmxErrChk (DAQmxCreateAOVoltageChan(aoTaskHandle,aoChan.toUtf8().constData(),"",aoMin,aoMax,DAQmx_Val_Volts,NULL));
            DAQmxErrChk (DAQmxCfgSampClkTiming(aoTaskHandle,aoClockSource,aoSampleRate,DAQmx_Val_Rising,DAQmx_Val_ContSamps,/*aoBufferSize*/aoSamplesPerChan/*0*/));
			//if (p.lowLatency) DAQmxCfgOutputBuffer(aoTaskHandle, dmaBufSize/10);
            //DAQmxErrChk (DAQmxCfgOutputBuffer(aoTaskHandle,aoSamplesPerChan));
            aoWriteThr = new AOWriteThread(0, aoTaskHandle, aoBufferSize, p);
            Connect(aoWriteThr, SIGNAL(daqError(const QString &)), this, SIGNAL(daqError(const QString &)));
            aoWriteThr->start();                
        }

        DAQmxErrChk (DAQmxStartTask(taskHandle)); 
		if (p.dualDevMode) { DAQmxErrChk (DAQmxStartTask(taskHandle2)); }

		if (muxMode) {
            setDO(true); // now set DO line high to start external MUX and clock on PFI2
		}

        startTime = getTime();
        u64 aoSampCount = 0;
        while( !pleaseStop ) {
            data.clear(); // should already be cleared, but enforce...
			data2.clear();
            if (leftOver.size()) data.swap(leftOver);            
			if (leftOver2.size()) data2.swap(leftOver2);
            unsigned long oldS = data.size(), oldS2 = data2.size();
            data.reserve(pointsToRead+oldS);
            data.resize(pointsToRead+oldS);

            DAQmxErrChk (DAQmxReadBinaryI16(taskHandle,samplesPerChan,timeout,DAQmx_Val_GroupByScanNumber,&data[oldS],pointsToRead,&pointsRead,NULL));
			if (p.dualDevMode) {
				data2.reserve(pointsToRead2+oldS2);
				data2.resize(pointsToRead2+oldS2);				
				DAQmxErrChk (DAQmxReadBinaryI16(taskHandle2,samplesPerChan2,timeout,DAQmx_Val_GroupByScanNumber,&data2[oldS2],pointsToRead2,&pointsRead2,NULL));
				// TODO FIXME XXX -- *use* this data.. for now we are just testing
				//Debug() << "Read " << pointsRead2 << " samples from second dev.\n";
			}
			
            u64 sampCount = totalRead;
            if (!sampCount) emit(gotFirstScan());
            int32 nRead = pointsRead * nChans + oldS, nRead2 = pointsRead2 * nChans2 + oldS2;                  
            int nDemuxScans = nRead/nChans/nscans_per_mux_scan, nDemuxScans2 = 1;
            if ( nDemuxScans*nscans_per_mux_scan*nChans != nRead ) {// not on 60 (or 75 if have interwoven PD and in PD mode) channel boundary, so save extra channels and enqueue them next time around..
                nRead = nDemuxScans*nscans_per_mux_scan*nChans;
                leftOver.insert(leftOver.end(), data.begin()+nRead, data.end());
                data.erase(data.begin()+nRead, data.end());
            }
			if (p.dualDevMode && nChans2) {
				nDemuxScans2 = nRead2 / nChans2 / nscans_per_mux_scan;
				if (nDemuxScans2 * nscans_per_mux_scan * nChans2 != nRead2) {
					nRead2 = nDemuxScans2*nscans_per_mux_scan*nChans2;
					leftOver2.insert(leftOver2.end(), data2.begin()+nRead2, data2.end());
					data2.erase(data2.begin()+nRead2, data2.end());					
				}
			} else
				nRead2 = 0;
			
			if (p.dualDevMode && nDemuxScans != nDemuxScans2) { // ensure same exact number of scans
				if (nDemuxScans > nDemuxScans2) {
					const int diff = nDemuxScans-nDemuxScans2;
					nRead = (nDemuxScans-diff)*nscans_per_mux_scan*nChans;
					leftOver.insert(leftOver.end(), data.begin()+nRead, data.end());
					data.erase(data.begin()+nRead, data.end());					
					nDemuxScans -= diff;					
				} else if (nDemuxScans2 > nDemuxScans) {
					const int diff = nDemuxScans2-nDemuxScans;
					nRead2 = (nDemuxScans2-diff)*nscans_per_mux_scan*nChans2;
					leftOver2.insert(leftOver2.end(), data2.begin()+nRead2, data2.end());
					data2.erase(data2.begin()+nRead2, data2.end());					
					nDemuxScans2 -= diff;
				}
			}
			
            // at this point we have scans of size 60 (or 75) channels (or 32 in JFRCIntan32)
            // in the 75-channel case we need to throw away 14 of those channels since they are interwoven PD-channels!
            data.resize(nRead);
			data2.resize(nRead2);
            if (!nRead) {
                Warning() << "Read less than a full scan from DAQ hardware!  FIXME on line:" << __LINE__ << " in " << __FILE__ << "!";
                continue; 
            }

            //Debug() << "Acquired " << nRead << " samples. Total " << totalRead;
            if (muxMode && (nExtraChans1 || nExtraChans2)) {
                /*
                  NB: at this point data contains scans of the form:

				
                  | 0 | 1 | 2 | 3 | Extra 1 | Extra 2 | PD | ...
                  | 4 | 5 | 6 | 7 | Extra 1 | Extra 2 | PD | ... 
                  | 56| 57| 58| 59| Extra 1 | Extra 2 | PD |
                  ---------------------------------------------------------------

                  Notice how the Extra channels are interwoven into the
                  0:3 (or 0:7 if in 120 channel mode) AI channels.

                  We need to remove them!

                  We want to turn that into 1 (or more) demuxed VIRTUAL scans
                  of either form:
				 
                  Pre-July 2011 ordering, which was INTAN_Channel major:

                  | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | ... | 58 | 59 | Extra 1 | Extra 2| PD |
                  -------------------------------------------------------------------------

                  Or, the current ordering, which is INTAN major:
				 
                  | 0 | 4 | 8 | 12 | 16 | 20 | 24 | 28 | ... | 55 | 59 | Extra 1 | Extra 2| PD |
                  ------------------------------------------------------------------------------
				 
                  Where we remove every `nChans' extraChans except for when 
                  we have (NCHANS-extraChans) samples, then we add the extraChans, 
                  effectively downsampling the extra channels from 444kHz to 
                  29.630 kHz.

                  Capisce?

                  NB: nChans = Number of physical channels per physical scan
                  NCHANS = Number of virtual channels per virtual scan

                */
                std::vector<int16> tmp, tmp2;
                const int datasz = data.size(), datasz2 = data2.size();
                tmp.reserve(datasz);
				tmp2.reserve(datasz2);
                const int nMx = nChans-nExtraChans1, nMx2 = nChans2-nExtraChans2;
                for (int i = nMx; nExtraChans1 && i <= datasz; i += nChans) {
                    std::vector<int16>::const_iterator begi = data.begin()+(i-nMx), endi = data.begin()+i;
                    tmp.insert(tmp.end(), begi, endi); // copy non-aux channels for this MUXed sub-scan
                    if ((!((tmp.size()+nExtraChans1) % NCHANS1)) && i+nExtraChans1 <= datasz) {// if we are on a virtual scan-boundary, then ...
                        begi = data.begin()+i;  endi = begi+nExtraChans1;
                        tmp.insert(tmp.end(), begi, endi); // .. we keep the extra channels
                    }
                }
                for (int i = nMx2; p.dualDevMode && nExtraChans2 && i <= datasz2; i += nChans2) {
                    std::vector<int16>::const_iterator begi = data2.begin()+(i-nMx2), endi = data2.begin()+i;
                    tmp2.insert(tmp2.end(), begi, endi); // copy non-aux channels for this MUXed sub-scan
                    if ((!((tmp2.size()+nExtraChans2) % NCHANS2)) && i+nExtraChans2 <= datasz2) {// if we are on a virtual scan-boundary, then ...
                        begi = data2.begin()+i;  endi = begi+nExtraChans2;
                        tmp2.insert(tmp2.end(), begi, endi); // .. we keep the extra channels
                    }
                }
                if (nExtraChans1) { data.swap(tmp); nRead = data.size(); }
				if (p.dualDevMode && nExtraChans2) { data2.swap(tmp2); nRead2 = data2.size(); }
                if ((data.size()+data2.size()) % (NCHANS1+NCHANS2)) {
                    // data didn't end on scan-boundary -- we have leftover scans!
                    Error() << "INTERNAL ERROR SCAN DIDN'T END ON A SCAN BOUNDARY FIXME!!! in " << __FILE__ << ":" << __LINE__;                 
                }
            }
            
			if (p.dualDevMode) {
				std::vector<int16> out;
				mergeDualDevData(out, data, data2, NCHANS1, NCHANS2, nExtraChans1, nExtraChans2);
				data.swap(out);
			}

			totalRead += nRead + nRead2;

			// note that from this point forward, the 'data' buffer is the only valid buffer
			// and it contains the MERGED data from both devices if in dual dev mode.
			
            // now, do optional AO output .. done in another thread to save on latency...
            if (aoWriteThr) {  
                {   // detect AO passthru changes and re-setup the AO task if ao passthru spec changed
                    QMutexLocker l(&p.mutex);
                    if (saved_aoPassthruMap != p.aoPassthruMap
                        || saved_aoRange != p.aoRange
                        || saved_aoDev != p.aoDev) {
                        aoData.clear();
                        delete aoWriteThr, aoWriteThr = 0;
                        recomputeAOAITab(aoAITab, aoChan, p);
                        DAQmxErrChk (DAQmxStopTask(aoTaskHandle));
                        DAQmxErrChk (DAQmxClearTask(aoTaskHandle));
                        DAQmxErrChk (DAQmxCreateTask("",&aoTaskHandle));
                        const float64     aoMin = p.aoRange.min;
                        const float64     aoMax = p.aoRange.max;
                        const int32 aoSamplesPerChan = aoSampleRate * (double(p.aiBufferSizeCS) / 100.0); //aoSampleRate/task_write_freq_hz > 0 ? int(aoSampleRate/task_write_freq_hz) : 1;
                        aoBufferSize = u64(aoSamplesPerChan) * aoAITab.size() * sizeof(int16);
                        DAQmxErrChk (DAQmxCreateAOVoltageChan(aoTaskHandle,aoChan.toUtf8().constData(),"",aoMin,aoMax,DAQmx_Val_Volts,NULL));
                        DAQmxErrChk (DAQmxCfgSampClkTiming(aoTaskHandle,aoClockSource,aoSampleRate,DAQmx_Val_Rising,DAQmx_Val_ContSamps,/*aoBufferSize*/aoSamplesPerChan/*0*/));
                        //DAQmxErrChk (DAQmxCfgOutputBuffer(aoTaskHandle,aoSamplesPerChan));     
                        saved_aoPassthruMap = p.aoPassthruMap;
                        saved_aoRange = p.aoRange;
                        saved_aoDev = p.aoDev;
                        aoWriteThr = new AOWriteThread(0, aoTaskHandle, aoBufferSize, p);
                        Connect(aoWriteThr, SIGNAL(daqError(const QString &)), this, SIGNAL(daqError(const QString &)));
                        aoWriteThr->start();
                    }
                }
                const int dsize = data.size();
                aoData.reserve(aoData.size()+dsize);
                for (int i = 0; i < dsize; i += NCHANS1+NCHANS2) { // for each scan..
                    for (QVector<QPair<int,int> >::const_iterator it = aoAITab.begin(); it != aoAITab.end(); ++it) { // take ao channels
                        const int aiChIdx = p.doPreJuly2011IntanDemux || !muxMode ? (*it).second : mapNewChanIdToPreJuly2011ChanId((*it).second, p.mode, p.dualDevMode);
						const int dix = i+aiChIdx;
						if (dix < dsize)
							aoData.push_back(data[dix]);
						else {
							static int errct = 0;
							aoData.push_back(0);
							if (errct++ < 5)
								Error() << "INTERNAL ERROR: This shouldn't happen.  AO passthru code is buggy. FIX ME!!";
						}
                    }
                }
                if (aoData.size() >= aoBufferSize) { 
                    u64 sz = aoData.size();
                    aoWriteThr->enqueueBuffer(aoData, aoSampCount);
                    aoSampCount += sz;
                }
            }
             
			
			breakupDataIntoChunksAndEnqueue(data, sampCount);

            // fast settle...
            if (muxMode && fast_settle && !leftOver.size() && !leftOver2.size()) {
                double t0 = getTime();
                /// now possibly do a 'fast settle' request by stopping the task, setting the DO line low, then restarting the task after fast_settle ms have elapsed, and setting the line high
                Debug() << "Fast settle of " << fast_settle << " ms begin";
                DAQmxErrChk(DAQmxStopTask(taskHandle));
				if (p.dualDevMode) DAQmxErrChk(DAQmxStopTask(taskHandle2));
                if (aoTaskHandle) {
                    delete aoWriteThr, aoWriteThr = 0; 
                    DAQmxErrChk(DAQmxStopTask(aoTaskHandle));
                }
                setDO(false);
                msleep(fast_settle);
                fast_settle = 0;
                DAQmxErrChk(DAQmxStartTask(taskHandle));                
                if (p.dualDevMode) DAQmxErrChk(DAQmxStartTask(taskHandle2));    
                aoWriteThr = new AOWriteThread(0, aoTaskHandle, aoBufferSize, p);
                Connect(aoWriteThr, SIGNAL(daqError(const QString &)), this, SIGNAL(daqError(const QString &)));
                aoWriteThr->start();
                // no need to restart AO as it is autostart..
                setDO(true);
                Debug() << "Fast settle completed in " << (getTime()-t0) << " secs";
                emit(fastSettleCompleted());
            }
        }

        Debug() << "Acquired " << totalRead << " total samples.";

    Error_Out:
        if ( DAQmxFailed(error) )
            DAQmxGetExtendedErrorInfo(errBuff,2048);
        if ( taskHandle != 0) {
            DAQmxStopTask (taskHandle);
            DAQmxClearTask (taskHandle);
        }
        if ( taskHandle2 != 0) {
            DAQmxStopTask (taskHandle2);
            DAQmxClearTask (taskHandle2);
        }
        if (aoWriteThr != 0) {
            delete aoWriteThr, aoWriteThr = 0;
        }
        if ( aoTaskHandle != 0) {
            DAQmxStopTask (aoTaskHandle);
            DAQmxClearTask (aoTaskHandle);
        }
        if( DAQmxFailed(error) ) {
            QString e;
            e.sprintf("DAQmx Error: %s\nDAQMxBase Call: %s",errBuff, callStr);
            if (!noDaqErrPrint) {
                Error() << e;
            }
            emit daqError(e);
        }
        
    }
	
	/*static*/
	inline void Task::mergeDualDevData(std::vector<int16> & out,
								const std::vector<int16> & data, const std::vector<int16> & data2, 
								int NCHANS1, int NCHANS2, 
								int nExtraChans1, int nExtraChans2)
	{
		const int nMx = NCHANS1-nExtraChans1, nMx2 = NCHANS2-nExtraChans2, s1 = data.size(), s2 = data2.size();
		out.clear();
		out.reserve(s1+s2);
		int i,j;
		for (i = 0, j = 0; i < s1 && j < s2; i+=NCHANS1, j+=NCHANS2) {
			out.insert(out.end(), data.begin()+i, data.begin()+i+nMx);
			out.insert(out.end(), data2.begin()+j, data2.begin()+j+nMx2);
			out.insert(out.end(), data.begin()+i+nMx, data.begin()+i+NCHANS1);
			out.insert(out.end(), data2.begin()+j+nMx2, data2.begin()+j+NCHANS2);
		}
		if (i < s1 || j < s2) 
			Error() << "INTERNAL ERROR IN FUNCTION `mergeDualDevData()'!  The two device buffers data and data2 have differing numbers of scans! FIXME!  Aieeeee!!\n";
	}	

	void Task::breakupDataIntoChunksAndEnqueue(std::vector<int16> & data, u64 sampCount)
	{
		int chunkSize = int(params.srate / computeTaskReadFreq(params.srate)) * params.nVAIChans, nchunk = 0, ct = 0;
		if (chunkSize < (int)params.nVAIChans) chunkSize = params.nVAIChans;
        static const int limit = (1024*786)/sizeof(int16); /* 768KB max chunk size? */
        if (chunkSize > limit) { 
            chunkSize = limit - (limit % params.nVAIChans);
        }
		const int nSamps = data.size();
		if (chunkSize >= nSamps) {
			enqueueBuffer(data, sampCount);
			return;
		}
		std::vector<int16> chunk;
		for (int i = 0; i < nSamps; i += nchunk, ++ct) {
			chunk.reserve(chunkSize);
			nchunk = nSamps - i;
			if (nchunk > chunkSize) nchunk = chunkSize;
			chunk.insert(chunk.begin(), data.begin()+i, data.begin()+i+nchunk);
			enqueueBuffer(chunk, sampCount+u64(i));
			chunk.clear();
		}
		//Debug() << "broke up data of size " << data.size() << " into " << ct << " chunks of size " << chunkSize;
		data.clear();
	}

    void Task::setDO(bool onoff)
    {
        const char *callStr = "";

        // Task parameters
        int      error = 0;
        TaskHandle  taskHandle = 0;
        char        errBuff[2048];
        QString tmp;

        // Channel parameters
        const QString & chan (doCtlChan());
        
        // Write parameters
        uint32      w_data [1];

        // Create Digital Output (DO) Task and Channel
        DAQmxErrChk (DAQmxCreateTask ("", &taskHandle));
        DAQmxErrChk (DAQmxCreateDOChan(taskHandle,chan.toUtf8().constData(),"",DAQmx_Val_ChanPerLine));

        // Start Task (configure port)
        //DAQmxErrChk (DAQmxStartTask (taskHandle));

        //  Only 1 sample per channel supported for static DIO
        //  Autostart ON

        if (!onoff) 
            w_data[0] = 0x0;
        else 
            w_data[0] = 0x1;

        tmp.sprintf("Writing to DO: %s data: 0x%X", chan.toUtf8().constData(),(unsigned int)w_data[0]);
        Debug() << tmp;

        DAQmxErrChk (DAQmxWriteDigitalScalarU32(taskHandle,1,DAQ_TIMEOUT,w_data[0],NULL));


    Error_Out:

        if (DAQmxFailed (error))
            DAQmxGetExtendedErrorInfo (errBuff, 2048);

        if (taskHandle != 0)
            {
                DAQmxStopTask (taskHandle);
                DAQmxClearTask (taskHandle);
            }

        if (error) {
            QString e;
            e.sprintf("DAQmx Error %d: %s", error, errBuff);
            if (!noDaqErrPrint) 
                Error() << e;
            emit daqError(e);
        }
    }

    void Task::requestFastSettle() 
    {
        if (!muxMode) {
            Warning() << "Fast settle requested -- but not running in MUX mode!  FIXME!";
            return;
        }
        unsigned ms = params.fastSettleTimeMS;
        if (!fast_settle) {
            if (ms > 10000) { ///< hard limit on ms
                Warning() << "Requested fast settled of " << ms << " ms, limiting to 10000 ms";
                ms = 10000;
            }
            fast_settle = ms;
        } else {
            Warning() << "Dupe fast settle requested -- fast settle already running!";
        }
    }

    int32 DAQPvt::everyNSamples_func (TaskHandle taskHandle, int32 everyNsamplesEventType, uint32 nSamples, void *callbackData)
    {
        Task *daq = (Task *)callbackData;
        (void)daq;
        // todo: read data here
        (void)taskHandle; (void)everyNsamplesEventType; (void) nSamples;
        return 0;
    }

#endif // ! FAKEDAQ


    /// some helper funcs from SpikeGL.h
    static const QString acqModes[] = { "AI60Demux", "AIRegular", "AI120Demux", "JFRCIntan32", "AI128Demux", "AI256Demux", QString::null };
    
    const QString & ModeToString(Mode m)
    {
        return acqModes[(int)m];
    }
    
    Mode StringToMode(const QString &str)
    {
        int i;
        for (i = 0; i < (int)AIUnknown; ++i) {
            if (str.trimmed().compare(acqModes[i], Qt::CaseInsensitive) == 0) {
                break;
            }
        }
        return (Mode)i;
    }

    TermConfig StringToTermConfig(const QString & txt) 
    {
        if (!txt.compare("RSE", Qt::CaseInsensitive))
            return RSE;
        else if (!txt.compare("NRSE", Qt::CaseInsensitive))
            return NRSE;
        else if (!txt.compare("Differential", Qt::CaseInsensitive) 
                 || !txt.compare("Diff", Qt::CaseInsensitive) )
            return Diff;
        else if (!txt.compare("PseudoDifferential", Qt::CaseInsensitive) 
                 || !txt.compare("PseudoDiff", Qt::CaseInsensitive) )
            return PseudoDiff;
        return Default;       
    }

    QString TermConfigToString(TermConfig t)
    {
        switch(t) {
        case RSE: return "RSE";
        case NRSE: return "NRSE";
        case Diff: return "Differential";
        case PseudoDiff: return "PseudoDifferential";
        default: break;
        }
        return "Default";
    }


    static const QString acqStartEndModes[] = { "Immediate", "PDStartEnd", "PDStart", "Timed", "StimGLStartEnd", "StimGLStart", "AITriggered", QString::null };
    
    const QString & AcqStartEndModeToString(AcqStartEndMode m) {
        if (m >= 0 && m < N_AcqStartEndModes) 
            return acqStartEndModes[(int)m];
        static QString unk ("Unknown");
        return unk;
    }

} // end namespace DAQ

//-- #pragma mark Windows Hacks

///-- below is a bunch of ugly hacks for windows only to not have this .EXE depend on NI .DLLs!  

#if defined(Q_OS_WIN) && defined(HAVE_NIDAQmx)
#include <windows.h>

namespace DAQ {
	static HMODULE module = 0;
	
	bool Available(void) {
		static bool tried = false;
		//bool hadNoModule = !module;
		if (!module && !tried && !(module = LoadLibraryA("nicaiu.dll")) ) {
			//Log() << "Could not find nicaiu.dll, DAQ features disabled!";
			tried = true;
			return false;
		} else if (tried) return false;
		//if (hadNoModule)
		//	Log() << "Found and dynamically loaded NI Driver DLL: nicaiu.dll, DAQ features enabled";
		return true;
	}
	
	template <typename T> void tryLoadFunc(T * & func, const char *funcname) {
		if (!func && Available()) {
			func = reinterpret_cast<T *> (GetProcAddress( module, funcname ));
			if (!func) {
				//Warning() << "Could not find the function " << funcname << " in nicaiu.dll, NI functionality may fail!";
				return;				
			}
		}
	}
}

extern "C" {	
	//*** Set/Get functions for DAQmx_Dev_DO_Lines ***
	int32 __CFUNC DAQmxGetDevDOLines(const char device[], char *data, uInt32 bufferSize) {
		static int32 (__CFUNC *func)(const char *, char *, uInt32) = 0;
		DAQ::tryLoadFunc(func, "DAQmxGetDevDOLines");
		//Debug() << "DAQmxGetDevDOLines called";
		if (func) return func(device, data, bufferSize);
		return DAQmxErrorRequiredDependencyNotFound;
	}
	
	int32 __CFUNC DAQmxWriteDigitalScalarU32   (TaskHandle taskHandle, bool32 autoStart, float64 timeout, uInt32 value, bool32 *reserved) {
		static int32 (__CFUNC *func) (TaskHandle, bool32, float64, uInt32, bool32 *) = 0;
		DAQ::tryLoadFunc(func, "DAQmxWriteDigitalScalarU32");
		//Debug() << "DAQmxWriteDigitalScalarU32 called";
		if (func) return func(taskHandle, autoStart, timeout, value, reserved);
		return DAQmxErrorRequiredDependencyNotFound;
	}
	
	int32 __CFUNC  DAQmxStartTask (TaskHandle taskHandle) {
		static int32 (__CFUNC  *func) (TaskHandle) = 0;
		DAQ::tryLoadFunc(func, "DAQmxStartTask");
		Debug() << "DAQmxStartTask called";
		if (func) return func(taskHandle);
		return DAQmxErrorRequiredDependencyNotFound;
	}	
	
	int32 __CFUNC  DAQmxStopTask (TaskHandle taskHandle) {
		static int32 (__CFUNC  *func) (TaskHandle) = 0;
		DAQ::tryLoadFunc(func, "DAQmxStopTask");
		Debug() << "DAQmxStopTask called";
		if (func) return func(taskHandle);
		return DAQmxErrorRequiredDependencyNotFound;
	}
	
	int32 __CFUNC  DAQmxClearTask (TaskHandle taskHandle) {
		static int32 (__CFUNC  *func) (TaskHandle) = 0;
		DAQ::tryLoadFunc(func, "DAQmxClearTask");
		//Debug() << "DAQmxClearTask called";
		if (func) return func(taskHandle);
		return DAQmxErrorRequiredDependencyNotFound;
	}
	
	int32 __CFUNC DAQmxCreateDOChan (TaskHandle taskHandle, const char lines[], const char nameToAssignToLines[], int32 lineGrouping) {
		static int32 (__CFUNC *func)(TaskHandle, const char *, const char *, int32 lineGrouping) = 0;
		DAQ::tryLoadFunc(func, "DAQmxCreateDOChan");
		//Debug() << "DAQmxCreateDOChan called";
		if (func) return func(taskHandle,lines,nameToAssignToLines,lineGrouping);
		return DAQmxErrorRequiredDependencyNotFound;
	}
	
	int32 __CFUNC     DAQmxGetExtendedErrorInfo (char errorString[], uInt32 bufferSize) {
		static int32 (__CFUNC *func) (char *, uInt32) = 0;
		DAQ::tryLoadFunc(func, "DAQmxGetExtendedErrorInfo");
		//Debug() << "DAQmxGetExtendedErrorInfo called";
		if (func) return func(errorString,bufferSize);
		strncpy(errorString, "DLL Missing", bufferSize);
		return DAQmxSuccess;		
	}
	
	int32 __CFUNC     DAQmxCreateTask          (const char taskName[], TaskHandle *taskHandle) {
		static int32 (__CFUNC *func) (const char *, TaskHandle *) = 0;
		static const char * const fname = "DAQmxCreateTask";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(taskName,taskHandle);
		return DAQmxErrorRequiredDependencyNotFound;				
	}
	
	int32 __CFUNC DAQmxGetDevAIMaxMultiChanRate(const char device[], float64 *data) {
		static int32 (__CFUNC *func)(const char *, float64 *) = 0;
		static const char * const fname = "DAQmxGetDevAIMaxMultiChanRate";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(device,data);
		return DAQmxErrorRequiredDependencyNotFound;				
	}
	
	int32 __CFUNC DAQmxGetDevAISimultaneousSamplingSupported(const char device[], bool32 *data) {
		static int32 (__CFUNC *func)(const char *, bool32 *) 	 = 0;
		const char *fname = "DAQmxGetDevAISimultaneousSamplingSupported";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(device,data);
		return DAQmxErrorRequiredDependencyNotFound;						
	}
	
	int32 __CFUNC DAQmxGetDevAIMaxSingleChanRate(const char device[], float64 *data) {
		static int32 (__CFUNC *func)(const char *, float64 *) 	 = 0;
		const char *fname = "DAQmxGetDevAIMaxSingleChanRate";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(device,data);
		return DAQmxErrorRequiredDependencyNotFound;						
	}
	
	int32 __CFUNC DAQmxGetDevAIPhysicalChans(const char device[], char *data, uInt32 bufferSize) {
		static int32 (__CFUNC *func)(const char *, char *, uInt32) 	 = 0;
		const char *fname = "DAQmxGetDevAIPhysicalChans";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(device,data,bufferSize);
		return DAQmxErrorRequiredDependencyNotFound;								
	}
	
	int32 __CFUNC DAQmxGetDevAOVoltageRngs(const char device[], float64 *data, uInt32 arraySizeInSamples) {
		static int32 (__CFUNC *func)(const char *, float64 *, uInt32) 	 = 0;
		const char *fname = "DAQmxGetDevAOVoltageRngs";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(device,data,arraySizeInSamples);
		return DAQmxErrorRequiredDependencyNotFound;										
	}
	
	int32 __CFUNC DAQmxGetDevAIVoltageRngs(const char device[], float64 *data, uInt32 arraySizeInSamples) {
		static int32 (__CFUNC *func)(const char *, float64 *, uInt32) 	 = 0;
		const char *fname = "DAQmxGetDevAIVoltageRngs";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(device,data,arraySizeInSamples);
		return DAQmxErrorRequiredDependencyNotFound;												
	}
	
	int32 __CFUNC DAQmxGetDevAIMinRate(const char device[], float64 *data) {
		static int32 (__CFUNC *func)(const char *, float64 *) 	 = 0;
		const char *fname = "DAQmxGetDevAIMinRate";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(device,data);
		return DAQmxErrorRequiredDependencyNotFound;														
	}
	
	int32 __CFUNC DAQmxGetDevAOPhysicalChans(const char device[], char *data, uInt32 bufferSize) {
		static int32 (__CFUNC *func)(const char *, char  *, uInt32) 	 = 0;
		const char *fname = "DAQmxGetDevAOPhysicalChans";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(device,data,bufferSize);
		return DAQmxErrorRequiredDependencyNotFound;																
	}
	
	int32 __CFUNC DAQmxGetDevProductType(const char device[], char *data, uInt32 bufferSize) {
		static int32 (__CFUNC *func)(const char *, char  *, uInt32) 	 = 0;
		const char *fname = "DAQmxGetDevProductType";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(device,data,bufferSize);
		return DAQmxErrorRequiredDependencyNotFound;																		
	}
	
	int32 __CFUNC DAQmxCfgInputBuffer(TaskHandle taskHandle, uInt32 numSampsPerChan) {
		static int32 (__CFUNC *func)(TaskHandle, uInt32) 	 = 0;
		const char *fname = "DAQmxCfgInputBuffer";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(taskHandle,numSampsPerChan);
		return DAQmxErrorRequiredDependencyNotFound;		
	}
	
	int32 __CFUNC DAQmxCfgOutputBuffer(TaskHandle taskHandle, uInt32 numSampsPerChan) {
	static int32 (__CFUNC *func)(TaskHandle, uInt32) 	 = 0;
	const char *fname = "DAQmxCfgOutputBuffer";
	DAQ::tryLoadFunc(func, fname);
	//Debug() << fname << " called";
	if (func) return func(taskHandle,numSampsPerChan);
		return DAQmxErrorRequiredDependencyNotFound;		
	}
	
	int32 __CFUNC DAQmxCfgSampClkTiming(TaskHandle taskHandle, const char source[], float64 rate, int32 activeEdge, int32 sampleMode, uInt64 sampsPerChan) {
		static int32 (__CFUNC *func)(TaskHandle, const char *, float64, int32, int32, uInt64) 	 = 0;
		const char *fname = "DAQmxCfgSampClkTiming";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(taskHandle,source,rate,activeEdge,sampleMode,sampsPerChan);
		return DAQmxErrorRequiredDependencyNotFound;				
	}

	int32 __CFUNC DAQmxCreateAIVoltageChan(TaskHandle taskHandle, const char physicalChannel[], const char nameToAssignToChannel[], int32 terminalConfig, float64 minVal, float64 maxVal, int32 units, const char customScaleName[]) {
		static int32 (__CFUNC *func)(TaskHandle, const char *, const char *, int32, float64, float64, int32, const char *) 	 = 0;
		const char *fname = "DAQmxCreateAIVoltageChan";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(taskHandle,physicalChannel,nameToAssignToChannel,terminalConfig,minVal,maxVal,units,customScaleName);
		return DAQmxErrorRequiredDependencyNotFound;				
	}		

	int32 __CFUNC DAQmxCreateAOVoltageChan(TaskHandle taskHandle, const char physicalChannel[], const char nameToAssignToChannel[], float64 minVal, float64 maxVal, int32 units, const char customScaleName[]) {
		static int32 (__CFUNC *func)(TaskHandle, const char *, const char *, float64, float64, int32, const char *) 	 = 0;
		const char *fname = "DAQmxCreateAOVoltageChan";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(taskHandle,physicalChannel,nameToAssignToChannel,minVal,maxVal,units,customScaleName);
		return DAQmxErrorRequiredDependencyNotFound;				
	}		

	int32 __CFUNC DAQmxReadBinaryI16(TaskHandle taskHandle, int32 numSampsPerChan, float64 timeout, bool32 fillMode, int16 readArray[], uInt32 arraySizeInSamps, int32 *sampsPerChanRead, bool32 *reserved) {
		static int32 (__CFUNC *func)(TaskHandle, int32, float64, bool32, int16 *, uInt32, int32 *, bool32 *) 	 = 0;
		const char *fname = "DAQmxReadBinaryI16";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(taskHandle,numSampsPerChan,timeout,fillMode,readArray,arraySizeInSamps,sampsPerChanRead,reserved);
		return DAQmxErrorRequiredDependencyNotFound;				
	}

	int32 __CFUNC DAQmxWriteBinaryI16(TaskHandle taskHandle, int32 numSampsPerChan, bool32 autoStart, float64 timeout, bool32 dataLayout, const int16 writeArray[], int32 *sampsPerChanWritten, bool32 *reserved) {
		static int32 (__CFUNC *func)(TaskHandle, int32, bool32, float64, bool32, const int16 *, int32 *, bool32 *) 	 = 0;
		const char *fname = "DAQmxWriteBinaryI16";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(taskHandle,numSampsPerChan,autoStart,timeout,dataLayout,writeArray,sampsPerChanWritten,reserved);
		return DAQmxErrorRequiredDependencyNotFound;				
	}
	
	int32 __CFUNC DAQmxRegisterEveryNSamplesEvent(TaskHandle taskHandle, int32 everyNsamplesEventType, uInt32 nSamples, uInt32 options, DAQmxEveryNSamplesEventCallbackPtr callbackFunction, void *callbackData) {
		static int32 (__CFUNC *func)(TaskHandle, int32, uInt32, uInt32, DAQmxEveryNSamplesEventCallbackPtr, void *) 	 = 0;
		const char *fname = "DAQmxRegisterEveryNSamplesEvent";
		DAQ::tryLoadFunc(func, fname);
		//Debug() << fname << " called";
		if (func) return func(taskHandle,everyNsamplesEventType,nSamples,options,callbackFunction,callbackData);
		return DAQmxErrorRequiredDependencyNotFound;		
	}


	/* Implementation Checklist for DLL decoupling:
			 √ DAQmxCfgInputBuffer
			 √ DAQmxCfgOutputBuffer
			 √ DAQmxCfgSampClkTiming
			 √ DAQmxClearTask
			 √ DAQmxCreateAIVoltageChan
			 √ DAQmxCreateAOVoltageChan
			 √ DAQmxCreateDOChan
			 √ DAQmxCreateTask
			 √ DAQmxErrChk
			 √ DAQmxErrorRequiredDependencyNotFound
			 √ DAQmxFailed
			 √ DAQmxGetDevAIMaxMultiChanRate
			 √ DAQmxGetDevAIMaxSingleChanRate
			 √ DAQmxGetDevAIMinRate
			 √ DAQmxGetDevAIPhysicalChans
			 √ DAQmxGetDevAISimultaneousSamplingSupported
			 √ DAQmxGetDevAIVoltageRngs
			 √ DAQmxGetDevAOPhysicalChans
			 √ DAQmxGetDevAOVoltageRngs
			 √ DAQmxGetDevDOLines
			 √ DAQmxGetDevProductType
			 √ DAQmxGetExtendedErrorInfo
			 √ DAQmxReadBinaryI16
			 √ DAQmxRegisterEveryNSamplesEvent
			 √ DAQmxStartTask
			 √ DAQmxStopTask
			 √ DAQmxSuccess
			 √ DAQmxWriteBinaryI16
			 √ DAQmxWriteDigitalScalarU32
		*/
	
}
#else
namespace DAQ {
	bool Available() { return true; /* emulated, but available! */ }
}
#endif

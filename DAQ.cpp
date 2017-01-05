#include "SpikeGL.h"
#include "DAQ.h"
#ifdef HAVE_NIDAQmx
#  include "NI/NIDAQmx.h"
#  include "AOWriteThread.h"
#else
#  ifndef FAKEDAQ
#    define FAKEDAQ
#  endif
#  warning Not a real NI platform.  All acquisition related functions are emulated!
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
#include <QProcess>
#include <QProcessEnvironment>
#include <math.h>
#include "SampleBufQ.h"
#include "MainApp.h"
#include "FrameGrabber/FG_SpikeGL/FG_SpikeGL/XtCmd.h"

#define DAQmxErrChk(functionCall) do { if( DAQmxFailed(error=(functionCall)) ) { callStr = STR(functionCall); goto Error_Out; } } while (0)
#define DAQmxErrChkNoJump(functionCall) ( DAQmxFailed(error=(functionCall)) && (callStr = STR(functionCall)) )

bool excessiveDebug = false; /* from SpikeGL.h */

namespace DAQ 
{
    static bool noDaqErrPrint = false;

    const unsigned ModeNumChansPerIntan[N_Modes] = {
        15, 0, 15, 16, 16, 32, 16, 16, 32
    };
    
    const unsigned ModeNumIntans[N_Modes] = {
        4, 0, 8, 2, 8, 8, 4, 6, 4
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
        r.min = -2.5;
        r.max = 2.5;
        ret.insert("Dev2", r);
        r.min = -5.;
        r.max = 5.;
        ret.insert("Dev2", r);
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
        r.min = -2.5;
        r.max = 2.5;
        ret.insert("Dev2", r);
        r.min = -5.;
        r.max = 5.;
        ret.insert("Dev2", r);
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
        if (devname == "Dev2") { // new "massive-channel" dev 2, for testing..
            for (int i = 0; i < 4096; ++i) {
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
        if (devname == "Dev1" || devname == "Dev2") {
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
		if (dev.compare("USB_Bug3", Qt::CaseInsensitive)==0) return "Intan USB Telemetry Receiver (Bug3)";
		if (dev.compare("Framegrabber", Qt::CaseInsensitive)==0) return "XtiumCL PX4 + Camerialink to Intan";
#ifdef HAVE_NIDAQmx
        char buf[65536] = "Unknown";
        if (DAQmxFailed(DAQmxGetDevProductType(dev.toUtf8().constData(), buf, sizeof(buf)))) {
            Error() << "Failed to query product name for dev " << dev << ".";
        } 
        // else..
        return buf;
#else
        (void)dev;
        return dev == "Dev2" ? "FakeDAQMassiveTest" : "FakeDAQ";
#endif
    }
    
    NITask::NITask(const Params & acqParams, QObject *p, const PagedScanReader &psr, const QString & taskDescriptiveName)
        : Task(p,taskDescriptiveName, psr), pleaseStop(false), params(acqParams),
          fast_settle(0), muxMode(false), aoWriteThr(0),
#ifdef HAVE_NIDAQmx
          taskHandle(0), taskHandle2(0),
#endif
          clockSource(0), error(0), callStr(0)
    {
        errBuff[0] = 0;
        setDO(false); // assert DO is low when stopped...
    }
	
    NITask::~NITask() {
        stop();
        if (numChans() > 0)
            Debug() << "NITask `" << objectName() << "' deleted after processing " << totalRead/u64(numChans()) << " scans.";
    }


    void NITask::stop() 
    {
        if (isRunning() && !pleaseStop) {
            pleaseStop = true;
            wait();
            pleaseStop = false;
        }
        setDO(false); // assert DO low when stopped...
    }

    void NITask::overflowWarning() 
    {
        Warning() << "DAQ Task sample buffer overflow!  Dropping a buffer!";
        emit(bufferOverrun());
#ifdef FAKEDAQ
        static int overflowct = 0;
    
        if (++overflowct == 5) {
            emit(taskError("Overflow limit exceeded"));
        }
#endif
    }

    /* static */
    int NITask::computeTaskReadFreq(double srate_in) {
        int srate = ceil(srate_in); (void)srate;
        return DEF_TASK_READ_FREQ_HZ;
        //if (ll) return DEF_TASK_READ_FREQ_HZ_ * 3;
        //return DEF_TASK_READ_FREQ_HZ_;
    }
    
#ifdef FAKEDAQ
}// end namespace DAQ

#include <stdlib.h>

namespace DAQ 
{
    void NITask::daqThr()
    {
        static QString fname(":/fakedaq/fakedaqdata.bin");
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
            emit taskError(err);
            return;
        }
        std::vector<int16> data;
        const double onePd = writer.scansPerPage()/params.srate;
        while (!pleaseStop) {
			double ts = getTime();
            data.resize(unsigned(params.nVAIChans*writer.scansPerPage()));
            qint64 nread = f.read((char *)&data[0], data.size()*sizeof(int16));
            if (nread != qint64(data.size()*sizeof(int16))) {
                f.seek(0);
            } else if (nread > 0) {
                nread /= sizeof(int16);
                data.resize(nread);
                if (!totalRead) emit(gotFirstScan());

                doFinalDemuxAndEnqueue(data);
      
                totalReadMut.lock();
                totalRead += nread;
                totalReadMut.unlock();
            }
            int sleeptime = int(onePd*1e6) - int((getTime()-ts)*1e6);
			if (sleeptime > 0) 
				usleep(sleeptime);
			else
				Warning() << "FakeDAQ NITask::daqThr(). sleeptime (" << sleeptime << ") is less than 0!";
        }
    }

    void NITask::setDO(bool onoff)
    {
        Warning() << "setDO(" << (onoff ? "on" : "off") << ") called (unimplemented in FAKEDAQ mode)";
    }


    void NITask::requestFastSettle() 
    {
        Warning() << "requestFastSettle() unimplemented for FAKEDAQ mode!";
        emit(fastSettleCompleted());
    }

#else // !FAKEDAQ

    void AOWriteThread::run()
    {
        if (old2Delete) {
            delete old2Delete;
            old2Delete = 0;
        }
        const Params & p(params);
        TaskHandle taskHandle(0);
        int32       error = 0;
        char        errBuff[2048]={'\0'};
        const char *callStr = "";
        unsigned bufferSizeCS = p.aoBufferSizeCS;
        if (p.aoBufferSizeCS < p.aiBufferSizeCS) {
            Warning() << "AOWrite thread AO buf=" << (p.aoBufferSizeCS*10.) << " is less than AI buf=" << (p.aiBufferSizeCS*10.) << ", this is unsupported.  Forcing AO buffer size to " << (p.aiBufferSizeCS*10.);
            bufferSizeCS = p.aiBufferSizeCS;
        }
        int32 aoSamplesPerChan( ceil(ceil(p.aoSrate) * ((bufferSizeCS/100.0)+0.005/*<--ugly fudge to make sure AO buffer size is always 5ms bigger than AI! Argh!*/)) );
        while (aoSamplesPerChan % 2)
            ++aoSamplesPerChan; // force aoSamplesPerChan to be multiple of 2? I forget why!!!!
        const int32 aoChansSize = p.aoChannels.size();
        unsigned aoBufferSize(aoSamplesPerChan * aoChansSize);
        const float64     aoMin = p.aoRange.min;
        const float64     aoMax = p.aoRange.max;
        const float64     aoTimeout = DAQ_TIMEOUT*2.;
        unsigned aoWriteCt = 0;
        pleaseStop = false;
        //std::vector<int16> leftOver;
        // XXX diags..
        int nWritesAvg = 0, nWritesAvgMax = 10;
        double writeSpeedAvg = 0.;
        double tLastData = -1.0, inpDataRate=0., outDataRate=0.;
        std::vector<int16> sampsOrig, samps;
        Util::Resampler resampler(double(p.aoSrate) / double(p.srate) /* HACK FIXME * 2.0*/, aoChansSize);
        int nodatact = 0;
        int daqerrct = 0;

        while (daqerrct < 3 && !pleaseStop) {
            bool break2 = false;
            DAQmxErrChk (DAQmxCreateTask("",&taskHandle));
            DAQmxErrChk (DAQmxCreateAOVoltageChan(taskHandle,aoChanString.toUtf8().constData(),"",aoMin,aoMax,DAQmx_Val_Volts,NULL));
            DAQmxErrChk (DAQmxCfgSampClkTiming(taskHandle,p.aoClock.toUtf8().constData(),p.aoSrate,DAQmx_Val_Rising,DAQmx_Val_ContSamps,/*aoBufferSize*/aoSamplesPerChan/*0*/));
            DAQmxErrChk (DAQmxCfgOutputBuffer(taskHandle,aoSamplesPerChan));
            DAQmxSetWriteRegenMode(taskHandle, DAQmx_Val_DoNotAllowRegen);
			DAQmxSetImplicitUnderflowBehavior(taskHandle, DAQmx_Val_PauseUntilDataAvailable);

            Debug() << "AOWrite thread started.";

            while (!pleaseStop && !break2) {
                u64 sampCount;
                if (!dequeueBuffer(sampsOrig, sampCount) || !sampsOrig.size()) {
                    if (dequeueWarnThresh > 0 && ++nodatact > dequeueWarnThresh) {
                        nodatact = 0;
                        if (aoWriteCt) Warning() << "AOWrite thread failed to dequeue any sample data " << dequeueWarnThresh << " times in a row. Clocks drifting?";
                        // failed to dequeue data, insert silence to keep ao writer going otherwise we get underruns!
 /*                       sampsOrig.clear();
                        samps.clear();
                        samps.resize(p.aoSrate * 0.002 * aoChansSize, lastSamp); // insert 2ms of silence?
*/
                    } else {
                        msleep(2);
                        continue;
                    }
                } else { // normal operation..
                    double now = getTime();
					nodatact = 0;
                    QString err = resampler.resample(sampsOrig, samps);
					if (tLastData > 0.0) { 
                        inpDataRate = (double(sampsOrig.size())/aoChansSize)/(now-tLastData);
						outDataRate = (double(samps.size())/aoChansSize)/(now-tLastData);
					}
                    tLastData = now;
                    if (err.length()) {
                        Error() << "AOWrite thread resampler error: " << err;
                        continue;
                    }
                }
                //if (leftOver.size()) samps.insert(samps.begin(), leftOver.begin(), leftOver.end());
                //leftOver.clear();
                unsigned sampsSize = (unsigned)samps.size(), rem = sampsSize % aoChansSize;
                if (rem) {
                    Warning() << "AOWrite thread got scan data that has invalid size.. throwing away last " << rem << " chans (writect " << aoWriteCt << ")";
                    //sampsSize -= rem;
                    //leftOver.reserve(rem);
                    //leftOver.insert(leftOver.begin(), samps.begin()+sampsSize, samps.end());
                }
                unsigned sampIdx = 0;
                while (sampIdx < sampsSize && !pleaseStop) {
                    int32 nScansToWrite = (sampsSize-sampIdx)/aoChansSize;
                    int32 nScansWritten = 0;                

					if (nScansToWrite > aoSamplesPerChan) {
						Warning() << "AOWrite thread got " << nScansToWrite << " input scans but AO Buffer size is only " << aoSamplesPerChan << " scans.. increase AO buffer size in UI!";
					}
                    /*if (nScansToWrite > aoSamplesPerChan)
                    nScansToWrite = aoSamplesPerChan;
                    else if (!aoWriteCt && nScansToWrite < aoSamplesPerChan) {                    leftOver.insert(leftOver.end(), samps.begin()+sampIdx, samps.end());
                    break;
                    }*/

                    double t0 = getTime();
                    if ( DAQmxErrChkNoJump(DAQmxWriteBinaryI16(taskHandle, nScansToWrite, 1, aoTimeout, DAQmx_Val_GroupByScanNumber, &samps[sampIdx], &nScansWritten, NULL)) )
                    {
                        break2 = true;
                        if (++daqerrct < 3) {
                            DAQmxGetExtendedErrorInfo(errBuff,2048);
                            Debug() << "AOWrite thread error on write: \"" << errBuff << "\". AOWrite retrying since errct < 3.";
                            DAQmxStopTask(taskHandle);
                            DAQmxClearTask(taskHandle);
                            taskHandle = 0;
                        } else {
                            Debug() << "AOWrite error on write and not retrying since errct >= 3!";
                        }
                        break;
                    } else if (daqerrct > 0) --daqerrct; 

                    // test code to test AI->AO delay in software, in a very rough way.. remove if you want.
                    if (excessiveDebug) {
                        u64 scanCt = (sampsOrig.size() + sampCount)/aoChansSize,
                        gScanCt = mainApp()->currentDAQScan();
                        Debug() << "AO got scan:" << scanCt << " (global scan: " << gScanCt << "). delay=~" << int32( (gScanCt-scanCt) / double(p.srate) * 1000.) << "ms";
                    }

                    const double tWrite(getTime()-t0);

                    if (nScansWritten >= 0 && nScansWritten < nScansToWrite) {
                        rem = (nScansToWrite - nScansWritten) * aoChansSize;
                        Debug() << "Partial write: nScansWritten=" << nScansWritten << " nScansToWrite=" << nScansToWrite << ", queueing for next write..";
                        //leftOver.insert(leftOver.end(), samps.begin()+sampIdx+nScansWritten, samps.end());
                        break;
                    } else if (nScansWritten != nScansToWrite) {
                        Error() << "nScansWritten (" << nScansWritten << ") != nScansToWrite (" << nScansToWrite << ")";
                        break;
                    }

                    if (++nWritesAvg > nWritesAvgMax) nWritesAvg = nWritesAvgMax;
                    writeSpeedAvg -= writeSpeedAvg/nWritesAvg;
                    double spd;
                    writeSpeedAvg += (spd = double(nScansWritten)/tWrite ) / double(nWritesAvg);

                    if (tWrite > 0.250) {
                        Debug() << "AOWrite #" << aoWriteCt << " " << nScansWritten << " scans " << aoChansSize << " chans" << " (bufsize=" << aoBufferSize << ") took: " << tWrite << " secs";
                    }
                    if (excessiveDebug) Debug() << "AOWrite speed " << spd << " Hz avg speed " << writeSpeedAvg << " Hz" << " (" << nScansWritten << " scans) buffer fill " << ((dataQueueSize()/double(dataQueueMaxSize))*100.0) << "% inprate->outrate:" << inpDataRate << "->" << outDataRate << "(?) Hz";

                    ++aoWriteCt;
                    sampIdx += nScansWritten*aoChansSize;
                }
            }
        }
    Error_Out:
        if ( DAQmxFailed(error) )
            DAQmxGetExtendedErrorInfo(errBuff,2048);
        if ( taskHandle != 0) {
            DAQmxStopTask(taskHandle);
            DAQmxClearTask(taskHandle);
            taskHandle = 0;
        }

        if( DAQmxFailed(error) ) {
            QString e;
            e.sprintf("DAQmx Error: %s\nDAQMxBase Call: %s",errBuff, callStr);
            if (!noDaqErrPrint) {
                Error() << e;
            }
            emit daqError(e);
        }   

        Debug() << "AOWrite thread ended after " << aoWriteCt << " chunks written.";
    }

    /* static */
    void NITask::recomputeAOAITab(QVector<QPair<int, int> > & aoAITab, QString & aoChan, const Params & p)
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
    
    bool NITask::createAITasks() 
    {
        const DAQ::Params & p(params);

        if (    DAQmxErrChkNoJump (DAQmxCreateTask("",&taskHandle))
             || DAQmxErrChkNoJump (DAQmxCreateAIVoltageChan(taskHandle,chan.toUtf8().constData(),"",(int)p.aiTerm,min,max,DAQmx_Val_Volts,NULL))
             || DAQmxErrChkNoJump (DAQmxCfgSampClkTiming(taskHandle,clockSource,sampleRate,DAQmx_Val_Rising,DAQmx_Val_ContSamps,bufferSize)) 
             || DAQmxErrChkNoJump (DAQmxTaskControl(taskHandle, DAQmx_Val_Task_Commit)) )
            return false; 
        if ( DAQmxErrChkNoJump(DAQmxSetAIMax(taskHandle,chan.toUtf8().constData(),max)) )
            return false;
        if ( DAQmxErrChkNoJump(DAQmxSetAIMin(taskHandle,chan.toUtf8().constData(),min)) )
            return false;

        //DAQmxErrChk (DAQmxCfgInputBuffer(taskHandle,dmaBufSize));  //use a 1,000,000 sample DMA buffer per channel
        //DAQmxErrChk (DAQmxRegisterEveryNSamplesEvent (taskHandle, DAQmx_Val_Acquired_Into_Buffer, everyNSamples, 0, DAQPvt::everyNSamples_func, this)); 
        if (p.dualDevMode) {
            const char * clockSource2 = clockSource;//*/"PFI2";
//#ifdef DEBUG
//            clockSource2 = clockSource;
//#endif
            if (    DAQmxErrChkNoJump (DAQmxCreateTask((QString("")+QString::number(qrand())).toUtf8(),&taskHandle2))
                 || DAQmxErrChkNoJump (DAQmxCreateAIVoltageChan(taskHandle2,chan2.toUtf8().constData(),"",(int)p.aiTerm,min,max,DAQmx_Val_Volts,NULL)) 
                 || DAQmxErrChkNoJump (DAQmxCfgSampClkTiming(taskHandle2,clockSource2,sampleRate,DAQmx_Val_Rising,DAQmx_Val_ContSamps,bufferSize2)) 
                 || DAQmxErrChkNoJump (DAQmxTaskControl(taskHandle2, DAQmx_Val_Task_Commit)) )
                 return false;
        }
        return true;
    }

    bool NITask::startAITasks() 
    {
        const DAQ::Params & p(params);
        if ( DAQmxErrChkNoJump(DAQmxStartTask(taskHandle)) )
            return false; 
        if (p.dualDevMode) { 
            if ( DAQmxErrChkNoJump(DAQmxStartTask(taskHandle2)) )
                return false; 
        }
        return true;
    }

    void NITask::destroyAITasks()
    {
        if ( taskHandle != 0) {
            DAQmxStopTask (taskHandle);
            DAQmxClearTask (taskHandle);
            taskHandle = 0;
        }
        if ( taskHandle2 != 0) {
            DAQmxStopTask (taskHandle2);
            DAQmxClearTask (taskHandle2);
            taskHandle2 = 0;
        }
    }

    // returns 0 if all ok, -1 if unrecoverable error, 1 if had "buffer overflow error" and tried did acq restart..
    int NITask::doAIRead(TaskHandle th, u64 samplesPerChan, std::vector<int16> & data, unsigned long oldS, int32 pointsToRead, int32 & pointsRead)
    {
        const DAQ::Params & p(params);
        if (DAQmxErrChkNoJump (DAQmxReadBinaryI16(th,samplesPerChan,timeout,DAQmx_Val_GroupByScanNumber,&data[oldS],pointsToRead,&pointsRead,NULL))) {
            Debug() << "Got error number on AI read: " << error;
            if (p.autoRetryOnAIOverrun && acceptableRetryErrors.contains(error)) {
                if (nReadRetries > 2) {
                    Error() << "Auto-Retry of AI read failed due to too many consecutive overrun errors!";
                    return -1;
                }
                Debug() << "Error ok? Ignoring/Retrying read.. ";
                double t0 = Util::getTime();
                static const double fixedRestartTime = 0.050;
                destroyAITasks();
                // XXX TODO FIXME -- figure out if we need to settle here for some time to restart the task
                if (muxMode) { setDO(false); /*QThread::msleep(100);*/ }
                if ( !createAITasks() || !startAITasks() )
                    return -1;
                const double tleft = fixedRestartTime - (Util::getTime()-t0);
                if (tleft > 0.0) QThread::usleep(tleft*1e6);
                if (muxMode) setDO(true);
                Debug() << "Restart elapsed time: " << ((Util::getTime()-t0)*1000.0) << "ms";
                ++nReadRetries;
                fudgeDataDueToReadRetry();
                return 1;
            } else
                return -1;
        }
        nReadRetries = 0;
        return 0;
    }

    void NITask::fudgeDataDueToReadRetry()
    {
        const DAQ::Params & p (params);
        const double tNow = Util::getTime();
        const double tFudge = tNow - lastEnq;
        u64 samps2Fudge = static_cast<u64>(p.srate * tFudge) * static_cast<u64>(p.nVAIChans);
        Debug() << "Fudging " << (tFudge*1e3) << "ms worth of data (" << samps2Fudge << " samples)..";
        std::vector<int16> dummy(p.nVAIChans,0x7fff);
        while (samps2Fudge) {
            if (!writer.write(&dummy[0],1))
                Error() << "PagedScanWriter::write() returned false in NITask::fudgeDataDueToReadRetry()!  Fixme!";
            samps2Fudge -= p.nVAIChans;
        }
        if (aoWriteThr) {
            dummy.clear();
            u64 aosamps2fudge = static_cast<u64>(p.srate * tFudge) * static_cast<u64>(aoAITab.size());
            Debug() << "Fudging " << (tFudge*1e3) << "ms worth of AO data as well (" << aosamps2fudge << " samples)..";
            aoWriteThr->enqueueBuffer(dummy, aoSampCount, true, aosamps2fudge);
            aoSampCount += aosamps2fudge;
        }
        totalRead += samps2Fudge;
        lastEnq = tNow;
    }

    void NITask::daqThr()
    {
        // Task parameters
        error = 0;
        taskHandle = 0, taskHandle2 = 0;
        errBuff[0]='\0';
        callStr = "";
        double startTime, lastReadTime;
        const Params & p (params);

        // used for auto-retry code .. the following status codes are accepted for auto-retry, if p.autoRetryOnAIOverrun is true
        acceptableRetryErrors.clear(); acceptableRetryErrors << -200279 <<  -200284;
        nReadRetries = 0;

        // Channel spec string for NI driver
        chan = "", chan2 = "";
        QString aoChan = "";
          
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
        min = p.range.min;
        max = p.range.max;
        const int nExtraChans1 = p.nExtraChans1, nExtraChans2 = p.dualDevMode ? p.nExtraChans2 : 0;
        

        // Params dependent on mode and DAQ::Params, etc
        clockSource = p.extClock ? "PFI2" : "OnboardClock"; ///< TODO: make extClock possibly be something other than PFI2
        sampleRate = p.srate;
        timeout = DAQ_TIMEOUT;
        const int NCHANS1 = p.nVAIChans1, NCHANS2 = p.dualDevMode ? p.nVAIChans2 : 0; 
        muxMode =  (p.mode != AIRegular);
        int nscans_per_mux_scan = 1;
        aoWriteThr = 0;

        if (muxMode) {
            const int mux_chans_per_phys = ModeNumChansPerIntan[p.mode];
            sampleRate *= double(mux_chans_per_phys);
            nscans_per_mux_scan = mux_chans_per_phys;
            /*if (!p.extClock) {
                /// Aieeeee!  Need ext clock for demux mode!
                QString e("Aieeeee!  Need to use an EXTERNAL clock for DEMUX mode!");
                Error() << e;
                emit taskError(e);
                return;
            }
             */
        }

        recomputeAOAITab(aoAITab, aoChan, p);
        
        const int task_read_freq_hz = computeTaskReadFreq(p.srate);
        
        int fudged_srate = ceil(sampleRate);
        while ((fudged_srate/task_read_freq_hz) % 2) // samples per chan needs to be a multiple of 2
            ++fudged_srate;
        bufferSize = u64(fudged_srate*nChans);
        bufferSize2 = u64(fudged_srate*nChans2);
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

        // Timing parameters
        int32       pointsRead=0, pointsRead2=0;
        const int32 pointsToRead = bufferSize, pointsToRead2 = bufferSize2;
        std::vector<int16> data, data2, leftOver, leftOver2, aoData;       

        QMap<unsigned, unsigned> saved_aoPassthruMap = p.aoPassthruMap;
        QString saved_aoDev = p.aoDev;
        Range saved_aoRange = p.aoRange;
        QString saved_aoClock = p.aoClock;
        double saved_aoSrate = p.aoSrate;
        unsigned saved_aoBufferSizeCS = p.aoBufferSizeCS; 


        if (muxMode) {
            setDO(false);// set DO line low to reset external MUX
            msleep(1000); // keep it low for 1 second
        }

        if ( !createAITasks() ) goto Error_Out;

        if (p.aoPassthru && aoAITab.size()) {
            aoWriteThr = new AOWriteThread(0, aoChan, p);
            Connect(aoWriteThr, SIGNAL(daqError(const QString &)), this, SIGNAL(taskError(const QString &)));
            aoWriteThr->start();                
        }

        if ( !startAITasks() ) goto Error_Out;

        if (muxMode) {
            setDO(true); // now set DO line high to start external MUX and clock on PFI2
        }

        lastEnq = lastReadTime = startTime = getTime();
        aoSampCount = 0;
        double hzAvg = 0.;
        int nHz = 0, nHzMax = 20;

        while( !pleaseStop ) {
            data.clear(); // should already be cleared, but enforce...
            data2.clear();
            if (leftOver.size()) data.swap(leftOver);            
            if (leftOver2.size()) data2.swap(leftOver2);
            unsigned long oldS = (unsigned long)data.size(), oldS2 = (unsigned long)data2.size();
            data.reserve(pointsToRead+oldS);
            data.resize(pointsToRead+oldS);

            double tr0 = getTime();

            // XXX DEBUG HACK.. test DAQ retry mechanism
            /*static int ctr = 0;
            if (++ctr >= 10) {
                QThread::msleep(1000);
                ctr = 0;
            }*/

            //DAQmxErrChk (DAQmxReadBinaryI16(taskHandle,samplesPerChan,timeout,DAQmx_Val_GroupByScanNumber,&data[oldS],pointsToRead,&pointsRead,NULL));
            int retVal = doAIRead(taskHandle, samplesPerChan, data, oldS, pointsToRead, pointsRead);
            if (retVal < 0) goto Error_Out; // fatal
            else if (retVal > 0) continue; // retry
            // else retval == 0, all ok

            double hz = pointsRead/(getTime()-tr0);
            hzAvg = ((hzAvg*nHz) + hz) / (nHz+1);
            if (++nHz >= nHzMax) nHz = nHzMax-1;
            if (excessiveDebug) Debug() << "read rate: " << hz << " Hz, avg " << hzAvg << " Hz";

            if (p.dualDevMode) {
                data2.reserve(pointsToRead2+oldS2);
                data2.resize(pointsToRead2+oldS2);                
                //DAQmxErrChk (DAQmxReadBinaryI16(taskHandle2,samplesPerChan2,timeout,DAQmx_Val_GroupByScanNumber,&data2[oldS2],pointsToRead2,&pointsRead2,NULL));
                retVal = doAIRead(taskHandle2, samplesPerChan2, data2, oldS2, pointsToRead2, pointsRead2);
                if (retVal < 0) goto Error_Out; // fatal
                else if (retVal > 0) continue; // retry
                // else retval == 0, all ok

                // TODO FIXME XXX -- *use* this data.. for now we are just testing
                //Debug() << "Read " << pointsRead2 << " samples from second dev.\n";
            }
            lastReadTime = getTime();
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
                const int datasz = int(data.size()), datasz2 = int(data2.size());
                tmp.reserve(datasz);
                tmp2.reserve(datasz2);
                const int nMx = nChans-nExtraChans1, nMx2 = nChans2-nExtraChans2;
                if (nMx > 0) {
                    for (int i = nMx; nExtraChans1 && i <= datasz; i += nChans) {
                        std::vector<int16>::const_iterator begi = data.begin()+(i-nMx), endi = data.begin()+i;
                        tmp.insert(tmp.end(), begi, endi); // copy non-aux channels for this MUXed sub-scan
                        if ((!((tmp.size()+nExtraChans1) % NCHANS1)) && i+nExtraChans1 <= datasz) {// if we are on a virtual scan-boundary, then ...
                            begi = data.begin()+i;  endi = begi+nExtraChans1;
                            tmp.insert(tmp.end(), begi, endi); // insert the scan into the destination data
                        }
                    }
                } else { // not normally reached currently, but just in case we are MUX mode and 1st dev is *ALL* AUX, we take every Nth scan
                    for (int i = 0; nExtraChans1 && i+nChans <= datasz; i += nChans * nscans_per_mux_scan) {
                        tmp.insert(tmp.end(), data.begin()+i, data.begin()+i+nChans);
                    }
                }
                if (nMx2 > 0) { // if we _aren't_ in "secondDevIsAuxOnly" mode
                    for (int i = nMx2; p.dualDevMode && nExtraChans2 && i <= datasz2; i += nChans2) {
                        std::vector<int16>::const_iterator begi = data2.begin()+(i-nMx2), endi = data2.begin()+i;
                        tmp2.insert(tmp2.end(), begi, endi); // copy non-aux channels for this MUXed sub-scan
                        if ((!((tmp2.size()+nExtraChans2) % NCHANS2)) && i+nExtraChans2 <= datasz2) {// if we are on a virtual scan-boundary, then ...
                            begi = data2.begin()+i;  endi = begi+nExtraChans2;
                            tmp2.insert(tmp2.end(), begi, endi); // insert the scan into the destination data
                        }
                    }
                } else { // we are in "secondDevIsAuxOnly" mode, so take every Nth scan to match first dev sampling rate..
                    for (int i = 0; nExtraChans2 && i+nChans2 <= datasz2; i += nChans2 * nscans_per_mux_scan) {
                        tmp2.insert(tmp2.end(), data2.begin()+i, data2.begin()+i+nChans2);
                    }
                }
                if (tmp.size()) { data.swap(tmp); nRead = (int32)data.size(); }
                if (tmp2.size()) { data2.swap(tmp2); nRead2 = (int32)data2.size(); }
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

            totalReadMut.lock();
            totalRead += static_cast<u64>(nRead) + static_cast<u64>(nRead2);
            totalReadMut.unlock();

            // note that from this point forward, the 'data' buffer is the only valid buffer
            // and it contains the MERGED data from both devices if in dual dev mode.
            
            // now, do optional AO output .. done in another thread to save on latency...
            if (aoWriteThr) {  
                {   // detect AO passthru changes and re-setup the AO task if ao passthru spec changed
                    QMutexLocker l(&p.mutex);
                    if (saved_aoPassthruMap != p.aoPassthruMap
                        || saved_aoRange != p.aoRange
                        || saved_aoDev != p.aoDev
                        || saved_aoClock != p.aoClock
                        || saved_aoSrate != p.aoSrate
                        || saved_aoBufferSizeCS != p.aoBufferSizeCS) {
                        // delete aoWriteThr, aoWriteThr = 0;   <--- SLOW SLOW SLOW EXPENSIVE OPERATION!  So we don't do it here, instead we defer deletion to the new AOWriteThread that replaces this one!
                        aoData.clear();
                        recomputeAOAITab(aoAITab, aoChan, p);
                        saved_aoPassthruMap = p.aoPassthruMap;
                        saved_aoRange = p.aoRange;
                        saved_aoDev = p.aoDev;
                        saved_aoClock = p.aoClock;
                        saved_aoSrate = p.aoSrate;
                        saved_aoBufferSizeCS = p.aoBufferSizeCS;
                        /* Note: deleting the old AOWrite thread is potentially slow due to expensive/slow wait for DAQmxWriteBinary to finish when joining threads
                                 so.. we tell the newly created aowrite thread to delete the old thread for us!  Clever! :) */
                        aoWriteThr = new AOWriteThread(0, aoChan, p, aoWriteThr /* old thread to delete when start() is called... */);
                        Connect(aoWriteThr, SIGNAL(daqError(const QString &)), this, SIGNAL(taskError(const QString &)));
                        aoWriteThr->start();
                    }
                }
                const int dsize = int(data.size());
                aoData.reserve(aoData.size()+dsize);
                for (int i = 0; i < dsize; i += NCHANS1+NCHANS2) { // for each scan..
                    for (QVector<QPair<int,int> >::const_iterator it = aoAITab.begin(); it != aoAITab.end(); ++it) { // take ao channels
						const int FIRSTAUX = muxMode ? (NCHANS1+NCHANS2) - (nExtraChans1+nExtraChans2) : 0;
                        const int aiChIdx = ( (p.doPreJuly2011IntanDemux || !muxMode || ((*it).second) >= FIRSTAUX) ? ((*it).second) : mapNewChanIdToPreJuly2011ChanId((*it).second, p.mode, p.dualDevMode && !p.secondDevIsAuxOnly) );
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
                u64 sz = aoData.size();
                aoWriteThr->enqueueBuffer(aoData, aoSampCount);
                aoSampCount += sz;
            }
            
            doFinalDemuxAndEnqueue(data);
            lastEnq = lastReadTime;

            // fast settle...
            if (muxMode && fast_settle && !leftOver.size() && !leftOver2.size()) {
                double t0 = getTime();
                /// now possibly do a 'fast settle' request by stopping the task, setting the DO line low, then restarting the task after fast_settle ms have elapsed, and setting the line high
                Debug() << "Fast settle of " << fast_settle << " ms begin";
                DAQmxErrChk(DAQmxStopTask(taskHandle));
                if (p.dualDevMode) DAQmxErrChk(DAQmxStopTask(taskHandle2));
                if (aoWriteThr) {
                    delete aoWriteThr, aoWriteThr = 0; 
                    aoWriteThr = new AOWriteThread(0, aoChan, p);
                    Connect(aoWriteThr, SIGNAL(daqError(const QString &)), this, SIGNAL(taskError(const QString &)));
                    aoWriteThr->start();
                }
                setDO(false);
                msleep(fast_settle);
                fast_settle = 0;
                DAQmxErrChk(DAQmxStartTask(taskHandle));                
                if (p.dualDevMode) DAQmxErrChk(DAQmxStartTask(taskHandle2));    
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
        destroyAITasks();
        if (aoWriteThr != 0) {
            delete aoWriteThr, aoWriteThr = 0;
        }
        if( DAQmxFailed(error) ) {
            QString e;
            e.sprintf("DAQmx Error: %s\nDAQMxBase Call: %s",errBuff, callStr);
            if (!noDaqErrPrint) {
                Error() << e;
            }
            emit taskError(e);
        }
        
    }
    
    /*static*/
    inline void NITask::mergeDualDevData(std::vector<int16> & out,
                                const std::vector<int16> & data, const std::vector<int16> & data2, 
                                int NCHANS1, int NCHANS2, 
                                int nExtraChans1, int nExtraChans2)
    {
        const int nMx = NCHANS1-nExtraChans1, nMx2 = NCHANS2-nExtraChans2, s1 = int(data.size()), s2 = int(data2.size());
        out.clear();
        out.reserve(s1+s2);
        int i,j;
        for (i = 0, j = 0; i < s1 && j < s2; i+=NCHANS1, j+=NCHANS2) {
            if (nMx > 0) out.insert(out.end(), data.begin()+i, data.begin()+i+nMx);
            if (nMx2 > 0) out.insert(out.end(), data2.begin()+j, data2.begin()+j+nMx2);
            out.insert(out.end(), data.begin()+i+nMx, data.begin()+i+NCHANS1);
            out.insert(out.end(), data2.begin()+j+nMx2, data2.begin()+j+NCHANS2);
        }
        if (i < s1 || j < s2) 
            Error() << "INTERNAL ERROR IN FUNCTION `mergeDualDevData()'!  The two device buffers data and data2 have differing numbers of scans! FIXME!  Aieeeee!!\n";
    }        

    void NITask::setDO(bool onoff)
    {
        const char *callStr = "";

        // Task parameters
        int      error = 0;
        TaskHandle  taskHandle = 0;
        char        errBuff[2048];
        QString tmp;

        // Channel parameters
        const QString & chan (doCtlChan());
        if (chan.isEmpty()) return;

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
            emit taskError(e);
        }
    }

    void NITask::requestFastSettle() 
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


    static inline void ApplyNewIntanDemuxToScan(int16 *begin, const unsigned nchans_per_intan, const unsigned num_intans) {
        int narr = nchans_per_intan*num_intans;
        int16 tmparr[NUM_MUX_CHANS_MAX]; // hopefully 512 channels is freakin' enough..
        if (narr > NUM_MUX_CHANS_MAX) {
            Error() << "INTERNAL ERROR: Scan size too large for compiled-in limits (too many INTANs?!).  Please fix the sourcecode at DAQ::ApplyJFRCIntanXXDemuxToScan!";
            return;
        }
        int i,j,k;
        for (k = 0; k < int(num_intans); ++k) {
            const int jlimit = (k+1)*int(nchans_per_intan);
            for (i = k, j = k*int(nchans_per_intan); j < jlimit; i+=num_intans,++j) {
                tmparr[j] = begin[i];
            }
        }
        memcpy(begin, tmparr, narr*sizeof(int16));
    }

    void NITask::doFinalDemuxAndEnqueue(std::vector<int16> & data)
    {
        const DAQ::Params & p (params);
        if (!p.doPreJuly2011IntanDemux && p.mode != DAQ::AIRegular) {
            for (int i = 0; i < (int)data.size(); i += p.nVAIChans) {
                ApplyNewIntanDemuxToScan(&data[i], DAQ::ModeNumChansPerIntan[p.mode], DAQ::ModeNumIntans[p.mode]*(p.dualDevMode && !p.secondDevIsAuxOnly ? 2 : 1));
            }
        }
        if (!writer.write(&data[0],unsigned(data.size())/p.nVAIChans)) {
            Error() << "NITask::daqThr writer.write() returned false! FIXME!";
        }
        data.clear();
    }


    /// some helper funcs from SpikeGL.h
    static const QString acqModes[] = { "AI60Demux", "AIRegular", "AI120Demux", "JFRCIntan32", "AI128Demux", "AI256Demux", "AI64Demux", "AI96Demux", "AI128_32_Demux", QString::null };
    
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


    static const QString acqStartEndModes[] = { "Immediate", "PDStartEnd", "PDStart", "Timed", "StimGLStartEnd", "StimGLStart", "AITriggered", "Bug3TTLTriggered", QString::null };
    
    const QString & AcqStartEndModeToString(AcqStartEndMode m) {
        if (m >= 0 && m < N_AcqStartEndModes) 
            return acqStartEndModes[(int)m];
        static QString unk ("Unknown");
        return unk;
    }

    Task::Task(QObject *parent, const QString & nam, const PagedScanReader & prb)
        : QThread(parent), totalRead(0ULL), writer(prb.scanSizeSamps(),prb.metaDataSizeBytes(),prb.rawData(),prb.totalSize(),prb.pageSize())
	{
        setObjectName(nam);
	}
	
    Task::~Task() {   }
	
	u64 Task::lastReadScan() const
    {
        u64 ret (0);
        totalReadMut.lock();
        ret = totalRead / u64(numChans());
        totalReadMut.unlock();
        return ret;
    }
	
    SubprocessTask::SubprocessTask(DAQ::Params & p, QObject *parent, const QString & shortName,
                                   const QString & exeName, const PagedScanReader &ref_reader)
        : Task(parent, shortName + " DAQ task", ref_reader), params(p),
	  shortName(shortName), dirName(shortName + "_SpikeGLv" + QString(VERSION_STR).right(8)), exeName(exeName)
	{
        QString tp(QDir::tempPath());
        if (!tp.endsWith("/")) tp.append("/");
        exeDir = tp + dirName + "/";
        Debug() << shortName << " exedir = " << exeDir;
    }
	
	SubprocessTask::~SubprocessTask() { if (isRunning()) { stop(); wait(); } }
	
	void SubprocessTask::stop() { pleaseStop = true; }
		
    QString SubprocessTask::exePath() const { return exeDir + exeName; }
	
	bool SubprocessTask::setupExeDir(QString *errOut) const
	{
		if (errOut) *errOut = "";
        QDir().mkpath(exeDir);
        QDir d(exeDir);
        if (!d.exists()) { if (errOut) *errOut = QString("Could not create ") + exeDir; return false; }
		QStringList files = filesList();
		for (QStringList::const_iterator it = files.begin(); it != files.end(); ++it) {
			const QString bn ((*it).split("/").last());
            const QString dest (exeDir + bn);
			if (QFile::exists(dest) && !QFile::remove(dest)) {
				if (errOut) *errOut = dest + " exists and cannot be removed.";
				return false;
			}
			if (!QFile::copy(*it, dest)) {
				if (errOut) *errOut = dest + " file create/write failed.";
				return false;
			}
			QFile f(dest);
			if (dest.endsWith(".exe") || dest.endsWith(".dll")) 
                f.setPermissions(f.permissions()|QFile::ReadOwner|QFile::ExeOwner|QFile::WriteOwner|QFile::WriteOther|QFile::WriteGroup|QFile::ReadOther|QFile::ExeOther|QFile::ReadGroup|QFile::ExeGroup);
			else
                f.setPermissions(f.permissions()|QFile::ReadOwner|QFile::ReadOther|QFile::ReadGroup|QFile::WriteOwner|QFile::WriteOther|QFile::WriteGroup);
		}
		return true;
	}
	
	void SubprocessTask::slaveProcStateChanged(QProcess::ProcessState ps)
	{
		switch(ps) {
			case QProcess::NotRunning:
				Debug() << shortName << " slave process got state change -> NotRunning";
				if (isRunning() && !pleaseStop) {
                    emit taskError(shortName + " slave process died unexpectedly!");
					pleaseStop = true;
					Debug() << shortName << " task thread still running but slave process died -- signaling daqThr() to end as a result...";
				} 
				break;
			case QProcess::Starting:
				Debug() << shortName << " slave process got state change -> Starting";
				break;
			case QProcess::Running:
				Debug() << shortName << " slave process got state change -> Running";
				break;
		}
	}
	
	void SubprocessTask::daqThr() 
	{		
		if (!platformSupported()) {
            emit taskError(QString("This platform does not support the ") + shortName + " acquisition mode!");
			pleaseStop = true;
			return;			
		}
		
		pleaseStop = false;
		QString err("");
		if (!setupExeDir(&err)) {
            emit taskError(err);
			return;
		}
        QProcess p(0); // Qt throws a warning if we create the process with "this" as its QObject parent because 'this' object lives in another thread. Weird.  No harm tho: note that even though the qprocess has no parent, it's ok, it will die anyway :)
		//		p.moveToThread(this);
		static bool RegisteredMetaType = false;
		if (!RegisteredMetaType) {
			int id = qRegisterMetaType<QProcess::ProcessState>("QProcess::ProcessState");
			(void) id;
			RegisteredMetaType = true;
		}
		Connect(&p, SIGNAL(stateChanged(QProcess::ProcessState)), this, SLOT(slaveProcStateChanged(QProcess::ProcessState)));		
        p.setProcessChannelMode(usesMergedChannels() ? QProcess::MergedChannels : QProcess::SeparateChannels);
        p.setWorkingDirectory(exeDir);
		QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
		setupEnv(env);
		p.setProcessEnvironment(env);
		if (!interpreter().length()) {
			p.start(exePath());
		} else  {
			p.start(interpreter(), QStringList()<<exePath());
		} 
		if (p.state() == QProcess::NotRunning) {
            emit taskError(shortName + " slave process startup failed!");
			p.kill();
			return;
		}
		if (!p.waitForStarted(30000)) {
            emit taskError(shortName + " slave process: startup timed out!");
			p.kill();
			return;
		}
		Debug() << shortName << " slave process started ok";
        emit(justStarted());

		int tout_ct = 0;
		QByteArray data;
		
        int notRunningCt = 0;
		
		data.reserve(4*1024*1024);
        while (!pleaseStop /*&& p.state() != QProcess::NotRunning*/) {
			if (p.state() == QProcess::Running) { 
				qint64 n_avail = p.bytesAvailable();
                int timeout_ms = readTimeoutMS(); if (timeout_ms < 0) timeout_ms = -1;
                if (n_avail == 0 && !p.waitForReadyRead(timeout_ms)) {
                    if (int((double(++tout_ct)*double(timeout_ms))/1000.0) > readTimeoutMaxSecs()) {
                        emit(taskError(shortName + " slave process: timed out while waiting for data!"));
						p.kill();
						return;
					} else {
						Warning() << shortName << " slave process: read timed out, ct=" << tout_ct;
                        readStdErr(p);
                        processCmds(p);
						continue;
					}
				} else if (n_avail < 0) {
                    emit(taskError(shortName + " slave process: eof on stdout stream!"));
					p.kill();
					return;
				}
                tout_ct = 0;
				QByteArray buf = p.readAll();
				if (!buf.size()) {
					Warning() << shortName << " slave process: read 0 bytes!";
                    readStdErr(p);
                    processCmds(p);
					continue;
				}
				data.append(buf);
				unsigned consumed = gotInput(data, buf.size(), p);
				data.remove(0,consumed);
				if (excessiveDebug && data.size()) {
					Debug() << shortName << " slave process: partial data left over " << data.size() << " bytes";
				}
				
                readStdErr(p);
				processCmds(p);
				
            } else {
                msleep(10); // sleep 10ms and try again..
                if (++notRunningCt > 100) {
                    emit taskError(shortName + " slave process died unexpectedly!");
                    p.kill();
                    return;
                }
            }
			
		}
		if (p.state() == QProcess::Running) {
			sendExitCommand(p);
			Debug() << "Sent 'exit' cmd to " << shortName << " slave process...";
			p.waitForFinished(500); /// wait 500 msec for finish
		}
		if (p.state() != QProcess::NotRunning) {
			p.kill();
			Debug() << shortName << " slave process refuses to exit gracefully.. forcibly killed!";
		}
	}
	
    void SubprocessTask::readStdErr(QProcess &p) {
        if (!usesMergedChannels()) {
            p.setReadChannel(QProcess::StandardError);
            QByteArray tmpData = p.readAll();
            p.setReadChannel(QProcess::StandardOutput);
            if (tmpData.size()) gotStdErr(tmpData);
        }
    }

    void SubprocessTask::gotStdErr(const QByteArray & data) {
        QString s(data);
        s = s.trimmed();
        Debug() << shortName << " stderr: " << s;
    }

	void SubprocessTask::pushCmd(const QByteArray &c) {
		cmdQMut.lock();
		cmdQ.push_back(c);
		cmdQMut.unlock();
	}
	
	void SubprocessTask::processCmds(QProcess & p)
	{
		QList<QByteArray> cmds;
		cmdQMut.lock();
		cmds = cmdQ;
		cmdQ.clear();
		cmdQMut.unlock();
		for (QList<QByteArray>::iterator it = cmds.begin(); it != cmds.end(); ++it) {
			if (outputCmdsAreBinary()) {
				Debug() << shortName << ": sending command of size " << (*it).size() << " bytes";				
			} else {
                Debug() << shortName << ": sending command `" << QString(*it).trimmed() << "'";
			}
			p.write(*it);
		}
	}
	

    /*static*/ const double BugTask::SamplingRate = 16.0 / 0.0006144;
    /*static*/ const double BugTask::ADCStepNeural = 2.5;                // units = uV
    /*static*/ const double BugTask::ADCStepEMG = 0.025;                 // units = mV
    /*static*/ const double BugTask::ADCStepAux = 0.0052;                // units = V
    /*static*/ const double BugTask::ADCStep = 1.02 * 1.225 / 1024.0;    // units = V
    /*static*/ const double BugTask::MaxBER = -1.0;
    /*static*/ const double BugTask::MinBER = -5.0;

    BugTask::BugTask(DAQ::Params & p, QObject *parent, const PagedScanReader & psr)
        : SubprocessTask(p, parent, "Bug3", "bug3_spikegl.exe", psr), req_shm_pg_sz(requiredShmPageSize(p.nVAIChans)), aoWriteThread(0), aoSampCount(0), aireader(0)
	{
		state = 0; nblocks = 0; nlines = 0;
		debugTTLStart = 0;
        if (writer.pageSize() != req_shm_pg_sz)
            Error() << "INTERNAL ERROR: BugTask needs a shm with page size == requiredShmPageSize()!  FIXME!!";
	}

    BugTask::~BugTask() {
        if (isRunning()) { stop(); wait(); }
#ifdef FAKEDAQ
        // stuff...
#else // !FAKEDAQ == real NI platform
        if (aoWriteThread) delete aoWriteThread; aoWriteThread=0;
#endif
        if (aireader) delete aireader, aireader=0;
        if (numChans() > 0)
            Debug() << "Bug Task `" << objectName() << "' deleted after processing " << totalRead/u64(numChans()) << " scans.";
    }

    unsigned BugTask::requiredShmPageSize() const { return req_shm_pg_sz; }
    /*static*/ unsigned BugTask::requiredShmPageSize(unsigned nChans) {
        return sizeof(BlockMetaData) + (nChans * NeuralSamplesPerFrame * FramesPerBlock * sizeof(int16));
    }

    unsigned BugTask::numChans() const { return params.nVAIChans; }
    unsigned BugTask::samplingRate() const { return params.srate; }
	
	QStringList BugTask::filesList() const
	{
		QStringList files; 
		files.push_back(QString(":/Bug3/Bug3_for_SpikeGL/Bug3_for_SpikeGL/bin/Release/") + exeName);
		files.push_back(":/Bug3/Bug3_for_SpikeGL/Bug3_for_SpikeGL/bug3a_receiver_1.bit");
		files.push_back(":/Bug3/Bug3_for_SpikeGL/Bug3_for_SpikeGL/libFrontPanel-csharp.dll");
		files.push_back(":/Bug3/Bug3_for_SpikeGL/Bug3_for_SpikeGL/libFrontPanel-pinv.dll");
		return files;
	}
	
    int BugTask::readTimeoutMaxSecs() const
    {
#ifdef Q_OS_DARWIN
        return 60*5; // 5 mins enough?!
#else
        return 10;
#endif
    }


	void BugTask::setupEnv(QProcessEnvironment & env) const
	{
		env.insert("BUG3_SPIKEGL_MODE", "yes");
		switch (params.bug.rate) {
			case 0:
				env.insert("BUG3_DATA_RATE", "LOW");
				break;
			case 1:
				env.insert("BUG3_DATA_RATE", "MEDIUM");
				break;
			default:
				env.insert("BUG3_DATA_RATE", "HIGH");
				break;
		}
		env.insert("BUG3_CHANGES_ON_CLOCK", QString::number(params.bug.clockEdge));
		env.insert("BUG3_ERR_TOLERANCE", QString::number(params.bug.errTol));
		if (params.bug.hpf > 0) env.insert("BUG3_HIGHPASS_FILTER_CUTOFF", QString::number(params.bug.hpf));
		if (params.bug.snf) env.insert("BUG3_60HZ_NOTCH_FILTER", "yes");		
	}
		
	void BugTask::sendExitCommand(QProcess & p) const 
	{
		p.write("exit\r\n");				
	}
	
	int BugTask::usbDataBlockSizeSamps() const
	{
		int frameSize = 174;
		switch (params.bug.rate) {
			case 0: frameSize = 48; break;
			case 1: frameSize = 96; break;
			default: break;
		}
		return frameSize * FramesPerBlock;
	}
	
	unsigned BugTask::gotInput(const QByteArray & data, unsigned lastReadNBytes, QProcess & p) 
	{
		(void) p;
		unsigned consumed = 0;
		for (int i = (data.size()-lastReadNBytes), l = 0; i < data.size(); ++i) {
			if (data[i] == '\n') {
				QString line = data.mid(l, i-l+1);
				l = consumed = i+1;
				processLine(line, block, nblocks, state, nlines); 
				if (state > nstates) {
					processBlock(block, nblocks++);
					block.clear();
					state = 0;
				}
			}
		}
		return consumed;
	}
			
	void BugTask::processLine(const QString & lineUntrimmed, QMap<QString, QString> & block, const quint64 & nblocks, int & state, quint64 & nlines)
	{
		QString line = lineUntrimmed.trimmed();
#if 0
		if (excessiveDebug) {
			QString linecpy = line;
			if (line.length() > 75) linecpy = line.left(65) + "..." + line.right(10);
			if (linecpy.length() > 3) { Debug() << "Bug3: " << linecpy; }
		}
#endif
		if (line.startsWith("---> Console")) { 
			++nlines;
			state=1; 
			if (excessiveDebug) Debug() << "Bug3: New block, current nblocks=" << nblocks; 
		} else if (state) {
			++nlines;
            static QRegExp blkSepRE("[}{]"); ///< made static here to compile the RE only once.. performance optimization
			// we are in parsing mode..
            QStringList nv = line.split(blkSepRE, QString::SkipEmptyParts);
			if (nv.count() == 2) {
				//Debug() << "Bug3: Got name=" << nv.first() << " v=" << nv.last().left(5) << "..." << nv.last().right(5);
				block[nv.first()] = nv.last();
				++state;
            }
            else if (excessiveDebug)  Debug() << "Bug3: line: '" << line << "' had nv.count()=" << nv.count();
		} else if (!state && line.startsWith("USRMSG:")) {
            emit(taskWarning(line.mid(7).trimmed()));
        } else if (!state && line.startsWith("WARNMSG:")) {
            Warning() << "Bug3: " << line.mid(8).trimmed();
        } else if (!state && line.startsWith("LOGMSG:")) {
            Log() << "Bug3: " << line.mid(7).trimmed();
        }
	}
	
	void BugTask::processBlock(const QMap<QString, QString> & blk, quint64 blockNum)
	{
		BlockMetaData meta;
		meta.blockNum = blockNum;
		const int nchans (numChans());
		std::vector<int16> samps;
        samps.resize(nchans * NeuralSamplesPerFrame * FramesPerBlock, 0); // todo: fix to come from params?
		
		// todo implement..
		for (QMap<QString,QString>::const_iterator it = blk.begin(); it != blk.end(); ++it) {
			const QString & k = it.key(), & v(it.value());
#if 0
            if (excessiveDebug) Debug() << "Bug3: " << "Got key: '" << k << "'";
#endif
			if (k.startsWith("NEU_")) {
				QStringList knv = k.split("_");
				int neur_chan = 0;
				bool ok = false;
				if (knv.count() == 2) {
					neur_chan = knv.last().toInt(&ok);
					if (neur_chan < 0 || neur_chan >= TotalNeuralChans) { neur_chan = 0; ok = false; }
				} 
				if (!ok) 
					Warning() << "Bug3: Internal problem -- parse error on NEU_ key.";

				QStringList nums = v.split(",");
                //if (excessiveDebug) Debug() << "Bug3: got " << nums.count() << " neural samples for NEU chan " << neur_chan << "..";
				bool warned = false;
				for (int frame = 0; frame < FramesPerBlock; ++frame) {
					for (int neurix = 0; neurix < NeuralSamplesPerFrame; ++neurix) {
						QString num = nums.empty() ? "0" : nums.front();
						if (nums.empty()) { 
							if (!warned) Warning() << "Bug3: Internal problem -- ran out of neural samples in frame `" << v << "'";
							warned = true;
						} else nums.pop_front();
						ok = false;
						int samp = num.toUShort(&ok);
						if (!ok) Error() << "Bug3: Internal error -- parse error while reading neuronal sample `" << num << "'";
						// todo.. fix this.  wtf? can't think straight now...
						samp = samp-ADCOffset; // incoming data is such that 0v = (int16)1023, this is because incoming samples are 11-bit. normalize so that 0v = 0
						samp = samp * int16(32); // promote to 16-bit
						samps[ frame*(NeuralSamplesPerFrame*nchans) + neurix*nchans + neur_chan ] = samp;
					}
				}
			} else if (k.startsWith("EMG_")) {
				QStringList knv = k.split("_");
				int emg_chan = 0;
				bool ok = false;
				if (knv.count() == 2) {
					emg_chan = knv.last().toInt(&ok);
					if (emg_chan < 0 || emg_chan >= TotalEMGChans) { emg_chan = 0; ok = false; }
				} 
				if (!ok) Warning() << "Bug3: Internal problem -- parse error on EMG_ key.";

				QStringList nums = v.split(",");
                //if (excessiveDebug) Debug() << "Bug3: got " << nums.count() << " EMG samples..";
				bool warned = false;
				for (int frame = 0; frame < FramesPerBlock; ++frame) {
					QString num = nums.empty() ? "0" : nums.front();
					if (nums.empty()) { 
						if (!warned) Warning() << "Bug3: Internal problem -- ran out of EMG samples in frame `" << v << "'";
						warned = true;
					}
					else nums.pop_front();
					bool ok = false;
					int samp = num.toUShort(&ok);
					if (!ok) Error() << "Bug3: Internal error -- parse error while reading emg sample `" << num << "'";
					samp = samp-ADCOffset; // normalize since incoming data is weird 11 bit
					samp = samp * int16(32); // promote to 16-bit
					for (int emgix = 0; emgix < NeuralSamplesPerFrame; ++emgix) { // need to produce 16 samples for each 1 emg sample read in order to match neuronal rate!						
						samps[ frame*(NeuralSamplesPerFrame*nchans) + emgix*nchans + (TotalNeuralChans+emg_chan) ] = samp;
					}

				}
			} else if (k.startsWith("AUX_")) {
				QStringList knv = k.split("_");
				int aux_chan = 0;
				bool ok = false;
				if (knv.count() == 2) {
					aux_chan = knv.last().toInt(&ok);
					if (aux_chan < 0 || aux_chan >= TotalAuxChans) { aux_chan = 0; ok = false; }
				} 
				if (!ok) Warning() << "Bug3: Internal problem -- parse error on AUX_ key.";
				bool warned = false;
				QStringList nums = v.split(",");
                //if (excessiveDebug) Debug() << "Bug3: got " << nums.count() << " AUX samples..";
				double avgVunreg = 0.0;
				for (int frame = 0; frame < FramesPerBlock; ++frame) {
					QString num = nums.empty() ? "0" : nums.front();
					if (nums.empty()) { 
						if (!warned) Warning() << "Bug3: Internal problem -- ran out of AUX samples in frame `" << v << "'";
						warned = true;
                    }
					else nums.pop_front();
					bool ok = false;
					int samp = num.toUShort(&ok);
					if (!ok) Error() << "Bug3: Internal error -- parse error while reading emg sample `" << num << "'";
					samp = samp-ADCOffset; // normalize since incoming data is 11-bit
					if (aux_chan == 1)  avgVunreg += ADCStep * double(samp);
					samp = samp * int16(32); // promote to 16-bit					
					for (int auxix = 0; auxix < NeuralSamplesPerFrame; ++auxix) { // need to produce 16 samples for each 1 emg sample read in order to match neuronal rate!	
						samps[ frame*(NeuralSamplesPerFrame*nchans) + auxix*nchans + (TotalNeuralChans+TotalEMGChans+aux_chan) ] = samp;
					}
				}
				if (aux_chan == 1) {
					avgVunreg = avgVunreg / (double)FramesPerBlock;
					if (avgVunreg < 0.0) avgVunreg = 0.0;
					if (avgVunreg > 6.0) avgVunreg = 6.0;
					meta.avgVunreg = avgVunreg;
				}
			} else if (k.startsWith("TTL_")) {
				// don't grab all ttl chans here, since we only really support a subset of them
				QStringList knv = k.split("_");
				int ttl_chan = 0;
				bool ok = false;
				if (knv.count() == 2) {
					ttl_chan = knv.last().toInt(&ok);
					if (ttl_chan < 0 || ttl_chan >= TotalTTLChans) { ttl_chan = 0; ok = false; }
				} 
				if (!ok) Warning() << "Bug3: Internal problem -- parse error on TTL_ key.";
				if (params.bug.whichTTLs & (0x1<<ttl_chan)) {
                    int ttl_chan_translated = ttl_chan;
                    for (int i = 0; i < TotalTTLChans && i < ttl_chan; ++i)
                        // make the TTL line number -> our_channelid
                        // (because TTL10 from bug3 may not always be our TTL10 if say the ttl chans we have on are like 0,1,3,5,10,
                        // then TTL 10 is really 4 for us!
                        if (!(params.bug.whichTTLs & (0x1<<i)))
                            --ttl_chan_translated;
					bool warned = false;
					QStringList nums = v.split(",");
                    //if (excessiveDebug) Debug() << "Bug3: got " << nums.count() << " TTL samples..";
					for (int frame = 0; frame < FramesPerBlock; ++frame) {
						QString num = nums.empty() ? "0" : nums.front();
						if (nums.empty()) { 
							if (!warned) Warning() << "Bug3: Internal problem -- ran out of TTL samples in frame `" << v << "'";
							warned = true;
						}
						else nums.pop_front();
						bool ok = false;
						int samp = num.toUShort(&ok);
						if (!ok) Error() << "Bug3: Internal error -- parse error while reading emg sample `" << num << "'";
#if 0 // BUG3_TTL_TESTING
						// TODO HACK BUG FIXME TESTING XXX
						///*
						if (ttl_chan_translated == 0) {
							qint64 scan = qint64(blockNum)*qint64(SpikeGLScansPerBlock) + qint64(frame*NeuralSamplesPerFrame);
							const qint64 sr = params.srate;
							static const qint64 ttlWSamps = 640*2;
							static const qint64 everyNSecs = 2;
							
							if (scan>sr*everyNSecs) {
								if (scan>(debugTTLStart+sr*everyNSecs)) 
									debugTTLStart = scan;
							
								if (scan<debugTTLStart+ttlWSamps)
									samp = 1;
								else samp = 0;
							}
						}
						//*/
#endif
						if (samp) samp = 32767; // normalize high to maxV
						for (int ix = 0; ix < NeuralSamplesPerFrame; ++ix) { // need to produce 16 samples for each 1 emg sample read in order to match neuronal rate!						
                            samps[ frame*(NeuralSamplesPerFrame*nchans) + ix*nchans + (TotalNeuralChans+TotalEMGChans+TotalAuxChans+ttl_chan_translated) ] = samp;
						}						
					}				
				}
			} else if (k.startsWith("CHIPID")) {
				QStringList nums = v.split(",");
				bool warned = false;
				//if (excessiveDebug) Debug() << "Bug3: got " << nums.count() << " chipIDs..";
				for (int frame = 0; frame < FramesPerBlock; ++frame) {
					QString num = nums.empty() ? "0" : nums.front();
					if (nums.empty()) { 
						if (!warned) Warning() << "Bug3: Internal problem -- ran out of chipID data in frame `" << v << "'";
						warned = true;
					}
					else nums.pop_front();
					bool ok = false;
					quint16 chipid = num.toUShort(&ok);
					if (!ok) Error() << "Bug3: Internal error -- parse error while reading chipID `" << num << "'";
					meta.chipID[frame] = chipid;
				}
			} else if (k.startsWith("CHIP_FC")) {
				QStringList nums = v.split(",");
				bool warned = false;
				//if (excessiveDebug) Debug() << "Bug3: got " << nums.count() << " chip_fcs..";
				for (int frame = 0; frame < FramesPerBlock; ++frame) {
					QString num = nums.empty() ? "0" : nums.front();
					if (nums.empty()) { 
						if (!warned) Warning() << "Bug3: Internal problem -- ran out of chip_fc data in frame `" << v << "'";
						warned = true;
					}
					else nums.pop_front();
					bool ok = false;
					int chipfc = num.toInt(&ok);
					if (!ok) Error() << "Bug3: Internal error -- parse error while reading chip_fc `" << num << "'";
					meta.chipFrameCounter[frame] = chipfc;
				}
			} else if (k.startsWith("FRAME_MARKER_COR")) {
				QStringList nums = v.split(",");
				bool warned = false;
				//if (excessiveDebug) Debug() << "Bug3: got " << nums.count() << " FMCorr..";
				for (int frame = 0; frame < FramesPerBlock; ++frame) {
					QString num = nums.empty() ? "0" : nums.front();
					if (nums.empty()) { 
						if (!warned) Warning() << "Bug3: Internal problem -- ran out of framemarker_corr data in frame `" << v << "'";
						warned = true;
					}
					else nums.pop_front();
					bool ok = false;
					quint16 fmc  = num.toUShort(&ok);
					if (!ok) Error() << "Bug3: Internal error -- parse error while reading fmcorr `" << num << "'";
					meta.frameMarkerCorrelation[frame] = fmc;
				}
			} else if (k.startsWith("BOARD_FC")) {
				QStringList nums = v.split(",");
				bool warned = false;
				//if (excessiveDebug) Debug() << "Bug3: got " << nums.count() << " board_fc..";
				for (int frame = 0; frame < FramesPerBlock; ++frame) {
					QString num = nums.empty() ? "0" : nums.front();
					if (nums.empty()) { 
						if (!warned) Warning() << "Bug3: Internal problem -- ran out of board_fc data in frame `" << v << "'";
						warned = true;
					}
					else nums.pop_front();
					bool ok = false;
					int bfc  = num.toInt(&ok);
					if (!ok) Error() << "Bug3: Internal error -- parse error while reading board_fc `" << num << "'";
					meta.boardFrameCounter[frame] = bfc;
				}
			} else if (k.startsWith("BOARD_FRAME_TIMER")) {
				QStringList nums = v.split(",");
				bool warned = false;
				//if (excessiveDebug) Debug() << "Bug3: got " << nums.count() << " board_frame_timer..";
				for (int frame = 0; frame < FramesPerBlock; ++frame) {
					QString num = nums.empty() ? "0" : nums.front();
					if (nums.empty()) { 
						if (!warned) Warning() << "Bug3: Internal problem -- ran out of board_ftimer data in frame `" << v << "'";
						warned = true;
					}
					else nums.pop_front();
					bool ok = false;
					int bft  = num.toInt(&ok);
					if (!ok) Error() << "Bug3: Internal error -- parse error while reading board_ftimer `" << num << "'";
					meta.boardFrameTimer[frame] = bft;
				}
			} else if (k.startsWith("BER")) {
				bool ok = false;
				double n = v.toDouble(&ok);
				if (!ok) Warning() << "Bug3: Internal problem -- error parsing BER `" << v << "'";
				meta.BER = n;
			} else if (k.startsWith("WER")) {
				bool ok = false;
				double n = v.toDouble(&ok);
				if (!ok) Warning() << "Bug3: Internal problem -- error parsing WER `" << v << "'";
				meta.WER = n;				
			} else if (k.startsWith("MISSING_FC")) {
				bool ok = false;
				int n = v.toInt(&ok);
				if (!ok) Warning() << "Bug3: Internal problem -- error parsing MISSING_FC `" << v << "'";
				// TESTING HACK XXX TODO FIXME!
//				if (Util::random() < .3 && totalRead < 10000000ULL) n = int(Util::random(0.,16.)) % 6;
                meta.missingFrameCount = n;
            } else if (k.startsWith("COMM_ABSTIMENS")) {
                bool ok = false;
                u64 abs = v.toULongLong(&ok);
                if (!ok) Warning() << "Bug3: Internal problem -- error parsing COMM_ABSTIMENS `" << v << "'";
                meta.comm_absTimeNS = abs;
            } else if (k.startsWith("CREATION_ABSTIMENS")) {
                bool ok = false;
                u64 abs = v.toULongLong(&ok);
                if (!ok) Warning() << "Bug3: Internal problem -- error parsing CREATION_ABSTIMENS `" << v << "'";
                meta.creation_absTimeNS = abs;
            } else if (k.startsWith("FALSE_FC")) {
				bool ok = false;
				int n = v.toInt(&ok);
				if (!ok) Warning() << "Bug3: Internal problem -- error parsing FALSE_FC `" << v << "'";
				// TESTING HACK XXX TODO FIXME!
//				if (Util::random() < .3 && totalRead < 10000000ULL) n = int(Util::random(0.,16.)) % 6;
				meta.falseFrameCount = n;								
			}
		}
		totalReadMut.lock();
		quint64 oldTotalRead = totalRead;
		totalRead += (quint64)samps.size(); 
		totalReadMut.unlock();

        handleAI(samps);
        handleBadDataGraph(samps, meta);
        handleAOPassthru(samps);

		//Debug() << "Enq: " << samps.size() << " samps, firstSamp: " << oldTotalRead;
        if (!writer.write(&samps[0],unsigned(samps.size())/nchans,&meta)) {
            Error() << "Bug3: INTERNAL PROBLEM, writer.write() returned false!";
        }
		if (!oldTotalRead) emit(gotFirstScan());
	}

    void BugTask::handleBadDataGraph(std::vector<int16> &samps, const BugTask::BlockMetaData & meta)
    {
       const DAQ::Params & p(params);
       if (!p.bug.graphBadData) return;
       const int nchans = int(numChans());
       if (!nchans) return;
       const int nscans = int(samps.size())/nchans;

       const double & BER(meta.BER);
       double logBER;

       if (BER > 0.0) {
           logBER = ::log10(BER);
           if (logBER > 0) logBER = 0;
           else if (logBER < DAQ::BugTask::MinBER) logBER = DAQ::BugTask::MinBER;
       } else	{
           logBER = -100.0;
       }
       const double scale = -MinBER; //5.0
       double pt = logBER+scale;
       if (pt < 0.) pt = 0.;
       if (pt > scale) pt = scale;

       //const int16 samp = static_cast<int16>((double(meta.missingFrameCount) / 40.0) * 32767.0);

       const int16 samp(static_cast<int16>((pt/scale)*32767.0));
       //Debug() << "logBer=" << logBER << " scale=" << scale << " pt=" << pt << " samp=" << samp;
       for (int i = 0; i < nscans; ++i) {
           samps[i*nchans+(nchans-1)] = samp;
       }
    }

    void BugTask::handleAI(std::vector<int16> & samps) {
        if (!aireader && !params.bug.aiChans.isEmpty()) {
            aireader = new MultiChanAIReader(0);
            Connect(aireader, SIGNAL(error(const QString &)), this, SIGNAL(taskError(const QString &)));
            Connect(aireader, SIGNAL(warning(const QString &)), this, SIGNAL(taskWarning(const QString &)));
            Range r(-5.0,5.0);
            for (int i = 0; i < params.customRanges.size(); ++i) {
                if (isAIChan(params,i)) {
                    r = params.customRanges[i];
                    break;
                }
            }
            QString err = aireader->startDAQ(params.bug.aiChans, params.srate*params.bug.aiDownsampleFactor, 0.010, 1000, params.aiTerm, r);
            if (!err.isEmpty()) { emit taskError(err); return; }
        }
        if (!aireader) return;

        const int nCh = aireader->nChans();
        {
            // this block is here.. to auto-remove aisamps buffer when done
            std::vector<int16> aisamps;
            if (aireader->readAll(aisamps)) {
                if (excessiveDebug) Debug() << shortName << ": read " << aisamps.size() << " samples from AI";
                std::vector<int16> ai_resampled;
                const double factor = params.bug.aiDownsampleFactor > 0. ? 1.0/params.bug.aiDownsampleFactor : 1.0;
                if (factor > 1.0) {
                    //QString err = Resampler::resample(aisamps, ai_resampled, factor, nCh, Resampler::SincFastest, false);
                    //if (err.length()) emit taskError(err);
                    // the above is broken for some reason and produces artifacts.  i had to use this manual code..
                    int f = qRound(factor), scans = int(aisamps.size())/nCh;
                    ai_resampled.resize(aisamps.size()*f);
                    for (int i = 0, ctr = 0; i < scans; ++i) {
                        for (int j = 0; j < nCh; ++j) {
                            for (int k = 0; k < f; ++k)
                                ai_resampled[ctr++] = aisamps[i*nCh+j];
                        }
                    }
                } else
                    ai_resampled.swap(aisamps);
                ais.reserve(ais.size()+ai_resampled.size());
                ais.insert(ais.end(), ai_resampled.begin(), ai_resampled.end());
            } else {
                if (excessiveDebug) Debug() << shortName << ": failed to read from AI";
            }
        }
        if (ais.size()) {
            const int nchans = int(numChans());
            const int mfc = params.bug.graphBadData ? 1 : 0;
            const int nscans = int(samps.size())/(nchans>0?nchans:1);
            const int aisz = int(ais.size());
            const int aiscans = aisz/nCh;
            const int off = aiscans-nscans;
            for (int i = off < 0 ? -off : 0, j = 0; i < nscans && j < aiscans; ++i, ++j) {
                for (int k = 0; k < nCh; ++k)
                    samps[i*nchans+(nchans-(nCh-k))-mfc] = ais[(j*nCh)+k];
            }
            if (off > 0) {
                ais.erase(ais.begin(), ais.begin()+nscans*nCh);
                if (excessiveDebug) Debug() << "AI reader is long " << (ais.size()/nCh) << " (" << (((ais.size()/nCh)/params.srate)*1000.0) << "ms) scans";
                if (int(ais.size()/nCh) > qRound(params.srate/4.0)) {
                    ais.erase(ais.begin(), ais.begin()+( ((ais.size()/nCh)-qRound(params.srate/4.0))*nCh ) );
                }
            } else {
                if (!off) if (excessiveDebug) Debug() << "AI reader is MIRACULOUSLY spot-on!!!!!! <<<<<<";
                if (off < 0) if (excessiveDebug) Debug() << "AI reader is short " << -off << " scans";
                ais.clear();
            }
        }
    }
		
    void BugTask::handleAOPassthru(const std::vector<int16> &samps)
    {
#ifdef FAKEDAQ
        if (!params.aoPassthru) return;
        if (!aoSampCount)
            Error() << "AOWriteThread unsupported on this platform -- not a real NI platform!  AOWrites will be disabled...";
        aoSampCount += samps.size() / params.nVAIChans;
#else
        if (params.aoPassthru) {
            params.mutex.lock();
            QString aops = params.bug.aoPassthruString;
            params.mutex.unlock();
            if (!aoWriteThread || (aops.length() && savedAOPassthruString != aops)) {
                // slow! so don't do it here, instead defer deletion to inside the new AOWriteThread!
                // if (aoWriteThread) delete aoWriteThread, aoWriteThread = 0;
                savedAOPassthruString = aops;
                NITask::recomputeAOAITab(aoAITab, aoChan, params);
                aoWriteThread = new AOWriteThread(0,aoChan,params,aoWriteThread);
                Connect(aoWriteThread, SIGNAL(daqError(const QString &)), this, SIGNAL(taskError(const QString &)));
                aoWriteThread->dequeueWarnThresh = 50;
                aoWriteThread->start();
            }
        }
        if (aoWriteThread) {
            const int dsize = int(samps.size());
            std::vector<int16> aoData;
            aoData.reserve(dsize);
            const int NCHANS = params.nVAIChans;

            for (int i = 0; i < dsize; i += NCHANS) { // for each scan..
                for (QVector<QPair<int,int> >::const_iterator it = aoAITab.begin(); it != aoAITab.end(); ++it) { // take ao channels
                    const int aiChIdx = (*it).second;
                    const int dix = i+aiChIdx;
                    if (dix < dsize)
                        aoData.push_back(samps[dix]);
                    else {
                        static int errct = 0;
                        aoData.push_back(0);
                        if (errct++ < 5)
                            Error() << "INTERNAL ERROR: This shouldn't happen.  AO passthru code is buggy. FIX ME!!";
                    }
                }
            }
            u64 sz = aoData.size();
            aoWriteThread->enqueueBuffer(aoData, aoSampCount);
            aoSampCount += sz;
        }
#endif
    }

	BugTask::BlockMetaData::BlockMetaData() { ::memset(this, 0, sizeof(*this)); /*zero out all data.. yay!*/ }
	BugTask::BlockMetaData::BlockMetaData(const BlockMetaData &o) { *this = o; }
	BugTask::BlockMetaData & BugTask::BlockMetaData::operator=(const BlockMetaData & o) { ::memcpy(this, &o, sizeof(o)); return *this; }
	
	void BugTask::setNotchFilter(bool enabled)
	{
		pushCmd(QString("SNF=%1\r\n").arg(enabled ? "1" : "0").toUtf8());
	}
	void BugTask::setHPFilter(int val) { ///<   <=0 == off, >0 = freq in Hz to high-pass filter
		if (val < 0) val = 0;
		pushCmd(QString("HPF=%1\r\n").arg(val).toUtf8());
	}
	
    /* static */ QString BugTask::getChannelName(unsigned num, const Params & p)
	{
        if (int(num) < TotalNeuralChans)
            return QString("NEU%1").arg(num);
		num -= TotalNeuralChans;
		if (int(num) < TotalEMGChans)
            return QString("EMG%1").arg(num);
		num -= TotalEMGChans;
		if (int(num) < TotalAuxChans)
            return QString("AUX%1").arg(num) + (p.bug.auxTrig == int(num) ? " (TRG)" : "");
		num -= TotalAuxChans;
		if (int(num) < TotalTTLChans)
            return QString("TTL%1").arg(num) + (p.bug.ttlTrig == int(num) ? " (TRG)" : "");
        num -= TotalTTLChans;
        if (int(num)  < p.bug.aiChans.count()) {
            const QString s(p.bug.aiChans.at(num));
            QString trg = (p.bug.aiTrig.compare(s,Qt::CaseInsensitive) == 0 ? " (TRG)" : "");
            if (!trg.length() && p.bug.backupTrigger > -1 && !num) trg = " (TRG ALT)";
            return s.split("/").back().toUpper() + trg;
        }
        if (p.bug.graphBadData && int(num) == p.bug.aiChans.count())
            return QString("BER");
        return QString("UNK%1").arg(num);
    }
	
	/* static */ bool BugTask::isNeuralChan(unsigned num) { return int(num) < TotalNeuralChans; }
	/* static */ bool BugTask::isEMGChan(unsigned num) { return int(num) >= TotalNeuralChans && int(num) < TotalNeuralChans+TotalEMGChans; }
	/* static */ bool BugTask::isAuxChan(unsigned num) { return int(num) >= TotalNeuralChans+TotalEMGChans && int(num) < TotalNeuralChans+TotalEMGChans+TotalAuxChans; }
	/* static */ bool BugTask::isTTLChan(unsigned num) { return int(num) >= TotalNeuralChans+TotalEMGChans+TotalAuxChans && int(num) < TotalNeuralChans+TotalEMGChans+TotalAuxChans+TotalTTLChans; }
    /* static */ bool BugTask::isAIChan(const Params & p, unsigned num) { if (p.bug.graphBadData && p.nVAIChans==(num+1)) return false; return num < p.nVAIChans && int(p.nVAIChans-num) <= p.bug.aiChans.count()+(p.bug.graphBadData?1:0); }
    /* static */ bool BugTask::isMissingFCChan(const Params & p, unsigned num) { return p.bug.graphBadData && num < p.nVAIChans && int(p.nVAIChans-num) == 1; }

	
	/*-------------------- Framegrabber Task --------------------------------*/
	
    /* static */ const double FGTask::SamplingRate = /*100;*/ 26739.0; // 26.739kHz sampling rate!

    /* static */ const int FGTask::NumChans = 2304 /* 72 * 32 */;

    unsigned FGTask::numChans() const { return params.nVAIChans; }
    unsigned FGTask::samplingRate() const { return params.srate; }
	


    FGTask::FGTask(Params & ap, QObject *parent, const PagedScanReader &psr, bool isDummy)
        : SubprocessTask(ap, parent, "Framegrabber",
                         ap.fg.sidx == -1234 && ap.fg.ridx == -1234 ? "Fake_FG.exe" : "FG_SpikeGL.exe",
                         psr), lastScanTS(0), lastScanTSMut(QMutex::Recursive)
	{
        killAllInstancesOfProcessWithImageName(exeName);                

        dialogW = 0; dialog = 0;
        didImgSizeWarn = sentFGCmd = false;
        need2EmitFirstScan = true;
        if (!isDummy) {
            dialogW = new QDialog(0,Qt::CustomizeWindowHint|Qt::Dialog|Qt::WindowTitleHint);
            dialog = new Ui::FG_Controls;
            dialog->setupUi(dialogW);
            /*dialog->stopGrabBut->hide();
            Connect(dialog->calibAdcBut, SIGNAL(clicked()), this, SLOT(calibClicked()));
            Connect(dialog->setupRegsBut, SIGNAL(clicked()), this, SLOT(setupRegsClicked()));
            Connect(dialog->contAdcBut, SIGNAL(clicked()), this, SLOT(contAdcClicked()));
            Connect(dialog->grabFramesBut, SIGNAL(clicked()), this, SLOT(grabFramesClicked()));
            Connect(dialog->stopGrabBut, SIGNAL(clicked()), this, SLOT(stopGrabClicked()));*/
            Connect(this, SIGNAL(gotMsg(QString,QColor)), this, SLOT(appendTE(QString,QColor)));
            Connect(this, SIGNAL(gotClkSignals(int)), this, SLOT(updateClkSignals(int)));
            Connect(this, SIGNAL(gotFPS(int)), this, SLOT(updateFPS(int)));
            Connect(this, SIGNAL(justStarted()), this, SLOT(setSaperaDevice()));
            Connect(this, SIGNAL(justStarted()), this, SLOT(openComPort()));
            QTimer *t = new QTimer(dialogW);
            Connect(t, SIGNAL(timeout()), this, SLOT(do_updateTimestampLabel()));
            t->setSingleShot(false); t->start(250);
            dialogW->show();
            mainApp()->windowMenuAdd(dialogW);
            dialogW->setAttribute(Qt::WA_DeleteOnClose, false);
            dialogW->installEventFilter(mainApp());

            // now, after subprocess starts, inititate the "Grab Frames" action that would have been done in the GUI
            // as of Sept 8, 2016.. this happens automatically now.
            Connect(this, SIGNAL(justStarted()), this, SLOT(doGrabFrames()));
        }
    }

    FGTask::~FGTask() {
        if (isRunning()) { stop(); wait(); }
        if (numChans() > 0)
            Debug() << "FGTask `" << objectName() << "' deleted after processing " << totalRead/u64(numChans()) << " scans.";
        if (dialogW) mainApp()->windowMenuRemove(dialogW);
        if (dialog) delete dialog; dialog = 0;
        if (dialogW) delete dialogW; dialogW = 0;
    }

	QStringList FGTask::filesList() const 
	{
		QStringList files;
        files.push_back(QString(":/FG/FrameGrabber/FG_SpikeGL/x64/Release/FG_SpikeGL.exe"));
        files.push_back(QString(":/FG/FrameGrabber/Fake_FG.exe"));
		files.push_back(":/FG/FrameGrabber/J_2000+_Electrode_8tap_8bit.ccf");
        files.push_back(":/FG/FrameGrabber/SapClassBasic75.dll");
		return files;
	}
		
    void FGTask::sendExitCommand(QProcess & p) const
    {
        XtCmd xt;
        xt.init();
        xt.cmd = XtCmd_Exit;
        p.write((const char *)&xt, sizeof(xt));
    }

    void FGTask::openComPort()
    {
        const DAQ::Params & p(params);
        int parms[6];
        parms[0] = p.fg.com;
        parms[1] = p.fg.baud;
        parms[2] = p.fg.bits;
        parms[3] = p.fg.parity;
        parms[4] = p.fg.stop;
        parms[5] = 0;
        XtCmdOpenPort x;
        x.init(parms);
        pushCmd(x);
    }

    void FGTask::setSaperaDevice()
    {
        const DAQ::Params & p(params);
        XtCmdServerResource x;
        x.init("","",p.fg.sidx,p.fg.ridx,0,true);
        pushCmd(x);
    }

	unsigned FGTask::gotInput(const QByteArray & data, unsigned lastReadNBytes, QProcess & p) 
	{
        (void) p;
		(void) lastReadNBytes;
		unsigned consumed = 0;
		
		std::vector<const XtCmd *> cmds;
		cmds.reserve(16384);
		const XtCmd *xt = 0;
        int cons = 0;
		unsigned char *pdata = (unsigned char *)data.data();
		while ((xt = XtCmd::parseBuf(pdata+consumed, data.size()-consumed, cons))) {
			consumed += cons;
			cmds.push_back(xt);
		}
		for (std::vector<const XtCmd *>::iterator it = cmds.begin(); it != cmds.end(); ++it) {
			xt = *it;
            if (xt->cmd == XtCmd_Img) {
                Error() << "XtCmd_Img mechanism via stdout/stdin is no longer supported -- subprocess should be using shm mechanism!";
                p.kill();
                return 0;
            } else if (xt->cmd == XtCmd_ConsoleMessage) {
				XtCmdConsoleMsg *xm = (XtCmdConsoleMsg *)xt; 
                QString msg(xm->msg);
				msg = msg.trimmed();
                QColor c = QColor(Qt::black);
				switch (xm->msgType) {
					case XtCmdConsoleMsg::Error:
                        c = QColor(Qt::red);
						Error() << shortName << ": " << msg; 
                        emit(taskError(shortName + " slave process: " + msg));
						break;
					case XtCmdConsoleMsg::Warning:
                        c = QColor(Qt::magenta);
                        Warning() << shortName << ": " << msg; break;
					case XtCmdConsoleMsg::Debug:
                        c = QColor(Qt::blue);
                        Debug() << shortName << ": " << msg; break;
					default:
                        Log() << shortName << ": " << msg;
                        break;
				}
                emit gotMsg(msg,c);
            } else if (xt->cmd == XtCmd_ClkSignals) {
                emit(gotClkSignals(xt->param));
            } else if (xt->cmd == XtCmd_ServerResource) {
                XtCmdServerResource *xs = (XtCmdServerResource *)xt;
                Hardware h;
                h.serverName = xs->serverName;
                h.resourceName = xs->resourceName;
                h.serverIndex = xs->serverIndex;
                h.resourceIndex = xs->resourceIndex;
                h.serverType = xs->serverType;
                h.accessible = xs->accessible;
                probedHardware.push_back(h);
            } else if (xt->cmd == XtCmd_FPS) {
                emit (gotFPS(xt->param));
            } else {
				// todo.. handle other cmds coming in?
                Warning() << "Unrecognized command from FrameGrabber subprocess: " << (unsigned)xt->cmd;
			}
		}

		return consumed;
	}
	
    void FGTask::updateClkSignals(int param) {
        if (need2EmitFirstScan) {
            emit gotFirstScan();
            need2EmitFirstScan = false;
        }
        XtCmdClkSignals dummy; dummy.init(0,0,0,0,0); dummy.param = param;
        dialog->pxClk1Lbl->setText(dummy.isPxClk1() ? "<font color=green>+</font>" : "<font color=red>-</font>");
        dialog->pxClk2Lbl->setText(dummy.isPxClk2() ? "<font color=green>+</font>" : "<font color=red>-</font>");
        dialog->pxClk3Lbl->setText(dummy.isPxClk3() ? "<font color=green>+</font>" : "<font color=red>-</font>");
        dialog->hsyncLbl->setText(dummy.isHSync() ? "<font color=green>+</font>" : "<font color=red>-</font>");
        dialog->vsyncLbl->setText(dummy.isVSync() ? "<font color=green>+</font>" : "<font color=red>-</font>");
        unsigned long ctr = dialog->clkCtLbl->text().toULong();
        dialog->clkCtLbl->setText(QString::number(ctr+1UL));
        quint64 scanSkipCt = mainApp()->scanSkipCount(),
                xferCt = (mainApp()->scanCount()-scanSkipCt)/static_cast<quint64>(writer.scansPerPage());


        dialog->xferCtLbl->setText(QString::number(xferCt));
        dialog->scanSkipsLbl->setText(
                    QString( !scanSkipCt ?  "<font color=green>" : "<font color=red>")
                    + QString::number(scanSkipCt) + "</font>"
                    );
    }

    void FGTask::updateFPS(int fps) {
        dialog->fpsLbl->setText(QString::number(fps));
    }

    void FGTask::updateTimesampLabel(unsigned long long ts)
    {
        QMutexLocker l(&lastScanTSMut);
        lastScanTS = ts;
    }

    void FGTask::do_updateTimestampLabel() {
        unsigned long long ts = 0;
        char *pts = (char *)&ts;
        lastScanTSMut.lock();
        // swab bytes -- added 1/4/2017 as Jim Chen updated FPGA code to send us a timestamp as first 32 bits of line
        volatile char *p = (volatile char *)&lastScanTS;
        pts[0] = p[3]; pts[1] = p[2]; pts[2] = p[1]; pts[3] = p[0]; // weird format.. swab first 4 bytes
        pts[4] = p[7]; pts[5] = p[6]; pts[6] = p[5]; pts[7] = p[4];  // weird format.. swab last 4 bytes
        lastScanTSMut.unlock();
        QString txt = QString::number(ts);
        dialog->timestampLbl->setText(txt);
    }

	/* static */
    QString FGTask::getChannelName(unsigned num, const ChanMap *cmp)
	{ 
        QString chStr(QString("AD %1").arg(num));
        if (cmp && num < unsigned(cmp->size())) {
            const ChanMapDesc &desc(cmp->at(num));
            chStr.sprintf("%d [INTAN:%u CHAN:%u ELEC:%u]",num,desc.intan,desc.intanCh,desc.electrodeId);
        }
        return chStr;
	}
	
    void FGTask::pushCmd(const XtCmd * c)
    {
        if (!c) return;
        QByteArray b((char *)c, sizeof(*c) - sizeof(int) + c->len);
        SubprocessTask::pushCmd(b);
    }

    void FGTask::appendTE(const QString &s, const QColor &color)
    {
        QColor origColor = dialog->textEdit->textColor();
        dialog->textEdit->moveCursor(QTextCursor::End);
        dialog->textEdit->setTextColor(color);
        dialog->textEdit->append(s);
        dialog->textEdit->moveCursor(QTextCursor::End);
        dialog->textEdit->ensureCursorVisible();
        dialog->textEdit->setTextColor(origColor);
    }

    /*
    void FGTask::calibClicked() {
        appendTE("Calib clicked.", QColor(Qt::gray));
    }

    void FGTask::setupRegsClicked() {
        appendTE("Setup regs clicked.", QColor(Qt::gray));
    }

    void FGTask::contAdcClicked() {
        appendTE("Cont ADC clicked.", QColor(Qt::gray));
        XtCmdFPGAProto p;
        p.init(6,0,0);
        pushCmd(p);
    }
    */

    /* static */
    bool FGTask::setupCMFromArray(const int *mapping, int which /* 1=calin 0=janelia */, ChanMap *cm_out)
    {
        /*
                RAW FRAME LAYOUT is as follows... (from Channel Map.pdf exchanged in emails from Jim Chen

                                      chip 1                  chip 2                chip 3            ...   chip 36
                                     Lower32ch   Higer32ch   Lower32ch   Higer32ch  Lower32ch   Higer32ch   Lower32ch   Higer32ch
description       Frame Info        MSB   LSB   MSB   LSB   MSB  LSB   MSB   LSB   MSB   LSB   MSB   LSB   MSB   LSB   MSB   LSB
channel #1 & #33   64‐bit           8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit
channel #2 & #34   64‐bit           8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit
channel #3 & #35   64‐bit           8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit
channel #32 & #64  64‐bit           8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit 8‐bit
*/
            // setup SpikeGL-native chanmap
            static const int N_INTANS = 36, N_CHANS_PER_INTAN = 64;
            const int n_chans = which ? 2048 : 2304 ;
            int *revmap = (int *)calloc(sizeof(int),n_chans);
            for (int i = 0; i < n_chans; ++i) {
                int j = mapping[i];
                if (j < 0 || j >= n_chans) { Warning() << "setupCMFromArray: mapping array has invalid values!"; j = 0; return false; }
                revmap[j] = i;
            }
            ChanMap & cm(*cm_out);  cm.resize(n_chans);
            for (int i = 0, chinc = -1, even = 1; i < n_chans; ++i, even = !even) {
                if (!(i%(N_INTANS*2))) ++chinc;
                const int intan = (i/2) % N_INTANS, intan_chan = even ? chinc : chinc+(N_CHANS_PER_INTAN/2);
                int electrode = revmap[i];
                if (electrode < 0 || electrode >= n_chans) {
                    Warning() << "setupCMFromArray: electrode is out of range!";
                    electrode = 0;
                    return false;
                }
                if (intan_chan >= N_CHANS_PER_INTAN || intan_chan < 0) {
                    Warning() << "setupCMFromArray: intan_chan is out of range!";
                    return false;
                }
                ChanMapDesc & d(cm[i]);
                d.electrodeId = electrode;
                d.intan = intan;
                d.intanCh = intan_chan;
            }
            free(revmap);
            return true;
    }



    /* static */
    const int *FGTask::getDefaultMapping(int which /* 1=calin 0=janelia*/, ChanMap *cm_out /*= 0*/) {
        static int hardcoded1[] =       { 1, 65, 129, 193, 257, 321, 385, 449, 513, 577, 641, 705, 769, 833, 897, 961, 1025, 1089, 1153, 1217, 1281, 1345, 1409, 1473, 1537, 1601, 1665, 1729, 1793, 1857, 1921, 1985,
                                         2, 66, 130, 194, 258, 322, 386, 450, 514, 578, 642, 706, 770, 834, 898, 962, 1026, 1090, 1154, 1218, 1282, 1346, 1410, 1474, 1538, 1602, 1666, 1730, 1794, 1858, 1922, 1986,
                                         3, 67, 131, 195, 259, 323, 387, 451, 515, 579, 643, 707, 771, 835, 899, 963, 1027, 1091, 1155, 1219, 1283, 1347, 1411, 1475, 1539, 1603, 1667, 1731, 1795, 1859, 1923, 1987,
                                         4, 68, 132, 196, 260, 324, 388, 452, 516, 580, 644, 708, 772, 836, 900, 964, 1028, 1092, 1156, 1220, 1284, 1348, 1412, 1476, 1540, 1604, 1668, 1732, 1796, 1860, 1924, 1988,
                                         5, 69, 133, 197, 261, 325, 389, 453, 517, 581, 645, 709, 773, 837, 901, 965, 1029, 1093, 1157, 1221, 1285, 1349, 1413, 1477, 1541, 1605, 1669, 1733, 1797, 1861, 1925, 1989,
                                         6, 70, 134, 198, 262, 326, 390, 454, 518, 582, 646, 710, 774, 838, 902, 966, 1030, 1094, 1158, 1222, 1286, 1350, 1414, 1478, 1542, 1606, 1670, 1734, 1798, 1862, 1926, 1990,
                                         7, 71, 135, 199, 263, 327, 391, 455, 519, 583, 647, 711, 775, 839, 903, 967, 1031, 1095, 1159, 1223, 1287, 1351, 1415, 1479, 1543, 1607, 1671, 1735, 1799, 1863, 1927, 1991,
                                         8, 72, 136, 200, 264, 328, 392, 456, 520, 584, 648, 712, 776, 840, 904, 968, 1032, 1096, 1160, 1224, 1288, 1352, 1416, 1480, 1544, 1608, 1672, 1736, 1800, 1864, 1928, 1992,
                                         9, 73, 137, 201, 265, 329, 393, 457, 521, 585, 649, 713, 777, 841, 905, 969, 1033, 1097, 1161, 1225, 1289, 1353, 1417, 1481, 1545, 1609, 1673, 1737, 1801, 1865, 1929, 1993,
                                         10, 74, 138, 202, 266, 330, 394, 458, 522, 586, 650, 714, 778, 842, 906, 970, 1034, 1098, 1162, 1226, 1290, 1354, 1418, 1482, 1546, 1610, 1674, 1738, 1802, 1866, 1930, 1994,
                                         11, 75, 139, 203, 267, 331, 395, 459, 523, 587, 651, 715, 779, 843, 907, 971, 1035, 1099, 1163, 1227, 1291, 1355, 1419, 1483, 1547, 1611, 1675, 1739, 1803, 1867, 1931, 1995,
                                         12, 76, 140, 204, 268, 332, 396, 460, 524, 588, 652, 716, 780, 844, 908, 972, 1036, 1100, 1164, 1228, 1292, 1356, 1420, 1484, 1548, 1612, 1676, 1740, 1804, 1868, 1932, 1996,
                                         13, 77, 141, 205, 269, 333, 397, 461, 525, 589, 653, 717, 781, 845, 909, 973, 1037, 1101, 1165, 1229, 1293, 1357, 1421, 1485, 1549, 1613, 1677, 1741, 1805, 1869, 1933, 1997,
                                         14, 78, 142, 206, 270, 334, 398, 462, 526, 590, 654, 718, 782, 846, 910, 974, 1038, 1102, 1166, 1230, 1294, 1358, 1422, 1486, 1550, 1614, 1678, 1742, 1806, 1870, 1934, 1998,
                                         15, 79, 143, 207, 271, 335, 399, 463, 527, 591, 655, 719, 783, 847, 911, 975, 1039, 1103, 1167, 1231, 1295, 1359, 1423, 1487, 1551, 1615, 1679, 1743, 1807, 1871, 1935, 1999,
                                         16, 80, 144, 208, 272, 336, 400, 464, 528, 592, 656, 720, 784, 848, 912, 976, 1040, 1104, 1168, 1232, 1296, 1360, 1424, 1488, 1552, 1616, 1680, 1744, 1808, 1872, 1936, 2000,
                                         17, 81, 145, 209, 273, 337, 401, 465, 529, 593, 657, 721, 785, 849, 913, 977, 1041, 1105, 1169, 1233, 1297, 1361, 1425, 1489, 1553, 1617, 1681, 1745, 1809, 1873, 1937, 2001,
                                         18, 82, 146, 210, 274, 338, 402, 466, 530, 594, 658, 722, 786, 850, 914, 978, 1042, 1106, 1170, 1234, 1298, 1362, 1426, 1490, 1554, 1618, 1682, 1746, 1810, 1874, 1938, 2002,
                                         19, 83, 147, 211, 275, 339, 403, 467, 531, 595, 659, 723, 787, 851, 915, 979, 1043, 1107, 1171, 1235, 1299, 1363, 1427, 1491, 1555, 1619, 1683, 1747, 1811, 1875, 1939, 2003,
                                         20, 84, 148, 212, 276, 340, 404, 468, 532, 596, 660, 724, 788, 852, 916, 980, 1044, 1108, 1172, 1236, 1300, 1364, 1428, 1492, 1556, 1620, 1684, 1748, 1812, 1876, 1940, 2004,
                                         21, 85, 149, 213, 277, 341, 405, 469, 533, 597, 661, 725, 789, 853, 917, 981, 1045, 1109, 1173, 1237, 1301, 1365, 1429, 1493, 1557, 1621, 1685, 1749, 1813, 1877, 1941, 2005,
                                         22, 86, 150, 214, 278, 342, 406, 470, 534, 598, 662, 726, 790, 854, 918, 982, 1046, 1110, 1174, 1238, 1302, 1366, 1430, 1494, 1558, 1622, 1686, 1750, 1814, 1878, 1942, 2006,
                                         23, 87, 151, 215, 279, 343, 407, 471, 535, 599, 663, 727, 791, 855, 919, 983, 1047, 1111, 1175, 1239, 1303, 1367, 1431, 1495, 1559, 1623, 1687, 1751, 1815, 1879, 1943, 2007,
                                         24, 88, 152, 216, 280, 344, 408, 472, 536, 600, 664, 728, 792, 856, 920, 984, 1048, 1112, 1176, 1240, 1304, 1368, 1432, 1496, 1560, 1624, 1688, 1752, 1816, 1880, 1944, 2008,
                                         25, 89, 153, 217, 281, 345, 409, 473, 537, 601, 665, 729, 793, 857, 921, 985, 1049, 1113, 1177, 1241, 1305, 1369, 1433, 1497, 1561, 1625, 1689, 1753, 1817, 1881, 1945, 2009,
                                         26, 90, 154, 218, 282, 346, 410, 474, 538, 602, 666, 730, 794, 858, 922, 986, 1050, 1114, 1178, 1242, 1306, 1370, 1434, 1498, 1562, 1626, 1690, 1754, 1818, 1882, 1946, 2010,
                                         27, 91, 155, 219, 283, 347, 411, 475, 539, 603, 667, 731, 795, 859, 923, 987, 1051, 1115, 1179, 1243, 1307, 1371, 1435, 1499, 1563, 1627, 1691, 1755, 1819, 1883, 1947, 2011,
                                         28, 92, 156, 220, 284, 348, 412, 476, 540, 604, 668, 732, 796, 860, 924, 988, 1052, 1116, 1180, 1244, 1308, 1372, 1436, 1500, 1564, 1628, 1692, 1756, 1820, 1884, 1948, 2012,
                                         29, 93, 157, 221, 285, 349, 413, 477, 541, 605, 669, 733, 797, 861, 925, 989, 1053, 1117, 1181, 1245, 1309, 1373, 1437, 1501, 1565, 1629, 1693, 1757, 1821, 1885, 1949, 2013,
                                         30, 94, 158, 222, 286, 350, 414, 478, 542, 606, 670, 734, 798, 862, 926, 990, 1054, 1118, 1182, 1246, 1310, 1374, 1438, 1502, 1566, 1630, 1694, 1758, 1822, 1886, 1950, 2014,
                                         31, 95, 159, 223, 287, 351, 415, 479, 543, 607, 671, 735, 799, 863, 927, 991, 1055, 1119, 1183, 1247, 1311, 1375, 1439, 1503, 1567, 1631, 1695, 1759, 1823, 1887, 1951, 2015,
                                         32, 96, 160, 224, 288, 352, 416, 480, 544, 608, 672, 736, 800, 864, 928, 992, 1056, 1120, 1184, 1248, 1312, 1376, 1440, 1504, 1568, 1632, 1696, 1760, 1824, 1888, 1952, 2016,
                                         33, 97, 161, 225, 289, 353, 417, 481, 545, 609, 673, 737, 801, 865, 929, 993, 1057, 1121, 1185, 1249, 1313, 1377, 1441, 1505, 1569, 1633, 1697, 1761, 1825, 1889, 1953, 2017,
                                         34, 98, 162, 226, 290, 354, 418, 482, 546, 610, 674, 738, 802, 866, 930, 994, 1058, 1122, 1186, 1250, 1314, 1378, 1442, 1506, 1570, 1634, 1698, 1762, 1826, 1890, 1954, 2018,
                                         35, 99, 163, 227, 291, 355, 419, 483, 547, 611, 675, 739, 803, 867, 931, 995, 1059, 1123, 1187, 1251, 1315, 1379, 1443, 1507, 1571, 1635, 1699, 1763, 1827, 1891, 1955, 2019,
                                         36, 100, 164, 228, 292, 356, 420, 484, 548, 612, 676, 740, 804, 868, 932, 996, 1060, 1124, 1188, 1252, 1316, 1380, 1444, 1508, 1572, 1636, 1700, 1764, 1828, 1892, 1956, 2020,
                                         37, 101, 165, 229, 293, 357, 421, 485, 549, 613, 677, 741, 805, 869, 933, 997, 1061, 1125, 1189, 1253, 1317, 1381, 1445, 1509, 1573, 1637, 1701, 1765, 1829, 1893, 1957, 2021,
                                         38, 102, 166, 230, 294, 358, 422, 486, 550, 614, 678, 742, 806, 870, 934, 998, 1062, 1126, 1190, 1254, 1318, 1382, 1446, 1510, 1574, 1638, 1702, 1766, 1830, 1894, 1958, 2022,
                                         39, 103, 167, 231, 295, 359, 423, 487, 551, 615, 679, 743, 807, 871, 935, 999, 1063, 1127, 1191, 1255, 1319, 1383, 1447, 1511, 1575, 1639, 1703, 1767, 1831, 1895, 1959, 2023,
                                         40, 104, 168, 232, 296, 360, 424, 488, 552, 616, 680, 744, 808, 872, 936, 1000, 1064, 1128, 1192, 1256, 1320, 1384, 1448, 1512, 1576, 1640, 1704, 1768, 1832, 1896, 1960, 2024,
                                         41, 105, 169, 233, 297, 361, 425, 489, 553, 617, 681, 745, 809, 873, 937, 1001, 1065, 1129, 1193, 1257, 1321, 1385, 1449, 1513, 1577, 1641, 1705, 1769, 1833, 1897, 1961, 2025,
                                         42, 106, 170, 234, 298, 362, 426, 490, 554, 618, 682, 746, 810, 874, 938, 1002, 1066, 1130, 1194, 1258, 1322, 1386, 1450, 1514, 1578, 1642, 1706, 1770, 1834, 1898, 1962, 2026,
                                         43, 107, 171, 235, 299, 363, 427, 491, 555, 619, 683, 747, 811, 875, 939, 1003, 1067, 1131, 1195, 1259, 1323, 1387, 1451, 1515, 1579, 1643, 1707, 1771, 1835, 1899, 1963, 2027,
                                         44, 108, 172, 236, 300, 364, 428, 492, 556, 620, 684, 748, 812, 876, 940, 1004, 1068, 1132, 1196, 1260, 1324, 1388, 1452, 1516, 1580, 1644, 1708, 1772, 1836, 1900, 1964, 2028,
                                         45, 109, 173, 237, 301, 365, 429, 493, 557, 621, 685, 749, 813, 877, 941, 1005, 1069, 1133, 1197, 1261, 1325, 1389, 1453, 1517, 1581, 1645, 1709, 1773, 1837, 1901, 1965, 2029,
                                         46, 110, 174, 238, 302, 366, 430, 494, 558, 622, 686, 750, 814, 878, 942, 1006, 1070, 1134, 1198, 1262, 1326, 1390, 1454, 1518, 1582, 1646, 1710, 1774, 1838, 1902, 1966, 2030,
                                         47, 111, 175, 239, 303, 367, 431, 495, 559, 623, 687, 751, 815, 879, 943, 1007, 1071, 1135, 1199, 1263, 1327, 1391, 1455, 1519, 1583, 1647, 1711, 1775, 1839, 1903, 1967, 2031,
                                         48, 112, 176, 240, 304, 368, 432, 496, 560, 624, 688, 752, 816, 880, 944, 1008, 1072, 1136, 1200, 1264, 1328, 1392, 1456, 1520, 1584, 1648, 1712, 1776, 1840, 1904, 1968, 2032,
                                         49, 113, 177, 241, 305, 369, 433, 497, 561, 625, 689, 753, 817, 881, 945, 1009, 1073, 1137, 1201, 1265, 1329, 1393, 1457, 1521, 1585, 1649, 1713, 1777, 1841, 1905, 1969, 2033,
                                         50, 114, 178, 242, 306, 370, 434, 498, 562, 626, 690, 754, 818, 882, 946, 1010, 1074, 1138, 1202, 1266, 1330, 1394, 1458, 1522, 1586, 1650, 1714, 1778, 1842, 1906, 1970, 2034,
                                         51, 115, 179, 243, 307, 371, 435, 499, 563, 627, 691, 755, 819, 883, 947, 1011, 1075, 1139, 1203, 1267, 1331, 1395, 1459, 1523, 1587, 1651, 1715, 1779, 1843, 1907, 1971, 2035,
                                         52, 116, 180, 244, 308, 372, 436, 500, 564, 628, 692, 756, 820, 884, 948, 1012, 1076, 1140, 1204, 1268, 1332, 1396, 1460, 1524, 1588, 1652, 1716, 1780, 1844, 1908, 1972, 2036,
                                         53, 117, 181, 245, 309, 373, 437, 501, 565, 629, 693, 757, 821, 885, 949, 1013, 1077, 1141, 1205, 1269, 1333, 1397, 1461, 1525, 1589, 1653, 1717, 1781, 1845, 1909, 1973, 2037,
                                         54, 118, 182, 246, 310, 374, 438, 502, 566, 630, 694, 758, 822, 886, 950, 1014, 1078, 1142, 1206, 1270, 1334, 1398, 1462, 1526, 1590, 1654, 1718, 1782, 1846, 1910, 1974, 2038,
                                         55, 119, 183, 247, 311, 375, 439, 503, 567, 631, 695, 759, 823, 887, 951, 1015, 1079, 1143, 1207, 1271, 1335, 1399, 1463, 1527, 1591, 1655, 1719, 1783, 1847, 1911, 1975, 2039,
                                         56, 120, 184, 248, 312, 376, 440, 504, 568, 632, 696, 760, 824, 888, 952, 1016, 1080, 1144, 1208, 1272, 1336, 1400, 1464, 1528, 1592, 1656, 1720, 1784, 1848, 1912, 1976, 2040,
                                         57, 121, 185, 249, 313, 377, 441, 505, 569, 633, 697, 761, 825, 889, 953, 1017, 1081, 1145, 1209, 1273, 1337, 1401, 1465, 1529, 1593, 1657, 1721, 1785, 1849, 1913, 1977, 2041,
                                         58, 122, 186, 250, 314, 378, 442, 506, 570, 634, 698, 762, 826, 890, 954, 1018, 1082, 1146, 1210, 1274, 1338, 1402, 1466, 1530, 1594, 1658, 1722, 1786, 1850, 1914, 1978, 2042,
                                         59, 123, 187, 251, 315, 379, 443, 507, 571, 635, 699, 763, 827, 891, 955, 1019, 1083, 1147, 1211, 1275, 1339, 1403, 1467, 1531, 1595, 1659, 1723, 1787, 1851, 1915, 1979, 2043,
                                         60, 124, 188, 252, 316, 380, 444, 508, 572, 636, 700, 764, 828, 892, 956, 1020, 1084, 1148, 1212, 1276, 1340, 1404, 1468, 1532, 1596, 1660, 1724, 1788, 1852, 1916, 1980, 2044,
                                         61, 125, 189, 253, 317, 381, 445, 509, 573, 637, 701, 765, 829, 893, 957, 1021, 1085, 1149, 1213, 1277, 1341, 1405, 1469, 1533, 1597, 1661, 1725, 1789, 1853, 1917, 1981, 2045,
                                         62, 126, 190, 254, 318, 382, 446, 510, 574, 638, 702, 766, 830, 894, 958, 1022, 1086, 1150, 1214, 1278, 1342, 1406, 1470, 1534, 1598, 1662, 1726, 1790, 1854, 1918, 1982, 2046,
                                         63, 127, 191, 255, 319, 383, 447, 511, 575, 639, 703, 767, 831, 895, 959, 1023, 1087, 1151, 1215, 1279, 1343, 1407, 1471, 1535, 1599, 1663, 1727, 1791, 1855, 1919, 1983, 2047,
                                         64, 128, 192, 256, 320, 384, 448, 512, 576, 640, 704, 768, 832, 896, 960, 1024, 1088, 1152, 1216, 1280, 1344, 1408, 1472, 1536, 1600, 1664, 1728, 1792, 1856, 1920, 1984, 2048,
                                       };
        static int hardcoded2[] =       { 1, 65, 129, 193, 257, 321, 385, 449, 513, 577, 641, 705, 769, 833, 897, 961, 1025, 1089, 1153, 1217, 1281, 1345, 1409, 1473, 1537, 1601, 1665, 1729, 1793, 1857, 1921, 1985, 2049, 2113, 2177, 2241,
                                         2, 66, 130, 194, 258, 322, 386, 450, 514, 578, 642, 706, 770, 834, 898, 962, 1026, 1090, 1154, 1218, 1282, 1346, 1410, 1474, 1538, 1602, 1666, 1730, 1794, 1858, 1922, 1986, 2050, 2114, 2178, 2242,
                                         3, 67, 131, 195, 259, 323, 387, 451, 515, 579, 643, 707, 771, 835, 899, 963, 1027, 1091, 1155, 1219, 1283, 1347, 1411, 1475, 1539, 1603, 1667, 1731, 1795, 1859, 1923, 1987, 2051, 2115, 2179, 2243,
                                         4, 68, 132, 196, 260, 324, 388, 452, 516, 580, 644, 708, 772, 836, 900, 964, 1028, 1092, 1156, 1220, 1284, 1348, 1412, 1476, 1540, 1604, 1668, 1732, 1796, 1860, 1924, 1988, 2052, 2116, 2180, 2244,
                                         5, 69, 133, 197, 261, 325, 389, 453, 517, 581, 645, 709, 773, 837, 901, 965, 1029, 1093, 1157, 1221, 1285, 1349, 1413, 1477, 1541, 1605, 1669, 1733, 1797, 1861, 1925, 1989, 2053, 2117, 2181, 2245,
                                         6, 70, 134, 198, 262, 326, 390, 454, 518, 582, 646, 710, 774, 838, 902, 966, 1030, 1094, 1158, 1222, 1286, 1350, 1414, 1478, 1542, 1606, 1670, 1734, 1798, 1862, 1926, 1990, 2054, 2118, 2182, 2246,
                                         7, 71, 135, 199, 263, 327, 391, 455, 519, 583, 647, 711, 775, 839, 903, 967, 1031, 1095, 1159, 1223, 1287, 1351, 1415, 1479, 1543, 1607, 1671, 1735, 1799, 1863, 1927, 1991, 2055, 2119, 2183, 2247,
                                         8, 72, 136, 200, 264, 328, 392, 456, 520, 584, 648, 712, 776, 840, 904, 968, 1032, 1096, 1160, 1224, 1288, 1352, 1416, 1480, 1544, 1608, 1672, 1736, 1800, 1864, 1928, 1992, 2056, 2120, 2184, 2248,
                                         9, 73, 137, 201, 265, 329, 393, 457, 521, 585, 649, 713, 777, 841, 905, 969, 1033, 1097, 1161, 1225, 1289, 1353, 1417, 1481, 1545, 1609, 1673, 1737, 1801, 1865, 1929, 1993, 2057, 2121, 2185, 2249,
                                         10, 74, 138, 202, 266, 330, 394, 458, 522, 586, 650, 714, 778, 842, 906, 970, 1034, 1098, 1162, 1226, 1290, 1354, 1418, 1482, 1546, 1610, 1674, 1738, 1802, 1866, 1930, 1994, 2058, 2122, 2186, 2250,
                                         11, 75, 139, 203, 267, 331, 395, 459, 523, 587, 651, 715, 779, 843, 907, 971, 1035, 1099, 1163, 1227, 1291, 1355, 1419, 1483, 1547, 1611, 1675, 1739, 1803, 1867, 1931, 1995, 2059, 2123, 2187, 2251,
                                         12, 76, 140, 204, 268, 332, 396, 460, 524, 588, 652, 716, 780, 844, 908, 972, 1036, 1100, 1164, 1228, 1292, 1356, 1420, 1484, 1548, 1612, 1676, 1740, 1804, 1868, 1932, 1996, 2060, 2124, 2188, 2252,
                                         13, 77, 141, 205, 269, 333, 397, 461, 525, 589, 653, 717, 781, 845, 909, 973, 1037, 1101, 1165, 1229, 1293, 1357, 1421, 1485, 1549, 1613, 1677, 1741, 1805, 1869, 1933, 1997, 2061, 2125, 2189, 2253,
                                         14, 78, 142, 206, 270, 334, 398, 462, 526, 590, 654, 718, 782, 846, 910, 974, 1038, 1102, 1166, 1230, 1294, 1358, 1422, 1486, 1550, 1614, 1678, 1742, 1806, 1870, 1934, 1998, 2062, 2126, 2190, 2254,
                                         15, 79, 143, 207, 271, 335, 399, 463, 527, 591, 655, 719, 783, 847, 911, 975, 1039, 1103, 1167, 1231, 1295, 1359, 1423, 1487, 1551, 1615, 1679, 1743, 1807, 1871, 1935, 1999, 2063, 2127, 2191, 2255,
                                         16, 80, 144, 208, 272, 336, 400, 464, 528, 592, 656, 720, 784, 848, 912, 976, 1040, 1104, 1168, 1232, 1296, 1360, 1424, 1488, 1552, 1616, 1680, 1744, 1808, 1872, 1936, 2000, 2064, 2128, 2192, 2256,
                                         17, 81, 145, 209, 273, 337, 401, 465, 529, 593, 657, 721, 785, 849, 913, 977, 1041, 1105, 1169, 1233, 1297, 1361, 1425, 1489, 1553, 1617, 1681, 1745, 1809, 1873, 1937, 2001, 2065, 2129, 2193, 2257,
                                         18, 82, 146, 210, 274, 338, 402, 466, 530, 594, 658, 722, 786, 850, 914, 978, 1042, 1106, 1170, 1234, 1298, 1362, 1426, 1490, 1554, 1618, 1682, 1746, 1810, 1874, 1938, 2002, 2066, 2130, 2194, 2258,
                                         19, 83, 147, 211, 275, 339, 403, 467, 531, 595, 659, 723, 787, 851, 915, 979, 1043, 1107, 1171, 1235, 1299, 1363, 1427, 1491, 1555, 1619, 1683, 1747, 1811, 1875, 1939, 2003, 2067, 2131, 2195, 2259,
                                         20, 84, 148, 212, 276, 340, 404, 468, 532, 596, 660, 724, 788, 852, 916, 980, 1044, 1108, 1172, 1236, 1300, 1364, 1428, 1492, 1556, 1620, 1684, 1748, 1812, 1876, 1940, 2004, 2068, 2132, 2196, 2260,
                                         21, 85, 149, 213, 277, 341, 405, 469, 533, 597, 661, 725, 789, 853, 917, 981, 1045, 1109, 1173, 1237, 1301, 1365, 1429, 1493, 1557, 1621, 1685, 1749, 1813, 1877, 1941, 2005, 2069, 2133, 2197, 2261,
                                         22, 86, 150, 214, 278, 342, 406, 470, 534, 598, 662, 726, 790, 854, 918, 982, 1046, 1110, 1174, 1238, 1302, 1366, 1430, 1494, 1558, 1622, 1686, 1750, 1814, 1878, 1942, 2006, 2070, 2134, 2198, 2262,
                                         23, 87, 151, 215, 279, 343, 407, 471, 535, 599, 663, 727, 791, 855, 919, 983, 1047, 1111, 1175, 1239, 1303, 1367, 1431, 1495, 1559, 1623, 1687, 1751, 1815, 1879, 1943, 2007, 2071, 2135, 2199, 2263,
                                         24, 88, 152, 216, 280, 344, 408, 472, 536, 600, 664, 728, 792, 856, 920, 984, 1048, 1112, 1176, 1240, 1304, 1368, 1432, 1496, 1560, 1624, 1688, 1752, 1816, 1880, 1944, 2008, 2072, 2136, 2200, 2264,
                                         25, 89, 153, 217, 281, 345, 409, 473, 537, 601, 665, 729, 793, 857, 921, 985, 1049, 1113, 1177, 1241, 1305, 1369, 1433, 1497, 1561, 1625, 1689, 1753, 1817, 1881, 1945, 2009, 2073, 2137, 2201, 2265,
                                         26, 90, 154, 218, 282, 346, 410, 474, 538, 602, 666, 730, 794, 858, 922, 986, 1050, 1114, 1178, 1242, 1306, 1370, 1434, 1498, 1562, 1626, 1690, 1754, 1818, 1882, 1946, 2010, 2074, 2138, 2202, 2266,
                                         27, 91, 155, 219, 283, 347, 411, 475, 539, 603, 667, 731, 795, 859, 923, 987, 1051, 1115, 1179, 1243, 1307, 1371, 1435, 1499, 1563, 1627, 1691, 1755, 1819, 1883, 1947, 2011, 2075, 2139, 2203, 2267,
                                         28, 92, 156, 220, 284, 348, 412, 476, 540, 604, 668, 732, 796, 860, 924, 988, 1052, 1116, 1180, 1244, 1308, 1372, 1436, 1500, 1564, 1628, 1692, 1756, 1820, 1884, 1948, 2012, 2076, 2140, 2204, 2268,
                                         29, 93, 157, 221, 285, 349, 413, 477, 541, 605, 669, 733, 797, 861, 925, 989, 1053, 1117, 1181, 1245, 1309, 1373, 1437, 1501, 1565, 1629, 1693, 1757, 1821, 1885, 1949, 2013, 2077, 2141, 2205, 2269,
                                         30, 94, 158, 222, 286, 350, 414, 478, 542, 606, 670, 734, 798, 862, 926, 990, 1054, 1118, 1182, 1246, 1310, 1374, 1438, 1502, 1566, 1630, 1694, 1758, 1822, 1886, 1950, 2014, 2078, 2142, 2206, 2270,
                                         31, 95, 159, 223, 287, 351, 415, 479, 543, 607, 671, 735, 799, 863, 927, 991, 1055, 1119, 1183, 1247, 1311, 1375, 1439, 1503, 1567, 1631, 1695, 1759, 1823, 1887, 1951, 2015, 2079, 2143, 2207, 2271,
                                         32, 96, 160, 224, 288, 352, 416, 480, 544, 608, 672, 736, 800, 864, 928, 992, 1056, 1120, 1184, 1248, 1312, 1376, 1440, 1504, 1568, 1632, 1696, 1760, 1824, 1888, 1952, 2016, 2080, 2144, 2208, 2272,
                                         33, 97, 161, 225, 289, 353, 417, 481, 545, 609, 673, 737, 801, 865, 929, 993, 1057, 1121, 1185, 1249, 1313, 1377, 1441, 1505, 1569, 1633, 1697, 1761, 1825, 1889, 1953, 2017, 2081, 2145, 2209, 2273,
                                         34, 98, 162, 226, 290, 354, 418, 482, 546, 610, 674, 738, 802, 866, 930, 994, 1058, 1122, 1186, 1250, 1314, 1378, 1442, 1506, 1570, 1634, 1698, 1762, 1826, 1890, 1954, 2018, 2082, 2146, 2210, 2274,
                                         35, 99, 163, 227, 291, 355, 419, 483, 547, 611, 675, 739, 803, 867, 931, 995, 1059, 1123, 1187, 1251, 1315, 1379, 1443, 1507, 1571, 1635, 1699, 1763, 1827, 1891, 1955, 2019, 2083, 2147, 2211, 2275,
                                         36, 100, 164, 228, 292, 356, 420, 484, 548, 612, 676, 740, 804, 868, 932, 996, 1060, 1124, 1188, 1252, 1316, 1380, 1444, 1508, 1572, 1636, 1700, 1764, 1828, 1892, 1956, 2020, 2084, 2148, 2212, 2276,
                                         37, 101, 165, 229, 293, 357, 421, 485, 549, 613, 677, 741, 805, 869, 933, 997, 1061, 1125, 1189, 1253, 1317, 1381, 1445, 1509, 1573, 1637, 1701, 1765, 1829, 1893, 1957, 2021, 2085, 2149, 2213, 2277,
                                         38, 102, 166, 230, 294, 358, 422, 486, 550, 614, 678, 742, 806, 870, 934, 998, 1062, 1126, 1190, 1254, 1318, 1382, 1446, 1510, 1574, 1638, 1702, 1766, 1830, 1894, 1958, 2022, 2086, 2150, 2214, 2278,
                                         39, 103, 167, 231, 295, 359, 423, 487, 551, 615, 679, 743, 807, 871, 935, 999, 1063, 1127, 1191, 1255, 1319, 1383, 1447, 1511, 1575, 1639, 1703, 1767, 1831, 1895, 1959, 2023, 2087, 2151, 2215, 2279,
                                         40, 104, 168, 232, 296, 360, 424, 488, 552, 616, 680, 744, 808, 872, 936, 1000, 1064, 1128, 1192, 1256, 1320, 1384, 1448, 1512, 1576, 1640, 1704, 1768, 1832, 1896, 1960, 2024, 2088, 2152, 2216, 2280,
                                         41, 105, 169, 233, 297, 361, 425, 489, 553, 617, 681, 745, 809, 873, 937, 1001, 1065, 1129, 1193, 1257, 1321, 1385, 1449, 1513, 1577, 1641, 1705, 1769, 1833, 1897, 1961, 2025, 2089, 2153, 2217, 2281,
                                         42, 106, 170, 234, 298, 362, 426, 490, 554, 618, 682, 746, 810, 874, 938, 1002, 1066, 1130, 1194, 1258, 1322, 1386, 1450, 1514, 1578, 1642, 1706, 1770, 1834, 1898, 1962, 2026, 2090, 2154, 2218, 2282,
                                         43, 107, 171, 235, 299, 363, 427, 491, 555, 619, 683, 747, 811, 875, 939, 1003, 1067, 1131, 1195, 1259, 1323, 1387, 1451, 1515, 1579, 1643, 1707, 1771, 1835, 1899, 1963, 2027, 2091, 2155, 2219, 2283,
                                         44, 108, 172, 236, 300, 364, 428, 492, 556, 620, 684, 748, 812, 876, 940, 1004, 1068, 1132, 1196, 1260, 1324, 1388, 1452, 1516, 1580, 1644, 1708, 1772, 1836, 1900, 1964, 2028, 2092, 2156, 2220, 2284,
                                         45, 109, 173, 237, 301, 365, 429, 493, 557, 621, 685, 749, 813, 877, 941, 1005, 1069, 1133, 1197, 1261, 1325, 1389, 1453, 1517, 1581, 1645, 1709, 1773, 1837, 1901, 1965, 2029, 2093, 2157, 2221, 2285,
                                         46, 110, 174, 238, 302, 366, 430, 494, 558, 622, 686, 750, 814, 878, 942, 1006, 1070, 1134, 1198, 1262, 1326, 1390, 1454, 1518, 1582, 1646, 1710, 1774, 1838, 1902, 1966, 2030, 2094, 2158, 2222, 2286,
                                         47, 111, 175, 239, 303, 367, 431, 495, 559, 623, 687, 751, 815, 879, 943, 1007, 1071, 1135, 1199, 1263, 1327, 1391, 1455, 1519, 1583, 1647, 1711, 1775, 1839, 1903, 1967, 2031, 2095, 2159, 2223, 2287,
                                         48, 112, 176, 240, 304, 368, 432, 496, 560, 624, 688, 752, 816, 880, 944, 1008, 1072, 1136, 1200, 1264, 1328, 1392, 1456, 1520, 1584, 1648, 1712, 1776, 1840, 1904, 1968, 2032, 2096, 2160, 2224, 2288,
                                         49, 113, 177, 241, 305, 369, 433, 497, 561, 625, 689, 753, 817, 881, 945, 1009, 1073, 1137, 1201, 1265, 1329, 1393, 1457, 1521, 1585, 1649, 1713, 1777, 1841, 1905, 1969, 2033, 2097, 2161, 2225, 2289,
                                         50, 114, 178, 242, 306, 370, 434, 498, 562, 626, 690, 754, 818, 882, 946, 1010, 1074, 1138, 1202, 1266, 1330, 1394, 1458, 1522, 1586, 1650, 1714, 1778, 1842, 1906, 1970, 2034, 2098, 2162, 2226, 2290,
                                         51, 115, 179, 243, 307, 371, 435, 499, 563, 627, 691, 755, 819, 883, 947, 1011, 1075, 1139, 1203, 1267, 1331, 1395, 1459, 1523, 1587, 1651, 1715, 1779, 1843, 1907, 1971, 2035, 2099, 2163, 2227, 2291,
                                         52, 116, 180, 244, 308, 372, 436, 500, 564, 628, 692, 756, 820, 884, 948, 1012, 1076, 1140, 1204, 1268, 1332, 1396, 1460, 1524, 1588, 1652, 1716, 1780, 1844, 1908, 1972, 2036, 2100, 2164, 2228, 2292,
                                         53, 117, 181, 245, 309, 373, 437, 501, 565, 629, 693, 757, 821, 885, 949, 1013, 1077, 1141, 1205, 1269, 1333, 1397, 1461, 1525, 1589, 1653, 1717, 1781, 1845, 1909, 1973, 2037, 2101, 2165, 2229, 2293,
                                         54, 118, 182, 246, 310, 374, 438, 502, 566, 630, 694, 758, 822, 886, 950, 1014, 1078, 1142, 1206, 1270, 1334, 1398, 1462, 1526, 1590, 1654, 1718, 1782, 1846, 1910, 1974, 2038, 2102, 2166, 2230, 2294,
                                         55, 119, 183, 247, 311, 375, 439, 503, 567, 631, 695, 759, 823, 887, 951, 1015, 1079, 1143, 1207, 1271, 1335, 1399, 1463, 1527, 1591, 1655, 1719, 1783, 1847, 1911, 1975, 2039, 2103, 2167, 2231, 2295,
                                         56, 120, 184, 248, 312, 376, 440, 504, 568, 632, 696, 760, 824, 888, 952, 1016, 1080, 1144, 1208, 1272, 1336, 1400, 1464, 1528, 1592, 1656, 1720, 1784, 1848, 1912, 1976, 2040, 2104, 2168, 2232, 2296,
                                         57, 121, 185, 249, 313, 377, 441, 505, 569, 633, 697, 761, 825, 889, 953, 1017, 1081, 1145, 1209, 1273, 1337, 1401, 1465, 1529, 1593, 1657, 1721, 1785, 1849, 1913, 1977, 2041, 2105, 2169, 2233, 2297,
                                         58, 122, 186, 250, 314, 378, 442, 506, 570, 634, 698, 762, 826, 890, 954, 1018, 1082, 1146, 1210, 1274, 1338, 1402, 1466, 1530, 1594, 1658, 1722, 1786, 1850, 1914, 1978, 2042, 2106, 2170, 2234, 2298,
                                         59, 123, 187, 251, 315, 379, 443, 507, 571, 635, 699, 763, 827, 891, 955, 1019, 1083, 1147, 1211, 1275, 1339, 1403, 1467, 1531, 1595, 1659, 1723, 1787, 1851, 1915, 1979, 2043, 2107, 2171, 2235, 2299,
                                         60, 124, 188, 252, 316, 380, 444, 508, 572, 636, 700, 764, 828, 892, 956, 1020, 1084, 1148, 1212, 1276, 1340, 1404, 1468, 1532, 1596, 1660, 1724, 1788, 1852, 1916, 1980, 2044, 2108, 2172, 2236, 2300,
                                         61, 125, 189, 253, 317, 381, 445, 509, 573, 637, 701, 765, 829, 893, 957, 1021, 1085, 1149, 1213, 1277, 1341, 1405, 1469, 1533, 1597, 1661, 1725, 1789, 1853, 1917, 1981, 2045, 2109, 2173, 2237, 2301,
                                         62, 126, 190, 254, 318, 382, 446, 510, 574, 638, 702, 766, 830, 894, 958, 1022, 1086, 1150, 1214, 1278, 1342, 1406, 1470, 1534, 1598, 1662, 1726, 1790, 1854, 1918, 1982, 2046, 2110, 2174, 2238, 2302,
                                         63, 127, 191, 255, 319, 383, 447, 511, 575, 639, 703, 767, 831, 895, 959, 1023, 1087, 1151, 1215, 1279, 1343, 1407, 1471, 1535, 1599, 1663, 1727, 1791, 1855, 1919, 1983, 2047, 2111, 2175, 2239, 2303,
                                         64, 128, 192, 256, 320, 384, 448, 512, 576, 640, 704, 768, 832, 896, 960, 1024, 1088, 1152, 1216, 1280, 1344, 1408, 1472, 1536, 1600, 1664, 1728, 1792, 1856, 1920, 1984, 2048, 2112, 2176, 2240, 2304
                                       };        

        static bool didDec = false;
        if (!didDec) {
            int n = sizeof(hardcoded1) / sizeof(int);
            for (int i = 0; i < n; ++i) --hardcoded1[i];
            n = sizeof(hardcoded2) / sizeof(int);
            for (int i = 0; i < n; ++i) --hardcoded2[i];
            didDec = true;
        }
        const int *retarray = which ? hardcoded1 : hardcoded2;
        if (cm_out)
            setupCMFromArray(retarray, which, cm_out);
        return retarray;
    }

    void FGTask::doGrabFrames() {
        appendTE("Grab frames initiated.", QColor(Qt::gray));
        //dialog->grabFramesBut->hide();
        //dialog->stopGrabBut->show();

        // as per Jim's email 2/18/2016
        XtCmdFPGAProto f;
        f.init(7,0,0);
        pushCmd(f);



        // grab frames.. does stuff with Sapera API in the slave process
        XtCmdGrabFrames x;
        x.init(SAMPLES_SHM_NAME, writer.totalSize(), writer.pageSize(), writer.metaDataSizeBytes(), "J_2000+_Electrode_8tap_8bit.ccf", 144, 32, NumChans, 0/*getDefaultMapping(0)*/);
        pushCmd(x);
    }

    /*
    void FGTask::stopGrabClicked() {
        appendTE("Stop grab clicked.", QColor(Qt::gray));
        dialog->grabFramesBut->show();
        dialog->stopGrabBut->hide();

        // as per Jim's email 2/18/2016
        XtCmdFPGAProto f;
        f.init(8,0,0);
        pushCmd(f);
    }
    */
    /* static */
    QList<FGTask::Hardware> FGTask::probedHardware;

    /* static */
    double FGTask::last_hw_probe_ts = 0.0;

    /* static */
    void FGTask::probeHardware() {
        double t0 = getTime();
#ifdef Q_OS_WINDOWS
        DAQ::Params dummy;
        PagedScanReader dummy2(0,0,0,0,0);
        FGTask task(dummy,0,dummy2,true);
        if (!task.platformSupported())  return;

        QString err;
        if (!task.setupExeDir(&err)) {
            Error() << "Failed to probe Sapera for active hardware: " << err;
            return;
        }
        QProcess p(0);
        p.setProcessChannelMode(task.usesMergedChannels() ? QProcess::MergedChannels : QProcess::SeparateChannels);
        p.setWorkingDirectory(task.exeDir);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        task.setupEnv(env);
        p.setProcessEnvironment(env);
        p.start(task.exePath());
        if (p.state() == QProcess::NotRunning) {
            Error() << "Failed to probe Sapera for active hardware: slave process startup failed!";
            p.kill();
            return;
        }
        if (!p.waitForStarted(30000)) {
            Error() << "Failed to probe Sapera for active hardware: slave process startup timed out!";
            p.kill();
            return;
        }
        QByteArray ba;
        ba.resize(sizeof(XtCmdServerResource));
        XtCmdServerResource *x = (XtCmdServerResource *)ba.data();
        x->init("","",-1,-1,0,0);
        p.write(ba);
        int timeout = 10000;
        if (p.state() == QProcess::Running && p.waitForReadyRead(timeout)) {
            ba.clear();
            probedHardware.clear();
            do {
                ba += p.readAll();
                unsigned consumed = task.gotInput(ba, 0, p);
                ba.remove(0,consumed);
                if (!probedHardware.empty()) timeout = 500;
            } while (p.state() == QProcess::Running && p.waitForReadyRead(timeout));
            p.kill();
        } else {
            Error() << "Failed to probe Sapera for active hardware: slave process not running or waitForReadyRead() timed out!";
            p.kill();
            return;
        }
        task.sendExitCommand(p);
        p.waitForFinished(250);
        if (p.state() != QProcess::NotRunning) p.kill();

#endif
        {
            bool hasfake = false;
            // add fake FG if needed
            for (QList<DAQ::FGTask::Hardware>::const_iterator it = probedHardware.begin(); it != probedHardware.end(); ++it)
                if (it->serverName.startsWith("Fake")) hasfake = true;
            if (!hasfake) {
                Hardware h; h.serverName = "FakeFG"; h.resourceName = "Synthetic Data for Testing"; h.serverIndex = -1234; h.resourceIndex = -1234; h.serverType = -1234; h.accessible = true;
                probedHardware.push_back(h);
            }
        }
        last_hw_probe_ts = getTime();
        Debug() << "Probe done, found " << probedHardware.size() << " valid AcqDevices in " << (last_hw_probe_ts-t0) << " secs.";
    }

    MultiChanAIReader::MultiChanAIReader(QObject *parent)
        : QObject(parent), mem(0), psr(0), nitask(0) {}

    MultiChanAIReader::~MultiChanAIReader() { reset(); }

    void MultiChanAIReader::reset() {
        if (nitask) {
            nitask->stop();
            delete nitask, nitask = 0;
        }
        if (psr) { delete psr, psr = 0; }
        if (mem) { delete [] mem, mem = 0; }
    }

    bool MultiChanAIReader::readAll(std::vector<int16> & samps) {
        samps.clear();
        if (!nitask || !psr || !mem) return false;
        int nSkips; unsigned nScans;
        const short *buf = 0;
        do {
            buf = psr->next(&nSkips, 0, &nScans);
            if (buf && nScans) {
                unsigned num = nScans*unsigned(nChans());
                samps.reserve(samps.size()+num);
                for (unsigned i = 0; i < num; ++i) {
                    samps.push_back(buf[i]);
                }
            }
        } while(buf);
        return samps.size() != 0;
    }

    /*static*/ int MultiChanAIReader::parseDevChan(const QString & devchan, QString & dev) {
        QStringList l = devchan.split(QString("/"), QString::KeepEmptyParts);
        dev = "";
        if (l.size() == 2) {
            dev = l.front();
            QString ch = l.back();
            QRegExp re("[^0-9]*([0-9]+)");
            if (re.exactMatch(ch)) {
                return re.cap(1).toUInt();
            }
        }
        return -1;
    }

    QString MultiChanAIReader::startDAQ(const QStringList &devchList, double srate, double bsecs, unsigned nbufs, DAQ::TermConfig aiTerm, const DAQ::Range & range)
    {
        reset();
        if (srate < 16.0) return "SinglsChanAIReader: Invalid sampling rate specified. Need at least 16Hz";
        if (bsecs < 0.001) return "SinglsChanAIReader: Invalid buffer size secs specified.  Need at least 1ms worth of buffer size!";
        const int nCh = devchList.size();
        if (!nCh) return QString("MultiChanAIReader: Need to specify at least 1 channel as first parameter!");
        int bufsamps = bsecs * srate * nCh;
        if (bufsamps < 16) bufsamps = 16;
        if (nbufs < 2) nbufs = 2;
        unsigned long sz_bytes = 0;
        mem = new char[(sz_bytes = sizeof(int16) * bufsamps * nbufs + 4096)];
        Debug() << "MultiChanAIReader using " << double(sz_bytes/(1024.0)) << " KB buffer";
        Debug() << "MultiChanAIReader using chans: " << devchList.join(",") << " range: " << range.min << "-" << range.max << " V" ;
        psr = new PagedScanReader(nCh, 0, mem, sz_bytes, bufsamps*sizeof(int16));
        Params & p(fakeParams);
        p.dualDevMode = false;
        p.range = range;
        p.mode=AIRegular;
        p.extClock = false;
        p.srate = srate;
        p.aiChannels.clear();
        p.chanDisplayNames.clear();
        p.aiString = "";
        for (QStringList::const_iterator it = devchList.begin(); it != devchList.end(); ++it) {
            int chan = parseDevChan(*it, p.dev);
            if (chan < 0) { return QString("MultiChanAIReader: Invalid devChan string: ") + *it; }
            p.aiChannels.push_back(chan);
            p.aiString = p.aiString + (p.aiString.length() ? QString(",") : QString("")) + QString::number(chan);
            p.chanDisplayNames.push_back(QString("AI")+QString::number(chan));
        }
        p.nVAIChans = nCh; p.nVAIChans1 = nCh; p.nVAIChans2 = 0; p.nExtraChans1 = 0, p.nExtraChans2 = 0;
        p.aoPassthru = false;
        p.aoDev = "";
        p.acqStartEndMode = Immediate;
        p.usePD = false;  p.lowLatency = true/*false*/;
        p.aiTerm = aiTerm;
        //Debug() << "aiterm=" << int(aiTerm);
        p.auxGain = 1.0;
        p.aiBufferSizeCS = (bsecs*100.0) / 2.0;
        if (p.aiBufferSizeCS < 1) p.aiBufferSizeCS = 1;
        p.autoRetryOnAIOverrun = true;
        ChanMap m; m.push_back(ChanMapDesc());
        p.chanMap = m;
        p.subsetString = "ALL";
        p.demuxedBitMap.resize(nCh); p.demuxedBitMap.fill(true);
        p.doCtlChan = 0;
        p.doCtlChanString = "";
        p.autoRetryOnAIOverrun = true;
        nitask = new NITask(p, 0, *psr, "MultiChanAIReader NITask Helper");
        Connect(nitask, SIGNAL(taskError(const QString &)), this, SIGNAL(error(const QString &)));
        Connect(nitask, SIGNAL(taskWarning(const QString &)), this, SIGNAL(warning(const QString &)));
        nitask->start();
        return "";
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
        //    Log() << "Found and dynamically loaded NI Driver DLL: nicaiu.dll, DAQ features enabled";
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
        static int32 (__CFUNC *func)(const char *, bool32 *)      = 0;
        const char *fname = "DAQmxGetDevAISimultaneousSamplingSupported";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(device,data);
        return DAQmxErrorRequiredDependencyNotFound;                        
    }
    
    int32 __CFUNC DAQmxGetDevAIMaxSingleChanRate(const char device[], float64 *data) {
        static int32 (__CFUNC *func)(const char *, float64 *)      = 0;
        const char *fname = "DAQmxGetDevAIMaxSingleChanRate";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(device,data);
        return DAQmxErrorRequiredDependencyNotFound;                        
    }
    
    int32 __CFUNC DAQmxGetDevAIPhysicalChans(const char device[], char *data, uInt32 bufferSize) {
        static int32 (__CFUNC *func)(const char *, char *, uInt32)      = 0;
        const char *fname = "DAQmxGetDevAIPhysicalChans";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(device,data,bufferSize);
        return DAQmxErrorRequiredDependencyNotFound;                                
    }
    
    int32 __CFUNC DAQmxGetDevAOVoltageRngs(const char device[], float64 *data, uInt32 arraySizeInSamples) {
        static int32 (__CFUNC *func)(const char *, float64 *, uInt32)      = 0;
        const char *fname = "DAQmxGetDevAOVoltageRngs";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(device,data,arraySizeInSamples);
        return DAQmxErrorRequiredDependencyNotFound;                                        
    }
    
    int32 __CFUNC DAQmxGetDevAIVoltageRngs(const char device[], float64 *data, uInt32 arraySizeInSamples) {
        static int32 (__CFUNC *func)(const char *, float64 *, uInt32)      = 0;
        const char *fname = "DAQmxGetDevAIVoltageRngs";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(device,data,arraySizeInSamples);
        return DAQmxErrorRequiredDependencyNotFound;                                                
    }
    
    int32 __CFUNC DAQmxGetDevAIMinRate(const char device[], float64 *data) {
        static int32 (__CFUNC *func)(const char *, float64 *)      = 0;
        const char *fname = "DAQmxGetDevAIMinRate";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(device,data);
        return DAQmxErrorRequiredDependencyNotFound;                                                        
    }
    
    int32 __CFUNC DAQmxGetDevAOPhysicalChans(const char device[], char *data, uInt32 bufferSize) {
        static int32 (__CFUNC *func)(const char *, char  *, uInt32)      = 0;
        const char *fname = "DAQmxGetDevAOPhysicalChans";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(device,data,bufferSize);
        return DAQmxErrorRequiredDependencyNotFound;                                                                
    }
    
    int32 __CFUNC DAQmxGetDevProductType(const char device[], char *data, uInt32 bufferSize) {
        static int32 (__CFUNC *func)(const char *, char  *, uInt32)      = 0;
        const char *fname = "DAQmxGetDevProductType";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(device,data,bufferSize);
        return DAQmxErrorRequiredDependencyNotFound;                                                                        
    }
    
    int32 __CFUNC DAQmxCfgInputBuffer(TaskHandle taskHandle, uInt32 numSampsPerChan) {
        static int32 (__CFUNC *func)(TaskHandle, uInt32)      = 0;
        const char *fname = "DAQmxCfgInputBuffer";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(taskHandle,numSampsPerChan);
        return DAQmxErrorRequiredDependencyNotFound;        
    }
    
    int32 __CFUNC DAQmxCfgOutputBuffer(TaskHandle taskHandle, uInt32 numSampsPerChan) {
    static int32 (__CFUNC *func)(TaskHandle, uInt32)      = 0;
    const char *fname = "DAQmxCfgOutputBuffer";
    DAQ::tryLoadFunc(func, fname);
    //Debug() << fname << " called";
    if (func) return func(taskHandle,numSampsPerChan);
        return DAQmxErrorRequiredDependencyNotFound;        
    }
    
    int32 __CFUNC DAQmxCfgSampClkTiming(TaskHandle taskHandle, const char source[], float64 rate, int32 activeEdge, int32 sampleMode, uInt64 sampsPerChan) {
        static int32 (__CFUNC *func)(TaskHandle, const char *, float64, int32, int32, uInt64)      = 0;
        const char *fname = "DAQmxCfgSampClkTiming";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(taskHandle,source,rate,activeEdge,sampleMode,sampsPerChan);
        return DAQmxErrorRequiredDependencyNotFound;                
    }

    int32 __CFUNC DAQmxCreateAIVoltageChan(TaskHandle taskHandle, const char physicalChannel[], const char nameToAssignToChannel[], int32 terminalConfig, float64 minVal, float64 maxVal, int32 units, const char customScaleName[]) {
        static int32 (__CFUNC *func)(TaskHandle, const char *, const char *, int32, float64, float64, int32, const char *)      = 0;
        const char *fname = "DAQmxCreateAIVoltageChan";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(taskHandle,physicalChannel,nameToAssignToChannel,terminalConfig,minVal,maxVal,units,customScaleName);
        return DAQmxErrorRequiredDependencyNotFound;                
    }        

    int32 __CFUNC DAQmxCreateAOVoltageChan(TaskHandle taskHandle, const char physicalChannel[], const char nameToAssignToChannel[], float64 minVal, float64 maxVal, int32 units, const char customScaleName[]) {
        static int32 (__CFUNC *func)(TaskHandle, const char *, const char *, float64, float64, int32, const char *)      = 0;
        const char *fname = "DAQmxCreateAOVoltageChan";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(taskHandle,physicalChannel,nameToAssignToChannel,minVal,maxVal,units,customScaleName);
        return DAQmxErrorRequiredDependencyNotFound;                
    }        

    int32 __CFUNC DAQmxReadBinaryI16(TaskHandle taskHandle, int32 numSampsPerChan, float64 timeout, bool32 fillMode, int16 readArray[], uInt32 arraySizeInSamps, int32 *sampsPerChanRead, bool32 *reserved) {
        static int32 (__CFUNC *func)(TaskHandle, int32, float64, bool32, int16 *, uInt32, int32 *, bool32 *)      = 0;
        const char *fname = "DAQmxReadBinaryI16";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(taskHandle,numSampsPerChan,timeout,fillMode,readArray,arraySizeInSamps,sampsPerChanRead,reserved);
        return DAQmxErrorRequiredDependencyNotFound;                
    }

    int32 __CFUNC DAQmxWriteBinaryI16(TaskHandle taskHandle, int32 numSampsPerChan, bool32 autoStart, float64 timeout, bool32 dataLayout, const int16 writeArray[], int32 *sampsPerChanWritten, bool32 *reserved) {
        static int32 (__CFUNC *func)(TaskHandle, int32, bool32, float64, bool32, const int16 *, int32 *, bool32 *)      = 0;
        const char *fname = "DAQmxWriteBinaryI16";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(taskHandle,numSampsPerChan,autoStart,timeout,dataLayout,writeArray,sampsPerChanWritten,reserved);
        return DAQmxErrorRequiredDependencyNotFound;                
    }
    
    int32 __CFUNC DAQmxRegisterEveryNSamplesEvent(TaskHandle taskHandle, int32 everyNsamplesEventType, uInt32 nSamples, uInt32 options, DAQmxEveryNSamplesEventCallbackPtr callbackFunction, void *callbackData) {
        static int32 (__CFUNC *func)(TaskHandle, int32, uInt32, uInt32, DAQmxEveryNSamplesEventCallbackPtr, void *)      = 0;
        const char *fname = "DAQmxRegisterEveryNSamplesEvent";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(taskHandle,everyNsamplesEventType,nSamples,options,callbackFunction,callbackData);
        return DAQmxErrorRequiredDependencyNotFound;        
    }

    //*** Set/Get functions for DAQmx_AI_Max ***
    int32 __CFUNC DAQmxGetAIMax(TaskHandle taskHandle, const char channel[], float64 *data) {
        static int32 (__CFUNC *func)(TaskHandle,const char *,float64 *) = 0;
        const char *fname = "DAQmxGetAIMax";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(taskHandle,channel,data);
        return DAQmxErrorRequiredDependencyNotFound;
    }
    int32 __CFUNC DAQmxSetAIMax(TaskHandle taskHandle, const char channel[], float64 data) {
        static int32 (__CFUNC *func)(TaskHandle,const char *,float64) = 0;
        const char *fname = "DAQmxSetAIMax";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(taskHandle,channel,data);
        return DAQmxErrorRequiredDependencyNotFound;
    }
    int32 __CFUNC DAQmxResetAIMax(TaskHandle taskHandle, const char channel[]) {
        static int32 (__CFUNC *func)(TaskHandle,const char *) = 0;
        const char *fname = "DAQmxResetAIMax";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(taskHandle,channel);
        return DAQmxErrorRequiredDependencyNotFound;
    }
    //*** Set/Get functions for DAQmx_AI_Min ***
    int32 __CFUNC DAQmxGetAIMin(TaskHandle taskHandle, const char channel[], float64 *data) {
        static int32 (__CFUNC *func)(TaskHandle,const char *,float64 *) = 0;
        const char *fname = "DAQmxGetAIMin";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(taskHandle,channel,data);
        return DAQmxErrorRequiredDependencyNotFound;
    }
    int32 __CFUNC DAQmxSetAIMin(TaskHandle taskHandle, const char channel[], float64 data) {
        static int32 (__CFUNC *func)(TaskHandle,const char *,float64) = 0;
        const char *fname = "DAQmxSetAIMin";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(taskHandle,channel,data);
        return DAQmxErrorRequiredDependencyNotFound;
    }
    int32 __CFUNC DAQmxResetAIMin(TaskHandle taskHandle, const char channel[]) {
        static int32 (__CFUNC *func)(TaskHandle,const char *) = 0;
        const char *fname = "DAQmxResetAIMin";
        DAQ::tryLoadFunc(func, fname);
        //Debug() << fname << " called";
        if (func) return func(taskHandle,channel);
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

#ifndef DAQ_H
#define DAQ_H
#include "SpikeGL.h"
#include "SampleBufQ.h"
#include "ChanMap.h"
#include <QMultiMap>
#include <QString>
#include <QThread>
#include <QMutex>
#include <QMutexLocker>
#include <vector>
#include <deque>
#include <QMap>
#include <QStringList>
#include <QVector>
#include <QPair>
#include <QProcess>
#include <QSharedMemory>
#ifdef HAVE_NIDAQmx
#include "NI/NIDAQmx.h"
#endif
#ifdef Q_OS_WIN
// Argh!  Windows for some reason has this macro defined..
#undef min
#undef max
#endif
#include <list>
#include "ui_FG_Controls.h"
#include "PagedRingBuffer.h"

struct XtCmd;

namespace DAQ
{
    struct Range {
        double min, max;
        Range() : min(0.), max(0.) {}
		Range(double mn, double mx): min(mn), max(mx) {}
		bool operator==(const Range &rhs) const { return min == rhs.min && max == rhs.max; }
		bool operator!=(const Range &rhs) const { return !((*this) == rhs); }
    };

    enum Mode {
        AI60Demux=0, AIRegular, AI120Demux, JFRCIntan32, AI128Demux, AI256Demux, AI64Demux, AI96Demux, AI128_32_Demux, N_Modes, AIUnknown = N_Modes
    };

	extern const unsigned ModeNumChansPerIntan[N_Modes];
	extern const unsigned ModeNumIntans[N_Modes];
	
    enum AcqStartEndMode { 
        /* these correspond to items in 'acqStartEndCB' combobox in 
           the Ui::ConfigureDialog form */
        Immediate=0, PDStartEnd, PDStart, Timed, StimGLStartEnd, StimGLStart, 
		AITriggered, /* use physical AI line for triggering */
		Bug3TTLTriggered, /* identical in behavior to PDStart above, but different UI messaging */
        N_AcqStartEndModes
    };

    enum TermConfig {
        Default = -1,
        RSE = 10083,
        NRSE = 10078,
        Diff = 10106,
        PseudoDiff = 12529
    };

    TermConfig StringToTermConfig(const QString & txt);
    QString TermConfigToString(TermConfig t);

    const QString & ModeToString(Mode m);
    Mode StringToMode(const QString &);

    const QString & AcqStartEndModeToString(AcqStartEndMode m);
	
    struct Params {
        QString outputFile, outputFileOrig, dev, dev2;
		bool dualDevMode, secondDevIsAuxOnly;
        bool stimGlTrigResave;
        Range range;
		QVector<Range> customRanges; ///< if this has a size, then it's a vector where each vai chan (demuxed) maps to a specific range for UI display!
        unsigned doCtlChan;
        QString doCtlChanString;
        Mode mode;
        double srate, aoSrate;
        bool extClock;
		QString aoClock;
        QString aiString, aiString2;
        QVector<unsigned> aiChannels, aiChannels2;
        QString subsetString; ///< subset of demuxed AI chans to actually save/graph.
        QBitArray demuxedBitMap; ///< bitmap of the demuxed AI chans to actually save/graph.  Derived from subsetString above.
        unsigned nVAIChans, ///< number of virtual (demuxed) AI chans
		         nVAIChans1, nVAIChans2;  ///< same, but per dev
        unsigned nExtraChans1, nExtraChans2; ///< the number of extra channels (PD, etc) that aren't part of the demux.. if not in MUX mode, this is always 0
        bool aoPassthru;
        QString aoDev;
        Range aoRange;
        QMap<unsigned, unsigned> aoPassthruMap;
        QVector<unsigned> aoChannels; ///< the AO channels from the above map, plus possibly the photodiode-passthru channel
        QString aoPassthruString;
		QVector<QString> chanDisplayNames; ///< this is for UI use mainly.. the display names associated with the channels.  This list is either empty (default) or has size nVAIChans
        /// etc...

        /// index into the acqStartEndCB in Ui::ConfigureDialog
        AcqStartEndMode acqStartEndMode;

        // for acqStartEndMode == timed
        bool isIndefinite, isImmediate;
        double startIn, duration;

        // for threshold crossing of PD chan
        int16 pdThresh;
		unsigned pdThreshW; /**< Number of samples that the signal must be past 
						         threshhold thold crossing.  Default 5. */
        bool usePD;
        int pdChan, idxOfPdChan;
		bool pdChanIsVirtual; ///< default false, if true, PD/trigger channel is virtual (demuxed) electrode id, if false, old behavior of physical channel
		bool pdOnSecondDev;
        int pdPassThruToAO; ///< if negative, don't, else the channel id
        double pdStopTime; ///< iff PDEnd mode, the amount of time in seconds that need to be elapsed before we pronounce the PD signal as gone and we stop the task
        double silenceBeforePD; ///< Time, in seconds, of silence before the photodiode signal in PD trigger mode only!  This time window is also applied to the file end as well.

        bool suppressGraphs, lowLatency;

        TermConfig aiTerm;

        unsigned fastSettleTimeMS; ///< defaults to 15ms

        double auxGain;

        ChanMap chanMap;

		bool doPreJuly2011IntanDemux; /**< This affects how we demux the AI channels as they come in
									       off the card -- Pre July 2011 demux lead to scans demuxed as:
									       
									           Intan0_CH0, Intan1_CH0, ... IntanN_ChanM, extrachan1, extrachan2
									   
										   Whereas now, the new scheme is:
									   
									           Intan0_CH0, Intan0_CH1, ... Intan0_ChanM, Intan1_Chan0, ... IntanN_ChanM, extrachan1, extrachan2
									   */
		
		unsigned aiBufferSizeCS; ///< a value from 1 -> 100, which represents the centiseconds (.01 sec or 10 ms) worth of time to make the analog input buffer
		unsigned aoBufferSizeCS; ///< same as above, but for the AO buffer size
		
        int cludgyFilenameCounterOverride; ///< Here it is. Yet more insane complexity and non-obvious behavior to the UI! Hurray!
		
		bool resumeGraphSettings;

		bool autoRetryOnAIOverrun; ///< if true, auto-restart the acquisition every time there is a buffer overrun error from the NI DAQ drivers. Note that if we get more than 2 failures in a 1s period, the acquisition is aborted anyway.
        int overrideGraphsPerTab; ///< if nonzero, the number of graphs per tab to display, 0 implies use mode-specific limits
        int graphUpdateRate, spatialVisUpdateRate; ///< if nonzero, update the graphs this many times per second.  if <=0, will use DEF_TASK_READ_FREQ_HZ from SpikeGL.h

		struct Bug {
			bool enabled; // if true, acquisition is in bug mode
			int rate; // 0 = Low, 1 = Medium, 2 = High
			int whichTTLs; // bitset of which TTLs to save/graph, TTLs from 1->11 maps to bits #0->10
            int ttlTrig; // the TTL channel to use for a trigger, or -1 if not using ttl to trigger. Note either this or auxTrig or aiTrig should be set to > -1/"", or none of the 3, but never more than 1
            int auxTrig; // the AUX channel to use for a trigger, or -1 if not using aux to trigger. Note either this or ttlTrig or aiTrig should be set to > -1/"", or none of the 3, but never more than 1
            QString aiTrig; // the NI-DAQ AI channel to use for a trigger, in Dev1/ai1 format... or "" if not using NI-DAQ for trigger. Note either this or ttlTrig or auxTrig should be set, or none of the 3, but never more than 1
            QStringList aiChans; // the list of channels to use for AI.  This includes the optional aiTrig, if set.. or up to 2 other channels.  Up to 2 channels may be specified here.
            double aiDownsampleFactor;
			int clockEdge; // 0 = rising, 1 = falling
			int hpf; // if nonzero, the high pass filter is enabled at set to filter past this many Hz
			bool snf; // if true, use the software notch filter at 60Hz
			int errTol; // out of 144, default is 6
            double trigThreshV; ///< for UI -- the actual trigger code uses .pdThresh
            QString aoPassthruString; // defaults to "", but can be something eg 0=1
            unsigned aoSrate;
            bool graphBadData; ///< if true, add an additional channel to the end which is a graph of 'meta.BER'.. logarithmically scaled to 0,MAXV
            bool altTTL; ///< if true, use alternate TTL triggering scheme whereby a single TTL pulse has a pre window and a post window surrounding it in the data file
            int backupTrigger; ///< index of channel which is the "backup" trigger, or -1 if no channel is the backup trigger
            int16 backupTriggerThresh; ///< in samps
            double aithold;
            QString guiAIRange;
            void reset() { rate = 2; whichTTLs = 0; errTol = 6; ttlTrig = -1; auxTrig = -1; aiTrig = ""; clockEdge = 0; hpf = 0; snf = false; enabled = false; altTTL = true; trigThreshV = 3.0; aiDownsampleFactor=1.0; graphBadData = false; backupTrigger = -1; backupTriggerThresh = 10000; aithold = 1.0; guiAIRange="-5-5 V"; }
		} bug;
		
		struct FG { // framegrabber
			bool enabled;
            bool disableChanMap;
            int sidx, ridx; ///< server and resource index
            int com,baud,bits,parity,stop;
            QString chanMapText;
            int spatialRows, spatialCols;
            bool extraAI;
            bool spatialVisSuppressExtraChans; ///< if true, the spatial vis window only shows the base MUXed channels and suppresses the nExtraChans1 and nExtraChans2 channels from its display.. defaults to true
            void reset() { enabled = false; com=1,baud=1,bits=0,parity=0,stop=0; sidx=1; ridx=0; disableChanMap = false; spatialRows=spatialCols=0; extraAI=false; spatialVisSuppressExtraChans = true; }
		} fg;
		
        mutable QMutex mutex;
        void lock() const { mutex.lock(); }
        void unlock() const { mutex.unlock(); }
		
		bool isAuxChan(unsigned num) const { return num >= (nVAIChans-(nExtraChans1+nExtraChans2)); }
		
    };

    //-------- NI DAQmx helper methods -------------

    typedef QMultiMap<QString,Range> DeviceRangeMap;

    /// if empty map returned, no devices with AI!
    DeviceRangeMap ProbeAllAIRanges();
    /// if empty map returned, no devices with AO!
    DeviceRangeMap ProbeAllAORanges();

    typedef QMap<QString, QStringList> DeviceChanMap;
    
    /// returns a list of Devicename->ai channel names for all devices containing AI subdevices in the system
    DeviceChanMap ProbeAllAIChannels();
    /// returns a list of Devicename->ai channel names for all devices containing AO subdevices in the system
    DeviceChanMap ProbeAllAOChannels();

    /// returns the NI channel list of DO chans for a devname, or empty list on failure
    QStringList GetDOChans(const QString & devname);

    /// returns the NI channel list of AI chans for a devname, or empty list on failure
    QStringList GetAIChans(const QString & devname);
    /// returns the NI channel list of AO chans for a devname, or empty list on failure
    QStringList GetAOChans(const QString & devname);

    /// returns the number of physical channels in the AI subdevice for this device, or 0 if AI not supported on this device
    unsigned GetNAIChans(const QString & devname);
    /// returns the number of physical channels in the AO subdevice for this device, or 0 if AO not supported on this device
    unsigned GetNAOChans(const QString & devname);

    /// returns true iff the device supports AI simultaneous sampling
    bool     SupportsAISimultaneousSampling(const QString & devname);
    
    double   MaximumSampleRate(const QString & devname, int nChans = 1);
    double   MinimumSampleRate(const QString & devname);

    QString  GetProductName(const QString &devname);
    //-------- END NI DAQmx helper methods -------------


    class AOWriteThread;

    class Task : public QThread
	{
		Q_OBJECT
	public:
        Task(QObject *parent, const QString & name, const PagedScanReader & reference_reader);
		virtual ~Task(); ///< default impl. does nothing.  probably should make it call stop() in a subclass..
        virtual void stop() = 0;
		
        virtual unsigned numChans() const = 0;
        virtual unsigned samplingRate() const = 0;
		        
        u64 lastReadScan() const;

        const PagedScanWriter & pagedWriter() const { return writer; }

	protected:
		/// reimplemented from QThread, just calls daqThr
        void run() { daqThr(); }  
		virtual void daqThr() = 0; ///< reimplement this!
		
	signals:
        void bufferOverrun();
        void gotFirstScan();
        void taskError(const QString &);
        void taskWarning(const QString &);
		
	protected:
        u64 totalRead;
        mutable QMutex totalReadMut;
        PagedScanWriter writer;
	};
	
	
    /** This class represents 1 ni-based daq task, running in a separate thread.  
        Data is enqueued and other threads may be alerted via siganl/slot 
        mechanism when there is more data. */
    class NITask : public Task
    {
        Q_OBJECT
    public:
        NITask(const Params & acqParams, QObject *parent, const PagedScanReader &psr,
               const QString & taskDescriptiveName = "DAQ Task");
		~NITask(); ///< calls stop()

        void stop(); ///< stops and joins thr

        const QString & devString() const { return params.dev; }
        const QString & doCtlChan() const { return params.doCtlChanString; }
        Mode mode() const { return params.mode; }
        unsigned numChans() const { return params.nVAIChans; }
        unsigned samplingRate() const { return params.srate; }

        void setDO(bool onoff);

        static void recomputeAOAITab(QVector<QPair<int, int> > & aoAITab, QString & aoChan, const Params & p);

    public slots:
        void requestFastSettle(); ///< task needs to be running and it will then be done synchronously with the task

    signals:
        void fastSettleCompleted();

    protected: 
        void overflowWarning(); ///< reimplemented from SampleBufQ
		void daqThr(); ///< remplemented from DAQ::Task

    private:

        volatile bool pleaseStop;
        const Params & params;
        volatile unsigned fast_settle; ///< if >0, do fast settle
        bool muxMode;

        friend struct DAQPvt;

        static int computeTaskReadFreq(double srate);
		
		static void mergeDualDevData(std::vector<int16> & output, const std::vector<int16> & data, const std::vector<int16> & data2, int NCHANS1, int NCHANS2, int nExtraChans, int nExtraChans2);

        // used on all platforms
        void doFinalDemuxAndEnqueue(std::vector<int16> & data);

        AOWriteThread *aoWriteThr;
        QVector<QPair<int,int> > aoAITab;
        u64 aoSampCount;

#ifdef HAVE_NIDAQmx
		// used for task create/destroy
		TaskHandle taskHandle, taskHandle2;
#endif
		QString chan, chan2;
		const char *clockSource;
		float64 sampleRate,min,max,timeout;
		u64 bufferSize, bufferSize2;
        // DAQMx error macro related.. for task create/destroy/etc
        int32 error;
        const char *callStr;
        char errBuff[2048];
        QSet<int32> acceptableRetryErrors;
        int nReadRetries;
        double lastEnq;

		bool createAITasks();
		bool startAITasks();
		void destroyAITasks();
#ifdef HAVE_NIDAQmx
        // returns 0 if all ok, -1 if unrecoverable error, 1 if had "buffer overflow error" and tried did acq restart..
        int doAIRead(TaskHandle th, u64 samplesPerChan, std::vector<int16> & data, unsigned long oldS, int32 pointsToRead, int32 & pointsRead);
#endif
        void fudgeDataDueToReadRetry();
		
	};

#ifdef HAVE_NIDAQmx
	struct DAQPvt {
		static int32 everyNSamples_func (TaskHandle taskHandle, int32 everyNsamplesEventType, uint32 nSamples, void *callbackData); 
	};
#endif
		
	class SubprocessTask : public Task {
			Q_OBJECT
	public:
        SubprocessTask(Params & acqParams, QObject *parent,
					   const QString & shortName,
                       const QString & exeName,
                       const PagedScanReader &ref_reader);
        virtual ~SubprocessTask();

        void stop();

        DAQ::Params & allParams() { return params; }

    signals:
        void justStarted();

	protected:
		void daqThr(); ///< implemented from Task
		
         ///< return number of bytes consumed from data.  Data buffer will then have those bytes removed from beginning
        virtual unsigned gotInput(const QByteArray & allData, unsigned lastReadNBytes, QProcess & p) { (void)allData; (void)lastReadNBytes; (void)p; return 0; }
        ///< used to setup the exedir with the appropriate files in resources.  reimplement to return a list of resource paths to put into the exedir..
        virtual QStringList filesList() const { return QStringList(); }
         ///< called before the exe is about to be run. set up any needed environment parameters
        virtual void setupEnv(QProcessEnvironment & e) const { (void)e; }
		virtual void sendExitCommand(QProcess & p) const { (void)p; }
		virtual bool platformSupported() const { return true; }
		virtual int readTimeoutMaxSecs() const { return 5; }
        virtual int readTimeoutMS() const { return 1000; }
		virtual bool outputCmdsAreBinary() const { return false; }
        virtual bool usesMergedChannels() const { return true; }
        virtual void gotStdErr(const QByteArray & data);

#ifdef Q_OS_WINDOWS
		virtual QString interpreter() const { return ""; } 
#else
        virtual QString interpreter() const { return "/usr/local/bin/mono"; }
#endif
		
		volatile bool pleaseStop;
        Params & params;
        QString shortName, dirName, exeName, exeDir;
		
		void pushCmd(const QByteArray & cmd);
		
        bool setupExeDir(QString * err = 0) const;
        /// returns a strig of the form "c:\temp\bug3_spikegl\"
        QString exePath() const;

	protected slots:
		void slaveProcStateChanged(QProcess::ProcessState);
		
    private:
		QList<QByteArray> cmdQ; QMutex cmdQMut;
		void processCmds(QProcess & p);
        void readStdErr(QProcess & p);
	};
	
    class MultiChanAIReader; ///< fwd decl -- see declaration at end of this namespace

	class BugTask : public SubprocessTask {
		Q_OBJECT
	public:
		
		
		// some useful constants for the bug3 USB-based acquisition
		static const int ADCOffset = 1023;
		static const int FramesPerBlock = 40;
		static const int NeuralSamplesPerFrame = 16;
		static const int SpikeGLScansPerBlock = (FramesPerBlock*NeuralSamplesPerFrame); // 640
		static const int TotalNeuralChans = 10;
		static const int TotalEMGChans = 4;
		static const int TotalAuxChans = 2;
        static const int FirstAuxChan = TotalNeuralChans+TotalEMGChans;
		static const int BaseNChans = TotalNeuralChans+TotalEMGChans+TotalAuxChans;
		static const int TotalTTLChans = 11;
        static const double SamplingRate;// = 16.0 / 0.0006144;
        static const double ADCStepNeural;// = 2.5;                // units = uV
        static const double ADCStepEMG;// = 0.025;                 // units = mV
        static const double ADCStepAux;// = 0.0052;                // units = V
        static const double ADCStep;// = 1.02 * 1.225 / 1024.0;    // units = V
		static const int MaxMetaData = 120;
        static const double MaxBER;// = -1.0;
        static const double MinBER;// = -5.0;
		
		
        BugTask(Params & acqParams, QObject * parent, const PagedScanReader & ref_reader);
        ~BugTask();
		
        unsigned numChans() const;
        unsigned samplingRate() const;

        unsigned requiredShmPageSize() const;
        static unsigned requiredShmPageSize(unsigned nChans);

        Params::Bug & bugParams() { return params.bug; }

		struct BlockMetaData {
			quint64 blockNum; ///< sequential number. incremented for each new block
			int boardFrameCounter[FramesPerBlock];
			int boardFrameTimer[FramesPerBlock];
			int chipFrameCounter[FramesPerBlock];
			quint16 chipID[FramesPerBlock];
			quint16 frameMarkerCorrelation[FramesPerBlock];
			int missingFrameCount;
			int falseFrameCount;
			double BER, WER; ///< bit error rate and word error rate
			double avgVunreg; ///< computed value derived from the "AUX" voltage channel.  Avg of all frames.  To save time we compute it on-the-fly as well.
            u64 comm_absTimeNS, creation_absTimeNS; ///< timestamp ultimately coming off the CPU TSC that went with the block when it was created (creation_) and when it was sent down the pipe to SpikeGL (comm_)
			
			BlockMetaData();
			BlockMetaData(const BlockMetaData &o);
			BlockMetaData & operator=(const BlockMetaData & o);
		};
				
		void setNotchFilter(bool enabled);
		void setHPFilter(int val); ///<   <=0 == off, >0 = freq in Hz to high-pass filter
			
		/// returns the usb data block size, in samples, depending on HIGH, MEDIUM, LOW data rate
		int  usbDataBlockSizeSamps() const;
		
        static QString getChannelName(unsigned num, const Params & p); ///< returns UI-friendly name for a particular BUG channel
		static bool isNeuralChan(unsigned num);
		static bool isEMGChan(unsigned num);
		static bool isAuxChan(unsigned num);
		static bool isTTLChan(unsigned num);
        static bool isAIChan(const Params &p, unsigned num);
        static bool isMissingFCChan(const Params &p, unsigned num);
		
	protected:
		unsigned gotInput(const QByteArray & data, unsigned lastReadNBytes, QProcess & p); ///< return number of bytes consumed from data.  Data buffer will then have those bytes removed from beginning
		QStringList filesList() const; ///< used to setup the exedir with the appropriate files in resources.  reimplement to return a list of resource paths to put into the exedir..
		void setupEnv(QProcessEnvironment &) const; ///< called before the exe is about to be run. set up any needed environment parameters
		void sendExitCommand(QProcess &p) const;
        int readTimeoutMaxSecs() const;

	private:
        unsigned req_shm_pg_sz;

		int state;
        static const int nstates = 38;
		quint64 nblocks, nlines;
		qint64 debugTTLStart;
		QMap<QString, QString> block;

        AOWriteThread *aoWriteThread;
        u64 aoSampCount;
        QString savedAOPassthruString;
        QVector<QPair<int, int> > aoAITab;
        QString aoChan;

        MultiChanAIReader *aireader;
        std::vector<int16> ais; ///< persistent ai buffer
		
		void processLine(const QString & lineUntrimmed, QMap<QString, QString> & block, const quint64 & nblocks, int & state, quint64 & nlines);
		void processBlock(const QMap<QString, QString> &, quint64 blockNum);		
        void handleAOPassthru(const std::vector<int16> & samps);
        void handleAI(std::vector<int16> & samps);
        void handleBadDataGraph(std::vector<int16> & samps, const BlockMetaData & meta);
	};
	

    class FGTask : public SubprocessTask {
			Q_OBJECT
    public:

        struct Hardware {
            QString serverName, resourceName;
            int serverIndex, resourceIndex, serverType;
            bool accessible;
        };

        static QList<Hardware> probedHardware; ///< populate this by calling probeHardware()
        static double lastProbeTS() { return last_hw_probe_ts; }
        static void probeHardware();

        FGTask(Params & acqParams, QObject * parent, const PagedScanReader & psr, bool isDummyTask = false);
        ~FGTask();
		
        static const int *getDefaultMapping(int which /* 1=calin 0=janelia*/, ChanMap *cm_out = 0);
        static bool setupCMFromArray(const int *mapping, int which /* 1=calin 0=janelia */, ChanMap *cm_out);

        unsigned numChans() const;
        unsigned samplingRate() const;
		
        static QString getChannelName(unsigned num, const ChanMap *chanMap=0);
		
        static const double SamplingRate;
        static const int NumChans /*= 2304 == 72 * 32 */;

        void pushCmd(const XtCmd * c);
        void pushCmd(const XtCmd & c) { pushCmd(&c); }

        QDialog *dialogW;

        void updateTimesampLabel(unsigned int ts);

    protected:
        int readTimeoutMaxSecs() const { return 9999; }
		unsigned gotInput(const QByteArray & data, unsigned lastReadNBytes, QProcess & p);
		QStringList filesList() const;
        void sendExitCommand(QProcess & p) const;
        bool outputCmdsAreBinary() const { return true; }
        /*virtual*/ QString interpreter() const { return ""; }

    signals:

        void gotMsg(const QString &txt, const QColor & color);
        void gotClkSignals(int param);
        void gotFPS(int fps);

    private slots:

        //void calibClicked();
        //void setupRegsClicked();
        //void contAdcClicked();
        void doGrabFrames();
        //void stopGrabClicked();
        void appendTE(const QString &s, const QColor & color = QColor(Qt::black));
        void updateClkSignals(int param);
        void updateFPS(int fps);
        void openComPort();
        void setSaperaDevice();
        void do_updateTimestampLabel();

	private:

        static double last_hw_probe_ts;
        bool sentFGCmd, didImgSizeWarn;
        Ui::FG_Controls *dialog;
        bool need2EmitFirstScan;
        volatile unsigned int lastScanTS; QMutex lastScanTSMut;
	};
	

    class MultiChanAIReader : public QObject
    {
        Q_OBJECT
    public:
        MultiChanAIReader(QObject *parent=0);
        ~MultiChanAIReader();
        QString startDAQ(const QStringList & aiDevChans, double srate, double bufsize_secs, unsigned nbufs_total, DAQ::TermConfig aiTerm, const DAQ::Range & range);
        bool readAll(std::vector<int16> & samps);

        int nChans() const { return int(fakeParams.nVAIChans); }
        double rate() const { return fakeParams.srate; }

    signals:
        void error(const QString &);
        void warning(const QString &);

    private:
        static int parseDevChan(const QString &devChan, QString & dev_parsed);
        void reset();
        Params fakeParams;
        char *mem;
        PagedScanReader *psr;
        NITask *nitask;
    };
}
#endif

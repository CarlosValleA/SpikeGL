#ifndef DataFile_H
#define DataFile_H
#include <QString>
#include <QFile>
#include <QPair>
#include <QList>
#include <QVector>
#include <QMutexLocker>
#include <QMutex>
#include "TypeDefs.h"
#include "Params.h"

#include "sha1.h"
#include "DAQ.h"
#include "ChanMap.h"

class DFWriteThread;

class DataFile
{
	friend class DFWriteThread;
	
public:
    DataFile();
    ~DataFile();

	/** Checks if filename (presumably a .bin file) is openable for read and that
	    it is valid (has the proper size as specified in the meta file, etc).  
		If it's valid, returns true.  Otherwise returns false, and, if error is not 
	    NULL, optionally sets *error to an appropriate error message. */
	static bool isValidInputFile(const QString & filename, QString * error = 0);
	
    /// this function is threadsafe
    bool openForWrite(const DAQ::Params & params, const QString & filename_override = "");
	
	/** Normally you won't use this method.  This is used by the FileViewerWindow export code to reopen a new file for output
	    based on a previous input file.  It computes the channel subset and other parameters correctly from the input file.
	    The passed-in chanNumSubset is a subset of channel indices (not chan id's!) to use in the export.  So if the 
	    channel id's you are reading in are 0,1,2,3,6,7,8 and you want to export the last 3 of these using openForRewrite, you would 
        pass in [4,5,6] (not [6,7,8]) as the chanNumSubset.
        Note: this function is not threadsafe as it was written to be used in unthreaded code. */
	bool openForReWrite(const DataFile & other, const QString & filename, const QVector<unsigned> & chanNumSubset);

	/** Returns true if binFileName was successfully opened, false otherwise. If 
        opened, puts this instance into Input mode.
        Note: this function is not threadsafe as it was never intended to be called by threaded code. */
	bool openForRead(const QString & binFileName);

    bool isOpen() const { QMutexLocker ml(&mut); return dataFile.isOpen() && metaFile.isOpen(); }
    bool isOpenForRead() const { QMutexLocker ml(&mut); return isOpen() && mode == Input; }
    bool isOpenForWrite() const { QMutexLocker ml(&mut); return isOpen() && mode == Output; }
    QString fileName() const { QMutexLocker ml(&mut); return dataFile.fileName(); }
    QString metaFileName() const { QMutexLocker ml(&mut); return metaFile.fileName(); }

    /// param management
    void setParam(const QString & name, const QVariant & value);
    /// param management
    const QVariant & getParam(const QString & name) const;

    /// call this to indicate certain regions of the file are "bad", that is, they contained dropped/fake scans
    void pushBadData(u64 scan, u64 length) { badData.push_back(QPair<u64,u64>(scan,length)); }
    typedef QList<QPair<u64,u64> > BadData;
    const BadData & badDataList() const { return badData; }

    /// closes the file, and saves the SHA1 hash to the metafile 
    bool closeAndFinalize();

    /** Write complete scans to the file.  File must have been opened for write 
		using openForWrite().
        Must be vector of length a multiple of  numChans() otherwise it will 
	    fail unconditionally */
    bool writeScans(const std::vector<int16> & scan, bool asynch = false, unsigned asynch_queue_size = 0);
	

    /// *synchronous* write of a scan to a file.  File must have been opened for write
    /// using openForWrite()
    /// A scan is defined as a unit of numChans() samples.  Pass the scanCt to tell this function how many scans
    /// are in `scans'.
    bool writeScans(const int16 *scans, unsigned scanCt);

	/// Returns true iff we did an asynch write and we have writes that still haven't finished.  False otherwise.
	bool hasPendingWrites() const;
	
	/// Waits timeout_ms milliseconds for pending writes to complete.  Returns true if writes finishd within allotted time, false otherwise. If timeout_ms is negative, waits indefinitely.
	bool waitForPendingWrites(int timeout_ms) const;
	
	/// If using asynch writes, returns the pending write queue fill percent, otherwise 0.
	double pendingWriteQFillPct() const;
	
	/** Read scans from the file.  File must have been opened for read
	    using openForRead().  Returns number of scans actually read or -1 on failure.  
		NB: Note that return value is normally num2read / downSampleFactor. 
	    NB 2: Short reads are supported -- that is, if pos + num2read is past 
	    the end of file, the number of scans available is read instead.  
        The return value will reflect this.
        Note: this function is not threadsafe as it was never intended to be called by threaded code. */
	i64 readScans(std::vector<int16> & scans_out, u64 pos, u64 num2read, const QBitArray & channelSubset = QBitArray(), unsigned downSampleFactor = 1);
	
    // all below functions are not threadsafe.. they are not called by anything other than the main thread typically, or if they are.
    // they use volatile ints and such or return 'non-critical' data...
    u64 sampleCount() const { return scanCt*(u64)nChans; }
    u64 scanCount() const { return scanCt; }
    unsigned numChans() const { return nChans; }
    double samplingRateHz() const { return sRate; }
	double fileTimeSecs() const { return double(scanCt) / double(sRate); }
	double rangeMin(int i=-1) const { if (i >= 0 && i < customRanges.size()) return customRanges[i].min; return range.min; }
	double rangeMax(int i=-1) const { if (i >= 0 && i < customRanges.size()) return customRanges[i].max; return range.max; }
	QString channelDisplayName(int i) const { if (i>=0 && i < chanDisplayNames.size()) return chanDisplayNames[i]; return QString("Ch ") + QString::number(i); }
	/// from meta file: based on the channel subset used for this data file, returns a list of the channel id's for all the channels in the data file
	const QVector<unsigned> & channelIDs() const { return chanIds; }
	int pdChanID() const { return pd_chanId; } ///< returns negative value if not using pd channel
	/// aux gain value from .meta file or 1.0 if "auxGain" field not found in meta file
	double auxGain() const;
	bool isDualDevMode() const;
	bool secondDevIsAuxOnly() const;
	DAQ::Mode daqMode() const;
	ChanMap chanMap() const;

    /// the average speed in bytes/sec for writes
    double writeSpeedBytesSec() const { return writeRateAvg_for_ui; }
    /// the minimal write speed required in bytes/sec, based on sample rate
    double minimalWriteSpeedRequired() const { return nChans*sizeof(int16)*double(sRate); }

	/// Add a comment to the meta file.  Comments will appear BEFORE all the name = value pairs in the file.
	/// This was added on 2/5/2015 in order to support 'meta' data coming in from the bug3/telemetry USB-based
	/// acquisition device.  Note that acquisitions using this method will produce LOTS of comments in the 
	/// meta file, because they record the word error rate, bit error rate, and avg voltage per "block"
	/// of data
    /// NOT THREADSAFE
	void writeCommentToMetaFile(const QString & comment, bool prepend_hash_symbol = true);
	
    /// STATIC METHODS
    static bool verifySHA1(const QString & filename); 

protected:
	bool doFileWrite(const std::vector<int16> & scans);
    bool doFileWrite(const int16 *scans, unsigned nScans);

    mutable QMutex mut;

private:
    
	enum { Undefined, Input, Output } mode;
	
	/// member vars used for Input and Output mode
    QFile dataFile, metaFile;
    Params params;
    volatile u64 scanCt;
    int nChans;
    double sRate;

    // list of scan_number,number_of_scans for locations of bad/faked data for buffer overrun situations
    BadData badData;

	/// member vars used for Input mode
	DAQ::Range range;
    QVector<DAQ::Range> customRanges;
	QVector<unsigned> chanIds;
	QVector<QString> chanDisplayNames;
	int pd_chanId;
	
	/// member vars used for Output mode only
    SHA1 sha;
    volatile unsigned writeRateAvg_for_ui; ///< in bytes/sec
    double writeRateAvg; ///< in bytes/sec
    unsigned nWritesAvg, nWritesAvgMax; ///< the number of writes in the average, tops off at sRate/10
	DFWriteThread *dfwt;
};
#endif

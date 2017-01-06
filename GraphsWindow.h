#ifndef GraphsWindow_H
#define GraphsWindow_H

#include <QMainWindow>
#include "DAQ.h"
#include "GLGraph.h"
#include "TypeDefs.h"
#include "VecWrapBuffer.h"
#include <QVector>
#include <vector>
#include "ChanMappingController.h"
#include <QSet>
#include <QMutex>
#include <QMutexLocker>
#include "GenericGrapher.h"


class QToolBar;
class QLabel;
class QAction;
class QFrame;
class QDoubleSpinBox;
class QCheckBox;
class HPFilter;
class QLed;
class QPushButton;
class QTabWidget;
class QTimer;
class QStackedWidget;
class QComboBox;

class GraphsWindow : public QMainWindow, public GenericGrapher
{
    Q_OBJECT
public:
    GraphsWindow(DAQ::Params & params, QWidget *parent = 0, bool isSaving = true, bool usesTabModeForNavigation = true/*set to false for framegrabber mode and enable spatial vis click method*/,
                 int graphUpdateRateHz = -1 /* if set to >0, specifies that the graphs should be updated this many times per second.  If not set, uses DEF_TASK_READ_FREQ_HZ from SpikeGL.h */
                 );
    ~GraphsWindow();

    /// if false, need to navigate channels using spatial visualiztion click method.
    bool usesTabModeForNavigation() const { return useTabs; }

    /* virtual */ void putScans(const int16 *data, unsigned data_size_samps, u64 firstSamp);
    /* virtual */ bool threadsafeIsVisible() const { return threadsafe_is_visible; }
    /* virtual */ bool caresAboutSkippedScans() const { return true; }
    /* virtual */ const char *grapherName() const { return "GraphsWindow"; }

    // clear a specific graph's points, or all if negative
    void clearGraph(int which = -1);

    // overrides parent -- applies event filtering to the doublespinboxes as well!
    void installEventFilter(QObject * filterObj);
    void removeEventFilter(QObject * filterObj);

    void setToggleSaveChkBox(bool b);
    void setToggleSaveLE(const QString & fname);

    const QLineEdit *saveFileLineEdit() const  { return saveFileLE; }


	void hideUnhideSaveChannelCBs();
	
	void sortGraphsByElectrodeId();
	void sortGraphsByIntan();
	
	int numGraphsPerTab() const { return NUM_GRAPHS_PER_GRAPH_TAB; }
	int numGraphTabs() const { return nGraphTabs; }
	int numColsPerGraphTab() const { return nColsGraphTab; }
	int numRowsPerGraphTab() const { return nRowsGraphTab; }

    const DAQ::Params & daqParams() const { return params; }

    const QVector<int> & currentSorting() const { return sorting; }
    const QVector<int> & currentNaming() const { return naming; }

    double getGraphTimeSecs(int i) const { return ( (i > -1 && i < graphTimesSecs.size()) ? graphTimesSecs[i] : 0.0);  }
    const QVector<double> & getGraphTimesSecs() const { return graphTimesSecs; }

    /// downsampling disabled == a return value of 1.0.  Otherwise, if > 1.0, downsampling happens by that ratio
    double getDownsampleRatio() const { QMutexLocker l(&graphsMut); return downsampleRatio; }

    /** Hack for Huai-Ti's emergency 'manual override' button that implicitly saves
     *  all data currently in the on-screen graphs immediately to the new data file.
     *
     *  This function helps with the implementation of that feature by grabbing all the data
     *  currently on-screen and putting it into the scans_out array. It returns the number of
     *  full scans written (divide scans_out.size() by the return value to determine scansize
     *  aka numchans).
     *
     *  Note that graphs that aren't on-screen or have 'smaller' windows will have 0's written to the
     *  appropriate spots in the scans_out data.. so that data still lines up (to the amount of data
     *  in the largest graph on-screen). */
    unsigned grabAllScansFromDisplayBuffers(std::vector<int16> & scans_out) const;

signals:
	void tabChanged(int tabNum);
    void manualTrig(bool);
    void sortingChanged(const QVector<int> & sorting, const QVector<int> & naming);
    void graphTimeSecsChanged(int graphId, double timeSecs);
    void downsampleRatioChanged(double newDownSampleRatio); ///< 1.0 == no downsampling, >1.0 == downsampling by that ratio

public slots:
    void setSGLTrig(bool);
    void setPDTrig(bool);
    void setTrigOverrideEnabled(bool);

    void openGraphsById(const QVector<unsigned> & electrode_ids); ///< creates a "custom" page and opens the first numGraphsPerTab() graphs in the list on that page!
    void setDownsampling(bool checked);
    void setDownsamplingCheckboxEnabled(bool en);
    void setDownsamplingSpinboxEnabled(bool en);
    void setDownsamplekHz(double khz);

protected:
    void showEvent(QShowEvent *);
    void hideEvent(QHideEvent *);

private slots:
    void updateGraphs();
    void hpfChk(bool checked);
    void pauseGraph();
    void toggleMaximize();
    void selectGraph(int num);
    void graphSecsChanged(double d);
    void graphYScaleChanged(double d);
    void applyAll();

    void mouseOverGraph(double x, double y);
    void mouseClickGraph(double x, double y);
    void mouseDoubleClickGraph(double x, double y);
    void updateMouseOver(); // called periodically every 1s
    void doGraphColorDialog();
    void toggleSaveChecked(bool b);

	void saveGraphChecked(bool b);

	void tabChange(int);

	void saveFileLineEditChanged(const QString & newtext);
	
    void manualTrigOverrideChanged(bool);
	
private:
    void setGraphTimeSecs(int graphnum, double t); // note you should call update_nPtsAllGs after this!  (Not auto-called in this function just in case of batch setGraphTimeSecs() in which case 1 call at end to update_nPtsAllGs() suffices.)
    
    void updateGraphCtls();
    void doPauseUnpause(int num, bool updateCtls = true);
    void computeGraphMouseOverVars(unsigned num, double & y,
                                   double & mean, double & stdev, double & rms,
                                   const char * & unit);
    static int parseGraphNum(QObject *gl_graph_instance);
    void sharedCtor(DAQ::Params & p, bool isSaving, int graphUpdateRateHz);

	void retileGraphsAccordingToSorting();
	void setupGraph(int num, int firstExtraChan);
	
	QString getGraphSettingsKey() const;
	void loadGraphSettings();
	void saveGraphSettings();


	static int NumGraphsPerGraphTab[DAQ::N_Modes];
	static void SetupNumGraphsPerGraphTab();

    int getNumGraphsPerGraphTab() const;

    void openCustomChanset(const QVector<unsigned> & ids_of_graphs);

    volatile bool threadsafe_is_visible;

    int NUM_GRAPHS_PER_GRAPH_TAB, nGraphTabs, nColsGraphTab, nRowsGraphTab;

	
    DAQ::Params & params;
    const bool useTabs;
    QWidget *nonTabWidget; ///< for the useTabs=false mode...!
	QTabWidget *tabWidget;
	QStackedWidget *stackedWidget; QComboBox *stackedCombo;
    QVector<QWidget *> graphTabs;
    QToolBar *graphCtls;
    QPushButton *chanBut;
	QLabel *chanLbl;
    QDoubleSpinBox *graphYScale, *graphSecs;
    QCheckBox *highPassChk, *toggleSaveChk, *downsampleChk;
    QLineEdit *saveFileLE;
    QPushButton *graphColorBut;
    QVector<Vec2fWrapBuffer> points;
    QVector<GLGraph *> graphs;
	QVector<QCheckBox *> chks; /// checkboxes for above graphs!
    QVector<QFrame *> graphFrames;
    QVector<bool> pausedGraphs;
    QVector<double> graphTimesSecs;
    struct GraphStats {
        double s1; ///< sum of values
        double s2; ///< sum of squares of values
        unsigned num; ///< total number of values
        GraphStats()  { clear(); }
        void clear() { s1 = s2 = num = 0; }
        double mean() const { return s1/double(num); }
        double rms2() const { return s2/double(num); }
        double rms() const;
        double stdDev() const;
    };
    QVector<GraphStats> graphStats; ///< mean/stddev stuff
	QVector<GLGraphState> graphStates; ///< used to maintain internal glgraph state for graph re-use...
    volatile double downsampleRatio;
    int dsLeftOver, dsMaxNPts;
    double tNow, tLast, tAvg, tNum;
    int pdChan, firstExtraChan;
    QAction *pauseAct, *maxAct, *applyAllAct;
    GLGraph *maximized; ///< if not null, a graph is maximized 
    HPFilter *filter;
    Vec2 lastMousePos;
    int lastMouseOverGraph;
    int selectedGraph;
	QLed *stimTrigLed, *pdTrigLed;
    QCheckBox *trigOverrideChk;
    bool modeCaresAboutSGL, modeCaresAboutPD;
    bool suppressRecursive;
	QVector <int> sorting, naming;
	QSet<GLGraph *> extraGraphs;
    std::vector<int16> scanTmp;
    QVector<unsigned> lastCustomChanset;
    QDoubleSpinBox *downsamplekHz;

    mutable QMutex graphsMut; ///< recursive mutex.  locked whenever this class accesses graph data.  used because we are transitioning over to a threaded graphing data reader model as of Feb. 2016
};


#endif

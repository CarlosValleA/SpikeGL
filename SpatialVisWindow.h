#ifndef SpatialVisWindow_H
#define SpatialVisWindow_H

#include <QMainWindow>
#include "DAQ.h"
#include "GLSpatialVis.h"
#include "TypeDefs.h"
#include "VecWrapBuffer.h"
#include <QVector>
#include <QMap>
#include <vector>
#include <QSet>
#include <QColor.h>
#include "StimGL_SpikeGL_Integration.h"
#include <QMutex>
#include <QMutexLocker>
#include "GenericGrapher.h"

class QToolBar;
class QLabel;
class QAction;
class QFrame;
class QPushButton;
class QSpinBox;
class QSlider;
class QCheckBox;

class SpatialVisWindow : public QMainWindow, public GenericGrapher
{
    Q_OBJECT
public:
    SpatialVisWindow(DAQ::Params & params, const Vec2i & xy_dims, unsigned selection_box_width, QWidget *parent = 0, int updateRateHz = -1 /* if >0, update graph this many times per second otherwise use default */, bool suppressExtra = false /* if true, don't show extra chans in spatial vis insteead they are selectable via a button*/);
    ~SpatialVisWindow();
	
    void setSelectionBoxRedefine(bool enabled) { can_redefine_selection_box = enabled; }
    bool isSelectionBoxRedefine() const { return can_redefine_selection_box; }
    void setSelectionEnabled(bool en) { click_to_select = en; }
    bool isSelectionEnabled() const { return click_to_select; }

    /* virtual */ void putScans(const int16 *data, unsigned data_size_samps, u64 firstSamp);
    /* virtual */ bool threadsafeIsVisible() const { return threadsafe_is_visible; }
    /* virtual */ bool caresAboutSkippedScans() const { return false; }
    /* virtual */ const char *grapherName() const { return "SpatialVisWindow"; }

    void setStaticBlockLayout(int ncols, int nrows);

    QColor & glyphColor1() { return fg; }
    QColor & glyphColor2() { return fg2; }

    void selectChansStartingAt(int chan);

    void setGraphTimesSecs(const QVector<double> & times) { QMutexLocker l(&mut); if (times.size() >= nvai) graphTimes = times; }

public slots:
    void setSorting(const QVector<int> & sorting, const QVector<int> & naming);
    void setGraphTimeSecs(int graphId, double secs) { QMutexLocker l(&mut); if (graphId > -1 && graphId < graphTimes.size()) graphTimes[graphId] = secs; }
    void setAutoScale(bool);
    void setDownsampleRatio(double ratio);

signals:
	void channelsSelected(const QVector<unsigned> & ids);
	
protected:
	// virtual from parent class
	void resizeEvent(QResizeEvent *event);
	void keyPressEvent(QKeyEvent *event);
    void closeEvent(QCloseEvent *);
    void showEvent(QShowEvent *);
    void hideEvent(QHideEvent *);

private slots:
    void updateGraph();
    void selectChansCenteredOn(int chan);
    void selectChansFromTopLeft(int chan);

    void mouseOverGraph(double x, double y);
    void mouseClickGraph(double x, double y);
    void mouseReleaseGraph(double x, double y);
    void mouseDoubleClickGraph(double x, double y);
    void updateMouseOver(); // called periodically every 1s
	void updateToolBar();
	void colorButPressed();
    void chanLayoutChanged();
	void overlayChecked(bool);
	void setOverlayAlpha(int);
	void ovlUpdate();
	void overlayButPushed();
	void ovlFFChecked(bool);
	void ovlAlphaChanged(int);
	void ovlFpsChanged(int);
    void gotoExtraAIClicked();

private:	
	int pos2ChanId(double x, double y) const;
    Vec2 chanId2Pos(const int chanId) const;
    Vec4 chanBoundingRect(int chanId) const;
    Vec4 chanBoundingRectNoMargins(int chanId) const;
    Vec2 chanMargins() const;
	void updateGlyphSize();
	void selClear();
	
	void setupGridlines();
	
	void saveSettings();
	void loadSettings();
	Vec2 glyphMargins01Coords() const;
	void ovlSetNoData();
	
    volatile bool threadsafe_is_visible;

    DAQ::Params & params;
    int nvai, nextra;
    const bool suppressExtra; ///< iff true, extra chans not visible in spatial vis but selectable via a button in toolbar
    int nbx, nby;
    Vec2i selectionDims; // the size of the selection box, in terms of number of channels    
    bool didSelDimsDefine, can_redefine_selection_box, click_to_select;
    Vec2 mouseDownAt;
    QVector<Vec2> points;
    QVector<Vec4f> colors;
	QVector<double> chanVolts;
    QVector<int16> chanRawSamps;
    GLSpatialVis * graph;
    QFrame * graphFrame;
	QColor fg, fg2;
	QLabel *statusLabel;
	int mouseOverChan;
	QVector<unsigned> selIdxs;
	
	QToolBar *toolBar;
    QPushButton *colorBut;
	QSpinBox *sbCols, *sbRows;
    QCheckBox *overlayChk, *ovlFFChk, *autoScaleChk;
	QLabel *ovlAlphaLbl;
	QPushButton *overlayBut;
	QSlider *overlayAlpha;
	QLabel *ovlfpsTit, *ovlfpsLimit;
	QSlider *ovlFps;
		
	StimGL_SpikeGL_Integration::FrameShare fshare;
	GLuint last_fs_frame_num;
	quint64 last_fs_frame_tsc;
	Avg frameDelayAvg;
	QString fdelayStr;
    QVector<int> sorting, revsorting, naming;

    volatile bool autoScaleColorRange;
    QVector<double> graphTimes;
    struct ChanMinMax {
        int16 smin, smax;
        ChanMinMax() : smin(32767), smax(-32768) {}
    };
    typedef QVector<ChanMinMax> ChanMinMaxs;
    typedef QMap<double, ChanMinMaxs> ChunkChanMinMaxs;
    ChunkChanMinMaxs chunkChanMinMaxs;
    volatile double downsampleRatio;

    QMutex mut;    
};


#endif

/*
 *  FileViewerWindow.cpp
 *  SpikeGL
 *
 *  Created by calin on 10/26/10.
 *  Copyright 2010 Calin Culianu <calin.culianu@gmail.com>. All rights reserved.
 *
 */

#include "FileViewerWindow.h"
#include "GLGraph.h"
#include "MainApp.h"
#include "ConsoleWindow.h"
#include <QFileInfo>
#include <QFile>
#include <QScrollArea>
#include <limits.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QToolBar>
#include <QMenuBar>
#include <QStatusBar>
#include "close_but_16px.xpm"
#include <QPixmap>
#include <QTimer>
#include <QCursor>
#include <QResizeEvent>
#include <QScrollBar>
#include <QAction>
#include <QMenu>
#include "ExportDialogController.h"
#include <QProgressDialog>
#include <QTextStream>
#include <QMessageBox>
#include <QFrame>
#include <QCheckBox>
#include <QPushButton>
#include "HPFilter.h"
#include "ClickableLabel.h"
#include <QKeyEvent>
#include "ui_FVW_OptionsDialog.h"
#include <QDialog>
#include <QComboBox>
#include <stdio.h>
#include "ui_FVW_Readme.h"


const QString FileViewerWindow::viewModeNames[] = {
	"Tiled", "Stacked", "Stacked Large", "Stacked Huge"
};

const QString FileViewerWindow::colorSchemeNames[] = {
	"Ice", "Fire", "Green", "BlackWhite", "Classic"
};


class TaggableLabel : public QLabel
{
public:
	TaggableLabel(QWidget *parent, Qt::WindowFlags f = 0) : QLabel(parent, f), tagPtr(0) {}
	void setTag(void *ptr) { tagPtr = ptr; }
	void *tag() const { return tagPtr; }
private:
	mutable void *tagPtr;
};

FileViewerWindow::FileViewerWindow()
: QMainWindow(0), pscale(1), mouseOverT(-1.), mouseOverV(0), mouseOverGNum(-1), mouseButtonIsDown(false), dontKillSelection(false), hpfilter(0), arrowKeyFactor(.1), pgKeyFactor(.5), n_graphs_pg(8), curr_graph_page(0), showReadme(true)
{	
    readmeDlg = 0; readme=0;

	QWidget *cw = new QWidget(this);
	QVBoxLayout *l = new QVBoxLayout(cw);
	setCentralWidget(cw);
	
	scrollArea = new QScrollArea(cw);	
	scrollArea->installEventFilter(this);
	graphParent = new QWidget(scrollArea);
	scrollArea->setWidget(graphParent);
	l->addWidget(scrollArea, 1);

    // all channels hidden label
    allChannelsHiddenLbl = new QLabel("<b><i><font size=+1 color=#6666cc>All graphs on this page are hidden. Use the \"channels\" menu to unhide.</font></i></b>", cw);
    l->addWidget(allChannelsHiddenLbl,0);
    allChannelsHiddenLbl->setHidden(true);
	
	// bottom slider
	QWidget *w = new QWidget(cw);
	QHBoxLayout *hl = new QHBoxLayout(w);

    maximizedLbl = new QLabel("<b><i>MAXIMIZED (right-click to restore)</i></b>", w);
    hl->addWidget(maximizedLbl);
    maximizedLbl->setHidden(true);
    pageCB = new QComboBox(w);
    hl->addWidget(pageCB);
    QLabel *lbl = pgLbl = new QLabel("Graphs/Page", w);
    lbl->setToolTip("The number of graphs to display on-screen at a time. Page through graphs using the 'tabwidget' control.");
    hl->addWidget(lbl);
    graphPgSz = new QSpinBox(w);
    graphPgSz->setMinimum(8);
    graphPgSz->setMaximum(256);
    graphPgSz->setToolTip("The number of graphs to display on-screen at a time. Page through graphs using the 'tabwidget' control.");
    hl->addWidget(graphPgSz);
    Connect(graphPgSz, SIGNAL(valueChanged(int)), this, SLOT(graphsPerPageChanged(int)));

    lbl = new QLabel("File position: ", w);
	hl->addWidget(lbl);
	lbl = new QLabel("scans", w);
	posScansSB = new QSpinBox(w);
	hl->addWidget(lbl, 0, Qt::AlignRight);
	hl->addWidget(posScansSB, 0, Qt::AlignLeft);
	lbl = totScansLbl =  new QLabel("", w);
	hl->addWidget(lbl, 0, Qt::AlignLeft);
	hl->addSpacing(25);
	lbl = new QLabel("secs", w);
	posSecsSB = new QDoubleSpinBox(w);
	posSecsSB->setDecimals(3);
	posSecsSB->setSingleStep(0.001);
	hl->addWidget(lbl, 0, Qt::AlignRight);
	hl->addWidget(posSecsSB, 0, Qt::AlignLeft);
	lbl = totSecsLbl =  new QLabel("", w);
	hl->addWidget(lbl, 0, Qt::AlignLeft);
	hl->addSpacing(10);
	
	posSlider = new QSlider(Qt::Horizontal, w);
	posSlider->setMinimum(0);
	posSlider->setMaximum(1000);
	hl->addWidget(posSlider, 1);
	Connect(posSlider, SIGNAL(valueChanged(int)), this, SLOT(setFilePos(int)));
	Connect(posScansSB, SIGNAL(valueChanged(int)), this, SLOT(setFilePos(int)));
	Connect(posSecsSB, SIGNAL(valueChanged(double)), this, SLOT(setFilePosSecs(double)));
	
	toolBar = addToolBar("Graph Controls");

	lbl = new QLabel("Graph:", toolBar);
	toolBar->addWidget(lbl);
	lbl = graphNameLbl = new QLabel("<font size=-1>0 Ch. 0</font>", toolBar);
	toolBar->addWidget(lbl);
	toolBar->addSeparator();

	lbl = new QLabel("<font size=-1>X-Scale (secs):</font>", toolBar);
    toolBar->addWidget(lbl);	
    xScaleSB = new QDoubleSpinBox(toolBar);
    xScaleSB->setDecimals(4);
    xScaleSB->setRange(.0001, 1e6);
    xScaleSB->setSingleStep(.25);    
    toolBar->addWidget(xScaleSB);
	
    lbl = new QLabel("<font size=-1>Y-Scale (factor):</font>", toolBar);
    toolBar->addWidget(lbl);
    yScaleSB = new QDoubleSpinBox(toolBar);
    yScaleSB->setRange(.01, 100.0);
    yScaleSB->setSingleStep(0.25);
    toolBar->addWidget(yScaleSB);

	Connect(xScaleSB, SIGNAL(valueChanged(double)), this, SLOT(setXScaleSecs(double)));
    Connect(yScaleSB, SIGNAL(valueChanged(double)), this, SLOT(setYScale(double)));
	
	
	toolBar->addSeparator();
	toolBar->addWidget(lbl = new QLabel("<font size=-1>N Divs:</font>", toolBar));
	toolBar->addWidget(nDivsSB = new QSpinBox(toolBar));
	nDivsSB->setMinimum(0);
	nDivsSB->setMaximum(10);
	Connect(nDivsSB, SIGNAL(valueChanged(int)), this, SLOT(setNDivs(int)));
	xDivLbl = new QLabel("", toolBar);
	toolBar->addWidget(xDivLbl);
	yDivLbl = new QLabel("", toolBar);
	toolBar->addWidget(yDivLbl);
	
	toolBar->addWidget(new QLabel("<font size=-1>Gain:</font>", toolBar));
	toolBar->addWidget(auxGainSB = new QDoubleSpinBox(toolBar));
	auxGainSB->setDecimals(3);
	auxGainSB->setRange(0.001,1e6);
	Connect(auxGainSB, SIGNAL(valueChanged(double)), this, SLOT(setAuxGain(double)));

	toolBar->addSeparator();
    highPassChk = new QCheckBox(toolBar);
	lbl = new ClickableLabel("<font size=-1>Filter &lt; 300Hz</font>", toolBar);
	Connect(lbl, SIGNAL(clicked()), this, SLOT(hpfLblClk()));
    toolBar->addWidget(highPassChk);
	toolBar->addWidget(lbl);
    Connect(highPassChk, SIGNAL(clicked(bool)), this, SLOT(hpfChk(bool)));

	dcfilterChk = new QCheckBox(toolBar);
	lbl = new ClickableLabel("<font size=-1>DC Filter</font>", toolBar);
	Connect(lbl, SIGNAL(clicked()), this, SLOT(dcfLblClk()));
    toolBar->addWidget(dcfilterChk);
	toolBar->addWidget(lbl);
    Connect(dcfilterChk, SIGNAL(clicked(bool)), this, SLOT(dcfChk(bool)));
	
	toolBar->addSeparator();
	QPushButton *but = new QPushButton("Set All", toolBar);
	but->setToolTip("Apply these graph settings to all graphs");
	toolBar->addWidget(but);
	Connect(but, SIGNAL(clicked(bool)), this, SLOT(applyAllSlot()));
	
	
	l->addWidget(w);
	
	
	statusBar(); // creates one implicitly

//#ifdef Q_OS_MACX
//	// shared menu bar on OSX
//	QMenuBar *mb = mainApp()->console()->menuBar();
//#else
//	// otherwise window-specific menu bar
	QMenuBar *mb = menuBar();
//#endif
	
	QMenu *m = mb->addMenu("File");
	m->addAction("&Open...", this, SLOT(fileOpenMenuSlot()));
	m->addAction("Misc. Options...", this, SLOT(fileOptionsMenuSlot()));
	
	m = mb->addMenu("Color &Scheme");
	for (int i = 0; i < (int)N_ColorScheme; ++i) {
		QAction * a = colorSchemeActions[i] = m->addAction(colorSchemeNames[i], this, SLOT(colorSchemeMenuSlot()));
		a->setCheckable(true);
	}
	m = mb->addMenu("&View Mode");
	sortByIntan = m->addAction("Sort Graphs by Intan Channel", this, SLOT(sortGraphsByIntan()));
	sortByElectrode = m->addAction("Sort Graphs by Electrode Id", this, SLOT(sortGraphsByElectrode()));
	sortByIntan->setCheckable(true);
	sortByIntan->setChecked(true);
	sortByElectrode->setCheckable(true);
	m->addSeparator();
	
	for (int i = 0; i < (int)N_ViewMode; ++i) {
		QAction *a = viewModeActions[i] = m->addAction(viewModeNames[i], this, SLOT(viewModeMenuSlot()));
		a->setCheckable(true);
	}
    channelsMenu = mb->addMenu("&Channels");
	channelsMenu->addAction("Show All", this, SLOT(showAllGraphs()));
    channelsMenu->addSeparator();
	
	closeLbl = new TaggableLabel(0, (Qt::WindowFlags)Qt::FramelessWindowHint);
	QPixmap pm(close_but_16px);
	closeLbl->setPixmap(pm);
	closeLbl->resize(pm.size());
	closeLbl->setToolTip("Hide this graph");
	closeLbl->setCursor(Qt::ArrowCursor);
	closeLbl->hide();
	closeLbl->setTag(0);
	closeLbl->move(-100,-100);
	closeLbl->installEventFilter(this);
	
	didLayout = false;
	
	hideCloseTimer = new QTimer(this);
	hideCloseTimer->setInterval(1000);
	hideCloseTimer->setSingleShot(false);
	Connect(hideCloseTimer, SIGNAL(timeout()), this, SLOT(hideCloseTimeout()));
	
	exportAction = new QAction("Export...", this);
	Connect(exportAction, SIGNAL(triggered()), this, SLOT(exportSlot()));
	exportSelectionAction = new QAction("Export Selection", this);
	Connect(exportSelectionAction, SIGNAL(triggered()), this, SLOT(exportSlot()));
	exportSelectionAction->setEnabled(false);
    maxunmaxAction = new QAction("Toggle Maximize", this);
    Connect(maxunmaxAction, SIGNAL(triggered()), this, SLOT(toggleMaximize()));

	
	exportCtl = new ExportDialogController(this);        
}

FileViewerWindow::~FileViewerWindow()
{	
	/// scrollArea and graphParent automatically deleted here because they are children of us.
	delete hpfilter;
    // these aren't children, so delete them
    delete readmeDlg, readmeDlg = 0;
    delete readme, readme = 0;
}


bool FileViewerWindow::viewFile(const QString & fname, QString *errMsg /* = 0 */)
{	
	QString fname_no_path = QFileInfo(fname).fileName();
	if (!dataFile.openForRead(fname)) {
		QString err = fname_no_path + ": miscellaneous error on open."; 
		if (errMsg) *errMsg = err;
		Error() << err;
		return false;
	}
	if (!dataFile.scanCount()) {
		QString err = fname_no_path + " is empty!";
		if (errMsg) *errMsg = err;
		Error() << err;
		return false; // file is empty
	}
	
	setWindowTitle(QString(APPNAME) + QString(" File Viewer - ") + QFileInfo(fname_no_path).fileName() + " " 
				   + QString::number(dataFile.numChans()) + " channels @ " 
				   + QString::number(dataFile.samplingRateHz()) 
				   + " Hz"
				   + ", " + QString::number(dataFile.scanCount()) + " scans"
				   );
	
	loadSettings();
	
	const bool reusing = graphFrames.size();
    for (int i = 0, n = graphHideUnhideActions.size(); i < n; ++i) {
        channelsMenu->removeAction(graphHideUnhideActions[i]);
        delete graphHideUnhideActions[i];
	}

	if (hpfilter) delete hpfilter;
	hpfilter = new HPFilter(dataFile.numChans(), 300);
    const int nChans = dataFile.numChans();
    graphHideUnhideActions.clear(); graphHideUnhideActions.resize(nChans);
    graphParams.clear(); graphParams.resize(nChans);
	defaultGain = dataFile.auxGain();
	chanMap = dataFile.chanMap();
    graphSorting.resize(nChans); revGraphSorting.resize(nChans);
    pageCB->blockSignals(true);
    pageCB->setCurrentIndex((curr_graph_page=0)); // set it back to first page..
    pageCB->blockSignals(false);

	if (electrodeSort) {
		sortByIntan->setChecked(false);
		sortByElectrode->setChecked(true);
	} else {
		sortByIntan->setChecked(true);
		sortByElectrode->setChecked(false);
	}
    for (int i = 0, n = nChans; i < n; ++i) {
        revGraphSorting[i] = graphSorting[i] = i; ///< identity sort initially
		graphParams[i].yZoom = defaultYZoom;
		graphParams[i].gain = defaultGain;
		graphParams[i].filter300Hz = false;
		graphParams[i].dcFilter = true;
        if (i < (int)chanMap.size())
            graphParams[i].objname = QString(QString("Graph ") + QString::number(i) + " Electrode " + QString::number(chanMap[i].electrodeId));
        else
            graphParams[i].objname = QString(QString("Graph ") + QString::number(i) + " ChanID " + QString::number(dataFile.channelIDs()[i]));

        QAction *a = new QAction(graphParams[i].objname, this);
        a->setObjectName(QString::number(i)); // hack sorta: tag it with its id for use later in the slot
        a->setCheckable(true);
        a->setChecked(true);
        channelsMenu->addAction(a);
        Connect(a, SIGNAL(triggered()), this, SLOT(hideUnhideGraphSlot()));
        graphHideUnhideActions[i] = a;
	}

    // NOTE THIS DELETES OLD GRAPHS & FRAMES (if any) and RE-CREATES GRAPHS
    redoGraphs();

    hiddenGraphs.fill(false, nChans);

    didLayout = false;
	pscale = 1;
	pos = 0;
	maximizedGraph = -1;
	selectionBegin = selectionEnd = -1;
	selectedGraph = mouseOverT = mouseOverV = mouseOverGNum = -1;

	selectGraph(0);

    repaginate();

	if (electrodeSort)
		sortGraphsByElectrode(); ///< implicitly calls layoutGraphs()
	else
		sortGraphsByIntan(); ///< implicitly calls layoutGraphs()
			
	pos = 0;
	if (reusing) configureMiscControls(true);
	setFilePos64(0, true);
	if (reusing) QTimer::singleShot(10, this, SLOT(updateData()));
	if (errMsg) *errMsg = QString::null;
	
    if (showReadme) {
        delete readmeDlg; delete readme;
        readmeDlg = new QDialog(0);
        readme = new Ui::FVW_Readme;
        readme->setupUi(readmeDlg);
        Connect(readmeDlg, SIGNAL(accepted()), this, SLOT(readmeDlgDone()));
    }

	return true;
}

bool FileViewerWindow::queryCloseOK() 
{
	// for now, always ok..
	return true;
}

void FileViewerWindow::loadSettings() 
{
    QSettings settings(SETTINGS_DOMAIN, SETTINGS_APP);
	
    settings.beginGroup("FileViewerWindow");
	viewMode = Tiled;
    int vm = settings.value("viewMode", (int)Tiled).toInt();
	if (vm >= 0 && vm < N_ViewMode) viewMode = (ViewMode)vm;
	nSecsZoom = settings.value("nSecsZoom", 4.0).toDouble();
	defaultYZoom = settings.value("yZoom", 1.0).toDouble();
	nDivs = settings.value("nDivs", 4).toUInt();
	int cs = settings.value("colorScheme", DefaultScheme).toInt();	
	if (cs < 0 || cs >= N_ColorScheme) cs = 0;
	int fmt = settings.value("lastExportFormat", ExportParams::Bin).toInt();
	if (fmt < 0 || fmt >= ExportParams::N_Format) fmt = ExportParams::Bin;    
	exportCtl->params.format = (ExportParams::Format)fmt;
    fmt = settings.value("lastExportCSVSubFormat", ExportParams::Real).toInt();
    if (fmt < 0 || fmt >= ExportParams::N_CSVSubFormat) fmt = ExportParams::Real;
    exportCtl->params.csvSubFormat = (ExportParams::CSVSubFormat)fmt;
	int lec = settings.value("lastExportChans", 1).toInt();
	if (lec < 0 || lec > 2) lec = 1;
	exportCtl->params.allChans = exportCtl->params.allShown = exportCtl->params.customSubset = false;
	if (lec == 0) exportCtl->params.allChans = true;
	else if (lec == 1) exportCtl->params.allShown = true;
	else if (lec == 2) exportCtl->params.customSubset = true;
	arrowKeyFactor = settings.value("arrowKeyFactor", .1).toDouble();
	if (fabs(arrowKeyFactor) < 0.0001) arrowKeyFactor = .1;
	pgKeyFactor = settings.value("pgKeyFactor", .5).toDouble();
	if (fabs(pgKeyFactor) < 0.0001) pgKeyFactor = .5;
	colorScheme = (ColorScheme)cs;
	xScaleSB->blockSignals(true);
	yScaleSB->blockSignals(true);
	nDivsSB->blockSignals(true);
	xScaleSB->setValue(nSecsZoom);
	yScaleSB->setValue(defaultYZoom);
	nDivsSB->setValue(nDivs);
	nDivsSB->blockSignals(false);
	xScaleSB->blockSignals(false);
	yScaleSB->blockSignals(false);	
	electrodeSort = settings.value("sortGraphsByElectrode", false).toBool();

    graphPgSz->blockSignals(true);
    graphPgSz->setValue((n_graphs_pg = settings.value("graphsPerPage", 36).toUInt()));
    graphPgSz->blockSignals(false);

    showReadme = !settings.value("neverShowReadme", false).toBool();
}

void FileViewerWindow::saveSettings()
{
    QSettings settings(SETTINGS_DOMAIN, SETTINGS_APP);
	
    settings.beginGroup("FileViewerWindow");
    settings.setValue("viewMode", (int)viewMode);	
	settings.setValue("colorScheme", (int)colorScheme);
	settings.setValue("lastExportFormat", int(exportCtl->params.format));
    settings.setValue("lastExportCSVSubFormat", int(exportCtl->params.csvSubFormat));

	settings.setValue("pgKeyFactor", pgKeyFactor);
	settings.setValue("arrowKeyFactor", arrowKeyFactor);
	int lec = 0;
	if (exportCtl->params.allShown) lec = 1;
	if (exportCtl->params.customSubset) lec = 2;
	settings.setValue("lastExportChans", lec);
	settings.setValue("sortGraphsByElectrode", electrodeSort);
    settings.setValue("graphsPerPage", graphsPerPage());
    settings.setValue("neverShowReadme", !showReadme);
}

void FileViewerWindow::layoutGraphs()
{	
	didLayout = false;
	closeLbl->hide();
	static const int padding = 2;
	const int n = graphFrames.size();

    graphParent->setHidden(false);
    allChannelsHiddenLbl->setHidden(true);

	if (graphParent->layout()) 
		delete graphParent->layout();

    int graphsVisible = 0;

	if (maximizedGraph > -1) {
		
		updateGeometry();
		scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);	
		// hide everything except for maximized graph
		for (int i = 0; i < n; ++i)
			graphFrames[i]->hide();
		const QSize sz = scrollArea->size();
		graphParent->resize(sz.width(), sz.height());
		graphFrames[maximizedGraph]->resize(sz.width(), sz.height());
		graphFrames[maximizedGraph]->move(0, 0);		
		graphFrames[maximizedGraph]->show();

        channelsMenu->setEnabled(false);
		
        graphsVisible = 1;

	} else {

        channelsMenu->setEnabled(true);

		// non-maximized more.. make sure non-hidden graphs are shown!
        for (int i = 0; i < n; ++i) {
            int idx = g2i(i);
            if (idx >= 0 && idx < hiddenGraphs.size() && !hiddenGraphs.testBit(idx))
                graphFrames[i]->show();
            else
                graphFrames[i]->hide();
        }
		
		if (viewMode == Tiled) {
			updateGeometry();
			const QSize sz = scrollArea->size();
			scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
			scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
			graphParent->resize(sz.width(), sz.height());
			QGridLayout *l = new QGridLayout(graphParent);
			l->setHorizontalSpacing(padding);
			l->setVerticalSpacing(padding);
			graphParent->setLayout(l);
			updateGeometry();
            int hiddenGraphCount = 0;
            for (int i = g2i(0); i < g2i(n); ++i)
                if (i < 0 || i >= hiddenGraphs.size() || hiddenGraphs.testBit(i)) ++hiddenGraphCount;
            const int ngraphs = graphFrames.size() - hiddenGraphCount;
			int nrows = int(sqrtf(ngraphs)), ncols = nrows;
			while (nrows*ncols < (int)ngraphs) {
				if (nrows > ncols) ++ncols;
				else ++nrows;
			}
			for (int r = 0, num = 0; r < nrows; ) {
				for (int c = 0; c < ncols; ++num) {
                    int idx = g2i(num);
					if (num >= graphs.size()) { r=nrows,c=ncols; break; } // break out of loop
                    if (idx >= 0 && idx < hiddenGraphs.size() && !hiddenGraphs.testBit(/*graphSorting[*/idx/*]*/)) {
                        l->addWidget(graphFrames[/*graphSorting[*/num/*]*/], r, c);
						if (++c >= ncols) ++r;
                    }
				}
			}

            graphsVisible = ngraphs;

		} else if (viewMode == StackedLarge || viewMode == Stacked || viewMode == StackedHuge) {
			const int stk_h = viewMode == StackedHuge ? 320 : (viewMode == StackedLarge ? 160 : 80);
			scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
			scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

			int w; 
            int shownGraphCount = n;
            for (int i = g2i(0); i < g2i(n); ++i)
                if (i < 0 || i >= hiddenGraphs.size() || hiddenGraphs.testBit(i)) --shownGraphCount;
            graphParent->resize(w=(scrollArea->width() - scrollArea->verticalScrollBar()->width() - 5), (stk_h + padding) * shownGraphCount);
			if (w <= 0) w = 1;
			int y = 0;

			for (int i = 0; i < n; ++i) {
                int idx = g2i(i);
                if (idx >= 0 && idx < hiddenGraphs.size() && !hiddenGraphs.testBit(idx/*graphSorting[i]*/)) {
                    QFrame *f = graphFrames[/*graphSorting[i]*/i];
					f->resize(w, stk_h);
					f->move(0,y);
					y += f->height() + padding;
                }
			}

            graphsVisible = shownGraphCount;
			
		}	
	}

    if (!graphsVisible) {
        graphParent->setHidden(true);
        allChannelsHiddenLbl->setHidden(false);
        updateGeometry();
        update();
    }
	
	didLayout = true;
}


void FileViewerWindow::updateData()
{
//    Debug() << "updateData() called..";

	const double srate = dataFile.samplingRateHz();

    QBitArray channelSubset = ~hiddenGraphs;
    const int nChans = graphParams.size();
    int nChansOn = channelSubset.count(true);
    const int nGraphs = graphs.size();
	QVector<int> chanIdsOn(nChansOn);
	std::vector<bool> chansToFilter(nChansOn, false), chansToDCSubtract(nChansOn, false);
	int maxW = 1;
	bool hasDCSubtract = false;
	for (int i = 0, j = 0; i < nChans; ++i) {
         const int gnum = i2g(i);
         if (channelSubset.testBit(i)) {
             if (gnum >= 0 && gnum < nGraphs) {
                // channel is on, and on-screen.  Read it.
                if (maxW < graphs[gnum]->width()) maxW = graphs[gnum]->width();
                if (graphParams[i].filter300Hz)
                    chansToFilter[j] = true;
                if (graphParams[i].dcFilter)
                    chansToDCSubtract[j] = true, hasDCSubtract = true;
                chanIdsOn[j++] = i;
             } else {
                 // channel is on, but not on-screen.  Don't read it.
                 channelSubset.clearBit(i);
                 --nChansOn;
             }
        }
	}
    chanIdsOn.resize(nChansOn);
    chansToFilter.resize(nChansOn);
    chansToDCSubtract.resize(nChansOn);
    HPFilter & filter(*hpfilter);
    if ((int)filter.scanSize() != nChansOn) filter.setScanSize(nChansOn);

	
    i64 num = nSecsZoom * srate;
	int downsample = num / maxW;
	downsample /= 2; // Make sure to 2x oversample the data in the graphs.  This, combined with the glBlend we enabled in our graphs should lead to sweetness in the graph detail.  
	if (downsample <= 0) downsample = 1;
	
    //double t0r = getTime();

	std::vector<int16> data;
	i64 nread = dataFile.readScans(data, pos, num, channelSubset, downsample);	
	
    //Debug() << "dataFile.readScans() took " << ((getTime()-t0r)*1e3) << " msec";

	if (nread < 0) {
		Error() << "Error reading data from input file!";
	} else {

        //double t0 = getTime();

		// demux for graphs
        for (int i = 0; i < nChansOn; ++i) {
            int chanId = chanIdsOn[i];
            int g = i2g(chanId);
            if (g >= 0 && g < nGraphs) {
                graphs[g]->setPoints(0);
                if (channelSubset.testBit(chanId) && graphBufs[g].capacity() != nread)
                    graphBufs[g].reserve(nread);
            }
		}

        const float smin(SHRT_MIN), usmax(USHRT_MAX);
        const float dt = 1.0f / (srate / float(downsample));
        std::vector<float> avgs(nChansOn, 0.f);
        const float avgfactor = 1.0f/float(nread);
        QVector<QVector<Vec2f> > & vecs (scratchVecs);
        if (vecs.size() < nChansOn) vecs.resize(nChansOn);
		for (int i = 0; i < nread; ++i) {
			filter.apply(&data[i * nChansOn], dt, chansToFilter);
			for (int j = 0; j < nChansOn; ++j) {                
                const int chanId = chanIdsOn[j];
                const int g = i2g(chanId);
                if (g >= 0 && g < nGraphs) {
                    if (vecs[j].size() < nread) vecs[j].resize(nread);
                    int16 & rawsampl = data[i * nChansOn + j];
                    const float sampl = ( ((float(rawsampl) + (-smin))/(usmax)) * (2.0f) ) - 1.0f;
                    Vec2f & vec(vecs[j][i]);
                    vec.x = float(i)/float(nread);
                    vec.y = sampl;
                    //Vec2f vec(float(i)/float(nread), sampl);
                    //graphBufs[g].putData(&vec, 1);
                    avgs[j] += sampl * avgfactor;
                }
			}
		}
        for (int j = 0; j < nChansOn; ++j) {
            const int chanId = chanIdsOn[j];
            const int g = i2g(chanId);
            if (g >= 0 && g < nGraphs) {
                graphBufs[g].putData(&vecs[j][0], nread);
            }
        }
		
		if (hasDCSubtract) {
			// if has dc subtract, apply subtract to channels that have it enabled
			for (int i = 0; i < nread; ++i) {
				for (int j = 0; j < nChansOn; ++j) {
					const int chanId = chanIdsOn[j];
                    const int g = i2g(chanId);
                    if (g >= 0 && g < nGraphs && chansToDCSubtract[j]) {
                        Vec2f & vec(graphBufs[g].at(i));
						vec.y -= avgs[j];
					}
				}
			}			
		}

        //Debug() << "remux & filter data took " << ((getTime()-t0)*1e3) << " msec";

	}
	
    //double t0 = getTime();

    for (int i = 0; i < nGraphs; ++i) {
        const int idx = g2i(i);
        if (idx >= 0 && idx < nChans) {
            graphs[i]->setYScale(graphParams[idx].yZoom);
            graphs[i]->setNumHGridLines(nDivs);
            graphs[i]->setNumVGridLines(nDivs);
            applyColorScheme(graphs[i]);
            graphs[i]->setPoints(&graphBufs[i]);
        }
	}

    //Debug() << "GLGraph::setPoints() took " << ((getTime()-t0)*1e3) << " msec";

	// set ranges, etc of various widgets
	configureMiscControls();
	
	// now update various widgets
	posScansSB->blockSignals(true);
	posSecsSB->blockSignals(true);
	posSlider->blockSignals(true);
	posScansSB->setValue(pos); // FIXME TODO: the value here is an int.  We want a 64-bit value...
	posSecsSB->setValue(timeFromPos(pos));
	posSlider->setValue(pos);
	posScansSB->blockSignals(false);
	posSecsSB->blockSignals(false);
	posSlider->blockSignals(false);
	
	int ndigs = 1;
	qint64 sc = dataFile.scanCount();
	while (sc /= 10LL) ndigs++;
	QString fmt = "%0" + QString::number(ndigs) + "lld";
	totScansLbl->setText(QString("<font size=-1 face=fixed> to ") + QString().sprintf(fmt.toLatin1(),qint64(pos + nScansPerGraph())) + "</font> <font face=fixed size=-2>(out of " + QString::number(dataFile.scanCount()-1) + ")</font>");
	totSecsLbl->setText(QString("<font size=-1 face=fixed> to ") + QString::number(timeFromPos(pos + nScansPerGraph()), 'f', 3) + "</font> <font face=fixed size=-2>(out of " + QString::number(timeFromPos(dataFile.scanCount()-1), 'f', 3) + ")</font>");
	xDivLbl->setText(QString("<font size=-1>Secs/Div: ") + (!nDivs ? "-" : QString::number(nSecsZoom / nDivs)) + "</font>");
	QPair<double, double> yv = yVoltsAfterGain(selectedGraph);
    const double yZoom = selectedGraph > -1 ? graphParams[g2i(selectedGraph)].yZoom : 1.;
	yDivLbl->setText(QString("<font size=-1>Volts/Div: ") + (!nDivs ? "-" : QString::number(((yv.second-yv.first) / nDivs) / yZoom)) + "</font>");
	
	for (int i = 0; i < (int)N_ColorScheme; ++i) 
		colorSchemeActions[i]->setChecked(i == (int)colorScheme);	
	for (int i = 0; i < (int)N_ViewMode; ++i)
		viewModeActions[i]->setChecked(i == (int)viewMode);

    updateSelection(false);
	
    for (int i = 0; i < nGraphs; ++i)
        graphs[i]->update();
}

void FileViewerWindow::updateSelection() {
    updateSelection(true);
}

void FileViewerWindow::updateSelection(bool do_update)
{
    const double srate = dataFile.samplingRateHz();
    const i64 num = nSecsZoom * srate;
    const int nGraphs = graphs.size();

    // configure selection
    if (selectionEnd >= 0 && selectionEnd >= selectionBegin && selectionEnd < (qint64)dataFile.scanCount()
        && selectionBegin >= 0 && selectionBegin < (qint64)dataFile.scanCount()) {
        // transform selection from scans to 0->1 value in graph
        const double gselbeg = (selectionBegin - pos) / double(num),
                     gselend = (selectionEnd - pos) / double(num);

        for (int i = 0; i < nGraphs; ++i) {
            graphs[i]->setSelectionEnabled(true);
            graphs[i]->setSelectionRange(gselbeg, gselend);
            if (do_update && !graphs[i]->isHidden()) graphs[i]->update();
        }
        exportSelectionAction->setEnabled(true);
    } else {
        exportSelectionAction->setEnabled(false);
        for (int i = 0; i < nGraphs; ++i) {
            graphs[i]->setSelectionEnabled(false);
            if (do_update && !graphs[i]->isHidden()) graphs[i]->update();
        }
    }

    printStatusMessage();
}


double FileViewerWindow::timeFromPos(qint64 p) const 
{
	return p / dataFile.samplingRateHz();
}

qint64 FileViewerWindow::posFromTime(double s) const 
{
	return dataFile.samplingRateHz() * s;
}

void FileViewerWindow::setFilePos(int p) 
{
	setFilePos64(p);
}

void FileViewerWindow::setFilePos64(qint64 p, bool noupdate) 
{
	if (p < 0) p = 0;
	pos = qint64(p) * pscale;
	qint64 limit = dataFile.scanCount() - nScansPerGraph() - 1;
	if (limit < 0) limit = 0;
	if (pos > limit) pos = limit;
	if (!noupdate)
		updateData();
}

void FileViewerWindow::setFilePosSecs(double s)
{
	setFilePos64(posFromTime(s));
}

void FileViewerWindow::configureMiscControls(bool blockSignals)
{	
	xScaleSB->setDecimals(4);
	xScaleSB->setRange(0.0001, dataFile.fileTimeSecs());

	qint64 maxVal = (dataFile.scanCount()-1) - nScansPerGraph();
	if (maxVal < 0) maxVal = 0;

	if (blockSignals) {
		posScansSB->blockSignals(true);
		posSecsSB->blockSignals(true);
		posSlider->blockSignals(true);
	}		
		
	posScansSB->setMinimum(0);
	posScansSB->setMaximum(maxVal);	
	posSecsSB->setMinimum(0);
	posSecsSB->setMaximum(timeFromPos(maxVal));
	
	// figure out appropriate scale for the slider
	pscale = 1;
	while ( maxVal / pscale > qint64(INT_MAX))
		pscale = pscale==qint64(1) ? qint64(2) : pscale*pscale;	
	posSlider->setMaximum(maxVal);

	posScansSB->blockSignals(true);
	posSecsSB->blockSignals(true);
	posSlider->blockSignals(true);
	
	posSlider->setValue(pos);
	posScansSB->setValue(pos);
	posSecsSB->setValue(timeFromPos(pos));
	
	posScansSB->blockSignals(false);
	posSecsSB->blockSignals(false);
	posSlider->blockSignals(false);
}

qint64 FileViewerWindow::nScansPerGraph() const { return nSecsZoom * dataFile.samplingRateHz(); }

void FileViewerWindow::setXScaleSecs(double d) 
{ 
	nSecsZoom = d;
	updateData();
}

void FileViewerWindow::setYScale(double d) 
{ 
	if (selectedGraph < 0) return;
    graphParams[g2i(selectedGraph)].yZoom = d;
	updateData();
}

QPair<double, double> FileViewerWindow::yVoltsAfterGain(int g) const
{
	QPair<double, double> ret;
    const int chan = g2i(g);
	if (chan < 0 || chan >= (int)graphParams.size()) return ret;
	const double gain = graphParams[chan].gain;
	ret.first = dataFile.rangeMin(chan) / gain;
	ret.second = dataFile.rangeMax(chan) / gain;
	return ret;
}

void FileViewerWindow::setNDivs(int n)
{
	nDivs = n;
	updateData();
}

void FileViewerWindow::applyColorScheme(GLGraph *g)
{
	QColor fg, bg, grid;
	switch (colorScheme) {
		case Ice:
			fg = QColor(0x87, 0xce, 0xfa, 0x7f);
			bg.setRgbF(.15,.15,.15);
			grid.setRgbF(0.4,0.4,0.4);			
			break;
		case Fire:
			fg = QColor(0xfa, 0x87, 0x37, 0x7f);
			bg.setRgbF(.15,.15,.15);
			grid.setRgbF(0.4,0.4,0.4);			
			break;
		case Green:
			fg = QColor(0x09, 0xca, 0x09, 0x7f);
			bg.setRgbF(.07,.07,.07);
			grid.setRgbF(0.4,0.4,0.4);			
			break;
		case BlackWhite:
			fg = QColor(0xca, 0xca, 0xca, 0xc0);
			bg.setRgbF(.05,.05,.05);
			grid.setRgbF(0.4,0.4,0.4);			
			break;
		case Classic:
		default:
			bg = QColor(0x2f, 0x4f, 0x4f);
			fg = QColor(0xee, 0xdd, 0x82);
			grid = QColor(0x87, 0xce, 0xfa, 0x7f);			
			break;
	}
	g->bgColor() = bg;
	g->graphColor() = fg;
	g->gridColor() = grid;
}

void FileViewerWindow::colorSchemeMenuSlot()
{
	ColorScheme oldScheme = colorScheme;
	for (int i = 0; i < (int)N_ColorScheme; ++i) {
		if (sender() == colorSchemeActions[i]) {
			colorScheme = (ColorScheme)i;
			colorSchemeActions[i]->setChecked(true);
		} else {
			colorSchemeActions[i]->setChecked(false);			
		}
	}
	if (oldScheme != colorScheme) {
		saveSettings();
        if (selectedGraph > -1) selectGraph(selectedGraph);
        for (int i = 0; i < graphs.size(); ++i)
            applyColorScheme(graphs[i]), graphs[i]->updateGL();
	}
}

void FileViewerWindow::setAuxGain(double d)
{
	if (selectedGraph < 0) return;
    graphParams[g2i(selectedGraph)].gain = d;
	updateData();
}

void FileViewerWindow::mouseOverGraph(double x, double y)
{
	GLGraph *sendr = dynamic_cast<GLGraph *>(sender());
	if (!sendr) return;
	const int num = sendr->tag().toInt();
    const int idx = g2i(num);
	mouseOverGNum = num;
    if (num < 0 || num >= (int)graphs.size()) {
        statusBar()->clearMessage();
        return;
    }
	x = timeFromPos(pos) + x*nSecsZoom; // map x, originally a 0->1 value, to a time
	mouseOverT = x;
	y += 1.;
    y /= 2.;
    const double gain = graphParams[idx].gain;
    y = (y*(dataFile.rangeMax(idx)-dataFile.rangeMin(idx)) + dataFile.rangeMin(idx)) / gain;
	mouseOverV = y;


	// now, handle selection change
	if (mouseButtonIsDown) {
		qint64 p = posFromTime(x);
		if (p >= (qint64)dataFile.scanCount()) p = dataFile.scanCount()-1;
		else if (p < 0) p = 0;
		qint64 diff = 0;
		// slide graph over if we dragged selection past end of graph...
		if (p < pos) {
			diff = p - pos;
		} else if (p > pos + nScansPerGraph()) {
			diff = p - (pos + nScansPerGraph());
		}
        //Debug() << "mouse over graph " << num << " ch " << idx << " x=" << x << " y=" << y << " diff="<< diff << " selBeg="<<selectionBegin<<" selEnd="<<selectionEnd;

		if (diff) setFilePos64(pos+diff, true);
		if (selectionEnd < 0) selectionEnd = selectionBegin;
		if (p < selectionBegin) selectionBegin = p;
		else selectionEnd = p;
        if (diff) updateData();
        updateSelection();
	}

	printStatusMessage();
		
}

QString FileViewerWindow::generateGraphNameString(unsigned num, bool verbose) const
{
	if (num >= (unsigned)graphs.size()) return "";
    const int idx = g2i(num);
	QString chStr;
    const int chanId = dataFile.channelIDs()[idx];
    if (dataFile.daqMode() == DAQ::AIRegular) {
        chStr = dataFile.channelDisplayName(idx);
//        chStr.sprintf("AI%d", chanId);
    } else { // MUX mode
		const int m = dataFile.daqMode();
		const int first_non_mux_id = dataFile.isDualDevMode() && !dataFile.secondDevIsAuxOnly() ? (DAQ::ModeNumIntans[m]*2 * DAQ::ModeNumChansPerIntan[m]) : (DAQ::ModeNumIntans[m] * DAQ::ModeNumChansPerIntan[m]);
        if (idx < int(chanMap.size()) && chanId < first_non_mux_id) {
            const ChanMapDesc & desc = chanMap[idx];
			if (verbose) 
                chStr.sprintf("%u ChanID %d  [I%u_C%u e:%u]", idx, chanId, desc.intan,desc.intanCh,desc.electrodeId);
			else
                chStr.sprintf("%u Ch. %d [I%u_C%u e:%u]", idx, chanId, desc.intan, desc.intanCh, desc.electrodeId);
		} else {
			if (chanId == dataFile.pdChanID())
                chStr.sprintf("%u %s %d (PD)", idx, verbose ? "ChanID" : "Ch.", chanId);
			else
                chStr.sprintf("%u %s %d (AUX)", idx, verbose ? "ChanID" : "Ch.", chanId);
		}
    }
	return chStr;
}

void FileViewerWindow::printStatusMessage()
{
	const int num = mouseOverGNum;
	if (num < 0) return;
	double x = mouseOverT, y = mouseOverV;
	
	// check for millivolt..
    const char *unit = "V";
    const int idx = g2i(num);
    if (idx < 0 || idx >= graphParams.size()) return;
    const double gain = graphParams[idx].gain;
    if ((((dataFile.rangeMax(idx)-dataFile.rangeMin(idx)) + dataFile.rangeMin(idx)) / gain) < 1.0)
        unit = "mV",y *= 1000.;
	
    QString msg;
    QString chStr = generateGraphNameString(num, true);
	
	msg.sprintf("Mouse tracking Graph %s @ pos (%.4f s, %.4f %s)", chStr.toUtf8().constData(),x,y,unit);
	if (selectionBegin >= 0 && selectionEnd >= 0) {
		msg += QString().sprintf(" - Selection range scans: (%lld,%lld) secs: (%.4f, %.4f)",selectionBegin, selectionEnd, timeFromPos(selectionBegin), timeFromPos(selectionEnd));
	}
	
    statusBar()->showMessage(msg);	
}

void FileViewerWindow::mouseOverGraphInWindowCoords(GLGraph *g, int x, int y)
{
	if (!didLayout) return;
	if (maximizedGraph > -1) return; // no close label in maximized mode!
	const QSize sz = closeLbl->size();
	if (!g) return;
	if (x > g->width()-sz.width() && y < sz.height()) {
        //if (hiddenGraphs.count(true) >= graphs.size()-1) // on last graph, can't close
        //    return;
		closeLbl->hide();
		QPoint p (g->width()-sz.width(),0);
		p = g->mapToGlobal(p);
		closeLbl->move(p.x(),p.y());
		closeLbl->raise();
		closeLbl->show();
		closeLbl->setTag(g);
		hideCloseTimer->stop();
		hideCloseTimer->start();
	} else if (closeLbl->isVisible()) {
		// let the timer hide it! :)
	}
}

void FileViewerWindow::mouseOverGraphInWindowCoords(int x, int y)
{
	mouseOverGraphInWindowCoords(dynamic_cast<GLGraph *>(sender()), x, y);
}

void FileViewerWindow::hideGraph(int num)
{
    if (num >= 0 && num < graphFrames.size()) {
        graphFrames[num]->hide();
    }
    const int idx = g2i(num);
    if (idx < 0 || idx >= graphParams.size()) return;
    hiddenGraphs.setBit(idx);
    graphHideUnhideActions[idx]->setChecked(false);
	layoutGraphs();
	updateData();
}

void FileViewerWindow::showGraph(int num)
{
    QFrame *f = 0;
    if (num >= 0 && num < graphFrames.size())
        f = graphFrames[num];
    const int idx = g2i(num);
    if (idx < 0 || idx >= graphParams.size()) return;
    hiddenGraphs.clearBit(idx);
    graphHideUnhideActions[idx]->setChecked(true);
	layoutGraphs();
	updateData();
    if (f) f->show();
}

void FileViewerWindow::clickedCloseLbl(GLGraph *g)
{
	dontKillSelection = true;
	int num = g->tag().toInt();
	hideGraph(num);
	hideCloseTimeout();
}

void FileViewerWindow::clickedGraphInWindowCoords(int x, int y)
{
	if (!didLayout) return;
	GLGraph *g = dynamic_cast<GLGraph *>(sender());
	const QSize sz = closeLbl->size();
	if (!g) return;
	if (x > g->width()-sz.width()  && y < sz.height() )
		clickedCloseLbl(g);
}

void FileViewerWindow::showAllGraphs()
{
	closeLbl->hide();
	maximizedGraph = -1;
    const int n = hiddenGraphs.size();
	for (int i = 0; i < n; ++i) {
        hiddenGraphs.clearBit(i);
        graphHideUnhideActions[i]->setChecked(true);
	}
    for (int i = 0, n = graphFrames.size(); i < n; ++i)
        graphFrames[i]->show();
    layoutGraphs();
	QTimer::singleShot(10,this,SLOT(updateData()));
}

void FileViewerWindow::hideUnhideGraphSlot()
{
	QAction *s = dynamic_cast<QAction *>(sender());
	if (!s) return;
	bool ok = false;
    const int idx = s->objectName().toInt(&ok);
    if (!ok) return;
    const int num = i2g(idx);
	if (maximizedGraph == num) {
		maximizedGraph = -1;
	}

    //if (num >= 0 && num < graphs.size()) {
        if (hiddenGraphs.testBit(idx)) {
			// unhide			
			showGraph(num);
		} else {
            //if (hiddenGraphs.count(true) < graphs.size()-1) // if last graph, can't close
				// hide
				hideGraph(num);
		}
    //}

}

void FileViewerWindow::hideCloseTimeout()
{
	QPoint p = QCursor::pos();
	QRect r = closeLbl->geometry();
	r.moveTopLeft(closeLbl->mapToGlobal(QPoint(0,0)));
	if (r.contains(p)) {
		// still inside the close but, do nothing
	} else {
		closeLbl->hide();
		closeLbl->setTag(0);
		hideCloseTimer->stop();
	}
}

void FileViewerWindow::viewModeMenuSlot()
{
	QAction *a = dynamic_cast<QAction *>(sender());
	if (!a) return;
	for (int i = 0; i < (int)N_ViewMode; ++i) {
		if (a == viewModeActions[i]) {
			viewMode = (ViewMode)i;
			saveSettings();
			layoutGraphs();
			QTimer::singleShot(10, this, SLOT(updateData()));
			return;
		}
	}
}

void FileViewerWindow::resizeEvent(QResizeEvent *r)
{
	QMainWindow::resizeEvent(r);
	layoutGraphs();
}

void FileViewerWindow::resizeIt()
{
    resize(1050,640);
    QTimer::singleShot(10, this, SLOT(updateData()));
}

void FileViewerWindow::showEvent(QShowEvent *e)
{
	QMainWindow::showEvent(e);
	updateGeometry();
	QTimer::singleShot(10, this, SLOT(resizeIt()));
    if (readmeDlg) {
        QTimer::singleShot(100, readmeDlg, SLOT(show()));
    }
}

void FileViewerWindow::toggleMaximize()
{
	if (maximizedGraph > -1) {
        maximizedGraph = -1;
        pgLbl->setHidden(false);
        graphPgSz->setHidden(false); graphPgSz->setEnabled(true);
        pageCB->setHidden(false); pageCB->setEnabled(true);
        maximizedLbl->setHidden(true);
	} else {
		GLGraph *g = dynamic_cast<GLGraph *>(sender());
        int tomax = -1;
        if (g) {
            tomax = g->tag().toInt();
        } else {
            if (mouseOverGNum < 0) return;
            tomax = mouseOverGNum;
        }
        selectGraph(tomax);
        maximizedGraph = tomax;
        pgLbl->setHidden(true);
        graphPgSz->setHidden(true); graphPgSz->setEnabled(false);
        pageCB->setHidden(true); pageCB->setEnabled(false);
        maximizedLbl->setHidden(false);
    }
	// revert to old selection since we temporarily lost it due to spammed click events
    //selectionBegin = saved_selectionBegin;
    //selectionEnd = saved_selectionEnd;
	
	layoutGraphs();
    QTimer::singleShot(10, this, SLOT(updateData()));
}

void FileViewerWindow::mouseReleaseSlot(double x, double y)
{
	(void)x,(void)y;
	mouseButtonIsDown = false;
}

void FileViewerWindow::mouseClickSlot(double x, double y)
{	
	GLGraph *g = dynamic_cast<GLGraph *>(sender());
	if (!g) return;
	int oldSelection = selectedGraph;
	selectGraph(g->tag().toInt());	
	if (dontKillSelection) { dontKillSelection = false; return; }
	/* in order to disambiguate graph selection and region selection, dont do region selection stuff 
	   until they switched to this graph.  So first click, change graph.  Second click, support selection. */
	if (oldSelection != selectedGraph) return; 
	(void)x,(void)y;
	saved_selectionBegin = selectionBegin;
	saved_selectionEnd = selectionEnd;
	selectionBegin = qint64(x * nScansPerGraph()) + pos;
	selectionEnd = -1;
	mouseButtonIsDown = true;
    updateSelection();
}

void FileViewerWindow::exportSlot()
{
	QAction *a = dynamic_cast<QAction *>(sender());
	if (!a) return;
	
	loadSettings();
	
	ExportParams & p(exportCtl->params);
	p.nScans = dataFile.scanCount();
	p.nChans = dataFile.numChans();
	p.chansNotHidden = ~hiddenGraphs;
	p.selectionFrom = selectionBegin;
	p.selectionTo = selectionEnd;
	QFileInfo fi(dataFile.fileName());
	
	p.filename = fi.absoluteDir().canonicalPath() + "/" + fi.completeBaseName() + "_exported" + (p.format == ExportParams::Bin ? ".bin" : ".csv");
	
	if (a == exportSelectionAction || p.from || p.to) {
		p.allScans = false;
		p.from = p.to = 0;
	} else 
		p.allScans = true;

	if (!p.chanSubset.count(true)) {
		p.chanSubset.fill(false, p.nChans);
		p.chanSubset.setBit(mouseOverGNum, true);		
	}
	if (!p.allChans && !p.allShown && !p.customSubset) 
		p.customSubset = true;			
	
	ExportParams p_backup = p;
	if (exportCtl->exec()) {
		saveSettings();
		doExport(p);
	} else
		p = p_backup; // don't accept params
	
}

void FileViewerWindow::doExport(const ExportParams & p)
{
	qint64 nscans = (p.to - p.from) + 1;

	QProgressDialog progress(QString("Exporting ") + QString::number(nscans) + " scans...", "Abort Export", 0, 100, this);
	progress.setWindowModality(Qt::WindowModal);
	progress.setMinimumDuration(0);
	

	if (p.format == ExportParams::Bin) {
	
		DataFile out;
		QVector<unsigned> chanNumSubset;
		for (int i = 0; i < (int)p.chanSubset.size(); ++i) if (p.chanSubset.testBit(i)) chanNumSubset.push_back(i);
		if (!out.openForReWrite(dataFile, p.filename, chanNumSubset)) {
			Error() << "Could not open export file for write.";
			return;
		}
		
		// override the auxGain parameter from the input file with out custom gain setting
		out.setParam("auxGain", defaultGain);
		
		int prevVal = -1;
		std::vector<int16> scan;
		scan.resize(chanNumSubset.size());
		
		for (qint64 i = 0; i < nscans; ++i) {
			
			dataFile.readScans(scan, p.from+i, 1, p.chanSubset);
			out.writeScans(scan);
			int val = int((i*100LL)/nscans);
			if (val > prevVal) progress.setValue(prevVal = val);		
			if (progress.wasCanceled()) {
				QString f = out.fileName(), m = out.metaFileName();
				out.closeAndFinalize();
				QFile::remove(f);
				QFile::remove(m);
				return;
			}
		}
		out.closeAndFinalize();
		progress.setValue(100);
			
	} else if (p.format == ExportParams::Csv) {
		
		QFile out(p.filename);
		if (!out.open(QIODevice::WriteOnly|QIODevice::Truncate)) {			
			Error() << "Could not open export file for write.";
			return;
		}

		QTextStream outs(&out);
		
		int prevVal = -1;
		std::vector<int16> scan, chansOn;
		const int scansz = p.chanSubset.count(true);
		chansOn.reserve(scansz);
		for (int i = 0; i < (int)p.chanSubset.size(); ++i) 
			if (p.chanSubset.testBit(i)) chansOn.push_back(i);		
		scan.resize(scansz);
		
		for (qint64 i = 0; i < nscans; ++i) {
			
			dataFile.readScans(scan, p.from+i, 1, p.chanSubset);
            if (p.csvSubFormat == ExportParams::Real) {
                const double smin = double(SHRT_MIN), usmax = double(USHRT_MAX);
                for (int j = 0; j < scansz; ++j) {
                    const double minR = dataFile.rangeMin(j), maxR = dataFile.rangeMax(j);
                    double sampl = ( ((double(scan[j]) + (-smin))/(usmax+1.)) * (maxR-minR) ) + minR;
                    sampl /= graphParams[chansOn[j]].gain;
                    outs << sampl << ( j+1 < scansz ? "," : "");
                }
                outs << "\n";
            } else if (p.csvSubFormat == ExportParams::DecInt16) {
                for (int j = 0; j < scansz; ++j)
                    outs << scan[j] << ( j+1 < scansz ? "," : "");
                outs << "\n";
            } else if (p.csvSubFormat == ExportParams::HexUInt16) {
                char buf[64];
                for (int j = 0; j < scansz; ++j) {
#ifdef Q_OS_WINDOWS
                    _snprintf_s
#else
                    snprintf
#endif
                        (buf,sizeof(buf),"0x%04hx",(unsigned short)scan[j]);
                    outs << buf << (j+1 < scansz ? "," : "");
                }
                outs << "\n";
            }
			int val = int((i*100LL)/nscans);
			if (val > prevVal) progress.setValue(prevVal = val);		
			if (progress.wasCanceled()) {
				out.close();
				out.remove();
				return;
			}
		}
		
		progress.setValue(100);				
	}
	
	QMessageBox::information(this, "Export Complete", "Export completed successfully.");	
}

void FileViewerWindow::selectGraph(int num)
{
    bool earlyReturn = false;
    if (num >= graphFrames.size() || num < 0) {
        num = -1;
        earlyReturn = true;
    }
    int old = selectedGraph;
	QFrame *f;
    if (old > -1 && old < graphFrames.size()) {
		f = graphFrames[old];
		f->setLineWidth(0);
		f->setFrameStyle(QFrame::StyledPanel|QFrame::Plain);
        f->setObjectName("");
        f->setStyleSheet("border: 0px solid black");
	}
    selectedGraph = num;
    const int idx = g2i(num);
    if (idx < 0 || idx >= graphParams.size()) {
        selectedGraph = -1;
        earlyReturn = true;
    }
    if (earlyReturn) return;
	f = graphFrames[num];
    f->setFrameStyle(QFrame::Box|QFrame::Plain);
    f->setLineWidth(3);
    f->setObjectName("selectedFrame");
    QString sColor = "blue";
    switch (colorScheme) {
        case Ice: sColor = "#9999FF"; break;
        case Fire: sColor = "#FF6699"; break;
        case Green: sColor = "#6600CC"; break;
        case BlackWhite: sColor = "#66ff99"; break;
        case Classic:
        default: sColor = "#ffcc66"; break;
    }
    f->setStyleSheet(QString("#selectedFrame { border: 3px solid %1; }").arg(sColor));
    graphNameLbl->setText(QString("<font size=-1>") + generateGraphNameString(num, false) + "</font>");
	yScaleSB->blockSignals(true);
	auxGainSB->blockSignals(true);
	highPassChk->blockSignals(true);
	dcfilterChk->blockSignals(true);
    yScaleSB->setValue(graphParams[idx].yZoom);
    auxGainSB->setValue(graphParams[idx].gain);
    highPassChk->setChecked(graphParams[idx].filter300Hz);
    dcfilterChk->setChecked(graphParams[idx].dcFilter);
	yScaleSB->blockSignals(false);
	auxGainSB->blockSignals(false);
	highPassChk->blockSignals(false);
	dcfilterChk->blockSignals(false);
    //if (didLayout) updateData();
}

void FileViewerWindow::hpfChk(bool b)
{
	if (selectedGraph < 0) return;
    graphParams[g2i(selectedGraph)].filter300Hz = b;
	if (didLayout) updateData();
}

void FileViewerWindow::dcfChk(bool b)
{
	if (selectedGraph < 0) return;
    graphParams[g2i(selectedGraph)].dcFilter = b;
	if (didLayout) updateData();
}

void FileViewerWindow::hpfLblClk()
{
    highPassChk->setChecked(!graphParams[g2i(selectedGraph)].filter300Hz);
	hpfChk(highPassChk->isChecked());	
}

void FileViewerWindow::dcfLblClk()
{
    dcfilterChk->setChecked(!graphParams[g2i(selectedGraph)].dcFilter);
	dcfChk(dcfilterChk->isChecked());	
}

void FileViewerWindow::applyAllSlot()
{
	if (selectedGraph < 0) return;
    const int sidx = g2i(selectedGraph);
    const GraphParams & p (graphParams[sidx]);
	for (int i = 0; i < (int)graphParams.size(); ++i)
        if (i != sidx) graphParams[i] = p;
	if (didLayout) updateData();
}

void FileViewerWindow::fileOpenMenuSlot()
{
	mainApp()->fileOpen(this);
}

bool FileViewerWindow::eventFilter(QObject *obj, QEvent *event)
{
	if (obj == closeLbl && event->type() == QEvent::MouseButtonRelease) {
		GLGraph *g = reinterpret_cast<GLGraph *>(closeLbl->tag());
		if (g) clickedCloseLbl(g);
		return true;
	} else if (obj == scrollArea && event->type() == QEvent::KeyPress) {
		QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
		double sfactor = 0.0;
		switch(keyEvent->key()) {
			case Qt::Key_Left: sfactor = -arrowKeyFactor; break;
			case Qt::Key_Right: sfactor = arrowKeyFactor; break;
            case Qt::Key_PageUp: sfactor = -pgKeyFactor; break;
            case Qt::Key_PageDown: sfactor = pgKeyFactor; break;
		}
		if (fabs(sfactor) > 0.00001) {
			qint64 delta = nScansPerGraph() * sfactor;
			setFilePos64(pos+delta);
			return true;
		}
	} 
	return QMainWindow::eventFilter(obj, event);
}

void FileViewerWindow::fileOptionsMenuSlot()
{
	QDialog dlg;
	Ui::FVW_OptionsDialog ui;
	ui.setupUi(&dlg);
	ui.arrowSB->setValue(arrowKeyFactor);
	ui.pgSB->setValue(pgKeyFactor);
	if (dlg.exec() == QDialog::Accepted) {
		arrowKeyFactor = ui.arrowSB->value();
		pgKeyFactor = ui.pgSB->value();
		saveSettings();
	}
}

void FileViewerWindow::sortGraphsByIntan() {
	electrodeSort = false; saveSettings();
	sortByElectrode->setChecked(false);
    for (int i = 0; i < (int)graphSorting.size(); ++i)
        revGraphSorting[i] = graphSorting[i] = i;
	
	layoutGraphs();
    updateData();
}

void FileViewerWindow::sortGraphsByElectrode() {
	sortByIntan->setChecked(false);
	electrodeSort = true; saveSettings();
	QMap<int,int> eid2graph;
    const int cms = chanMap.size(), gs = graphParams.size();
	int i;
	for (i = 0; i < cms; ++i) {
		eid2graph[chanMap[i].electrodeId] = i;
	}
    QVector<int> & sorting (graphSorting), & rev(revGraphSorting);
    sorting.clear(); rev.clear();
    sorting.reserve(gs); rev.resize(gs);
    i=0;
    for (QMap<int,int>::iterator it = eid2graph.begin(); it != eid2graph.end(); ++it, ++i)
        sorting.push_back(it.value()), rev[it.value()] = i;
	for ( ; i < gs; ++i)
        sorting.push_back(i), rev[i] = i;
		
	layoutGraphs();
    updateData();
}

void FileViewerWindow::graphsPerPageChanged(int n) {
    n_graphs_pg = n;
    saveSettings();
    redoGraphs();
    repaginate();
}

void FileViewerWindow::repaginate() {
    disconnect(pageCB, SIGNAL(currentIndexChanged(int)), this, SLOT(pageChanged(int)));
    int oldPage = pageCB->currentIndex();
    if (oldPage < 0) oldPage = 0;
    pageCB->clear();
    int nChans = graphParams.size();
    int nPages = nChans / graphsPerPage() + (nChans%graphsPerPage()?1:0);
    for (int i = 0; i < nPages; ++i)
        pageCB->addItem(QString("Page %1/%2").arg(i+1).arg(nPages));
    if (oldPage < nPages) pageCB->setCurrentIndex(oldPage);
    else pageCB->setCurrentIndex(0);

    pageChanged(pageCB->currentIndex()); // implicitly does layout and data update, etc

    Connect(pageCB, SIGNAL(currentIndexChanged(int)), this, SLOT(pageChanged(int)));
}

void FileViewerWindow::pageChanged(int p) {
//    Debug() << "Page set to " << p+1;
    curr_graph_page = p;

    for (int i = 0; i < graphs.size(); ++i) {
        const int idx = g2i(i);
        if (idx >= 0 && idx < graphParams.size())
            graphs[i]->setToolTip(graphParams[idx].objname + " (right-click for context menu)");
    }
    if (didLayout) {
        layoutGraphs();
        QTimer::singleShot(10, this, SLOT(updateData()));
    }
    if (selectedGraph > -1) selectGraph(selectedGraph);
}

void FileViewerWindow::redoGraphs()
{
    // if reopening a file, delete all the old graphs first, and any auxilliary objects, and also do some cleanup/pre-init
    for (int i = 0, n = graphFrames.size(); i < n; ++i) {
        delete graphs[i]; // explicit to free up resources asap?
        delete graphFrames[i];
    }

    const int nGraphs = graphsPerPage();
    graphBufs.clear(); graphBufs.resize(nGraphs);
    graphs.clear(); graphs.resize(nGraphs);
    graphFrames.clear(); graphFrames.resize(nGraphs);

    for (int i = 0, n = nGraphs; i < n; ++i) {
        QFrame *f = graphFrames[i] = new QFrame(graphParent);
        f->setLineWidth(0);
        f->setFrameStyle(QFrame::StyledPanel|QFrame::Plain); // only enable frame when it's selected!
        QVBoxLayout *vbl = new QVBoxLayout(f);
        f->setLayout(vbl);
        graphs[i] = new GLGraph(f);
        vbl->addWidget(graphs[i],1);
        vbl->setSpacing(0);
        vbl->setContentsMargins(0,0,0,0);
        graphs[i]->setAutoUpdate(false);
        graphs[i]->setMouseTracking(true);
        const int idx = g2i(i);
        if (idx >= 0 && idx < graphParams.size())
            graphs[i]->setToolTip(graphParams[idx].objname + " (right-click for context menu)"); // TODO: set the name appropriately on layout!
        graphs[i]->setTag(i);
        graphs[i]->setCursor(Qt::CrossCursor);
        graphs[i]->addAction(exportAction);
        graphs[i]->addAction(exportSelectionAction);
        graphs[i]->addAction(maxunmaxAction);
        graphs[i]->setContextMenuPolicy(Qt::ActionsContextMenu);
        applyColorScheme(graphs[i]);
        Connect(graphs[i], SIGNAL(cursorOver(double,double)), this, SLOT(mouseOverGraph(double,double)));
        Connect(graphs[i], SIGNAL(cursorOverWindowCoords(int,int)), this, SLOT(mouseOverGraphInWindowCoords(int,int)));
        Connect(graphs[i], SIGNAL(clickedWindowCoords(int,int)), this, SLOT(clickedGraphInWindowCoords(int,int)));
        Connect(graphs[i], SIGNAL(clicked(double, double)), this, SLOT(mouseClickSlot(double,double)));
        Connect(graphs[i], SIGNAL(clickReleased(double, double)), this, SLOT(mouseReleaseSlot(double,double)));
    }
}

void FileViewerWindow::readmeDlgDone()
{
    if (readme->neverShowChk->isChecked()) showReadme = false, saveSettings();
    delete readmeDlg, readmeDlg = 0;
    delete readme, readme = 0;
}

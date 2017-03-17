#include "SpatialVisWindow.h"
#include <QVBoxLayout>
#include <QFrame>
#include <QTimer>
#include <math.h>
#include <QLabel>
#include <QStatusBar>
#include <QPushButton>
#include <QToolBar>
#include <QPainter>
#include <QPixmap>
#include <QIcon>
#include <QSettings>
#include <QColorDialog>
#include <QKeyEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QSpinBox>
#include <QSlider>
#include <QMatrix>
#include <QCheckBox>
#include <QMutexLocker>
#include <stdio.h>
#include "StimGL_SpikeGL_Integration.h"

#include "no_data.xpm"

#define SETTINGS_GROUP "SpatialVisWindow Settings"
#define GlyphScaleFactor 0.9725 /**< set this to less than 1 to give each glyph a margin */
#define BlockScaleFactor 1.0 /**< set this to less than 1 to give blocks a margin */

SpatialVisWindow::SpatialVisWindow(DAQ::Params & params, const Vec2i & xy_dims, unsigned selboxw, QWidget * parent, int updateRateHz, bool suppressExtra)
: QMainWindow(parent), threadsafe_is_visible(false), params(params), nvai(params.nVAIChans), nextra(params.nExtraChans1+params.nExtraChans2), suppressExtra(suppressExtra && nextra > 0),
  graph(0), graphFrame(0), mouseOverChan(-1), last_fs_frame_num(0xfDffffff), last_fs_frame_tsc(getAbsTimeNS()), mut(QMutex::Recursive)
{
    if (suppressExtra) {
        nvai -= nextra; // NEW 3/17/2017 -- spatial vis doesn't show the extra AI channels in Framegrabber acquisition mode!
        nextra = 0;
    }
    can_redefine_selection_box = false;
    click_to_select = false;
    mouseDownAt = Vec2(-1e6,-1e6);
	static bool registeredMetaType = false;
    autoScaleColorRange = false;
    downsampleRatio = 1.0;

	if (fshare.shm) {
		Log() << "SpatialVisWindow: " << (fshare.createdByThisInstance ? "Created" : "Attatched to pre-existing") <<  " StimGL 'frame share' memory segment, size: " << (double(fshare.size())/1024.0/1024.0) << "MB.";
		fshare.lock();
		const bool already_running = fshare.shm->spikegl_pid;
		fshare.unlock();
		if (!already_running || fshare.warnUserAlreadyRunning()) {
			fshare.lock();
			fshare.shm->enabled = 0;
			fshare.shm->spikegl_pid = Util::getPid();
			fshare.shm->dump_full_window = 0;
			ovlSetNoData();
			fshare.unlock();
		} else {
			Warning() << "Possible duplicate instance of SpikeGL, disabling 'frame share' in this instance.";
			fshare.detach();
		}
	} else {
		Error() << "INTERNAL ERROR: Could not attach to StimGL 'frame share' shared memory segment! FIXME!";
	}
	
	if (!registeredMetaType) {
		qRegisterMetaType<QVector<uint>	>("QVector<uint>");
		registeredMetaType = true;
	}

	setWindowTitle("Spatial Visualization");
	resize(900,675);

	toolBar = addToolBar("Spatial Visualization Controls");
	
    if (suppressExtra) {
        QPushButton *b = new QPushButton("Go To AI Chans", toolBar);
        QFont f (b->font());
        f.setPointSize(f.pointSize()-1);
        b->setFont(f);
        b->setToolTip("Click here to open the 'Extra AI' chans in the Graphs Window");
        toolBar->addWidget(b);
        toolBar->addSeparator();
        Connect(b, SIGNAL(clicked(bool)), this, SLOT(gotoExtraAIClicked()));
    }

	QLabel *label;
    toolBar->addWidget(label = new QLabel("Color: ", toolBar));
    toolBar->addWidget(colorBut = new QPushButton(toolBar));
	
    toolBar->addSeparator();

    toolBar->addWidget(autoScaleChk = new QCheckBox("AutoScale", toolBar));
    autoScaleChk->setToolTip("If checked, scale the displayed colors' dynamic ranges based on the MIN/MAX values seen in the last few seconds worth of data");

    toolBar->addSeparator();
	
	toolBar->addWidget(label = new QLabel("Layout: ", toolBar));
	toolBar->addWidget(sbCols = new QSpinBox(toolBar));
	toolBar->addWidget(new QLabel("x", toolBar));
	toolBar->addWidget(sbRows = new QSpinBox(toolBar));

	toolBar->addSeparator();
	
	toolBar->addWidget(overlayChk = new QCheckBox("StimGL Overlay:", toolBar));
	toolBar->addWidget(overlayAlpha = new QSlider(Qt::Horizontal, toolBar));
	toolBar->addWidget(ovlAlphaLbl = new QLabel(QString("50% %1").arg(QChar(ushort(0x03b1))), toolBar));
	QSize sz = overlayAlpha->maximumSize(); overlayAlpha->setMaximumSize(QSize(125,sz.height()));
	overlayAlpha->setRange(0,100);
	overlayAlpha->setSingleStep(1);
	overlayAlpha->setPageStep(10);
	overlayAlpha->setValue(50);
	
	overlayChk->setEnabled(!!fshare.shm);
	overlayAlpha->setEnabled(!!fshare.shm);
	
	Connect(overlayChk, SIGNAL(toggled(bool)), this, SLOT(overlayChecked(bool)));
	Connect(overlayAlpha, SIGNAL(valueChanged(int)), this, SLOT(setOverlayAlpha(int)));
	Connect(overlayAlpha, SIGNAL(valueChanged(int)), this, SLOT(ovlAlphaChanged(int)));
														
	toolBar->addWidget(overlayBut = new QPushButton("Set Overlay Box...",toolBar));
	QFont f = overlayBut->font();
	f.setPointSize(f.pointSize()-1);
	overlayBut->setFont(f);
	f = ovlAlphaLbl->font();
	f.setPointSize(f.pointSize()-1);
	ovlAlphaLbl->setFont(f);
	Connect(overlayBut, SIGNAL(clicked(bool)), this, SLOT(overlayButPushed()));
    Connect(colorBut, SIGNAL(clicked(bool)), this, SLOT(colorButPressed()));
    Connect(autoScaleChk, SIGNAL(toggled(bool)), this, SLOT(setAutoScale(bool)));
	
	toolBar->addWidget(ovlFFChk = new QCheckBox("Full Frame", toolBar));
	ovlFFChk->setEnabled(!!fshare.shm);
	ovlFFChk->setChecked(fshare.shm && fshare.shm->dump_full_window);
	Connect(ovlFFChk, SIGNAL(toggled(bool)), this, SLOT(ovlFFChecked(bool)));
	
	toolBar->addWidget(ovlfpsTit = new QLabel("FPS Limit:", toolBar));
	toolBar->addWidget(ovlFps = new QSlider(Qt::Horizontal, toolBar));
	ovlFps->setRange(1,121);
	ovlFps->setSingleStep(1);
	ovlFps->setPageStep(10);
	toolBar->addWidget(ovlfpsLimit = new QLabel("10", toolBar));
	ovlfpsTit->setEnabled(!!fshare.shm), ovlfpsLimit->setEnabled(!!fshare.shm), ovlFps->setEnabled(!!fshare.shm);			
	ovlFpsChanged(fshare.shm ? fshare.shm->frame_rate_limit : 0);
	Connect(ovlFps, SIGNAL(valueChanged(int)), this, SLOT(ovlFpsChanged(int)));
	
    if (!selboxw) selboxw = 1;
    selectionDims = Vec2i(selboxw,selboxw);

    nbx = xy_dims.x; nby = xy_dims.y; if (nbx <= 0) nbx = 1; if (nby <= 0) nby = 1;
    if (nbx*nby < nvai) {
        int ox = nbx, oy = nby;
        bool ff = true;
        while (nbx*nby < nvai) {
            if (ff) ++nbx; else ++nby;
            ff = !ff;
        }
        Warning() << "SpatialVisWindow: Passed-in dimensions (" << ox << "x" << oy << ") don't match the number of channels!  Auto-corrected to: " << nbx << "x" << nby;
    }
    points.resize(nvai);
    colors.resize(nvai);
	chanVolts.resize(nvai);
    chanRawSamps.resize(nvai);
    graphTimes.resize(nvai);
	
    // default sorting
    sorting.resize(nvai); naming.resize(nvai); revsorting.resize(nvai);

	for (int chanid = 0; chanid < nvai; ++chanid) {
        revsorting[chanid] = sorting[chanid] = naming[chanid] = chanid;
        points[chanid] = chanId2Pos(chanid);
        graphTimes[chanid] = 3.0;
	}

	sbRows->setValue(nby);
	sbCols->setValue(nbx);
	sbRows->setRange(1,100);
	sbCols->setRange(1,100);
    Connect(sbCols, SIGNAL(valueChanged(int)), this, SLOT(chanLayoutChanged()));
    Connect(sbRows, SIGNAL(valueChanged(int)), this, SLOT(chanLayoutChanged()));
	
	graphFrame = new QFrame(this);
	QVBoxLayout *bl = new QVBoxLayout(graphFrame);
    bl->setSpacing(0);
	bl->setContentsMargins(0,0,0,0);
	graph = new GLSpatialVis(graphFrame);
	bl->addWidget(graph,1);	
	setCentralWidget(graphFrame);

	setupGridlines();
	
	fg = QColor(0x87, 0xce, 0xfa, 0x7f);
	fg2 = QColor(0xfa, 0x87, 0x37, 0x7f);
	QColor bg, grid;
	bg.setRgbF(.15,.15,.15);
	grid.setRgbF(0.4,0.4,0.4);			
	
	graph->setBGColor(bg);
	graph->setGridColor(grid);
		
	QTimer *t;
	
	t = new QTimer(this);
	Connect(t, SIGNAL(timeout()), this, SLOT(ovlUpdate()));
	t->setSingleShot(false);
	t->start(1000.0/120.0);
		
	Connect(graph, SIGNAL(cursorOver(double, double)), this, SLOT(mouseOverGraph(double, double)));
	Connect(graph, SIGNAL(clicked(double, double)), this, SLOT(mouseClickGraph(double, double)));
	Connect(graph, SIGNAL(clickReleased(double, double)), this, SLOT(mouseReleaseGraph(double, double)));
	Connect(graph, SIGNAL(doubleClicked(double, double)), this, SLOT(mouseDoubleClickGraph(double, double)));
	
	QStatusBar *sb = statusBar();
	sb->addWidget(statusLabel = new QLabel(sb),1);
	
    if (updateRateHz <= 0 || updateRateHz >= 500)
        updateRateHz = DEF_TASK_READ_FREQ_HZ;

	t = new QTimer(this);
    Connect(t, SIGNAL(timeout()), this, SLOT(updateGraph()));
    t->setSingleShot(false);
    t->start(1000/updateRateHz);

    Debug() << "SpatialVisWindow: update rate set to " << updateRateHz << " Hz.";

	
    t = new QTimer(this);
    Connect(t, SIGNAL(timeout()), this, SLOT(updateMouseOver()));
    t->setSingleShot(false);
    t->start(1000/DEF_TASK_READ_FREQ_HZ);
	
	graph->setMouseTracking(true);	
	
	graph->setGlyphType(GLSpatialVis::Square);
	graph->setPoints(points); // setup graph points
	
	selClear();
	
	loadSettings();
	updateToolBar();
	
	graph->setAutoUpdate(false);
}

void SpatialVisWindow::setupGridlines()
{
	graph->setNumVGridLines(nbx);
	graph->setNumHGridLines(nby);
}

void SpatialVisWindow::setAutoScale(bool b) {
    autoScaleColorRange = b;
    saveSettings();
}

void SpatialVisWindow::resizeEvent (QResizeEvent * event)
{
	updateGlyphSize();
	QMainWindow::resizeEvent(event);
}

void SpatialVisWindow::updateGlyphSize()
{
	if (!graph) return;
    Vec4 br = chanBoundingRect(0);
	Vec2 bs = Vec2(br.v3-br.v1, br.v2-br.v4);
    int szx = fabs(bs.x * graph->width());
    int szy = fabs(bs.y * graph->height());
	szx *= GlyphScaleFactor, szy *= GlyphScaleFactor;
	if (szx < 1) szx = 1;
	if (szy < 1) szy = 1;
	graph->setGlyphSize(Vec2f(szx,szy));		
}

void SpatialVisWindow::setDownsampleRatio(double r)
{
    QMutexLocker l(&mut);
    downsampleRatio = r >= 1.0 ? r : 1.0;
//    Debug() << "SpatialVisWindow got new downsample ratio: " << downsampleRatio;
}

void SpatialVisWindow::putScans(const int16 *scans, unsigned scans_size_samps, u64 firstSamp)
{
    QMutexLocker l(&mut);

    //double t0 = getTime(); (void)t0;

    const int downSampleSkips = downsampleRatio > 1.0 ? qRound(downsampleRatio)-1 : 0;//params.srate >= 1000.0 ? qRound(params.srate/1000.0)-1 : 0;

    ChanMinMaxs cmm; cmm.resize(nvai);
    const bool doAutoScale = autoScaleColorRange;

    if (doAutoScale) {
        // bookkeeping -- keep track of min/max values seen, per channel for a window of time to determine scale..
        double  now = double(u64(firstSamp/nvai))/params.srate,
                maxAge = 0;
        for (int i = 0; i < graphTimes.size(); ++i) {
            if (graphTimes[i] > maxAge) maxAge = graphTimes[i];
        }
        // expire old chunk min/max's
        while (!chunkChanMinMaxs.empty() && (now-chunkChanMinMaxs.begin().key()) > maxAge)
            chunkChanMinMaxs.erase(chunkChanMinMaxs.begin());

        // now scan entire data chunk to update chanMinMax state...
        for (int i = 0, ch = 0; i < (int)scans_size_samps; /*nothing. i is incremented last line of loop below..*/) {
            if (scans[i] <= cmm[ch].smin) cmm[ch].smin = scans[i];
            if (scans[i] >= cmm[ch].smax) cmm[ch].smax = scans[i];
            if ((ch=(++i % nvai))==0)
                i += nvai*downSampleSkips;  // every Nth scan..
        }
        chunkChanMinMaxs[now] = cmm;

        // now, get an up-to-date min/max state per channel based on each channel's window size... by looking at all recent chunks
        for (ChunkChanMinMaxs::iterator it = chunkChanMinMaxs.begin(); it != chunkChanMinMaxs.end() && it.key() < now; ++it) {
            for (int i = 0; i < nvai; ++i) {
                if (now-it.key() <= graphTimes[i]) {
                    const ChanMinMax & v(it.value()[i]);
                    if (cmm[i].smin > v.smin) cmm[i].smin = v.smin;
                    if (cmm[i].smax < v.smax) cmm[i].smax = v.smax;
                }
            }
        }
       // Debug() << " auto-scaling code took: " << (getTime()-t0)*1e3 << " ms, chunksize:"  << (scans_size_samps/nvai) << " scans" <<  " maxage=" << maxAge << " n_chunks_remembered:" << chunkChanMinMaxs.size() << " now=" << now;
    }

    int firstidx = scans_size_samps - nvai;
    if (firstidx < 0) firstidx = 0;
    const bool nocm = params.fg.enabled && params.fg.disableChanMap;
    for (int i = firstidx; i < int(scans_size_samps); ++i) {
        int ch = i%nvai;
        int chanId = nocm ? ch : revsorting[ch];
        const QColor color (chanId < nvai-nextra ? fg : fg2);
#ifdef HEADACHE_PROTECTION
		double val = .9;
#else
        double val, sampval;

        val = ((sampval=double(scans[i]))+32768.) / 65535.;

        if (doAutoScale) {
            //double oldval = val;
            if (cmm[ch].smax>cmm[ch].smin) {
                val = (sampval-double(cmm[ch].smin)) / (double(cmm[ch].smax) - double(cmm[ch].smin));
                if (val < 0.) val = 0.;
                else if (val > 1.) val = 1.;
            }
/*           if (excessiveDebug) {
                Debug() << "Channel:" << ch << " sorting:" << chanId << " unscaled_val:" << oldval << " scaled_val:" << val << " sampval: " << sampval << " min:" << cmm[ch].smin << " max: " << cmm[ch].smax << " secs:" << graphTimes[ch];
            }
 */
        }
#endif
        chanRawSamps[chanId] = scans[i];
        chanVolts[chanId] = val * (params.range.max-params.range.min)+params.range.min;
        colors[chanId].x = color.redF()*float(val);
        colors[chanId].y = color.greenF()*float(val);
        colors[chanId].z = color.blueF()*float(val);
        colors[chanId].w = color.alphaF();
	}

    //Debug() << "SpatialVisWindow::putScans took " << (getTime()-t0)*1e3 << " ms for " << double(scans_size_samps/nvai/params.srate)*1e3 << " ms worth of scans, skipped every " << downSampleSkips << " scans." ;
}

SpatialVisWindow::~SpatialVisWindow()
{
	delete graphFrame, graphFrame = 0;
	graph = 0;
	if (fshare.shm) {
		if (fshare.lock()) {
			fshare.shm->enabled = 0;
			fshare.shm->spikegl_pid = 0;
			fshare.unlock();
		}
		fshare.detach();
	}
}

void SpatialVisWindow::updateGraph()
{

	if (!graph) return;
	updateGlyphSize();
    mut.lock();
	graph->setColors(colors);
    mut.unlock();
	if (graph->needsUpdateGL())
		graph->updateGL();
	
	if (fshare.shm) {
		bool en = overlayBut->isEnabled(), en2 = ovlFFChk->isEnabled();
		if ((en || en2) && !fshare.shm->stimgl_pid) {
			overlayBut->setEnabled(false);
			ovlFFChk->setEnabled(false);
			ovlfpsTit->setEnabled(false), ovlfpsLimit->setEnabled(false), ovlFps->setEnabled(false);			
			fshare.lock();
			fshare.shm->dump_full_window = 0;
			ovlSetNoData();
			fshare.unlock();
			graph->unsetXForm();
			ovlFFChk->blockSignals(true);
			ovlFFChk->setChecked(false);
			ovlFFChk->blockSignals(false);
		}
		else if ((!en || !en2) && fshare.shm->stimgl_pid) 
			overlayBut->setEnabled(true), ovlFFChk->setEnabled(true), ovlfpsTit->setEnabled(true), ovlfpsLimit->setEnabled(true), ovlFps->setEnabled(true);

	} else
		overlayBut->setEnabled(false), ovlFFChk->setEnabled(false),	ovlfpsTit->setEnabled(false), ovlfpsLimit->setEnabled(false), ovlFps->setEnabled(false);			
}

void SpatialVisWindow::selClear() { 
	selIdxs.clear(); 	
}

void SpatialVisWindow::selectChansFromTopLeft(int chan)
{
    Vec4 br;
    bool enabled = false;

    selIdxs.clear();

    if (selectionDims.x == 0) selectionDims.x = 1;
    if (selectionDims.y == 0) selectionDims.y = 1;
    // normalize selectionDims to always be positive...
    if (selectionDims.x < 0) selectionDims.x = -selectionDims.x;
    if (selectionDims.y < 0) selectionDims.y = -selectionDims.y;


    if (chan >= 0 && chan < nvai) {
        int r = chan / nbx, c = chan % nbx;

        if (c + selectionDims.x >= nbx) c -= (c + selectionDims.x)-nbx; // don't let box go off-grid
        if (r + selectionDims.y >= nby) r -= (r + selectionDims.y)-nby; // don't let box go off-grid
        if (c < 0) c = 0; // ensure top left is on-grid too
        if (r < 0) r = 0;

        int r2 = r+selectionDims.y, c2 = c + selectionDims.x;
        if (r2 > nby) r2 = nby;
        if (c2 > nbx) c2 = nbx;
        if (r2 < 0) r2 = r, r = 0;
        if (c2 < 0) c2 = c, c = 0;

        for (int i = r; i < r2; ++i) {
            for (int j = c; j < c2; ++j) {
                int ch = i*nbx + j;
                Vec4 bb = chanBoundingRect(ch);
                if (i==r && c==j) br = bb;
                else {
                    if (bb.v3 > br.v3) br.v3 = bb.v3;
                    if (bb.v4 < br.v4) br.v4 = bb.v4;
                }
                selIdxs.push_back(ch);
                enabled = true;
            }
        }

    }

    QMutexLocker l(&mut);

    if (enabled) {
        graph->setSelectionRange(br.v1, br.v3, br.v2, br.v4, GLSpatialVis::Outline);
        graph->setSelectionEnabled(true, GLSpatialVis::Outline);
//        Debug() << "selection of " << selectionDims.x << " x " << selectionDims.y << " top-left channel " << chan << " params: " << br.v1 << " " << br.v2 << " " << br.v3 << " " << br.v4;
//        QString s = "";
//        for (int i = 0; i < selIdxs.size(); ++i) s = s + QString::number(selIdxs[i]) + " ";
//        Debug() << "selidx.size=" << selIdxs.size() << " vals=" << s;
    } else {
        // not normally reached unless we have "blank" blocks at the end.....
        graph->setSelectionEnabled(false, GLSpatialVis::Outline);
    }
}

void SpatialVisWindow::selectChansCenteredOn(int chan)
{
    bool ok = false;

    if (chan >= 0 && chan < nvai)  {
        int r = chan / nbx, c = chan % nbx;

        r -= selectionDims.y/2;
        c -= selectionDims.x/2;

        if (r < 0) r = 0;
        if (c < 0) c = 0;
        chan = r*nbx + c;
        ok = true;
    }
    selectChansFromTopLeft(ok?chan:-1);
}

void SpatialVisWindow::gotoExtraAIClicked()
{
    if (suppressExtra) {
        int nx = params.nExtraChans1+params.nExtraChans2;
        if (nx > 0 && nx < (int)params.nVAIChans) {
            selectChansFromTopLeft(-1); // turn off selection
            for (int i = 0; i < nx; ++i)
                selIdxs.push_back(nvai+i);
            if (selIdxs.size()) emit channelsSelected(selIdxs);
        }
    }
}

void SpatialVisWindow::mouseClickGraph(double x, double y)
{
    didSelDimsDefine = false;
    mouseDownAt = Vec2(x,y);
}

void SpatialVisWindow::mouseOverGraph(double x, double y)
{
    mouseOverChan = -1;
    int chanId = pos2ChanId(x,y);
    if (chanId < 0 || chanId >= nvai)
        mouseOverChan = -1;
    else
        mouseOverChan = chanId;
    updateMouseOver();

    if (can_redefine_selection_box && mouseDownAt.x > -1e5) { // mouse button is down.. so maybe they are creating a new selection box
        const double thresh =  (1.0/MAX(nbx,nby)) / 3.0;
        Vec2 pt = mouseDownAt;
        double dx=x-pt.x, dy=pt.y-y;
        if ( fabs(dx) >= thresh || fabs(dy) >= thresh ) {
            selectionDims.x = qCeil(fabs(dx/(1.0/double(nbx))));
            selectionDims.y = qCeil(fabs(dy/(1.0/double(nby))));
        }
        if (selectionDims.x == 0) selectionDims.x = 1;
        if (selectionDims.y == 0) selectionDims.y = 1;
        int ch = pos2ChanId(dx>=0.0?pt.x:x,dy>=0.0?pt.y:y);
        selectChansFromTopLeft(ch);
        didSelDimsDefine = true;
    }
}


void SpatialVisWindow::mouseReleaseGraph(double x, double y)
{ 
	(void)x; (void)y;

    if (click_to_select) {
        if (!didSelDimsDefine) {
            int chanId = pos2ChanId(x,y);
            selectChansCenteredOn(chanId);
        }
        if (selIdxs.size()) emit channelsSelected(selIdxs);
    }

    mouseDownAt = Vec2(-1e6,-1e6);
}

void SpatialVisWindow::selectChansStartingAt(int chan)
{
    selectChansFromTopLeft(chan);
    emit channelsSelected(selIdxs);
}


void SpatialVisWindow::mouseDoubleClickGraph(double x, double y)
{
	(void)x; (void)y;
}

void SpatialVisWindow::updateMouseOver() // called periodically every 1s
{
    char buf[64];

	if (!statusLabel) return;
	const int chanId = mouseOverChan;
	if (chanId < 0 || chanId >= chanVolts.size()) 
		statusLabel->setText("");
    else {
        QMutexLocker l (&mut);
        int nid;
        if (params.fg.enabled && params.fg.disableChanMap)
            nid = chanId; // XXX this is for when we ENABLE sorting/remapping of framegrabber graphs
        else
            nid = chanId < (int)chanVolts.size() ? naming[chanId] : 0;
        if (nid < 0 || nid >= (int)chanVolts.size()) {
            Debug() << "naming weird? nid=" << nid << " chanid=" << chanId;
            nid = 0;
        }
#ifdef Q_OS_WINDOWS
        _snprintf_s
#else
        snprintf
#endif
                (buf,sizeof(buf),"%04hx",chanRawSamps[chanId]);

        QString raw = QString(buf).toUpper();
        if (raw.length() > 4) {
            Debug() << "Error in text formatting code.. raw: " << raw << " original value: " << chanRawSamps[nid];

        }
        statusLabel->setText(QString("Elec: %2 (Graph %1) -- %3 V [0x%4] ")
                             .arg(sorting[chanId])
                             .arg(naming[chanId])
                             .arg(chanVolts[chanId])
                             .arg(raw));
        //Debug() << "ChanId=" << chanId << " nid=" << nid << " sorting=" << sorting[chanId];
    }
	
	if (selIdxs.size()) {
		QString t = statusLabel->text();
		if (t.length()) t = QString("(mouse at: %1)").arg(t);
        statusLabel->setText(QString("Selected %1-%2 of %3. %4").arg(selIdxs.first()).arg(selIdxs.last()).arg(nvai).arg(t));
	}
	if (fdelayStr.size()) {
		QString t = statusLabel->text();
		statusLabel->setText(QString("%1%2").arg(t.size() ? t : "").arg(fdelayStr));
	}
}

Vec4 SpatialVisWindow::chanBoundingRectNoMargins(int chan) const
{
    int row = chan / nbx, col = chan % nbx;
	Vec4 ret;
	// top left
    ret.v1 = (1.0/nbx) * col;
    ret.v2 = 1.0 - (1.0/nby) * row;
	// bottom right
	ret.v3 = ret.v1 + (1.0/nbx);
	ret.v4 = ret.v2 - (1.0/nby);
	return ret;
}


Vec2 SpatialVisWindow::chanMargins() const
{
	return Vec2(1.0/nbx - (1.0/nbx * BlockScaleFactor), 1.0/nby - (1.0/nby * BlockScaleFactor)); 
}

Vec4 SpatialVisWindow::chanBoundingRect(int chan) const
{
    Vec4 ret (chanBoundingRectNoMargins(chan));
    Vec2 mrg(chanMargins());
    ret.v1+=mrg.x;
    ret.v3-=mrg.x;
    ret.v2-=mrg.y;
    ret.v4+=mrg.y;
	return ret;
}

Vec2 SpatialVisWindow::chanId2Pos(const int chanid) const
{
    Vec4 r(chanBoundingRectNoMargins(chanid));
    double cellw = (r.v3-r.v1), cellh = (r.v2-r.v4);
	return Vec2(
                r.v1 + (cellw/2.0),
                r.v2 - (cellh/2.0)
		);
}

int SpatialVisWindow::pos2ChanId(double x, double y) const
{
	
    int col = x*nbx, row = (1.0-y)*nby;
    int chan = (row * nbx) + col;
    return chan;
}

void SpatialVisWindow::updateToolBar()
{
    { // update color button
        QPixmap pm(22,22);
        QPainter p;
        p.begin(&pm);
        p.fillRect(0,0,22,22,QBrush(fg));
        p.end();
        colorBut->setIcon(QIcon(pm));
    }
    autoScaleChk->setChecked(autoScaleColorRange);
}

void SpatialVisWindow::colorButPressed()
{
    mut.lock();
    QColor f(fg),f2(fg2);
    mut.unlock();
    QColorDialog::setCustomColor(0,f.rgba());
    QColorDialog::setCustomColor(1,f2.rgba());
    QColor c = QColorDialog::getColor(f, this);
    if (c.isValid()) {
        {
            QMutexLocker l(&mut);
            int olda = fg.alpha();
            fg = c;
            fg.setAlpha(olda);
        }
        updateToolBar();
		saveSettings();
    }	
}

void SpatialVisWindow::saveSettings()
{
    QMutexLocker l (&mut);

	QSettings settings(SETTINGS_DOMAIN, SETTINGS_APP);
	settings.beginGroup(SETTINGS_GROUP);

    settings.setValue("fgcolor1_new", static_cast<unsigned int>(fg.rgba()));
    settings.setValue(QString("layout_%1_cols").arg(nvai), nbx);
    settings.setValue(QString("layout_%1_rows").arg(nvai), nby);
	settings.setValue(QString("UseStimGLOverlay"), overlayChk->isChecked());
	settings.setValue(QString("OverlayFPS"), ovlFps->value());
    settings.setValue("autoScaleColorRange", autoScaleColorRange);
	
	settings.endGroup();
}

void SpatialVisWindow::loadSettings()
{
    QMutexLocker l(&mut);
	QSettings settings(SETTINGS_DOMAIN, SETTINGS_APP);
	settings.beginGroup(SETTINGS_GROUP);

    int olda = fg.alpha();
    fg = QColor::fromRgba(settings.value("fgcolor1_new", static_cast<unsigned int>(fg.rgba())).toUInt());
    fg.setAlpha(olda);
    int sbcols = settings.value(QString("layout_%1_cols").arg(nvai), nbx).toInt();
    int sbrows = settings.value(QString("layout_%1_rows").arg(nvai), nby).toInt();
    if (sbrows > 0 && sbcols > 0 && sbrows <= 100 && sbcols <= 100 && sbrows * sbcols >= nvai) {
		sbRows->setValue(sbrows);
		sbCols->setValue(sbcols);
		if (nbx != sbcols || nby != sbrows)
            chanLayoutChanged();
    }
	overlayChecked(settings.value(QString("UseStimGLOverlay"), false).toBool());
//	ovlFpsChanged(settings.value(QString("OverlayFPS"), ovlFps->value()).toInt());

    autoScaleColorRange = settings.value("autoScaleColorRange", true).toBool();
	settings.endGroup();
}

void SpatialVisWindow::setStaticBlockLayout(int ncols, int nrows) {
    if (nrows * ncols < nvai) return;
    sbRows->setValue(nrows);
    sbCols->setValue(ncols);
    sbRows->setDisabled(true);
    sbCols->setDisabled(true);
    sbRows->setToolTip("Layout is fixed in this acq. mode");
    sbCols->setToolTip("Layout is fixed in this acq. mode");
}

Vec2 SpatialVisWindow::glyphMargins01Coords() const 
{
	Vec2 ret(graph->glyphSize().x/GlyphScaleFactor, graph->glyphSize().y/GlyphScaleFactor);
	ret.x = (ret.x - graph->glyphSize().x) / double(graph->width()) / 2.0;
	ret.y = (ret.y - graph->glyphSize().y) / double(graph->height()) / 2.0;
	return ret;
}

void SpatialVisWindow::keyPressEvent(QKeyEvent *e)
{
/*	if (e->key() == Qt::Key_Return && selIdxs.size() > 0) {
		e->accept();
		emit channelsOpened(selIdxs);
		return;
	}
    */
	QMainWindow::keyPressEvent(e);
}

void SpatialVisWindow::chanLayoutChanged()
{
    QMutexLocker l(&mut);

	int sbrows = sbRows->value(), sbcols = sbCols->value();
    if (sbrows * sbcols >= nvai) {
		nbx = sbcols;
		nby = sbrows;
		
		for (int chanid = 0; chanid < nvai; ++chanid) {
			points[chanid] = chanId2Pos(chanid);
		}
		setupGridlines();
		graph->setPoints(points);
		saveSettings();
	}
}

void SpatialVisWindow::overlayChecked(bool chk)
{
	overlayAlpha->setEnabled(chk);
	overlayChk->blockSignals(true);
	overlayChk->setChecked(chk);
	ovlFFChk->blockSignals(true);
	setOverlayAlpha(chk ? overlayAlpha->value() : 0);
	if (fshare.shm && fshare.lock()) {
		fshare.shm->enabled = chk ? 1 : 0;
		overlayBut->setEnabled(chk && fshare.shm->stimgl_pid);
		ovlFFChk->setEnabled(chk && fshare.shm->stimgl_pid);
		ovlFFChk->setChecked(ovlFFChk->isChecked() && chk);
		bool dfw = (fshare.shm->dump_full_window = !!ovlFFChk->isChecked());
		fshare.unlock();
		if (!dfw) graph->unsetXForm();
	}
	overlayChk->blockSignals(false);
	ovlFFChk->blockSignals(false);
//	saveSettings(); // uncomment if you want to save the state of this in the settings...
}

void SpatialVisWindow::setOverlayAlpha(int v)
{
	graph->setOverlayAlpha(v / 100.0f);
}

void SpatialVisWindow::ovlAlphaChanged(int v)
{
	ovlAlphaLbl->setText(QString("%2% %1").arg(QChar(ushort(0x03b1))).arg(v));	
}

void SpatialVisWindow::ovlUpdate()
{
	GLuint frameNum = last_fs_frame_num;
	quint64 frameTsc = last_fs_frame_tsc, nowTsc = getAbsTimeNS();
	bool tscIsValid = false;
	if (overlayAlpha->value() && overlayAlpha->isEnabled() && fshare.shm && fshare.shm->enabled) {
		if (fshare.shm->dump_full_window) 
			graph->setXForm(fshare.shm->box_x, fshare.shm->box_y, fshare.shm->box_w, fshare.shm->box_h);
		else
			graph->unsetXForm();
		graph->setOverlay((void *)fshare.shm->data, fshare.shm->w, fshare.shm->h, fshare.shm->fmt);
		frameNum = fshare.shm->frame_num;
		frameTsc = fshare.shm->frame_abs_time_ns;
		tscIsValid = true;
	}
	if (frameNum != last_fs_frame_num && graph->needsUpdateGL()) {
		graph->updateGL();
		//if (excessiveDebug) Debug() << "Frame: " << frameNum << " delay: " << ((nowTsc-frameTsc)/1e6) << " ms";
		if (tscIsValid) {
			double delay = (nowTsc-frameTsc)/1e6;
			if (delay < 1500.0 && delay > 0.0) { // throw out outliers...
				frameDelayAvg(delay);
				fdelayStr = QString().sprintf(" - Overlay %2.2f ms avg. frame latency",frameDelayAvg()); 
			}
		}
	} else if (nowTsc-frameTsc > 1500000000)
		fdelayStr = "";
	last_fs_frame_num = frameNum;
	last_fs_frame_tsc = frameTsc;
}

void SpatialVisWindow::ovlSetNoData()
{
	QImage nodata(no_data_xpm);
	nodata = QGLWidget::convertToGLFormat(nodata);
	if (nodata.isNull()) {
		Error() << "could not convert nodata QImage to GL format";
	} else {
		fshare.shm->fmt = GL_RGBA;
		fshare.shm->w = nodata.width();
		fshare.shm->h = nodata.height();
		fshare.shm->sz_bytes = nodata.byteCount();
		memcpy((void *)fshare.shm->data, nodata.bits(), nodata.byteCount() < fshare.size() ? nodata.byteCount() : fshare.size());
	}
}

void SpatialVisWindow::closeEvent(QCloseEvent *e)
{
	overlayChecked(false); // force overlay to uncheck when closing window..
	QMainWindow::closeEvent(e);
}

void SpatialVisWindow::overlayButPushed()
{
	if (fshare.shm) {
		fshare.lock();
		fshare.shm->do_box_select = 1;
		fshare.unlock();
	}
}

void SpatialVisWindow::ovlFFChecked(bool b)
{
	if (fshare.shm && fshare.shm->stimgl_pid) {
		fshare.shm->dump_full_window = b?1:0;
	} else  {
		fshare.shm->dump_full_window = 0;
		ovlFFChk->blockSignals(true); ovlFFChk->setChecked(false); ovlFFChk->blockSignals(false);
	}
}

void SpatialVisWindow::ovlFpsChanged(int fps)
{
	if (fps < 1 || fps >= ovlFps->maximum()) fps = 0;
	if (fshare.shm) fshare.shm->frame_rate_limit = fps;
	//if (excessiveDebug) Debug() << "set fshare shm fps to " << fps;
	ovlfpsLimit->setText(fps ? QString().sprintf("%3d",fps) : QString("  %1  ").arg(QChar(ushort(0x221e))));
	if (sender() != ovlFps || fps != ovlFps->value()) {
		ovlFps->blockSignals(true);
		ovlFps->setValue(!fps ? ovlFps->maximum() : fps);
		ovlFps->blockSignals(false);
	}
//	if (sender() == ovlFps)
//		saveSettings(); // uncomment if you want to save the state of this in the settings...
}

void SpatialVisWindow::showEvent(QShowEvent *e)
{
     QMainWindow::showEvent(e);
     threadsafe_is_visible = e->isAccepted() || isVisible();
}

void SpatialVisWindow::hideEvent(QHideEvent *e)
{
    QMainWindow::hideEvent(e);
    threadsafe_is_visible = !(e->isAccepted() || isHidden());
}

void SpatialVisWindow::setSorting(const QVector<int> & s, const QVector<int> & n)
{
    QVector<int> rs; rs.resize(s.size());
    for (int i = 0; i < s.size(); ++i) {
        int n = s[i];
        if (n < 0 || n >= rs.size()) {
            Error() << "INTERNAL ERROR: SpatialVisWindow::setSorting -- sorting specified seems fishy.  Got a value out of range! FIXME!";
            n = 0;
        }
        rs[n] = i;
    }
    /*QString str1 = "sorting: ", str2 = "naming: ", str3 = "revsorting: ";
    for (int i = 0; i < s.size(); ++i) str1 += QString::number(s[i]) + ",", str2 += QString::number(n[i]) + ",", str3 += QString::number(rs[i]) + ",";
    Debug() << "SpatialVisWindow sorting changed called...";
    Debug() << str1;
    Debug() << str3;
    Debug() << str2;*/
    QMutexLocker l(&mut);
    sorting = s;
    naming = n;
    revsorting = rs;
    chanLayoutChanged();
    if (selIdxs.size()) {
        updateMouseOver();
    }
}


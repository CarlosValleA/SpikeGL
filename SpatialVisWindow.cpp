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

#define SETTINGS_GROUP "SpatialVisWindow Settings"
#define GlyphShrink 0.925

SpatialVisWindow::SpatialVisWindow(DAQ::Params & params, QWidget * parent)
: QMainWindow(parent), params(params), nvai(params.nVAIChans), nextra(params.nExtraChans1+params.nExtraChans2), 
  graph(0), graphFrame(0), mouseOverChan(-1)
{

	setWindowTitle("Spatial Visualization");
	resize(800,600);

	toolBar = addToolBar("Spatial Visualization Controls");
	
	QLabel *label;
	toolBar->addWidget(label = new QLabel("Color: ", toolBar));
	toolBar->addWidget(colorBut = new QPushButton(toolBar));
	
	toolBar->addSeparator();
	
	Connect(colorBut, SIGNAL(clicked(bool)), this, SLOT(colorButPressed()));
	
    nx = sqrt(nvai), ny = nx;
	if (nx <= 0) nx = 1;
	while (nx*ny < nvai) ++ny;	
	
	points.resize(nvai);
	colors.resize(nvai);
	chanVolts.resize(nvai);
	
	for (int chanid = 0; chanid < nvai; ++chanid) {
		points[chanid] = chanId2Pos(chanid);
	}
	
	graphFrame = new QFrame(this);
	QVBoxLayout *bl = new QVBoxLayout(graphFrame);
    bl->setSpacing(0);
	bl->setContentsMargins(0,0,0,0);
	graph = new GLSpatialVis(graphFrame);
	bl->addWidget(graph,1);	
	setCentralWidget(graphFrame);

	if ((int)graph->numVGridLines() != nx) graph->setNumVGridLines(nx);
	if ((int)graph->numHGridLines() != ny) graph->setNumHGridLines(ny);
	
	fg = QColor(0x87, 0xce, 0xfa, 0x7f);
	fg2 = QColor(0xfa, 0x87, 0x37, 0x7f);
	QColor bg, grid;
	bg.setRgbF(.15,.15,.15);
	grid.setRgbF(0.4,0.4,0.4);			
	
	graph->setBGColor(bg);
	graph->setGridColor(grid);
	
	// load settings here..
	
		
	Connect(graph, SIGNAL(cursorOver(double, double)), this, SLOT(mouseOverGraph(double, double)));
	Connect(graph, SIGNAL(clicked(double, double)), this, SLOT(mouseClickGraph(double, double)));
	Connect(graph, SIGNAL(clickReleased(double, double)), this, SLOT(mouseReleaseGraph(double, double)));
	Connect(graph, SIGNAL(doubleClicked(double, double)), this, SLOT(mouseDoubleClickGraph(double, double)));
	
	QStatusBar *sb = statusBar();
	sb->addWidget(statusLabel = new QLabel(sb),1);
	
	QTimer *t = new QTimer(this);
    Connect(t, SIGNAL(timeout()), this, SLOT(updateGraph()));
    t->setSingleShot(false);
    t->start(1000/DEF_TASK_READ_FREQ_HZ);        
	
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
}

void SpatialVisWindow::resizeEvent (QResizeEvent * event)
{
	updateGlyphSize();
	QMainWindow::resizeEvent(event);
}

void SpatialVisWindow::updateGlyphSize()
{
	if (!graph) return;
	int szx = graph->width() / nx, szy = graph->height() / ny;
	szx *= GlyphShrink, szy *= GlyphShrink;
	if (szx < 1) szx = 1;
	if (szy < 1) szy = 1;
	graph->setGlyphSize(Vec2f(szx,szy));		
}

void SpatialVisWindow::putScans(const std::vector<int16> & scans, u64 firstSamp)
{
	(void)firstSamp; // unused warning
	int firstidx = scans.size() - nvai;
	if (firstidx < 0) firstidx = 0;
	for (int i = firstidx; i < int(scans.size()); ++i) {
		int chanid = i % nvai;
		const QColor color (chanid < nvai-nextra ? fg : fg2);
		double val = (double(scans[i])+32768.) / 65535.;
		chanVolts[chanid] = val * (params.range.max-params.range.min)+params.range.min;
		colors[chanid].x = color.redF()*val;
		colors[chanid].y = color.greenF()*val;
		colors[chanid].z = color.blueF()*val;
		colors[chanid].w = color.alphaF();
	}
	updateGraph();
}

SpatialVisWindow::~SpatialVisWindow()
{
	delete graphFrame, graphFrame = 0;
	graph = 0;
}

void SpatialVisWindow::updateGraph()
{
	if (!graph) return;
	updateGlyphSize();
	graph->setColors(colors);
}

bool SpatialVisWindow::selStarted() const
{
	return selClick.x > -.009 && selClick.y > -.009;
}

void SpatialVisWindow::selClear() { 
	selClick = Vec2(-1.,-1.); 
	selIdxs.clear(); 	
	graph->setSelectionEnabled(false, GLSpatialVis::Outline);
	graph->setSelectionEnabled(false, GLSpatialVis::Box);
 }

void SpatialVisWindow::mouseOverGraph(double x, double y)
{
	mouseOverChan = -1;
	int chanId = pos2ChanId(x,y);
	if (chanId < 0 || chanId >= (int)params.nVAIChans) 
		mouseOverChan = -1;
	else
		mouseOverChan = chanId;
	updateMouseOver();
	
	// update selection
	if (selStarted()) {
		graph->setSelectionRange(selClick.x, x, selClick.y, y, GLSpatialVis::Outline);
		graph->setSelectionEnabled(true, GLSpatialVis::Outline);
		selIdxs = graph->selectAllGlyphsIntersectingRect(Vec2(selClick.x,selClick.y),Vec2(x,y),GLSpatialVis::Box,glyphMargins01Coords());
	}
}

void SpatialVisWindow::mouseClickGraph(double x, double y)
{
	int chanId = pos2ChanId(x,y);
	if (chanId < 0 || chanId >= (int)params.nVAIChans) statusLabel->setText("");  
	else statusLabel->setText(QString("Mouse click %1,%2 -> %3").arg(x).arg(y).arg(chanId));
	selClear();
	selClick = Vec2(x,y);
}


void SpatialVisWindow::mouseReleaseGraph(double x, double y)
{
	bool hasSel = selStarted() && !feq(selClick.x,x,0.01) && !feq(selClick.y,y,0.01);
	Vec2 sclk = selClick;
	selClear();
	if (hasSel) {
		// update/grow selection here to include all squares that it crosses...	
		selIdxs = graph->selectAllGlyphsIntersectingRect(Vec2(sclk.x,sclk.y),Vec2(x,y),GLSpatialVis::Box,glyphMargins01Coords());
		selIdxs = graph->selectAllGlyphsIntersectingRect(Vec2(sclk.x,sclk.y),Vec2(x,y),GLSpatialVis::Outline,glyphMargins01Coords());
	}
}

void SpatialVisWindow::mouseDoubleClickGraph(double x, double y)
{
	int chanId = pos2ChanId(x,y);
	if (chanId < 0 || chanId >= (int)params.nVAIChans) { statusLabel->setText(""); return; }
	statusLabel->setText(QString("Mouse dbl click %1,%2 -> %3").arg(x).arg(y).arg(chanId));
}

void SpatialVisWindow::updateMouseOver() // called periodically every 1s
{
	if (!statusLabel) return;
	const int chanId = mouseOverChan;
	if (chanId < 0 || chanId >= chanVolts.size()) 
		statusLabel->setText("");
	else
		statusLabel->setText(QString("Chan: #%3 -- Volts: %4 V")
							 .arg(chanId)
							 .arg(chanId < (int)chanVolts.size() ? chanVolts[chanId] : 0.0));
	
	if (selIdxs.size()) {
		QString t = statusLabel->text();
		if (t.length()) t = QString("(mouse at: %1)").arg(t);
		statusLabel->setText(QString("Selection: %1 channels, double-click to open. %2").arg(selIdxs.size()).arg(t));
	}
}

Vec2 SpatialVisWindow::chanId2Pos(const int chanid) const
{
	Vec2 ret;
	const int col = chanid % nx, row = chanid / nx;
	ret.x = (col/double(nx)) + (1./(nx*2.));
	ret.y = (row/double(ny)) + (1./(ny*2.));
	return ret;
}

int SpatialVisWindow::pos2ChanId(double x, double y) const
{
	int col = x*nx, row = y*ny;
	return col + row*nx;
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
}

void SpatialVisWindow::colorButPressed()
{
    QColorDialog::setCustomColor(0,fg.rgba());
    QColorDialog::setCustomColor(1,fg2.rgba());
    QColor c = QColorDialog::getColor(fg, this);
    if (c.isValid()) {
		fg = c;
        updateToolBar();
		saveSettings();
    }	
}

void SpatialVisWindow::saveSettings()
{
	QSettings settings(SETTINGS_DOMAIN, SETTINGS_APP);
	settings.beginGroup(SETTINGS_GROUP);

	settings.setValue("fgcolor1", static_cast<unsigned int>(fg.rgba())); 
	
	settings.endGroup();
}

void SpatialVisWindow::loadSettings()
{
	QSettings settings(SETTINGS_DOMAIN, SETTINGS_APP);
	settings.beginGroup(SETTINGS_GROUP);

	fg = QColor::fromRgba(settings.value("fgcolor1", static_cast<unsigned int>(fg.rgba())).toUInt());
	
	settings.endGroup();
}

Vec2 SpatialVisWindow::glyphMargins01Coords() const 
{
	Vec2 ret(graph->glyphSize().x/GlyphShrink, graph->glyphSize().y/GlyphShrink);
	ret.x = (ret.x - graph->glyphSize().x) / double(graph->width()) / 2.0;
	ret.y = (ret.y - graph->glyphSize().y) / double(graph->height()) / 2.0;
	return ret;
}

#include "GraphsWindow.h"
#include <QToolBar>
#include <QLCDNumber>
#include <QLabel>
#include <QGridLayout>
#include <math.h>
#include <QTimer>
#include "Util.h"
#include <QCheckBox>
#include <QVector>
#include <math.h>

GraphsWindow::GraphsWindow(const DAQ::Params & p, QWidget *parent)
    : QMainWindow(parent), params(p), downsampleRatio(1.), graphTimeSecs(3.0), tNow(0.), tLast(0.), tAvg(0.), tNum(0.), npts(0)
{    
    setCentralWidget(graphsWidget = new QWidget(this));
    setAttribute(Qt::WA_DeleteOnClose, false);
    statusBar();
    resize(1024,768);
    graphCtls = addToolBar("Graph Controls");
    graphCtls->addWidget(new QLabel("Channel:", graphCtls));
    graphCtls->addWidget(chanLCD = new QLCDNumber(2, graphCtls));
    graphCtls->addSeparator();
    QCheckBox *dsc = new QCheckBox(QString("Downsample graphs to %1 Hz").arg(DOWNSAMPLE_TARGET_HZ), graphCtls);
    graphCtls->addWidget(dsc);
    dsc->setChecked(true);
    downsampleChk(true);
    Connect(dsc, SIGNAL(clicked(bool)), this, SLOT(downsampleChk(bool)));
    graphs.resize(p.nVAIChans);
    QGridLayout *l = new QGridLayout(graphsWidget);
    int nrows = int(sqrtf(p.nVAIChans)), ncols = nrows;
    while (nrows*ncols < (int)graphs.size()) {
        if (nrows > ncols) ++ncols;
        else ++nrows;
    };
    for (int r = 0; r < nrows; ++r) {
        for (int c = 0; c < ncols; ++c) {
            int num = r*ncols+c;
            if (num >= (int)graphs.size()) { r=nrows,c=ncols; break; } // break out of loop
            graphs[num] = new GLGraph(graphsWidget);
            graphs[num]->setAutoUpdate(false);
            l->addWidget(graphs[num], r, c);
        }
    }
    points.resize(graphs.size());

    setGraphTimeSecs(3.0);

    QTimer *t = new QTimer(this);
    Connect(t, SIGNAL(timeout()), this, SLOT(updateGraphs()));
    t->setSingleShot(false);
    t->start(1000/TASK_READ_FREQ_HZ);    
}

GraphsWindow::~GraphsWindow()
{
}

void GraphsWindow::setGraphTimeSecs(double t)
{
    graphTimeSecs = t;
    npts = i64(ceil(graphTimeSecs*params.srate/downsampleRatio));
    for (int i = 0; i < (int)points.size(); ++i) {
        points[i].reserve(npts);
        graphs[i]->setPoints(0);
    }
}

void GraphsWindow::putScans(std::vector<int16> & data, u64 firstSamp)
{
        const int NGRAPHS (graphs.size());
        const int DOWNSAMPLE_RATIO((int)downsampleRatio);
        const int SRATE (params.srate);

        int startpt = int(data.size()) - int(npts*NGRAPHS*DOWNSAMPLE_RATIO);
        i64 sidx = i64(firstSamp + u64(data.size())) - npts*i64(NGRAPHS*DOWNSAMPLE_RATIO);
        if (startpt < 0) {
            //qDebug("Startpt < 0 = %d", startpt);
            sidx += -startpt;
            startpt = 0;
        }
        int npergraph;
        {
            int ndiv = int(data.size()/NGRAPHS/DOWNSAMPLE_RATIO);
            if (NGRAPHS*DOWNSAMPLE_RATIO*ndiv < (int)data.size()) ndiv++;
            npergraph = int(MIN(ndiv, npts));
        }

        double t = double(double(sidx) / NGRAPHS) / double(SRATE);
        const double deltaT =  1.0/SRATE * double(DOWNSAMPLE_RATIO);
        // now, push new points to back of each graph, downsampling if need be
        Vec2 v;
        int idx = 0;
        for (int i = startpt; i < (int)data.size(); ++i) {
            v.x = t;
            v.y = data[i] / 32768.0; // hardcoded range of data
            points[idx].putData(&v, 1);
            if (!(++idx%NGRAPHS)) {                
                idx = 0;
                t += deltaT;
                i = int((i-NGRAPHS) + DOWNSAMPLE_RATIO*NGRAPHS);
                if ((i+1)%NGRAPHS) i -= (i+1)%NGRAPHS;
            }
        }
        for (int i = 0; i < NGRAPHS; ++i) {
            // now, copy in temp data
            if (points[i].size() >= 2) {
                // now, readjust x axis begin,end
                graphs[i]->minx() = points[i].first().x;
                graphs[i]->maxx() = graphs[i]->minx() + graphTimeSecs;
                // uncomment below 2 line if the empty gap at the end of the downsampled graph annoys you, or comment them out to remove this 'feature'
                //if (!points[i].unusedCapacity())
                //    graphs[i]->maxx() = points[i].last().x;
            } 
            // and, notify graph of new points
            graphs[i]->setPoints(&points[i]);
        }
        
        tNow = getTime();

        const double tDelta = tNow - tLast;
        if (tLast > 0) {
            tAvg *= tNum;
            if (tNum >= 30) { tAvg -= tAvg/30.; --tNum; }
            tAvg += tDelta;
            tAvg /= ++tNum;
        } 
        tLast = tNow;
}

void GraphsWindow::updateGraphs()
{
    // repaint all graphs..
    for (int i = 0; i < (int)graphs.size(); ++i)
        if (graphs[i]->needsUpdateGL())
            graphs[i]->updateGL();
}

void GraphsWindow::downsampleChk(bool checked)
{
    if (checked) {
        downsampleRatio = params.srate/double(DOWNSAMPLE_TARGET_HZ);
        if (downsampleRatio < 1.) downsampleRatio = 1.;
    } else
        downsampleRatio = 1.;
    setGraphTimeSecs(graphTimeSecs); // clear the points and reserve the right capacities.
}



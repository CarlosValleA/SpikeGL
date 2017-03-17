#include <QTextEdit>
#include <QMessageBox>
#include "MainApp.h"
#include "ConsoleWindow.h"
#include "Util.h"
#include "Version.h"
#include <qglobal.h>
#include <QEvent>
#include <cstdlib>
#include <QSettings>
#include <QMetaType>
#include <QStatusBar>
#include <QTimer>
#include <QKeyEvent>
#include <QFileDialog>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <QPixmap>
#include <QIcon>
#include <QDir>
#include <QDesktopWidget>
#include <QMutexLocker>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QProgressDialog>
#include <QDialog>
#include <QGLFormat>
#include <QGLContext>
#include <QLocale>
#include "Icon.xpm"
#include "ParWindowIcon.xpm"
#include "ConfigureDialogController.h"
#include "ChanMappingController.h"
#include "GraphsWindow.h"
#include "Sha1VerifyTask.h"
#include "Par2Window.h"
#include "ui_StimGLIntegration.h"
#include "StimGL_SpikeGL_Integration.h"
#include "ui_TextBrowser.h"
#include "ui_CommandServerOptions.h"
#include "CommandServer.h"
#include "ui_TempFileDialog.h"
#include "FileViewerWindow.h"
#include <algorithm>
#include "SpatialVisWindow.h"
#include "Bug_ConfigDialog.h"
#include "Bug_Popout.h"
#include "FG_ConfigDialog.h"
#include "ui_SampleBuf_Dialog.h"

Q_DECLARE_METATYPE(unsigned);

namespace {
    struct Init {
        Init() {
            qRegisterMetaType<unsigned>("unsigned");
            qRegisterMetaType<QVector<int> >("QVector<int>");
        }
    };

    Init * volatile init = 0;


    class LogLineEvent : public QEvent
    {
    public:
        LogLineEvent(const QString &str, const QColor & color)
            : QEvent((QEvent::Type)MainApp::LogLineEventType), str(str), color(color)
        {}
        QString str;
        QColor color;
    };

    class StatusMsgEvent : public QEvent
    {
    public:
        StatusMsgEvent(const QString &msg, int timeout)
            : QEvent((QEvent::Type)MainApp::StatusMsgEventType), msg(msg), timeout(timeout)
        {}
        QString msg;
        int timeout;
    };

};

MainApp * MainApp::singleton = 0;

MainApp::MainApp(int & argc, char ** argv)
    : QApplication(argc, argv, true), mut(QMutex::Recursive), consoleWindow(0), debug(false), initializing(true), sysTray(0), nLinesInLog(0), nLinesInLogMax(1000), task(0), graphsWindow(0), spatialWindow(0), bugWindow(0), fgWindow(0), notifyServer(0), commandServer(0), fastSettleRunning(false), helpWindow(0), noHotKeys(false), pdWaitingForStimGL(false), precreateDialog(0), pregraphDummyParent(0), maxPreGraphs(/*MAX_NUM_GRAPHS_PER_GRAPH_TAB*/4), tPerGraph(0.), acqStartingDialog(0), doBugAcqInstead(false)
{
    got_sgl_ended = got_sgl_save = got_sgl_started = false;
    reader = 0;
    gthread1 = gthread2 = 0;
    dthread = 0;
    samplesBuffer = 0;
    need2FreeSamplesBuffer = false;
    scanCt = 0;
    scanSkipCt = 0;

	QLocale::setDefault(QLocale::c());
	setApplicationName("SpikeGL");
	setApplicationVersion(VERSION_STR);

    sb_Timeout = 0;
    if (singleton) {
        QMessageBox::critical(0, "Invariant Violation", "Only 1 instance of MainApp allowed per application!");
        std::exit(1);
    }
    singleton = this;
    if (!::init) ::init = new Init;
    setQuitOnLastWindowClosed(false);
    loadSettings();

    initActions();

    createIcons();
    
    configCtl = new ConfigureDialogController(this);
	bugConfig = new Bug_ConfigDialog(configCtl->acceptedParams, this);
	fgConfig = new FG_ConfigDialog(configCtl->acceptedParams, this);

    installEventFilter(this); // filter our own events

    consoleWindow = new ConsoleWindow;
#ifdef Q_OS_MACX
	/* add the console window to the Window menu, which, on OSX is an app-global menu
	   -- on other platform the console window is *in* the window menu so only needs to be done on OSX */
	windowMenuAdd(consoleWindow);
#endif
	
    Connect(this, SIGNAL(do_updateWindowTitles()), this, SLOT(updateWindowTitles()));
    Connect(this, SIGNAL(do_stopTask()), this, SLOT(stopTask()));

    defaultLogColor = consoleWindow->textEdit()->textColor();
    consoleWindow->setAttribute(Qt::WA_DeleteOnClose, false);
	
	Connect(consoleWindow->windowMenu(), SIGNAL(aboutToShow()), this, SLOT(windowMenuAboutToShow()));

    sysTray = new QSystemTrayIcon(this);
    sysTray->setContextMenu(new QMenu(consoleWindow));
    sysTray->contextMenu()->addAction(hideUnhideConsoleAct);
    sysTray->contextMenu()->addAction(hideUnhideGraphsAct);
    sysTray->contextMenu()->addSeparator();
    sysTray->contextMenu()->addAction(aboutAct);
    sysTray->contextMenu()->addSeparator();
    sysTray->contextMenu()->addAction(quitAct);
    sysTray->setIcon(appIcon);
    sysTray->show();

    par2Win = new Par2Window(0);
    par2Win->setAttribute(Qt::WA_DeleteOnClose, false); 
    par2Win->setWindowTitle(QString(APPNAME) + " - Par2 Redundancy Tool");
    par2Win->setWindowIcon(QPixmap(ParWindowIcon_xpm));
	Connect(par2Win, SIGNAL(closed()), this, SLOT(par2WinClosed()));

    Log() << VERSION_STR;
	Log() << "Application started";

    if (getNProcessors() > 1)
        setProcessAffinityMask(0x1); // set it to core 1

    consoleWindow->installEventFilter(this);
    consoleWindow->textEdit()->installEventFilter(this);

    consoleWindow->resize(800, 300);
    consoleWindow->show();

    setupStimGLIntegration();
    setupCommandServer();

    QTimer *timer = new QTimer(this);
    Connect(timer, SIGNAL(timeout()), this, SLOT(updateStatusBar()));
    timer->setSingleShot(false);
    timer->start(247); // update status bar every 247ms.. i like this non-round-numbre.. ;)
    
	acqWaitingForPrecreate = false;
    pregraphTimer = new QTimer(this);
    Connect(pregraphTimer, SIGNAL(timeout()), this, SLOT(precreateGraphs()));
    pregraphTimer->setSingleShot(false);
    pregraphTimer->start(0);

	appInitialized();	
}

MainApp::~MainApp()
{
    stopTask();
    Log() << "Application shutting down..";
    Status() << "Application shutting down.";
    if (commandServer) delete commandServer, commandServer = 0;
    CommandServer::deleteAllActiveConnections();
    saveSettings();
    delete par2Win, par2Win = 0;
    delete configCtl, configCtl = 0;
    delete bugConfig, bugConfig = 0;
    delete fgConfig, fgConfig = 0;
    delete sysTray, sysTray = 0;
    delete helpWindow, helpWindow = 0;
    delete pregraphDummyParent, pregraphDummyParent = 0;
    pregraphs.clear();
    singleton = 0;
}


bool MainApp::isDebugMode() const
{
    // always true for now..
    return debug;
}

bool MainApp::isSaveCBEnabled() const { return saveCBEnabled; }

bool MainApp::isDSFacilityEnabled() const { return dsFacilityEnabled; }

bool MainApp::isConsoleHidden() const 
{
    return !consoleWindow || consoleWindow->isHidden();
}

void MainApp::toggleDebugMode()
{
    debug = !debug;
    Log() << "Debug mode: " << (debug ? "on" : "off");
    saveSettings();    
}

void MainApp::toggleExcessiveDebugMode()
{
    excessiveDebug = !excessiveDebug;
    Debug() << "Excessive Debug mode: " << (excessiveDebug ? "on" : "off");
	saveSettings();
}

void MainApp::toggleShowChannelSaveCB()
{
	saveCBEnabled = !saveCBEnabled;
	if (graphsWindow) graphsWindow->hideUnhideSaveChannelCBs();	
	saveSettings();
}

void MainApp::toggleEnableDSFacility()
{
	dsFacilityEnabled = !dsFacilityEnabled;
	if(dsFacilityEnabled)
		tempFileSizeAct->setEnabled(true);
	else {
		tempFileSizeAct->setEnabled(false);
		tmpDataFile.close();	 // immediately deletes file, resetting settings	
	}
	saveSettings();
}

bool MainApp::isShiftPressed()
{
    return (keyboardModifiers() & Qt::ShiftModifier);
}

bool MainApp::processKey(QKeyEvent *event) 
{        
    switch (event->key()) {
    case 'd':
    case 'D':
        if (event->modifiers() == Qt::ControlModifier)
		    toggleExcessiveDebugAct->trigger();
		else 
            toggleDebugAct->trigger();
        return true;
    case 'c':
    case 'C':
        if (event->modifiers()) // trap for hotkey if no ctrl of shift pressed unmodified.
            return false;
        hideUnhideConsoleAct->trigger();
        return true;
    case 'g':
    case 'G':
        if (event->modifiers()) // trap for hotkey if no ctrl of shift pressed unmodified.
            return false;
        hideUnhideGraphsAct->trigger();
        return true;
	case 'n':
	case 'N':
        if (event->modifiers()) // trap for hotkey if no ctrl of shift pressed unmodified.
            return false;
        newAcqAct->trigger();
		return true;
	case 'b':
	case 'B':
        if (event->modifiers()) // trap for hotkey if no ctrl of shift pressed unmodified.
            return false;
        bugAcqAct->trigger();
		return true;
	case 'f':
	case 'F':
        if (event->modifiers()) // trap for hotkey if no ctrl of shift pressed unmodified.
            return false;
        fgAcqAct->trigger();
		return true;
	case 'o':
	case 'O':
        if (event->modifiers()) // trap for hotkey if no ctrl of shift pressed unmodified.
            return false;
        fileOpenAct->trigger();
		return true;
	case Qt::Key_Escape:
        if (event->modifiers()) // trap for hotkey if no ctrl of shift pressed unmodified.
            return false;
        stopAcq->trigger();
		return true;
    }

    return false;
}

bool MainApp::eventFilter(QObject *watched, QEvent *event)
{
    int type = static_cast<int>(event->type());
    if (type == QEvent::KeyPress) {
        // globally forward all keypresses
        // if they aren't handled, then return false for normal event prop.
        QKeyEvent *k = dynamic_cast<QKeyEvent *>(event);
        if (k && !noHotKeys 
            && watched != helpWindow && (!helpWindow || !Util::objectHasAncestor(watched, helpWindow)) 
            && watched != par2Win && (!par2Win || !Util::objectHasAncestor(watched, par2Win)) 
            && (watched == graphsWindow || watched == consoleWindow || (spatialWindow && watched == spatialWindow) || (bugWindow && watched == bugWindow) || (fgWindow && watched == fgWindow) || watched == consoleWindow->textEdit() || ((!graphsWindow || watched != graphsWindow->saveFileLineEdit()) && Util::objectHasAncestor(watched, graphsWindow)))) {
            if (processKey(k)) {
                event->accept();
                return true;
            } 
        }
    }
	if (type == QEvent::Close) {
		FileViewerWindow *fvw = 0;
		
		if (watched == graphsWindow) {
			// request to close the graphsWindow.. this stops the acq -- ask the user to confirm.. do this after this event handler runs, so enqueue it with a timer
			QTimer::singleShot(1, this, SLOT(maybeCloseCurrentIfRunning()));
			event->ignore();
			return true;
		} else if ((precreateDialog && watched == precreateDialog) || (acqStartingDialog && watched == acqStartingDialog)) {
			event->ignore();
			return true;
		} else if ( (fvw = dynamic_cast<FileViewerWindow *>(watched)) ) {
			if (fvw->queryCloseOK()) {
				windowMenuRemove(fvw);
				fvw->deleteLater(); // schedule window for deletion...
			} else 
				event->ignore();
			return true; // tell Qt we handled the event
		}
	}
    if (watched == consoleWindow) {
        ConsoleWindow *cw = dynamic_cast<ConsoleWindow *>(watched);
        if (type == LogLineEventType) {
            LogLineEvent *evt = dynamic_cast<LogLineEvent *>(event);
            if (evt && cw->textEdit()) {
                QTextEdit *te = cw->textEdit();
                QColor origcolor = te->textColor();
                te->setTextColor(evt->color);
                te->append(evt->str);

                // make sure the log textedit doesn't grow forever
                // so prune old lines when a threshold is hit
                nLinesInLog += evt->str.split("\n").size();
                if (nLinesInLog > nLinesInLogMax) {
                    const int n2del = MAX(nLinesInLogMax/10, nLinesInLog-nLinesInLogMax);
                    QTextCursor cursor = te->textCursor();
                    cursor.movePosition(QTextCursor::Start);
                    for (int i = 0; i < n2del; ++i) {
                        cursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor);
                    }
                    cursor.removeSelectedText(); // deletes the lines, leaves a blank line
                    nLinesInLog -= n2del;
                }

                te->setTextColor(origcolor);
                te->moveCursor(QTextCursor::End);
                te->ensureCursorVisible();
                return true;
            } else {
                return false;
            }
        } else if (type == StatusMsgEventType) {/*
            StatusMsgEvent *evt = dynamic_cast<StatusMsgEvent *>(event);
            if (evt && cw->statusBar()) {
                cw->statusBar()->showMessage(evt->msg, evt->timeout);
                return true;
            } else {
                return false;
            }
                                                */
            return true;
        }
    }
    if (watched == this) {
        if (type == QuitEventType) {
            quit();
            return true;
        }
    }
    // otherwise do default action for event which probably means
    // propagate it down
    return  QApplication::eventFilter(watched, event);
}

void MainApp::logLine(const QString & line, const QColor & c)
{
    qApp->postEvent(consoleWindow, new LogLineEvent(line, c.isValid() ? c : defaultLogColor));
}

void MainApp::loadSettings()
{
    QSettings settings(SETTINGS_DOMAIN, SETTINGS_APP);

    settings.beginGroup("MainApp");
    debug = settings.value("debug", true).toBool();
	excessiveDebug = settings.value("excessiveDebug", excessiveDebug).toBool();
	saveCBEnabled = settings.value("saveChannelCB", true).toBool();

	dsFacilityEnabled = settings.value("dsFacilityEnabled", false).toBool();
    tmpDataFile.setTempFileSize(settings.value("dsTemporaryFileSize", 1048576000).toLongLong());

    mut.lock();
#ifdef Q_OS_WIN
    outDir = settings.value("outDir", "c:/users/code").toString();
#else
    outDir = settings.value("outDir", QDir::homePath() ).toString();
#endif
	lastOpenFile = settings.value("lastFileOpenFile", "").toString();
	
	m_sortGraphsByElectrodeId = settings.value("sortGraphsByElectrodeId", false).toBool();
	
    mut.unlock();
    {
        StimGLIntegrationParams & p(stimGLIntParams);
        p.iface = settings.value("StimGLInt_Listen_Interface", "0.0.0.0").toString();
        p.port = settings.value("StimGLInt_Listen_Port",  SPIKE_GL_NOTIFY_DEFAULT_PORT).toUInt();
        p.timeout_ms = settings.value("StimGLInt_TimeoutMS", SPIKE_GL_NOTIFY_DEFAULT_TIMEOUT_MSECS ).toInt(); 
    }
    {
        CommandServerParams & p (commandServerParams);
        p.iface = settings.value("CmdSrvr_Iface", "0.0.0.0").toString();
        p.port = settings.value("CmdSrvr_Port", DEFAULT_COMMAND_PORT).toUInt();
        p.timeout_ms = settings.value("CmdSrvr_TimeoutMS", DEFAULT_COMMAND_TIMEOUT_MS).toInt();
        p.enabled = settings.value("CmdSrvr_Enabled", true).toBool();
    }
    {
        BufSizesParams & p (bufSizesParams);
#ifdef WIN64
        p.regularMB = settings.value("BufSize_RegularAcq_MBx64", unsigned(DEF_SAMPLES_SHM_SIZE_REG)/(1024U*1024U)).toUInt();
        p.fgShmMB = settings.value("BufSize_FGAcq_MBx64", unsigned(DEF_SAMPLES_SHM_SIZE_FG)/(1024U*1024U)).toUInt();
#else
        p.regularMB = settings.value("BufSize_RegularAcq_MB", unsigned(DEF_SAMPLES_SHM_SIZE_REG)/(1024U*1024U)).toUInt();
        p.fgShmMB = settings.value("BufSize_FGAcq_MB", unsigned(DEF_SAMPLES_SHM_SIZE_FG)/(1024U*1024U)).toUInt();
#endif
    }
}

void MainApp::saveSettings()
{
    QSettings settings(SETTINGS_DOMAIN, SETTINGS_APP);

    settings.beginGroup("MainApp");
    settings.setValue("debug", debug);
	settings.setValue("excessiveDebug", excessiveDebug);
	settings.setValue("saveChannelCB", saveCBEnabled);

	settings.setValue("dsFacilityEnabled", dsFacilityEnabled);
    settings.setValue("dsTemporaryFileSize", tmpDataFile.getTempFileSize());

	settings.setValue("sortGraphsByElectrodeId", m_sortGraphsByElectrodeId);

    mut.lock();
    settings.setValue("outDir", outDir);
    mut.unlock();
	settings.setValue("lastFileOpenFile", lastOpenFile);
	
    {
        StimGLIntegrationParams & p(stimGLIntParams);
        settings.setValue("StimGLInt_Listen_Interface", p.iface);
        settings.setValue("StimGLInt_Listen_Port",  p.port);
        settings.setValue("StimGLInt_TimeoutMS", p.timeout_ms); 
    }
    {
        CommandServerParams & p(commandServerParams);
        settings.setValue("CmdSrvr_Iface", p.iface);
        settings.setValue("CmdSrvr_Port",  p.port);
        settings.setValue("CmdSrvr_TimeoutMS", p.timeout_ms); 
        settings.setValue("CmdSrvr_Enabled", p.enabled); 
    }    
    {
        BufSizesParams & p (bufSizesParams);
#ifdef WIN64
        settings.setValue("BufSize_RegularAcq_MBx64", p.regularMB);
        settings.setValue("BufSize_FGAcq_MBx64", p.fgShmMB);
#else
        settings.setValue("BufSize_RegularAcq_MB", p.regularMB);
        settings.setValue("BufSize_FGAcq_MB", p.fgShmMB);
#endif
    }
}


void MainApp::statusMsg(const QString &msg, int timeout)
{
    if (consoleWindow && consoleWindow->statusBar()) {
        QStatusBar *sbar = consoleWindow->statusBar();
        QFont f(sbar->font());
        static unsigned default_pointsize = 0;
        if (!default_pointsize) default_pointsize = f.pointSize();
        else f.setPointSize(default_pointsize);
        if (task && task->isRunning())
#ifdef Q_OS_WIN
            f.setPointSize(default_pointsize-1);
#else
            f.setPointSize(default_pointsize-2);
#endif
//        while (QFontMetrics(f).width(msg) > sbar->width()) {
//            f.setPointSize(f.pointSize()-1);
//            if (f.pointSize() == 5) break;
//        }
        sbar->setFont(f);
        consoleWindow->statusBar()->showMessage(msg, timeout);
    }
}

void MainApp::sysTrayMsg(const QString & msg, int timeout_msecs, bool iserror)
{
    if (sysTray) {
        sysTray->showMessage(APPNAME, msg, iserror ? QSystemTrayIcon::Critical : QSystemTrayIcon::Information, timeout_msecs);
    }
}

QString MainApp::sbString() const
{
    QMutexLocker ml(&mut);
    return sb_String;
}

void MainApp::setSBString(const QString &msg, int timeout) 
{
    QMutexLocker ml(&mut);
    sb_String = msg;
    sb_Timeout = timeout;
}

void MainApp::updateStatusBar()
{
    QMutexLocker ml(&mut);
    sysTray->setToolTip(sb_String);
    statusMsg(sb_String, sb_Timeout);
}

/** \brief A helper class that helps prevent reentrancy into certain functions.

    Mainly MainApp::pickOutputDir() makes use of this class to prevent recursive calls into itself. 

    Functions that want to be mutually exclusive with respect to each other
    and non-reentrant with respect to themselves need merely construct an 
    instance of this class as a local variable, and
    then reentrancy into the function can be guarded by checking against
    this class's operator bool() function.
*/
struct ReentrancyPreventer
{
    static volatile int ct;
    /// Increments a global counter.
    /// The global counter is 1 if only 1 instance of this class exists throughout the application, and >1 otherwise.
    ReentrancyPreventer() { ++ct; }
    /// Decrements the global counter.  
    /// If it reaches 0 this was the last instance of this class and there are no other ones active globally.
    ~ReentrancyPreventer() {--ct; }
    /// Returns true if the global counter is 1 (that is, only one globally active instance of this class exists throughout the application), and false otherwise.  If false is returned, you can then abort your function early as a reentrancy condition has been detected.
    operator bool() const { return ct == 1; }
};
volatile int ReentrancyPreventer::ct = 0;

void MainApp::about()
{
    QMessageBox::about(consoleWindow, "About " APPNAME,
                       VERSION_STR
                       "\n\n(C) 2010-2016 Calin A. Culianu <calin.culianu@gmail.com>\n\n"
                       "Developed for the Anthony Leonardo lab at\n"
                       "Janelia Farm Research Campus, HHMI\n\n"
                       "Software License: GPL v2 or later\n\n"
					   "Bitcoin Address: 1Ca1inQuedcKdyELCTmN8AtKTTehebY4mC\n"
					   "Git Repository: https://www.github.com/cculianu/SpikeGL"
					   );
	// find the QLabel for the above text to make it selectable...
	foreach (QWidget *w, QApplication::topLevelWidgets()) {
		//Debug() << "Window title: " << w->windowTitle();
		QList<QLabel *> chlds = w->findChildren<QLabel *>();
		foreach (QLabel *l, chlds) {
			//Debug() << "   Label text: " << l->text();
			if (l->text().startsWith(VERSION_STR)) { 
				// found it! make text selectable so they can email or bitcoin me! :)
				l->setTextInteractionFlags(Qt::LinksAccessibleByMouse|Qt::TextSelectableByKeyboard|Qt::TextSelectableByMouse);
			}
		}
	}	
}

bool MainApp::setOutputDirectory(const QString & dpath)
{
    QDir d(dpath);
    if (!d.exists()) return false;
    mut.lock();
    outDir = dpath;
    mut.unlock();
    return true;
}

void MainApp::pickOutputDir()
{
    ReentrancyPreventer rp; if (!rp) return;
    mut.lock();
    QString od = outDir;
    mut.unlock();
    noHotKeys = true;
    if ( !(od = QFileDialog::getExistingDirectory(0, "Choose a directory to which to save output files", od, QFileDialog::DontResolveSymlinks|QFileDialog::ShowDirsOnly)).isNull() ) { 
        mut.lock();
        outDir = od;
        mut.unlock();
        saveSettings(); // just to remember the file *now*
    }
    noHotKeys = false;
}

void MainApp::createIcons()
{
    appIcon.addPixmap(QPixmap(Icon_xpm));
	bugIcon.addPixmap(QPixmap(QString(":/Bug3/dragonfly.png")));
}

void MainApp::hideUnhideConsole()
{
    if (consoleWindow) {
        if (consoleWindow->isHidden()) {
            consoleWindow->show();
#ifdef Q_OS_MACX
			windowMenuAdd(consoleWindow);
#endif
        } else {
            bool hadfocus = ( focusWidget() == consoleWindow );
            consoleWindow->hide();
#ifdef Q_OS_MACX
			windowMenuRemove(consoleWindow);
#endif
            if (graphsWindow && hadfocus) graphsWindow->setFocus(Qt::OtherFocusReason);
        }
    }
}

void MainApp::hideUnhideGraphs()
{
    if (graphsWindow) {
        if (graphsWindow->isHidden()) {
            graphsWindow->clearGraph(-1);
            graphsWindow->show();
        } else {
            bool hadfocus = ( focusWidget() == graphsWindow );
            graphsWindow->hide();
            if (consoleWindow && hadfocus) consoleWindow->setFocus(Qt::OtherFocusReason);
        }
    }

}


void MainApp::initActions()
{
    Connect( quitAct = new QAction("&Quit", this) , 
             SIGNAL(triggered()), this, SLOT(maybeQuit()));
    Connect( toggleDebugAct = new QAction("&Debug Mode D", this) ,
             SIGNAL(triggered()), this, SLOT(toggleDebugMode()));
    Connect( toggleExcessiveDebugAct = new QAction("Excessive Debug Mode Ctrl+D", this) ,
             SIGNAL(triggered()), this, SLOT(toggleExcessiveDebugMode()));

	toggleDebugAct->setCheckable(true);
	toggleDebugAct->setChecked(isDebugMode());
	toggleExcessiveDebugAct->setCheckable(true);
	toggleExcessiveDebugAct->setChecked(excessiveDebug);

    Connect( chooseOutputDirAct = new QAction("Choose &Output Directory...", this),
             SIGNAL(triggered()), this, SLOT(pickOutputDir()));
    Connect( hideUnhideConsoleAct = new QAction("Hide/Unhide &Console C", this),
             SIGNAL(triggered()), this, SLOT(hideUnhideConsole()));
    Connect( hideUnhideGraphsAct = new QAction("Hide/Unhide &Graphs G", this),
             SIGNAL(triggered()), this, SLOT(hideUnhideGraphs()));
    hideUnhideGraphsAct->setEnabled(false);

    Connect( aoPassthruAct = new QAction("AO Passthru...", this),
             SIGNAL(triggered()), this, SLOT(respecAOPassthru()));
    aoPassthruAct->setEnabled(false);
    
    Connect( helpAct = new QAction("SpikeGL &Help", this), 
             SIGNAL(triggered()), this, SLOT(help()));
    Connect( aboutAct = new QAction("&About", this), 
             SIGNAL(triggered()), this, SLOT(about()));
    Connect(aboutQtAct = new QAction("About &Qt", this),
            SIGNAL(triggered()), this, SLOT(aboutQt()));
            
    Connect( newAcqAct = new QAction("New NI-DAQ Acquisition... &N", this),
             SIGNAL(triggered()), this, SLOT(newAcq()));
    Connect( bugAcqAct = new QAction("New Bug Acquisition... &B", this),
			SIGNAL(triggered()), this, SLOT(bugAcq()));
    Connect( fgAcqAct = new QAction("New Framegrabber Acquisition... &F", this),
			SIGNAL(triggered()), this, SLOT(fgAcq()));
    Connect( stopAcq = new QAction("Stop Running Acquisition ESC", this),
             SIGNAL(triggered()), this, SLOT(maybeCloseCurrentIfRunning()) );
    stopAcq->setEnabled(false);

    Connect( verifySha1Act = new QAction("Verify SHA1...", this),
             SIGNAL(triggered()), this, SLOT(verifySha1()) );

    Connect( par2Act = new QAction("PAR2 Redundancy Tool", this),
             SIGNAL(triggered()), this, SLOT(showPar2Win()) );

    Connect( stimGLIntOptionsAct = new QAction("StimGL Integration Options", this),
             SIGNAL(triggered()), this, SLOT(execStimGLIntegrationDialog()) );
	
    Connect( commandServerOptionsAct = new QAction("Command Server Options", this),
             SIGNAL(triggered()), this, SLOT(execCommandServerOptionsDialog()) );

	Connect( showChannelSaveCBAct = new QAction("Show Save Checkboxes", this), 
		     SIGNAL(triggered()), this, SLOT(toggleShowChannelSaveCB()) );
	showChannelSaveCBAct->setCheckable(true);
	showChannelSaveCBAct->setChecked(isSaveCBEnabled());

	Connect( enableDSFacilityAct = new QAction("Enable Matlab Data API", this) ,
             SIGNAL(triggered()), this, SLOT(toggleEnableDSFacility()));
	enableDSFacilityAct->setCheckable(true);
	enableDSFacilityAct->setChecked(isDSFacilityEnabled());

	Connect( tempFileSizeAct = new QAction("Matlab Data API Tempfile...", this),
             SIGNAL(triggered()), this, SLOT(execDSTempFileDialog()) );
    tempFileSizeAct->setEnabled(isDSFacilityEnabled());

    Connect( bufferSizesDialogAct = new QAction("Specify Realtime Buffer Sizes...", this),
             SIGNAL(triggered()), this, SLOT(execBufferSizesDialog()) );
	
	Connect( fileOpenAct = new QAction("Open... &O", this), SIGNAL(triggered()), this, SLOT(fileOpen())); 
	
	Connect( bringAllToFrontAct = new QAction("Bring All to Front", this), SIGNAL(triggered()), this, SLOT(bringAllToFront()) );
	
	Connect( sortGraphsByElectrodeAct = new QAction("Sort graphs by Electrode", this), SIGNAL(triggered()), this, SLOT(optionsSortGraphsByElectrode()) );
	sortGraphsByElectrodeAct->setCheckable(true);
	sortGraphsByElectrodeAct->setChecked(m_sortGraphsByElectrodeId);
}

static inline unsigned long computeSamplesShmPageSize(double samplingRateHz, unsigned scanSizeSamps, bool lowLatency, unsigned metaBytesPerScan = 0, unsigned *metaBytesPerPage = 0) {
    unsigned long oneScanBytes = scanSizeSamps * sizeof(int16);
    unsigned long nScansPerPage = (unsigned long)qRound(samplingRateHz * double((double)SAMPLES_SHM_DESIRED_PAGETIME_MS/1000.0));
    if (lowLatency) nScansPerPage /= 2;
    if (!nScansPerPage) nScansPerPage = 1;
    if (metaBytesPerPage) *metaBytesPerPage = nScansPerPage * metaBytesPerScan;
    return nScansPerPage*oneScanBytes + metaBytesPerScan*nScansPerPage;
}


bool MainApp::startAcq(QString & errTitle, QString & errMsg) 
{
	QMutexLocker ml (&mut);
	
    // NOTE: acq cannot be running here!
    if (isAcquiring()) {
        errTitle = "Already running!";
        errMsg = "The acquisition is already running.  Please stop it first.";
        return false;
    }
    fgWindow = 0;
    scanCt = 0;
    scanSkipCt = 0;
    lastScanSz = 0;
	stopRecordAtSamp = -1;
    tNow = getTime();
    taskShouldStop = false;
    pdWaitingForStimGL = false;
    queuedParams.clear();
    sgl_ended = sgl_save = sgl_started = SGL_Parms();
    got_sgl_ended = got_sgl_save = got_sgl_started = false;

    if (!configCtl || !bugConfig || !fgConfig) {
        errTitle = "Internal Error";
        errMsg = "configCtl and/or bugConfig and/or fgConfig pointer is NULL! Shouldn't happen!";
        return false;
    }
    DAQ::Params & params(doBugAcqInstead ? bugConfig->acceptedParams : (doFGAcqInstead ? fgConfig->acceptedParams : configCtl->acceptedParams));    
    lastNPDSamples.clear();
    lastNPDSamples.reserve(params.pdThreshW);
    if (!params.stimGlTrigResave) {
        if (!dataFile.openForWrite(params)) {            
            errTitle = "Error Opening File!";
            errMsg = QString("Could not open data file `%1'!").arg(params.outputFile);
            return false;
        }
        if (!queuedParams.isEmpty()) stimGL_SaveParams("", queuedParams);
    }

    if (samplesBuffer && need2FreeSamplesBuffer)  free(samplesBuffer);
    samplesBuffer = 0; need2FreeSamplesBuffer = false;

    int shmSizeMB = doFGAcqInstead ? bufSizesParams.fgShmMB : bufSizesParams.regularMB;
    if (sizeof(void *) <= 4 && shmSizeMB > 2047) shmSizeMB = 2047;
    const long shmSizeBytes = long(shmSizeMB)*1024L*1024L;

    if (doFGAcqInstead) {
        if (!shm.isAttached()) {
            shm.setKey(SAMPLES_SHM_NAME);

            if (shm.attach()) {
                shm.detach(); // for some reason doing this under windows often 'fixes' the problem
                if (shm.attach()) {
                    shm.detach();
                    errTitle = "Shared Memory Error";
                    errMsg = QString("The shared memory segment already exists.\n\nPlease kill all instances of SpikeGL and its subprocesses to continue.");
                    Error() << errTitle << ": " << errMsg;
                    return false;
                }
            }
            if (!shm.create(shmSizeBytes)) {
                errTitle = "Shared Memory Error";
                errMsg = QString("Error creating the shared memory segment.\n\nError was #") + QString::number(int(shm.error())) + ": \n`" + shm.errorString() + "'\n\nSpikeGL requires enough memory to create a buffer of size " + QString::number(shmSizeMB) + " MB.";
                return false;
            } else {
                if ( shm.size() < shmSizeBytes ) {
                    shm.detach();
                    errTitle = "Shm Segment Wrong Size";
                    errMsg = QString("Shm segment attached ok, but it is too small.  Required size: ") + QString::number(shmSizeBytes) + " Current size: " + QString::number(shm.size());
                    return false;
                } else {
                    Log() << "Successfully created '" << SAMPLES_SHM_NAME <<"' shm segment of size " << QString::number(shmSizeMB) << "MB";
                }
            }
        } else if ( shm.size() < shmSizeBytes ) {
            shm.detach();
            errTitle = "Shm Segment Wrong Size";
            errMsg = QString("Shm segment attached ok, but it is too small.  Required size: ") + QString::number(shmSizeBytes) + " Current size: " + QString::number(shm.size());
            return false;
        }
        samplesBuffer = shm.data();
        need2FreeSamplesBuffer = false;
    } else { // not framegrabber acq, so don't use  a SHM, instead just malloc the required memory
        need2FreeSamplesBuffer = false;
        samplesBuffer = malloc(shmSizeBytes);
        if (!samplesBuffer) {
            errTitle = "Not Enough Memory";
            errMsg = QString("Failed to allocate a sample buffer of size ") + QString::number(shmSizeMB) + " MB.\n\nSpikeGL requires a large sample buffer to avoid potential overruns.  Free up some memory or upgrade your system! ";
            return false;
        }
        need2FreeSamplesBuffer = true;
        Log() << "Successfully created '" << SAMPLES_SHM_NAME <<"' sample buffer size " << QString::number(shmSizeMB) << "MB";
    }

	// re-set the data temp file, delete it, etc
	tmpDataFile.close();		
    tmpDataFile.setNChans(params.nVAIChans);


    // acq starting dialog block -- show this dialog because the startup is kinda slow..
    if (acqStartingDialog) delete acqStartingDialog, acqStartingDialog = 0;
    acqStartingDialog = new QMessageBox ( QMessageBox::Information, "DAQ Task Starting Up", "DAQ task starting up, please wait...", QMessageBox::Ok, consoleWindow, Qt::WindowFlags(Qt::Dialog| Qt::MSWindowsFixedSizeDialogHint));
    acqStartingDialog->setWindowModality(Qt::ApplicationModal);
    QAbstractButton *but = acqStartingDialog->button(QMessageBox::Ok);
    if (but) but->hide();
    acqStartingDialog->open();
    // end acq starting dialog block
    
    preBuf.clear();
    preBuf.reserve(0);
    if (params.usePD && (params.acqStartEndMode == DAQ::PDStart || params.acqStartEndMode == DAQ::PDStartEnd
						 || params.acqStartEndMode == DAQ::AITriggered || params.acqStartEndMode == DAQ::Bug3TTLTriggered)) {
        const double sil = params.silenceBeforePD > 0. ? params.silenceBeforePD : DEFAULT_PD_SILENCE;
        if (params.stimGlTrigResave) pdWaitingForStimGL = true;
        int szSamps = params.nVAIChans*params.srate*sil;
        if (szSamps <= 0) szSamps = params.nVAIChans;
        if (szSamps % params.nVAIChans) 
            szSamps += params.nVAIChans - szSamps%params.nVAIChans;
        preBuf.reserve(szSamps*sizeof(int16));
		char *mem = new char[preBuf.capacity()];
		memset(mem, 0, preBuf.capacity());
		preBuf.putData(mem, preBuf.capacity());
		delete [] mem;
    }
    
    if (doFGAcqInstead) {
        if (params.fg.disableChanMap) {
            // for testing 4/26/16 -- we just sort by intan for testing.
            m_sortGraphsByElectrodeId = false;
            sortGraphsByElectrodeAct->setChecked(false);
        } else {
            // hardcoded .. make user experience be that by default, in framegrabber more, we sort graphs by electrode, so that the channel remapping thing looks right onscreen
            m_sortGraphsByElectrodeId = true;
            sortGraphsByElectrodeAct->setChecked(true);
        }
    }

    graphsWindow = new GraphsWindow(params, 0, dataFile.isOpen(), !doFGAcqInstead, doFGAcqInstead ? params.graphUpdateRate : -1);
    graphsWindow->setAttribute(Qt::WA_DeleteOnClose, false);
    
    Connect(this, SIGNAL(do_setPDTrigLED(bool)), graphsWindow, SLOT(setPDTrig(bool)));
    Connect(this, SIGNAL(do_setManualTrigEnabled(bool)), graphsWindow, SLOT(setTrigOverrideEnabled(bool)));
    Connect(this, SIGNAL(do_setSGLTrig(bool)), graphsWindow, SLOT(setSGLTrig(bool)));

    graphsWindow->setWindowIcon(appIcon);
    hideUnhideGraphsAct->setEnabled(true);
    graphsWindow->installEventFilter(this);
	
	windowMenuAdd(graphsWindow);
	// TESTING OF SPATIAL VISUALIZATION WINDOW -- REMOVE ME TO NOT USE SPATIAL VIS
    unsigned spatialBoxW = MAX(graphsWindow->numColsPerGraphTab(),graphsWindow->numRowsPerGraphTab());
    Vec2i spatialDims;
    if (doFGAcqInstead) {
        spatialDims =  Vec2i(params.fg.spatialCols, params.fg.spatialRows);
    } else {
        int nvai = params.nVAIChans, x = 1, y = 1;
        bool ff = true;
        while (x*y < nvai) {
            if (ff) ++x; else ++y;
            ff = !ff;
        }
        spatialDims = Vec2i(x,y);
    }

    spatialWindow = new SpatialVisWindow(params, spatialDims, spatialBoxW, 0, doFGAcqInstead ? params.spatialVisUpdateRate : -1, doFGAcqInstead && params.fg.spatialVisSuppressExtraChans);
    spatialWindow->setSorting(graphsWindow->currentSorting(), graphsWindow->currentNaming());
    spatialWindow->setGraphTimesSecs(graphsWindow->getGraphTimesSecs());
    spatialWindow->setDownsampleRatio(graphsWindow->getDownsampleRatio());
    spatialWindow->setAttribute(Qt::WA_DeleteOnClose, false);
	spatialWindow->setWindowIcon(appIcon);
    spatialWindow->installEventFilter(this);
	windowMenuAdd(spatialWindow);
	
    Connect(graphsWindow, SIGNAL(manualTrig(bool)), this, SLOT(gotManualTrigOverride(bool)));
    Connect(graphsWindow, SIGNAL(sortingChanged(const QVector<int> &, const QVector<int> &)), spatialWindow, SLOT(setSorting(const QVector<int> &, const QVector<int> &)));
    Connect(graphsWindow, SIGNAL(graphTimeSecsChanged(int,double)), spatialWindow, SLOT(setGraphTimeSecs(int,double)));
    Connect(graphsWindow, SIGNAL(downsampleRatioChanged(double)), spatialWindow, SLOT(setDownsampleRatio(double)));

    if (!params.suppressGraphs) {
		//spatialWindow->show();
        graphsWindow->show();

#if QT_VERSION >= 0x050000
    // Iff app built with Qt Creator, then graphs window
    // will not get any mouse events until a modal dialog
    // shows on top and is then destroyed!! That's what we
    // do here...make an invisible message box.

        {
            QMessageBox XX( consoleWindow );
            XX.setWindowModality( Qt::ApplicationModal );
            XX.setAttribute( Qt::WA_DontShowOnScreen, true );
            XX.move( QApplication::desktop()->screen()->rect().topLeft() );
            XX.show();
            // auto-destroyed
        }
#endif

    } else {
		spatialWindow->hide();
        graphsWindow->hide();
    }
    taskWaitingForStop = false;
    taskHasManualTrigOverride = false;

    switch (params.acqStartEndMode) {
        case DAQ::Immediate: taskWaitingForTrigger = false; break;
        case DAQ::Timed:
            if (params.isImmediate) {
                taskWaitingForTrigger = false;
                startScanCt = 0;
            } else {
                startScanCt = i64(params.startIn * params.srate);
            }
            stopScanCt = params.isIndefinite ? 0x7fffffffffffffffLL : i64(startScanCt + params.duration*params.srate);            
        case DAQ::PDStartEnd:
        case DAQ::PDStart:
        case DAQ::StimGLStartEnd:
        case DAQ::StimGLStart:
		case DAQ::AITriggered:
		case DAQ::Bug3TTLTriggered:
            taskWaitingForTrigger = true; break;
        default:
            errTitle = "Internal Error";
            errMsg = "params.acqStartEndMode is an illegal value!  FIXME!";
            Error() << errTitle << " " << errMsg;
            return false;
    }

    if (params.acqStartEndMode == DAQ::Bug3TTLTriggered) {
        graphsWindow->setTrigOverrideEnabled(true);
    } else
        graphsWindow->setTrigOverrideEnabled(false);

    if (reader) delete reader, reader = 0;
    reader = new PagedScanReader(params.nVAIChans, 0, samplesBuffer, shmSizeBytes, computeSamplesShmPageSize(params.srate,params.nVAIChans,params.lowLatency));
    reader->bzero();

	DAQ::NITask *nitask = 0;
	DAQ::BugTask *bugtask = 0;
	DAQ::FGTask *fgtask = 0;
    if (!doBugAcqInstead && !doFGAcqInstead) {
        task = nitask = new DAQ::NITask(params, this, *reader);
    } else if (doBugAcqInstead) {
        delete reader;  // need to force the page size to something smaller.. for bug's metadata requirements
        reader = new PagedScanReader(params.nVAIChans, sizeof(DAQ::BugTask::BlockMetaData), samplesBuffer, shmSizeBytes, DAQ::BugTask::requiredShmPageSize(params.nVAIChans));
        task = bugtask = new DAQ::BugTask(params, this, *reader);
    } else if (doFGAcqInstead) {
        delete reader; // need to force the page size to something that supports metadata
        unsigned metaSzPerPage = 0, metaBytesPerScan = sizeof(unsigned int); // just take the latest 32-bit timestamp value per scan.. even though FPGA gives us a value per row
        unsigned pgSize = computeSamplesShmPageSize(params.srate, params.nVAIChans, params.lowLatency, metaBytesPerScan, &metaSzPerPage);
        reader = new PagedScanReader(params.nVAIChans, metaSzPerPage, samplesBuffer, shmSizeBytes, pgSize);
        task = fgtask = new DAQ::FGTask(params, this, *reader);
        fgWindow = fgtask->dialogW;
    }
    Debug() << "SamplesSHM Page Size: " << reader->pageSize() << " bytes (" << reader->scansPerPage() << " scans per page), " << reader->nPages() << " total pages";

    if (gthread1) delete gthread1, gthread1 = 0;
    if (gthread2) delete gthread2, gthread2 = 0;
    if (dthread) delete dthread, dthread = 0;
    gthread1 = new GraphingThread(graphsWindow, *reader, params);
    gthread2 = new GraphingThread(spatialWindow, *reader, params);

	doBugAcqInstead = false;
	doFGAcqInstead = false;
    Connect(task, SIGNAL(bufferOverrun()), this, SLOT(gotBufferOverrun()));
    Connect(task, SIGNAL(taskError(const QString &)), this, SLOT(gotTaskError(const QString &)));
    Connect(task, SIGNAL(taskWarning(const QString &)), this, SLOT(gotTaskWarning(const QString &)));
	
	if (bugtask) {
		bugWindow = new Bug_Popout(bugtask,0);
		bugWindow->setAttribute(Qt::WA_DeleteOnClose, false);	
		bugWindow->installEventFilter(this);
		windowMenuAdd(bugWindow);
		if (!params.suppressGraphs) { bugWindow->show(); bugWindow->activateWindow(); }
	}	
	
    if (fgtask) { // HACK, testing for now!!
        spatialWindow->setSelectionEnabled(true);
        Connect(spatialWindow, SIGNAL(channelsSelected(const QVector<unsigned> &)), graphsWindow, SLOT(openGraphsById(const QVector<unsigned> &)));
        spatialWindow->selectChansStartingAt(0);
        graphsWindow->setDownsampling(true);
        graphsWindow->setDownsamplingCheckboxEnabled(false);
        delete acqStartingDialog; acqStartingDialog = 0;
        windowMenuActivate(spatialWindow);
        fgtask->dialogW->show();
        fgtask->dialogW->activateWindow();
    } else if (bugtask && params.acqStartEndMode == DAQ::Bug3TTLTriggered) {
        // force downsampling off because we need the on-screen data for our manual trigger override feature "prepend" feature!
        graphsWindow->setDownsampling(false);
        graphsWindow->setDownsamplingCheckboxEnabled(false);
        graphsWindow->setDownsamplingSpinboxEnabled(false);
    } else {
        graphsWindow->setDownsamplingCheckboxEnabled(true);
    }
	
    stopAcq->setEnabled(true);
    aoPassthruAct->setEnabled(params.aoPassthru && !bugtask && !fgtask);
    Connect(task, SIGNAL(gotFirstScan()), this, SLOT(gotFirstScan()));
    if (gthread1) gthread1->start(QThread::LowPriority);
    if (gthread2) gthread2->start(QThread::LowestPriority);
    dthread = new DataSavingThread(this);
    dthread->start(QThread::HighPriority);
    task->start();
    updateWindowTitles();
    //Systray() << "DAQ task starting up ...";
    Status() << "DAQ task starting up ...";
    Log() << "DAQ task starting up ...";
    
    QTimer::singleShot(10, this, SLOT(activateWindowsAfterAcqStart()));

    return true;
}

void MainApp::activateWindowsAfterAcqStart()
{
    // New: Qt5 doesn't raise the windows like the old Qt did, so we have to explicitly do it
    //if (graphsWindow) graphsWindow->raise();
    if (consoleWindow && !consoleWindow->isHidden()) consoleWindow->activateWindow(), consoleWindow->raise();
    if (bugWindow && !bugWindow->isHidden()) bugWindow->activateWindow(), bugWindow->raise();
    DAQ::FGTask *fgtask = dynamic_cast<DAQ::FGTask *>(task);
    if (fgtask && fgtask->dialogW && !fgtask->dialogW->isHidden()) fgtask->dialogW->activateWindow(), fgtask->dialogW->raise();
}

void MainApp::newAcq() 
{	
	if (acqWaitingForPrecreate) return; ///< disable 'N' key or new app menu spamming...
	
    if ( !maybeCloseCurrentIfRunning() ) return;
    if (DAQ::ProbeAllAIChannels().empty()) {
            QMessageBox::critical(0, "NI-DAQ AI Channels Not Found", "NI-DAQ data acquisition is unavailable because no AI channels were found on this system.\n\nTo enable NI-DAQ Acquisitions:\n\t1. Make sure NIDAQmx is installed.\n\t2. Configure either a test device or a real device in NI MAX.");
            return;
    }
    noHotKeys = true;
    int ret = configCtl->exec();
    noHotKeys = false;
    if (ret == QDialog::Accepted) {
		doBugAcqInstead = false;
		doFGAcqInstead = false;
		if (pregraphs.count() < maxPreGraphs) {
			acqWaitingForPrecreate = true;
			noHotKeys = true;
			showPrecreateDialog();
		} else 
			startAcqWithPossibleErrDialog();
	}
}

void MainApp::bugAcq()
{
	if (acqWaitingForPrecreate) return; ///< disable 'B' key or new app menu spamming...
    if ( !maybeCloseCurrentIfRunning() ) return;
    noHotKeys = true;
    int ret = bugConfig->exec();
    noHotKeys = false;
    if (ret == QDialog::Accepted) {
		doBugAcqInstead = true;
        doFGAcqInstead = false;
		if (pregraphs.count() < maxPreGraphs) {
			acqWaitingForPrecreate = true;
			noHotKeys = true;
			showPrecreateDialog();
		} else 
			startAcqWithPossibleErrDialog();
	}
}

/// new *framegrabber* acquisition!
void MainApp::fgAcq()
{
	if (acqWaitingForPrecreate) return; ///< disable 'F' key or new app menu spamming...
    if ( !maybeCloseCurrentIfRunning() ) return;
    if ( isDSFacilityEnabled() ) {
        int answer =
                QMessageBox::question(consoleWindow, "Disable Matlab Data API?",
                              "The Matlab Data API concurrently writes a temp file while the acquisition takes place.\n\n"
                              "This can impact performance for the Framegrabber acquisition mode, and is officially unsupported.\n\n"
                              "Disable Matlab Data API and continue?",
                              "Disable Matlab 'Data' API", "Cancel Operation",0,-1);
        if (answer == 1) return;
        if (isDSFacilityEnabled()) enableDSFacilityAct->trigger();
    }
    noHotKeys = true;
    int ret = fgConfig->exec();
    noHotKeys = false;
    if (ret == QDialog::Accepted) {
		doBugAcqInstead = false;
		doFGAcqInstead = true;
		if (pregraphs.count() < maxPreGraphs) {
			acqWaitingForPrecreate = true;
			noHotKeys = true;
			showPrecreateDialog();
		} else 
			startAcqWithPossibleErrDialog();
	}	
}


/// check if everything is ok, ask user, then quit
void MainApp::maybeQuit()
{
    if (task) {
        int but = QMessageBox::question(0, "Confirm Exit", "An acquisition is curently running!\nStop the acquisition and quit?", QMessageBox::Yes, QMessageBox::No);
        if (but != QMessageBox::Yes) return;
    }
    quit();
}

void MainApp::stopTask()
{
    // This was causing deadlocks in new threaded graphing/data saving scheme.  So line is removed.
    // it appears no locking is needed here.  But.. TODO: *more thoroughly verify* that no locking is needed here!
    //QMutexLocker ml (&mut);
	
    if (!task) return;
    if (task->isRunning()) task->stop();
    if (gthread1) delete gthread1, gthread1 = 0;
    if (gthread2) delete gthread2, gthread2 = 0;
    if (dthread) {
        QMessageBox *mb = 0;
        if ((reader->latest() - reader->latestPageRead()) * SAMPLES_SHM_DESIRED_PAGETIME_MS > 500) {
            // if we are more than 500ms behind in data saving, indicate there will be a delay
            // in ending the acquisition to the user via a messagebox..
            mb=new QMessageBox ( QMessageBox::Information, "Saving Data...", "Saving pending data, please wait...", QMessageBox::Ok, consoleWindow, Qt::WindowFlags(Qt::Dialog| Qt::MSWindowsFixedSizeDialogHint));
            mb->setWindowModality(Qt::ApplicationModal);
            QAbstractButton *but = mb->button(QMessageBox::Ok);
            if (but) but->hide();
            mb->show();
        }
        delete dthread, dthread = 0; // delete data saving thread.  this may block for a little bit as the data saving thread reads old data, depending on the stop condition.
        if (mb) delete mb;
    }
    if (bugWindow) {
		windowMenuRemove(bugWindow);
		delete bugWindow, bugWindow = 0;
	}
    delete task, task = 0;
	doBugAcqInstead = false;
	doFGAcqInstead = false;
    fastSettleRunning = false;
    fgWindow = 0;

    // . <--- at this point there is no more threading going on, everything is on main thread.

    if (graphsWindow) {
		windowMenuRemove(graphsWindow);
		delete graphsWindow, graphsWindow = 0;
	}
	if (spatialWindow) {
		windowMenuRemove(spatialWindow);
		delete spatialWindow, spatialWindow = 0;
	}
    hideUnhideGraphsAct->setEnabled(false);
    Log() << "Task " << dataFile.fileName() << " stopped.";
    Status() << "Task stopped.";
    dataFile.closeAndFinalize();
    queuedParams.clear();
    stopAcq->setEnabled(false);
    aoPassthruAct->setEnabled(false);
    taskWaitingForTrigger = false;
    taskHasManualTrigOverride = false;
	stopRecordAtSamp = -1;
    updateWindowTitles();
	if (acqStartingDialog) delete acqStartingDialog, acqStartingDialog = 0;
    unsigned long bufSize = reader ? reader->totalSize() : 0;
    if (reader) delete reader, reader = 0;
    if (need2FreeSamplesBuffer && samplesBuffer) {
        free(samplesBuffer);
        Log() << "Freed `" << SAMPLES_SHM_NAME << "' sample buffer of size " << (bufSize/(1024*1024)) << "MB";
    }
    if (shm.isAttached()) {
        shm.detach();
        Log() << "Deleted `" << SAMPLES_SHM_NAME << "' shm of size " << (bufSize/(1024*1024)) << "MB";
    }
    samplesBuffer = 0; need2FreeSamplesBuffer = false;
    //Systray() << "Acquisition stopped";
}

bool MainApp::maybeCloseCurrentIfRunning() 
{
    if (!task) return true;
    int but;
    if (dataFile.isOpen()) 
        but = QMessageBox::question(0, "Stop Current Acquisition", QString("An acquisition is currently running and saving to %1.\nDo you wish to stop it?").arg(dataFile.fileName()), QMessageBox::Yes, QMessageBox::No);
    else
        but = QMessageBox::question(0, "Stop Current Acquisition", QString("An acquisition is currently running.\nDo you wish to stop it?"), QMessageBox::Yes, QMessageBox::No);
    if (but == QMessageBox::Yes) {
        stopTask();
        return true;
    }
    return false;
}

void MainApp::gotBufferOverrun()
{
    Warning() << "Buffer overrun! Aieeeee! TODO FIXME!!!";
}

void MainApp::gotTaskError(const QString & e)
{
    QMessageBox::critical(0, "DAQ Error", e);
    stopTask();
}

void MainApp::gotTaskWarning(const QString & e)
{
    QMessageBox::critical(0, "DAQ Warning", e);
}


/* static */
void MainApp::prependPrebufToScans(const WrapBuffer & preBuf, std::vector<int16> & scans, int & num, int skip)
{
    if (scans.capacity() < (preBuf.size()/sizeof(int16))) scans.reserve(preBuf.size()/sizeof(int16));
    void *ptr=0;
    unsigned lenBytes=0, lenElems = 0;
    preBuf.dataPtr1(ptr, lenBytes);
	lenElems = lenBytes / sizeof(int16);
	num = 0;
    if (ptr && skip < (int)lenElems) {
        scans.insert(scans.begin(), reinterpret_cast<int16 *>(ptr) + skip, reinterpret_cast<int16 *>(ptr)+skip+(lenElems-skip));
		num += lenElems - skip;
    }
	skip -= lenElems;
	if (skip < 0) skip = 0;
    preBuf.dataPtr2(ptr, lenBytes);
	lenElems = lenBytes / sizeof(int16);
    if (ptr && skip < (int)lenElems) {
        scans.insert(scans.begin()+num, reinterpret_cast<int16 *>(ptr) + skip, reinterpret_cast<int16 *>(ptr)+skip+(lenElems-skip));
		num += lenElems - skip;
    }
	skip -= lenElems;
}



void MainApp::putRestarts(const DAQ::Params & p, u64 firstSamp, u64 restartNumScans) const
{
    const u64 dfScanNr = dataFile.isOpen() ? dataFile.scanCount() : 0;
    const u64 scanNr = firstSamp/p.nVAIChans;

    Warning() << "Buffer overflow - scan: " << scanNr << ", datafile scan: " << dfScanNr << " (size: " << restartNumScans << " scans). Scans for missing time are fudged with MAX_VOLTS!";
	QFileInfo fi(p.outputFileOrig);
	if (!fi.isAbsolute()) {
		fi.setFile(outputDirectory() + "/" + p.outputFileOrig);
	}
    QString dir = fi.path();
    QString fn = fi.fileName();
	QString ext = fi.completeSuffix();
    if (ext.length()) {
	    fn.chop(ext.length()+1);
    }
    QFile of (dir + "/" + fn + ".restarts");
    of.open(QIODevice::Text|QIODevice::Append);
    QTextStream ts(&of);
    if (!of.size()) {
        ts << "# .restarts file for `" << fn << "'. This file indicates at which scans and times there was a DAQ error requiring an AI restart.\n";
        ts << "# Data columns are:\n";
        ts << "# TIMESTAMP TRIAL_NAME ABS_SCAN_BEGIN ABS_SCAN_END TRIAL_RELATIVE_SCAN_BEGIN TRIAL_RELATIVE_SCAN_END\n";
    }
    QString trialName = "<NO TRIAL>";
    if (dataFile.isOpen()) {
        trialName = dataFile.fileName();
        QFileInfo fi2(trialName);
        trialName = fi2.baseName();
    }
    ts  << QDateTime::currentDateTime().toString(Qt::ISODate) << "\t" 
        << trialName << "\t"
        << scanNr << "\t"
        << (scanNr+restartNumScans) << "\t";
    if (dataFile.isOpen()) {
        ts << dfScanNr << "\t" << (dfScanNr + restartNumScans);
    } else {
        ts << "0\t0";
    }
    ts << "\n";
}

bool MainApp::sortGraphsByElectrodeId() const { return m_sortGraphsByElectrodeId || (configCtl && configCtl->acceptedParams.bug.enabled); }


MainApp::GraphingThread::GraphingThread(GenericGrapher *g, const PagedScanReader & psr, const DAQ::Params &p)
    : QThread(dynamic_cast<QObject *>(g)), g(g), reader(psr), p(p), pleaseStop(false)
{
    sampCount = 0ULL;
    reader.resetToBeginning();
}

MainApp::GraphingThread::~GraphingThread() {
    pleaseStop = true;
    if (isRunning())  wait(); // wait forever? *ahme* should always work since in theory the run function doesn't block for very long
}

void MainApp::GraphingThread::run()
{
    int nChansPerScan = reader.scanSizeSamps();
    int nScansPerPage = reader.scansPerPage();
    const std::vector<int16> droppedPageScans(nScansPerPage*nChansPerScan, 0x7fff);
    int sleepms = ((reader.scansPerPage()/p.srate) * 1e3)/2;
    if (sleepms < 1) sleepms = 1;
    if (sleepms > 200) sleepms = 200;

    Debug() << "Graphing thread '" << g->grapherName() << "' started, sleeptime_ms=" << sleepms << ", priority=" << int(priority());

    while (!pleaseStop) {
        int skips = 0;
        const int16 *scans = reader.next(&skips);
        if (!scans) {
            msleep(sleepms);
        } else {
            if (skips) {
                if (g->caresAboutSkippedScans()) Warning() << "GraphingThread '" << g->grapherName() << "' -- dropped " << (skips*nScansPerPage) << " scans! Graphs too slow for acquisition?";
                // TODO FIXME -- report dropped scans in UI permanently in taskbar or something here..
                int n = skips;
                if (n > 2) n = 2;
                sampCount += u64((skips-n)*droppedPageScans.size());
                for (int i = 0; i < n; ++i) {
                    if (g->threadsafeIsVisible() && g->caresAboutSkippedScans()) g->putScans(droppedPageScans, sampCount);
                    sampCount += u64(droppedPageScans.size());
                }
            }
            if (g->threadsafeIsVisible()) g->putScans(scans, nChansPerScan*nScansPerPage, sampCount);
            sampCount += u64(nChansPerScan*nScansPerPage);
        }
    }

    Debug() << "GraphingThread '" << g->grapherName() << "' ending after processing " << (sampCount/nChansPerScan) << " scans.";
}

MainApp::DataSavingThread::DataSavingThread(MainApp *mainApp)
    : QThread(mainApp), app(mainApp), pleaseStop(false)
{}

MainApp::DataSavingThread::~DataSavingThread()
{
    double t0 = getTime();
    pleaseStop = true;
    if (isRunning()) wait();
    Debug() << "Waited " << ((getTime()-t0)*1e3) << "ms for DataSavingThread to complete.";
}

void MainApp::DataSavingThread::run()
{
    unsigned sleeptime_ms = qRound( (((double(app->reader->scansPerPage()) / app->configCtl->acceptedParams.srate) * 1000.0)) / DEF_TASK_READ_FREQ_HZ);
    if (!sleeptime_ms) sleeptime_ms = 1;
    Debug() << "MainApp::DataSavingThread started, sleeptime_ms=" << sleeptime_ms << ", priority=" << int(priority());

    while (!pleaseStop) {
        if (app->taskReadFunc())
            msleep(sleeptime_ms);
        else
            pleaseStop = true;
    }

    Debug() << "MainApp::DataSavingThread ended after processing " << app->scanCount() << " scans.";
}

///< called from a thread!  BE CAREFUL! ALL CODE HERE SHOULD BE THREADSAFE!
bool MainApp::taskReadFunc()
{
    std::vector<int16> scans_subsetted;
    const int16 *scans = 0;
    bool needToStop = false;
    static double lastSBUpd = 0;
    const DAQ::Params & p (configCtl->acceptedParams);
    u64 firstSamp = scanCt * u64(p.nVAIChans);
    int fakeDataSz = -1, skips = 0;
    void *metaPtr = 0;
    unsigned scans_ret = 0;

    while (!needToStop || taskShouldStop) { ///< if taskShouldStop, stimGl signalled us, and task->stop() has been called.  So we keep emptying the queue to save all pending data.  Hence the reasosn for this while

        trf_stimGL_Process(); ///< polls the stimgl flags to see if stimgl asynch. announced us of a plugin start/end/save event and updates app state accordingly

        bool gotSomething = false;

        scans = reader->next(&skips,&metaPtr,&scans_ret);
        gotSomething = !!scans;

        if (!gotSomething) { break; }
        if (scans_ret != reader->scansPerPage()) {
            Error() << "MainApp::taskReadFunc INTERNAL ERROR: scans_ret != scansPerPage -- FIXME!";
        }
        if (skips>0) fakeDataSz = skips*reader->scansPerPage()*reader->scanSizeSamps();
        else fakeDataSz = -1;
        firstSamp += skips ? u64(fakeDataSz) : 0ULL;
        scanSkipCt += skips ? static_cast<unsigned long>(fakeDataSz/reader->scanSizeSamps()) : 0UL;
        const bool wasFakeData = fakeDataSz > -1;
        // TODO XXX FIXME -- implement detection of fake data (DAQ restart!) properly
        if (wasFakeData) {
            Warning() << "Skipped " << skips << " pages in sample buffer -- buffer overflow -- data may be corrupted or incomplete!";
            putRestarts(p, firstSamp, u64(fakeDataSz/p.nVAIChans));
        }

        const DAQ::BugTask::BlockMetaData *bugMeta = 0;
        int useAltTrigIdx = -1; int16 altTrigThresh = -1;

        tNow = getTime();

        if (bugWindow && bugTask() && reader->metaDataSizeBytes() >= (int)sizeof(DAQ::BugTask::BlockMetaData) && metaPtr) {
            bugMeta = reinterpret_cast<const DAQ::BugTask::BlockMetaData *>(metaPtr);
            if (p.bug.backupTrigger > -1 && bugMeta && bugMeta->missingFrameCount >= DAQ::BugTask::FramesPerBlock) {
                useAltTrigIdx = p.bug.backupTrigger, altTrigThresh = p.bug.backupTriggerThresh;
            }
        } else if (metaPtr && reader->metaDataSizeBytes() && fgTask()) {
            const unsigned int *m = reinterpret_cast<unsigned int *>(metaPtr); // recast metadata to a series of 32 bit ints
            int idx = (reader->metaDataSizeBytes() - sizeof(*m)) / sizeof(*m); // take the last 32bit int from the buffer
            if (idx > -1) fgTask()->updateTimesampLabel(m[idx]);
        }

        lastScanSz = reader->scansPerPage()*reader->scanSizeSamps();
        scanCt = firstSamp/u64(p.nVAIChans) + u64(reader->scansPerPage());
        i32 triggerOffset = -1;
        int prebufCopied = 0;

        if (taskWaitingForTrigger) { // task has been triggered , so save data, and graph it..
            if (!taskHasManualTrigOverride) {
                detectTriggerEvent(scans, lastScanSz, firstSamp, triggerOffset, useAltTrigIdx, altTrigThresh); // may set taskWaitingForTrigger...
                if (taskWaitingForTrigger && tNow-lastSBUpd > 0.25) { // every 1/4th of a second
                    if (p.acqStartEndMode == DAQ::Timed) {
                        Status() << "Acquisition will auto-start in " << (double(startScanCt-scanCt)/p.srate) << " seconds.";
                        //Debug() << "scanCt: " << scanCt << " startScanCt: " << startScanCt << " srate: "  << p.srate << "  tdiff: " << (double(startScanCt-scanCt)/p.srate);
                        lastSBUpd = tNow;
                    } else if (p.acqStartEndMode == DAQ::StimGLStart
                               || p.acqStartEndMode == DAQ::StimGLStartEnd) {
                        Status() << "Acquisition waiting for start trigger from StimGL program";
                    } else if (p.acqStartEndMode == DAQ::PDStart
                               || p.acqStartEndMode == DAQ::PDStartEnd) {
                        Status() << "Acquisition waiting for start trigger from photo-diode";
                    } else if (p.acqStartEndMode == DAQ::Bug3TTLTriggered) {
                        if (p.bug.ttlTrig > -1)
                            Status() << "Acquisition waiting for start trigger from Bug3 TTL line " << p.bug.ttlTrig;
                        else if (p.bug.auxTrig > -1)
                            Status() << "Acquisition waiting for start trigger from Bug3 AUX line " << p.bug.auxTrig;
                    } else if (p.acqStartEndMode == DAQ::AITriggered) {
                        Status() << "Acquisition waiting for start trigger from AI";
                    }
                }
            } else { //taskHasManualTrigOverride
                emit do_setPDTrigLED(true); // just always say it's high.  Note the led should be yellow here, to indicate an override
            }
        }

        // not an 'else' because both 'if' clauses are true on first trigger
        if (!taskWaitingForTrigger || taskHasManualTrigOverride) { // task not waiting from trigger, normal acq.. OR task has manual trigger override so save NOW
            const u64 scanSz = lastScanSz;

            if ((p.acqStartEndMode == DAQ::AITriggered || p.acqStartEndMode == DAQ::Bug3TTLTriggered) && !dataFile.isOpen()) {
                // HACK!
                QString fn = getNewDataFileName();
                if (!dataFile.openForWrite(p, fn)) {
                    Error() << "Could not open data file `" << fn << "'!";
                }
                emit do_updateWindowTitles();
            }

            if (!taskHasManualTrigOverride
                && !needToStop && !taskShouldStop && taskWaitingForStop) {
                needToStop = detectStopTask(scans, lastScanSz, firstSamp, useAltTrigIdx, altTrigThresh);
            }

            if (dataFile.isOpen()) {
                bool doStopRecord = false;
                i64 n = scanSz;
                prebuf_scans.resize(0);

                if (stopRecordAtSamp > -1 && (doStopRecord = (i64(firstSamp + scanSz) >= stopRecordAtSamp))) {
                    // ok, scheduled a stop, make scans be the subset of scans we want to keep for the datafile, and save the full scan to scans_full
                    n = i64(stopRecordAtSamp) - i64(firstSamp);
                    if (n < 0) n = 0;
                    else if (n > i64(scanSz)) n = scanSz;
                }
                if (dataFile.sampleCount() == 0 && taskHasManualTrigOverride
                        && p.acqStartEndMode == DAQ::Bug3TTLTriggered && graphsWindow) {
                    // if we are in Bug3 mode, and they clicked "manual trigger", then we force all the scans from the graphs
                    // windows to be written to the data file.  This feature was requested by Huai-Ti in August 2016
                    // as a workaround for telemetry system not sending frames and never receiving the trigger signal
                    // in SpikeGL
                    unsigned ndispscans = graphsWindow->grabAllScansFromDisplayBuffers(prebuf_scans);
                    Debug() << "Bug3 Mode: Manual trigger workaround: Grabbed " << ndispscans << " scans from display buffers and prepended to file...";
                } else {
                    // otherwise, do the normal prebuf prepend stuff here
                    if (triggerOffset >= 0 && preBuf.size()) {
                        prependPrebufToScans(preBuf, prebuf_scans, prebufCopied, triggerOffset);
                    }
                }
                if (wasFakeData) {
                    // indicate bad data in output file..
                    dataFile.pushBadData(dataFile.scanCount(), fakeDataSz/p.nVAIChans);
                }
                if (dataFile.numChans() != p.nVAIChans) {
                    //double ts = getTime();
                    // need to subset the chans in-place here.  a bit costly performance-wise.. we can optimize this further if need be by doing it on multiple cores at once using QConcurrent or somesuch mechanism
                    const int pbs = (int)prebuf_scans.size();
                    const double ratio = dataFile.numChans() / double(p.nVAIChans ? p.nVAIChans : 1.);
                    save_subset.resize(0);
                    save_subset.reserve(qRound((pbs + n) * ratio) + 256);
                    for (int i = 0; i < int(n+pbs); ++i) {
                        const int rel = i % p.nVAIChans;
                        if (p.demuxedBitMap.testBit(rel)) {
                            if (i < pbs)
                                save_subset.push_back(prebuf_scans[i]);
                            else
                                save_subset.push_back(scans[i-pbs]);
                        }
                    }
                    //Debug() << "subsetting took: " << ((getTime()-ts)*1e3) << " ms";
                    dataFile.writeScans(save_subset);
                    if (bugWindow && bugMeta) {
                        bugWindow->writeMetaToBug3File(dataFile, *bugMeta); // bugMetaFudge explanation: in order to make sure scan numbers in file line up with scan numbers in data file, make sure to writeScans() to the data file *before* calling this!
                    }
                } else {
                    dataFile.writeScans(prebuf_scans);
                    //if (prebuf_scans.size()) Debug() << "prebuf: wrote " << prebuf_scans.size()/p.nVAIChans << " prebuf scans";
                    dataFile.writeScans(scans, n/p.nVAIChans);
                    //if (n != i64(scanSz)) Debug() << "writeScans: n=,scanSz=" << n << "," << scanSz << " difference is " << ((scanSz-n)/p.nVAIChans) << " scans.." << (n%p.nVAIChans ? "NOT ALIGNED" : "ALIGNED") ;
                    if (bugWindow && bugMeta)
                        bugWindow->writeMetaToBug3File(dataFile, *bugMeta); // bugMetaFudge explanation: in order to make sure scan numbers in file line up with scan numbers in data file, make sure to writeScans() to the data file *before* calling this!
                }
                if (doStopRecord) {
                    if (!p.stimGlTrigResave && p.acqStartEndMode != DAQ::AITriggered && p.acqStartEndMode != DAQ::Bug3TTLTriggered)
                        needToStop = true;
                    Debug() << "Post-untrigger window detection: Closing datafile because passed samp# stopRecordAtSamp=" << stopRecordAtSamp;
                    dataFile.closeAndFinalize();
                    stopRecordAtSamp = -1;
                    emit do_updateWindowTitles();
                    if (p.stimGlTrigResave || p.acqStartEndMode == DAQ::AITriggered || p.acqStartEndMode == DAQ::Bug3TTLTriggered) {
                        taskWaitingForTrigger = true;
                        emit do_updateWindowTitles();
                    }
                }
            }

            if (isDSFacilityEnabled())
                tmpDataFile.writeScans(scans, lastScanSz); // write all scans


            if (tNow-lastSBUpd > 0.25) { // every 1/4th of a second
                QString taskEndStr = "";
                if (taskWaitingForStop && p.acqStartEndMode == DAQ::Timed) {
                    taskEndStr = QString(" - task will auto-stop in ") + QString::number((stopScanCt-scanCt)/p.srate) + " secs";
                }

                QString dfScanStr = "";
                if (dataFile.isOpen()) dfScanStr = QString(" - ") + QString::number(dataFile.scanCount()) + " scans saved";

                double bufferFill = (double(reader->latest() - reader->latestPageRead()) / reader->nPages()) * 1e2;
                QString bufStr = QString(" - ") + QString::number(bufferFill,'f',2) + "% buf. lag ";

                QString droppedScanStr = "";
                if (scanSkipCt) {
                    droppedScanStr = QString(" - ") + QString::number(scanSkipCt) + "/" + QString::number(scanCt) + " scans overflowed";
                }

                Status() << task->numChans() << "-channel acquisition running @ " << task->samplingRate()/1000. << " kHz" << bufStr << dfScanStr << droppedScanStr << " - " << dataFile.writeSpeedBytesSec()/1e6 << " MB/s disk speed (" << dataFile.minimalWriteSpeedRequired()/1e6 << " MB/s required)" <<  taskEndStr;
                lastSBUpd = tNow;
            }

        }


        QList<SampleBufQ *> overThresh = SampleBufQ::allQueuesAbove(90.0);
        for (QList<SampleBufQ *>::iterator it = overThresh.begin(); it != overThresh.end(); ++it) {
            SampleBufQ *buf = *it;
            Warning() << "The buffer: `" << (*it)->name << "' is " << double((buf->dataQueueSize()/double(buf->dataQueueMaxSize))*100.) << "% full! System too slow for the specified acquisition?";
        }

        // normally *always* pre-buffer the scans since we may need them at any time on a re-trigger event
        preBuf.putData(&scans[0], unsigned(lastScanSz*sizeof(scans[0])));

        firstSamp += reader->scansPerPage()*reader->scanSizeSamps();
    }

    if (taskShouldStop || needToStop) {
        emit do_stopTask();
        return false;
    }

    return true;
}

void MainApp::gotManualTrigOverride(bool b)
{
    taskHasManualTrigOverride = b;
    if (b) {
        Status() << "PD/TTL Manual Trigger Override ENABLED";
        Log() << "PD/TTL Manual Trigger Override ENABLED, will ignore PD/TTL trigger events and begin saving data immediately.";
        updateWindowTitles();
    } else {
        Status() << "PD/TTL Manual Trigger Override DISABLED";
        Log() << "PD/TTL Manual Trigger Override DISABLED, will close immediate data file and begin monitoring PD/TTL trigger events again.";
        if (dataFile.isOpen()) dataFile.closeAndFinalize();
        updateWindowTitles();
    }
}

bool MainApp::detectTriggerEvent(const int16 * scans, unsigned sz, u64 firstSamp, i32 & triggerOffset,
                                 int trigIndex, int16 trigThresh)
{
    triggerOffset = -1;
    bool triggered = false;
    DAQ::Params & p (configCtl->acceptedParams);
    if (trigIndex < 0 || trigThresh < 0) trigIndex = p.idxOfPdChan, trigThresh = p.pdThresh;
    switch (p.acqStartEndMode) {
    case DAQ::Timed:  
        if (p.isImmediate || scanCt >= startScanCt) {
            triggered = true;
            Debug() << "Triggered start.  startScanCt=" << startScanCt << " scanCt=" << scanCt;
        }
        break;
    case DAQ::PDStartEnd:
    case DAQ::PDStart:
	case DAQ::Bug3TTLTriggered:
	case DAQ::AITriggered: {
        // NB: photodiode channel is always the last channel.. unless in bug mode then it can be any of the last few TTL lines
        if (trigIndex < 0) {
            Error() << "INTERNAL ERROR, acqStartMode is PD based but no PD channel specified in DAQ params!";
            emit do_stopTask();
            return false;
        }
        //Debug() << "detectTrig: idx=" << trigIndex << " thresh=" << trigThresh;
        for (int i = trigIndex; i < int(sz); i += p.nVAIChans) {
            const int16 samp = scans[i];
            if (!pdWaitingForStimGL && samp > trigThresh) {
                if (lastNPDSamples.size() >= p.pdThreshW) {
                    triggered = true, lastNPDSamples.clear();
                    pdOffTimeSamps = p.srate * p.pdStopTime * p.nVAIChans;
					while (pdOffTimeSamps%p.nVAIChans) ++pdOffTimeSamps;
                    lastSeenPD = firstSamp + u64(i-trigIndex);
                    // we triggered, so save offset of where we triggered
                    triggerOffset = static_cast<i32>(i-int(trigIndex));
                    i = sz; // break out of loop
                } else 
                    lastNPDSamples.push_back(samp);
            } else
                lastNPDSamples.clear();
        }
    }
        break;
    case DAQ::StimGLStart:
    case DAQ::StimGLStartEnd:
        // do nothing.. this gets set from stimGL_PluginStarted() slot outside this function..
        break;
    default: {        
        QString err =  "Unanticipated/illegal p.acqStartEndMode in detectTriggerEvent: " + QString::number((int)p.acqStartEndMode);
        Error() << err;
        int but = QMessageBox::critical(0, "Trigger Event Error", err, QMessageBox::Abort, QMessageBox::Ignore);
        if (but == QMessageBox::Abort) quit();
    }
    }
    if (triggered) {
        emit do_setManualTrigEnabled(false);
        triggerTask();
    }
    emit do_setPDTrigLED(triggered);
    
    return triggered;
}

bool MainApp::detectStopTask(const int16 * scans, unsigned sz, u64 firstSamp, int trigIndex, int16 trigThresh)
{
    bool stopped = false;
    DAQ::Params & p (configCtl->acceptedParams);
    const bool isBugAlt = p.acqStartEndMode == DAQ::Bug3TTLTriggered && p.bug.altTTL;

    if (trigIndex < 0 || trigThresh < 0) trigIndex = p.idxOfPdChan, trigThresh = p.pdThresh;

    switch (p.acqStartEndMode) {
    case DAQ::Timed:  
        if (!p.isIndefinite && scanCt >= stopScanCt) {
            stopped = true;
            Log() << "Triggered stop because acquisition duration has fully elapsed.";
        }
        break;
	case DAQ::Bug3TTLTriggered:
	case DAQ::AITriggered: 
    case DAQ::PDStartEnd: {

        if (trigIndex < 0) {
            Error() << "INTERNAL ERROR, acqEndMode is PD/AI based but no PD/AI channel specified in DAQ params!";
            return true;
        }

        //Debug() << "detectStop: idx=" << trigIndex << " thresh=" << trigThresh;

        if (!isBugAlt) {
            for (int i = trigIndex; i < int(sz); i += p.nVAIChans) {
                const int16 samp = scans[i];
                if (samp > trigThresh) {
                    if (lastNPDSamples.size() >= p.pdThreshW)
                        lastSeenPD = firstSamp+u64(i-trigIndex)/*, lastNPDSamples.clear()*/;
                    else
                        lastNPDSamples.push_back(samp);
                } else
                    lastNPDSamples.clear();
            }
        }
        if (firstSamp+u64(sz) - lastSeenPD > pdOffTimeSamps) { // timeout PD after X scans..
			if (dataFile.isOpen()) {
                stopRecordAtSamp = lastSeenPD + MAX((preBuf.capacity()/sizeof(int16)),pdOffTimeSamps) /**< NB: preBuf.capacity() is the amount of silence time before/after PD, scan-aligned! */;
                if (isBugAlt) stopRecordAtSamp = lastSeenPD + pdOffTimeSamps;
				taskWaitingForStop = false;
			} else {
				Warning() << "PD/AI un-trig but datafile not open!  This is not really well-defined, but stopping task anyway.";
				stopped = true;
			}
            if (graphsWindow) {
                emit do_setPDTrigLED(false);
                if (p.acqStartEndMode == DAQ::Bug3TTLTriggered)
                    emit do_setManualTrigEnabled(true);
            }
            if (!isBugAlt)
                Log() << "PD/AI un-trig due to input line being off for >" << p.pdStopTime << " seconds.";
            else
                Log() << "Bug3 TTL append window reached at " << p.pdStopTime << " seconds; untriggering.";
        }
    }
        break;
    case DAQ::StimGLStartEnd:
        // do nothing.. this gets set from stimGL_PluginEnded() slot outside this function..
        break;
    default: {        
        QString err =  "Unanticipated/illegal p.acqStartEndMode in detectStopTask: " + QString::number((int)p.acqStartEndMode);
        Error() << err;
        int but = QMessageBox::critical(0, "Trigger Event Error", err, QMessageBox::Abort, QMessageBox::Ignore);
        if (but == QMessageBox::Abort) quit();
    }
    }
    return stopped;
}

void MainApp::triggerTask()
{
    if (task && taskWaitingForTrigger) {
        taskWaitingForTrigger = false;
        emit do_updateWindowTitles();
        //Systray() << "Acquisition triggered ON";
        Status() << "Task triggered";
        DAQ::Params & p(configCtl->acceptedParams);
        switch (p.acqStartEndMode) {
        case DAQ::PDStartEnd:             
        case DAQ::StimGLStartEnd:
		case DAQ::AITriggered: 
		case DAQ::Bug3TTLTriggered:
            taskWaitingForStop = true; 
            Log() << "Acquisition triggered";
            break;
        case DAQ::Timed:
            if (!p.isIndefinite) 
                taskWaitingForStop = true;
            Log() << "Acquisition started due to auto-timed trigger";
            break;
        default: 
            taskWaitingForStop = false;
            break; 
        }        
    }
}

void MainApp::updateWindowTitles()
{
    const bool isOpen = dataFile.isOpen();
    QString stat = "";
    QString fname = isOpen ? dataFile.fileName() : "(no outfile)";
    if (task) {
        if (taskHasManualTrigOverride) {
            stat = "MANUAL TRIG OVERRIDE - " + fname;
        } else if (taskWaitingForTrigger)
            stat = "WAITING - " + fname;
        else
            stat = "RUNNING - " + fname;
    } else {
        if (initializing) stat = "INITIALIZING";
        else stat = "No Acquisition Running";
    }
    QString tit = QString(APPNAME) + " Console - " + stat;
    consoleWindow->setWindowTitle(tit);
	if (windowActions.contains(consoleWindow)) 
		windowActions[consoleWindow]->setText(tit);
	
    sysTray->contextMenu()->setTitle(tit);
    if (graphsWindow) {
        tit = QString(APPNAME) + " Graphs - " + stat;
        graphsWindow->setWindowTitle(tit);
		if (windowActions.contains(graphsWindow))
			windowActions[graphsWindow]->setText(tit);
        graphsWindow->setToggleSaveChkBox(isOpen);
        if (isOpen) graphsWindow->setToggleSaveLE(fname);
    }
}

static QString getLastSha1FileName(const QString & def)
{
	QSettings s(SETTINGS_DOMAIN, SETTINGS_APP);
	s.beginGroup("Sha1Verify");
	return s.value("FileName", def).toString();
}
static void saveLastSha1FileName(const QString & f)
{
	if (f.isNull() || f.isEmpty()) return;
	QSettings s(SETTINGS_DOMAIN, SETTINGS_APP);
	s.beginGroup("Sha1Verify");
	s.setValue("FileName", f);
}

void MainApp::verifySha1()
{
    noHotKeys = true;
    QString dataFile = QFileDialog::getOpenFileName ( consoleWindow, "Select data file for SHA1 verification", getLastSha1FileName(outputDirectory()));
    noHotKeys = false;
    if (dataFile.isNull() || dataFile.isEmpty()) return;
    QFileInfo pickedFI(dataFile);
	saveLastSha1FileName(dataFile);
    Params p;


    if (pickedFI.suffix() != "meta") {
        pickedFI.setFile(pickedFI.path() + "/" + pickedFI.completeBaseName() + ".meta");
    } else {
        dataFile = ""; 
    }
    if (!pickedFI.exists()) {
        QMessageBox::critical(consoleWindow, ".meta file does not exist!", "SHA1 verification requires the meta-file for the data file to exist.\n`" + pickedFI.fileName() + "' does not exist or is not readable!");
        return;
    }
    if (!p.fromFile(pickedFI.filePath())) {
        QMessageBox::critical(consoleWindow, pickedFI.fileName() + " could not be read!", "SHA1 verification requires the meta-file for the data file to exist.\n`" + pickedFI.fileName() + "' could not be read!");
        return;
    }
    if (!dataFile.length()) dataFile = p["outputFile"].toString();
    if (!QFileInfo(dataFile).isAbsolute())
        dataFile = outputDirectory() + "/" + dataFile; 
    if (this->task && this->dataFile.isOpen() && QFileInfo(dataFile) == QFileInfo(this->dataFile.fileName())) {
        QMessageBox::critical(consoleWindow, "Acquisition is running on file", "Cannot verify SHA1 hash on this file as it is currently open and being used as the datafile for the currently-running acquisition!");
        return;
    }

    // now, spawn a new thread for the task..
    Sha1VerifyTask *task = new Sha1VerifyTask(dataFile, pickedFI.filePath(), p, this);
    Connect(task, SIGNAL(success()), this, SLOT(sha1VerifySuccess()));
    Connect(task, SIGNAL(failure()), this, SLOT(sha1VerifyFailure()));
    Connect(task, SIGNAL(canceled()), this, SLOT(sha1VerifyCancel()));
    Connect(task, SIGNAL(metaFileMissingSha1(QString)), this, SLOT(sha1VerifyMissing(QString)));
    task->prog->show();
    task->start();
}

void MainApp::sha1VerifySuccess()
{
    Sha1VerifyTask *task = dynamic_cast<Sha1VerifyTask *>(sender());
    QString fn;
    if (task) fn = task->dataFileNameShort;    
    QString str = fn + " SHA1 sum verified ok!";
    Log() << str;
    noHotKeys = true;
    QMessageBox::information(consoleWindow, fn + " SHA1 Verify", str);
    noHotKeys = false;
    if (task) delete task;
    else Error() << "sha1VerifySuccess error, no task!";
}

void MainApp::sha1VerifyFailure()
{
    Sha1VerifyTask *task = dynamic_cast<Sha1VerifyTask *>(sender());
    QString err;
    if (task) err = task->extendedError;
    QString fn;
    if (task) fn = task->dataFileNameShort;    
    QString str = fn + " verify error:\n" + err;
    Warning() << str;
    noHotKeys = true;
    QMessageBox::warning(consoleWindow, fn + " SHA1 Verify", str);
    noHotKeys = false;
    if (task) delete task;
    else Error() << "sha1VerifyFailure error, no task!";
}

void MainApp::sha1VerifyCancel()
{
    Sha1VerifyTask *task = dynamic_cast<Sha1VerifyTask *>(sender());
    QString fn;
    if (task) fn = task->dataFileNameShort;    
    QString str = fn + " SHA1 verify canceled.";
    Log() << str;
    if (task) task->deleteLater();
    else Error() << "sha1VerifyCancel error, no task!";
}

void MainApp::sha1VerifyMissing(QString computedHash)
{
    Sha1VerifyTask *task = dynamic_cast<Sha1VerifyTask *>(sender());
    QString fn, mfp;
    if (!task) {
        Error() << "sha1VerifyMissing error, no task!";
        return;
    }
    fn = task->dataFileNameShort, mfp = task->metaFilePath;
    QString str = QString("'") + fn + "' is missing a SHA1 hash because it was never computer for this file.\n\nSave the correct SHA1 hash to the .meta file now?";
    int but = QMessageBox::question(consoleWindow, "Missing Hash", str, QMessageBox::Yes|QMessageBox::No, QMessageBox::Yes);
    if (but == QMessageBox::Yes) {
        //...
        Params p;
        p.fromFile(mfp);
        p["sha1"] = computedHash;
        if (!p.toFile(mfp)) {
            Error() << "Error writing to " << mfp;
        } else {
            QString msg = QString("'") + fn + "'" + " SHA1 hash saved successfully!";
            Log() << msg;
            QMessageBox::information(consoleWindow, "Success!", msg);
        }
    }
    task->deleteLater();
}



void MainApp::showPar2Win()
{
    par2Win->show();
	windowMenuAdd(par2Win);
}

void MainApp::par2WinClosed()
{
	windowMenuRemove(par2Win);
}

void MainApp::execStimGLIntegrationDialog()
{
    bool again = false;
    QDialog dlg(0);
    dlg.setWindowIcon(consoleWindow->windowIcon());
    dlg.setWindowTitle("StimGL Integration Options");    
    dlg.setModal(true);
    StimGLIntegrationParams & p (stimGLIntParams);
        
    Ui::StimGLIntegration controls;
    controls.setupUi(&dlg);
    controls.interfaceLE->setText(p.iface);
    controls.portSB->setValue(p.port);
    controls.timeoutSB->setValue(p.timeout_ms);    
    do {     
        again = false;
        noHotKeys = true;
        const int ret = dlg.exec();
        noHotKeys = false;
        if ( ret == QDialog::Accepted ) {
            p.iface = controls.interfaceLE->text();
            p.port = controls.portSB->value();
            p.timeout_ms = controls.timeoutSB->value();
            if (!setupStimGLIntegration(false)) {
                QMessageBox::critical(0, "Listen Error", "Notification server could not listen on " + p.iface + ":" + QString::number(p.port) + "\nTry the options again again!");
                loadSettings();
                again = true;
                continue;
            }
        } else {
            loadSettings();
            if (!notifyServer) setupStimGLIntegration();
            return;
        }
    } while (again);

    saveSettings();
}

void MainApp::execCommandServerOptionsDialog()
{
    bool again = false;
    QDialog dlg(0);
    dlg.setWindowIcon(consoleWindow->windowIcon());
    dlg.setWindowTitle("Command Server Options");    
    dlg.setModal(true);
    CommandServerParams & p (commandServerParams);
    
    Ui::CommandServerOptions controls;
    controls.setupUi(&dlg);
    controls.interfaceLE->setText(p.iface);
    controls.portSB->setValue(p.port);
    controls.timeoutSB->setValue(p.timeout_ms);    
    controls.enabledGB->setChecked(p.enabled);
    
    do {     
        again = false;
        noHotKeys = true;
        const int ret = dlg.exec();
        noHotKeys = false;
        if ( ret == QDialog::Accepted ) {
            p.iface = controls.interfaceLE->text();
            p.port = controls.portSB->value();
            p.timeout_ms = controls.timeoutSB->value();
			p.enabled = controls.enabledGB->isChecked();
            if (!setupCommandServer(false)) {
                QMessageBox::critical(0, "Listen Error", "Command server could not listen on " + p.iface + ":" + QString::number(p.port) + "\nTry the options again again!");
                loadSettings();
                again = true;
                continue;
            }
        } else {
            loadSettings();
            if (!commandServer) setupCommandServer();
            return;
        }
    } while (again);
    
    saveSettings();
}

void MainApp::execDSTempFileDialog()
{
	bool again = false;
	QDialog dlg(0);
    dlg.setWindowIcon(consoleWindow->windowIcon());
    dlg.setWindowTitle("DataStream Temporary File Size");    
    dlg.setModal(true);
	dlg.setFixedSize(332, 171);

	Ui::TempFileDialog tmpFileDlg;
	tmpFileDlg.setupUi(&dlg);
	tmpFileDlg.fileSizeSB->setValue(tmpDataFile.getTempFileSize() / 1048576);
    tmpFileDlg.avDiskSpaceL->setText(   tmpFileDlg.avDiskSpaceL->text() + 
                                        QString::number(Util::availableDiskSpace() / 1048576) +
                                        " MB");

	do 
    {
        again = false;
        const int ret = dlg.exec();
        if ( ret == QDialog::Accepted ) 
        {
            unsigned spinVal = tmpFileDlg.fileSizeSB->value();

            if (spinVal >= Util::availableDiskSpace() / 1048576) 
            {
                QMessageBox::information(0, "Disk Space Exceeded", "The file size exceeds the available disk space!");
                loadSettings();
                again = true;
                continue;
            } else {
				tmpDataFile.close(); // will force a re-open if it was running
                tmpDataFile.setTempFileSize((qint64)spinVal * 1048576);
			}
        }
        else 
        {
            loadSettings();
            return;
        }
    } 
    while (again);

	saveSettings();
}

void MainApp::execBufferSizesDialog()
{
    QDialog bufferSizesDialog(consoleWindow, Qt::Dialog);
    Ui::SampleBuf_Dialog w;
    w.setupUi(&bufferSizesDialog);
    const unsigned long mem = static_cast<unsigned long>(getTotalPhysicalMemory() / (1024ULL*1024ULL));
    loadSettings();
    unsigned long max = mem;
    if (sizeof(void *) <= 4 && max > 2047) max = 2047;
    BufSizesParams & p(bufSizesParams);
    if (mem > 64) {
        w.fgShmSB->setMinimum(1); w.fgShmSB->setMaximum(max); w.fgShmSlider->setMinimum(1); w.fgShmSlider->setMaximum(max); w.fgShmSlider->setTickInterval(max/100 > 4 ? max/100 : 1);
        w.regularSB->setMinimum(1); w.regularSB->setMaximum(max); w.regularSlider->setMinimum(1); w.regularSlider->setMaximum(max); w.regularSlider->setTickInterval(max/100 > 4 ? max/100 : 1);
    }
    w.memLbl->setText(QString::number(mem) + " MB");
    w.fgShmSB->setValue(p.fgShmMB > max ? max : p.fgShmMB);
    w.fgShmSlider->setValue(p.fgShmMB > max ? max : p.fgShmMB);
    w.regularSB->setValue(p.regularMB > max ? max : p.regularMB);
    w.regularSlider->setValue(p.regularMB > max ? max : p.regularMB);
    int ret = -999;
    bool again = false;
    while (ret == -999 || again) {
        again = false;
        ret = bufferSizesDialog.exec();

        if (ret == QDialog::Accepted) {
            unsigned long fval = (unsigned long)w.fgShmSB->value(), rval = (unsigned long)w.regularSB->value();
            if (rval >= mem || fval >= mem) {
                QMessageBox::critical(consoleWindow, "Buffers Too Large", "Specified buffer sizes exceed physical memory! Go back and try again!",QMessageBox::Ok);
                again = true;
                continue;
            }
            if (rval >= mem/2 || fval >= mem/2) {
                int r = QMessageBox::warning(consoleWindow, "Are You Sure?", "One of the specified buffers would exceed 1/2 of physical memory!\n\nContinue anyway?", QMessageBox::Yes, QMessageBox::Cancel);
                if (r == QMessageBox::Cancel) {
                    again = true;
                    continue;
                }
                again = false;
            }
            if (rval > 2047 || fval > 2047) {
                QMessageBox::warning(consoleWindow,"Unsupported Size", "At present, only buffers of up to 2047 MB are supported due to limitations in Qt's QSharedMemory class.\n\nPlease try again.", QMessageBox::Ok);
                again = true;
                continue;
            }
            if (!fval || !rval) {
                QMessageBox::warning(consoleWindow,"Invalid Size", "Buffer size cannot be 0 MB!\n\nPlease try again.", QMessageBox::Ok);
                again = true;
                continue;
            }
            if (fval < 64) {
                QMessageBox::warning(consoleWindow,"Invalid Size", "Framegrabber buffer needs to be at least 64MB (and even that's tiny) to prevent dropped frames!\n\nPlease try again.", QMessageBox::Ok);
                again = true;
                continue;
            }

            /// else.. all good
            p.fgShmMB = fval;
            p.regularMB = rval;
            saveSettings();
            Log() << "User configued realtime sample buffer sizes as: NI/Bug=" << rval << " MB, FGShm=" << fval << " MB.";
            return;
        }
    }
}

bool MainApp::setupStimGLIntegration(bool doQuitOnFail)
{
    if (notifyServer) delete notifyServer;
    notifyServer = new StimGL_SpikeGL_Integration::NotifyServer(this);
    StimGLIntegrationParams & p (stimGLIntParams);
    if (!notifyServer->beginListening(p.iface, p.port, p.timeout_ms)) {
        if (doQuitOnFail) {
            int but = QMessageBox::critical(0, "Listen Error", "Notification server could not listen on port " + QString::number(p.port) + "\nAnother copy of this program might already be running.\nContinue anyway?", QMessageBox::Abort, QMessageBox::Ignore);
            if (but == QMessageBox::Abort) postEvent(this, new QEvent((QEvent::Type)QuitEventType)); // quit doesn't work here because we are in appliation c'tor
        }
        Error() << "Failed to start StimGLII integration notification server!";
        delete notifyServer, notifyServer = 0;
        return false;
    }
    Connect(notifyServer, SIGNAL(gotPluginStartNotification(const QString &, const QMap<QString, QVariant>  &)), this, SLOT(stimGL_PluginStarted(const QString &, const QMap<QString, QVariant>  &)));
    Connect(notifyServer, SIGNAL(gotPluginParamsNotification(const QString &, const QMap<QString, QVariant>  &)), this, SLOT(stimGL_SaveParams(const QString &, const QMap<QString, QVariant>  &)));
    Connect(notifyServer, SIGNAL(gotPluginEndNotification(const QString &, const QMap<QString, QVariant>  &)), this, SLOT(stimGL_PluginEnded(const QString &, const QMap<QString, QVariant>  &)));
    return true;
}

bool MainApp::setupCommandServer(bool doQuitOnFail) 
{
    if (commandServer) delete commandServer, commandServer = 0;
    CommandServerParams & p (commandServerParams);
    if (p.enabled) {
        commandServer = new CommandServer(this);
        if (!commandServer->beginListening(p.iface, p.port, p.timeout_ms)) {
            if (doQuitOnFail) {
                int but = QMessageBox::critical(0, "Listen Error", "Command server could not listen on port " + QString::number(p.port) + "\nAnother copy of this program might already be running.\nContinue anyway?", QMessageBox::Abort, QMessageBox::Ignore);
                if (but == QMessageBox::Abort) postEvent(this, new QEvent((QEvent::Type)QuitEventType)); // quit doesn't work here because we are in appliation c'tor
            }
            Error() << "Failed to start Command server!";
            delete commandServer, commandServer = 0;
            return false;
        }
    }
    return true;
}

QString MainApp::getNewDataFileName(const QString & suf) const
{
    const DAQ::Params & p (configCtl->acceptedParams);
	QString suffix = p.stimGlTrigResave ? suf : QString::null;
	QFileInfo fi(p.outputFileOrig);
	if (!fi.isAbsolute()) {
		fi.setFile(outputDirectory() + "/" + p.outputFileOrig);
	}
	QString prefix = fi.filePath();
	QString ext = fi.completeSuffix();
	prefix.chop(ext.length()+1);
	
	for (int i = p.cludgyFilenameCounterOverride; i > 0; ++i) {		
		QString fn = prefix + "_" + ( !suffix.isNull() ? suffix + "_" + QString::number(i) : QString::number(i)) + "." + ext;
		if (!QFile::exists(fn)) return fn;
	}
	return p.outputFileOrig;  // should never be reached unless we have 2^31 file collisions!
}

// called from data save thread
void MainApp::trf_stimGL_Process()
{
    bool got_sta, got_sav, got_end;
    SGL_Parms p_sta, p_sav, p_end;

    mut.lock();
    /// cache params locally
    if ((got_sta = got_sgl_started)) p_sta = sgl_started;
    if ((got_sav = got_sgl_save))    p_sav = sgl_save;
    if ((got_end = got_sgl_ended))   p_end = sgl_ended;
    sgl_started = sgl_ended = sgl_save = SGL_Parms(); // unconditionally clear params
    got_sgl_ended = got_sgl_save = got_sgl_started = false; // unconditionally clear flags
    mut.unlock();
    // reacy to any flags that were set
    if (got_sta) trf_stimGL_PluginStarted(p_sta.plugin, p_sta.parms);
    if (got_sav) trf_stimGL_SaveParams(p_sav.plugin, p_sav.parms);
    if (got_end) trf_stimGL_PluginEnded(p_end.plugin, p_end.parms);
}
// called from main thread.. set flags and queue params for real implementation in DataSave thread
void MainApp::stimGL_PluginStarted(const QString &plugin, const QMap<QString, QVariant>  &pm)
{
    mut.lock();
    got_sgl_started = true;
    sgl_started.plugin = plugin;
    sgl_started.parms = pm;
    mut.unlock();
}
// called from main thread.. set flags and queue params for real implementation in DataSave thread
void MainApp::stimGL_PluginEnded(const QString &plugin, const QMap<QString, QVariant>  &pm)
{
    mut.lock();
    got_sgl_ended = true;
    sgl_ended.plugin = plugin;
    sgl_ended.parms = pm;
    mut.unlock();
}
// called from main thread.. set flags and queue params for real implementation in DataSave thread
void MainApp::stimGL_SaveParams(const QString &plugin, const QMap<QString, QVariant>  &pm)
{
    mut.lock();
    got_sgl_save = true;
    sgl_save.plugin = plugin;
    sgl_save.parms = pm;
    mut.unlock();
}

void MainApp::trf_stimGL_PluginStarted(const QString &plugin, const QMap<QString, QVariant>  &pm)
{
    (void)pm;
    bool ignored = true;
    DAQ::Params & p (configCtl->acceptedParams);
    if (task && p.stimGlTrigResave) {
        if (dataFile.isOpen()) {
			if (stopRecordAtSamp > -1) {
				Error() << "Got 'plugin started' message from StimGL, but we were in the post-untrigger window state!  Make sure that the time between loops of StimGL is >= " << p.pdStopTime+p.silenceBeforePD << "s! Forcibly closing the datafile...";
				stopRecordAtSamp = -1;
			}
			Log() << "Data file: " << dataFile.fileName() << " closed by StimulateOpenGL.";
			dataFile.closeAndFinalize();			
        }
        QString fn = getNewDataFileName(plugin);
        if (!dataFile.openForWrite(p, fn)) {
            QMessageBox::critical(0, "Error Opening File!", QString("Could not open data file `%1'!").arg(fn));
        } else {
            Log() << "Data file: " << dataFile.fileName() << " opened by StimulateOpenGL.";
        }

		if (p.acqStartEndMode == DAQ::PDStart || p.acqStartEndMode == DAQ::PDStartEnd) {
           taskWaitingForTrigger = true; // turns on the detect trigger code again, so we can get a constant-offset PD signal
           pdWaitingForStimGL = false;
       }           
        emit do_updateWindowTitles();
        ignored = false;
    }
    if (task 
        && taskWaitingForTrigger 
        && (p.acqStartEndMode == DAQ::StimGLStart || p.acqStartEndMode == DAQ::StimGLStartEnd)) {
        Log() << "Triggered start by Stim GL plugin `" << plugin << "'";
        triggerTask();
        ignored = false;
    }
    if (task 
        && taskWaitingForTrigger
        && (p.acqStartEndMode == DAQ::PDStart || p.acqStartEndMode == DAQ::PDStartEnd)) {
        // just save params in this mode..
        ignored = false;
    }

    Debug() << "Received notification that Stim GL plugin `" << plugin << "' started." << (ignored ? " Ignored!" : "");

	if (!ignored) {
        trf_stimGL_SaveParams(plugin, pm);
        emit do_setSGLTrig(true);
	}
}

void MainApp::trf_stimGL_SaveParams(const QString & plugin, const QMap<QString, QVariant> & pm)
{
    if (dataFile.isOpen()) {
		dataFile.setParam("StimGL_PluginName", plugin);  
        for (QMap<QString, QVariant>::const_iterator it = pm.begin(); it != pm.end(); ++it)
            dataFile.setParam(QString("StimGL_") + it.key(), it.value());  
        queuedParams.clear();
    } else
        queuedParams = pm;
}

void MainApp::trf_stimGL_PluginEnded(const QString &plugin, const QMap<QString, QVariant> & pm)
{
    (void)pm;
    bool ignored = true;
    DAQ::Params & p (configCtl->acceptedParams);
    if (task && taskWaitingForStop && p.acqStartEndMode == DAQ::StimGLStartEnd) {
        Log() << "Triggered stop by Stim GL plugin `" << plugin << "'";
        taskShouldStop = true;
        trf_stimGL_SaveParams(plugin,pm);
        task->stop(); ///< stop the daq task now.. we will empty the queue later..
        Log() << "DAQ task no longer acquiring, emptying queue and saving to disk.";
        ignored = false;        
    } else if (task && p.stimGlTrigResave && dataFile.isOpen()) {
        trf_stimGL_SaveParams(plugin,pm);
		if (p.acqStartEndMode != DAQ::PDStartEnd) {
	        Log() << "Data file: " << dataFile.fileName() << " closed by StimulateOpenGL.";
		    dataFile.closeAndFinalize();
            emit do_updateWindowTitles();
		} else if (!taskWaitingForTrigger) {
			taskWaitingForStop = true;
		}
		if (taskWaitingForTrigger && (p.acqStartEndMode == DAQ::PDStart || p.acqStartEndMode == DAQ::PDStartEnd)) {
			Warning() << "PD signal never triggered a data save!  Is the PD threshold too low?";
		}
        ignored = false;
    }
    if (task && p.stimGlTrigResave && p.acqStartEndMode == DAQ::PDStart) {
        taskWaitingForTrigger = true;
        pdWaitingForStimGL = true;
        emit do_updateWindowTitles();
        ignored = false;        
    }
    emit do_setSGLTrig(false);
	Debug() << "Received notification that Stim GL plugin `" << plugin << "' ended." << (ignored ? " Ignored!" : "");
}

void MainApp::doFastSettle()
{
    if (fastSettleRunning || !task) return;
    fastSettleRunning = true;
	DAQ::NITask *nitask = dynamic_cast<DAQ::NITask *>(task);
	if (nitask) {
		Connect(nitask, SIGNAL(fastSettleCompleted()), this, SLOT(fastSettleCompletion()));
		nitask->requestFastSettle();
	}
}

void MainApp::fastSettleCompletion()
{
    if (!fastSettleRunning) return;
    fastSettleRunning = false;
	DAQ::NITask *nitask = dynamic_cast<DAQ::NITask *>(task);
	if (nitask) disconnect(nitask, SIGNAL(fastSettleCompleted()), this, SLOT(fastSettleCompletion()));
}

void MainApp::gotFirstScan()
{
    if (taskWaitingForTrigger) {
        //Systray() << "Acquisition waiting ...";
        Status() << "Task initiated, waiting for trigger event";
        Log() << "Acquisition initiated, waiting for trigger event";
    } else {
        //Systray() << "Acquisition started";
        Status() << "Task started";
        Log() << "Acquisition started.";
    }
	delete acqStartingDialog, acqStartingDialog = 0;
}

bool MainApp::isSaving() const {
    return dataFile.isOpen();
}

void MainApp::toggleSave(bool s)
{
    mut.lock();
    const DAQ::Params & p (configCtl->acceptedParams);
    if (s && !dataFile.isOpen()) {
        if (!dataFile.openForWrite(p)) {
            mut.unlock();
			if (!p.demuxedBitMap.count(true))
				// Aha!  Error was due to trying ot save a datafile with 0 chans!
				QMessageBox::critical(0, "Nothing to Save!", "Save channel subset is empty (cannot save an empty data file).");
			else
				QMessageBox::critical(0, "Error Opening File!", QString("Could not open data file `%1'!").arg(p.outputFile));            
            return;
        }
        if (!queuedParams.isEmpty()) stimGL_SaveParams("", queuedParams);
        Log() << "Save file: " << dataFile.fileName() << " opened from GUI.";
        //graphsWindow->clearGraph(-1);
        emit do_updateWindowTitles();
    } else if (!s && dataFile.isOpen()) {
        dataFile.closeAndFinalize();
        Log() << "Save file: " << dataFile.fileName() << " closed from GUI.";
        emit do_updateWindowTitles();
    }
    mut.unlock();
}

QString MainApp::outputFile() const { return configCtl->acceptedParams.outputFile; }

void MainApp::setOutputFile(const QString & fn) {
    configCtl->acceptedParams.outputFile = fn;
    configCtl->saveSettings();
    if (graphsWindow) graphsWindow->setToggleSaveLE(fn);
}

QString MainApp::getCurrentSaveFile() const 
{
	QMutexLocker ml(&mut);
	if (!isSaving()) return QString::null;
	QString ret = dataFile.fileName();
	if (!ret.length()) return QString::null;
	return ret;
}

void MainApp::respecAOPassthru()
{
    if (task) {
        configCtl->showAOPassThruDlg();
    }
}

void MainApp::help()
{
    if (!helpWindow) {
        Ui::TextBrowser tb;
        helpWindow = new HelpWindow(0);
        helpWindow->setWindowTitle("SpikeGL Help");
        tb.setupUi(helpWindow);
        tb.textBrowser->setSearchPaths(QStringList("qrc:/"));
        tb.textBrowser->setSource(QUrl("qrc:/SpikeGL-help-manual.html"));
		Connect(helpWindow, SIGNAL(closed()), this, SLOT(helpWindowClosed()));
    }
	helpWindow->show();
	windowMenuAdd(helpWindow);
	windowMenuActivate(helpWindow);
    helpWindow->setMaximumSize(helpWindow->size());
}

void HelpWindow::closeEvent(QCloseEvent *e) {
	QDialog::closeEvent(e);
	if (e->isAccepted()) emit closed();
}	

void MainApp::helpWindowClosed()
{
	windowMenuRemove(helpWindow);
}

void MainApp::precreateOneGraph(bool nograph)
{
    const double t0 = getTime();
    // keep creating GLContexts until the creation count hits 128
    if (!pregraphDummyParent) pregraphDummyParent = new QWidget(0);
    QFrame *f = new QFrame(pregraphDummyParent);
    QVBoxLayout *bl = new QVBoxLayout(f);
	
	QCheckBox *chk = new QCheckBox("Save enabled", f);
	chk->setToolTip("Enable/disable save of this channel's data to data file.  Note: can only edit this property when not saving.");
	bl->addWidget(chk);

	QWidget *dummy = nograph ? 0 : new QWidget(f);
	/* Note: the 'dummy' intermediate parent widget above is a workaround for 
	   Windows which is SLOW when reparenting QGLWidgets.  This is because under 
	   Windows, if reparenting a QGLWidget directly, a new GL context must be 
	   created each time.  Instead, reparenting the parent of the QGLWidget is a 
	   workaround to this.
	 
	   Since when we switch tabs we are reparenting the graphs to new graph 
	   frames, we need this intermediate QWidget.  We reparent this, rather than 
	   the GLGraph directly.  See GraphsWindow.cpp tabChanged() function. */
	GLGraph *g = nograph ? 0 : new GLGraph(dummy);
	QVBoxLayout *bl2 = dummy ? new QVBoxLayout(dummy) : 0;
	if (bl2) {
	    bl2->setSpacing(0);
		bl2->setContentsMargins(0,0,0,0);
		bl2->addWidget(g);
	}

	if (dummy) bl->addWidget(dummy,1);
    bl->setSpacing(0);
    bl->setContentsMargins(0,0,0,0);

    tPerGraph = tPerGraph * pregraphs.count();
    tPerGraph += (getTime()-t0);
    pregraphs.push_back(f);
    tPerGraph /= pregraphs.count();
}

void MainApp::showPrecreateDialog()
{
    if (!precreateDialog) {
        precreateDialog = new QMessageBox(QMessageBox::Information, 
                                          "Precreating GL Graphs",
                                          QString("Precreating ") + QString::number(maxPreGraphs) + " graphs, please wait...",
                                          QMessageBox::NoButton,
                                          consoleWindow);
        precreateDialog->addButton("Quit", QMessageBox::RejectRole);
        Connect(precreateDialog, SIGNAL(rejected()), this, SLOT(quit()));
        Connect(precreateDialog, SIGNAL(accepted()), this, SLOT(quit()));
        precreateDialog->show();
    }	
}

void MainApp::precreateGraphs()
{
    if (int(pregraphs.count()) < maxPreGraphs) {
        precreateOneGraph();
        if (pregraphs.count() == maxPreGraphs) {
 			precreateDone();
        } 
    }
}

void MainApp::precreateDone()
{
	if (pregraphTimer) {
		Log () << "Pre-created " << pregraphs.count() << " GLGraphs, creation time " << tPerGraph*1e3 << " msec avg per graph, done.";
		delete precreateDialog, precreateDialog = 0;
		delete pregraphTimer, pregraphTimer = 0;
	}
	if (acqWaitingForPrecreate) {
		acqWaitingForPrecreate = false;
		noHotKeys = false;
		startAcqWithPossibleErrDialog();
	}	
}

void MainApp::startAcqWithPossibleErrDialog()
{
	QString errTitle, errMsg;
	if ( ! startAcq(errTitle, errMsg) )
	QMessageBox::critical(0, errTitle, errMsg);
}

QFrame * MainApp::getGLGraphWithFrame(bool nograph)
{
    QFrame *f = 0;
    if (!pregraphs.count()) 
        precreateOneGraph(nograph); // creates at least one GLGraph before returning
    f = pregraphs.front();
    pregraphs.pop_front();
    if (!f) Error() << "INTERNAL ERROR: Expected a valid QFrame from pregraphs list! Aiiieee...";
    return f;
}

void MainApp::putGLGraphWithFrame(QFrame *f)
{
	QList<GLGraph *> cl = f->findChildren<GLGraph *>();
	if (!cl.size()) {
        //Error() << "INTERNAL ERROR: QFrame passed in to putGLGraphWithFrame does not contain a GLGraph child!";
        delete f;
        return;
    }
    GLGraph *g = cl.front();
	f->setUpdatesEnabled(false);
    f->setParent(pregraphDummyParent);
    g->reset();
    pregraphs.push_back(f);    
	f->setUpdatesEnabled(true);
}

void MainApp::appInitialized()
{
    initializing = false;
    Log() << "Application initialized";    
    Status() <<  APPNAME << " initialized.";
    updateWindowTitles();
}

struct FV_IsViewingFile {
	FV_IsViewingFile(const QString & fn) : fn(fn) {}
	bool operator()(QWidget * w) const { 
		FileViewerWindow *f = dynamic_cast<FileViewerWindow *>(w);
		if (f) {
			return f->file() == fn; 
		}
		return false;
	}
	QString fn;
};

void MainApp::fileOpen()
{
	fileOpen(0);
}

void MainApp::fileOpen(FileViewerWindow *reuseWindow)
{
	if (isAcquiring()) {
		QMessageBox::StandardButtons ret = QMessageBox::warning(consoleWindow, "Acquisition Running", "An acquisition is currently running.  Opening a file now can impact performance cause the acquisition to fail.  It's recommended that you first stop the acquisition before proceeding.\n\nContinue anyway?",  QMessageBox::No|QMessageBox::Yes, QMessageBox::No);
		if (ret == QMessageBox::Cancel || ret == QMessageBox::No) return;		
	}
	QString filters[] = { "SpikeGL Data (*.bin)", "All Files (* *.*)" };
	noHotKeys = true;
	if (!lastOpenFile.length()) {
		mut.lock();
		lastOpenFile = outDir + "/DUMMY.bin";
		mut.unlock();
	}	
	QString fname = QFileDialog::getOpenFileName( consoleWindow, "Select a data file to open", lastOpenFile,
												  filters[0] + ";;" + filters[1], &filters[0]);
	noHotKeys = false;

	if (!fname.length()) 
		//user closed dialog box
		return;
	
	lastOpenFile = fname;
	saveSettings();
	
	QString errorMsg;
	if (!DataFile::isValidInputFile(fname, &errorMsg)) {
		QMessageBox::critical(consoleWindow, "Error Opening File", QFileInfo(fname).fileName() + " cannot be used for input.  " + errorMsg);
		return;
	}
		
	QList<QWidget *>::iterator it = std::find_if(windows.begin(), windows.end(), FV_IsViewingFile(fname));
	
	if (reuseWindow) {
		if (it != windows.end()) {
			if (*it == reuseWindow) return; // silently ignore.. ;)
			int b = QMessageBox::question(reuseWindow, "File already open", QFileInfo(fname).fileName() + " is already open in another viewer window.\n\nClose the other window and open the file in this window?", QMessageBox::Yes, QMessageBox::Cancel);
			if (b != QMessageBox::Yes) return;				
			QWidget *other = *it;
			windowMenuRemove(other);
			delete other;
		}
		if (!reuseWindow->viewFile(fname, &errorMsg)) {
			QMessageBox::critical(reuseWindow, "Error Opening File", errorMsg);
			return; // should we delete the reuse window here?
		}
	} else {			
		if (it != windows.end()) {
				// it already exists.. just raise the one we are working on already!
				windowMenuActivate(*it);
		} else {	
			// doesn't exist, create a new one!
			FileViewerWindow *fvw = new FileViewerWindow; ///< note we catch its close event and delete it 
			fvw->setAttribute(Qt::WA_DeleteOnClose, false);
			fvw->installEventFilter(this);
			if (!fvw->viewFile(fname, &errorMsg)) {
				QMessageBox::critical(consoleWindow, "Error Opening File", errorMsg);
				delete fvw, fvw = 0;
				return;
			}
			fvw->show();
			windowMenuAdd(fvw);
		}
	}
}

void MainApp::windowMenuRemove(QWidget *w) 
{
	QList<QWidget *>::iterator it = std::find(windows.begin(), windows.end(), w);
	if (it != windows.end()) {
		windows.erase(it);
		QAction *a = windowActions[w];
		consoleWindow->windowMenu()->removeAction(a);
		windowActions.remove(w);
		delete a;
	} else {
		Error() << "INTERNAL ERROR: A Window was closed but it was not found in the list of Windows!!!  FIXME!";
	}			
}

void MainApp::windowMenuAdd(QWidget *w) 
{
	QList<QWidget *>::iterator it = std::find(windows.begin(), windows.end(), w);
	if (it != windows.end()) {
		// Already in list.. silently ignore.
		return;
	}
	windows.push_back(w);
	QAction *a = new QAction(w->windowTitle(), w);
	a->setCheckable(true);
	a->setData(QVariant(reinterpret_cast<qulonglong>(w)));
	Connect(a, SIGNAL(triggered()), this, SLOT(windowMenuActivate()));
	windowActions[w] = a;
	consoleWindow->windowMenu()->addAction(a);
}

void MainApp::windowMenuActivate(QWidget *w) 
{
	if (!w) {
		QAction *a = dynamic_cast<QAction *>(sender());
		if (!a) return;
		w = reinterpret_cast<QWidget *>(a->data().toULongLong());
	}
	if (w) {
		if (w->isHidden()) w->show();
		w->raise();
		w->activateWindow();
	}
}

void MainApp::windowMenuAboutToShow()
{
	QWidget *active = activeWindow();
	// figure out which action to check, if any..	
	for (QMap<QWidget*,QAction*>::iterator it = windowActions.begin(); it != windowActions.end(); ++it) {
		QWidget *w = it.key();
		QAction *a = it.value();
		a->setChecked(w == active);
	}	
}

void MainApp::bringAllToFront()
{
	QWidget *previousActive = activeWindow();
	QWidgetList all = topLevelWidgets();
	foreach(QWidget *w, all) {
		if (!w->isHidden())	w->raise();
	}
	if (previousActive) previousActive->raise();
}

void MainApp::optionsSortGraphsByElectrode()
{
	m_sortGraphsByElectrodeId = sortGraphsByElectrodeAct->isChecked();
	if (graphsWindow) {
		if (sortGraphsByElectrodeId())
			graphsWindow->sortGraphsByElectrodeId();
		else
			graphsWindow->sortGraphsByIntan();
	}
	saveSettings();
}

#ifdef Q_OS_WIN
/*static*/ const unsigned DataFile_Fn_Shm::BUF_SIZE = 1024;
/*static*/ const char DataFile_Fn_Shm::szName[1024] = "Global\\SpikeGLFileNameShm";

DataFile_Fn_Shm::DataFile_Fn_Shm() 
: DataFile(), hMapFile(NULL), pBuf(0)
{
	
	hMapFile = CreateFileMappingA(
								 INVALID_HANDLE_VALUE,    // use paging file
								 NULL,                    // default security
								 PAGE_READWRITE,          // read/write access
								 0,                       // maximum object size (high-order DWORD)
								 BUF_SIZE,                // maximum object size (low-order DWORD)
								 szName);                 // name of mapping object
	
	if (hMapFile == NULL)
	{
		Error() << "Could not create file mapping object (" << GetLastError() << ").";
		return;
	}
	pBuf = (void *) MapViewOfFile(hMapFile,   // handle to map object
								  FILE_MAP_ALL_ACCESS, // read/write permission
								  0,
								  0,
								  BUF_SIZE);
	
	if (pBuf == NULL)
	{
		Error() << "Could not map view of file (" << GetLastError() << ").";
		CloseHandle(hMapFile);		
		return;
	}
	/// now clear the shm...
	{
		char dummybuf[BUF_SIZE];
		memset(dummybuf, 0, BUF_SIZE); // clear the memory!
		CopyMemory((PVOID)pBuf, (PVOID)dummybuf, BUF_SIZE); // post cleared memory..
	}
}

DataFile_Fn_Shm::~DataFile_Fn_Shm() 
{
	UnmapViewOfFile(pBuf); pBuf = NULL;
	CloseHandle(hMapFile); hMapFile = NULL;
}

bool DataFile_Fn_Shm::openForWrite(const DAQ::Params & params, const QString & filename_override)
{
    QMutexLocker ml(&mut);
	bool ret = DataFile::openForWrite(params, filename_override);
	if (hMapFile && pBuf) {
		QString fn = fileName();
		QByteArray bytes = fn.toUtf8();
		CopyMemory((PVOID)pBuf, (PVOID)(bytes.constData()), bytes.length()+1);
	}
	return ret;
}


bool DataFile_Fn_Shm::closeAndFinalize()
{
    QMutexLocker ml(&mut);
	bool ret = DataFile::closeAndFinalize();
	if (hMapFile && pBuf) {
		CopyMemory((PVOID)pBuf, (PVOID)"", 1); // null string..
	}
	return ret;
}

#endif

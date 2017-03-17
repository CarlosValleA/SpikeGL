/*
 *  FG_ConfigDialog.h
 *  SpikeGL
 *
 *  Created by calin on 3/5/15.
 *  Copyright 2015 Calin Culianu <calin.culianu@gmail.com>. All rights reserved.
 *
 */
#include "FG_ConfigDialog.h"
#include "ui_FG_ConfigDialog.h"
#include "MainApp.h"
#include "ConfigureDialogController.h"
#include <QMessageBox>
#include <QFileDialog>
#include "ui_FG_ChanMapDialog.h"
#include <QTextStream>
#include <QSet>
#include <QStringList>
#include "Icon-Config.xpm"

FG_ConfigDialog::FG_ConfigDialog(DAQ::Params & params, QObject *parent)
: QObject(parent), acceptedParams(params)
{
    mb = 0;
	dialogW = new QDialog(0);
	dialogW->setAttribute(Qt::WA_DeleteOnClose, false);
	dialog = new Ui::FG_ConfigDialog;
    dialog->setupUi(dialogW);
    dialogW->setWindowIcon(QIcon(QPixmap(Icon_Config_xpm)));
    Connect(dialog->browseBut, SIGNAL(clicked()), this, SLOT(browseButClicked()));
    Connect(dialog->chanMapBut, SIGNAL(clicked()), this, SLOT(chanMapButClicked()));

    // triggering task stop/start params -- this is basically copy/pasted from ConfigureDialogController.cpp -- todo: refactor both!
    acqPdParams = new Ui::AcqPDParams;
    acqTimedParams = new Ui::AcqTimedParams;
    acqPdParamsW = new QWidget(dialog->acqFrame);
    acqTimedParamsW = new QWidget(dialog->acqFrame);
    acqPdParams->setupUi(acqPdParamsW);
    acqTimedParams->setupUi(acqTimedParamsW);
    Connect(dialog->acqStartEndCB, SIGNAL(activated(int)), this, SLOT(acqStartEndCBChanged()));
    acqPdParams->pdPassthruAOChk->setHidden(true); // not used in this mode
    acqPdParams->pdPassthruAOSB->setHidden(true); // not used in this mode
    acqPdParams->virtualChk->setHidden(true);
    acqPdParams->pdAIThreshSB->setMinimum(0.0);
    acqPdParams->pdAIThreshSB->setMaximum(5.0);
}

FG_ConfigDialog::~FG_ConfigDialog()
{
    delete mb, mb = 0;
	delete dialogW; dialogW = 0;
	delete dialog; dialog = 0;
}

void FG_ConfigDialog::browseButClicked()
{
    QString fn = QFileDialog::getSaveFileName(dialogW, "Select output file", dialog->outputFileLE->text());
    if (fn.length()) {
		QFileInfo fi(fn);
		QString suff = fi.suffix();
		if (!suff.startsWith(".")) suff = QString(".") + suff;
		if (suff.toLower() != ".bin") fn += ".bin";
		dialog->outputFileLE->setText(fn);
	}
}

void FG_ConfigDialog::createAndShowPleaseWaitDialog()
{
    int wflags = Qt::Window|/*Qt::FramelessWindowHint|Qt::MSWindowsFixedSizeDialogHint|Qt::CustomizeWindowHint|*/Qt::WindowTitleHint|Qt::WindowStaysOnTopHint;
    if (mb) delete mb;
    mb = new QMessageBox(QMessageBox::Information, "Probing Hardware", "Probing framegrabber hardware, please wait...", QMessageBox::Abort, (QWidget *)(mainApp()->console()), (Qt::WindowFlags)wflags);
    QList<QAbstractButton *> buts = mb->buttons();
    foreach (QAbstractButton *b, buts) {
        mb->removeButton(b);
    }
    mb->setModal(true);
    mb->setWindowModality(Qt::ApplicationModal);
    mb->setWindowFlags((Qt::WindowFlags)wflags);
    mb->show();
    mb->activateWindow();
    mb->raise();
    mb->update();
    mb->open();
}
void FG_ConfigDialog::actuallyDoHardwareProbe()
{
    DAQ::FGTask::probeHardware();
}

int FG_ConfigDialog::exec()
{
    if (DAQ::FGTask::probedHardware.empty() || Util::getTime() - DAQ::FGTask::lastProbeTS() > 10.0) {
        createAndShowPleaseWaitDialog();
        mainApp()->processEvents(QEventLoop::ExcludeUserInputEvents|QEventLoop::ExcludeSocketNotifiers,100);
        actuallyDoHardwareProbe();
    }
    delete mb, mb = 0;

    if (DAQ::FGTask::probedHardware.empty()) {
        QMessageBox::critical((QWidget *)(mainApp()->console()),"No Valid Framegrabbers", "No compatible framegrabbers appear to be present on the system.  If you believe this message is in error, try disabling then re-enabling your framegrabber card in the Windows device manager.");
        return ABORT;
    }

	mainApp()->configureDialogController()->loadSettings(); // this makes the shared params object get updated form the settings
	
	guiFromSettings();
	
	ValidationResult vr;
	int r;
	do {
		vr = ABORT;
		r = dialogW->exec();
		QString errTit, errMsg;
		if (r == QDialog::Accepted) {
			vr = validateForm(errTit, errMsg);
			if (vr == OK) {
				DAQ::Params & p(acceptedParams);
				p.bug.reset();
				p.fg.reset();
				p.fg.enabled = true;
                p.fg.disableChanMap = false;


				// todo.. form-specific stuff here which affects p.fg struct...
				// ...
                p.fg.chanMapText = chanMapTxt;

                p.fg.baud = dialog->baud->currentIndex();
                p.fg.com = dialog->com->currentIndex();
                p.fg.bits = dialog->bits->currentIndex();
                p.fg.parity = dialog->parity->currentIndex();
                p.fg.stop = dialog->stop->currentIndex();
                DAQ::FGTask::Hardware hw = DAQ::FGTask::probedHardware.at(dialog->sapdevCB->currentIndex());
                p.fg.sidx = hw.serverIndex;
                p.fg.ridx = hw.resourceIndex;
                p.fg.extraAI = dialog->aiExtraChk->isChecked();
                p.fg.spatialVisSuppressExtraChans = dialog->aiNoSpatialVisChk->isChecked();

				p.suppressGraphs = false; //dialog->disableGraphsChk->isChecked();
				p.resumeGraphSettings = false; //dialog->resumeGraphSettingsChk->isChecked();
				
                p.nVAIChans =  DAQ::FGTask::NumChans + (p.fg.extraAI ? 2 : 0);
				p.nVAIChans1 = p.nVAIChans;
				p.nVAIChans2 = 0;
				p.aiChannels2.clear();
                //p.aiString2.clear();
				p.aiChannels.resize(p.nVAIChans);
				p.subsetString = "ALL"; //dialog->channelSubsetLE->text();
				p.demuxedBitMap.resize(p.nVAIChans); p.demuxedBitMap.fill(true);
                for (int i = 0; i < (int)p.nVAIChans; ++i) {
					p.aiChannels[i] = i;
				}
				if (p.subsetString.compare("ALL", Qt::CaseInsensitive) != 0) {
					QVector<unsigned> subsetChans;
					bool err;
					p.subsetString = ConfigureDialogController::parseAIChanString(p.subsetString, subsetChans, &err, true);
					if (!err) {
						p.demuxedBitMap.fill(false);
						for (int i = 0; i < subsetChans.size(); ++i) {
							int bit = subsetChans[i];
							if (bit < p.demuxedBitMap.size()) p.demuxedBitMap[bit] = true;
						}
						if (p.demuxedBitMap.count(true) == 0) {
							Warning() << "Framegrabber channel subset string specified invalid. Proceeding with 'ALL' channels set to save!";
							p.demuxedBitMap.fill(true);
							p.subsetString = "ALL";
						}
					}
				}
				
				//p.overrideGraphsPerTab = dialog->graphsPerTabCB->currentText().toUInt();

				p.isIndefinite = true;
				p.isImmediate = true;
                p.acqStartEndMode = (DAQ::AcqStartEndMode)dialog->acqStartEndCB->currentIndex();
                p.usePD = 0; p.pdChan = 0; p.pdChanIsVirtual = true; p.idxOfPdChan = 0;
                if (p.acqStartEndMode == DAQ::PDStartEnd || p.acqStartEndMode == DAQ::PDStart || p.acqStartEndMode == DAQ::AITriggered) {
                    p.usePD = true;
                    p.pdChanIsVirtual = true;
                    p.pdChan = acqPdParams->pdAISB->value() + DAQ::FGTask::NumChans;
                    p.idxOfPdChan = p.pdChan;
                    double val = acqPdParams->pdAIThreshSB->value();
                    if (val < 0.0) val = 0.0; if (val > 5.0) val = 5.0;
                    p.pdThresh = static_cast<int16>(((val/5.0) * 65535.0) - 32768.0);
                    p.pdStopTime = acqPdParams->pdStopTimeSB->value();
                    p.silenceBeforePD = double(acqPdParams->pdPre->value()) / 1000.0;
                    p.pdThreshW = acqPdParams->pdWSB->value();
                }
                p.stimGlTrigResave = dialog->stimGLReopenCB->isChecked();
                if (p.acqStartEndMode == DAQ::Timed) {
                    p.startIn = acqTimedParams->startHrsSB->value()*60.*60. + acqTimedParams->startMinsSB->value()*60 + acqTimedParams->startSecsSB->value();
                    p.duration = acqTimedParams->durHrsSB->value()*60.*60. + acqTimedParams->durMinsSB->value()*60 + acqTimedParams->durSecsSB->value();
                    p.isIndefinite = acqTimedParams->indefCB->isChecked();
                    p.isImmediate = acqTimedParams->nowCB->isChecked();
                }



                /*p.chanMap SET HERE: */
                p.fg.spatialCols = spatialCols; p.fg.spatialRows = spatialRows;
                if (!DAQ::FGTask::setupCMFromArray(&chanMapFromUser[0], 0, &p.chanMap)) {
                    vr = ABORT;
                    continue;
                }
				
				if (AGAIN == ConfigureDialogController::setFilenameTakingIntoAccountIncrementHack(p, p.acqStartEndMode, dialog->outputFileLE->text(), dialogW)) {
					vr = AGAIN;
					continue;
				}

                p.graphUpdateRate = dialog->graphUpdateRateHzSB->value();
                p.spatialVisUpdateRate = dialog->spatialVisUpdateRateHzSB->value();
                p.overrideGraphsPerTab = dialog->graphsPerTabCB->currentText().toUInt();

				
				saveSettings();

                p.lowLatency = false/*true*/;

				// this stuff doesn't need to be saved since it's constant and will mess up regular acq "remembered" values
                p.dev = "Framegrabber";
                p.nExtraChans1 = p.fg.extraAI ? 2 : 0;
				p.nExtraChans2 = 0;
				
				p.extClock = true;
				p.mode = DAQ::AIRegular;
				p.aoPassthru = 0;
				p.dualDevMode = false;
          //      p.stimGlTrigResave = true; // HACK XXX don't open file for now by default since it's huge
                p.srate = DAQ::FGTask::SamplingRate;
				p.aiTerm = DAQ::Default;
				p.aiString = QString("0:%1").arg(p.nVAIChans-1);
				p.customRanges.resize(p.nVAIChans);
				p.chanDisplayNames.resize(p.nVAIChans);
				DAQ::Range rminmax(1e9,-1e9);
				for (unsigned i = 0; i < p.nVAIChans; ++i) {
					DAQ::Range r;
                    if (i < p.nVAIChans-(p.nExtraChans1+p.nExtraChans2)) {
                        int chan_id_for_display = i;
                        //r.min = -5., r.max = 5.;
                        r.min = -0.00638976; r.max = 0.006389565; // hardcoded range of framegrabber intan voltages...
                        // since ttl lines may be missing in channel set, renumber the ones that are missing for display purposes

                        if (rminmax.min > r.min) rminmax.min = r.min;
                        if (rminmax.max < r.max) rminmax.max = r.max;
                        p.customRanges[i] = r;
                        p.chanDisplayNames[i] = DAQ::FGTask::getChannelName(chan_id_for_display, &p.chanMap);
                    } else { // extra AI channels...
                        r.min = 0.0; r.max = 5.0;
                        p.customRanges[i] = r;
                        p.chanDisplayNames[i] = QString("ExtraAI%1").arg(i-(p.nVAIChans-(p.nExtraChans1+p.nExtraChans2)));
                    }
				}
				p.range = rminmax;
				p.auxGain = 1.0;
				
			} else if (vr==AGAIN) {
				if (errTit.length() && errMsg.length())
					QMessageBox::critical(dialogW, errTit, errMsg);
			} else if (vr==ABORT) r = QDialog::Rejected;
		}
	} while (vr==AGAIN && r==QDialog::Accepted);	
	return r;	
}

FG_ConfigDialog::ValidationResult 
FG_ConfigDialog::validateForm(QString & errTitle, QString & errMsg, bool isGUI)
{
	(void)errTitle; (void)errMsg; (void)isGUI;
    DAQ::Params & p (acceptedParams);
    QString mapstr = p.fg.chanMapText;
    int req_chans = DAQ::FGTask::NumChans;
    QString err = validateChanMappingText(mapstr, req_chans, spatialRows, spatialCols, &chanMapFromUser);
    if (err.length()) {
        errTitle = "Channel Mapping Invalid";
        errMsg = err;
        return AGAIN;
    }
    int m = dialog->acqStartEndCB->currentIndex();
    int ai = acqPdParams->pdAISB->value();
    switch (m) {
    case DAQ::AITriggered:
        if (dialog->stimGLReopenCB->isChecked()) {
            errTitle = "Incompatible Configuration", errMsg = QString().sprintf("'Re-Open New Save File on StimGL Experiment' not compatible with 'TTL Controlled Start & Re-Triggered'");
            return AGAIN;
        }
    case DAQ::PDStart:
    case DAQ::PDStartEnd:
        if (!dialog->aiExtraChk->isChecked()) {
            errTitle = "AI Extra Chan Required"; errMsg = "AI Extra Channels are required for AI-based triggering mode!";
            return AGAIN;
        }
        if (ai < 0 || ai > 1) {
            errTitle = "AI Channel Invalid";
            errMsg = "The AI channel specified for triggering is out of range. Either specify '0' or '1' as the AI channel to use for triggering!";
            return AGAIN;
        }
        break;
    case DAQ::StimGLStartEnd:
        if (dialog->stimGLReopenCB->isChecked()) {
            errTitle = "Incompatible Configuration", errMsg = QString().sprintf("'Re-Open New Save File on StimGL Experiment' not compatible with 'StimGL Plugin Start & End'");
            return AGAIN;
        }
        break;
    default:
        // NOOP
        (void)0;
        break;
    }

    chanMapTxt = mapstr;
	return OK;
}

static QString generateDefaultMappingString(int which, int rows, int cols)
{
    const int *m = DAQ::FGTask::getDefaultMapping(which);
    QString s = "";

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if (c) s += "\t";
            s += QString::number(m[r*cols+c]);
        }
        s += "\n";
    }
    return s;
}

void FG_ConfigDialog::guiFromSettings()
{
	DAQ::Params & p(acceptedParams);
	
	dialog->outputFileLE->setText(p.outputFile);
    dialog->baud->setCurrentIndex(p.fg.baud);
    dialog->com->setCurrentIndex(p.fg.com);
    dialog->bits->setCurrentIndex(p.fg.bits);
    dialog->parity->setCurrentIndex(p.fg.parity);
    dialog->stop->setCurrentIndex(p.fg.stop);
    dialog->graphUpdateRateHzSB->setValue(p.graphUpdateRate);
    dialog->spatialVisUpdateRateHzSB->setValue(p.spatialVisUpdateRate);
    dialog->aiExtraChk->setChecked(p.fg.extraAI);
    dialog->aiNoSpatialVisChk->setChecked(p.fg.spatialVisSuppressExtraChans);

    dialog->graphsPerTabCB->clear();
    dialog->graphsPerTabCB->addItem("Default");
    for (int i = 2; i*i <= MAX_NUM_GRAPHS_PER_GRAPH_TAB; ++i) {
        dialog->graphsPerTabCB->addItem(QString::number(i*i));
    }
    dialog->graphsPerTabCB->setCurrentIndex(0);
    for (int i = 1; i < dialog->graphsPerTabCB->count(); ++i) {
        if (dialog->graphsPerTabCB->itemText(i).toUInt() == (unsigned)p.overrideGraphsPerTab) {
            dialog->graphsPerTabCB->setCurrentIndex(i);
            break;
        }
    }

    if (!DAQ::FGTask::probedHardware.empty()) {
        dialog->sapdevCB->clear();
        int i = 0;
        foreach(const DAQ::FGTask::Hardware & h,DAQ::FGTask::probedHardware) {
            dialog->sapdevCB->addItem(QString("%1 - %2").arg(h.serverName).arg(h.resourceName));
            if (h.serverIndex == p.fg.sidx && h.resourceIndex == p.fg.ridx) dialog->sapdevCB->setCurrentIndex(i);
            ++i;
        }
    }

    if (!p.fg.chanMapText.trimmed().length()) p.fg.chanMapText = generateDefaultMappingString(0, 64, 36);


    dialog->acqStartEndCB->setCurrentIndex((int)p.acqStartEndMode);
    double val = (p.pdThresh/32768.+1.)/2. * (5.0-0.0) + 0.0;
    acqPdParams->pdAIThreshSB->setValue(val);
    acqPdParams->pdStopTimeSB->setValue(p.pdStopTime);
    acqPdParams->pdPre->setValue(p.silenceBeforePD*1000.);
    acqPdParams->pdWSB->setValue(p.pdThreshW);
    acqPdParams->virtualChk->setChecked(p.pdChanIsVirtual);
    acqPdParams->pdAISB->setMinimum(0);
    acqPdParams->pdAISB->setMaximum(1);
    acqPdParams->pdAISB->setValue(p.pdChan - DAQ::FGTask::NumChans);
    // now the timed params stuff
    acqTimedParams->startHrsSB->setValue(int(p.startIn/(60.*60.)));
    acqTimedParams->startMinsSB->setValue(int((int(p.startIn)%(60*60))/60.));
    acqTimedParams->startSecsSB->setValue(int((int(p.startIn)%(60*60)))%60);
    acqTimedParams->durHrsSB->setValue(int(p.duration/(60.*60.)));
    acqTimedParams->durMinsSB->setValue(int((int(p.duration)%(60*60))/60.));
    acqTimedParams->durSecsSB->setValue((int(p.duration)%(60*60))%60);
    acqTimedParams->indefCB->setChecked(p.isIndefinite);
    acqTimedParams->nowCB->setChecked(p.isImmediate);

    dialog->stimGLReopenCB->setChecked(p.stimGlTrigResave);

    // fire off slots to polish
    acqStartEndCBChanged();
}

void FG_ConfigDialog::saveSettings()
{
	mainApp()->configureDialogController()->saveSettings();
}

/* static */
QString FG_ConfigDialog::validateChanMappingText(const QString &s, int reqchans, int & rows, int & cols, QVector<int> * m_out) {
    rows = 0, cols = 0;
    bool seen0 = false;
    if (reqchans <= 0) return "Second parameter passed to function is invalid";
    QSet<int> allnums;
    for (int i = 0; i < reqchans+1; ++i) allnums.insert(i);
    if (m_out) m_out->clear(), m_out->reserve(reqchans);
    QStringList lines = s.split(QChar('\n'),QString::SkipEmptyParts);
    for (QStringList::iterator it = lines.begin(); it != lines.end(); ++it) {
        QStringList cells = (*it).trimmed().split(QRegExp("\\s+"), QString::SkipEmptyParts);
        if (cells.length()) {
            int c = 0, val;
            for (QStringList::iterator it2 = cells.begin(); it2 != cells.end(); ++it2) {
                bool ok;
                val = (*it2).toInt(&ok);
                if (!ok)  return QString("Parse error in row %1 col %2").arg(rows).arg(c);
                if (!allnums.contains(val)) return QString("Value %1 appears more than once in table at row %2 col %3").arg(val).arg(rows).arg(c);
                allnums.remove(val);
                if (m_out) m_out->push_back(val);
                if (!val) seen0 = true;
                ++c;
            }
            if (!rows) cols = c;
            else if (cols != c) {
                return "Irregular table -- some rows have more columns than others!";
            }
            ++rows;
        }
    }
    if (rows*cols != reqchans)
        return QString("Table does not specify the required %1 channels. (Read a %2x%3 table = %4 channels)").arg(reqchans).arg(rows).arg(cols).arg(rows*cols);
    if (!seen0 && m_out) { // was 1-indexed.. renormalize to 0-indexed
        int sz = m_out->size();
        for (int i = 0; i < sz; ++i) --(*m_out)[i];
    }
    return "";
}

void FG_ConfigDialog::chanMapButClicked()
{
    QDialog dW(dialogW);
    Ui::FG_ChanMapDialog d;
    DAQ::Params & p(acceptedParams);
    int req_chans = 0;

    d.setupUi(&dW);
    QString *paramStr = 0;
    d.acqModeLbl->setText("Janelia FPGA");
    d.nChansLbl->setText(QString::number(req_chans=DAQ::FGTask::NumChans));
    paramStr=&p.fg.chanMapText;
    d.chanMapTE->setPlainText(*paramStr);

    bool keepTrying = true;

    while (keepTrying) {
        int ret = dW.exec();
        if (ret == QDialog::Accepted) {
            int r, c; QVector<int> parsed;
            QString err = validateChanMappingText(d.chanMapTE->toPlainText(), req_chans, r, c, &parsed);
            if (err.length())
                QMessageBox::critical(dialogW,"Mapping Parse Error",err);
            else {
                Debug() << "mapping ok -- rows: " << r << " cols: " << c;
                *paramStr = d.chanMapTE->toPlainText();
                keepTrying = false;
            }
        } else keepTrying = false;
    }
}

void FG_ConfigDialog::acqStartEndCBChanged()
{
    bool entmp = false, aitriggered = false;
    acqPdParamsW->hide();
    acqTimedParamsW->hide();
    DAQ::AcqStartEndMode mode = (DAQ::AcqStartEndMode)dialog->acqStartEndCB->currentIndex();
    dialog->acqStartEndDescrLbl->hide();
    acqPdParams->pdAILabel->setToolTip(acqPdParams->pdAILabel->toolTip().replace("(physical or virtual)","'Extra'"));
    acqPdParams->pdAISB->setToolTip(acqPdParams->pdAILabel->toolTip());
    dialog->aiExtraChk->setEnabled(true);

    switch (mode) {
    case DAQ::Immediate:
        dialog->acqStartEndDescrLbl->setText("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.0//EN\" \"http://www.w3.org/TR/REC-html40/strict.dtd\"><html><head><meta name=\"qrichtext\" content=\"1\" /><style type=\"text/css\">p, li { white-space: pre-wrap; }</style></head><body style=\" font-family:'Sans Serif'; font-size:9pt; font-weight:400; font-style:normal;\"><p style=\" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;\"><span style=\" font-size:10pt; font-style:italic; \">The acquisition will start immediately.</span></p></body></html>");
        dialog->acqStartEndDescrLbl->show();
        break;
    case DAQ::AITriggered:
        aitriggered = true;
        // fall thru...
    case DAQ::PDStartEnd:
        entmp = true;
        // fall thru...
    case DAQ::PDStart:
        acqPdParams->pdStopTimeLbl->setEnabled(entmp);
        acqPdParams->pdAILabel->setText("AI Ch:");
        acqPdParams->pdAISB->setToolTip(acqPdParams->pdAILabel->toolTip());
        if (aitriggered) {
            acqPdParams->pdStopTimeLbl->setText("AI stop time (sec):");
        } else {
            acqPdParams->pdStopTimeLbl->setText("PD stop time (sec):");
        }
        acqPdParams->pdStopTimeSB->setEnabled(entmp);
        acqPdParamsW->setParent(dialog->acqFrame);
        acqPdParamsW->show();
        dialog->aiExtraChk->setChecked(true);
        dialog->aiExtraChk->setEnabled(false);
        break;
    case DAQ::Timed:
        acqTimedParamsW->setParent(dialog->acqFrame);
        acqTimedParamsW->show();
        dialog->aiExtraChk->setEnabled(true);
        break;
    case DAQ::StimGLStartEnd:
        dialog->acqStartEndDescrLbl->setText("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.0//EN\" \"http://www.w3.org/TR/REC-html40/strict.dtd\"><html><head><meta name=\"qrichtext\" content=\"1\" /><style type=\"text/css\">p, li { white-space: pre-wrap; }</style></head><body style=\" font-family:'Sans Serif'; font-size:9pt; font-weight:400; font-style:normal;\"><p style=\" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;\"><span style=\" font-size:10pt; font-style:italic; color:#294928;\">The acquisition will be triggered to start and end by the external StimGL II program.</span></p></body></html>");
        dialog->acqStartEndDescrLbl->show();
        dialog->aiExtraChk->setEnabled(true);
        break;
    case DAQ::StimGLStart:
        dialog->acqStartEndDescrLbl->setText("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.0//EN\" \"http://www.w3.org/TR/REC-html40/strict.dtd\"><html><head><meta name=\"qrichtext\" content=\"1\" /><style type=\"text/css\">p, li { white-space: pre-wrap; }</style></head><body style=\" font-family:'Sans Serif'; font-size:9pt; font-weight:400; font-style:normal;\"><p style=\" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;\"><span style=\" font-size:10pt; font-style:italic; color:#294928;\">The acquisition will be triggered to start by the external StimGL II program.</span></p></body></html>");
        dialog->acqStartEndDescrLbl->show();
        dialog->aiExtraChk->setEnabled(true);
        break;
    default:
        Error() << "INTERNAL ERROR: INVALID ACQSTARTENDMODE!  FIXME!";
        break;
    }

    dialog->stimGLReopenCB->setEnabled(!aitriggered);
    if (aitriggered) dialog->stimGLReopenCB->setChecked(false);

    //aiRangeChanged();
}

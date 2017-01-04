#include "ConfigureDialogController.h"
#include "SpikeGL.h"
#include "Util.h"
#include "MainApp.h"
#include "ui_Dialog.h"
#include "ui_TextBrowser.h"
#include <QTimeEdit>
#include <QTime>
#include <QMessageBox>
#include <QSet>
#include <QSettings>
#include <QFileInfo>
#include <QFileDialog>
#include <QRegExp>
#include <QTextStream>
#include "Icon-Config.xpm"
#include "ConsoleWindow.h"

#define SETTINGS_GROUP "ConfigureDialogController"

ConfigureDialogController::ConfigureDialogController(QObject *parent)
    : QObject(parent), chanMapCtl(this)
{
    aoPassW = 0;
    applyDlg = 0;
	helpWindow = 0;
    dialogW = new QDialog(0);
    dialog = new Ui::ConfigureDialog;
    aoPassthru = new Ui::AoPassThru;
    acqPdParams = new Ui::AcqPDParams;
    acqTimedParams = new Ui::AcqTimedParams;
	
	probeDAQHardware();
	
    dialog->setupUi((QDialog *)dialogW);
    acqPdParamsW = new QWidget(dialog->acqFrame);
    acqTimedParamsW = new QWidget(dialog->acqFrame);
    dialogW->setWindowIcon(QIcon(QPixmap(Icon_Config_xpm)));
    acqPdParams->setupUi(acqPdParamsW);
    acqTimedParams->setupUi(acqTimedParamsW);
    aoPassthru->setupUi(dialog->aoPassthruWidget);
    acceptedParams.mode = DAQ::AIUnknown;
    // connect signal slots
    Connect(dialog->acqStartEndCB, SIGNAL(activated(int)), this, SLOT(acqStartEndCBChanged()));
    Connect(dialog->acqModeCB, SIGNAL(activated(int)), this, SLOT(acqModeCBChanged()));
    Connect(dialog->deviceCB, SIGNAL(activated(const QString &)), this, SLOT(deviceCBChanged()));
    Connect(aoPassthru->aoDeviceCB, SIGNAL(activated(const QString &)), this, SLOT(aoDeviceCBChanged()));
	Connect(aoPassthru->aoNoteLink, SIGNAL(linkActivated(const QString  &)), this, SLOT(aoNote()));
    Connect(dialog->browseBut, SIGNAL(clicked()), this, SLOT(browseButClicked()));
    Connect(aoPassthru->aoPassthruGB, SIGNAL(toggled(bool)), this, SLOT(aoPassthruChkd()));
    Connect(acqPdParams->pdPassthruAOChk, SIGNAL(toggled(bool)), this, SLOT(aoPDChanChkd()));
	Connect(acqPdParams->virtualChk, SIGNAL(toggled(bool)), this, SLOT(aiRangeChanged()));
    Connect(dialog->muxMapBut, SIGNAL(clicked()), &chanMapCtl, SLOT(exec()));
    Connect(dialog->aiRangeCB, SIGNAL(currentIndexChanged(int)), this, SLOT(aiRangeChanged()));
    Connect(dialog->auxGainSB, SIGNAL(valueChanged(double)), this, SLOT(aiRangeChanged())); // to recompute AI/VAI pd threshold valid values..
    Connect(acqPdParams->pdPassthruAOSB, SIGNAL(valueChanged(int)), this, SLOT(aoPDPassthruUpdateLE()));
	Connect(dialog->bufferSizeSlider, SIGNAL(valueChanged(int)), this, SLOT(bufferSizeSliderChanged()));
	Connect(aoPassthru->bufferSizeSlider, SIGNAL(valueChanged(int)), this, SLOT(aoBufferSizeSliderChanged()));
	Connect(dialog->lowLatencyChk, SIGNAL(clicked()), this, SLOT(bufferSizeSliderChanged()));
	Connect(dialog->dualDevModeChk, SIGNAL(clicked()), this, SLOT(dualDevModeChkd()));
	Connect(dialog->secondIsAuxChk, SIGNAL(clicked()), this, SLOT(secondIsAuxChkd()));
	// hide fast settle stuff as it's no longer supported
	dialog->fastSettleSB->hide();
	dialog->fastSettleLbl->hide();
}

ConfigureDialogController::~ConfigureDialogController()
{
    delete acqPdParams;
    delete acqTimedParams;
    delete dialog;
    delete applyDlg;
    delete acqPdParamsW;
    delete acqTimedParamsW;
    delete dialogW;
    delete aoPassW;
	//delete helpWindow;
}

void ConfigureDialogController::probeDAQHardware()
{
    aiDevRanges = DAQ::ProbeAllAIRanges();
    aoDevRanges = DAQ::ProbeAllAORanges();
    
    aiChanLists = DAQ::ProbeAllAIChannels();
    aoChanLists = DAQ::ProbeAllAOChannels();	
}

void ConfigureDialogController::resetAOPassFromParams(Ui::AoPassThru *aoPassthru, DAQ::Params *p_in, const unsigned *srate_override)
{
    if (!p_in) {
        loadSettings();
        p_in = &acceptedParams;
    }
    DAQ::Params & p (*p_in); // this may have just gotten populated from settings...?
    aoPassthru->aoPassthruLE->setText(p.aoPassthruString);
    aoPassthru->aoDeviceCB->clear();
    aoPassthru->aoPassthruGB->setCheckable(Util::objectHasAncestor(aoPassthru->aoPassthruGB,dialogW));
    aoPassthru->aoPassthruGB->setChecked(p.aoPassthru);
    aoDevNames.clear();
    QList<QString> devs = aoChanLists.uniqueKeys();
    int sel = 0, i = 0;
    for (QList<QString>::const_iterator it = devs.begin(); it != devs.end(); ++it, ++i) {
        if (p.aoDev == *it) sel = i;
        aoPassthru->aoDeviceCB->addItem(QString("%1 (%2)").arg(*it).arg(DAQ::GetProductName(*it)));
        aoDevNames.push_back(*it);
    }
    if ( aoPassthru->aoDeviceCB->count() ) // empty CB sometimes on missing AO??
        aoPassthru->aoDeviceCB->setCurrentIndex(sel);
	
	aoPassthru->bufferSizeSlider->setValue(p.aoBufferSizeCS);
	aoPassthru->aoClockCB->setCurrentIndex(0);
	for (i = 0; i < aoPassthru->aoClockCB->count(); ++i) {
		if (aoPassthru->aoClockCB->itemText(i) == p.aoClock) {
			aoPassthru->aoClockCB->setCurrentIndex(i);
			break;
		}
	}
    aoPassthru->srateSB->setValue(srate_override ? *srate_override : p.aoSrate);
    updateAOBufferSizeLabel(aoPassthru);
}

void ConfigureDialogController::resetFromParams(DAQ::Params *p_in)
{
    if (!p_in) {
        loadSettings();
        p_in = &acceptedParams;
    }

	probeDAQHardware();

    DAQ::Params & p (*p_in); // this just got populated from settings

    // initialize the dialogs with some values from settings
	chanMapCtl.loadSettings();
    dialog->outputFileLE->setText(p.outputFile);
    if (int(p.mode) < dialog->acqModeCB->count())
        dialog->acqModeCB->setCurrentIndex((int)p.mode);    
    dialog->channelSubsetLE->setText(p.subsetString);
    dialog->channelSubsetLabel->setEnabled(p.mode != DAQ::AIRegular);
    dialog->channelSubsetLE->setEnabled(p.mode != DAQ::AIRegular);
    dialog->deviceCB->clear();
	dialog->deviceCB_2->clear();
    dialog->clockCB->setCurrentIndex(p.extClock ? 0 : 1);
    dialog->srateSB->setValue(p.srate);
    dialog->fastSettleSB->setValue(p.fastSettleTimeMS);
    dialog->auxGainSB->setValue(p.auxGain);
    dialog->stimGLReopenCB->setChecked(p.stimGlTrigResave);
    int ci = dialog->aiTerminationCB->findText(DAQ::TermConfigToString(p.aiTerm), Qt::MatchExactly);
    dialog->aiTerminationCB->setCurrentIndex(ci > -1 ? ci : 0);
    if (int(p.doCtlChan) < dialog->doCtlCB->count())
        dialog->doCtlCB->setCurrentIndex(p.doCtlChan);
    dialog->channelListLE->setText(p.aiString);
    dialog->channelListLE_2->setText(p.aiString2);
    if (p.acqStartEndMode == DAQ::Bug3TTLTriggered) p.acqStartEndMode = DAQ::Immediate;
    dialog->acqStartEndCB->setCurrentIndex((int)p.acqStartEndMode);
	dialog->lowLatencyChk->setChecked(p.lowLatency);
	dialog->preJuly2011DemuxCB->setChecked(p.doPreJuly2011IntanDemux);
	dialog->preJuly2011DemuxCB->setEnabled(p.mode != DAQ::AIRegular);
	double val = (p.pdThresh/32768.+1.)/2. * (p.range.max-p.range.min) + p.range.min;
	if (p.pdChanIsVirtual && !feq(p.auxGain,0.0)) {
		val = val / p.auxGain;
	}
    acqPdParams->pdAIThreshSB->setValue(val);
    acqPdParams->pdAISB->setValue(p.pdChan);
    acqPdParams->pdPassthruAOChk->setChecked(p.pdPassThruToAO > -1);
    acqPdParams->pdPassthruAOSB->setValue(p.pdPassThruToAO > -1 ? p.pdPassThruToAO : 0);   
    acqPdParams->pdStopTimeSB->setValue(p.pdStopTime);
    acqPdParams->pdPre->setValue(p.silenceBeforePD*1000.);
	acqPdParams->pdWSB->setValue(p.pdThreshW);
	acqPdParams->virtualChk->setChecked(p.pdChanIsVirtual);
	dialog->bufferSizeSlider->setValue(p.aiBufferSizeCS);

    QList<QString> devs = aiChanLists.uniqueKeys();
    devNames.clear();
    int sel = 0, sel2 = 0, i = 0;
    for (QList<QString>::const_iterator it = devs.begin(); it != devs.end(); ++it, ++i) {
        if (p.dev == *it) sel = i;
        if (p.dev2 == *it) sel2 = i;
        dialog->deviceCB->addItem(QString("%1 (%2)").arg(*it).arg(DAQ::GetProductName(*it)));
        dialog->deviceCB_2->addItem(QString("%1 (%2)").arg(*it).arg(DAQ::GetProductName(*it)));
        devNames.push_back(*it);
    }
    if ( dialog->deviceCB->count() ) // empty CB sometimes on missing AI??
        dialog->deviceCB->setCurrentIndex(sel);
    if ( dialog->deviceCB_2->count() ) // empty CB sometimes on missing AI??
        dialog->deviceCB_2->setCurrentIndex(sel2);

	const bool canHaveDualDevMode (dialog->deviceCB_2->count() > 1);
	
	if (!canHaveDualDevMode) {
		dialog->dualDevModeChk->setChecked(false);
		dialog->dualDevModeChk->setEnabled(false);
		dialog->secondIsAuxChk->setChecked(false);
		dialog->secondIsAuxChk->setEnabled(false);
		p.dualDevMode = false;
		p.secondDevIsAuxOnly = false;
	} else {
		dialog->dualDevModeChk->setChecked(p.dualDevMode);
		dialog->dualDevModeChk->setEnabled(true);
		dialog->secondIsAuxChk->setChecked(p.secondDevIsAuxOnly);
		dialog->secondIsAuxChk->setEnabled(true);
	}

	chanMapCtl.setDualDevMode(p.dualDevMode && !p.secondDevIsAuxOnly);

    resetAOPassFromParams(aoPassthru, &p);
    
	dialog->resumeGraphSettingsChk->setChecked(p.resumeGraphSettings);
    dialog->disableGraphsChk->setChecked(p.suppressGraphs);
	dialog->resumeGraphSettingsChk->setDisabled(dialog->disableGraphsChk->isChecked());
    
    // now the timed params stuff
    acqTimedParams->startHrsSB->setValue(int(p.startIn/(60.*60.)));
    acqTimedParams->startMinsSB->setValue(int((int(p.startIn)%(60*60))/60.));
    acqTimedParams->startSecsSB->setValue(int((int(p.startIn)%(60*60)))%60);
    acqTimedParams->durHrsSB->setValue(int(p.duration/(60.*60.)));
    acqTimedParams->durMinsSB->setValue(int((int(p.duration)%(60*60))/60.));
    acqTimedParams->durSecsSB->setValue((int(p.duration)%(60*60))%60);
    acqTimedParams->indefCB->setChecked(p.isIndefinite);
    acqTimedParams->nowCB->setChecked(p.isImmediate);
    
    // fire off the slots to polish?
    deviceCBChanged();
    acqStartEndCBChanged();
    acqModeCBChanged();
    dialog->clockCB->setCurrentIndex(p.extClock ? 0 : 1);
    aoDeviceCBChanged();
    aoPassthruChkd();
    aoPDChanChkd();
    aiRangeChanged();
	bufferSizeSliderChanged();
	dualDevModeChkd();

	dialog->autoRetryOnAIOverrunsChk->setChecked(p.autoRetryOnAIOverrun);

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

}

void ConfigureDialogController::aiRangeChanged()
{
    QString devStr = devNames[dialog->deviceCB->currentIndex()];
    const QList<DAQ::Range> ranges = aiDevRanges.values(devStr);
	if (!ranges.count()) {
		QMessageBox::critical(dialogW, "NI Unknown Error", "Error with your NIDAQ setup.  Please make sure all ghost/phantom/unused devices are deleted from your NI Measurement & Autiomation Explorer", QMessageBox::Abort);
        mainApp()->quit();
		return;
	}
    const DAQ::Range r = ranges[dialog->aiRangeCB->currentIndex()];
	double minn = r.min, maxx = r.max;
	if (acqPdParams->virtualChk->isChecked() && !feq(dialog->auxGainSB->value(),0.0)) {
		minn = minn / dialog->auxGainSB->value();
		maxx = maxx / dialog->auxGainSB->value();
	}
    acqPdParams->pdAIThreshSB->setMinimum(minn);
    acqPdParams->pdAIThreshSB->setMaximum(maxx);	
}

void ConfigureDialogController::acqStartEndCBChanged()
{
    bool entmp = false, aitriggered = false, vaitriggered = false;
    acqPdParamsW->hide();
    acqTimedParamsW->hide();
    DAQ::AcqStartEndMode mode = (DAQ::AcqStartEndMode)dialog->acqStartEndCB->currentIndex();
    dialog->acqStartEndDescrLbl->hide();
    
    switch (mode) {
    case DAQ::Immediate:
        dialog->acqStartEndDescrLbl->setText("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.0//EN\" \"http://www.w3.org/TR/REC-html40/strict.dtd\"><html><head><meta name=\"qrichtext\" content=\"1\" /><style type=\"text/css\">p, li { white-space: pre-wrap; }</style></head><body style=\" font-family:'Sans Serif'; font-size:9pt; font-weight:400; font-style:normal;\"><p style=\" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;\"><span style=\" font-size:10pt; font-style:italic; \">The acquisition will start immediately.</span></p></body></html>");
        dialog->acqStartEndDescrLbl->show();
        break;
	case DAQ::AITriggered:
		aitriggered = true;
    case DAQ::PDStartEnd:
        entmp = true;
    case DAQ::PDStart:
		vaitriggered = acqPdParams->virtualChk->isChecked();
        acqPdParams->pdStopTimeLbl->setEnabled(entmp);
		acqPdParams->pdAILabel->setText("AI Ch:");
		acqPdParams->pdAILabel->setToolTip(acqPdParams->pdAILabel->toolTip().replace("virtual (demuxed)","physical AI"));
	    acqPdParams->pdAISB->setToolTip(acqPdParams->pdAILabel->toolTip());
		if (aitriggered) {
			acqPdParams->pdStopTimeLbl->setText("AI stop time (sec):");
		} else {
			acqPdParams->pdStopTimeLbl->setText("PD stop time (sec):");	
		}
        acqPdParams->pdStopTimeSB->setEnabled(entmp);
        acqPdParamsW->setParent(dialog->acqFrame);
        acqPdParamsW->show();
        break;
    case DAQ::Timed:
        acqTimedParamsW->setParent(dialog->acqFrame);
        acqTimedParamsW->show();
        break;
    case DAQ::StimGLStartEnd:
        dialog->acqStartEndDescrLbl->setText("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.0//EN\" \"http://www.w3.org/TR/REC-html40/strict.dtd\"><html><head><meta name=\"qrichtext\" content=\"1\" /><style type=\"text/css\">p, li { white-space: pre-wrap; }</style></head><body style=\" font-family:'Sans Serif'; font-size:9pt; font-weight:400; font-style:normal;\"><p style=\" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;\"><span style=\" font-size:10pt; font-style:italic; color:#294928;\">The acquisition will be triggered to start and end by the external StimGL II program.</span></p></body></html>");
        dialog->acqStartEndDescrLbl->show();
        break;
    case DAQ::StimGLStart:
        dialog->acqStartEndDescrLbl->setText("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.0//EN\" \"http://www.w3.org/TR/REC-html40/strict.dtd\"><html><head><meta name=\"qrichtext\" content=\"1\" /><style type=\"text/css\">p, li { white-space: pre-wrap; }</style></head><body style=\" font-family:'Sans Serif'; font-size:9pt; font-weight:400; font-style:normal;\"><p style=\" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;\"><span style=\" font-size:10pt; font-style:italic; color:#294928;\">The acquisition will be triggered to start by the external StimGL II program.</span></p></body></html>");
        dialog->acqStartEndDescrLbl->show();
        break;			
    default:
        Error() << "INTERNAL ERROR: INVALID ACQSTARTENDMODE!  FIXME!";
        break;
    }
	
	dialog->stimGLReopenCB->setEnabled(!aitriggered);
	if (aitriggered) dialog->stimGLReopenCB->setChecked(false);
	
	aiRangeChanged();
}

void ConfigureDialogController::acqModeCBChanged()
{
    const int idx = dialog->acqModeCB->currentIndex();
    const bool jfrc32 = idx == 3;
    const bool intan = idx != 1;    
	bool notStraightAI = (idx != DAQ::AIRegular);
    
    /// HACK to auto-repopulate the lineedit based on acquisition mode!
    /*if ( (notStraightAI = (idx != DAQ::AIRegular)) ) { // if it's not straight AI, 
        QString txt = dialog->channelListLE->text();
        if (txt.startsWith("0:") && txt.length() == 3)  {
			txt = QString("0:%1").arg(DAQ::ModeNumIntans[idx] - 1);
            dialog->channelListLE->setText(txt);
        }
    }*/
	dialog->secondIsAuxChk->setEnabled(notStraightAI && dialog->dualDevModeChk->isChecked());
    dialog->channelSubsetLE->setEnabled(notStraightAI);
    dialog->channelSubsetLabel->setEnabled(notStraightAI);
    dialog->preJuly2011DemuxCB->setEnabled(notStraightAI);
    
    dialog->srateLabel->setEnabled(/*enabled*/true);
    dialog->srateSB->setEnabled(/*enabled*/true);
    dialog->clockCB->setEnabled(true); // NB: for now, the clockCB is always enabled!
    dialog->muxMapBut->setEnabled(intan);
	chanMapCtl.currentMode = (DAQ::Mode)idx;

    if (intan || jfrc32) {
        //if (!jfrc32) dialog->srateSB->setValue(INTAN_SRATE);
        dialog->clockCB->setCurrentIndex(0); // intan always uses external clock
    } else {
        dialog->clockCB->setCurrentIndex(1); // force INTERNAL clock on !intan
    }
    dialog->doCtlLabel->setEnabled(intan && !jfrc32);
    dialog->doCtlCB->setEnabled(intan && !jfrc32);
    dialog->fastSettleSB->setEnabled(intan && !jfrc32);
    dialog->fastSettleLbl->setEnabled(intan && !jfrc32);
	chanMapCtl.loadSettings();
}

void ConfigureDialogController::deviceCBChanged()
{
    if (!dialog->deviceCB->count()) return;
    QString devStr = devNames[dialog->deviceCB->currentIndex()];
    QList<DAQ::Range> ranges = aiDevRanges.values(devStr);
    QString curr = dialog->aiRangeCB->count() ? dialog->aiRangeCB->currentText() : QString::null;
    if (curr.isEmpty()) 
        // make the current selected gain be the saved params
        curr = QString("%1 - %2").arg(acceptedParams.range.min).arg(acceptedParams.range.max);
    // do control combo box setup..
    int doCtl = dialog->doCtlCB->currentIndex();
    QStringList DOS = DAQ::GetDOChans(devStr);
    if (doCtl < 0) doCtl = 0;
    dialog->doCtlCB->clear();
    
    for (int i = 0; i < DOS.count(); ++i) {
        dialog->doCtlCB->addItem(DOS.at(i).section('/',1,-1));
    }
    if (doCtl < dialog->doCtlCB->count())
        dialog->doCtlCB->setCurrentIndex(doCtl);

    int sel = 0, i = 0;
    dialog->aiRangeCB->clear();
    for (QList<DAQ::Range>::const_iterator it = ranges.begin(); it != ranges.end(); ++it, ++i) {
        const DAQ::Range & r (*it);
        QString txt = QString("%1 - %2").arg(r.min).arg(r.max);
        if (txt == curr) sel = i;
        dialog->aiRangeCB->insertItem(i, txt);
    }
    if (dialog->aiRangeCB->count())
        dialog->aiRangeCB->setCurrentIndex(sel);

    dialog->srateSB->setMinimum(DAQ::MinimumSampleRate(devStr));
    dialog->srateSB->setMaximum(DAQ::MaximumSampleRate(devStr));
}

void ConfigureDialogController::aoDeviceCBChanged()
{
    updateAORangeOnCBChange(aoPassthru);
}

void ConfigureDialogController::updateAORangeOnCBChange(Ui::AoPassThru *aoPassthru)
{
    if (!aoPassthru->aoDeviceCB->count()) return;
    int devix = aoPassthru->aoDeviceCB->currentIndex(); if (devix < 0) devix = 0;
    QString devStr = aoDevNames[devix];
    QList<DAQ::Range> ranges = aoDevRanges.values(devStr);
    QString curr = aoPassthru->aoRangeCB->currentText();

    if (!curr.length()) {
        curr = QString("%1 - %2").arg(acceptedParams.aoRange.min).arg(acceptedParams.aoRange.max);
    }
    int sel = 0, i = 0;
    aoPassthru->aoRangeCB->clear();
    for (QList<DAQ::Range>::const_iterator it = ranges.begin(); it != ranges.end(); ++it, ++i) {
        const DAQ::Range & r (*it);
        QString txt = QString("%1 - %2").arg(r.min).arg(r.max);
        if (txt == curr) sel = i;
        aoPassthru->aoRangeCB->insertItem(i, txt);
    }
    if (aoPassthru->aoRangeCB->count())
        aoPassthru->aoRangeCB->setCurrentIndex(sel);
    else {
        QString errMsg(QString("AO on device '") + devStr + "' seems to be invalid or improperly configured.  Please run NI-MAX and delete any ghost/unused devices and restart SpikeGL.");
        Warning() << errMsg;
        int but = QMessageBox::warning(0, "AO Device Invalid", errMsg, QMessageBox::Ignore|QMessageBox::Abort, QMessageBox::Abort);
        if (but != QMessageBox::Ignore) {
            mainApp()->quit();
            return;
        }
        aoPassthru->aoRangeCB->insertItem(0, "-10 - 10");
        aoPassthru->aoRangeCB->insertItem(1, "-5 - 5");
    }
}


void ConfigureDialogController::browseButClicked()
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

void ConfigureDialogController::aoPassthruChkd()
{
    if (!aoPassthru->aoDeviceCB->count()) {
        // if no AO subdevice present on the current machine, just disable all GUI related to AO and return
        acqPdParams->pdPassthruAOChk->setEnabled(false);
        acqPdParams->pdPassthruAOChk->setChecked(false); 
        acqPdParams->pdPassthruAOSB->setEnabled(false);
        aoPassthru->aoPassthruGB->setChecked(false);
        aoPassthru->aoPassthruGB->setEnabled(false);
        QString theTip = "AO missing on installed hardware, AO passthru unavailable.";
        aoPassthru->aoPassthruGB->setToolTip(theTip);
        acqPdParams->pdPassthruAOChk->setToolTip(theTip); 
        acqPdParams->pdPassthruAOSB->setToolTip(theTip);
        return;
    }
    bool chk = aoPassthru->aoPassthruGB->isChecked();
    acqPdParams->pdPassthruAOChk->setEnabled(chk);
    if (!chk && acqPdParams->pdPassthruAOChk->isChecked()) 
        acqPdParams->pdPassthruAOChk->setChecked(false);    
    acqPdParams->pdPassthruAOSB->setEnabled(chk && acqPdParams->pdPassthruAOChk->isChecked());
    aoPDPassthruUpdateLE();
}

void ConfigureDialogController::aoPDChanChkd()
{
    bool chk = acqPdParams->pdPassthruAOChk->isChecked();
    acqPdParams->pdPassthruAOSB->setEnabled(chk && aoPassthru->aoPassthruGB->isChecked() );
    aoPDPassthruUpdateLE();
}

void ConfigureDialogController::dualDevModeChkd()
{
    const bool chk (dialog->dualDevModeChk->isChecked()), straightAI = dialog->acqModeCB->currentIndex() == 1;
	dialog->deviceCB_2->setEnabled(chk);
	dialog->secondDevLbl->setEnabled(chk);
	dialog->secondIsAuxChk->setEnabled(chk && !straightAI);
	if (chk && dialog->channelListLE_2->text().length() == 0) {
		dialog->channelListLE_2->setText(dialog->channelListLE->text());
	}
	dialog->channelListLE_2->setEnabled(chk);
	chanMapCtl.setDualDevMode(!straightAI && chk && !dialog->secondIsAuxChk->isChecked());
}

void ConfigureDialogController::secondIsAuxChkd()
{
    const bool chk (dialog->secondIsAuxChk->isChecked()), straightAI = dialog->acqModeCB->currentIndex() == 1;
	chanMapCtl.setDualDevMode(!straightAI && dialog->dualDevModeChk->isChecked() && !chk);
}


void ConfigureDialogController::aoPDPassthruUpdateLE()
{
    bool aochk = aoPassthru->aoPassthruGB->isChecked(),
         pdaochk = acqPdParams->pdPassthruAOChk->isChecked();
    int aopd = pdaochk && aochk ? acqPdParams->pdPassthruAOSB->value() : -1;
    QString le = aoPassthru->aoPassthruLE->text();
    le = le.remove(QRegExp(",?\\s*\\d+\\s*=\\s*PDCHAN"));
    if (le.startsWith(",")) le = le.mid(1);
    if (aopd > -1) {
        // remove possibly already-existing AOPD=BLAH from string.. as per Leonardo email dated 7/8/2010 titled "stimGL bug testing, part II"
        le = le.remove(QRegExp(QString().sprintf(",?\\s*%d\\s*=\\s*((\\d+)|(PDCHAN))",aopd)));
        if (le.startsWith(",")) le = le.mid(1); 
        QString s = QString().sprintf("%d=PDCHAN",aopd);
        le = le + QString(le.length() ? ", " : "") + s;
    }
    aoPassthru->aoPassthruLE->setText(le);
}

/* static */ int ConfigureDialogController::setFilenameTakingIntoAccountIncrementHack(DAQ::Params & p, DAQ::AcqStartEndMode acqStartEndMode, const QString & filename, QWidget *dialogW, bool isGUI)
{
    p.outputFile = p.outputFileOrig = filename.trimmed();
	p.cludgyFilenameCounterOverride = 1;
	
	
	QString numberless;
	int number;
	if ( (acqStartEndMode != DAQ::Immediate && acqStartEndMode != DAQ::Timed) 
		&& chopNumberFromFilename(p.outputFile, numberless, number) ) {
		int q = QMessageBox::question(dialogW, 
									  QString("Resume filename counting?"), 
									  QString("The specified output file:\n\n") + p.outputFile + "\n\nis of the form FILE_XX.bin, and you also specified a triggered recording setup.\nResume filename increment?\n\nIf you say yes, `" + numberless + "' is the file count basename and filenames will auto-append a number from _" + QString::number(number) + " onward, on trigger.\n\nIf you say no, the entire filename `" + p.outputFile + "' will be taken as the basename (and triggering will append a further _1, _2, etc to filenames!).",
									  QMessageBox::Yes|QMessageBox::No,QMessageBox::Yes);
		if (q == QMessageBox::Yes) {
			p.outputFileOrig = numberless;
			//if (p.stimGlTrigResave)
			//	p.outputFile = numberless;
			p.cludgyFilenameCounterOverride = number;
		}
	}
	
	if (!p.stimGlTrigResave && isGUI) {
		// verify output file and if it exists, act appropriately
		QString outFile = p.outputFile;
		if (!QFileInfo(outFile).isAbsolute()) 
			outFile = mainApp()->outputDirectory() + "/" + outFile; 
		if (QFileInfo(outFile).exists()) {
			// output file exists, ask user if they should overwrite, cancel, or auto-rename
			QMessageBox *mb = new QMessageBox(dialogW);
			mb->setText("The specified output file already exists.");
			mb->setInformativeText("Do you want to overwrite it, auto-rename it, or go back and specify a new filename?");
			// the 'roles' below seem to only affect formatting of the buttons..
			QPushButton *cancelBut = mb->addButton("Go Back", QMessageBox::DestructiveRole);
			QPushButton *renameBut = mb->addButton("Auto-Rename", QMessageBox::RejectRole);
			QPushButton *overBut = mb->addButton("Overwrite", QMessageBox::AcceptRole);
			mb->exec();
			QAbstractButton * clicked = mb->clickedButton();
			(void)overBut;
			if (clicked == renameBut) {
				p.outputFile = mainApp()->getNewDataFileName(QString::null);
			} else if (clicked == cancelBut) {
				return AGAIN;
			}
			/// overwrite path is the normal path...
		}
	}
	return OK;
}

/** Validates the internal form.  If ok, saves form to settings as side-effect, otherwise if not OK, sets errTitle and errMsg.
 *  OK returned, form is ok, proceed -- form is saved to settings
 *  AGAIN returned, form is erroneous, but redo ok -- form is not saved, errTitle and errMsg set appropriately
 *  ABORT returned, form is erroneous, abort altogether -- form is not saved, errTitle and errMsg set appropriately
 */
ConfigureDialogController::ValidationResult ConfigureDialogController::validateForm(QString & errTitle, QString & errMsg, bool isGUI) 
{
    const QString dev = dialog->deviceCB->count() ? devNames[dialog->deviceCB->currentIndex()] : "";
    const QString dev2 = dialog->deviceCB_2->count() ? devNames[dialog->deviceCB_2->currentIndex()] : "";
    const QString aoDev = aoPassthru->aoDeviceCB->count() ? aoDevNames[aoPassthru->aoDeviceCB->currentIndex()] : "";
    const DAQ::Mode acqMode = (DAQ::Mode)dialog->acqModeCB->currentIndex();
    bool err = false;
    errTitle = errMsg = "";
	const bool dualDevMode = dialog->dualDevModeChk->isChecked(), secondDevIsAuxOnly = dialog->secondIsAuxChk->isChecked();
	const bool resumeGraphSettings = dialog->resumeGraphSettingsChk->isChecked();
    QVector<unsigned> chanVect, chanVect2;
    QString chans = parseAIChanString(dialog->channelListLE->text(), chanVect, &err);
    QString chans2 = dualDevMode ? parseAIChanString(dialog->channelListLE_2->text(), chanVect2, &err) : "";
    for (int i = 0; dev.length() && i < (int)chanVect.size(); ++i) {
        if (chanVect[i] >= static_cast<unsigned>(aiChanLists[dev].size())) {
            err = true; 
            break;
        }
    }
    if (err) {
        errTitle = "AI Chan List Error";
        errMsg = "Error parsing AI channel list!\nSpecify a string of the form 0,1,2,3 or 0-3,5,6 of valid channel id's!";
        return AGAIN;
    }
	if (dualDevMode && (!dev2.length() || !dev2.compare(dev,Qt::CaseInsensitive))) {
        errTitle = "Second AI Dev Error";
        errMsg = "Dual-device mode enabled, but the second AI device specified is invalid.\nPlease select a distinct AI device for the second AI device.";
        return AGAIN;
		
	}
	if (dualDevMode && DAQ::GetProductName(dev2).compare(DAQ::GetProductName(dev), Qt::CaseInsensitive)) {
        errTitle = "Dual Device Mode Error";
        errMsg = "Dual-device mode enabled, but both AI devices are not the same model.\nFor dual-device mode to work correctly, the second AI device must be the same exact model as the first.";
        return AGAIN;
		
	}
    for (int i = 0; dualDevMode && i < (int)chanVect2.size(); ++i) {
        if (chanVect2[i] >= static_cast<unsigned>(aiChanLists[dev2].size())) {
            err = true; 
            break;
        }
    }
    if (err) {
        errTitle = "AI Chan List Error";
        errMsg = "Error parsing 2nd AI channel list!\nSpecify a string of the form 0,1,2,3 or 0-3,5,6 of valid channel id's!";
        return AGAIN;
    }
	    
    int nExtraChans1 = 0, nExtraChans2 = 0;
    
    if ( acqMode != DAQ::AIRegular) {
        if (!DAQ::SupportsAISimultaneousSampling(dev)) {
            QString title = "INTAN Requires Simultaneous Sampling",
                    msg = QString("INTAN (60/64/96/120/128/256/JFRC32 demux) mode requires a board that supports simultaneous sampling, and %1 does not!").arg(dev);
			if (QMessageBox::Cancel == QMessageBox::warning(dialogW, title, msg, QMessageBox::Ignore, QMessageBox::Cancel)) 
				return AGAIN;
			// else continue and ignore the error...
        }
		if (dialog->clockCB->currentIndex() != 0) {
            QString title = "INTAN Requires External Clock",
			msg = QString("INTAN (60/64/96/120/128/256/JFRC32 demux) mode requires an external clock source for correct operation, yet you specified the use of the internal clock.  Ignore and use the internal clock anyway?");
			if (QMessageBox::Cancel == QMessageBox::warning(dialogW, title, msg, QMessageBox::Ignore, QMessageBox::Cancel)) 
				return AGAIN;
			// else continue and ignore the error...			
		}
        const int minChanSize = ( (acqMode == DAQ::AIRegular) ? 1 : DAQ::ModeNumIntans[acqMode]);
        if ( int(chanVect.size()) < minChanSize ) {
            errTitle = "AI Chan List Error", errMsg = QString("First AI dev chan list too short.\nSelected mode requires %1 channels!").arg(minChanSize);
            return AGAIN;
        }
        if ( dualDevMode && !secondDevIsAuxOnly && int(chanVect2.size()) < minChanSize ) {
            errTitle = "AI Chan List Error", errMsg = QString("Second AI dev chan list too short.\nSelected mode requires %1 channels!").arg(minChanSize);
            return AGAIN;
        }
        nExtraChans1 = chanVect.size() - minChanSize;
		nExtraChans2 = dualDevMode ? chanVect2.size()-minChanSize  : 0;
		if (dualDevMode && secondDevIsAuxOnly) nExtraChans2 = chanVect2.size();
    }
    
    if (!chanVect.size()) {
        errTitle = "AI Chan List Error", errMsg = "Need at least 1 channel in the AI channel list!";
        return AGAIN;
    }
    
    const double srate = dialog->srateSB->value();
    if (srate > DAQ::MaximumSampleRate(dev, chanVect.size())) {
        errTitle = "Sampling Rate Invalid", errMsg = QString().sprintf("The sample rate specified (%d) is too high for the number of channels (%d)!", (int)srate, (int)chanVect.size());
        return AGAIN;
    }
    
    const DAQ::AcqStartEndMode acqStartEndMode = (DAQ::AcqStartEndMode)dialog->acqStartEndCB->currentIndex();

    unsigned nVAI = chanVect.size() + chanVect2.size(), nVAI1 = chanVect.size(), nVAI2 = chanVect2.size();

	bool pdOnSecondDev = false;
    int pdChan = acqPdParams->pdAISB->value();
	int pdChanAdjusted = -1;
    bool usePD = acqStartEndMode == DAQ::PDStartEnd || acqStartEndMode == DAQ::PDStart || acqStartEndMode == DAQ::AITriggered;
	bool pdIsVAI = acqPdParams->virtualChk->isChecked();
	
	if (acqMode != DAQ::AIRegular) {
		nVAI1 = (nVAI1-nExtraChans1) * DAQ::ModeNumChansPerIntan[acqMode] + nExtraChans1; 
		nVAI2 = (nVAI2-nExtraChans2) * DAQ::ModeNumChansPerIntan[acqMode] + nExtraChans2; 
		nVAI = nVAI1 + nVAI2;
	}
	
    if ( usePD ) {
		if (pdIsVAI) {
			if (pdChan < 0 || pdChan >= (int)nVAI) {
				errTitle = "Trigger Channel Invalid", errMsg = QString().sprintf("Specified trigger channel (%d) is not valid. For the current setup, 0-%d are valid virtual channels.", pdChan, nVAI-1);
				return AGAIN;
			}			
        } else if (chanVect.contains(pdChan) || pdChan < 0 || pdChan >= aiChanLists[dev].count()) {
			pdChanAdjusted = pdChan-aiChanLists[dev].count();

			if (dualDevMode && pdChanAdjusted >= 0 && !chanVect.contains(pdChan) && !chanVect2.contains(pdChanAdjusted) && pdChanAdjusted < aiChanLists[dev2].size()) {
				pdOnSecondDev = true;
			} else {
				errTitle = "Trigger Channel Invalid", errMsg = QString().sprintf("Specified trigger channel (%d) is invalid or clashes with another AI channel!", pdChan);
				return AGAIN;
			}
		}

		if (!pdIsVAI) {
			if (pdOnSecondDev) {
				chanVect2.push_back(pdChanAdjusted);
				++nVAI;
				++nVAI2;
				++nExtraChans2;
			} else {
				chanVect.push_back(pdChan);
				++nVAI;
				++nVAI1;
				++nExtraChans1;
			}
		}		
    }
	    
    QMap<unsigned, unsigned> aopass;            
    if (aoPassthru->aoPassthruGB->isChecked() && aoDevNames.count()) {
        QString le = aoPassthru->aoPassthruLE->text();
		if (usePD) {
			if (pdIsVAI) {
				le=le.replace("PDCHAN", QString::number(pdChan));				
			} else {
				// pd is physical ai
				if (pdOnSecondDev) {
					le=le.replace("PDCHAN", QString::number(nVAI-1));
				} else {
					le=le.replace("PDCHAN", QString::number(nVAI1-1)); // PD channel index is always the last index
				}
			}
		}
        aopass = parseAOPassthruString(le, &err);
        if (err) {
            errTitle =  "AO Passthru Error", errMsg = "Error parsing AO Passthru list, specify a string of the form UNIQUE_AOCHAN=CHANNEL_INDEX_POS!";
            return AGAIN;
        }
    }
    
    QVector<unsigned> aoChanVect = aopass.uniqueKeys().toVector();
    
    int pdAOChan = acqPdParams->pdPassthruAOSB->value();
    
    if (!usePD || !acqPdParams->pdPassthruAOChk->isChecked()) {
        pdAOChan = -1;
    } else {
        if (!aoChanVect.contains(pdAOChan)) {
            errTitle = "AO Passthru Error", errMsg = QString().sprintf("INTERNAL ERROR: The AO channel specified for the PD (%d) does not exist in the passthru string!", (int)(pdAOChan));
            return AGAIN;
        }
    }
    	
    // validate AO channels in AO passthru map
    if (aoChanVect.size()) {
        QStringList aol = aoChanLists[aoDev];
        for (int i = 0; i < (int)aoChanVect.size(); ++i) {
            if ((int)aoChanVect[i] >= (int)aol.size()) {
                errTitle = "AO Passthru Error", errMsg = QString().sprintf("The AO channel specified in the passthru string (%d) is illegal/invalid!", (int)(aoChanVect[i]));
                return AGAIN;
            }
        }
    }
    
    {
        // validate AI channels in AO passthru map
        QList<unsigned> vl = aopass.values();
        for (QList<unsigned>::const_iterator it = vl.begin(); it != vl.end(); ++it)
            if (*it >= nVAI) {
                errTitle = "AO Passthru Invalid", errMsg = QString().sprintf("Chan index %d specified as an AO passthru source but it is > the number of virtual AI channels (%d)!", (int)*it, (int)nVAI);
                return AGAIN;
            }
    }
    
    
    if (dialog->stimGLReopenCB->isChecked() &&
        (acqStartEndMode == DAQ::StimGLStartEnd) ) {
            errTitle = "Incompatible Configuration", errMsg = QString().sprintf("'Re-Open New Save File on StimGL Experiment' not compatible with 'StimGL Plugin Start & End'");
            return AGAIN;
        }
	
    if (dialog->stimGLReopenCB->isChecked() &&
        (acqStartEndMode == DAQ::AITriggered) ) {
		errTitle = "Incompatible Configuration", errMsg = QString().sprintf("'Re-Open New Save File on StimGL Experiment' not compatible with 'TTL Controlled Start & Re-Triggered'");
		return AGAIN;
	}
    
    DAQ::Params & p (acceptedParams);
	p.bug.enabled = false;
    p.stimGlTrigResave = dialog->stimGLReopenCB->isChecked();
	if (AGAIN == setFilenameTakingIntoAccountIncrementHack(p, acqStartEndMode, dialog->outputFileLE->text(), dialogW, isGUI)) {
		errTitle = QString::null;
		errMsg = QString::null;
		return AGAIN;
	}
    p.dev = dev;
	p.dev2 = dev2;
    QStringList rngs = dialog->aiRangeCB->currentText().split(" - ");
    if (rngs.count() != 2) {
        errTitle = "AI Range ComboBox invalid!";
        errMsg = "INTERNAL ERROR: AI Range ComboBox needs numbers of the form REAL - REAL!";
        Error() << errMsg;
        return ABORT;
    }
    p.range.min = rngs.first().toDouble();
    p.range.max = rngs.last().toDouble();
	p.customRanges.clear();
	p.chanDisplayNames.clear();
    p.mode = acqMode;
    p.srate = srate;
	p.aoSrate = aoPassthru->srateSB->value();
	p.aoClock = aoPassthru->aoClockCB->currentText();
	p.aoBufferSizeCS = aoPassthru->bufferSizeSlider->value();
    p.extClock = dialog->clockCB->currentIndex() == 0;
    p.aiChannels = chanVect;
	p.dualDevMode = dualDevMode;
	p.secondDevIsAuxOnly = p.dualDevMode && secondDevIsAuxOnly;
	p.aiChannels2 = chanVect2;
    p.nVAIChans = nVAI;
	p.nVAIChans1 = nVAI1;
	p.nVAIChans2 = nVAI2;
    p.nExtraChans1 = nExtraChans1;
	p.nExtraChans2 = nExtraChans2;
    p.aiString = chans;
	p.aiString2 = chans2;
    p.doCtlChan = dialog->doCtlCB->currentIndex();
    p.doCtlChanString = QString("%1/%2").arg(p.dev).arg(dialog->doCtlCB->currentText());
    p.usePD = usePD;
	p.pdChanIsVirtual = pdIsVAI;
	p.lowLatency = dialog->lowLatencyChk->isChecked();
	p.doPreJuly2011IntanDemux = dialog->preJuly2011DemuxCB->isChecked();
	p.aiBufferSizeCS = dialog->bufferSizeSlider->value(); if (p.aiBufferSizeCS > 100) p.aiBufferSizeCS = 100;
    if (!aoDevNames.empty()) {                
        p.aoPassthru = aoPassthru->aoPassthruGB->isChecked();
        p.aoDev = aoDev;
        rngs = aoPassthru->aoRangeCB->currentText().split(" - ");
        if (rngs.count() != 2) {
            errTitle = "AO Range ComboBox invalid!";
            errMsg = "INTERNAL ERROR: AO Range ComboBox needs numbers of the form REAL - REAL!";
            Error() << errMsg;
            return ABORT;
        } 
        p.aoRange.min = rngs.first().toDouble();
        p.aoRange.max = rngs.last().toDouble();
        p.aoPassthruMap = aopass;
        p.aoChannels = aoChanVect;
        p.aoPassthruString = aoPassthru->aoPassthruLE->text();
    } else {
        p.aoPassthruMap.clear();
        p.aoChannels.clear();
        p.aoPassthruString = "";
        p.aoDev = "";
        p.aoPassthru = false;
    }
    p.suppressGraphs = dialog->disableGraphsChk->isChecked();
    
    p.acqStartEndMode = acqStartEndMode;
    p.isIndefinite = acqTimedParams->indefCB->isChecked();
    p.isImmediate = acqTimedParams->nowCB->isChecked();
    p.startIn = acqTimedParams->startHrsSB->value()*60.*60. 
    +acqTimedParams->startMinsSB->value()*60
    +acqTimedParams->startSecsSB->value();
    p.duration = acqTimedParams->durHrsSB->value()*60.*60. 
    +acqTimedParams->durMinsSB->value()*60
    +acqTimedParams->durSecsSB->value();
    
    p.pdChan = pdChan;
	p.pdOnSecondDev = pdOnSecondDev;
	if (pdIsVAI) {
		p.idxOfPdChan = p.pdChan;
	} else {
		if (pdOnSecondDev) {
			p.idxOfPdChan = p.nVAIChans-1;
		} else {
			p.idxOfPdChan = p.nVAIChans1-1 /* always the last index in first dev */;
		}
	}
	double threshGainFactor = ( (acqMode != DAQ::AIRegular && pdIsVAI) ? dialog->auxGainSB->value() : 1.0 );
    p.pdThresh = static_cast<signed short>(((acqPdParams->pdAIThreshSB->value()*threshGainFactor-p.range.min)/(p.range.max-p.range.min)) * 65535. - 32768.);
	
	p.pdThreshW = static_cast<unsigned>(acqPdParams->pdWSB->value());
    p.pdPassThruToAO = pdAOChan;
    p.pdStopTime = acqPdParams->pdStopTimeSB->value();
    
    p.aiTerm = DAQ::StringToTermConfig(dialog->aiTerminationCB->currentText());
    p.fastSettleTimeMS = dialog->fastSettleSB->value();
    p.auxGain = dialog->auxGainSB->value();
    p.chanMap = chanMapCtl.currentMapping();
    if (p.doPreJuly2011IntanDemux && p.mode != DAQ::AIRegular) {
		p.chanMap.scrambleToPreJuly2011Demux();
	}
    p.silenceBeforePD = acqPdParams->pdPre->value()/1000.;
    
    QVector<unsigned> subsetChans;
    QString subsetString = dialog->channelSubsetLE->text().trimmed();	
    if (!subsetString.size() || acqMode == DAQ::AIRegular) subsetString = "ALL";
	const bool hasAllSubset =  !subsetString.compare("ALL", Qt::CaseInsensitive) || subsetString == "*";
    if (true/*p.mode != DAQ::AIRegular*/) {
        subsetString = parseAIChanString(subsetString, subsetChans, &err, true);
        if (err && !hasAllSubset) {
            errTitle = "Channel subset error.";
            errMsg = "The channel subset is incorrectly defined.";
            return AGAIN;
        }
    }
    p.demuxedBitMap.resize(p.nVAIChans);
	if ((!subsetChans.count() && hasAllSubset) /*|| p.mode == DAQ::AIRegular*/)
			 p.demuxedBitMap.fill(true);
    else {
        p.demuxedBitMap.fill(false);
        for (QVector<unsigned>::iterator it = subsetChans.begin(); it != subsetChans.end(); ++it) {
            const unsigned n = *it;
            if (int(n) >= p.demuxedBitMap.size()) {
                errTitle = "Channel subset error.";
                errMsg = "The channel subset is incorrectly defined as it contains out-of-range channel id(s).";
                return AGAIN;
            }
            p.demuxedBitMap.setBit(n);
        }
    }
    p.subsetString = dialog->channelSubsetLE->text();  
    const unsigned nVAIChansForSave = p.demuxedBitMap.count(true);
	if (!nVAIChansForSave) {
		errTitle = "Channel subset empty error.";
		errMsg = "Cowardly refusing to save a file with an empty channel-save set!\nSpecify at least 1 channel for the channel subset!";
		return AGAIN;
	}
    QString debugStr = "";
    for (int i = 0; i < p.demuxedBitMap.count(); ++i) {
        debugStr += QString::number(int(p.demuxedBitMap.at(i))) + " ";
        //if (i && !(i % 9)) debugStr += "\n";
    }
    Debug() << "Channel subset bitmap: " << debugStr;

	p.resumeGraphSettings = resumeGraphSettings;
	p.autoRetryOnAIOverrun = dialog->autoRetryOnAIOverrunsChk->isChecked();
    p.overrideGraphsPerTab = dialog->graphsPerTabCB->currentText().toUInt();
	
	for (unsigned num = 0; num < p.nVAIChans; ++num) {
		QString chStr;
		if (p.mode == DAQ::AIRegular) {
			chStr.sprintf("AI%d", num);
		} else { // MUX mode
			if (p.isAuxChan(num)) {
				chStr.sprintf("AUX%d",int(num-(p.nVAIChans-(p.nExtraChans1+p.nExtraChans2))+1));
			} else {
				const ChanMapDesc & desc = p.chanMap[num];
				chStr.sprintf("%d [I%u_C%u elec:%u]",num,desc.intan,desc.intanCh,desc.electrodeId);        
			}
		}
		p.chanDisplayNames.push_back(chStr);
	}
	
    saveSettings();       
    
    return OK;
}

int ConfigureDialogController::exec()
{

	if (!aiDevRanges.count()) {
       QMessageBox::critical(dialogW, "NIDAQ Setup Error", "Something's wrong with your NI-DAQ hardware setup.  Make sure to delete unused devices in the NIDAQ Explorer and try again!");
	   return QDialog::Rejected;
	}

    bool again;
    int ret;

    if ( aoPassthru->aoPassthruGB->parent() != dialog->aoPassthruWidget )
        aoPassthru->aoPassthruGB->setParent(dialog->aoPassthruWidget);

    resetFromParams();

    do {

        // make sure these are gone..
        delete aoPassW, aoPassW = 0;
        delete applyDlg, applyDlg = 0;

        ret = ((QDialog *)dialogW)->exec();
		if (helpWindow) delete helpWindow, helpWindow = 0;
        again = false;

   
        if (ret == QDialog::Accepted) {
            QString errTitle, errMsg;
            ValidationResult result = validateForm(errTitle, errMsg, true);
            
            switch ( result ) {
                case AGAIN: 
					if (errTitle.length() && errMsg.length())
						QMessageBox::critical(dialogW, errTitle, errMsg);
                    again = true; 
                    break;
                case ABORT: 
                    again = false; 
                    return QDialog::Rejected;
                case OK: 
                    again = false; 
                    break;
            }
        }
    } while (again);

    return ret;
}

/*static*/ 
QString
ConfigureDialogController::parseAIChanString(const QString & str, 
                                             QVector<unsigned> & aiChannels,
                                             bool *parse_error,
                                             bool emptyOk) 
{
    QString ret("");
    QVector<unsigned> aiChans;
    QSet<unsigned> aiSet;

    QStringList vals = str.split(",", QString::SkipEmptyParts);
    for (QStringList::iterator it = vals.begin(); it != vals.end(); ++it) {
        QString v = (*it).trimmed();
        QStringList ranges = v.split(":", QString::SkipEmptyParts);
        if (ranges.count() == 2) {
            bool ok1, ok2;
            unsigned f = ranges.first().toUInt(&ok1), l = ranges.last().toUInt(&ok2);
            if (!ok1 || !ok2 || f >= l) {
                if (parse_error) *parse_error = true;
                return QString();                
            }
            for (unsigned i = f; i <= l; ++i) {
                if (aiSet.contains(i)) { // parse error! dupe chan!
                    Error() << "parseAIChanString: duplicate channel in channel list: " << i << "!";
                    if (parse_error) *parse_error = true;
                    return QString();                                    
                }
                aiChans.push_back(i);
                aiSet.insert(i);
            }
            
            ret.append(QString().sprintf("%s%d:%d", (ret.length()?",":""), f, l));
        } else if (ranges.count() == 1) {
            bool ok;
            unsigned i = ranges.first().toUInt(&ok);
            if (!ok) {
                if (parse_error) *parse_error = true;
                return QString();                
            }
            if (aiSet.contains(i)) { // parse error! dupe chan!
                Error() << "parseAIChanString: duplicate channel in channel list: " << i << "!";
                if (parse_error) *parse_error = true;
                return QString();                                    
            }
            aiChans.push_back(i);
            aiSet.insert(i);
            ret.append(QString().sprintf("%s%d", (ret.length()?",":""), i));
        } else {
            if (parse_error) *parse_error = true;
            return QString();                
        }
    }    
    if (!emptyOk && !ret.length() && parse_error) *parse_error = true;
    else if (parse_error) *parse_error = false;
    aiChannels = aiChans;
    return ret;
}
 
/*static*/
QMap<unsigned,unsigned> 
ConfigureDialogController::parseAOPassthruString(const QString & str,
                                                 bool *parse_error)
{
    QMap<unsigned,unsigned> ret;

    QStringList vals = str.split(",", QString::SkipEmptyParts);
    for (QStringList::iterator it = vals.begin(); it != vals.end(); ++it) {
        QString v = (*it).trimmed();
        QStringList nvp = v.split("=", QString::SkipEmptyParts);
        if (nvp.count() == 2) {
            bool ok1, ok2;
            unsigned n = nvp.first().toUInt(&ok1), v = nvp.last().toUInt(&ok2);
            if (!ok1 || !ok2) {
                if (parse_error) *parse_error = true;
                return QMap<unsigned,unsigned>();                
            }
            if (!ret.contains(n))
                ret.insert(n,v);
            else {
                if (parse_error) *parse_error = true;
                return QMap<unsigned,unsigned>();
            }
        } else {
            if (parse_error) *parse_error = true;
            return QMap<unsigned,unsigned>();
        }
    }
    if (ret.empty() && parse_error) *parse_error = true;
    else if (parse_error) *parse_error = false;
    return ret;
}

/*static*/ 
void ConfigureDialogController::paramsFromSettingsObject(DAQ::Params & p, const QSettings & settings)
{
    p.outputFile = settings.value("outputFile", "data.bin").toString();
    p.dev = settings.value("dev", "").toString();
    p.stimGlTrigResave = settings.value("stimGlTrigResave", false).toBool();
    p.doCtlChan = settings.value("doCtlChan", "0").toUInt();
    p.range.min = settings.value("rangeMin", -2.5).toDouble();
    p.range.max = settings.value("rangeMax", 2.5).toDouble();
    p.mode = (DAQ::Mode)settings.value("acqMode", 0).toInt();
    p.srate = settings.value("srate", INTAN_SRATE).toDouble();
    p.extClock = settings.value("extClock", true).toBool();
    p.aiString = settings.value("aiString", "0:3").toString();
    p.subsetString = settings.value("subsetString", "ALL").toString();
    p.aoPassthru = settings.value("aoPassthru", false).toBool();
    p.aoPassthruString = settings.value("aoPassthruString", "0=1,1=2").toString();
    p.aoRange.min = settings.value("aoRangeMin", -2.5).toDouble();
    p.aoRange.max = settings.value("aoRangeMax", 2.5).toDouble();
    
    p.aoDev = settings.value("aoDev", "").toString();
    p.suppressGraphs = settings.value("suppressGraphs", false).toBool();
    
    p.acqStartEndMode =  (DAQ::AcqStartEndMode)settings.value("acqStartEndMode", 0).toInt();
    p.isImmediate = settings.value("acqStartTimedImmed", false).toBool();
    p.isIndefinite = settings.value("acqStartTimedIndef", false).toBool();
    p.startIn = settings.value("acqStartTimedTime", 0.0).toDouble();
    p.duration = settings.value("acqStartTimedDuration", 60.0).toDouble();
    
    p.pdThresh = settings.value("acqPDThresh", 48000-32768).toInt();
    p.pdChan = settings.value("acqPDChan", 4).toInt();
    p.pdChanIsVirtual = settings.value("pdChanIsVirtual", false).toBool();
    p.pdStopTime = settings.value("acqPDOffStopTime", .5).toDouble();
    p.pdPassThruToAO = settings.value("acqPDPassthruChanAO", 2).toInt();
	p.pdThreshW = settings.value("acqPDThreshW", 5).toUInt();
    
    p.aiTerm = (DAQ::TermConfig)settings.value("aiTermConfig", (int)DAQ::Default).toInt();
    p.fastSettleTimeMS = settings.value("fastSettleTimeMS", DEFAULT_FAST_SETTLE_TIME_MS).toUInt();
    p.auxGain = settings.value("auxGain", 200.0).toDouble();    
    
    p.silenceBeforePD = settings.value("silenceBeforePD", DEFAULT_PD_SILENCE).toDouble();
	
	p.lowLatency = settings.value("lowLatency", false).toBool();
	
	p.doPreJuly2011IntanDemux = settings.value("doPreJuly2011IntanDemux", false).toBool();

	p.aiBufferSizeCS = settings.value("aiBufferSizeCentiSeconds", DEF_AI_BUFFER_SIZE_CENTISECONDS).toUInt();
	p.dualDevMode = settings.value("dualDevMode", false).toBool();
	p.dev2 = settings.value("dev2", "").toString();
	p.aiString2 = settings.value("aiString2", "0:3").toString();
	p.pdOnSecondDev = settings.value("pdOnSecondDev", false).toBool();

	p.aoSrate = settings.value("aoSrate", p.srate).toDouble();
	p.aoClock = settings.value("aoClock", "OnboardClock").toString();
	p.aoBufferSizeCS = settings.value("aoBufferSizeCentiSeconds", DEF_AO_BUFFER_SIZE_CENTISECONDS).toUInt();
	p.secondDevIsAuxOnly = settings.value("secondDevIsAuxOnly", false).toBool();
	p.resumeGraphSettings = settings.value("resumeGraphSettings", true).toBool();
	p.autoRetryOnAIOverrun = settings.value("autoRetryOnAIOverrun", true).toBool();
    p.overrideGraphsPerTab = settings.value("overrideGraphsPerTab", 0).toUInt();
    p.graphUpdateRate = settings.value("graphUpdateRate", DEF_TASK_READ_FREQ_HZ).toInt();
    p.spatialVisUpdateRate = settings.value("spatialVisUpdateRate", DEF_TASK_READ_FREQ_HZ).toInt();

	
	p.bug.rate = settings.value("bug_rate", 2).toUInt();
	p.bug.ttlTrig = settings.value("bug_ttlTrig", -1).toInt();
    p.bug.auxTrig = settings.value("bug_auxTrig", -1).toInt();
    p.bug.aiTrig = settings.value("bug_aiTrig_v2", "").toString();
    p.bug.whichTTLs = settings.value("bug_whichTTLs", 0).toInt();
	p.bug.clockEdge = settings.value("bug_clockEdge", 0).toInt();
	p.bug.hpf = settings.value("bug_hpf", 0).toInt();
	p.bug.snf = settings.value("bug_snf", false).toBool();	
	p.bug.errTol = settings.value("bug_errTol", 6).toInt();
    p.bug.aoPassthruString = settings.value("bug_aoPassthruString", "0=0").toString();
    p.bug.aoSrate = settings.value("bug_aoSrate", DAQ::BugTask::SamplingRate).toUInt();
    p.bug.altTTL = settings.value("bug_altTTL", true).toBool();
    p.bug.trigThreshV = settings.value("bug_trigThreshV", 3.0).toDouble();
    p.bug.aiChans = settings.value("bug_aiChans",QStringList()).toStringList();
    p.bug.aiDownsampleFactor = settings.value("bug_ai_downsample_factor",1.0).toDouble();
    p.bug.graphBadData = settings.value("bug_graphBad", false).toBool();
    p.bug.backupTrigger = settings.value("bug_backuptrigger", -1).toInt();
    p.bug.aithold = settings.value("bug_aithold", 1.0).toDouble();
    p.bug.guiAIRange = settings.value("bug_guiAIRange", "-5-5 V").toString();


    p.fg.baud = settings.value("fg_baud", 1).toInt();
    p.fg.com = settings.value("fg_com", 1).toInt();
    p.fg.bits = settings.value("fg_bits", 0).toInt();
    p.fg.stop = settings.value("fg_stop", 0).toInt();
    p.fg.parity = settings.value("fg_parity", 0).toInt();
    p.fg.sidx = settings.value("fg_sidx", 1).toInt();
    p.fg.ridx = settings.value("fg_ridx", 0).toInt();
    p.fg.disableChanMap = settings.value("fg_no_chan_map", false).toBool();
    p.fg.chanMapText = settings.value("fg_chanmap_txt", "").toString();
}

void ConfigureDialogController::loadSettings()
{
    DAQ::Params & p(acceptedParams);

    QSettings settings(SETTINGS_DOMAIN, SETTINGS_APP);

    settings.beginGroup(SETTINGS_GROUP);

    paramsFromSettingsObject(p, settings);
    
    settings.endGroup();

}

void ConfigureDialogController::saveSettings(int sc) const
{
    const DAQ::Params & p(acceptedParams);

    QSettings settings(SETTINGS_DOMAIN, SETTINGS_APP);

    settings.beginGroup(SETTINGS_GROUP);
    
    if (sc & BASE) {
        settings.setValue("outputFile", /*QFileInfo(*/p.outputFile/*).fileName()*/);
        //QString path = QFileInfo(p.outputFile).path();
        //mainApp()->setOutputDirectory(path.length() ? path : QString(PATH_SEPARATOR));
        settings.setValue("dev", p.dev);
        settings.setValue("stimGlTrigResave", p.stimGlTrigResave);

        settings.setValue("rangeMin", p.range.min);
        settings.setValue("rangeMax", p.range.max);
        settings.setValue("acqMode", (int)p.mode);
        settings.setValue("doCtlChan", p.doCtlChan);
        settings.setValue("srate", p.srate);
        settings.setValue("extClock", p.extClock);
        settings.setValue("aiString", p.aiString);
        settings.setValue("subsetString", p.subsetString);
        settings.setValue("aoDev", p.aoDev);
        settings.setValue("aoPassthru", p.aoPassthru);
        settings.setValue("aoRangeMin", p.aoRange.min);
        settings.setValue("aoRangeMax", p.aoRange.max);
        settings.setValue("aoPassthruString", p.aoPassthruString);
        settings.setValue("suppressGraphs", p.suppressGraphs);

        settings.setValue("acqStartEndMode", (int)p.acqStartEndMode);
        settings.setValue("acqStartTimedImmed", p.isImmediate);
        settings.setValue("acqStartTimedIndef", p.isIndefinite);
        settings.setValue("acqStartTimedTime", p.startIn);
        settings.setValue("acqStartTimedDuration", p.duration);

        settings.setValue("acqPDThresh", p.pdThresh);
        settings.setValue("acqPDChan", p.pdChan);
        settings.setValue("pdChanIsVirtual", p.pdChanIsVirtual);
        settings.setValue("acqPDPassthruChanAO", p.pdPassThruToAO);
        settings.setValue("acqPDOffStopTime", p.pdStopTime);
        settings.setValue("acqPDThreshW", p.pdThreshW);
        settings.setValue("aiTermConfig", (int)p.aiTerm);
        settings.setValue("fastSettleTimeMS", p.fastSettleTimeMS);
        settings.setValue("auxGain", p.auxGain);

        settings.setValue("silenceBeforePD", p.silenceBeforePD);

        settings.setValue("lowLatency", p.lowLatency);
        settings.setValue("doPreJuly2011IntanDemux", p.doPreJuly2011IntanDemux);
        settings.setValue("aiBufferSizeCentiSeconds", p.aiBufferSizeCS);
        settings.setValue("dualDevMode", p.dualDevMode);
        settings.setValue("dev2", p.dev2);
        settings.setValue("aiString2", p.aiString2);

        settings.setValue("aoSrate", p.aoSrate);
        settings.setValue("aoClock", p.aoClock);
        settings.setValue("aoBufferSizeCentiSeconds", p.aoBufferSizeCS);

        settings.setValue("secondDevIsAuxOnly", p.secondDevIsAuxOnly);
        settings.setValue("pdOnSecondDev", p.pdOnSecondDev);
        settings.setValue("resumeGraphSettings", p.resumeGraphSettings);
        settings.setValue("autoRetryOnAIOverrun", p.autoRetryOnAIOverrun);
        settings.setValue("overrideGraphsPerTab", p.overrideGraphsPerTab);
        settings.setValue("graphUpdateRate", p.graphUpdateRate);
        settings.setValue("spatialVisUpdateRate", p.spatialVisUpdateRate);
    }

    if (sc & BUG) {
        settings.setValue("bug_rate", p.bug.rate);
        settings.setValue("bug_ttlTrig", p.bug.ttlTrig);
        settings.setValue("bug_auxTrig", p.bug.auxTrig);
        settings.setValue("bug_aiTrig_v2", p.bug.aiTrig);
        settings.setValue("bug_whichTTLs", p.bug.whichTTLs);
        settings.setValue("bug_clockEdge", p.bug.clockEdge);
        settings.setValue("bug_hpf", p.bug.hpf);
        settings.setValue("bug_snf", p.bug.snf);
        settings.setValue("bug_errTol", p.bug.errTol);
        settings.setValue("bug_aoPassthruString", p.bug.aoPassthruString);
        settings.setValue("bug_aoSrate", p.bug.aoSrate);
        settings.setValue("bug_altTTL", p.bug.altTTL);
        settings.setValue("bug_trigThreshV", p.bug.trigThreshV);
        settings.setValue("bug_aiChans", p.bug.aiChans);
        settings.setValue("bug_ai_downsample_factor", p.bug.aiDownsampleFactor);
        settings.setValue("bug_graphBad", p.bug.graphBadData);
        settings.setValue("bug_backuptrigger", p.bug.backupTrigger);
        settings.setValue("bug_aithold", p.bug.aithold);
        settings.setValue("bug_guiAIRange", p.bug.guiAIRange);

    }

    if (sc & FG) {
        settings.setValue("fg_baud", p.fg.baud);
        settings.setValue("fg_com", p.fg.com);
        settings.setValue("fg_bits", p.fg.bits);
        settings.setValue("fg_stop", p.fg.stop);
        settings.setValue("fg_parity", p.fg.parity);
        settings.setValue("fg_sidx", p.fg.sidx);
        settings.setValue("fg_ridx", p.fg.ridx);
        settings.setValue("fg_no_chan_map", p.fg.disableChanMap);
        settings.setValue("fg_chanmap_txt", p.fg.chanMapText);
    }

	settings.endGroup(); 
}

QString ConfigureDialogController::acqParamsToString() 
{    
    loadSettings(); // so that we have fresh settings CREATED (otherwise on first run this would be empty!!)
    saveSettings();
    
    QString out;
    QTextStream ts(&out, QIODevice::WriteOnly);
    
    QSettings settings(SETTINGS_DOMAIN, SETTINGS_APP);
    
    settings.beginGroup(SETTINGS_GROUP);    
    
    QStringList keys (settings.childKeys());
    for (QStringList::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        ts << *it << " = " << settings.value(*it).toString() << "\n";
        //Debug() << "sending: " << *it << " = " << settings.value(*it).toString();
    }
    ts.flush();
    return out;
}

QString ConfigureDialogController::acqParamsFromString(const QString & str)
{
    QString err (QString::null);
    
    QSettings settings(SETTINGS_DOMAIN, SETTINGS_APP);
    settings.beginGroup("TempAcqParamsFromMatlab");
    QTextStream ts(str.toUtf8());
    QString line;
    
    while (!(line = ts.readLine(65536)).isNull()) {
        int eq = line.indexOf("=");
        if (eq > -1 && eq < line.length()) {
            QString n = line.left(eq).trimmed(), v = line.mid(eq+1).trimmed();
            settings.setValue(n, v);
            //Debug() << "Settings set value: " << n << "=" << v;
        }
    }
    
    DAQ::Params p;
    paramsFromSettingsObject(p, settings);
    
    resetFromParams(&p); // resets the dialog box form from the params
    
    QString errTitle, errMsg;
    ValidationResult result = validateForm(errTitle, errMsg);
    
    if (result != OK) err = errTitle + " " + errMsg.replace("\n", " ");
    
    return err;
}

void ConfigureDialogController::showAOPassThruDlg()
{
    if (!aoPassW) {
        aoPassW = new QWidget(0);
        applyDlg = new Ui::ApplyDialog;
        applyDlg->setupUi(aoPassW);
        Connect(applyDlg->applyButton, SIGNAL(clicked()), this, SLOT(applyAOPass()));
    }
    aoPassW->setWindowTitle("Re-Specify AO Passthru");
    QRect r = mainApp()->console()->frameGeometry();
    aoPassW->move(r.center().x()-aoPassW->width()/2, r.center().y()-aoPassW->height()/2);    
    if (aoPassthru->aoPassthruGB->parent() != applyDlg->widget)
        aoPassthru->aoPassthruGB->setParent(applyDlg->widget);
    resetAOPassFromParams(aoPassthru);
    aoPassW->show();
}

void ConfigureDialogController::applyAOPass()
{
    DAQ::Params & p (acceptedParams);
    if (/*!aoPassthru->aoPassthruGB->isChecked()*/!p.aoPassthru) {
        QMessageBox::information(0, "AO Passthru Disabled", "AO Passthru spec cannot be modified as it was disabled at acquisition start.");
        resetAOPassFromParams(aoPassthru);
        return;
    }
    aoPassthru->aoPassthruGB->setCheckable(false);
    bool hadPdChan = aoPassthru->aoPassthruLE->text().contains("PDCHAN");
    bool err;

    aoDeviceCBChanged();

    Ui::AoPassThru & aop(*aoPassthru);

    if ((!aop.aoPassthruLE->text().contains("PDCHAN") && hadPdChan)) {
        QMessageBox::critical(0, "PDCHAN missing", "PD Passthru was enabled at startup, but the PDCHAN is missing from the chan list!");
        resetAOPassFromParams(aoPassthru);
        return;            
    }
    if ((aop.aoPassthruLE->text().contains("PDCHAN") && !hadPdChan) ) {
        QMessageBox::critical(0, "PD not enabled", "PDCHAN is in the chanlist, but PD was not enabled at ACQ startup!");
        resetAOPassFromParams(aoPassthru);
        return;            
    }
    const QString aoDev = aop.aoDeviceCB->count() ? aoDevNames[aop.aoDeviceCB->currentIndex()] : "";
    QMap<unsigned, unsigned> aopass;            
	const unsigned nExtraChans2 = p.nExtraChans2, nVAI = p.nVAIChans;
    bool mux = false;
	int num_chans_per_intan = 1;
    if ( (mux = (p.mode != DAQ::AIRegular)) ) {
		num_chans_per_intan = DAQ::ModeNumChansPerIntan[p.mode];
    }
        
    if (aoDevNames.count()) {
        QString le = aop.aoPassthruLE->text();
		if (p.usePD) {
			if (p.pdOnSecondDev) {
				le=le.replace("PDCHAN", /*QString::number(pdChan)*/QString::number(nVAI-1)); // PD channel index is always the last index in this case
			} else {
				le=le.replace("PDCHAN", /*QString::number(pdChan)*/QString::number(nVAI-(nExtraChans2+1))); // PD channel index is always the last index in first device, in this case
			}
		}
        aopass = parseAOPassthruString(le, &err);
        if (err) {
            QMessageBox::critical(0, "AO Passthru Error", "Error parsing AO Passthru list, specify a string of the form UNIQUE_AOCHAN=CHANNEL_INDEX_POS!");
            resetAOPassFromParams(aoPassthru);
            return;            
        }
    }
    QVector<unsigned> aoChanVect = aopass.uniqueKeys().toVector();
    if (aoChanVect.size()) {
        QStringList aol = aoChanLists[aoDev];
        bool again = false;
        for (int i = 0; i < (int)aoChanVect.size(); ++i) {
            if ((int)aoChanVect[i] >= (int)aol.size()) {
                QMessageBox::critical(0, "AO Passthru Error", QString().sprintf("The AO channel specified in the passthru string (%d) is illegal/invalid!", (int)(aoChanVect[i])));
                again = true;
                break;
            }
        }
        if (again) {
            resetAOPassFromParams(aoPassthru);
            return;            
        }
    } else {
        QMessageBox::critical(0, "AO Passthru Error", QString().sprintf("Need at least 1 AO=CHAN_INDEX spec in passthru string since passthru was enabled at startup!"));
        resetAOPassFromParams(aoPassthru);
        return;            
    }
    {
        // validate AI channels in AO passthru map
        QList<unsigned> vl = aopass.values();
        bool again = false;
        for (QList<unsigned>::const_iterator it = vl.begin(); it != vl.end(); ++it)
            if (*it >= nVAI) {
                QMessageBox::critical(dialogW, "AO Passthru Invalid", QString().sprintf("Chan index %d specified as an AO passthru source but it is > the number of virtual AI channels (%d)!", (int)*it, (int)nVAI));
                again = true;
                break;
            }
        if (again) {
            resetAOPassFromParams(aoPassthru);
            return;            
        }
    }
        
    QStringList rngs = aop.aoRangeCB->currentText().split(" - ");
    if (rngs.count() != 2) {
        Error() << "INTERNAL ERROR: AO Range ComboBox needs numbers of the form REAL - REAL!";
        resetAOPassFromParams(aoPassthru);
        return;                    
    }            
    p.lock();
    p.aoDev = aoDev;
    p.aoRange.min = rngs.first().toDouble();
    p.aoRange.max = rngs.last().toDouble();
    p.aoPassthruMap = aopass;
    p.aoChannels = aoChanVect;
    p.aoPassthruString = aop.aoPassthruLE->text();
	p.aoClock = aop.aoClockCB->currentText();
	p.aoBufferSizeCS = aop.bufferSizeSlider->value();
	p.aoSrate = aop.srateSB->value();
    p.unlock();
    saveSettings();
    Log() << "Applied new AO Passthru settings";
}

/*static */
QString ConfigureDialogController::generateAIChanString(const QVector<unsigned> & chans)
{
	QString ret = "";
	const int n = chans.count();
	int diff = 2, lastdiff = 2, last = -10, runct = 0;
	for (int i = 0; i < n; ++i) {
		const int c = chans[i];
		if (i > 0) {
			diff = c - last;
		}
		if (diff == 1) {
			// we're in a run here.. do nothing
			++runct;
		} else {
			if (runct) ret.append(QString(":%1").arg(last));
			runct = 0;
			if (i) ret.append(",");
			ret.append(QString("%1").arg(c));
		}
		lastdiff = diff;
		last = c;
	}
	if (runct) ret.append(QString(":%1").arg(last));
	return ret;
}

void ConfigureDialogController::bufferSizeSliderChanged()
{
	dialog->aiBufSzLbl->setText(QString("%1 ms").arg(dialog->bufferSizeSlider->value()*10));
	const bool en = !dialog->lowLatencyChk->isChecked();
	dialog->aiBufSzLbl->setEnabled(en);
	dialog->bufferSizeSlider->setEnabled(en);
	dialog->aiBufferSizeLbl->setEnabled(en);
}

void ConfigureDialogController::aoBufferSizeSliderChanged()
{
    updateAOBufferSizeLabel(aoPassthru);
}

void ConfigureDialogController::updateAOBufferSizeLabel(Ui::AoPassThru *aopass)
{
    aopass->bufSzLbl->setText(QString("%1 ms").arg(aopass->bufferSizeSlider->value()*10));
}

/* static */
bool ConfigureDialogController::chopNumberFromFilename(const QString & filename, QString & numberless, int & number)
{
	QRegExp re("^(.*)_([0-9]+)[.]([bB][iI][nN])$");
	if (re.indexIn(filename) == 0) {
		numberless = re.cap(1) + "." + re.cap(3);
		number = re.cap(2).toInt();
		return true;
	}
	return false;
}


void ConfigureDialogController::aoNote()
{
	bool createNew = !helpWindow;

    if (createNew) {
        Ui::TextBrowser tb;
        helpWindow = new HelpWindow(dialogW);
        tb.setupUi(helpWindow);
        helpWindow->setWindowTitle("AO Setup Note");
        tb.textBrowser->setSearchPaths(QStringList("qrc:/"));
        tb.textBrowser->setSource(QUrl("qrc:/AO-Note.html"));
    }
	helpWindow->show();
	helpWindow->activateWindow();
    if (createNew) helpWindow->setMaximumSize(helpWindow->size());
}

QString ConfigureDialogController::getAODevName(Ui::AoPassThru *ao)
{
    return ao->aoDeviceCB->count() ? aoDevNames[ao->aoDeviceCB->currentIndex()] : "";
}

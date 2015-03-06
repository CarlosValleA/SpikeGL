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

FG_ConfigDialog::FG_ConfigDialog(DAQ::Params & params, QObject *parent)
: QObject(parent), acceptedParams(params)
{
	dialogW = new QDialog(0);
	dialogW->setAttribute(Qt::WA_DeleteOnClose, false);
	dialog = new Ui::FG_ConfigDialog;
    dialog->setupUi(dialogW);
}

FG_ConfigDialog::~FG_ConfigDialog()
{
	delete dialogW; dialogW = 0;
	delete dialog; dialog = 0;
}


int FG_ConfigDialog::exec()
{
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

				// todo.. form-specific stuff here which affects p.fg struct...
				// ...
			
				p.suppressGraphs = false; //dialog->disableGraphsChk->isChecked();
				p.resumeGraphSettings = false; //dialog->resumeGraphSettingsChk->isChecked();
//				p.outputFile = dialog->outputFileLE->text();
				
				p.nVAIChans = 2304;
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
				p.acqStartEndMode = DAQ::Immediate;
				p.usePD = 0;
				
				saveSettings();

                p.lowLatency = true; // we HAVE to do this for fg_mode, otherwise this never works right!

				// this stuff doesn't need to be saved since it's constant and will mess up regular acq "remembered" values
				p.dev = "Framegrabber";
				p.nExtraChans1 = 0;
				p.nExtraChans2 = 0;
				
				p.extClock = true;
				p.mode = DAQ::AIRegular;
				p.aoPassthru = 0;
				p.dualDevMode = false;
				p.stimGlTrigResave = false;
				p.srate = DAQ::FGTask::SamplingRate;
				p.aiTerm = DAQ::Default;
				p.aiString = QString("0:%1").arg(p.nVAIChans-1);
				p.customRanges.resize(p.nVAIChans);
				p.chanDisplayNames.resize(p.nVAIChans);
				DAQ::Range rminmax(1e9,-1e9);
				for (unsigned i = 0; i < p.nVAIChans; ++i) {
					DAQ::Range r;
                    int chan_id_for_display = i;
					r.min = -5., r.max = 5.;
					// since ttl lines may be missing in channel set, renumber the ones that are missing for display purposes
						
					if (rminmax.min > r.min) rminmax.min = r.min;
					if (rminmax.max < r.max) rminmax.max = r.max;
					p.customRanges[i] = r;
                    p.chanDisplayNames[i] = DAQ::FGTask::getChannelName(chan_id_for_display);
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
	return OK;
}

void FG_ConfigDialog::guiFromSettings()
{
}

void FG_ConfigDialog::saveSettings()
{
}



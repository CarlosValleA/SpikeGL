/*
 *  Bug_ConfigDialog.h
 *  SpikeGL
 *
 *  Created by calin on 1/29/15.
 *  Copyright 2015 Calin Culianu <calin.culianu@gmail.com>. All rights reserved.
 *
 */
#ifndef FG_ConfigDialog_H
#define FG_ConfigDialog_H

#include <QObject>
#include "SpikeGL.h"
#include <QVector>
#include "DAQ.h"
#include "ui_FG_ConfigDialog.h"
#include "ui_AcqPDParams.h"
#include "ui_AcqTimedParams.h"

class QSettings;
class QDialog;
class QCheckBox;
class QMessageBox;

class FG_ConfigDialog : public QObject
{
    Q_OBJECT
public:
    FG_ConfigDialog(DAQ::Params & params, QObject *parent);
    ~FG_ConfigDialog();
	
	DAQ::Params & acceptedParams;
	
    int exec();
	
    Ui::FG_ConfigDialog *dialog; 
    Ui::AcqPDParams *acqPdParams;
    Ui::AcqTimedParams *acqTimedParams;

			
    bool isDialogVisible() const { return dialogW->isVisible(); }

    /// returns empty string on ok, or a string describing the problem if not ok
    static QString validateChanMappingText(const QString &txt, int nchans, int & rows_parsed, int & cols_parsed, QVector<int> *optional_parsed_array = 0);

private slots:
	void browseButClicked();
    void actuallyDoHardwareProbe();
    void chanMapButClicked();
    void acqStartEndCBChanged();

private:
	
   enum ValidationResult {
        AGAIN = -1,
        ABORT =  0,
        OK    =  1
    };
    
    ValidationResult validateForm(QString & errTitle, QString & errMsg, bool isGUI = false);
	
	
	void guiFromSettings(); 
	void saveSettings();
	
    void createAndShowPleaseWaitDialog();

    QDialog *dialogW;
    QWidget *acqPdParamsW, *acqTimedParamsW;
    QMessageBox *mb;
    QVector<int> chanMapFromUser;
    QString chanMapTxt; int spatialRows, spatialCols;
};


#endif


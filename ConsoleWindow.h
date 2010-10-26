#ifndef ConsoleWindow_H
#define ConsoleWindow_H

#include <QMainWindow>
class QTextEdit;
class QAction;

/// This class encapsulates the console window that the user sees in the GUI.
class ConsoleWindow : public QMainWindow
{
public:
    ConsoleWindow(QWidget *parent = 0, Qt::WindowFlags flags = 0);
    
    /// Returns a pointer to the QTextEdit area which is used for messages in the console window.  Classes such as Log, Error, Debug, and Warning make use of this textedit to print messages to the console.
    QTextEdit *textEdit() const;

	QMenu *windowMenu() const { return windMenu; }
	
protected:

    /// Basically calls QApplication::quit -- closing the console window is equivalent to quitting the application
    void closeEvent(QCloseEvent *);

private:
	mutable QMenu *windMenu;
};

#endif

######################################################################
# Automatically generated by qmake (2.01a) Tue Apr 21 18:41:54 2009
######################################################################

TEMPLATE = app
TARGET = 
DEPENDPATH += .
INCLUDEPATH += .

# Input
HEADERS += LeoDAQGL.h DataFile.h Params.h sha1.h Util.h TypeDefs.h ConsoleWindow.h MainApp.h Version.h ConfigureDialogController.h DAQ.h GraphsWindow.h GLGraph.h SampleBufQ.h Vec2.h WrapBuffer.h Vec2WrapBuffer.h Sha1VerifyTask.h Par2Window.h StimGL_LeoDAQGL_Integration.h HPFilter.h

SOURCES += DataFile.cpp osdep.cpp Params.cpp sha1.cpp Util.cpp MainApp.cpp ConsoleWindow.cpp main.cpp ConfigureDialogController.cpp DAQ.cpp GraphsWindow.cpp GLGraph.cpp SampleBufQ.cpp WrapBuffer.cpp Sha1VerifyTask.cpp Par2Window.cpp StimGL_LeoDAQGL_Integration.cpp HPFilter.cpp

FORMS += ConfigureDialog.ui AcqPDParams.ui AcqTimedParams.ui Par2Window.ui StimGLIntegration.ui

QT += opengl network


win32 {
        LIBS += NI/NIDAQmx.lib WS2_32.lib
        DEFINES += HAVE_NIDAQmx _CRT_SECURE_NO_WARNINGS
	RESOURCES += Resources.qrc
        RC_FILE += WinResources.rc
}

unix {
        CONFIG += debug warn_on
#	QMAKE_CFLAGS += -Wall -Wno-return-type
#	QMAKE_CXXFLAGS += -Wall -Wno-return-type
# Enable these for profiling!
#        QMAKE_CFLAGS += -pg
#        QMAKE_CXXFLAGS += -pg
#        QMAKE_LFLAGS += -pg
}

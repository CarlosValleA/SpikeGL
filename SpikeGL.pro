TEMPLATE = subdirs

SUBDIRS = Fake_FG_SpikeGL SpikeGLApp
CONFIG += ordered

Fake_FG_SpikeGL.subdir = FrameGrabber/Fake_FG_SpikeGL
SpikeGLApp.file = SpikeGLApp.pro
SpikeGLApp.depends = Fake_FG_SpikeGL

win32 {
    SUBDIRS -= Fake_GL_SpikeGL
    SpikeGLApp.depends -= Fake_FG_SpikeGL
}



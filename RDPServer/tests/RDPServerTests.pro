QT += core network testlib

TEMPLATE = app
TARGET = tst_rdpserver

CONFIG += c++17 console testcase warn_on
CONFIG -= app_bundle c++11

FREERDP_COMPONENTS = \
    freerdp-server \
    freerdp \
    winpr-tools \
    winpr

isEmpty(FREERDP_INSTALL_PREFIX) {
    FREERDP_INSTALL_PREFIX = $$clean_path($$_PRO_FILE_PWD_/../../build/freerdp-install)
}

include(../../qmake/freerdp.pri)

win32: LIBS += -lws2_32 -lgdi32 -luser32

INCLUDEPATH += ..

SOURCES += \
    tst_rdpserver.cpp \
    ../clipboardcontroller.cpp \
    ../desktopcapture.cpp \
    ../desktopframebuffer.cpp \
    ../inputcontroller.cpp \
    ../rdpclipboardhandler.cpp \
    ../rdpinputhandler.cpp \
    ../rdpserver.cpp \
    ../rdpserversession.cpp

win32: SOURCES += \
    ../windowsclipboardcontroller.cpp \
    ../windowsdesktopcapture.cpp \
    ../windowsinputcontroller.cpp

HEADERS += \
    ../clipboardcontroller.h \
    ../desktopcapture.h \
    ../desktopframebuffer_p.h \
    ../inputcontroller.h \
    ../rdpclipboardhandler_p.h \
    ../rdpinputhandler_p.h \
    ../rdpserver.h \
    ../rdpserversession_p.h

win32: HEADERS += \
    ../windowsclipboardcontroller_p.h \
    ../windowsdesktopcapture_p.h \
    ../windowsinputcontroller_p.h

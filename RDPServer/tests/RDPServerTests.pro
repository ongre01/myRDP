QT += core network testlib

TEMPLATE = app
TARGET = tst_rdpserver

CONFIG += c++17 console testcase warn_on
CONFIG -= app_bundle c++11

FREERDP_COMPONENTS = \
    freerdp-server \
    freerdp \
    winpr

isEmpty(FREERDP_INSTALL_PREFIX) {
    FREERDP_INSTALL_PREFIX = $$clean_path($$_PRO_FILE_PWD_/../../build/freerdp-install)
}

include(../../qmake/freerdp.pri)

win32: LIBS += -lws2_32

INCLUDEPATH += ..

SOURCES += \
    tst_rdpserver.cpp \
    ../rdpserver.cpp \
    ../rdpserversession.cpp

HEADERS += \
    ../rdpserver.h \
    ../rdpserversession_p.h

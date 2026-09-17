QT += core testlib

TEMPLATE = app
TARGET = rdpclient_channel_test

CONFIG += c++17 console testcase warn_on
CONFIG -= app_bundle c++11
DEFINES += RDPCLIENT_TESTING

FREERDP_COMPONENTS = \
    freerdp-client \
    freerdp \
    winpr

FREERDP_INSTALL_PREFIX = $$clean_path($$_PRO_FILE_PWD_/../../build/freerdp-install)
include(../../qmake/freerdp.pri)

win32: LIBS += -lws2_32

SOURCES += \
    rdpclient_channel_test.cpp \
    ../clipboardtextcodec.cpp \
    ../rdpclient.cpp

HEADERS += \
    ../clipboardtextcodec.h \
    ../rdpclient.h \
    ../rdpinput.h

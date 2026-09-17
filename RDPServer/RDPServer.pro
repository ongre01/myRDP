QT += core gui widgets

TEMPLATE = app
TARGET = RDPServer

CONFIG += c++17 warn_on
CONFIG -= c++11

FREERDP_COMPONENTS = \
    freerdp-server \
    freerdp \
    winpr

include(../qmake/freerdp.pri)

win32: LIBS += -lws2_32

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    rdpserver.cpp \
    rdpserversession.cpp

HEADERS += \
    mainwindow.h \
    rdpserver.h \
    rdpserversession_p.h

FORMS += \
    mainwindow.ui

qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

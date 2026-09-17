QT += core gui widgets

TEMPLATE = app
TARGET = RDPServer

CONFIG += c++17 warn_on
CONFIG -= c++11

FREERDP_COMPONENTS = \
    freerdp-server \
    freerdp \
    winpr-tools \
    winpr

include(../qmake/freerdp.pri)

win32: LIBS += -lws2_32 -lgdi32 -luser32

SOURCES += \
    desktopcapture.cpp \
    desktopframebuffer.cpp \
    inputcontroller.cpp \
    main.cpp \
    mainwindow.cpp \
    rdpinputhandler.cpp \
    rdpserver.cpp \
    rdpserversession.cpp

win32: SOURCES += \
    windowsdesktopcapture.cpp \
    windowsinputcontroller.cpp

HEADERS += \
    desktopcapture.h \
    desktopframebuffer_p.h \
    inputcontroller.h \
    mainwindow.h \
    rdpinputhandler_p.h \
    rdpserver.h \
    rdpserversession_p.h

win32: HEADERS += \
    windowsdesktopcapture_p.h \
    windowsinputcontroller_p.h

FORMS += \
    mainwindow.ui

qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

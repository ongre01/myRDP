QT += core gui network widgets

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
    autostartmanager.cpp \
    clipboardcontroller.cpp \
    desktopcapture.cpp \
    desktopframebuffer.cpp \
    inputcontroller.cpp \
    main.cpp \
    mainwindow.cpp \
    rdpclipboardhandler.cpp \
    rdpinputhandler.cpp \
    rdpserver.cpp \
    rdpserversession.cpp \
    serverconfiguration.cpp \
    serverlogger.cpp

win32: SOURCES += \
    windowsclipboardcontroller.cpp \
    windowsdesktopcapture.cpp \
    windowsinputcontroller.cpp

HEADERS += \
    autostartmanager.h \
    clipboardcontroller.h \
    desktopcapture.h \
    desktopframebuffer_p.h \
    inputcontroller.h \
    mainwindow.h \
    rdpclipboardhandler_p.h \
    rdpinputhandler_p.h \
    rdpserver.h \
    rdpserversession_p.h \
    serverconfiguration.h \
    serverlogger.h

win32: HEADERS += \
    windowsclipboardcontroller_p.h \
    windowsdesktopcapture_p.h \
    windowsinputcontroller_p.h

FORMS += \
    mainwindow.ui

DISTFILES += RDPServer.ini

CONFIG += file_copies
server_configuration.files = $$PWD/RDPServer.ini
server_configuration.path = $$OUT_PWD
win32:CONFIG(debug, debug|release): server_configuration.path = $$OUT_PWD/debug
win32:CONFIG(release, debug|release): server_configuration.path = $$OUT_PWD/release
COPIES += server_configuration

qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

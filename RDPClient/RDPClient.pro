QT += core gui widgets

TEMPLATE = app
TARGET = RDPClient

CONFIG += c++17 warn_on
CONFIG -= c++11

FREERDP_COMPONENTS = \
    freerdp-client \
    freerdp \
    winpr

include(../qmake/freerdp.pri)

win32: LIBS += -lws2_32

SOURCES += \
    clientsettings.cpp \
    main.cpp \
    mainwindow.cpp \
    rdpclient.cpp \
    remotedesktopwidget.cpp

HEADERS += \
    clientsettings.h \
    mainwindow.h \
    rdpclient.h \
    remotedesktopwidget.h

FORMS += \
    mainwindow.ui

RDPCLIENT_INI = $$clean_path($$_PRO_FILE_PWD_/RDPClient.ini)
PRE_TARGETDEPS += $$RDPCLIENT_INI
QMAKE_POST_LINK += $$QMAKE_COPY $$quote($$shell_path($$RDPCLIENT_INI)) $$quote($(DESTDIR)) $$escape_expand(\\n\\t)

DISTFILES += \
    RDPClient.ini

qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

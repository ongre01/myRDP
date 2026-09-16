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

SOURCES += \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    mainwindow.h

FORMS += \
    mainwindow.ui

qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

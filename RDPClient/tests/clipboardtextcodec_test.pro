QT += core testlib

TEMPLATE = app
TARGET = clipboardtextcodec_test

CONFIG += c++17 console testcase warn_on
CONFIG -= app_bundle c++11

SOURCES += \
    clipboardtextcodec_test.cpp \
    ../clipboardtextcodec.cpp

HEADERS += \
    ../clipboardtextcodec.h

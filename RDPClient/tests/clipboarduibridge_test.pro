QT += core gui testlib

TEMPLATE = app
TARGET = clipboarduibridge_test

CONFIG += c++17 console testcase warn_on
CONFIG -= app_bundle c++11

SOURCES += \
    clipboarduibridge_test.cpp \
    ../clipboarduibridge.cpp

HEADERS += \
    ../clipboarduibridge.h

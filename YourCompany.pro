#-------------------------------------------------
#
# Project created by QtCreator 2017-11-12T21:43:38
#
#-------------------------------------------------

QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = YourCompany
TEMPLATE = app

INCLUDEPATH += C:/boost/boost_msvc2017/include/boost-1_66
LIBS += "-LC:/boost/boost_msvc2017/lib" \
            -llibboost_system-vc141-mt-gd-x32-1_66

SOURCES += main.cpp\
        mainwindow.cpp

HEADERS  += mainwindow.h

FORMS    += mainwindow.ui \
    dialog.ui \
    formed.ui

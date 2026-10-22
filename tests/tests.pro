QT += core gui widgets dbus qml testlib
CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = bridge-tests
SOURCES += bridge_tests.cpp ../bridge/traybridge.cpp ../src/tray.cpp
HEADERS += ../bridge/traybridge.h ../src/tray.h
RESOURCES += ../assets/assets.qrc

TARGET = shiftnet-billing-server
TEMPLATE = app
DESTDIR = $$PWD/../dist
QT = core websockets sql
SOURCES += \
    main.cpp \
    client.cpp \
    server.cpp

HEADERS  += \
    global.h \
    client.h \
    server.h


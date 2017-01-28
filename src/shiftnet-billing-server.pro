TARGET = shiftnet-billing-server
TEMPLATE = app
DESTDIR = $$PWD/../dist
QT = core websockets sql
SOURCES += \
    main.cpp \
    client.cpp \
    server.cpp \
    database.cpp \
    vouchervalidator.cpp

HEADERS  += \
    global.h \
    client.h \
    server.h \
    user.h \
    voucher.h \
    database.h \
    vouchervalidator.h


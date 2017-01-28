#include "server.h"

#include <QCoreApplication>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    shiftnet::Server server(&app);

    if (!server.start())
        return 1;

    return app.exec();
}

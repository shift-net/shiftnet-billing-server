#include "server.h"
#include <iostream>
#include <QFile>
#include <QCoreApplication>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    {
        QFile file("lock.shts");
        file.open(QFile::ReadWrite | QFile::Text);
        QByteArray data = file.readAll();

        QDateTime ts;
        const QString format = "dd/MM/yyyy HH:mm:ss.zzz";
        if (!data.isEmpty()) {
            ts = QDateTime::fromString(QString::fromUtf8(data), format);
        }
        else {
            ts = QDateTime::currentDateTime();
        }

        if (QDateTime::currentDateTime() < ts) {
            std::cerr << "Periksa jam dan tanggal pada sistem!" << std::endl;
            return 2;
        }

        ts = QDateTime::currentDateTime();
        file.seek(0);
        file.write(ts.toString(format).toUtf8());
        file.close();
    }

    shiftnet::Server server(&app);

    if (!server.start())
        return 1;

    return app.exec();
}

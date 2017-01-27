#include "global.h"
#include "server.h"
#include <unistd.h>

#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>

QString generateVoucherCode()
{
    sleep(1);

    const QDateTime now = QDateTime::currentDateTime();
    const QDate currentDate = now.date();
    const QStringList monthCodes = {"HBJGMDAFKCEL", "RQNSPVYXTZUW", "I183O4927650"};
    const QString dayCodes = "6Y2ENJ3T9Z8QBUDGX5HWKCMPA7FVL4R";
    const QStringList base36Map = {"0123456789abcdefghijklmnopqrstuvwxyz", "nofpid60e2v38u9b1zchj5klm7r4syqagwt"};

    QString code;
    code += dayCodes.at(currentDate.day() - 1);
    code += monthCodes.at(qrand() % 3).at(currentDate.month() - 1);
    QString plain = QString::number(now.toMSecsSinceEpoch() / 100, 36).right(4);

    for (int i = 0; i < 4; i++)
        code.append(base36Map.at(1).at(base36Map.at(0).indexOf(plain.at(i))));

    return code.toUpper();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Server server(&app);
    if (!server.start()) {
        return 1;
    }

    return app.exec();
}

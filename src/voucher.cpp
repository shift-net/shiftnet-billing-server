#include "voucher.h"

using namespace shiftnet;

QString Voucher::durationString() const
{
    int h = duration() / 60;
    int m = duration() - (h * 60);
    QString str;
    str += QString("%1").arg(h, 2, 10, QChar('0'));
    str += ":";
    str += QString("%1").arg(m, 2, 10, QChar('0'));
    return str;
}

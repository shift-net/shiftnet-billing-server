#ifndef VOUCHER_H
#define VOUCHER_H

#include <QString>

namespace shiftnet {

class Voucher
{
public:
    Voucher(const QString& code = QString(), int duration = 0)
        : _duration(duration), _code(code) {}

    inline QString code() const { return _code; }

    inline int duration() const { return _duration; }
    inline void decreaseDuration(int minute) { _duration -= minute; }

private:
    int _duration;
    QString _code;
};

}

#endif // VOUCHER_H

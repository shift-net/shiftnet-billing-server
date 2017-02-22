#ifndef VOUCHER_H
#define VOUCHER_H

#include <QString>

namespace shiftnet {

class Voucher
{
public:
    Voucher(const QString& code = QString(), int duration = 0, quint64 id = 0)
        : _duration(duration), _code(code), _id(id) {}

    inline quint64 id() const { return _id; }
    inline QString code() const { return _code; }
    inline int duration() const { return _duration; }
    QString durationString() const;

    inline void decreaseDuration(int minute) { _duration -= minute; }

private:
    int _duration;
    QString _code;
    quint64 _id;
};

}

#endif // VOUCHER_H

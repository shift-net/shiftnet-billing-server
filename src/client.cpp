#include "client.h"

Client::Client(QObject *parent)
    : QObject(parent)
{
    _timer.setInterval(60 * 1000);
    _timer.setTimerType(Qt::PreciseTimer);

    connect(&_timer, SIGNAL(timeout()), SLOT(updateDuration()));
}

void Client::topupVoucher(const Voucher& voucher)
{
    _user.addDuration(voucher.duration);
    _vouchers.enqueue(voucher);
    emit sessionUpdated(_user.duration());
}

void Client::startGuestSession(const Voucher& voucher)
{
    _vouchers.clear();
    _activeVoucher = voucher;
    _user = User(QString("guest%1").arg(id()), voucher.duration);
    _timer.start();
}

void Client::startMemberSession(const User& user)
{
    _vouchers.clear();
    _activeVoucher = Voucher();
    _user = user;
    _timer.start();
}

void Client::stopSession()
{
    _vouchers.clear();
    _timer.stop();
    _activeVoucher = Voucher();
    _user = User();
}

void Client::updateDuration()
{
    _user.addDuration(-1);
    emit sessionUpdated(_user.duration());

    if (_user.duration() == 0)
        emit sessionTimeout();

    if (_user.isMember())
        return;

    _activeVoucher.duration -= 1;
    if (_activeVoucher.duration > 0)
        return;

    emit voucherSessionTimeout(_activeVoucher.code);
    if (_vouchers.isEmpty())
        return;

    _activeVoucher = _vouchers.dequeue();
}

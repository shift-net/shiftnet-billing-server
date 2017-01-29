#include "client.h"
#include <QVariantMap>

using namespace shiftnet;

Client::Client(QObject *parent)
    : QObject(parent)
    , _socket(0)
    , _id(0)
    , _state(Offline)
{
    _timer.setInterval(60 * 1000);
    _timer.setTimerType(Qt::PreciseTimer);
    connect(&_timer, SIGNAL(timeout()), SLOT(updateDuration()));
}

void Client::topupVoucher(const Voucher& voucher)
{
    _user.addDuration(voucher.duration());
    if (_user.isGuest())
        _vouchers.enqueue(voucher);
    emit sessionUpdated();
}

void Client::startGuestSession(const Voucher& voucher)
{
    resetSession();
    _state = Used;
    _user = User::createGuest(voucher.duration());
    _activeVoucher = voucher;
    _timer.start();
}

void Client::startMemberSession(const User& user)
{
    resetSession();
    _state = Used;
    _user = user;
    _timer.start();
}

void Client::resetSession()
{
    _vouchers.clear();
    _timer.stop();
    _activeVoucher = Voucher();
    _user = User();
    _state = connection() ? Ready : Offline;
}

void Client::updateDuration()
{
    _user.addDuration(-1);
    if (_user.isGuest())
         _activeVoucher.decreaseDuration(1);

    emit sessionUpdated();

    if (_user.duration() == 0)
        emit sessionTimeout();

    if (_user.isMember()) {
        resetSession();
        return;
    }

    if (_activeVoucher.duration() > 0)
        return;

    emit voucherSessionTimeout(_activeVoucher.code());
    if (_vouchers.isEmpty()) {
        resetSession();
        return;
    }

    _activeVoucher = _vouchers.dequeue();
}

void Client::startAdminstratorSession()
{
    resetSession();
    _user = User::createAdministrator();
    _state = Maintenance;
}

void Client::resetConnection()
{
    resetSession();
    _state = Offline;
    _socket = 0;
}

QVariantMap Client::toMap() const
{
    return QVariantMap({
        { "client", QVariantMap({
            { "id",    id()},
            { "state", state()},
        })},
        { "user", QVariantMap({
            { "username", _user.username() },
            { "group", _user.group() },
            { "duration", _user.duration() },
        })},
    });
}

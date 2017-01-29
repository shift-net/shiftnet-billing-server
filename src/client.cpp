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
    _timer.setSingleShot(false);
    connect(&_timer, SIGNAL(timeout()), SLOT(updateDuration()));
}

void Client::topupVoucher(const Voucher& voucher)
{
    _user.addDuration(voucher.duration());
    if (_user.isGuest())
        _vouchers.enqueue(voucher);
    emit sessionUpdated();
}

void Client::startGuestSession(const QString& username, const Voucher& voucher)
{
    resetSession();
    _state = Used;
    _user = User::createGuest(username, voucher.duration());
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

    if (_user.isGuest()) {
        _activeVoucher.decreaseDuration(1);

        if (_activeVoucher.duration() <= 0) {
            emit voucherSessionTimeout(_activeVoucher.code());

            if (!_vouchers.isEmpty())
                _activeVoucher = _vouchers.dequeue();
        }
    }

    emit sessionUpdated();

    if (_user.duration() == 0) {
        User user = _user;
        resetSession();
        emit sessionTimeout(user);
    }
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
        { "id"   , id()},
        { "state", state()},
        { "user" , QVariantMap({
            { "id"      , _user.id() },
            { "username", _user.username() },
            { "group"   , _user.group() },
            { "duration", _user.duration() },
        })},
    });
}

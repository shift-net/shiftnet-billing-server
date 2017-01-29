#ifndef CLIENT_H
#define CLIENT_H

#include <QTimer>
#include <QQueue>

#include "user.h"
#include "voucher.h"

class QWebSocket;

namespace shiftnet {

class Client : public QObject
{
    Q_OBJECT

public:
    enum State {
        Offline,
        Ready,
        Used,
        Maintenance
    };

    explicit Client(QObject* parent = 0);

    inline void setConnection(QWebSocket* socket) { _socket = socket; }
    inline QWebSocket* connection() const { return _socket; }

    inline void setId(int id) { _id = id; }
    inline int id() const { return _id; }

    inline void setHostAddress(const QString& address) { _hostAddress = address; }
    inline QString hostAddress() const { return _hostAddress; }

    inline State state() const { return _state; }
    inline User user() const { return _user; }
    inline Voucher activeVoucher() const { return _activeVoucher; }

    void topupVoucher(const Voucher& voucher);
    void startGuestSession(const QString& username, const Voucher& voucher);
    void startMemberSession(const User& user);
    void startAdminstratorSession();
    void resetSession();
    void resetConnection();

    QVariantMap toMap() const;

signals:
    void voucherSessionTimeout(const QString& code);
    void sessionTimeout();
    void sessionUpdated();

private slots:
    void updateDuration();

private:
    QTimer _timer;
    QWebSocket* _socket;

    int _id;
    State _state;
    QString _hostAddress;
    QString _macAddress;

    User _user;
    Voucher _activeVoucher;
    QQueue<Voucher> _vouchers;
};

}

#endif // CLIENT_H

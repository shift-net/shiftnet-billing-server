#ifndef CLIENT_H
#define CLIENT_H

#include <QTimer>
#include <QQueue>

class Socket;

class User
{
public:
    User(const QString& username = QString(), int duration = 0, int id = 0)
        : _id(id), _duration(duration), _username(username) {}

    inline int id() const { return _id; }
    inline QString username() const { return _username; }
    inline bool isMember() const { return _id != 0; }
    inline int duration() const { return _duration; }

    inline void addDuration(int minute) { _duration += minute; }

private:
    int _id;
    int _duration;
    QString _username;
};

struct Voucher
{
    int duration;
    QString code;
    Voucher(const QString& code = QString(), int duration = 0)
        : duration(duration), code(code) {}
};

class Client : public QObject
{
    Q_OBJECT

public:
    explicit Client(QObject* parent = 0);

    inline void setHostAddress(const QString& address) { _hostAddress = address; }
    inline QString hostAddress() const { return _hostAddress; }

    inline void setId(int id) { _id = id; }
    inline int id() const { return _id; }

    inline User user() const { return _user; }

    void topupVoucher(const Voucher& voucher);
    void startGuestSession(const Voucher& voucher);
    void startMemberSession(const User& user);
    void stopSession();

    inline Voucher activeVoucher() const { return _activeVoucher; }

signals:
    void voucherSessionTimeout(const QString& code);
    void sessionTimeout();
    void sessionUpdated(int remainingDuration);

private slots:
    void updateDuration();

private:
    QTimer _timer;
    int _id;
    QString _hostAddress;
    User _user;
    Voucher _activeVoucher;
    QQueue<Voucher> _vouchers;
};

#endif // CLIENT_H

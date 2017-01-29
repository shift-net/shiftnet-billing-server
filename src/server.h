#ifndef SERVER_H
#define SERVER_H

#include <QObject>
#include <QSettings>
#include <QWebSocketServer>
#include <QWebSocket>

class QWebSocket;

namespace shiftnet {

class Client;

class Server : public QObject
{
    Q_OBJECT
public:
    explicit Server(QObject *parent = 0);
    bool start();

private slots:
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onWebSocketTextMessageReceived(const QString& message);

    void onClientSessionTimeout();
    void onClientSessionUpdated();
    void onVoucherSessionTimeout(const QString& code);

private:
    void processClientMessage(QWebSocket* socket, const QString& type, const QVariant& message);
    void processClientMonitorMessage(QWebSocket* socket, const QString& type, const QVariant& message);

    void processClientInit(Client* client, const QString& state);
    void processClientGuestLogin(Client* client, const QString& username, const QString& code);
    void processClientMemberLogin(Client* client, const QString& username, const QString& password,
                                  const QString& voucherCode);
    void processClientSessionStop(Client* client);

    void processClientMaintenanceStart(Client* client);
    void processClientMaintenanceStop(Client* client);

    void processClientUserTopup(Client* client, const QString& voucherCode);

    void sendToClientMonitors(const QString& type, const QVariant& message);
    void sendToClients(const QString& type, const QVariant& message);
    void sendTo(QWebSocket* socket, const QString& type, const QVariant& message = QVariant());

    Client* findClient(const QHostAddress& address);

    void resetDatabaseClientState(Client* client);

private:
    QSettings settings;
    QWebSocketServer webSocketServer;
    QList<Client*> clients;
    QList<QWebSocket*> clientMonitorSockets;
    QList<QWebSocket*> clientSockets;
    QHash<int, Client*> clientsByIds;
};

}

#endif // SERVER_H

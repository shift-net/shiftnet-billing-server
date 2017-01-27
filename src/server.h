#ifndef SERVER_H
#define SERVER_H

#include <QObject>
#include <QSettings>
#include <QWebSocketServer>
#include <QWebSocket>

class QWebSocket;
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
    void onClientSessionUpdated(int duration);
    void onVoucherSessionTimeout(const QString& code);

private:
    void processClientMessage(QWebSocket* socket, const QString& type, const QVariant& message);
    void processClientMonitorMessage(QWebSocket* socket, const QString& type, const QVariant& message);

    Client* findClient(const QHostAddress& address);

    void sendToClientMonitors(const QString& type, const QVariant& message);
    void sendToClients(const QString& type, const QVariant& message);
    void sendMessage(QWebSocket* socket, const QString& type, const QVariant& message = QVariant());

    bool resetClientSession(Client* client);

private:
    QSettings settings;
    QWebSocketServer webSocketServer;
    QList<Client*> clients;
    QList<QWebSocket*> clientMonitorSockets;
    QList<QWebSocket*> clientSockets;
    QHash<int, Client*> clientsByIds;
    QHash<int, QWebSocket*> clientSocketsByIds;
};

#endif // SERVER_H

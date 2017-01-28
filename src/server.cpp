#include "server.h"
#include "client.h"
#include "database.h"
#include "vouchervalidator.h"

#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariantList>
#include <QVariantMap>
#include <QVariant>
#include <QDebug>
#include <QCryptographicHash>
#include <QSqlRecord>

using namespace shiftnet;

Server::Server(QObject *parent)
    : QObject(parent)
    , settings("shiftnet-billing-server.ini", QSettings::IniFormat)
    , webSocketServer("snbs", QWebSocketServer::NonSecureMode)
{
    Database::setup(settings);

    settings.beginReadArray("Clients");
    for (const QString key : settings.childKeys()) {
        int id = key.toInt();
        Client* client = new Client(this);
        client->setId(id);
        client->setHostAddress(settings.value(key).toString());
        clients.append(client);
        clientsByIds.insert(id, client);

        connect(client, SIGNAL(sessionTimeout()), SLOT(onClientSessionTimeout()));
        connect(client, SIGNAL(voucherSessionTimeout(QString)), SLOT(onVoucherSessionTimeout(QString)));
        connect(client, SIGNAL(sessionUpdated()), SLOT(onClientSessionUpdated()));
    }
    settings.endArray();

    connect(&webSocketServer, SIGNAL(newConnection()), SLOT(onWebSocketConnected()));
}

bool Server::start()
{
    if (!Database::init()) {
        qCritical() << "Database connection failed!";
        return false;
    }

    if (!webSocketServer.listen(QHostAddress::Any, settings.value("Server/port").toInt())) {
        qCritical() << "Websocket server failed!";
        return false;
    }

    return true;
}

// WebSocket Callbacks
void Server::onWebSocketConnected()
{
    QWebSocket* socket = webSocketServer.nextPendingConnection();
    connect(socket, SIGNAL(disconnected()), SLOT(onWebSocketDisconnected()));
    connect(socket, SIGNAL(textMessageReceived(QString)), SLOT(onWebSocketTextMessageReceived(QString)));
}

void Server::onWebSocketDisconnected()
{
    QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
    if (socket->property("client-type").toString() == "client") {
        Client* client = clientsByIds.value(socket->property("client-id").toInt());
        resetDatabaseClientState(client);
        client->resetConnection();
        sendToClientMonitors("client-disconnected", client->toMap());
        clientSockets.removeOne(socket);
    }
    else if (socket->property("client-type").toString() == "client-monitor") {
        clientMonitorSockets.removeOne(socket);
    }
}

void Server::onWebSocketTextMessageReceived(const QString& jsonString)
{
    QString closeReason;
    QJsonParseError jsonParseError;
    QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
    const QJsonDocument doc = QJsonDocument::fromJson(jsonString.toUtf8(), &jsonParseError);

    for (;;) {
        if (!(jsonParseError.error == QJsonParseError::NoError && doc.isArray())) {
            closeReason = "Invalid json format.";
            break;
        }

        QVariantList data = doc.array().toVariantList();
        if (data.size() != 3) {
            closeReason = "Invalid json format.";
            break;
        }

        const QString clientType = data.at(0).toString();

        if (socket->property("client-type").toString() == "") {
            if (clientType == "client-monitor") {
                clientMonitorSockets.append(socket);
            }
            else if (clientType == "client") {
                Client* client = findClient(socket->peerAddress());
                if (!client) {
                    closeReason = "Client not registered";
                    break;
                }
                client->setConnection(socket);
                socket->setProperty("client-id", client->id());
                clientSockets.append(socket);
            }
            else {
                closeReason = "Unknown client type";
                break;
            }

            socket->setProperty("client-type", clientType);
        }

        if (clientType == "client") {
            processClientMessage(socket, data.at(1).toString(), data.at(2));
            return;
        }
        else if (clientType == "client-monitor") {
            processClientMonitorMessage(socket, data.at(1).toString(), data.at(2));
            return;
        }

        closeReason = "Unknown client type. " + jsonString;
        break;
    }

    qWarning() << "Connection refused:" << qPrintable(closeReason);

    socket->close(QWebSocketProtocol::CloseCodeNormal, closeReason);
}

// Client Callbacks

void Server::onClientSessionTimeout()
{
    Client* client = qobject_cast<Client*>(sender());
    resetDatabaseClientState(client);
    sendTo(client->connection(), "session-timeout", QVariant());
    sendToClientMonitors("client-session-timeout", client->toMap());
}

void Server::onVoucherSessionTimeout(const QString& voucherCode)
{
    Database::deleteVoucher(voucherCode);
}

void Server::onClientSessionUpdated()
{
    Client* client = qobject_cast<Client*>(sender());
    User user = client->user();
    if (user.isMember())
        Database::updateMemberDuration(user.id(), user.duration());
    else {
        Voucher activeVoucher = client->activeVoucher();
        Database::updateVoucherDuration(activeVoucher.code(), activeVoucher.duration());
    }

    sendTo(client->connection(), "session-sync", user.duration());
    sendToClientMonitors("client-session-sync", client->toMap());
}

// Process message methods (Client)

void Server::processClientInit(Client* client, const QString& state)
{
    if (state == "maintenance") {
        client->startAdminstratorSession();
    }
    else {
        client->resetSession();
    }

    sendTo(client->connection(), "init", QVariantMap({
        { "company", QVariantMap({
            { "name", settings.value("Company/name") },
            { "address", settings.value("Company/address") },
        })},
        { "client", QVariantMap({
            { "id", client->id() },
            { "password", QCryptographicHash::hash(settings.value("Client/password").toByteArray(), QCryptographicHash::Sha1).toHex() },
        })}
    }));
    sendToClientMonitors("client-connected", client->toMap());
}

void Server::processClientGuestLogin(Client *client, const QString &voucherCode)
{
    VoucherValidator validator;

    if (!validator.isValid(voucherCode, false)) {
        sendTo(client->connection(), "guest-login-failed", validator.error());
        return;
    }

    const Voucher voucher = validator.voucher();

    if (!Database::useVoucher(voucher.code(), client->id())) {
        sendTo(client->connection(), "guest-login-failed", "Kesalahan pada server database.");
        return;
    }

    client->startGuestSession(voucher);

    User user = client->user();
    sendTo(client->connection(), "session-start", QVariantMap({
        { "username", user.username() },
        { "duration", user.duration() },
    }));
    sendToClientMonitors("client-session-start", client->toMap());
}

void Server::processClientMemberLogin(Client* client, const QString& username, const QString& password, const QString& voucherCode)
{
    QSqlRecord record = Database::findMember(username);
    if (record.isEmpty()) {
        sendTo(client->connection(), "member-login-failed", QVariantList({"username", "Nama pengguna tidak ditemukan."}));
        return;
    }

    User user = User::createMember(record.value("id").toInt(), record.value("username").toString(), record.value("duration").toInt());

    if (record.value("password").toString() != password) {
        sendTo(client->connection(), "member-login-failed", QVariantList({"password", "Kata sandi anda salah."}));
        return;
    }

    // pastikan user aktif
    if (record.value("active").toBool() != true) {
        sendTo(client->connection(), "member-login-failed",
               QVariantList({"username","Akun anda tidak aktif, silahkan hubungi operator."}));
        return;
    }

    // jangan sampai double login
    int activeClientId = record.value("client_id").toInt();
    if (activeClientId != 0) {
        sendTo(client->connection(), "member-login-failed",
               QVariantList({"username", QString("Akun anda sedang login di client %1.").arg(activeClientId)}));
        return;
    }

    if (!voucherCode.isEmpty()) {
        VoucherValidator validator;
        if (!validator.isValid(voucherCode, true)) {
            sendTo(client->connection(), "member-login-failed", QVariantList({"voucherCode", validator.error() }));
            return;
        }

        const Voucher voucher = validator.voucher();

        if (!Database::topupMemberVoucher(user.id(), user.duration(), voucher.code(), voucher.duration())) {
            sendTo(client->connection(), "member-login-failed", QVariantList({"voucherCode", "Kesalahan pada database server."}));
            return;
        }

        user.addDuration(voucher.duration());
    }

    if (user.duration() <= 0) {
        sendTo(client->connection(), "member-login-failed", QVariantList({"voucherCode", "Sisa waktu habis, silahkan isi voucher!"}));
        return;
    }

    if (!Database::setMemberClientId(user.id(), client->id())) {
        sendTo(client->connection(), "member-login-failed", QVariantList({"username", "Kesalahan pada server database."}));
        return;
    }

    client->startMemberSession(user);

    sendTo(client->connection(), "session-start", QVariantMap({
        { "username", user.username() },
        { "duration", user.duration() },
    }));
    sendToClientMonitors("client-session-start", client->toMap());
}

void Server::processClientMaintenanceStart(Client* client)
{
    client->startAdminstratorSession();
    sendToClientMonitors("client-maintenance-started", client->toMap());
}

void Server::processClientMaintenanceStop(Client* client)
{
    client->resetSession();
    sendToClientMonitors("client-maintenance-finished", client->toMap());
}

void Server::processClientMemberTopup(Client* client, const QString& voucherCode)
{
    VoucherValidator validator;

    if (!validator.isValid(voucherCode, true)) {
        sendTo(client->connection(), "user-topup-failed", validator.error());
        return;
    }

    const Voucher voucher = validator.voucher();
    const User user = client->user();

    if (!Database::topupMemberVoucher(user.id(), user.duration(), voucher.code(), voucher.duration())) {
        sendTo(client->connection(), "user-topup-failed", "Kesalahan pada server database.");
        return;
    }

    client->topupVoucher(voucher);

    sendTo(client->connection(), "user-topup-success", voucher.duration());
    sendToClientMonitors("user-topup-success", client->toMap());
}

void Server::processClientGuestTopup(Client* client, const QString& voucherCode)
{
    VoucherValidator validator;

    if (!validator.isValid(voucherCode, false)) {
        sendTo(client->connection(), "user-topup-failed", validator.error());
        return;
    }

    const Voucher voucher = validator.voucher();

    if (!Database::useVoucher(voucher.code(), client->id())) {
        sendTo(client->connection(), "user-topup-failed", "Kesalahan pada database server.");
        return;
    }

    client->topupVoucher(voucher);

    sendTo(client->connection(), "user-topup-success", voucher.duration());
    sendToClientMonitors("user-topup", client->toMap());
}

void Server::processClientSessionStop(Client* client)
{
    resetDatabaseClientState(client);
    client->resetSession();
    sendTo(client->connection(), "session-stop");
    sendToClientMonitors("client-session-stop", client->toMap());
}

void Server::processClientMessage(QWebSocket* socket, const QString& type, const QVariant& message)
{
    Client* client = clientsByIds.value(socket->property("client-id").toInt());
    if (type == "init") {
        processClientInit(client, message.toString());
    }
    else if (type == "guest-login") {
        processClientGuestLogin(client, message.toString());
    }
    else if (type == "member-login") {
        const QStringList messages = message.toStringList();
        processClientMemberLogin(client, messages.at(0), messages.at(1), messages.at(2));
    }
    else if (type == "session-stop") {
        processClientSessionStop(client);
    }
    else if (type == "user-topup") {
        const QString code = message.toString();
        if (client->user().isMember())
            processClientMemberTopup(client, code);
        else
            processClientGuestTopup(client, code);
    }
    else if (type == "maintenance-start") {
        processClientMaintenanceStart(client);
    }
    else if (type == "maintenance-stop") {
        processClientMaintenanceStop(client);
    }
}

// Process message methods (ClientMonitor)

void Server::processClientMonitorMessage(QWebSocket* connection, const QString& msgType, const QVariant& message)
{
    if (msgType == "init") {
        QVariantList clientList;
        for (Client* client: clients) {
            User user = client->user();
            clientList.append(QVariantMap({
                { "client", QVariantMap({
                    { "id", client->id() },
                    { "state", client->state() },
                })},
                { "user", QVariantMap({
                    { "username", user.username() },
                    { "group", user.group() },
                    { "duration", user.duration() },
                })}
            }));
        }

        sendTo(connection, "init", QVariantMap({
            { "company", QVariantMap({
                { "name", settings.value("Company/name") },
                { "address", settings.value("Company/address") },
            })},
            { "clients", clientList }
        }));
    }
    else if (msgType == "stop-sessions") {
        for (const QVariant id: message.toList()) {
            Client* client = clientsByIds.value(id.toInt());
            if (!(client && client->connection())) continue;
            processClientSessionStop(client);
        }
    }
    else if (msgType == "shutdown-clients" || msgType == "restart-clients") {
        for (const QVariant id: message.toList()) {
            Client* client = clientsByIds.value(id.toInt());
            if (!(client && client->connection())) continue;
            sendTo(client->connection(), "system-" + msgType.split("-").first());
        }
    }
}

// Send message methods
void Server::sendToClientMonitors(const QString& type, const QVariant& data)
{
    for (QWebSocket* socket: clientMonitorSockets)
        sendTo(socket, type, data);
}

void Server::sendToClients(const QString& type, const QVariant& data)
{
    for (QWebSocket* socket: clientSockets)
        sendTo(socket, type, data);
}

void Server::sendTo(QWebSocket* socket, const QString& type, const QVariant& message)
{
    QString textMessage = QJsonDocument::fromVariant(QVariantList({ type, message })).toJson(QJsonDocument::Compact);
    socket->sendTextMessage(textMessage);
    socket->flush();
}

// Common helper methods
void Server::resetDatabaseClientState(Client* client)
{
    User user = client->user();
    if (user.isMember())
        Database::resetMemberClientState(user.id());
    else
        Database::resetVoucherClientState(client->id());
}

Client* Server::findClient(const QHostAddress& address)
{
    QString addr = address.toString().split(":").last();
    for (Client* client: clients)
        if (client->hostAddress() == addr)
            return client;
    return 0;
}

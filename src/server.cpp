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
#include <QCryptographicHash>
#include <QSqlRecord>
#include <QDebug>

#define ACTIVITY_USER_TOPUP "topup"
#define ACTIVITY_USER_SESSION_START "session-start"
#define ACTIVITY_USER_SESSION_STOP  "session-stop"
#define ACTIVITY_MAINTENANCE_START  "maintenance-start"
#define ACTIVITY_MAINTENANCE_STOP   "maintenance-stop"

using namespace shiftnet;

Server::Server(QObject *parent)
    : QObject(parent)
    , settings("shiftnet-billing-server.ini", QSettings::IniFormat)
    , webSocketServer("snbs", QWebSocketServer::NonSecureMode)
{
    Database::setup(settings);

    connect(&webSocketServer, SIGNAL(newConnection()), SLOT(onWebSocketConnected()));
}

bool Server::start()
{
    if (!Database::init()) {
        qCritical() << "Database connection failed!";
        return false;
    }

    for (const QSqlRecord& record : Database::clients()) {
        Client* client = new Client(this);
        client->setId(record.value("id").toInt());
        client->setHostAddress(record.value("ipAddress").toString());
        client->setMacAddress(record.value("macAddress").toString());

        clients.append(client);
        clientsByIds.insert(client->id(), client);

        connect(client, SIGNAL(sessionTimeout(User)), SLOT(onClientSessionTimeout(User)));
        connect(client, SIGNAL(voucherSessionTimeout(QString)), SLOT(onVoucherSessionTimeout(QString)));
        connect(client, SIGNAL(sessionUpdated()), SLOT(onClientSessionUpdated()));
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
        const User user = client->user();

        if (user.isMember() || user.isGuest()) {
            Database::transaction();
            Voucher voucher = client->activeVoucher();
            if (user.isMember())
                Database::resetMemberClientState(user.id());
            else
                Database::resetVoucherClientState(client->id());

            Database::logUserActivity(client->id(), user, ACTIVITY_USER_SESSION_STOP,
                                      QString("Koneksi terputus, sesi telah dihentikan."),
                                      voucher.id());
            Database::commit();
        }

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

void Server::onClientSessionTimeout(const User& user)
{
    Client* client = qobject_cast<Client*>(sender());

    if (user.isMember() || user.isGuest()) {
        Database::transaction();
        Voucher voucher = client->activeVoucher();
        if (user.isMember()) {
            Database::resetMemberClientState(user.id());
            Database::updateMemberDuration(user.id(), 0);
        }
        else {
            Database::resetVoucherClientState(client->id());
        }
        Database::logUserActivity(client->id(), user, ACTIVITY_USER_SESSION_STOP,
                                  QString("Pemakaian dihentikan karena sisa waktu telah habis."),
                                  voucher.id());
        Database::commit();
    }

    sendTo(client->connection(), "session-timeout", QVariant());
    sendToClientMonitors("client-session-timeout", client->toMap());
}

void Server::onVoucherSessionTimeout(const QString& voucherCode)
{
    Client* client = qobject_cast<Client*>(sender());
    Database::logUserActivity(client->id(), client->user(),
                              ACTIVITY_USER_SESSION_STOP, QString("Pemakaian dihentikan. Durasi voucher %1 telah habis.").arg(voucherCode),
                              client->activeVoucher().id());
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

void Server::processClientGuestLogin(Client *client, const QString& username, const QString &voucherCode)
{
    VoucherValidator validator;

    if (!validator.isValid(voucherCode, false)) {
        sendTo(client->connection(), "guest-login-failed", validator.error());
        return;
    }

    const Voucher voucher = validator.voucher();

    if (!Database::useVoucher(voucher.code(), client->id(), username)) {
        sendTo(client->connection(), "guest-login-failed", "Kesalahan pada server database.");
        return;
    }

    client->startGuestSession(username, voucher);
    Database::logUserActivity(client->id(), client->user(), ACTIVITY_USER_SESSION_START,
                              QString("Memulai pemakaian voucher %1 durasi %2.").arg(voucher.code(), voucher.durationString()),
                              voucher.id());

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

    User user = User::createMember(record.value("id").toInt(), record.value("username").toString(), record.value("remainingDuration").toInt());

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
    int activeClientId = record.value("activeClientId").toInt();
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
        Database::logUserActivity(client->id(), user, ACTIVITY_USER_TOPUP,
                                  QString("Topup voucher %1 durasi %2.").arg(voucher.code(), voucher.durationString()),
                                  voucher.id());
    }

    if (user.duration() <= 0) {
        sendTo(client->connection(), "member-login-failed", QVariantList({"username", "Sisa waktu habis, silahkan isi voucher!"}));
        return;
    }

    if (!Database::setMemberClientId(user.id(), client->id())) {
        sendTo(client->connection(), "member-login-failed", QVariantList({"username", "Kesalahan pada server database."}));
        return;
    }

    client->startMemberSession(user);
    Database::logUserActivity(client->id(), user, ACTIVITY_USER_SESSION_START, "Memulai pemakaian.");

    sendTo(client->connection(), "session-start", QVariantMap({
        { "username", user.username() },
        { "duration", user.duration() },
    }));
    sendToClientMonitors("client-session-start", client->toMap());
}

void Server::processClientMaintenanceStart(Client* client)
{
    client->startAdminstratorSession();
    Database::logUserActivity(client->id(), client->user(), ACTIVITY_MAINTENANCE_START, "Pemeliharaan dimulai.");
    sendToClientMonitors("client-maintenance-started", client->toMap());
}

void Server::processClientMaintenanceStop(Client* client)
{
    Database::logUserActivity(client->id(), client->user(), ACTIVITY_MAINTENANCE_STOP, "Pemeliharaan selesai.");
    client->resetSession();
    sendToClientMonitors("client-maintenance-finished", client->toMap());
}

void Server::processClientUserTopup(Client* client, const QString& voucherCode)
{
    VoucherValidator validator;
    User user = client->user();

    if (!validator.isValid(voucherCode, user.isMember())) {
        sendTo(client->connection(), "user-topup-failed", validator.error());
        return;
    }

    const Voucher voucher = validator.voucher();

    if (!Database::topupVoucher(client->id(), user, voucher)) {
        sendTo(client->connection(), "user-topup-failed", "Kesalahan pada server database.");
        return;
    }

    client->topupVoucher(voucher);

    Database::logUserActivity(client->id(), user, ACTIVITY_USER_TOPUP,
                              QString("Topup voucher %1 durasi %2").arg(voucher.code(), voucher.durationString()),
                              voucher.id());

    sendTo(client->connection(), "user-topup-success", voucher.duration());
}

void Server::processClientSessionStop(Client* client)
{
    const User user = client->user();
    const Voucher voucher = client->activeVoucher();
    QString activityInfo;

    Database::transaction();
    if (user.isGuest()) {
        Database::resetVoucherClientState(client->id());
        const Voucher voucher = client->activeVoucher();
        activityInfo = QString("Kode voucher: %1, Sisa Waktu: %2.").arg(voucher.code(), voucher.durationString());
    }
    else if (user.isMember()) {
        Database::resetMemberClientState(user.id());
        activityInfo = QString("Sisa Waktu: %1.").arg(Voucher("", user.duration()).durationString());
    }
    Database::logUserActivity(client->id(), user, ACTIVITY_USER_SESSION_STOP, "Sesi pemakaian dihentikan. " + activityInfo,
                              voucher.id());
    Database::commit();

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
        const QStringList messages = message.toStringList();
        processClientGuestLogin(client, messages.at(0), messages.at(1));
    }
    else if (type == "member-login") {
        const QStringList messages = message.toStringList();
        processClientMemberLogin(client, messages.at(0), messages.at(1), messages.at(2));
    }
    else if (type == "session-stop") {
        processClientSessionStop(client);
    }
    else if (type == "user-topup") {
        processClientUserTopup(client, message.toString());
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
            clientList.append(client->toMap());
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

            if (client->state() == Client::Used)
                processClientSessionStop(client);
            else if (client->state() == Client::Maintenance) {
                sendTo(client->connection(), "maintenance-remote-stop");
                processClientMaintenanceStop(client);
            }
        }
    }
    else if (msgType == "shutdown-clients" || msgType == "restart-clients") {
        for (const QVariant id: message.toList()) {
            Client* client = clientsByIds.value(id.toInt());
            if (!(client && client->connection())) continue;
            if (client->state() == Client::Offline) continue;
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
Client* Server::findClient(const QHostAddress& address)
{
    QString addr = address.toString().split(":").last();
    for (Client* client: clients)
        if (client->hostAddress() == addr)
            return client;
    return 0;
}

#include "server.h"
#include "client.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
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

Server::Server(QObject *parent)
    : QObject(parent)
    , settings("shiftnet-billing-server.ini", QSettings::IniFormat)
    , webSocketServer("snbs", QWebSocketServer::NonSecureMode)
{
    settings.beginGroup("Databases");
    QSqlDatabase db = QSqlDatabase::addDatabase(settings.value("main.driver").toString());
    db.setHostName(settings.value("main.host").toString());
    db.setPort(settings.value("main.port").toInt());
    db.setUserName(settings.value("main.username").toString());
    db.setPassword(settings.value("main.password").toString());
    db.setDatabaseName(settings.value("main.schema").toString());
    settings.endGroup();

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
        connect(client, SIGNAL(sessionUpdated(int)), SLOT(onClientSessionUpdated(int)));
    }
    settings.endArray();

    connect(&webSocketServer, SIGNAL(newConnection()), SLOT(onWebSocketConnected()));
}

bool Server::start()
{
    QSqlDatabase db = QSqlDatabase::database();
    QSqlQuery q(db);

    if (!db.isOpen()) {
        qCritical() << "Database connection failed!";
        return false;
    }

    db.transaction();

    q.exec("update members set client_id=0 where 1");

    q.prepare("delete from vouchers where duration<=0 or expiration_datetime<=?");
    q.bindValue(0, QDateTime::currentDateTime());
    q.exec();

    q.exec("update vouchers set client_id=0 where 1");

    db.commit();

    if (!webSocketServer.listen(QHostAddress::Any, settings.value("Server/port").toInt())) {
        qCritical() << "Websocket server failed!";
        return false;
    }

    return true;
}

Client* Server::findClient(const QHostAddress& address)
{
    QString addr = address.toString().split(":").last();
    for (Client* client: clients)
        if (client->hostAddress() == addr)
            return client;
    return 0;
}

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

        resetClientSession(client);

        qDebug() << "client disconnected:" << client->id() << qPrintable(client->hostAddress());

        sendToClientMonitors("client-disconnected", client->id());

        clientSockets.removeOne(socket);
        clientSocketsByIds.remove(client->id());
    }
    else if (socket->property("client-type").toString() == "client-monitor") {
        qDebug() << "client monitor disconnected:" << qPrintable(socket->peerAddress().toString());
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

        if (socket->property("client-type") == QVariant()) {
            if (clientType == "client-monitor") {
                clientMonitorSockets.append(socket);
            }
            else if (clientType == "client") {
                Client* client = findClient(socket->peerAddress());
                if (!client) {
                    closeReason = "Client not registered";
                    break;
                }
                socket->setProperty("client-id", client->id());
                clientSockets.append(socket);
                clientSocketsByIds.insert(client->id(), socket);
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

        closeReason = "Unknown client type.";
        break;
    }

    qWarning() << "Connection refused:" << qPrintable(closeReason);

    socket->close(QWebSocketProtocol::CloseCodeNormal, closeReason);
}

void Server::onClientSessionTimeout()
{
    Client* client = qobject_cast<Client*>(sender());
    QWebSocket* socket = clientSocketsByIds.value(client->id());
    if (!socket) {
        return;
    }

    resetClientSession(client);

    sendMessage(socket, "session-timeout", QVariant());
    sendToClientMonitors("client-session-timeout", client->id());
}

void Server::onVoucherSessionTimeout(const QString& code)
{
    QSqlQuery q(QSqlDatabase::database());
    q.prepare("delete from vouchers where code=?");
    q.bindValue(0, code);
    q.exec();
}

void Server::onClientSessionUpdated(int duration)
{
    Client* client = qobject_cast<Client*>(sender());
    QWebSocket* socket = clientSocketsByIds.value(client->id());
    if (!socket) {
        return;
    }

    QSqlQuery q(QSqlDatabase::database());
    if (client->user().isMember()) {
        q.prepare("update members set duration=? where id=?");
        q.bindValue(0, duration);
        q.bindValue(1, client->user().id());
    }
    else {
        q.prepare("update vouchers set duration=? where code=?");
        q.bindValue(0, client->activeVoucher().duration);
        q.bindValue(1, client->activeVoucher().code);
    }
    q.exec();

    sendToClientMonitors("client-session-sync", QVariantMap({
        {"client", client->id()},
        {"username", client->user().username()},
        {"duration", duration},
    }));
    sendMessage(socket, "session-sync", duration);
}

void Server::processClientMessage(QWebSocket* socket, const QString& type, const QVariant& message)
{
    Client* client = clientsByIds.value(socket->property("client-id").toInt());
    if (type == "init") {
        sendToClientMonitors("client-connected", client->id());
        sendMessage(socket, type, QVariantMap({
            { "company", QVariantMap({
                { "name", settings.value("Company/name") },
                { "address", settings.value("Company/address") },
            })},
            { "client", QVariantMap({
                { "id", client->id() },
                { "password", QCryptographicHash::hash(settings.value("Client/password").toByteArray(), QCryptographicHash::Sha1).toHex() },
            })}
        }));
    }
    else if (type == "guest-login") {
        const QString code = message.toString();
        QSqlQuery q(QSqlDatabase::database());
        q.prepare("select * from vouchers where code=?");
        q.bindValue(0, code);
        if (!q.exec()) {
            qCritical() << "Database error:" << qPrintable(q.lastError().text());
            return;
        }

        // pastikan kode voucher ada
        if (!q.next()) {
            sendMessage(socket, "guest-login-failed", "Kode voucher tidak ditemukan.");
            return;
        }

        // cek status kadaluarsa
        const QDateTime now = QDateTime::currentDateTime();
        const QDateTime expirationDatetime = q.value("expiration_datetime").toDateTime();
        if (expirationDatetime < now) {
            sendMessage(socket, "guest-login-failed", "Voucher sudah kadaluarsa sejak "
                        + expirationDatetime.toString("dddd, dd MMMM yyyy hh:mm:ss") + ".");
            return;
        }

        // cek sedang dipakai
        int activeClientId = q.value("client_id").toInt();
        if (activeClientId) {
            sendMessage(socket, "guest-login-failed", "Voucher sedang digunakan di Client " + QString::number(activeClientId) + ".");
            return;
        }

        // cek sisa durasi
        int duration = q.value("duration").toInt();
        if (duration <= 0) {
            sendMessage(socket, "guest-login-failed", "Sisa waktu sudah habis.");
            return;
        }

        // update session di database
        q.prepare("update vouchers set client_id=?, is_used=1 where code=?");
        q.bindValue(0, client->id());
        q.bindValue(1, code);
        if (!q.exec()) {
            qCritical() << "Database error:" << qPrintable(q.lastError().text());
            return;
        }

        // mulai sesi client
        client->startGuestSession(Voucher(code, duration));

        qDebug() << "guest login success:" << code << client->id();

        sendMessage(socket, "session-start", QVariantMap({
            { "username", client->user().username() },
            { "duration", duration },
        }));

        sendToClientMonitors("client-session-start", QVariantMap({
            { "client", client->id() },
            { "username", client->user().username() },
            { "duration", duration },
        }));
    }
    else if (type == "member-login") {
        const QStringList d = message.toStringList();
        const QString username = d.at(0);
        const QString password = d.at(1);
        const QString voucherCode = d.at(2);

        QSqlQuery q(QSqlDatabase::database());
        q.prepare("select * from members where username=?");
        q.bindValue(0, username);
        if (!q.exec()) {
            qCritical() << "Database error:" << qPrintable(q.lastError().text());
            return;
        }

        // pastikan user ada
        if (!q.next()) {
            sendMessage(socket, "member-login-failed", QVariantList({"username", "Nama pengguna tidak ditemukan."}));
            return;
        }

        User user(q.value("username").toString(), q.value("duration").toInt(), q.value("id").toInt());

        // pastikan password cocok
        if (q.value("password").toString() != password) {
            sendMessage(socket, "member-login-failed", QVariantList({"password", "Kata sandi anda salah."}));
            return;
        }

        // pastikan user aktif
        if (q.value("active").toBool() != true) {
            sendMessage(socket, "member-login-failed", QVariantList({"username", "Akun anda tidak aktif, silahkan hubungi operator."}));
            return;
        }

        // jangan sampai double login
        int activeClientId = q.value("client_id").toInt();
        if (activeClientId != 0) {
            sendMessage(socket, "member-login-failed", QVariantList({"username", QString("Akun anda sedang login di client %1.").arg(activeClientId)}));
            return;
        }

        // Validasi voucher hanya jika user topup
        if (!voucherCode.isEmpty()) {
            q.prepare("select * from vouchers where code=?");
            q.bindValue(0, voucherCode);
            if (!q.exec()) {
                qCritical() << "Database error:" << qPrintable(q.lastError().text());
                return;
            }

            // pastikan kode voucher ada
            if (!q.next()) {
                sendMessage(socket, "member-login-failed", QVariantList({"voucherCode", "Kode voucher tidak ditemukan."}));
                return;
            }

            // pastikan bukan voucher bekas pakai
            if (q.value("is_used").toBool()) {
                sendMessage(socket, "member-login-failed", QVariantList({"voucherCode", "Kode voucher bekas tidak dapat dipakai."}));
                return;
            }

            // cek status kadaluarsa
            const QDateTime now = QDateTime::currentDateTime();
            const QDateTime expirationDatetime = q.value("expiration_datetime").toDateTime();
            if (expirationDatetime < now) {
                sendMessage(socket, "member-login-failed", QVariantList({
                            "voucherCode",
                            "Voucher sudah kadaluarsa sejak " + expirationDatetime.toString("dddd, dd MMMM yyyy hh:mm:ss") + "."
                }));
                return;
            }

            // cek sedang dipakai
            activeClientId = q.value("client_id").toInt();
            if (activeClientId) {
                sendMessage(socket, "member-login-failed", QVariantList({
                    "voucherCode",
                    "Voucher sedang digunakan di Client " + QString::number(activeClientId) + "."
                }));
                return;
            }

            // cek sisa durasi
            int voucherDuration = q.value("duration").toInt();
            if (voucherDuration <= 0) {
                sendMessage(socket, "member-login-failed", QVariantList({
                    "voucherCode",
                    "Sisa waktu voucher telah habis."
                }));
                return;
            }

            // tambahkan durasi akun user sesuai voucher
            QSqlDatabase db = QSqlDatabase::database();
            db.transaction();
            QSqlQuery qq(db);
            qq.prepare("update members set duration=? where id=?");
            qq.bindValue(0, voucherDuration + user.duration());
            qq.bindValue(1, user.id());
            if (!qq.exec()) {
                db.rollback();
                sendMessage(socket, "member-login-failed", QVariantList({"voucherCode", "Topup voucher gagal, kesalahan pada server!"}));
                return;
            }

            // hapus voucher dari daftar
            qq.prepare("delete from vouchers where code=?");
            qq.bindValue(0, voucherCode);
            if (!qq.exec()) {
                db.rollback();
                sendMessage(socket, "member-login-failed", QVariantList({"voucherCode", "Topup voucher gagal, kesalahan pada server!"}));
                return;
            }

            if (!db.commit()) {
                db.rollback();
                sendMessage(socket, "member-login-failed", QVariantList({"voucherCode", "Topup voucher gagal, kesalahan pada server!"}));
                return;
            }

            user.addDuration(voucherDuration);
        }

        // paksa isi voucher  apabila sisa waktu telah habis
        if (user.duration() <= 0) {
            sendMessage(socket, "member-login-failed", QVariantList({"voucherCode", "Sisa waktu habis, silahkan isi voucher!"}));
            return;
        }

        q.prepare("update members set client_id=? where id=?");
        q.bindValue(0, client->id());
        q.bindValue(1, user.id());
        if (!q.exec()) {
            sendMessage(socket, "member-login-failed", QVariantList({"username", "Login gagal, kesalahan pada server!"}));
            return;
        }

        client->startMemberSession(user);

        qDebug() << "member login success:" << user.username() << client->id();

        sendMessage(socket, "session-start", QVariantMap({
            { "username", client->user().username() },
            { "duration", client->user().duration() },
        }));
        sendToClientMonitors("client-session-start", QVariantMap({
            { "client", client->id() },
            { "username", client->user().username() },
            { "duration", client->user().duration() },
        }));
    }
    else if (type == "session-stop") {
        if (!resetClientSession(client)) {
            return;
        }
        sendMessage(socket, "session-stop");
        sendToClientMonitors("client-session-stop", QVariantMap({
            { "client", client->id() },
            { "username", client->user().username() },
        }));
    }
    else if (type == "user-topup") {
        User user = client->user();
        const QString voucherCode = message.toString();
        QSqlDatabase db = QSqlDatabase::database();
        QSqlQuery q(db);

        if (user.isMember()) {
            // pastikan user ada
            q.prepare("select count(0) from members where username=?");
            q.bindValue(0, user.username());
            if (!q.exec()) {
                sendMessage(socket, "user-topup-failed", "Topup voucher gagal, kesalahan pada database server!");
                return;
            }

            if (!q.next()) {
                sendMessage(socket, "user-topup-failed", "Akun pengguna tidak ditemukan.");
                return;
            }

            // pastikan kode voucher ada
            q.prepare("select * from vouchers where code=?");
            q.bindValue(0, voucherCode);
            if (!q.exec()) {
                qCritical() << "Database error:" << qPrintable(q.lastError().text());
                sendMessage(socket, "user-topup-failed", "Topup voucher gagal, kesalahan pada database server!");
                return;
            }

            if (!q.next()) {
                sendMessage(socket, "user-topup-failed", "Kode voucher tidak ditemukan.");
                return;
            }

            // pastikan bukan voucher bekas pakai
            if (q.value("is_used").toBool()) {
                sendMessage(socket, "user-topup-failed", "Kode voucher bekas tidak dapat dipakai.");
                return;
            }

            // cek status kadaluarsa
            const QDateTime now = QDateTime::currentDateTime();
            const QDateTime expirationDatetime = q.value("expiration_datetime").toDateTime();
            if (expirationDatetime < now) {
                sendMessage(socket, "user-topup-failed", "Voucher sudah kadaluarsa sejak " + expirationDatetime.toString("dddd, dd MMMM yyyy hh:mm:ss") + ".");
                return;
            }

            // cek sedang dipakai
            int activeClientId = q.value("client_id").toInt();
            if (activeClientId) {
                sendMessage(socket, "user-topup-failed", "Voucher sedang digunakan di Client " + QString::number(activeClientId) + ".");
                return;
            }

            // cek sisa durasi
            int voucherDuration = q.value("duration").toInt();
            if (voucherDuration <= 0) {
                sendMessage(socket, "user-topup-failed", "Sisa waktu voucher telah habis.");
                return;
            }

            // tambahkan durasi akun user sesuai voucher
            db.transaction();
            q.prepare("update members set duration=? where id=?");
            q.bindValue(0, voucherDuration + user.duration());
            q.bindValue(1, user.id());
            if (!q.exec()) {
                db.rollback();
                sendMessage(socket, "user-topup-failed", "Topup voucher gagal, kesalahan pada database server!");
                return;
            }

            // hapus voucher dari daftar
            q.prepare("delete from vouchers where code=?");
            q.bindValue(0, voucherCode);
            if (!q.exec()) {
                db.rollback();
                sendMessage(socket, "user-topup-failed", "Topup voucher gagal, kesalahan pada database server!");
                return;
            }

            if (!db.commit()) {
                db.rollback();
                sendMessage(socket, "user-topup-failed", "Topup voucher gagal, kesalahan pada database server!");
                return;
            }

            client->topupVoucher(Voucher(voucherCode, voucherDuration));

            qDebug() << "member topup success:" << user.username() << client->id() << voucherCode;

            sendMessage(socket, "user-topup-success", voucherDuration);
            sendToClientMonitors("user-topup-success", QVariantMap({
                { "client", client->id() },
                { "username", user.username() },
                { "duration", voucherDuration },
            }));
        }
        else {
            q.prepare("select * from vouchers where code=?");
            q.bindValue(0, voucherCode);
            if (!q.exec()) {
                qCritical() << "Database error:" << qPrintable(q.lastError().text());
                sendMessage(socket, "user-topup-failed", "Topup voucher gagal, kesalahan pada database server!");
                return;
            }

            // pastikan kode voucher ada
            if (!q.next()) {
                sendMessage(socket, "user-topup-failed", "Kode voucher tidak ditemukan.");
                return;
            }

            // cek status kadaluarsa
            const QDateTime now = QDateTime::currentDateTime();
            const QDateTime expirationDatetime = q.value("expiration_datetime").toDateTime();
            if (expirationDatetime < now) {
                sendMessage(socket, "user-topup-failed", "Voucher sudah kadaluarsa sejak "
                            + expirationDatetime.toString("dddd, dd MMMM yyyy hh:mm:ss") + ".");
                return;
            }

            // cek sedang dipakai
            int activeClientId = q.value("client_id").toInt();
            if (activeClientId) {
                sendMessage(socket, "user-topup-failed", "Voucher sedang digunakan di Client " + QString::number(activeClientId) + ".");
                return;
            }

            // cek sisa durasi
            int voucherDuration = q.value("duration").toInt();
            if (voucherDuration <= 0) {
                sendMessage(socket, "user-topup-failed", "Sisa waktu sudah habis.");
                return;
            }

            // update session di database
            q.prepare("update vouchers set client_id=?, is_used=1 where code=?");
            q.bindValue(0, client->id());
            q.bindValue(1, voucherCode);
            if (!q.exec()) {
                qCritical() << "Database error:" << qPrintable(q.lastError().text());
                return;
            }

            // topup voucher
            client->topupVoucher(Voucher(voucherCode, voucherDuration));

            qDebug() << "voucher topup success:" << voucherCode << client->id();

            sendMessage(socket, "user-topup-success", voucherDuration);

            sendToClientMonitors("user-topup", QVariantMap({
                { "client", client->id() },
                { "username", user.username() },
                { "duration", voucherDuration },
            }));
        }
    }
}

void Server::processClientMonitorMessage(QWebSocket* socket, const QString& msgType, const QVariant& message)
{
    if (msgType == "init") {
        QVariantList clientList;

        for (Client* client: clients) {
            QVariantMap clientData;
            clientData["id"] = client->id();
            clientData["connected"] = clientSocketsByIds.value(client->id()) != 0;
            clientData["duration"] = client->user().duration();
            clientData["username"] = client->user().username();
            clientList.append(clientData);
        }

        sendMessage(socket, "init", QVariantMap({
            { "company", QVariantMap({
                { "name", settings.value("Company/name") },
                { "address", settings.value("Company/address") },
            })},
            { "clients", clientList }
        }));
    }
    else if (msgType == "stop-sessions") {
        for (const QVariant id: message.toList()) {
            QWebSocket* socket = clientSocketsByIds.value(id.toInt());
            if (socket) {
                // forward ke processClientMessage
                processClientMessage(socket, "session-stop", QVariant());
            }
        }
    }
    else if (msgType == "shutdown-clients" || msgType == "restart-clients") {
        for (const QVariant id: message.toList()) {
            QWebSocket* socket = clientSocketsByIds.value(id.toInt());
            if (socket) {
                sendMessage(socket, "system-" + msgType.split("-").first());
            }
        }
    }
}

void Server::sendToClientMonitors(const QString& type, const QVariant& data)
{
    for (QWebSocket* socket: clientMonitorSockets)
        sendMessage(socket, type, data);
}

void Server::sendToClients(const QString& type, const QVariant& data)
{
    for (QWebSocket* socket: clientSockets)
        sendMessage(socket, type, data);
}

void Server::sendMessage(QWebSocket* socket, const QString& type, const QVariant& message)
{
    QString textMessage = QJsonDocument::fromVariant(QVariantList({ type, message })).toJson(QJsonDocument::Compact);
    qDebug() << "sending message" << qPrintable(textMessage);
    socket->sendTextMessage(textMessage);
    socket->flush();
}

bool Server::resetClientSession(Client* client)
{
    QString table = client->user().isMember() ? "members" : "vouchers";

    QSqlQuery q(QSqlDatabase::database());
    q.prepare("update " + table + " set client_id=0 where client_id=?");
    q.bindValue(0, client->id());
    if (!q.exec())
        return false;

    client->stopSession();
    return true;
}

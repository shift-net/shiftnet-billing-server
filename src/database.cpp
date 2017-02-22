#include "database.h"
#include "user.h"
#include "voucher.h"

#include <QSqlDatabase>
#include <QSqlRecord>
#include <QSqlError>
#include <QSqlQuery>
#include <QSettings>
#include <QDebug>
#include <QDateTime>

#define LOG_DB_ERROR(obj) qCritical() << Q_FUNC_INFO << __FILE__ << __LINE__\
    << "Database Error:" << qPrintable(obj.lastError().text())

using namespace shiftnet;

void Database::setup(QSettings &settings)
{
    settings.beginGroup("Databases");
    QSqlDatabase db = QSqlDatabase::addDatabase(settings.value("main.driver").toString());
    db.setHostName(settings.value("main.host").toString());
    db.setPort(settings.value("main.port").toInt());
    db.setUserName(settings.value("main.username").toString());
    db.setPassword(settings.value("main.password").toString());
    db.setDatabaseName(settings.value("main.schema").toString());
    settings.endGroup();
}

QList<QSqlRecord> Database::clients()
{
    QList<QSqlRecord> clients;

    QSqlQuery q(QSqlDatabase::database());
    q.prepare("select * from shiftnet_clients order by id asc");
    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return clients;
    }

    while (q.next())
        clients << q.record();

    return clients;
}

bool Database::init()
{
    QSqlDatabase db = QSqlDatabase::database();
    QSqlQuery q(db);

    if (!db.isOpen()) {
        LOG_DB_ERROR(db);
        return false;
    }

    QList<quint64> expiredVoucherIds;
    q.prepare("select"
              " t.id, t.expirationDateTime"
              " from shiftnet_active_vouchers a"
              " inner join shiftnet_voucher_transactions t"
              "   on t.id = a.voucherId");

    if (!q.exec()) {
        LOG_DB_ERROR(db);
        return false;
    }

    QDateTime now = QDateTime::currentDateTime();
    while (q.next()) {
        QDateTime dateTime = q.value("expirationDateTime").toDateTime();

        if (dateTime < now) {
            expiredVoucherIds << q.value("id").value<quint64>();
        }
    }

    if (!db.transaction()) {
        LOG_DB_ERROR(db);
        return false;
    }

    // reset activeClientId
    if (!q.exec("update shiftnet_members set activeClientId=null where 1")) {
        LOG_DB_ERROR(q);
        db.rollback();
        return false;
    }

    // delete empty duration voucher
    q.prepare("delete from shiftnet_active_vouchers where remainingDuration<=0");
    if (!q.exec()) {
        LOG_DB_ERROR(q);
        db.rollback();
        return false;
    }

    // delete expired vouchers
    for (quint64 voucherId: expiredVoucherIds) {
        q.prepare("delete from shiftnet_active_vouchers where voucherId=?");
        q.bindValue(0, voucherId);
        if (!q.exec()) {
            LOG_DB_ERROR(q);
            db.rollback();
            return false;
        }
    }

    if (!q.exec("update shiftnet_active_vouchers set activeClientId=null where 1")) {
        LOG_DB_ERROR(q);
        return false;
    }

    if (!db.commit()) {
        LOG_DB_ERROR(db);
        return false;
    }

    return true;
}

bool Database::deleteVoucher(const QString &code)
{
    QSqlQuery q(QSqlDatabase::database());
    q.prepare("delete from shiftnet_active_vouchers where code=?");
    q.bindValue(0, code);
    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return false;
    }

    return q.numRowsAffected() > 0;
}

bool Database::updateMemberDuration(int id, int duration)
{
    QSqlQuery q(QSqlDatabase::database());

    q.prepare("update shiftnet_members set remainingDuration=? where id=?");
    q.bindValue(0, duration);
    q.bindValue(1, id);

    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return false;
    }

    return q.numRowsAffected() > 0;
}

bool Database::updateVoucherDuration(const QString& code, int duration)
{
    QSqlQuery q(QSqlDatabase::database());

    q.prepare("update shiftnet_active_vouchers set remainingDuration=? where code=?");
    q.bindValue(0, duration);
    q.bindValue(1, code);

    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return false;
    }

    return q.numRowsAffected() > 0;
}

bool Database::resetVoucherClientState(int id)
{
    QSqlQuery q(QSqlDatabase::database());
    q.prepare("update shiftnet_active_vouchers set activeClientId=null where activeClientId=?");
    q.bindValue(0, id);
    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return false;
    }

    return q.numRowsAffected();
}

bool Database::resetMemberClientState(int memberId)
{
    QSqlQuery q(QSqlDatabase::database());
    q.prepare("update shiftnet_members set activeClientId=null where id=?");
    q.bindValue(0, memberId);
    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return false;
    }

    return q.numRowsAffected();
}

QSqlRecord Database::findVoucher(const QString &code)
{
    QSqlQuery q(QSqlDatabase::database());
    q.prepare("select a.code, a.lastActiveUsername, a.remainingDuration, a.activeClientId, t.id, t.expirationDateTime"
              " from shiftnet_active_vouchers a"
              " inner join shiftnet_voucher_transactions t on t.id = a.voucherId"
              " where a.code=?");
    q.bindValue(0, code);
    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return QSqlRecord();
    }

    if (!q.next()) {
        return QSqlRecord();
    }

    return q.record();
}

bool Database::useVoucher(const QString &code, int clientId, const QString& username)
{
    QSqlQuery q(QSqlDatabase::database());
    q.prepare("update shiftnet_active_vouchers set activeClientId=?, lastActiveUsername=? where code=?");
    q.bindValue(0, clientId);
    q.bindValue(1, username);
    q.bindValue(2, code);
    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return false;
    }

    return q.numRowsAffected();
}

bool Database::topupMemberVoucher(int userId, int memberDuration, const QString &voucherCode, int voucherDuration)
{
    QSqlDatabase db = QSqlDatabase::database();
    db.transaction();
    if (!updateMemberDuration(userId, memberDuration + voucherDuration)) {
        db.rollback();
        return false;
    }

    if (!deleteVoucher(voucherCode)) {
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        LOG_DB_ERROR(db);
        db.rollback();
        return false;
    }

    return true;
}

QSqlRecord Database::findMember(const QString &username)
{
    QSqlQuery q(QSqlDatabase::database());
    q.prepare("select id, username, password, active, remainingDuration, activeClientId"
              " from shiftnet_members where username=?");
    q.bindValue(0, username);
    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return QSqlRecord();
    }

    if (!q.next()) return QSqlRecord();

    return q.record();
}

bool Database::setMemberClientId(int memberId, int clientId)
{
    QSqlQuery q(QSqlDatabase::database());
    q.prepare("update shiftnet_members set activeClientId=? where id=?");
    q.bindValue(0, clientId);
    q.bindValue(1, memberId);
    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return false;
    }
    return true;
}

bool Database::topupVoucher(int clientId, const User& user, const Voucher& voucher)
{
    if (user.isMember())
        return topupMemberVoucher(user.id(), user.duration(), voucher.code(), voucher.duration());

    if (user.isGuest())
        return useVoucher(voucher.code(), clientId, user.username());

    return false;
}

bool Database::logUserActivity(int clientId, const User& user, const QString& activity, const QString &text, quint64 voucherId)
{
    QSqlQuery q(QSqlDatabase::database());
    q.prepare("insert into shiftnet_activities"
              "( dateTime, groupId, clientId, memberId, voucherId, username, type, detail)"
              " values "
              "(:dateTime,:groupId,:clientId,:memberId,:voucherId,:username,:type,:detail)");
    q.bindValue(":dateTime", QDateTime::currentDateTime());
    q.bindValue(":clientId", clientId);
    q.bindValue(":memberId", user.isMember() ? user.id() : QVariant());
    q.bindValue(":voucherId", voucherId ? voucherId : QVariant());
    q.bindValue(":groupId", user.group());
    q.bindValue(":username", user.username());
    q.bindValue(":type", activity);
    q.bindValue(":detail", text);

    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return false;
    }
    return true;
}

bool Database::transaction()
{
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) {
        LOG_DB_ERROR(db);
        return false;
    }
    return true;
}

bool Database::commit()
{
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.commit()) {
        LOG_DB_ERROR(db);
        db.rollback();
        return false;
    }
    return true;
}

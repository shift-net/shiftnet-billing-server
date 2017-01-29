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

bool Database::init()
{
    QSqlDatabase db = QSqlDatabase::database();
    QSqlQuery q(db);

    if (!db.isOpen()) {
        LOG_DB_ERROR(db);
        return false;
    }

    if (!db.transaction()) {
        LOG_DB_ERROR(db);
        return false;
    }

    if (!q.exec("update members set client_id=0 where 1")) {
        LOG_DB_ERROR(q);
        return false;
    }

    q.prepare("delete from vouchers where duration<=0 or expiration_datetime<=?");
    q.bindValue(0, QDateTime::currentDateTime());
    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return false;
    }

    if (!q.exec("update vouchers set client_id=0 where 1")) {
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
    q.prepare("delete from vouchers where code=?");
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

    q.prepare("update members set duration=? where id=?");
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

    q.prepare("update vouchers set duration=? where code=?");
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
    q.prepare("update vouchers set client_id=0 where client_id=?");
    q.bindValue(0, id);
    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return false;
    }

    return q.numRowsAffected();
}

bool Database::resetMemberClientState(int id)
{
    QSqlQuery q(QSqlDatabase::database());
    q.prepare("update members set client_id=0 where client_id=?");
    q.bindValue(0, id);
    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return false;
    }

    return q.numRowsAffected();
}

QSqlRecord Database::findVoucher(const QString &code)
{
    QSqlQuery q(QSqlDatabase::database());
    q.prepare("select * from vouchers where code=?");
    q.bindValue(0, code);
    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return QSqlRecord();
    }

    if (!q.next()) {
        LOG_DB_ERROR(q);
        return QSqlRecord();
    }

    return q.record();
}

bool Database::useVoucher(const QString &code, int clientId)
{
    QSqlQuery q(QSqlDatabase::database());
    q.prepare("update vouchers set client_id=?, is_used=1 where code=?");
    q.bindValue(0, clientId);
    q.bindValue(1, code);
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
    q.prepare("select * from members where username=?");
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
    q.prepare("update members set client_id=? where id=?");
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
        return useVoucher(voucher.code(), clientId);

    return false;
}

bool Database::logUserActivity(int clientId, const User& user, const QString& activity, const QString &text)
{
    QSqlQuery q(QSqlDatabase::database());
    q.prepare("insert into user_activities"
              "( date_time, client_id, user_id, user_group, user_username, activity_type, activity_detail)"
              "values"
              "(:date_time,:client_id,:user_id,:user_group,:user_username,:activity_type,:activity_detail)");
    q.bindValue(":date_time", QDateTime::currentDateTime());
    q.bindValue(":client_id", clientId);
    q.bindValue(":user_id", user.id());
    q.bindValue(":user_group", user.group());
    q.bindValue(":user_username", user.username());
    q.bindValue(":activity_type", activity);
    q.bindValue(":activity_detail", text);
    if (!q.exec()) {
        LOG_DB_ERROR(q);
        return false;
    }
    return true;
}

#include "vouchervalidator.h"
#include "database.h"
#include <QSqlRecord>
#include <QDateTime>
#include <QVariant>

using namespace shiftnet;

bool VoucherValidator::isValid(const QString& code, bool checkUsedVoucher)
{
    QSqlRecord record = Database::findVoucher(code);
    if (record.isEmpty()) {
        _error = "Voucher tidak ditemukan";
        return false;
    }

    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime expirationDatetime = record.value("expiration_datetime").toDateTime();
    if (expirationDatetime < now) {
        _error = "Voucher sudah kadaluarsa sejak " + expirationDatetime.toString("dddd, dd MMMM yyyy hh:mm:ss") + ".";
        return false;
    }

    if (checkUsedVoucher && record.value("is_used").toBool()) {
        _error = "Kode voucher bekas tidak dapat dipakai.";
        return false;
    }

    int activeClientId = record.value("client_id").toInt();
    if (activeClientId) {
        _error = "Voucher sedang digunakan di Client " + QString::number(activeClientId) + ".";
        return false;
    }

    int duration = record.value("duration").toInt();
    if (duration <= 0) {
        _error = "Sisa waktu telah habis.";
        return false;
    }

    _voucher = Voucher(record.value("code").toString(), record.value("duration").toInt());

    return true;
}

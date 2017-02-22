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
    const QDateTime expirationDateTime = record.value("expirationDateTime").toDateTime();
    if (expirationDateTime < now) {
        _error = "Voucher sudah kadaluarsa sejak " + expirationDateTime.toString("dddd, dd MMMM yyyy hh:mm:ss") + ".";
        return false;
    }

    if (checkUsedVoucher && !record.value("lastActiveUsername").toString().isEmpty()) {
        _error = "Kode voucher bekas tidak dapat dipakai.";
        return false;
    }

    int activeClientId = record.value("activeClientId").toInt();
    if (activeClientId) {
        _error = "Voucher sedang digunakan di Client " + QString::number(activeClientId) + ".";
        return false;
    }

    int duration = record.value("remainingDuration").toInt();
    if (duration <= 0) {
        _error = "Sisa waktu telah habis.";
        return false;
    }

    _voucher = Voucher(record.value("code").toString(), record.value("remainingDuration").toInt(), record.value("id").value<quint64>());

    return true;
}

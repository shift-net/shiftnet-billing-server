#ifndef DATABASE_H
#define DATABASE_H

class QSettings;
class QString;
class QSqlRecord;

namespace shiftnet {

class Database
{
public:
    static void setup(QSettings& settings);
    static bool init();

    static bool useVoucher(const QString& code, int clientId);
    static bool deleteVoucher(const QString& code);
    static bool updateMemberDuration(int memberId, int duration);
    static bool updateVoucherDuration(const QString& code, int duration);
    static bool resetVoucherClientState(int clientId);
    static bool resetMemberClientState(int memberId);
    static bool setMemberClientId(int memberId, int clientId);

    static bool topupMemberVoucher(int userId, int memberDuration,
                                   const QString& voucherCode, int duration);

    static QSqlRecord findVoucher(const QString& code);
    static QSqlRecord findMember(const QString& username);

private:
    Database();
};

}

#endif // DATABASE_H

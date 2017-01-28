#ifndef VOUCHERVALIDATOR_H
#define VOUCHERVALIDATOR_H

#include "voucher.h"

namespace shiftnet {

class VoucherValidator
{
public:
    inline VoucherValidator() {}
    bool isValid(const QString& code, bool checkUsedVoucher);
    inline QString error() const { return _error; }
    inline Voucher voucher() const { return _voucher; }

private:
    Voucher _voucher;
    QString _error;
};


}

#endif // VOUCHERVALIDATOR_H

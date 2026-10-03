#include <cstdint>
#include <cstring>
#include <new>
#include "dci_msvc_failure.hpp"
#include "E:/Qt/6.7.3/msvc2019_64/include/QtCore/qstring.h"
#include "E:/Qt/6.7.3/msvc2019_64/include/QtCore/qcoreapplication.h"
#include "E:/Qt/6.7.3/msvc2019_64/include/QtCore/qobject.h"
#include "E:/Qt/6.7.3/msvc2019_64/include/QtWidgets/qabstractbutton.h"
#include "E:/Qt/6.7.3/msvc2019_64/include/QtWidgets/qpushbutton.h"
#include "E:/Qt/6.7.3/msvc2019_64/include/QtWidgets/qlcdnumber.h"
#include "E:/Qt/6.7.3/msvc2019_64/include/QtWidgets/qwidget.h"
#include "E:/Qt/6.7.3/msvc2019_64/include/QtWidgets/qapplication.h"
#include "E:/Dev/C++/VyxLan-selfhost-yolo/probes/gates/dci-qt-counter/native/qt_widgets_dci.hpp"

struct dci_Translated_dci_tr_resize_QString_QEAAX_J_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_resize_QString_QEAAX_J_Z dci_tr_resize_QString_QEAAX_J_Z(QString* self, qsizetype p0) noexcept {
    dci_Translated_dci_tr_resize_QString_QEAAX_J_Z out{};
    try {
        self->resize(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_resize_QString_QEAAX_J_Z_destroy(dci_Translated_dci_tr_resize_QString_QEAAX_J_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_truncate_QString_QEAAX_J_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_truncate_QString_QEAAX_J_Z dci_tr_truncate_QString_QEAAX_J_Z(QString* self, qsizetype p0) noexcept {
    dci_Translated_dci_tr_truncate_QString_QEAAX_J_Z out{};
    try {
        self->truncate(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_truncate_QString_QEAAX_J_Z_destroy(dci_Translated_dci_tr_truncate_QString_QEAAX_J_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_chop_QString_QEAAX_J_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_chop_QString_QEAAX_J_Z dci_tr_chop_QString_QEAAX_J_Z(QString* self, qsizetype p0) noexcept {
    dci_Translated_dci_tr_chop_QString_QEAAX_J_Z out{};
    try {
        self->chop(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_chop_QString_QEAAX_J_Z_destroy(dci_Translated_dci_tr_chop_QString_QEAAX_J_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_reserve_QString_QEAAX_J_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_reserve_QString_QEAAX_J_Z dci_tr_reserve_QString_QEAAX_J_Z(QString* self, qsizetype p0) noexcept {
    dci_Translated_dci_tr_reserve_QString_QEAAX_J_Z out{};
    try {
        self->reserve(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_reserve_QString_QEAAX_J_Z_destroy(dci_Translated_dci_tr_reserve_QString_QEAAX_J_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_squeeze_QString_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_squeeze_QString_QEAAXXZ dci_tr_squeeze_QString_QEAAXXZ(QString* self) noexcept {
    dci_Translated_dci_tr_squeeze_QString_QEAAXXZ out{};
    try {
        self->squeeze();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_squeeze_QString_QEAAXXZ_destroy(dci_Translated_dci_tr_squeeze_QString_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_detach_QString_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_detach_QString_QEAAXXZ dci_tr_detach_QString_QEAAXXZ(QString* self) noexcept {
    dci_Translated_dci_tr_detach_QString_QEAAXXZ out{};
    try {
        self->detach();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_detach_QString_QEAAXXZ_destroy(dci_Translated_dci_tr_detach_QString_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isDetached_QString_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isDetached_QString_QEBA_NXZ dci_tr_isDetached_QString_QEBA_NXZ(const QString* self) noexcept {
    dci_Translated_dci_tr_isDetached_QString_QEBA_NXZ out{};
    try {
        out.ok = self->isDetached();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isDetached_QString_QEBA_NXZ_destroy(dci_Translated_dci_tr_isDetached_QString_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isSharedWith_QString_QEBA_NAEBV1_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isSharedWith_QString_QEBA_NAEBV1_Z dci_tr_isSharedWith_QString_QEBA_NAEBV1_Z(const QString* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_isSharedWith_QString_QEBA_NAEBV1_Z out{};
    try {
        out.ok = self->isSharedWith(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isSharedWith_QString_QEBA_NAEBV1_Z_destroy(dci_Translated_dci_tr_isSharedWith_QString_QEBA_NAEBV1_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_clear_QString_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_clear_QString_QEAAXXZ dci_tr_clear_QString_QEAAXXZ(QString* self) noexcept {
    dci_Translated_dci_tr_clear_QString_QEAAXXZ out{};
    try {
        self->clear();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_clear_QString_QEAAXXZ_destroy(dci_Translated_dci_tr_clear_QString_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_contains_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_contains_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z dci_tr_contains_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z(const QString* self, const QString& p0, Qt::CaseSensitivity p1) noexcept {
    dci_Translated_dci_tr_contains_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z out{};
    try {
        out.ok = self->contains(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_contains_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z_destroy(dci_Translated_dci_tr_contains_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_startsWith_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_startsWith_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z dci_tr_startsWith_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z(const QString* self, const QString& p0, Qt::CaseSensitivity p1) noexcept {
    dci_Translated_dci_tr_startsWith_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z out{};
    try {
        out.ok = self->startsWith(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_startsWith_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z_destroy(dci_Translated_dci_tr_startsWith_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_endsWith_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_endsWith_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z dci_tr_endsWith_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z(const QString* self, const QString& p0, Qt::CaseSensitivity p1) noexcept {
    dci_Translated_dci_tr_endsWith_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z out{};
    try {
        out.ok = self->endsWith(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_endsWith_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z_destroy(dci_Translated_dci_tr_endsWith_QString_QEBA_NAEBV1_W4CaseSensitivity_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isUpper_QString_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isUpper_QString_QEBA_NXZ dci_tr_isUpper_QString_QEBA_NXZ(const QString* self) noexcept {
    dci_Translated_dci_tr_isUpper_QString_QEBA_NXZ out{};
    try {
        out.ok = self->isUpper();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isUpper_QString_QEBA_NXZ_destroy(dci_Translated_dci_tr_isUpper_QString_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isLower_QString_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isLower_QString_QEBA_NXZ dci_tr_isLower_QString_QEBA_NXZ(const QString* self) noexcept {
    dci_Translated_dci_tr_isLower_QString_QEBA_NXZ out{};
    try {
        out.ok = self->isLower();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isLower_QString_QEBA_NXZ_destroy(dci_Translated_dci_tr_isLower_QString_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_localeAwareCompare_QString_QEBAHAEBV1_Z {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_localeAwareCompare_QString_QEBAHAEBV1_Z dci_tr_localeAwareCompare_QString_QEBAHAEBV1_Z(const QString* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_localeAwareCompare_QString_QEBAHAEBV1_Z out{};
    try {
        out.ok = self->localeAwareCompare(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_localeAwareCompare_QString_QEBAHAEBV1_Z_destroy(dci_Translated_dci_tr_localeAwareCompare_QString_QEBAHAEBV1_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_localeAwareCompare_QString_SAHAEBV1_0_Z {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_localeAwareCompare_QString_SAHAEBV1_0_Z dci_tr_localeAwareCompare_QString_SAHAEBV1_0_Z(const QString& p0, const QString& p1) noexcept {
    dci_Translated_dci_tr_localeAwareCompare_QString_SAHAEBV1_0_Z out{};
    try {
        out.ok = QString::localeAwareCompare(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_localeAwareCompare_QString_SAHAEBV1_0_Z_destroy(dci_Translated_dci_tr_localeAwareCompare_QString_SAHAEBV1_0_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_0QString_QEAA_PEBD_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_0QString_QEAA_PEBD_Z dci_tr_x_0QString_QEAA_PEBD_Z(QString* self, const char * p0) noexcept {
    dci_Translated_dci_tr_x_0QString_QEAA_PEBD_Z out{};
    try {
        ::new (static_cast<void*>(self)) QString(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_0QString_QEAA_PEBD_Z_destroy(dci_Translated_dci_tr_x_0QString_QEAA_PEBD_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_0QString_QEAA_AEBVQByteArray_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_0QString_QEAA_AEBVQByteArray_Z dci_tr_x_0QString_QEAA_AEBVQByteArray_Z(QString* self, const QByteArray& p0) noexcept {
    dci_Translated_dci_tr_x_0QString_QEAA_AEBVQByteArray_Z out{};
    try {
        ::new (static_cast<void*>(self)) QString(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_0QString_QEAA_AEBVQByteArray_Z_destroy(dci_Translated_dci_tr_x_0QString_QEAA_AEBVQByteArray_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_8QString_QEBA_NAEBVQByteArray_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_8QString_QEBA_NAEBVQByteArray_Z dci_tr_x_8QString_QEBA_NAEBVQByteArray_Z(const QString* self, const QByteArray& p0) noexcept {
    dci_Translated_dci_tr_x_8QString_QEBA_NAEBVQByteArray_Z out{};
    try {
        out.ok = (self->operator==(p0));
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_8QString_QEBA_NAEBVQByteArray_Z_destroy(dci_Translated_dci_tr_x_8QString_QEBA_NAEBVQByteArray_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_9QString_QEBA_NAEBVQByteArray_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_9QString_QEBA_NAEBVQByteArray_Z dci_tr_x_9QString_QEBA_NAEBVQByteArray_Z(const QString* self, const QByteArray& p0) noexcept {
    dci_Translated_dci_tr_x_9QString_QEBA_NAEBVQByteArray_Z out{};
    try {
        out.ok = (self->operator!=(p0));
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_9QString_QEBA_NAEBVQByteArray_Z_destroy(dci_Translated_dci_tr_x_9QString_QEBA_NAEBVQByteArray_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_MQString_QEBA_NAEBVQByteArray_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_MQString_QEBA_NAEBVQByteArray_Z dci_tr_MQString_QEBA_NAEBVQByteArray_Z(const QString* self, const QByteArray& p0) noexcept {
    dci_Translated_dci_tr_MQString_QEBA_NAEBVQByteArray_Z out{};
    try {
        out.ok = (self->operator<(p0));
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_MQString_QEBA_NAEBVQByteArray_Z_destroy(dci_Translated_dci_tr_MQString_QEBA_NAEBVQByteArray_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_OQString_QEBA_NAEBVQByteArray_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_OQString_QEBA_NAEBVQByteArray_Z dci_tr_OQString_QEBA_NAEBVQByteArray_Z(const QString* self, const QByteArray& p0) noexcept {
    dci_Translated_dci_tr_OQString_QEBA_NAEBVQByteArray_Z out{};
    try {
        out.ok = (self->operator>(p0));
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_OQString_QEBA_NAEBVQByteArray_Z_destroy(dci_Translated_dci_tr_OQString_QEBA_NAEBVQByteArray_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_NQString_QEBA_NAEBVQByteArray_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_NQString_QEBA_NAEBVQByteArray_Z dci_tr_NQString_QEBA_NAEBVQByteArray_Z(const QString* self, const QByteArray& p0) noexcept {
    dci_Translated_dci_tr_NQString_QEBA_NAEBVQByteArray_Z out{};
    try {
        out.ok = (self->operator<=(p0));
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_NQString_QEBA_NAEBVQByteArray_Z_destroy(dci_Translated_dci_tr_NQString_QEBA_NAEBVQByteArray_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_PQString_QEBA_NAEBVQByteArray_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_PQString_QEBA_NAEBVQByteArray_Z dci_tr_PQString_QEBA_NAEBVQByteArray_Z(const QString* self, const QByteArray& p0) noexcept {
    dci_Translated_dci_tr_PQString_QEBA_NAEBVQByteArray_Z out{};
    try {
        out.ok = (self->operator>=(p0));
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_PQString_QEBA_NAEBVQByteArray_Z_destroy(dci_Translated_dci_tr_PQString_QEBA_NAEBVQByteArray_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_push_back_QString_QEAAXAEBV1_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_push_back_QString_QEAAXAEBV1_Z dci_tr_push_back_QString_QEAAXAEBV1_Z(QString* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_push_back_QString_QEAAXAEBV1_Z out{};
    try {
        self->push_back(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_push_back_QString_QEAAXAEBV1_Z_destroy(dci_Translated_dci_tr_push_back_QString_QEAAXAEBV1_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_push_front_QString_QEAAXAEBV1_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_push_front_QString_QEAAXAEBV1_Z dci_tr_push_front_QString_QEAAXAEBV1_Z(QString* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_push_front_QString_QEAAXAEBV1_Z out{};
    try {
        self->push_front(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_push_front_QString_QEAAXAEBV1_Z_destroy(dci_Translated_dci_tr_push_front_QString_QEAAXAEBV1_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_shrink_to_fit_QString_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_shrink_to_fit_QString_QEAAXXZ dci_tr_shrink_to_fit_QString_QEAAXXZ(QString* self) noexcept {
    dci_Translated_dci_tr_shrink_to_fit_QString_QEAAXXZ out{};
    try {
        self->shrink_to_fit();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_shrink_to_fit_QString_QEAAXXZ_destroy(dci_Translated_dci_tr_shrink_to_fit_QString_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isNull_QString_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isNull_QString_QEBA_NXZ dci_tr_isNull_QString_QEBA_NXZ(const QString* self) noexcept {
    dci_Translated_dci_tr_isNull_QString_QEBA_NXZ out{};
    try {
        out.ok = self->isNull();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isNull_QString_QEBA_NXZ_destroy(dci_Translated_dci_tr_isNull_QString_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isRightToLeft_QString_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isRightToLeft_QString_QEBA_NXZ dci_tr_isRightToLeft_QString_QEBA_NXZ(const QString* self) noexcept {
    dci_Translated_dci_tr_isRightToLeft_QString_QEBA_NXZ out{};
    try {
        out.ok = self->isRightToLeft();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isRightToLeft_QString_QEBA_NXZ_destroy(dci_Translated_dci_tr_isRightToLeft_QString_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_0QString_QEAA_JW4Initialization_Qt_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_0QString_QEAA_JW4Initialization_Qt_Z dci_tr_x_0QString_QEAA_JW4Initialization_Qt_Z(QString* self, qsizetype p0, Qt::Initialization p1) noexcept {
    dci_Translated_dci_tr_x_0QString_QEAA_JW4Initialization_Qt_Z out{};
    try {
        ::new (static_cast<void*>(self)) QString(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_0QString_QEAA_JW4Initialization_Qt_Z_destroy(dci_Translated_dci_tr_x_0QString_QEAA_JW4Initialization_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_0QStringViewArg_QtPrivate_QEAA_XZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_0QStringViewArg_QtPrivate_QEAA_XZ dci_tr_x_0QStringViewArg_QtPrivate_QEAA_XZ(QtPrivate::QStringViewArg* self) noexcept {
    dci_Translated_dci_tr_x_0QStringViewArg_QtPrivate_QEAA_XZ out{};
    try {
        ::new (static_cast<void*>(self)) QtPrivate::QStringViewArg();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_0QStringViewArg_QtPrivate_QEAA_XZ_destroy(dci_Translated_dci_tr_x_0QStringViewArg_QtPrivate_QEAA_XZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_0QLatin1StringArg_QtPrivate_QEAA_XZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_0QLatin1StringArg_QtPrivate_QEAA_XZ dci_tr_x_0QLatin1StringArg_QtPrivate_QEAA_XZ(QtPrivate::QLatin1StringArg* self) noexcept {
    dci_Translated_dci_tr_x_0QLatin1StringArg_QtPrivate_QEAA_XZ out{};
    try {
        ::new (static_cast<void*>(self)) QtPrivate::QLatin1StringArg();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_0QLatin1StringArg_QtPrivate_QEAA_XZ_destroy(dci_Translated_dci_tr_x_0QLatin1StringArg_QtPrivate_QEAA_XZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_0QCoreApplication_QEAA_AEAHPEAPEADH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_0QCoreApplication_QEAA_AEAHPEAPEADH_Z dci_tr_x_0QCoreApplication_QEAA_AEAHPEAPEADH_Z(QCoreApplication* self, int & p0, char ** p1, int p2) noexcept {
    dci_Translated_dci_tr_x_0QCoreApplication_QEAA_AEAHPEAPEADH_Z out{};
    try {
        ::new (static_cast<void*>(self)) QCoreApplication(p0, p1, p2);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_0QCoreApplication_QEAA_AEAHPEAPEADH_Z_destroy(dci_Translated_dci_tr_x_0QCoreApplication_QEAA_AEAHPEAPEADH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setAttribute_QCoreApplication_SAXW4ApplicationAttribute_Qt_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setAttribute_QCoreApplication_SAXW4ApplicationAttribute_Qt_N_Z dci_tr_setAttribute_QCoreApplication_SAXW4ApplicationAttribute_Qt_N_Z(Qt::ApplicationAttribute p0, bool p1) noexcept {
    dci_Translated_dci_tr_setAttribute_QCoreApplication_SAXW4ApplicationAttribute_Qt_N_Z out{};
    try {
        QCoreApplication::setAttribute(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setAttribute_QCoreApplication_SAXW4ApplicationAttribute_Qt_N_Z_destroy(dci_Translated_dci_tr_setAttribute_QCoreApplication_SAXW4ApplicationAttribute_Qt_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_testAttribute_QCoreApplication_SA_NW4ApplicationAttribute_Qt_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_testAttribute_QCoreApplication_SA_NW4ApplicationAttribute_Qt_Z dci_tr_testAttribute_QCoreApplication_SA_NW4ApplicationAttribute_Qt_Z(Qt::ApplicationAttribute p0) noexcept {
    dci_Translated_dci_tr_testAttribute_QCoreApplication_SA_NW4ApplicationAttribute_Qt_Z out{};
    try {
        out.ok = QCoreApplication::testAttribute(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_testAttribute_QCoreApplication_SA_NW4ApplicationAttribute_Qt_Z_destroy(dci_Translated_dci_tr_testAttribute_QCoreApplication_SA_NW4ApplicationAttribute_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setOrganizationDomain_QCoreApplication_SAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setOrganizationDomain_QCoreApplication_SAXAEBVQString_Z dci_tr_setOrganizationDomain_QCoreApplication_SAXAEBVQString_Z(const QString& p0) noexcept {
    dci_Translated_dci_tr_setOrganizationDomain_QCoreApplication_SAXAEBVQString_Z out{};
    try {
        QCoreApplication::setOrganizationDomain(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setOrganizationDomain_QCoreApplication_SAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setOrganizationDomain_QCoreApplication_SAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setOrganizationName_QCoreApplication_SAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setOrganizationName_QCoreApplication_SAXAEBVQString_Z dci_tr_setOrganizationName_QCoreApplication_SAXAEBVQString_Z(const QString& p0) noexcept {
    dci_Translated_dci_tr_setOrganizationName_QCoreApplication_SAXAEBVQString_Z out{};
    try {
        QCoreApplication::setOrganizationName(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setOrganizationName_QCoreApplication_SAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setOrganizationName_QCoreApplication_SAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setApplicationName_QCoreApplication_SAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setApplicationName_QCoreApplication_SAXAEBVQString_Z dci_tr_setApplicationName_QCoreApplication_SAXAEBVQString_Z(const QString& p0) noexcept {
    dci_Translated_dci_tr_setApplicationName_QCoreApplication_SAXAEBVQString_Z out{};
    try {
        QCoreApplication::setApplicationName(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setApplicationName_QCoreApplication_SAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setApplicationName_QCoreApplication_SAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setApplicationVersion_QCoreApplication_SAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setApplicationVersion_QCoreApplication_SAXAEBVQString_Z dci_tr_setApplicationVersion_QCoreApplication_SAXAEBVQString_Z(const QString& p0) noexcept {
    dci_Translated_dci_tr_setApplicationVersion_QCoreApplication_SAXAEBVQString_Z out{};
    try {
        QCoreApplication::setApplicationVersion(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setApplicationVersion_QCoreApplication_SAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setApplicationVersion_QCoreApplication_SAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setSetuidAllowed_QCoreApplication_SAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setSetuidAllowed_QCoreApplication_SAX_N_Z dci_tr_setSetuidAllowed_QCoreApplication_SAX_N_Z(bool p0) noexcept {
    dci_Translated_dci_tr_setSetuidAllowed_QCoreApplication_SAX_N_Z out{};
    try {
        QCoreApplication::setSetuidAllowed(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setSetuidAllowed_QCoreApplication_SAX_N_Z_destroy(dci_Translated_dci_tr_setSetuidAllowed_QCoreApplication_SAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isSetuidAllowed_QCoreApplication_SA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isSetuidAllowed_QCoreApplication_SA_NXZ dci_tr_isSetuidAllowed_QCoreApplication_SA_NXZ(void) noexcept {
    dci_Translated_dci_tr_isSetuidAllowed_QCoreApplication_SA_NXZ out{};
    try {
        out.ok = QCoreApplication::isSetuidAllowed();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isSetuidAllowed_QCoreApplication_SA_NXZ_destroy(dci_Translated_dci_tr_isSetuidAllowed_QCoreApplication_SA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_exec_QCoreApplication_SAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_exec_QCoreApplication_SAHXZ dci_tr_exec_QCoreApplication_SAHXZ(void) noexcept {
    dci_Translated_dci_tr_exec_QCoreApplication_SAHXZ out{};
    try {
        out.ok = QCoreApplication::exec();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_exec_QCoreApplication_SAHXZ_destroy(dci_Translated_dci_tr_exec_QCoreApplication_SAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_startingUp_QCoreApplication_SA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_startingUp_QCoreApplication_SA_NXZ dci_tr_startingUp_QCoreApplication_SA_NXZ(void) noexcept {
    dci_Translated_dci_tr_startingUp_QCoreApplication_SA_NXZ out{};
    try {
        out.ok = QCoreApplication::startingUp();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_startingUp_QCoreApplication_SA_NXZ_destroy(dci_Translated_dci_tr_startingUp_QCoreApplication_SA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_closingDown_QCoreApplication_SA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_closingDown_QCoreApplication_SA_NXZ dci_tr_closingDown_QCoreApplication_SA_NXZ(void) noexcept {
    dci_Translated_dci_tr_closingDown_QCoreApplication_SA_NXZ out{};
    try {
        out.ok = QCoreApplication::closingDown();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_closingDown_QCoreApplication_SA_NXZ_destroy(dci_Translated_dci_tr_closingDown_QCoreApplication_SA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setLibraryPaths_QCoreApplication_SAXAEBV_QList_VQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setLibraryPaths_QCoreApplication_SAXAEBV_QList_VQString_Z dci_tr_setLibraryPaths_QCoreApplication_SAXAEBV_QList_VQString_Z(const QList<QString>& p0) noexcept {
    dci_Translated_dci_tr_setLibraryPaths_QCoreApplication_SAXAEBV_QList_VQString_Z out{};
    try {
        QCoreApplication::setLibraryPaths(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setLibraryPaths_QCoreApplication_SAXAEBV_QList_VQString_Z_destroy(dci_Translated_dci_tr_setLibraryPaths_QCoreApplication_SAXAEBV_QList_VQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_addLibraryPath_QCoreApplication_SAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_addLibraryPath_QCoreApplication_SAXAEBVQString_Z dci_tr_addLibraryPath_QCoreApplication_SAXAEBVQString_Z(const QString& p0) noexcept {
    dci_Translated_dci_tr_addLibraryPath_QCoreApplication_SAXAEBVQString_Z out{};
    try {
        QCoreApplication::addLibraryPath(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_addLibraryPath_QCoreApplication_SAXAEBVQString_Z_destroy(dci_Translated_dci_tr_addLibraryPath_QCoreApplication_SAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_removeLibraryPath_QCoreApplication_SAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_removeLibraryPath_QCoreApplication_SAXAEBVQString_Z dci_tr_removeLibraryPath_QCoreApplication_SAXAEBVQString_Z(const QString& p0) noexcept {
    dci_Translated_dci_tr_removeLibraryPath_QCoreApplication_SAXAEBVQString_Z out{};
    try {
        QCoreApplication::removeLibraryPath(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_removeLibraryPath_QCoreApplication_SAXAEBVQString_Z_destroy(dci_Translated_dci_tr_removeLibraryPath_QCoreApplication_SAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isQuitLockEnabled_QCoreApplication_SA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isQuitLockEnabled_QCoreApplication_SA_NXZ dci_tr_isQuitLockEnabled_QCoreApplication_SA_NXZ(void) noexcept {
    dci_Translated_dci_tr_isQuitLockEnabled_QCoreApplication_SA_NXZ out{};
    try {
        out.ok = QCoreApplication::isQuitLockEnabled();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isQuitLockEnabled_QCoreApplication_SA_NXZ_destroy(dci_Translated_dci_tr_isQuitLockEnabled_QCoreApplication_SA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setQuitLockEnabled_QCoreApplication_SAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setQuitLockEnabled_QCoreApplication_SAX_N_Z dci_tr_setQuitLockEnabled_QCoreApplication_SAX_N_Z(bool p0) noexcept {
    dci_Translated_dci_tr_setQuitLockEnabled_QCoreApplication_SAX_N_Z out{};
    try {
        QCoreApplication::setQuitLockEnabled(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setQuitLockEnabled_QCoreApplication_SAX_N_Z_destroy(dci_Translated_dci_tr_setQuitLockEnabled_QCoreApplication_SAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_quit_QCoreApplication_SAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_quit_QCoreApplication_SAXXZ dci_tr_quit_QCoreApplication_SAXXZ(void) noexcept {
    dci_Translated_dci_tr_quit_QCoreApplication_SAXXZ out{};
    try {
        QCoreApplication::quit();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_quit_QCoreApplication_SAXXZ_destroy(dci_Translated_dci_tr_quit_QCoreApplication_SAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_exit_QCoreApplication_SAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_exit_QCoreApplication_SAXH_Z dci_tr_exit_QCoreApplication_SAXH_Z(int p0) noexcept {
    dci_Translated_dci_tr_exit_QCoreApplication_SAXH_Z out{};
    try {
        QCoreApplication::exit(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_exit_QCoreApplication_SAXH_Z_destroy(dci_Translated_dci_tr_exit_QCoreApplication_SAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_organizationNameChanged_QCoreApplication_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_organizationNameChanged_QCoreApplication_QEAAXXZ dci_tr_organizationNameChanged_QCoreApplication_QEAAXXZ(QCoreApplication* self) noexcept {
    dci_Translated_dci_tr_organizationNameChanged_QCoreApplication_QEAAXXZ out{};
    try {
        self->organizationNameChanged();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_organizationNameChanged_QCoreApplication_QEAAXXZ_destroy(dci_Translated_dci_tr_organizationNameChanged_QCoreApplication_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_organizationDomainChanged_QCoreApplication_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_organizationDomainChanged_QCoreApplication_QEAAXXZ dci_tr_organizationDomainChanged_QCoreApplication_QEAAXXZ(QCoreApplication* self) noexcept {
    dci_Translated_dci_tr_organizationDomainChanged_QCoreApplication_QEAAXXZ out{};
    try {
        self->organizationDomainChanged();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_organizationDomainChanged_QCoreApplication_QEAAXXZ_destroy(dci_Translated_dci_tr_organizationDomainChanged_QCoreApplication_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_applicationNameChanged_QCoreApplication_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_applicationNameChanged_QCoreApplication_QEAAXXZ dci_tr_applicationNameChanged_QCoreApplication_QEAAXXZ(QCoreApplication* self) noexcept {
    dci_Translated_dci_tr_applicationNameChanged_QCoreApplication_QEAAXXZ out{};
    try {
        self->applicationNameChanged();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_applicationNameChanged_QCoreApplication_QEAAXXZ_destroy(dci_Translated_dci_tr_applicationNameChanged_QCoreApplication_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_applicationVersionChanged_QCoreApplication_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_applicationVersionChanged_QCoreApplication_QEAAXXZ dci_tr_applicationVersionChanged_QCoreApplication_QEAAXXZ(QCoreApplication* self) noexcept {
    dci_Translated_dci_tr_applicationVersionChanged_QCoreApplication_QEAAXXZ out{};
    try {
        self->applicationVersionChanged();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_applicationVersionChanged_QCoreApplication_QEAAXXZ_destroy(dci_Translated_dci_tr_applicationVersionChanged_QCoreApplication_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_0QObjectData_QEAA_XZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_0QObjectData_QEAA_XZ dci_tr_x_0QObjectData_QEAA_XZ(QObjectData* self) noexcept {
    dci_Translated_dci_tr_x_0QObjectData_QEAA_XZ out{};
    try {
        ::new (static_cast<void*>(self)) QObjectData();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_0QObjectData_QEAA_XZ_destroy(dci_Translated_dci_tr_x_0QObjectData_QEAA_XZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_1QObjectData_UEAA_XZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_1QObjectData_UEAA_XZ dci_tr_x_1QObjectData_UEAA_XZ(QObjectData* self) noexcept {
    dci_Translated_dci_tr_x_1QObjectData_UEAA_XZ out{};
    try {
        self->~QObjectData();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_1QObjectData_UEAA_XZ_destroy(dci_Translated_dci_tr_x_1QObjectData_UEAA_XZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isWidgetType_QObject_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isWidgetType_QObject_QEBA_NXZ dci_tr_isWidgetType_QObject_QEBA_NXZ(const QObject* self) noexcept {
    dci_Translated_dci_tr_isWidgetType_QObject_QEBA_NXZ out{};
    try {
        out.ok = self->isWidgetType();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isWidgetType_QObject_QEBA_NXZ_destroy(dci_Translated_dci_tr_isWidgetType_QObject_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isWindowType_QObject_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isWindowType_QObject_QEBA_NXZ dci_tr_isWindowType_QObject_QEBA_NXZ(const QObject* self) noexcept {
    dci_Translated_dci_tr_isWindowType_QObject_QEBA_NXZ out{};
    try {
        out.ok = self->isWindowType();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isWindowType_QObject_QEBA_NXZ_destroy(dci_Translated_dci_tr_isWindowType_QObject_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isQuickItemType_QObject_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isQuickItemType_QObject_QEBA_NXZ dci_tr_isQuickItemType_QObject_QEBA_NXZ(const QObject* self) noexcept {
    dci_Translated_dci_tr_isQuickItemType_QObject_QEBA_NXZ out{};
    try {
        out.ok = self->isQuickItemType();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isQuickItemType_QObject_QEBA_NXZ_destroy(dci_Translated_dci_tr_isQuickItemType_QObject_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_startTimer_QObject_QEAAHHW4TimerType_Qt_Z {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_startTimer_QObject_QEAAHHW4TimerType_Qt_Z dci_tr_startTimer_QObject_QEAAHHW4TimerType_Qt_Z(QObject* self, int p0, Qt::TimerType p1) noexcept {
    dci_Translated_dci_tr_startTimer_QObject_QEAAHHW4TimerType_Qt_Z out{};
    try {
        out.ok = self->startTimer(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_startTimer_QObject_QEAAHHW4TimerType_Qt_Z_destroy(dci_Translated_dci_tr_startTimer_QObject_QEAAHHW4TimerType_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_killTimer_QObject_QEAAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_killTimer_QObject_QEAAXH_Z dci_tr_killTimer_QObject_QEAAXH_Z(QObject* self, int p0) noexcept {
    dci_Translated_dci_tr_killTimer_QObject_QEAAXH_Z out{};
    try {
        self->killTimer(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_killTimer_QObject_QEAAXH_Z_destroy(dci_Translated_dci_tr_killTimer_QObject_QEAAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_disconnect_QObject_SA_NAEBVConnection_QMetaObject_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_disconnect_QObject_SA_NAEBVConnection_QMetaObject_Z dci_tr_disconnect_QObject_SA_NAEBVConnection_QMetaObject_Z(const QMetaObject::Connection& p0) noexcept {
    dci_Translated_dci_tr_disconnect_QObject_SA_NAEBVConnection_QMetaObject_Z out{};
    try {
        out.ok = QObject::disconnect(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_disconnect_QObject_SA_NAEBVConnection_QMetaObject_Z_destroy(dci_Translated_dci_tr_disconnect_QObject_SA_NAEBVConnection_QMetaObject_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_dumpObjectTree_QObject_QEBAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_dumpObjectTree_QObject_QEBAXXZ dci_tr_dumpObjectTree_QObject_QEBAXXZ(const QObject* self) noexcept {
    dci_Translated_dci_tr_dumpObjectTree_QObject_QEBAXXZ out{};
    try {
        self->dumpObjectTree();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_dumpObjectTree_QObject_QEBAXXZ_destroy(dci_Translated_dci_tr_dumpObjectTree_QObject_QEBAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_dumpObjectInfo_QObject_QEBAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_dumpObjectInfo_QObject_QEBAXXZ dci_tr_dumpObjectInfo_QObject_QEBAXXZ(const QObject* self) noexcept {
    dci_Translated_dci_tr_dumpObjectInfo_QObject_QEBAXXZ out{};
    try {
        self->dumpObjectInfo();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_dumpObjectInfo_QObject_QEBAXXZ_destroy(dci_Translated_dci_tr_dumpObjectInfo_QObject_QEBAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_deleteLater_QObject_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_deleteLater_QObject_QEAAXXZ dci_tr_deleteLater_QObject_QEAAXXZ(QObject* self) noexcept {
    dci_Translated_dci_tr_deleteLater_QObject_QEAAXXZ out{};
    try {
        self->deleteLater();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_deleteLater_QObject_QEAAXXZ_destroy(dci_Translated_dci_tr_deleteLater_QObject_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setText_QAbstractButton_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setText_QAbstractButton_QEAAXAEBVQString_Z dci_tr_setText_QAbstractButton_QEAAXAEBVQString_Z(QAbstractButton* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_setText_QAbstractButton_QEAAXAEBVQString_Z out{};
    try {
        self->setText(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setText_QAbstractButton_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setText_QAbstractButton_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setIcon_QAbstractButton_QEAAXAEBVQIcon_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setIcon_QAbstractButton_QEAAXAEBVQIcon_Z dci_tr_setIcon_QAbstractButton_QEAAXAEBVQIcon_Z(QAbstractButton* self, const QIcon& p0) noexcept {
    dci_Translated_dci_tr_setIcon_QAbstractButton_QEAAXAEBVQIcon_Z out{};
    try {
        self->setIcon(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setIcon_QAbstractButton_QEAAXAEBVQIcon_Z_destroy(dci_Translated_dci_tr_setIcon_QAbstractButton_QEAAXAEBVQIcon_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setShortcut_QAbstractButton_QEAAXAEBVQKeySequence_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setShortcut_QAbstractButton_QEAAXAEBVQKeySequence_Z dci_tr_setShortcut_QAbstractButton_QEAAXAEBVQKeySequence_Z(QAbstractButton* self, const QKeySequence& p0) noexcept {
    dci_Translated_dci_tr_setShortcut_QAbstractButton_QEAAXAEBVQKeySequence_Z out{};
    try {
        self->setShortcut(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setShortcut_QAbstractButton_QEAAXAEBVQKeySequence_Z_destroy(dci_Translated_dci_tr_setShortcut_QAbstractButton_QEAAXAEBVQKeySequence_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setCheckable_QAbstractButton_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setCheckable_QAbstractButton_QEAAX_N_Z dci_tr_setCheckable_QAbstractButton_QEAAX_N_Z(QAbstractButton* self, bool p0) noexcept {
    dci_Translated_dci_tr_setCheckable_QAbstractButton_QEAAX_N_Z out{};
    try {
        self->setCheckable(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setCheckable_QAbstractButton_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setCheckable_QAbstractButton_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isCheckable_QAbstractButton_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isCheckable_QAbstractButton_QEBA_NXZ dci_tr_isCheckable_QAbstractButton_QEBA_NXZ(const QAbstractButton* self) noexcept {
    dci_Translated_dci_tr_isCheckable_QAbstractButton_QEBA_NXZ out{};
    try {
        out.ok = self->isCheckable();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isCheckable_QAbstractButton_QEBA_NXZ_destroy(dci_Translated_dci_tr_isCheckable_QAbstractButton_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isChecked_QAbstractButton_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isChecked_QAbstractButton_QEBA_NXZ dci_tr_isChecked_QAbstractButton_QEBA_NXZ(const QAbstractButton* self) noexcept {
    dci_Translated_dci_tr_isChecked_QAbstractButton_QEBA_NXZ out{};
    try {
        out.ok = self->isChecked();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isChecked_QAbstractButton_QEBA_NXZ_destroy(dci_Translated_dci_tr_isChecked_QAbstractButton_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setDown_QAbstractButton_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setDown_QAbstractButton_QEAAX_N_Z dci_tr_setDown_QAbstractButton_QEAAX_N_Z(QAbstractButton* self, bool p0) noexcept {
    dci_Translated_dci_tr_setDown_QAbstractButton_QEAAX_N_Z out{};
    try {
        self->setDown(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setDown_QAbstractButton_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setDown_QAbstractButton_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isDown_QAbstractButton_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isDown_QAbstractButton_QEBA_NXZ dci_tr_isDown_QAbstractButton_QEBA_NXZ(const QAbstractButton* self) noexcept {
    dci_Translated_dci_tr_isDown_QAbstractButton_QEBA_NXZ out{};
    try {
        out.ok = self->isDown();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isDown_QAbstractButton_QEBA_NXZ_destroy(dci_Translated_dci_tr_isDown_QAbstractButton_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setAutoRepeat_QAbstractButton_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setAutoRepeat_QAbstractButton_QEAAX_N_Z dci_tr_setAutoRepeat_QAbstractButton_QEAAX_N_Z(QAbstractButton* self, bool p0) noexcept {
    dci_Translated_dci_tr_setAutoRepeat_QAbstractButton_QEAAX_N_Z out{};
    try {
        self->setAutoRepeat(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setAutoRepeat_QAbstractButton_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setAutoRepeat_QAbstractButton_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_autoRepeat_QAbstractButton_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_autoRepeat_QAbstractButton_QEBA_NXZ dci_tr_autoRepeat_QAbstractButton_QEBA_NXZ(const QAbstractButton* self) noexcept {
    dci_Translated_dci_tr_autoRepeat_QAbstractButton_QEBA_NXZ out{};
    try {
        out.ok = self->autoRepeat();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_autoRepeat_QAbstractButton_QEBA_NXZ_destroy(dci_Translated_dci_tr_autoRepeat_QAbstractButton_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setAutoRepeatDelay_QAbstractButton_QEAAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setAutoRepeatDelay_QAbstractButton_QEAAXH_Z dci_tr_setAutoRepeatDelay_QAbstractButton_QEAAXH_Z(QAbstractButton* self, int p0) noexcept {
    dci_Translated_dci_tr_setAutoRepeatDelay_QAbstractButton_QEAAXH_Z out{};
    try {
        self->setAutoRepeatDelay(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setAutoRepeatDelay_QAbstractButton_QEAAXH_Z_destroy(dci_Translated_dci_tr_setAutoRepeatDelay_QAbstractButton_QEAAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_autoRepeatDelay_QAbstractButton_QEBAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_autoRepeatDelay_QAbstractButton_QEBAHXZ dci_tr_autoRepeatDelay_QAbstractButton_QEBAHXZ(const QAbstractButton* self) noexcept {
    dci_Translated_dci_tr_autoRepeatDelay_QAbstractButton_QEBAHXZ out{};
    try {
        out.ok = self->autoRepeatDelay();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_autoRepeatDelay_QAbstractButton_QEBAHXZ_destroy(dci_Translated_dci_tr_autoRepeatDelay_QAbstractButton_QEBAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setAutoRepeatInterval_QAbstractButton_QEAAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setAutoRepeatInterval_QAbstractButton_QEAAXH_Z dci_tr_setAutoRepeatInterval_QAbstractButton_QEAAXH_Z(QAbstractButton* self, int p0) noexcept {
    dci_Translated_dci_tr_setAutoRepeatInterval_QAbstractButton_QEAAXH_Z out{};
    try {
        self->setAutoRepeatInterval(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setAutoRepeatInterval_QAbstractButton_QEAAXH_Z_destroy(dci_Translated_dci_tr_setAutoRepeatInterval_QAbstractButton_QEAAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_autoRepeatInterval_QAbstractButton_QEBAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_autoRepeatInterval_QAbstractButton_QEBAHXZ dci_tr_autoRepeatInterval_QAbstractButton_QEBAHXZ(const QAbstractButton* self) noexcept {
    dci_Translated_dci_tr_autoRepeatInterval_QAbstractButton_QEBAHXZ out{};
    try {
        out.ok = self->autoRepeatInterval();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_autoRepeatInterval_QAbstractButton_QEBAHXZ_destroy(dci_Translated_dci_tr_autoRepeatInterval_QAbstractButton_QEBAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setAutoExclusive_QAbstractButton_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setAutoExclusive_QAbstractButton_QEAAX_N_Z dci_tr_setAutoExclusive_QAbstractButton_QEAAX_N_Z(QAbstractButton* self, bool p0) noexcept {
    dci_Translated_dci_tr_setAutoExclusive_QAbstractButton_QEAAX_N_Z out{};
    try {
        self->setAutoExclusive(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setAutoExclusive_QAbstractButton_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setAutoExclusive_QAbstractButton_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_autoExclusive_QAbstractButton_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_autoExclusive_QAbstractButton_QEBA_NXZ dci_tr_autoExclusive_QAbstractButton_QEBA_NXZ(const QAbstractButton* self) noexcept {
    dci_Translated_dci_tr_autoExclusive_QAbstractButton_QEBA_NXZ out{};
    try {
        out.ok = self->autoExclusive();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_autoExclusive_QAbstractButton_QEBA_NXZ_destroy(dci_Translated_dci_tr_autoExclusive_QAbstractButton_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setIconSize_QAbstractButton_QEAAXAEBVQSize_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setIconSize_QAbstractButton_QEAAXAEBVQSize_Z dci_tr_setIconSize_QAbstractButton_QEAAXAEBVQSize_Z(QAbstractButton* self, const QSize& p0) noexcept {
    dci_Translated_dci_tr_setIconSize_QAbstractButton_QEAAXAEBVQSize_Z out{};
    try {
        self->setIconSize(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setIconSize_QAbstractButton_QEAAXAEBVQSize_Z_destroy(dci_Translated_dci_tr_setIconSize_QAbstractButton_QEAAXAEBVQSize_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_animateClick_QAbstractButton_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_animateClick_QAbstractButton_QEAAXXZ dci_tr_animateClick_QAbstractButton_QEAAXXZ(QAbstractButton* self) noexcept {
    dci_Translated_dci_tr_animateClick_QAbstractButton_QEAAXXZ out{};
    try {
        self->animateClick();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_animateClick_QAbstractButton_QEAAXXZ_destroy(dci_Translated_dci_tr_animateClick_QAbstractButton_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_click_QAbstractButton_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_click_QAbstractButton_QEAAXXZ dci_tr_click_QAbstractButton_QEAAXXZ(QAbstractButton* self) noexcept {
    dci_Translated_dci_tr_click_QAbstractButton_QEAAXXZ out{};
    try {
        self->click();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_click_QAbstractButton_QEAAXXZ_destroy(dci_Translated_dci_tr_click_QAbstractButton_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_toggle_QAbstractButton_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_toggle_QAbstractButton_QEAAXXZ dci_tr_toggle_QAbstractButton_QEAAXXZ(QAbstractButton* self) noexcept {
    dci_Translated_dci_tr_toggle_QAbstractButton_QEAAXXZ out{};
    try {
        self->toggle();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_toggle_QAbstractButton_QEAAXXZ_destroy(dci_Translated_dci_tr_toggle_QAbstractButton_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setChecked_QAbstractButton_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setChecked_QAbstractButton_QEAAX_N_Z dci_tr_setChecked_QAbstractButton_QEAAX_N_Z(QAbstractButton* self, bool p0) noexcept {
    dci_Translated_dci_tr_setChecked_QAbstractButton_QEAAX_N_Z out{};
    try {
        self->setChecked(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setChecked_QAbstractButton_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setChecked_QAbstractButton_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_pressed_QAbstractButton_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_pressed_QAbstractButton_QEAAXXZ dci_tr_pressed_QAbstractButton_QEAAXXZ(QAbstractButton* self) noexcept {
    dci_Translated_dci_tr_pressed_QAbstractButton_QEAAXXZ out{};
    try {
        self->pressed();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_pressed_QAbstractButton_QEAAXXZ_destroy(dci_Translated_dci_tr_pressed_QAbstractButton_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_released_QAbstractButton_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_released_QAbstractButton_QEAAXXZ dci_tr_released_QAbstractButton_QEAAXXZ(QAbstractButton* self) noexcept {
    dci_Translated_dci_tr_released_QAbstractButton_QEAAXXZ out{};
    try {
        self->released();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_released_QAbstractButton_QEAAXXZ_destroy(dci_Translated_dci_tr_released_QAbstractButton_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_clicked_QAbstractButton_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_clicked_QAbstractButton_QEAAX_N_Z dci_tr_clicked_QAbstractButton_QEAAX_N_Z(QAbstractButton* self, bool p0) noexcept {
    dci_Translated_dci_tr_clicked_QAbstractButton_QEAAX_N_Z out{};
    try {
        self->clicked(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_clicked_QAbstractButton_QEAAX_N_Z_destroy(dci_Translated_dci_tr_clicked_QAbstractButton_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_toggled_QAbstractButton_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_toggled_QAbstractButton_QEAAX_N_Z dci_tr_toggled_QAbstractButton_QEAAX_N_Z(QAbstractButton* self, bool p0) noexcept {
    dci_Translated_dci_tr_toggled_QAbstractButton_QEAAX_N_Z out{};
    try {
        self->toggled(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_toggled_QAbstractButton_QEAAX_N_Z_destroy(dci_Translated_dci_tr_toggled_QAbstractButton_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_0QPushButton_QEAA_PEAVQWidget_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_0QPushButton_QEAA_PEAVQWidget_Z dci_tr_x_0QPushButton_QEAA_PEAVQWidget_Z(QPushButton* self, QWidget* p0) noexcept {
    dci_Translated_dci_tr_x_0QPushButton_QEAA_PEAVQWidget_Z out{};
    try {
        ::new (static_cast<void*>(self)) QPushButton(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_0QPushButton_QEAA_PEAVQWidget_Z_destroy(dci_Translated_dci_tr_x_0QPushButton_QEAA_PEAVQWidget_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_0QPushButton_QEAA_AEBVQString_PEAVQWidget_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_0QPushButton_QEAA_AEBVQString_PEAVQWidget_Z dci_tr_x_0QPushButton_QEAA_AEBVQString_PEAVQWidget_Z(QPushButton* self, const QString& p0, QWidget* p1) noexcept {
    dci_Translated_dci_tr_x_0QPushButton_QEAA_AEBVQString_PEAVQWidget_Z out{};
    try {
        ::new (static_cast<void*>(self)) QPushButton(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_0QPushButton_QEAA_AEBVQString_PEAVQWidget_Z_destroy(dci_Translated_dci_tr_x_0QPushButton_QEAA_AEBVQString_PEAVQWidget_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_autoDefault_QPushButton_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_autoDefault_QPushButton_QEBA_NXZ dci_tr_autoDefault_QPushButton_QEBA_NXZ(const QPushButton* self) noexcept {
    dci_Translated_dci_tr_autoDefault_QPushButton_QEBA_NXZ out{};
    try {
        out.ok = self->autoDefault();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_autoDefault_QPushButton_QEBA_NXZ_destroy(dci_Translated_dci_tr_autoDefault_QPushButton_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setAutoDefault_QPushButton_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setAutoDefault_QPushButton_QEAAX_N_Z dci_tr_setAutoDefault_QPushButton_QEAAX_N_Z(QPushButton* self, bool p0) noexcept {
    dci_Translated_dci_tr_setAutoDefault_QPushButton_QEAAX_N_Z out{};
    try {
        self->setAutoDefault(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setAutoDefault_QPushButton_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setAutoDefault_QPushButton_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isDefault_QPushButton_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isDefault_QPushButton_QEBA_NXZ dci_tr_isDefault_QPushButton_QEBA_NXZ(const QPushButton* self) noexcept {
    dci_Translated_dci_tr_isDefault_QPushButton_QEBA_NXZ out{};
    try {
        out.ok = self->isDefault();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isDefault_QPushButton_QEBA_NXZ_destroy(dci_Translated_dci_tr_isDefault_QPushButton_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setDefault_QPushButton_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setDefault_QPushButton_QEAAX_N_Z dci_tr_setDefault_QPushButton_QEAAX_N_Z(QPushButton* self, bool p0) noexcept {
    dci_Translated_dci_tr_setDefault_QPushButton_QEAAX_N_Z out{};
    try {
        self->setDefault(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setDefault_QPushButton_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setDefault_QPushButton_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setFlat_QPushButton_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setFlat_QPushButton_QEAAX_N_Z dci_tr_setFlat_QPushButton_QEAAX_N_Z(QPushButton* self, bool p0) noexcept {
    dci_Translated_dci_tr_setFlat_QPushButton_QEAAX_N_Z out{};
    try {
        self->setFlat(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setFlat_QPushButton_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setFlat_QPushButton_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isFlat_QPushButton_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isFlat_QPushButton_QEBA_NXZ dci_tr_isFlat_QPushButton_QEBA_NXZ(const QPushButton* self) noexcept {
    dci_Translated_dci_tr_isFlat_QPushButton_QEBA_NXZ out{};
    try {
        out.ok = self->isFlat();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isFlat_QPushButton_QEBA_NXZ_destroy(dci_Translated_dci_tr_isFlat_QPushButton_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_showMenu_QPushButton_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_showMenu_QPushButton_QEAAXXZ dci_tr_showMenu_QPushButton_QEAAXXZ(QPushButton* self) noexcept {
    dci_Translated_dci_tr_showMenu_QPushButton_QEAAXXZ out{};
    try {
        self->showMenu();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_showMenu_QPushButton_QEAAXXZ_destroy(dci_Translated_dci_tr_showMenu_QPushButton_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_0QLCDNumber_QEAA_PEAVQWidget_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_0QLCDNumber_QEAA_PEAVQWidget_Z dci_tr_x_0QLCDNumber_QEAA_PEAVQWidget_Z(QLCDNumber* self, QWidget* p0) noexcept {
    dci_Translated_dci_tr_x_0QLCDNumber_QEAA_PEAVQWidget_Z out{};
    try {
        ::new (static_cast<void*>(self)) QLCDNumber(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_0QLCDNumber_QEAA_PEAVQWidget_Z_destroy(dci_Translated_dci_tr_x_0QLCDNumber_QEAA_PEAVQWidget_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_smallDecimalPoint_QLCDNumber_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_smallDecimalPoint_QLCDNumber_QEBA_NXZ dci_tr_smallDecimalPoint_QLCDNumber_QEBA_NXZ(const QLCDNumber* self) noexcept {
    dci_Translated_dci_tr_smallDecimalPoint_QLCDNumber_QEBA_NXZ out{};
    try {
        out.ok = self->smallDecimalPoint();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_smallDecimalPoint_QLCDNumber_QEBA_NXZ_destroy(dci_Translated_dci_tr_smallDecimalPoint_QLCDNumber_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_digitCount_QLCDNumber_QEBAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_digitCount_QLCDNumber_QEBAHXZ dci_tr_digitCount_QLCDNumber_QEBAHXZ(const QLCDNumber* self) noexcept {
    dci_Translated_dci_tr_digitCount_QLCDNumber_QEBAHXZ out{};
    try {
        out.ok = self->digitCount();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_digitCount_QLCDNumber_QEBAHXZ_destroy(dci_Translated_dci_tr_digitCount_QLCDNumber_QEBAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setDigitCount_QLCDNumber_QEAAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setDigitCount_QLCDNumber_QEAAXH_Z dci_tr_setDigitCount_QLCDNumber_QEAAXH_Z(QLCDNumber* self, int p0) noexcept {
    dci_Translated_dci_tr_setDigitCount_QLCDNumber_QEAAXH_Z out{};
    try {
        self->setDigitCount(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setDigitCount_QLCDNumber_QEAAXH_Z_destroy(dci_Translated_dci_tr_setDigitCount_QLCDNumber_QEAAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_checkOverflow_QLCDNumber_QEBA_NN_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_checkOverflow_QLCDNumber_QEBA_NN_Z dci_tr_checkOverflow_QLCDNumber_QEBA_NN_Z(const QLCDNumber* self, double p0) noexcept {
    dci_Translated_dci_tr_checkOverflow_QLCDNumber_QEBA_NN_Z out{};
    try {
        out.ok = self->checkOverflow(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_checkOverflow_QLCDNumber_QEBA_NN_Z_destroy(dci_Translated_dci_tr_checkOverflow_QLCDNumber_QEBA_NN_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_checkOverflow_QLCDNumber_QEBA_NH_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_checkOverflow_QLCDNumber_QEBA_NH_Z dci_tr_checkOverflow_QLCDNumber_QEBA_NH_Z(const QLCDNumber* self, int p0) noexcept {
    dci_Translated_dci_tr_checkOverflow_QLCDNumber_QEBA_NH_Z out{};
    try {
        out.ok = self->checkOverflow(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_checkOverflow_QLCDNumber_QEBA_NH_Z_destroy(dci_Translated_dci_tr_checkOverflow_QLCDNumber_QEBA_NH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setMode_QLCDNumber_QEAAXW4Mode_1_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setMode_QLCDNumber_QEAAXW4Mode_1_Z dci_tr_setMode_QLCDNumber_QEAAXW4Mode_1_Z(QLCDNumber* self, Mode p0) noexcept {
    dci_Translated_dci_tr_setMode_QLCDNumber_QEAAXW4Mode_1_Z out{};
    try {
        self->setMode(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setMode_QLCDNumber_QEAAXW4Mode_1_Z_destroy(dci_Translated_dci_tr_setMode_QLCDNumber_QEAAXW4Mode_1_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setSegmentStyle_QLCDNumber_QEAAXW4SegmentStyle_1_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setSegmentStyle_QLCDNumber_QEAAXW4SegmentStyle_1_Z dci_tr_setSegmentStyle_QLCDNumber_QEAAXW4SegmentStyle_1_Z(QLCDNumber* self, SegmentStyle p0) noexcept {
    dci_Translated_dci_tr_setSegmentStyle_QLCDNumber_QEAAXW4SegmentStyle_1_Z out{};
    try {
        self->setSegmentStyle(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setSegmentStyle_QLCDNumber_QEAAXW4SegmentStyle_1_Z_destroy(dci_Translated_dci_tr_setSegmentStyle_QLCDNumber_QEAAXW4SegmentStyle_1_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_value_QLCDNumber_QEBANXZ {
    std::int8_t tag;
    double ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_value_QLCDNumber_QEBANXZ dci_tr_value_QLCDNumber_QEBANXZ(const QLCDNumber* self) noexcept {
    dci_Translated_dci_tr_value_QLCDNumber_QEBANXZ out{};
    try {
        out.ok = self->value();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_value_QLCDNumber_QEBANXZ_destroy(dci_Translated_dci_tr_value_QLCDNumber_QEBANXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_intValue_QLCDNumber_QEBAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_intValue_QLCDNumber_QEBAHXZ dci_tr_intValue_QLCDNumber_QEBAHXZ(const QLCDNumber* self) noexcept {
    dci_Translated_dci_tr_intValue_QLCDNumber_QEBAHXZ out{};
    try {
        out.ok = self->intValue();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_intValue_QLCDNumber_QEBAHXZ_destroy(dci_Translated_dci_tr_intValue_QLCDNumber_QEBAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_display_QLCDNumber_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_display_QLCDNumber_QEAAXAEBVQString_Z dci_tr_display_QLCDNumber_QEAAXAEBVQString_Z(QLCDNumber* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_display_QLCDNumber_QEAAXAEBVQString_Z out{};
    try {
        self->display(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_display_QLCDNumber_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_display_QLCDNumber_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_display_QLCDNumber_QEAAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_display_QLCDNumber_QEAAXH_Z dci_tr_display_QLCDNumber_QEAAXH_Z(QLCDNumber* self, int p0) noexcept {
    dci_Translated_dci_tr_display_QLCDNumber_QEAAXH_Z out{};
    try {
        self->display(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_display_QLCDNumber_QEAAXH_Z_destroy(dci_Translated_dci_tr_display_QLCDNumber_QEAAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_display_QLCDNumber_QEAAXN_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_display_QLCDNumber_QEAAXN_Z dci_tr_display_QLCDNumber_QEAAXN_Z(QLCDNumber* self, double p0) noexcept {
    dci_Translated_dci_tr_display_QLCDNumber_QEAAXN_Z out{};
    try {
        self->display(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_display_QLCDNumber_QEAAXN_Z_destroy(dci_Translated_dci_tr_display_QLCDNumber_QEAAXN_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setHexMode_QLCDNumber_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setHexMode_QLCDNumber_QEAAXXZ dci_tr_setHexMode_QLCDNumber_QEAAXXZ(QLCDNumber* self) noexcept {
    dci_Translated_dci_tr_setHexMode_QLCDNumber_QEAAXXZ out{};
    try {
        self->setHexMode();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setHexMode_QLCDNumber_QEAAXXZ_destroy(dci_Translated_dci_tr_setHexMode_QLCDNumber_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setDecMode_QLCDNumber_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setDecMode_QLCDNumber_QEAAXXZ dci_tr_setDecMode_QLCDNumber_QEAAXXZ(QLCDNumber* self) noexcept {
    dci_Translated_dci_tr_setDecMode_QLCDNumber_QEAAXXZ out{};
    try {
        self->setDecMode();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setDecMode_QLCDNumber_QEAAXXZ_destroy(dci_Translated_dci_tr_setDecMode_QLCDNumber_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setOctMode_QLCDNumber_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setOctMode_QLCDNumber_QEAAXXZ dci_tr_setOctMode_QLCDNumber_QEAAXXZ(QLCDNumber* self) noexcept {
    dci_Translated_dci_tr_setOctMode_QLCDNumber_QEAAXXZ out{};
    try {
        self->setOctMode();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setOctMode_QLCDNumber_QEAAXXZ_destroy(dci_Translated_dci_tr_setOctMode_QLCDNumber_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setBinMode_QLCDNumber_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setBinMode_QLCDNumber_QEAAXXZ dci_tr_setBinMode_QLCDNumber_QEAAXXZ(QLCDNumber* self) noexcept {
    dci_Translated_dci_tr_setBinMode_QLCDNumber_QEAAXXZ out{};
    try {
        self->setBinMode();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setBinMode_QLCDNumber_QEAAXXZ_destroy(dci_Translated_dci_tr_setBinMode_QLCDNumber_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setSmallDecimalPoint_QLCDNumber_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setSmallDecimalPoint_QLCDNumber_QEAAX_N_Z dci_tr_setSmallDecimalPoint_QLCDNumber_QEAAX_N_Z(QLCDNumber* self, bool p0) noexcept {
    dci_Translated_dci_tr_setSmallDecimalPoint_QLCDNumber_QEAAX_N_Z out{};
    try {
        self->setSmallDecimalPoint(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setSmallDecimalPoint_QLCDNumber_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setSmallDecimalPoint_QLCDNumber_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_overflow_QLCDNumber_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_overflow_QLCDNumber_QEAAXXZ dci_tr_overflow_QLCDNumber_QEAAXXZ(QLCDNumber* self) noexcept {
    dci_Translated_dci_tr_overflow_QLCDNumber_QEAAXXZ out{};
    try {
        self->overflow();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_overflow_QLCDNumber_QEAAXXZ_destroy(dci_Translated_dci_tr_overflow_QLCDNumber_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_0QWidget_QEAA_PEAV0_V_QFlags_W4WindowType_Qt_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_0QWidget_QEAA_PEAV0_V_QFlags_W4WindowType_Qt_Z dci_tr_x_0QWidget_QEAA_PEAV0_V_QFlags_W4WindowType_Qt_Z(QWidget* self, QWidget* p0, QFlags<Qt::WindowType> p1) noexcept {
    dci_Translated_dci_tr_x_0QWidget_QEAA_PEAV0_V_QFlags_W4WindowType_Qt_Z out{};
    try {
        ::new (static_cast<void*>(self)) QWidget(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_0QWidget_QEAA_PEAV0_V_QFlags_W4WindowType_Qt_Z_destroy(dci_Translated_dci_tr_x_0QWidget_QEAA_PEAV0_V_QFlags_W4WindowType_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_devType_QWidget_UEBAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_devType_QWidget_UEBAHXZ dci_tr_devType_QWidget_UEBAHXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_devType_QWidget_UEBAHXZ out{};
    try {
        out.ok = self->devType();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_devType_QWidget_UEBAHXZ_destroy(dci_Translated_dci_tr_devType_QWidget_UEBAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_createWinId_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_createWinId_QWidget_QEAAXXZ dci_tr_createWinId_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_createWinId_QWidget_QEAAXXZ out{};
    try {
        self->createWinId();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_createWinId_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_createWinId_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isTopLevel_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isTopLevel_QWidget_QEBA_NXZ dci_tr_isTopLevel_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_isTopLevel_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->isTopLevel();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isTopLevel_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_isTopLevel_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isWindow_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isWindow_QWidget_QEBA_NXZ dci_tr_isWindow_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_isWindow_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->isWindow();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isWindow_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_isWindow_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isModal_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isModal_QWidget_QEBA_NXZ dci_tr_isModal_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_isModal_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->isModal();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isModal_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_isModal_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setWindowModality_QWidget_QEAAXW4WindowModality_Qt_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setWindowModality_QWidget_QEAAXW4WindowModality_Qt_Z dci_tr_setWindowModality_QWidget_QEAAXW4WindowModality_Qt_Z(QWidget* self, Qt::WindowModality p0) noexcept {
    dci_Translated_dci_tr_setWindowModality_QWidget_QEAAXW4WindowModality_Qt_Z out{};
    try {
        self->setWindowModality(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setWindowModality_QWidget_QEAAXW4WindowModality_Qt_Z_destroy(dci_Translated_dci_tr_setWindowModality_QWidget_QEAAXW4WindowModality_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isEnabled_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isEnabled_QWidget_QEBA_NXZ dci_tr_isEnabled_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_isEnabled_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->isEnabled();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isEnabled_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_isEnabled_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setEnabled_QWidget_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setEnabled_QWidget_QEAAX_N_Z dci_tr_setEnabled_QWidget_QEAAX_N_Z(QWidget* self, bool p0) noexcept {
    dci_Translated_dci_tr_setEnabled_QWidget_QEAAX_N_Z out{};
    try {
        self->setEnabled(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setEnabled_QWidget_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setEnabled_QWidget_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setDisabled_QWidget_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setDisabled_QWidget_QEAAX_N_Z dci_tr_setDisabled_QWidget_QEAAX_N_Z(QWidget* self, bool p0) noexcept {
    dci_Translated_dci_tr_setDisabled_QWidget_QEAAX_N_Z out{};
    try {
        self->setDisabled(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setDisabled_QWidget_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setDisabled_QWidget_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setWindowModified_QWidget_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setWindowModified_QWidget_QEAAX_N_Z dci_tr_setWindowModified_QWidget_QEAAX_N_Z(QWidget* self, bool p0) noexcept {
    dci_Translated_dci_tr_setWindowModified_QWidget_QEAAX_N_Z out{};
    try {
        self->setWindowModified(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setWindowModified_QWidget_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setWindowModified_QWidget_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_QWidget_QEBAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_QWidget_QEBAHXZ dci_tr_x_QWidget_QEBAHXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_x_QWidget_QEBAHXZ out{};
    try {
        out.ok = self->x();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_QWidget_QEBAHXZ_destroy(dci_Translated_dci_tr_x_QWidget_QEBAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_y_QWidget_QEBAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_y_QWidget_QEBAHXZ dci_tr_y_QWidget_QEBAHXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_y_QWidget_QEBAHXZ out{};
    try {
        out.ok = self->y();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_y_QWidget_QEBAHXZ_destroy(dci_Translated_dci_tr_y_QWidget_QEBAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_width_QWidget_QEBAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_width_QWidget_QEBAHXZ dci_tr_width_QWidget_QEBAHXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_width_QWidget_QEBAHXZ out{};
    try {
        out.ok = self->width();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_width_QWidget_QEBAHXZ_destroy(dci_Translated_dci_tr_width_QWidget_QEBAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_height_QWidget_QEBAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_height_QWidget_QEBAHXZ dci_tr_height_QWidget_QEBAHXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_height_QWidget_QEBAHXZ out{};
    try {
        out.ok = self->height();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_height_QWidget_QEBAHXZ_destroy(dci_Translated_dci_tr_height_QWidget_QEBAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_minimumWidth_QWidget_QEBAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_minimumWidth_QWidget_QEBAHXZ dci_tr_minimumWidth_QWidget_QEBAHXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_minimumWidth_QWidget_QEBAHXZ out{};
    try {
        out.ok = self->minimumWidth();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_minimumWidth_QWidget_QEBAHXZ_destroy(dci_Translated_dci_tr_minimumWidth_QWidget_QEBAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_minimumHeight_QWidget_QEBAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_minimumHeight_QWidget_QEBAHXZ dci_tr_minimumHeight_QWidget_QEBAHXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_minimumHeight_QWidget_QEBAHXZ out{};
    try {
        out.ok = self->minimumHeight();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_minimumHeight_QWidget_QEBAHXZ_destroy(dci_Translated_dci_tr_minimumHeight_QWidget_QEBAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_maximumWidth_QWidget_QEBAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_maximumWidth_QWidget_QEBAHXZ dci_tr_maximumWidth_QWidget_QEBAHXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_maximumWidth_QWidget_QEBAHXZ out{};
    try {
        out.ok = self->maximumWidth();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_maximumWidth_QWidget_QEBAHXZ_destroy(dci_Translated_dci_tr_maximumWidth_QWidget_QEBAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_maximumHeight_QWidget_QEBAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_maximumHeight_QWidget_QEBAHXZ dci_tr_maximumHeight_QWidget_QEBAHXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_maximumHeight_QWidget_QEBAHXZ out{};
    try {
        out.ok = self->maximumHeight();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_maximumHeight_QWidget_QEBAHXZ_destroy(dci_Translated_dci_tr_maximumHeight_QWidget_QEBAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setMinimumSize_QWidget_QEAAXAEBVQSize_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setMinimumSize_QWidget_QEAAXAEBVQSize_Z dci_tr_setMinimumSize_QWidget_QEAAXAEBVQSize_Z(QWidget* self, const QSize& p0) noexcept {
    dci_Translated_dci_tr_setMinimumSize_QWidget_QEAAXAEBVQSize_Z out{};
    try {
        self->setMinimumSize(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setMinimumSize_QWidget_QEAAXAEBVQSize_Z_destroy(dci_Translated_dci_tr_setMinimumSize_QWidget_QEAAXAEBVQSize_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setMinimumSize_QWidget_QEAAXHH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setMinimumSize_QWidget_QEAAXHH_Z dci_tr_setMinimumSize_QWidget_QEAAXHH_Z(QWidget* self, int p0, int p1) noexcept {
    dci_Translated_dci_tr_setMinimumSize_QWidget_QEAAXHH_Z out{};
    try {
        self->setMinimumSize(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setMinimumSize_QWidget_QEAAXHH_Z_destroy(dci_Translated_dci_tr_setMinimumSize_QWidget_QEAAXHH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setMaximumSize_QWidget_QEAAXAEBVQSize_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setMaximumSize_QWidget_QEAAXAEBVQSize_Z dci_tr_setMaximumSize_QWidget_QEAAXAEBVQSize_Z(QWidget* self, const QSize& p0) noexcept {
    dci_Translated_dci_tr_setMaximumSize_QWidget_QEAAXAEBVQSize_Z out{};
    try {
        self->setMaximumSize(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setMaximumSize_QWidget_QEAAXAEBVQSize_Z_destroy(dci_Translated_dci_tr_setMaximumSize_QWidget_QEAAXAEBVQSize_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setMaximumSize_QWidget_QEAAXHH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setMaximumSize_QWidget_QEAAXHH_Z dci_tr_setMaximumSize_QWidget_QEAAXHH_Z(QWidget* self, int p0, int p1) noexcept {
    dci_Translated_dci_tr_setMaximumSize_QWidget_QEAAXHH_Z out{};
    try {
        self->setMaximumSize(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setMaximumSize_QWidget_QEAAXHH_Z_destroy(dci_Translated_dci_tr_setMaximumSize_QWidget_QEAAXHH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setMinimumWidth_QWidget_QEAAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setMinimumWidth_QWidget_QEAAXH_Z dci_tr_setMinimumWidth_QWidget_QEAAXH_Z(QWidget* self, int p0) noexcept {
    dci_Translated_dci_tr_setMinimumWidth_QWidget_QEAAXH_Z out{};
    try {
        self->setMinimumWidth(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setMinimumWidth_QWidget_QEAAXH_Z_destroy(dci_Translated_dci_tr_setMinimumWidth_QWidget_QEAAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setMinimumHeight_QWidget_QEAAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setMinimumHeight_QWidget_QEAAXH_Z dci_tr_setMinimumHeight_QWidget_QEAAXH_Z(QWidget* self, int p0) noexcept {
    dci_Translated_dci_tr_setMinimumHeight_QWidget_QEAAXH_Z out{};
    try {
        self->setMinimumHeight(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setMinimumHeight_QWidget_QEAAXH_Z_destroy(dci_Translated_dci_tr_setMinimumHeight_QWidget_QEAAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setMaximumWidth_QWidget_QEAAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setMaximumWidth_QWidget_QEAAXH_Z dci_tr_setMaximumWidth_QWidget_QEAAXH_Z(QWidget* self, int p0) noexcept {
    dci_Translated_dci_tr_setMaximumWidth_QWidget_QEAAXH_Z out{};
    try {
        self->setMaximumWidth(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setMaximumWidth_QWidget_QEAAXH_Z_destroy(dci_Translated_dci_tr_setMaximumWidth_QWidget_QEAAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setMaximumHeight_QWidget_QEAAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setMaximumHeight_QWidget_QEAAXH_Z dci_tr_setMaximumHeight_QWidget_QEAAXH_Z(QWidget* self, int p0) noexcept {
    dci_Translated_dci_tr_setMaximumHeight_QWidget_QEAAXH_Z out{};
    try {
        self->setMaximumHeight(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setMaximumHeight_QWidget_QEAAXH_Z_destroy(dci_Translated_dci_tr_setMaximumHeight_QWidget_QEAAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setSizeIncrement_QWidget_QEAAXAEBVQSize_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setSizeIncrement_QWidget_QEAAXAEBVQSize_Z dci_tr_setSizeIncrement_QWidget_QEAAXAEBVQSize_Z(QWidget* self, const QSize& p0) noexcept {
    dci_Translated_dci_tr_setSizeIncrement_QWidget_QEAAXAEBVQSize_Z out{};
    try {
        self->setSizeIncrement(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setSizeIncrement_QWidget_QEAAXAEBVQSize_Z_destroy(dci_Translated_dci_tr_setSizeIncrement_QWidget_QEAAXAEBVQSize_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setSizeIncrement_QWidget_QEAAXHH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setSizeIncrement_QWidget_QEAAXHH_Z dci_tr_setSizeIncrement_QWidget_QEAAXHH_Z(QWidget* self, int p0, int p1) noexcept {
    dci_Translated_dci_tr_setSizeIncrement_QWidget_QEAAXHH_Z out{};
    try {
        self->setSizeIncrement(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setSizeIncrement_QWidget_QEAAXHH_Z_destroy(dci_Translated_dci_tr_setSizeIncrement_QWidget_QEAAXHH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setBaseSize_QWidget_QEAAXAEBVQSize_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setBaseSize_QWidget_QEAAXAEBVQSize_Z dci_tr_setBaseSize_QWidget_QEAAXAEBVQSize_Z(QWidget* self, const QSize& p0) noexcept {
    dci_Translated_dci_tr_setBaseSize_QWidget_QEAAXAEBVQSize_Z out{};
    try {
        self->setBaseSize(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setBaseSize_QWidget_QEAAXAEBVQSize_Z_destroy(dci_Translated_dci_tr_setBaseSize_QWidget_QEAAXAEBVQSize_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setBaseSize_QWidget_QEAAXHH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setBaseSize_QWidget_QEAAXHH_Z dci_tr_setBaseSize_QWidget_QEAAXHH_Z(QWidget* self, int p0, int p1) noexcept {
    dci_Translated_dci_tr_setBaseSize_QWidget_QEAAXHH_Z out{};
    try {
        self->setBaseSize(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setBaseSize_QWidget_QEAAXHH_Z_destroy(dci_Translated_dci_tr_setBaseSize_QWidget_QEAAXHH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setFixedSize_QWidget_QEAAXAEBVQSize_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setFixedSize_QWidget_QEAAXAEBVQSize_Z dci_tr_setFixedSize_QWidget_QEAAXAEBVQSize_Z(QWidget* self, const QSize& p0) noexcept {
    dci_Translated_dci_tr_setFixedSize_QWidget_QEAAXAEBVQSize_Z out{};
    try {
        self->setFixedSize(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setFixedSize_QWidget_QEAAXAEBVQSize_Z_destroy(dci_Translated_dci_tr_setFixedSize_QWidget_QEAAXAEBVQSize_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setFixedSize_QWidget_QEAAXHH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setFixedSize_QWidget_QEAAXHH_Z dci_tr_setFixedSize_QWidget_QEAAXHH_Z(QWidget* self, int p0, int p1) noexcept {
    dci_Translated_dci_tr_setFixedSize_QWidget_QEAAXHH_Z out{};
    try {
        self->setFixedSize(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setFixedSize_QWidget_QEAAXHH_Z_destroy(dci_Translated_dci_tr_setFixedSize_QWidget_QEAAXHH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setFixedWidth_QWidget_QEAAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setFixedWidth_QWidget_QEAAXH_Z dci_tr_setFixedWidth_QWidget_QEAAXH_Z(QWidget* self, int p0) noexcept {
    dci_Translated_dci_tr_setFixedWidth_QWidget_QEAAXH_Z out{};
    try {
        self->setFixedWidth(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setFixedWidth_QWidget_QEAAXH_Z_destroy(dci_Translated_dci_tr_setFixedWidth_QWidget_QEAAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setFixedHeight_QWidget_QEAAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setFixedHeight_QWidget_QEAAXH_Z dci_tr_setFixedHeight_QWidget_QEAAXH_Z(QWidget* self, int p0) noexcept {
    dci_Translated_dci_tr_setFixedHeight_QWidget_QEAAXH_Z out{};
    try {
        self->setFixedHeight(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setFixedHeight_QWidget_QEAAXH_Z_destroy(dci_Translated_dci_tr_setFixedHeight_QWidget_QEAAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setPalette_QWidget_QEAAXAEBVQPalette_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setPalette_QWidget_QEAAXAEBVQPalette_Z dci_tr_setPalette_QWidget_QEAAXAEBVQPalette_Z(QWidget* self, const QPalette& p0) noexcept {
    dci_Translated_dci_tr_setPalette_QWidget_QEAAXAEBVQPalette_Z out{};
    try {
        self->setPalette(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setPalette_QWidget_QEAAXAEBVQPalette_Z_destroy(dci_Translated_dci_tr_setPalette_QWidget_QEAAXAEBVQPalette_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setBackgroundRole_QWidget_QEAAXW4ColorRole_QPalette_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setBackgroundRole_QWidget_QEAAXW4ColorRole_QPalette_Z dci_tr_setBackgroundRole_QWidget_QEAAXW4ColorRole_QPalette_Z(QWidget* self, QPalette::ColorRole p0) noexcept {
    dci_Translated_dci_tr_setBackgroundRole_QWidget_QEAAXW4ColorRole_QPalette_Z out{};
    try {
        self->setBackgroundRole(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setBackgroundRole_QWidget_QEAAXW4ColorRole_QPalette_Z_destroy(dci_Translated_dci_tr_setBackgroundRole_QWidget_QEAAXW4ColorRole_QPalette_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setForegroundRole_QWidget_QEAAXW4ColorRole_QPalette_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setForegroundRole_QWidget_QEAAXW4ColorRole_QPalette_Z dci_tr_setForegroundRole_QWidget_QEAAXW4ColorRole_QPalette_Z(QWidget* self, QPalette::ColorRole p0) noexcept {
    dci_Translated_dci_tr_setForegroundRole_QWidget_QEAAXW4ColorRole_QPalette_Z out{};
    try {
        self->setForegroundRole(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setForegroundRole_QWidget_QEAAXW4ColorRole_QPalette_Z_destroy(dci_Translated_dci_tr_setForegroundRole_QWidget_QEAAXW4ColorRole_QPalette_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setFont_QWidget_QEAAXAEBVQFont_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setFont_QWidget_QEAAXAEBVQFont_Z dci_tr_setFont_QWidget_QEAAXAEBVQFont_Z(QWidget* self, const QFont& p0) noexcept {
    dci_Translated_dci_tr_setFont_QWidget_QEAAXAEBVQFont_Z out{};
    try {
        self->setFont(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setFont_QWidget_QEAAXAEBVQFont_Z_destroy(dci_Translated_dci_tr_setFont_QWidget_QEAAXAEBVQFont_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setCursor_QWidget_QEAAXAEBVQCursor_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setCursor_QWidget_QEAAXAEBVQCursor_Z dci_tr_setCursor_QWidget_QEAAXAEBVQCursor_Z(QWidget* self, const QCursor& p0) noexcept {
    dci_Translated_dci_tr_setCursor_QWidget_QEAAXAEBVQCursor_Z out{};
    try {
        self->setCursor(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setCursor_QWidget_QEAAXAEBVQCursor_Z_destroy(dci_Translated_dci_tr_setCursor_QWidget_QEAAXAEBVQCursor_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_unsetCursor_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_unsetCursor_QWidget_QEAAXXZ dci_tr_unsetCursor_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_unsetCursor_QWidget_QEAAXXZ out{};
    try {
        self->unsetCursor();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_unsetCursor_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_unsetCursor_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setMouseTracking_QWidget_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setMouseTracking_QWidget_QEAAX_N_Z dci_tr_setMouseTracking_QWidget_QEAAX_N_Z(QWidget* self, bool p0) noexcept {
    dci_Translated_dci_tr_setMouseTracking_QWidget_QEAAX_N_Z out{};
    try {
        self->setMouseTracking(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setMouseTracking_QWidget_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setMouseTracking_QWidget_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_hasMouseTracking_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_hasMouseTracking_QWidget_QEBA_NXZ dci_tr_hasMouseTracking_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_hasMouseTracking_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->hasMouseTracking();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_hasMouseTracking_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_hasMouseTracking_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_underMouse_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_underMouse_QWidget_QEBA_NXZ dci_tr_underMouse_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_underMouse_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->underMouse();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_underMouse_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_underMouse_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setTabletTracking_QWidget_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setTabletTracking_QWidget_QEAAX_N_Z dci_tr_setTabletTracking_QWidget_QEAAX_N_Z(QWidget* self, bool p0) noexcept {
    dci_Translated_dci_tr_setTabletTracking_QWidget_QEAAX_N_Z out{};
    try {
        self->setTabletTracking(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setTabletTracking_QWidget_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setTabletTracking_QWidget_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_hasTabletTracking_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_hasTabletTracking_QWidget_QEBA_NXZ dci_tr_hasTabletTracking_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_hasTabletTracking_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->hasTabletTracking();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_hasTabletTracking_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_hasTabletTracking_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setMask_QWidget_QEAAXAEBVQBitmap_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setMask_QWidget_QEAAXAEBVQBitmap_Z dci_tr_setMask_QWidget_QEAAXAEBVQBitmap_Z(QWidget* self, const QBitmap& p0) noexcept {
    dci_Translated_dci_tr_setMask_QWidget_QEAAXAEBVQBitmap_Z out{};
    try {
        self->setMask(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setMask_QWidget_QEAAXAEBVQBitmap_Z_destroy(dci_Translated_dci_tr_setMask_QWidget_QEAAXAEBVQBitmap_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setMask_QWidget_QEAAXAEBVQRegion_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setMask_QWidget_QEAAXAEBVQRegion_Z dci_tr_setMask_QWidget_QEAAXAEBVQRegion_Z(QWidget* self, const QRegion& p0) noexcept {
    dci_Translated_dci_tr_setMask_QWidget_QEAAXAEBVQRegion_Z out{};
    try {
        self->setMask(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setMask_QWidget_QEAAXAEBVQRegion_Z_destroy(dci_Translated_dci_tr_setMask_QWidget_QEAAXAEBVQRegion_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_clearMask_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_clearMask_QWidget_QEAAXXZ dci_tr_clearMask_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_clearMask_QWidget_QEAAXXZ out{};
    try {
        self->clearMask();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_clearMask_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_clearMask_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_ungrabGesture_QWidget_QEAAXW4GestureType_Qt_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_ungrabGesture_QWidget_QEAAXW4GestureType_Qt_Z dci_tr_ungrabGesture_QWidget_QEAAXW4GestureType_Qt_Z(QWidget* self, Qt::GestureType p0) noexcept {
    dci_Translated_dci_tr_ungrabGesture_QWidget_QEAAXW4GestureType_Qt_Z out{};
    try {
        self->ungrabGesture(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_ungrabGesture_QWidget_QEAAXW4GestureType_Qt_Z_destroy(dci_Translated_dci_tr_ungrabGesture_QWidget_QEAAXW4GestureType_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setWindowTitle_QWidget_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setWindowTitle_QWidget_QEAAXAEBVQString_Z dci_tr_setWindowTitle_QWidget_QEAAXAEBVQString_Z(QWidget* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_setWindowTitle_QWidget_QEAAXAEBVQString_Z out{};
    try {
        self->setWindowTitle(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setWindowTitle_QWidget_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setWindowTitle_QWidget_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setStyleSheet_QWidget_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setStyleSheet_QWidget_QEAAXAEBVQString_Z dci_tr_setStyleSheet_QWidget_QEAAXAEBVQString_Z(QWidget* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_setStyleSheet_QWidget_QEAAXAEBVQString_Z out{};
    try {
        self->setStyleSheet(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setStyleSheet_QWidget_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setStyleSheet_QWidget_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setWindowIcon_QWidget_QEAAXAEBVQIcon_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setWindowIcon_QWidget_QEAAXAEBVQIcon_Z dci_tr_setWindowIcon_QWidget_QEAAXAEBVQIcon_Z(QWidget* self, const QIcon& p0) noexcept {
    dci_Translated_dci_tr_setWindowIcon_QWidget_QEAAXAEBVQIcon_Z out{};
    try {
        self->setWindowIcon(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setWindowIcon_QWidget_QEAAXAEBVQIcon_Z_destroy(dci_Translated_dci_tr_setWindowIcon_QWidget_QEAAXAEBVQIcon_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setWindowIconText_QWidget_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setWindowIconText_QWidget_QEAAXAEBVQString_Z dci_tr_setWindowIconText_QWidget_QEAAXAEBVQString_Z(QWidget* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_setWindowIconText_QWidget_QEAAXAEBVQString_Z out{};
    try {
        self->setWindowIconText(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setWindowIconText_QWidget_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setWindowIconText_QWidget_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setWindowRole_QWidget_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setWindowRole_QWidget_QEAAXAEBVQString_Z dci_tr_setWindowRole_QWidget_QEAAXAEBVQString_Z(QWidget* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_setWindowRole_QWidget_QEAAXAEBVQString_Z out{};
    try {
        self->setWindowRole(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setWindowRole_QWidget_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setWindowRole_QWidget_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setWindowFilePath_QWidget_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setWindowFilePath_QWidget_QEAAXAEBVQString_Z dci_tr_setWindowFilePath_QWidget_QEAAXAEBVQString_Z(QWidget* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_setWindowFilePath_QWidget_QEAAXAEBVQString_Z out{};
    try {
        self->setWindowFilePath(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setWindowFilePath_QWidget_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setWindowFilePath_QWidget_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setWindowOpacity_QWidget_QEAAXN_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setWindowOpacity_QWidget_QEAAXN_Z dci_tr_setWindowOpacity_QWidget_QEAAXN_Z(QWidget* self, qreal p0) noexcept {
    dci_Translated_dci_tr_setWindowOpacity_QWidget_QEAAXN_Z out{};
    try {
        self->setWindowOpacity(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setWindowOpacity_QWidget_QEAAXN_Z_destroy(dci_Translated_dci_tr_setWindowOpacity_QWidget_QEAAXN_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isWindowModified_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isWindowModified_QWidget_QEBA_NXZ dci_tr_isWindowModified_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_isWindowModified_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->isWindowModified();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isWindowModified_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_isWindowModified_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setToolTip_QWidget_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setToolTip_QWidget_QEAAXAEBVQString_Z dci_tr_setToolTip_QWidget_QEAAXAEBVQString_Z(QWidget* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_setToolTip_QWidget_QEAAXAEBVQString_Z out{};
    try {
        self->setToolTip(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setToolTip_QWidget_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setToolTip_QWidget_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setToolTipDuration_QWidget_QEAAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setToolTipDuration_QWidget_QEAAXH_Z dci_tr_setToolTipDuration_QWidget_QEAAXH_Z(QWidget* self, int p0) noexcept {
    dci_Translated_dci_tr_setToolTipDuration_QWidget_QEAAXH_Z out{};
    try {
        self->setToolTipDuration(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setToolTipDuration_QWidget_QEAAXH_Z_destroy(dci_Translated_dci_tr_setToolTipDuration_QWidget_QEAAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_toolTipDuration_QWidget_QEBAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_toolTipDuration_QWidget_QEBAHXZ dci_tr_toolTipDuration_QWidget_QEBAHXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_toolTipDuration_QWidget_QEBAHXZ out{};
    try {
        out.ok = self->toolTipDuration();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_toolTipDuration_QWidget_QEBAHXZ_destroy(dci_Translated_dci_tr_toolTipDuration_QWidget_QEBAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setStatusTip_QWidget_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setStatusTip_QWidget_QEAAXAEBVQString_Z dci_tr_setStatusTip_QWidget_QEAAXAEBVQString_Z(QWidget* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_setStatusTip_QWidget_QEAAXAEBVQString_Z out{};
    try {
        self->setStatusTip(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setStatusTip_QWidget_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setStatusTip_QWidget_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setWhatsThis_QWidget_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setWhatsThis_QWidget_QEAAXAEBVQString_Z dci_tr_setWhatsThis_QWidget_QEAAXAEBVQString_Z(QWidget* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_setWhatsThis_QWidget_QEAAXAEBVQString_Z out{};
    try {
        self->setWhatsThis(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setWhatsThis_QWidget_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setWhatsThis_QWidget_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setAccessibleName_QWidget_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setAccessibleName_QWidget_QEAAXAEBVQString_Z dci_tr_setAccessibleName_QWidget_QEAAXAEBVQString_Z(QWidget* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_setAccessibleName_QWidget_QEAAXAEBVQString_Z out{};
    try {
        self->setAccessibleName(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setAccessibleName_QWidget_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setAccessibleName_QWidget_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setAccessibleDescription_QWidget_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setAccessibleDescription_QWidget_QEAAXAEBVQString_Z dci_tr_setAccessibleDescription_QWidget_QEAAXAEBVQString_Z(QWidget* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_setAccessibleDescription_QWidget_QEAAXAEBVQString_Z out{};
    try {
        self->setAccessibleDescription(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setAccessibleDescription_QWidget_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setAccessibleDescription_QWidget_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setLayoutDirection_QWidget_QEAAXW4LayoutDirection_Qt_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setLayoutDirection_QWidget_QEAAXW4LayoutDirection_Qt_Z dci_tr_setLayoutDirection_QWidget_QEAAXW4LayoutDirection_Qt_Z(QWidget* self, Qt::LayoutDirection p0) noexcept {
    dci_Translated_dci_tr_setLayoutDirection_QWidget_QEAAXW4LayoutDirection_Qt_Z out{};
    try {
        self->setLayoutDirection(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setLayoutDirection_QWidget_QEAAXW4LayoutDirection_Qt_Z_destroy(dci_Translated_dci_tr_setLayoutDirection_QWidget_QEAAXW4LayoutDirection_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_unsetLayoutDirection_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_unsetLayoutDirection_QWidget_QEAAXXZ dci_tr_unsetLayoutDirection_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_unsetLayoutDirection_QWidget_QEAAXXZ out{};
    try {
        self->unsetLayoutDirection();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_unsetLayoutDirection_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_unsetLayoutDirection_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setLocale_QWidget_QEAAXAEBVQLocale_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setLocale_QWidget_QEAAXAEBVQLocale_Z dci_tr_setLocale_QWidget_QEAAXAEBVQLocale_Z(QWidget* self, const QLocale& p0) noexcept {
    dci_Translated_dci_tr_setLocale_QWidget_QEAAXAEBVQLocale_Z out{};
    try {
        self->setLocale(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setLocale_QWidget_QEAAXAEBVQLocale_Z_destroy(dci_Translated_dci_tr_setLocale_QWidget_QEAAXAEBVQLocale_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_unsetLocale_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_unsetLocale_QWidget_QEAAXXZ dci_tr_unsetLocale_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_unsetLocale_QWidget_QEAAXXZ out{};
    try {
        self->unsetLocale();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_unsetLocale_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_unsetLocale_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isRightToLeft_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isRightToLeft_QWidget_QEBA_NXZ dci_tr_isRightToLeft_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_isRightToLeft_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->isRightToLeft();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isRightToLeft_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_isRightToLeft_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isLeftToRight_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isLeftToRight_QWidget_QEBA_NXZ dci_tr_isLeftToRight_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_isLeftToRight_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->isLeftToRight();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isLeftToRight_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_isLeftToRight_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setFocus_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setFocus_QWidget_QEAAXXZ dci_tr_setFocus_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_setFocus_QWidget_QEAAXXZ out{};
    try {
        self->setFocus();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setFocus_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_setFocus_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isActiveWindow_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isActiveWindow_QWidget_QEBA_NXZ dci_tr_isActiveWindow_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_isActiveWindow_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->isActiveWindow();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isActiveWindow_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_isActiveWindow_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_activateWindow_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_activateWindow_QWidget_QEAAXXZ dci_tr_activateWindow_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_activateWindow_QWidget_QEAAXXZ out{};
    try {
        self->activateWindow();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_activateWindow_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_activateWindow_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_clearFocus_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_clearFocus_QWidget_QEAAXXZ dci_tr_clearFocus_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_clearFocus_QWidget_QEAAXXZ out{};
    try {
        self->clearFocus();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_clearFocus_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_clearFocus_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setFocus_QWidget_QEAAXW4FocusReason_Qt_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setFocus_QWidget_QEAAXW4FocusReason_Qt_Z dci_tr_setFocus_QWidget_QEAAXW4FocusReason_Qt_Z(QWidget* self, Qt::FocusReason p0) noexcept {
    dci_Translated_dci_tr_setFocus_QWidget_QEAAXW4FocusReason_Qt_Z out{};
    try {
        self->setFocus(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setFocus_QWidget_QEAAXW4FocusReason_Qt_Z_destroy(dci_Translated_dci_tr_setFocus_QWidget_QEAAXW4FocusReason_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setFocusPolicy_QWidget_QEAAXW4FocusPolicy_Qt_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setFocusPolicy_QWidget_QEAAXW4FocusPolicy_Qt_Z dci_tr_setFocusPolicy_QWidget_QEAAXW4FocusPolicy_Qt_Z(QWidget* self, Qt::FocusPolicy p0) noexcept {
    dci_Translated_dci_tr_setFocusPolicy_QWidget_QEAAXW4FocusPolicy_Qt_Z out{};
    try {
        self->setFocusPolicy(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setFocusPolicy_QWidget_QEAAXW4FocusPolicy_Qt_Z_destroy(dci_Translated_dci_tr_setFocusPolicy_QWidget_QEAAXW4FocusPolicy_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_hasFocus_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_hasFocus_QWidget_QEBA_NXZ dci_tr_hasFocus_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_hasFocus_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->hasFocus();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_hasFocus_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_hasFocus_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setContextMenuPolicy_QWidget_QEAAXW4ContextMenuPolicy_Qt_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setContextMenuPolicy_QWidget_QEAAXW4ContextMenuPolicy_Qt_Z dci_tr_setContextMenuPolicy_QWidget_QEAAXW4ContextMenuPolicy_Qt_Z(QWidget* self, Qt::ContextMenuPolicy p0) noexcept {
    dci_Translated_dci_tr_setContextMenuPolicy_QWidget_QEAAXW4ContextMenuPolicy_Qt_Z out{};
    try {
        self->setContextMenuPolicy(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setContextMenuPolicy_QWidget_QEAAXW4ContextMenuPolicy_Qt_Z_destroy(dci_Translated_dci_tr_setContextMenuPolicy_QWidget_QEAAXW4ContextMenuPolicy_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_grabMouse_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_grabMouse_QWidget_QEAAXXZ dci_tr_grabMouse_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_grabMouse_QWidget_QEAAXXZ out{};
    try {
        self->grabMouse();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_grabMouse_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_grabMouse_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_grabMouse_QWidget_QEAAXAEBVQCursor_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_grabMouse_QWidget_QEAAXAEBVQCursor_Z dci_tr_grabMouse_QWidget_QEAAXAEBVQCursor_Z(QWidget* self, const QCursor& p0) noexcept {
    dci_Translated_dci_tr_grabMouse_QWidget_QEAAXAEBVQCursor_Z out{};
    try {
        self->grabMouse(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_grabMouse_QWidget_QEAAXAEBVQCursor_Z_destroy(dci_Translated_dci_tr_grabMouse_QWidget_QEAAXAEBVQCursor_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_releaseMouse_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_releaseMouse_QWidget_QEAAXXZ dci_tr_releaseMouse_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_releaseMouse_QWidget_QEAAXXZ out{};
    try {
        self->releaseMouse();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_releaseMouse_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_releaseMouse_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_grabKeyboard_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_grabKeyboard_QWidget_QEAAXXZ dci_tr_grabKeyboard_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_grabKeyboard_QWidget_QEAAXXZ out{};
    try {
        self->grabKeyboard();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_grabKeyboard_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_grabKeyboard_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_releaseKeyboard_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_releaseKeyboard_QWidget_QEAAXXZ dci_tr_releaseKeyboard_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_releaseKeyboard_QWidget_QEAAXXZ out{};
    try {
        self->releaseKeyboard();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_releaseKeyboard_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_releaseKeyboard_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_grabShortcut_QWidget_QEAAHAEBVQKeySequence_W4ShortcutContext_Qt_Z {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_grabShortcut_QWidget_QEAAHAEBVQKeySequence_W4ShortcutContext_Qt_Z dci_tr_grabShortcut_QWidget_QEAAHAEBVQKeySequence_W4ShortcutContext_Qt_Z(QWidget* self, const QKeySequence& p0, Qt::ShortcutContext p1) noexcept {
    dci_Translated_dci_tr_grabShortcut_QWidget_QEAAHAEBVQKeySequence_W4ShortcutContext_Qt_Z out{};
    try {
        out.ok = self->grabShortcut(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_grabShortcut_QWidget_QEAAHAEBVQKeySequence_W4ShortcutContext_Qt_Z_destroy(dci_Translated_dci_tr_grabShortcut_QWidget_QEAAHAEBVQKeySequence_W4ShortcutContext_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_releaseShortcut_QWidget_QEAAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_releaseShortcut_QWidget_QEAAXH_Z dci_tr_releaseShortcut_QWidget_QEAAXH_Z(QWidget* self, int p0) noexcept {
    dci_Translated_dci_tr_releaseShortcut_QWidget_QEAAXH_Z out{};
    try {
        self->releaseShortcut(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_releaseShortcut_QWidget_QEAAXH_Z_destroy(dci_Translated_dci_tr_releaseShortcut_QWidget_QEAAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setShortcutEnabled_QWidget_QEAAXH_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setShortcutEnabled_QWidget_QEAAXH_N_Z dci_tr_setShortcutEnabled_QWidget_QEAAXH_N_Z(QWidget* self, int p0, bool p1) noexcept {
    dci_Translated_dci_tr_setShortcutEnabled_QWidget_QEAAXH_N_Z out{};
    try {
        self->setShortcutEnabled(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setShortcutEnabled_QWidget_QEAAXH_N_Z_destroy(dci_Translated_dci_tr_setShortcutEnabled_QWidget_QEAAXH_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setShortcutAutoRepeat_QWidget_QEAAXH_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setShortcutAutoRepeat_QWidget_QEAAXH_N_Z dci_tr_setShortcutAutoRepeat_QWidget_QEAAXH_N_Z(QWidget* self, int p0, bool p1) noexcept {
    dci_Translated_dci_tr_setShortcutAutoRepeat_QWidget_QEAAXH_N_Z out{};
    try {
        self->setShortcutAutoRepeat(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setShortcutAutoRepeat_QWidget_QEAAXH_N_Z_destroy(dci_Translated_dci_tr_setShortcutAutoRepeat_QWidget_QEAAXH_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_updatesEnabled_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_updatesEnabled_QWidget_QEBA_NXZ dci_tr_updatesEnabled_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_updatesEnabled_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->updatesEnabled();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_updatesEnabled_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_updatesEnabled_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setUpdatesEnabled_QWidget_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setUpdatesEnabled_QWidget_QEAAX_N_Z dci_tr_setUpdatesEnabled_QWidget_QEAAX_N_Z(QWidget* self, bool p0) noexcept {
    dci_Translated_dci_tr_setUpdatesEnabled_QWidget_QEAAX_N_Z out{};
    try {
        self->setUpdatesEnabled(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setUpdatesEnabled_QWidget_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setUpdatesEnabled_QWidget_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_update_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_update_QWidget_QEAAXXZ dci_tr_update_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_update_QWidget_QEAAXXZ out{};
    try {
        self->update();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_update_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_update_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_repaint_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_repaint_QWidget_QEAAXXZ dci_tr_repaint_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_repaint_QWidget_QEAAXXZ out{};
    try {
        self->repaint();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_repaint_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_repaint_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_update_QWidget_QEAAXHHHH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_update_QWidget_QEAAXHHHH_Z dci_tr_update_QWidget_QEAAXHHHH_Z(QWidget* self, int p0, int p1, int p2, int p3) noexcept {
    dci_Translated_dci_tr_update_QWidget_QEAAXHHHH_Z out{};
    try {
        self->update(p0, p1, p2, p3);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_update_QWidget_QEAAXHHHH_Z_destroy(dci_Translated_dci_tr_update_QWidget_QEAAXHHHH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_update_QWidget_QEAAXAEBVQRect_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_update_QWidget_QEAAXAEBVQRect_Z dci_tr_update_QWidget_QEAAXAEBVQRect_Z(QWidget* self, const QRect& p0) noexcept {
    dci_Translated_dci_tr_update_QWidget_QEAAXAEBVQRect_Z out{};
    try {
        self->update(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_update_QWidget_QEAAXAEBVQRect_Z_destroy(dci_Translated_dci_tr_update_QWidget_QEAAXAEBVQRect_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_update_QWidget_QEAAXAEBVQRegion_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_update_QWidget_QEAAXAEBVQRegion_Z dci_tr_update_QWidget_QEAAXAEBVQRegion_Z(QWidget* self, const QRegion& p0) noexcept {
    dci_Translated_dci_tr_update_QWidget_QEAAXAEBVQRegion_Z out{};
    try {
        self->update(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_update_QWidget_QEAAXAEBVQRegion_Z_destroy(dci_Translated_dci_tr_update_QWidget_QEAAXAEBVQRegion_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_repaint_QWidget_QEAAXHHHH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_repaint_QWidget_QEAAXHHHH_Z dci_tr_repaint_QWidget_QEAAXHHHH_Z(QWidget* self, int p0, int p1, int p2, int p3) noexcept {
    dci_Translated_dci_tr_repaint_QWidget_QEAAXHHHH_Z out{};
    try {
        self->repaint(p0, p1, p2, p3);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_repaint_QWidget_QEAAXHHHH_Z_destroy(dci_Translated_dci_tr_repaint_QWidget_QEAAXHHHH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_repaint_QWidget_QEAAXAEBVQRect_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_repaint_QWidget_QEAAXAEBVQRect_Z dci_tr_repaint_QWidget_QEAAXAEBVQRect_Z(QWidget* self, const QRect& p0) noexcept {
    dci_Translated_dci_tr_repaint_QWidget_QEAAXAEBVQRect_Z out{};
    try {
        self->repaint(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_repaint_QWidget_QEAAXAEBVQRect_Z_destroy(dci_Translated_dci_tr_repaint_QWidget_QEAAXAEBVQRect_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_repaint_QWidget_QEAAXAEBVQRegion_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_repaint_QWidget_QEAAXAEBVQRegion_Z dci_tr_repaint_QWidget_QEAAXAEBVQRegion_Z(QWidget* self, const QRegion& p0) noexcept {
    dci_Translated_dci_tr_repaint_QWidget_QEAAXAEBVQRegion_Z out{};
    try {
        self->repaint(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_repaint_QWidget_QEAAXAEBVQRegion_Z_destroy(dci_Translated_dci_tr_repaint_QWidget_QEAAXAEBVQRegion_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setVisible_QWidget_UEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setVisible_QWidget_UEAAX_N_Z dci_tr_setVisible_QWidget_UEAAX_N_Z(QWidget* self, bool p0) noexcept {
    dci_Translated_dci_tr_setVisible_QWidget_UEAAX_N_Z out{};
    try {
        self->setVisible(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setVisible_QWidget_UEAAX_N_Z_destroy(dci_Translated_dci_tr_setVisible_QWidget_UEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setHidden_QWidget_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setHidden_QWidget_QEAAX_N_Z dci_tr_setHidden_QWidget_QEAAX_N_Z(QWidget* self, bool p0) noexcept {
    dci_Translated_dci_tr_setHidden_QWidget_QEAAX_N_Z out{};
    try {
        self->setHidden(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setHidden_QWidget_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setHidden_QWidget_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_show_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_show_QWidget_QEAAXXZ dci_tr_show_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_show_QWidget_QEAAXXZ out{};
    try {
        self->show();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_show_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_show_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_hide_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_hide_QWidget_QEAAXXZ dci_tr_hide_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_hide_QWidget_QEAAXXZ out{};
    try {
        self->hide();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_hide_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_hide_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_showMinimized_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_showMinimized_QWidget_QEAAXXZ dci_tr_showMinimized_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_showMinimized_QWidget_QEAAXXZ out{};
    try {
        self->showMinimized();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_showMinimized_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_showMinimized_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_showMaximized_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_showMaximized_QWidget_QEAAXXZ dci_tr_showMaximized_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_showMaximized_QWidget_QEAAXXZ out{};
    try {
        self->showMaximized();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_showMaximized_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_showMaximized_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_showFullScreen_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_showFullScreen_QWidget_QEAAXXZ dci_tr_showFullScreen_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_showFullScreen_QWidget_QEAAXXZ out{};
    try {
        self->showFullScreen();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_showFullScreen_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_showFullScreen_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_showNormal_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_showNormal_QWidget_QEAAXXZ dci_tr_showNormal_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_showNormal_QWidget_QEAAXXZ out{};
    try {
        self->showNormal();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_showNormal_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_showNormal_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_close_QWidget_QEAA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_close_QWidget_QEAA_NXZ dci_tr_close_QWidget_QEAA_NXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_close_QWidget_QEAA_NXZ out{};
    try {
        out.ok = self->close();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_close_QWidget_QEAA_NXZ_destroy(dci_Translated_dci_tr_close_QWidget_QEAA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_raise_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_raise_QWidget_QEAAXXZ dci_tr_raise_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_raise_QWidget_QEAAXXZ out{};
    try {
        self->raise();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_raise_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_raise_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_lower_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_lower_QWidget_QEAAXXZ dci_tr_lower_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_lower_QWidget_QEAAXXZ out{};
    try {
        self->lower();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_lower_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_lower_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_move_QWidget_QEAAXHH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_move_QWidget_QEAAXHH_Z dci_tr_move_QWidget_QEAAXHH_Z(QWidget* self, int p0, int p1) noexcept {
    dci_Translated_dci_tr_move_QWidget_QEAAXHH_Z out{};
    try {
        self->move(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_move_QWidget_QEAAXHH_Z_destroy(dci_Translated_dci_tr_move_QWidget_QEAAXHH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_move_QWidget_QEAAXAEBVQPoint_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_move_QWidget_QEAAXAEBVQPoint_Z dci_tr_move_QWidget_QEAAXAEBVQPoint_Z(QWidget* self, const QPoint& p0) noexcept {
    dci_Translated_dci_tr_move_QWidget_QEAAXAEBVQPoint_Z out{};
    try {
        self->move(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_move_QWidget_QEAAXAEBVQPoint_Z_destroy(dci_Translated_dci_tr_move_QWidget_QEAAXAEBVQPoint_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_resize_QWidget_QEAAXHH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_resize_QWidget_QEAAXHH_Z dci_tr_resize_QWidget_QEAAXHH_Z(QWidget* self, int p0, int p1) noexcept {
    dci_Translated_dci_tr_resize_QWidget_QEAAXHH_Z out{};
    try {
        self->resize(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_resize_QWidget_QEAAXHH_Z_destroy(dci_Translated_dci_tr_resize_QWidget_QEAAXHH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_resize_QWidget_QEAAXAEBVQSize_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_resize_QWidget_QEAAXAEBVQSize_Z dci_tr_resize_QWidget_QEAAXAEBVQSize_Z(QWidget* self, const QSize& p0) noexcept {
    dci_Translated_dci_tr_resize_QWidget_QEAAXAEBVQSize_Z out{};
    try {
        self->resize(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_resize_QWidget_QEAAXAEBVQSize_Z_destroy(dci_Translated_dci_tr_resize_QWidget_QEAAXAEBVQSize_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setGeometry_QWidget_QEAAXHHHH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setGeometry_QWidget_QEAAXHHHH_Z dci_tr_setGeometry_QWidget_QEAAXHHHH_Z(QWidget* self, int p0, int p1, int p2, int p3) noexcept {
    dci_Translated_dci_tr_setGeometry_QWidget_QEAAXHHHH_Z out{};
    try {
        self->setGeometry(p0, p1, p2, p3);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setGeometry_QWidget_QEAAXHHHH_Z_destroy(dci_Translated_dci_tr_setGeometry_QWidget_QEAAXHHHH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setGeometry_QWidget_QEAAXAEBVQRect_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setGeometry_QWidget_QEAAXAEBVQRect_Z dci_tr_setGeometry_QWidget_QEAAXAEBVQRect_Z(QWidget* self, const QRect& p0) noexcept {
    dci_Translated_dci_tr_setGeometry_QWidget_QEAAXAEBVQRect_Z out{};
    try {
        self->setGeometry(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setGeometry_QWidget_QEAAXAEBVQRect_Z_destroy(dci_Translated_dci_tr_setGeometry_QWidget_QEAAXAEBVQRect_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_restoreGeometry_QWidget_QEAA_NAEBVQByteArray_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_restoreGeometry_QWidget_QEAA_NAEBVQByteArray_Z dci_tr_restoreGeometry_QWidget_QEAA_NAEBVQByteArray_Z(QWidget* self, const QByteArray& p0) noexcept {
    dci_Translated_dci_tr_restoreGeometry_QWidget_QEAA_NAEBVQByteArray_Z out{};
    try {
        out.ok = self->restoreGeometry(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_restoreGeometry_QWidget_QEAA_NAEBVQByteArray_Z_destroy(dci_Translated_dci_tr_restoreGeometry_QWidget_QEAA_NAEBVQByteArray_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_adjustSize_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_adjustSize_QWidget_QEAAXXZ dci_tr_adjustSize_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_adjustSize_QWidget_QEAAXXZ out{};
    try {
        self->adjustSize();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_adjustSize_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_adjustSize_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isVisible_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isVisible_QWidget_QEBA_NXZ dci_tr_isVisible_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_isVisible_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->isVisible();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isVisible_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_isVisible_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isHidden_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isHidden_QWidget_QEBA_NXZ dci_tr_isHidden_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_isHidden_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->isHidden();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isHidden_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_isHidden_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isMinimized_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isMinimized_QWidget_QEBA_NXZ dci_tr_isMinimized_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_isMinimized_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->isMinimized();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isMinimized_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_isMinimized_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isMaximized_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isMaximized_QWidget_QEBA_NXZ dci_tr_isMaximized_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_isMaximized_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->isMaximized();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isMaximized_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_isMaximized_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isFullScreen_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isFullScreen_QWidget_QEBA_NXZ dci_tr_isFullScreen_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_isFullScreen_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->isFullScreen();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isFullScreen_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_isFullScreen_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setSizePolicy_QWidget_QEAAXW4Policy_QSizePolicy_0_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setSizePolicy_QWidget_QEAAXW4Policy_QSizePolicy_0_Z dci_tr_setSizePolicy_QWidget_QEAAXW4Policy_QSizePolicy_0_Z(QWidget* self, QSizePolicy::Policy p0, QSizePolicy::Policy p1) noexcept {
    dci_Translated_dci_tr_setSizePolicy_QWidget_QEAAXW4Policy_QSizePolicy_0_Z out{};
    try {
        self->setSizePolicy(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setSizePolicy_QWidget_QEAAXW4Policy_QSizePolicy_0_Z_destroy(dci_Translated_dci_tr_setSizePolicy_QWidget_QEAAXW4Policy_QSizePolicy_0_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_heightForWidth_QWidget_UEBAHH_Z {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_heightForWidth_QWidget_UEBAHH_Z dci_tr_heightForWidth_QWidget_UEBAHH_Z(const QWidget* self, int p0) noexcept {
    dci_Translated_dci_tr_heightForWidth_QWidget_UEBAHH_Z out{};
    try {
        out.ok = self->heightForWidth(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_heightForWidth_QWidget_UEBAHH_Z_destroy(dci_Translated_dci_tr_heightForWidth_QWidget_UEBAHH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_hasHeightForWidth_QWidget_UEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_hasHeightForWidth_QWidget_UEBA_NXZ dci_tr_hasHeightForWidth_QWidget_UEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_hasHeightForWidth_QWidget_UEBA_NXZ out{};
    try {
        out.ok = self->hasHeightForWidth();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_hasHeightForWidth_QWidget_UEBA_NXZ_destroy(dci_Translated_dci_tr_hasHeightForWidth_QWidget_UEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setContentsMargins_QWidget_QEAAXHHHH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setContentsMargins_QWidget_QEAAXHHHH_Z dci_tr_setContentsMargins_QWidget_QEAAXHHHH_Z(QWidget* self, int p0, int p1, int p2, int p3) noexcept {
    dci_Translated_dci_tr_setContentsMargins_QWidget_QEAAXHHHH_Z out{};
    try {
        self->setContentsMargins(p0, p1, p2, p3);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setContentsMargins_QWidget_QEAAXHHHH_Z_destroy(dci_Translated_dci_tr_setContentsMargins_QWidget_QEAAXHHHH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setContentsMargins_QWidget_QEAAXAEBVQMargins_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setContentsMargins_QWidget_QEAAXAEBVQMargins_Z dci_tr_setContentsMargins_QWidget_QEAAXAEBVQMargins_Z(QWidget* self, const QMargins& p0) noexcept {
    dci_Translated_dci_tr_setContentsMargins_QWidget_QEAAXAEBVQMargins_Z out{};
    try {
        self->setContentsMargins(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setContentsMargins_QWidget_QEAAXAEBVQMargins_Z_destroy(dci_Translated_dci_tr_setContentsMargins_QWidget_QEAAXAEBVQMargins_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_updateGeometry_QWidget_QEAAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_updateGeometry_QWidget_QEAAXXZ dci_tr_updateGeometry_QWidget_QEAAXXZ(QWidget* self) noexcept {
    dci_Translated_dci_tr_updateGeometry_QWidget_QEAAXXZ out{};
    try {
        self->updateGeometry();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_updateGeometry_QWidget_QEAAXXZ_destroy(dci_Translated_dci_tr_updateGeometry_QWidget_QEAAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setParent_QWidget_QEAAXPEAV1_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setParent_QWidget_QEAAXPEAV1_Z dci_tr_setParent_QWidget_QEAAXPEAV1_Z(QWidget* self, QWidget* p0) noexcept {
    dci_Translated_dci_tr_setParent_QWidget_QEAAXPEAV1_Z out{};
    try {
        self->setParent(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setParent_QWidget_QEAAXPEAV1_Z_destroy(dci_Translated_dci_tr_setParent_QWidget_QEAAXPEAV1_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_scroll_QWidget_QEAAXHH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_scroll_QWidget_QEAAXHH_Z dci_tr_scroll_QWidget_QEAAXHH_Z(QWidget* self, int p0, int p1) noexcept {
    dci_Translated_dci_tr_scroll_QWidget_QEAAXHH_Z out{};
    try {
        self->scroll(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_scroll_QWidget_QEAAXHH_Z_destroy(dci_Translated_dci_tr_scroll_QWidget_QEAAXHH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_scroll_QWidget_QEAAXHHAEBVQRect_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_scroll_QWidget_QEAAXHHAEBVQRect_Z dci_tr_scroll_QWidget_QEAAXHHAEBVQRect_Z(QWidget* self, int p0, int p1, const QRect& p2) noexcept {
    dci_Translated_dci_tr_scroll_QWidget_QEAAXHHAEBVQRect_Z out{};
    try {
        self->scroll(p0, p1, p2);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_scroll_QWidget_QEAAXHHAEBVQRect_Z_destroy(dci_Translated_dci_tr_scroll_QWidget_QEAAXHHAEBVQRect_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_acceptDrops_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_acceptDrops_QWidget_QEBA_NXZ dci_tr_acceptDrops_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_acceptDrops_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->acceptDrops();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_acceptDrops_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_acceptDrops_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setAcceptDrops_QWidget_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setAcceptDrops_QWidget_QEAAX_N_Z dci_tr_setAcceptDrops_QWidget_QEAAX_N_Z(QWidget* self, bool p0) noexcept {
    dci_Translated_dci_tr_setAcceptDrops_QWidget_QEAAX_N_Z out{};
    try {
        self->setAcceptDrops(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setAcceptDrops_QWidget_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setAcceptDrops_QWidget_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_addActions_QWidget_QEAAXAEBV_QList_PEAVQAction_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_addActions_QWidget_QEAAXAEBV_QList_PEAVQAction_Z dci_tr_addActions_QWidget_QEAAXAEBV_QList_PEAVQAction_Z(QWidget* self, const QList<QAction*>& p0) noexcept {
    dci_Translated_dci_tr_addActions_QWidget_QEAAXAEBV_QList_PEAVQAction_Z out{};
    try {
        self->addActions(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_addActions_QWidget_QEAAXAEBV_QList_PEAVQAction_Z_destroy(dci_Translated_dci_tr_addActions_QWidget_QEAAXAEBV_QList_PEAVQAction_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setWindowFlags_QWidget_QEAAXV_QFlags_W4WindowType_Qt_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setWindowFlags_QWidget_QEAAXV_QFlags_W4WindowType_Qt_Z dci_tr_setWindowFlags_QWidget_QEAAXV_QFlags_W4WindowType_Qt_Z(QWidget* self, QFlags<Qt::WindowType> p0) noexcept {
    dci_Translated_dci_tr_setWindowFlags_QWidget_QEAAXV_QFlags_W4WindowType_Qt_Z out{};
    try {
        self->setWindowFlags(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setWindowFlags_QWidget_QEAAXV_QFlags_W4WindowType_Qt_Z_destroy(dci_Translated_dci_tr_setWindowFlags_QWidget_QEAAXV_QFlags_W4WindowType_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setWindowFlag_QWidget_QEAAXW4WindowType_Qt_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setWindowFlag_QWidget_QEAAXW4WindowType_Qt_N_Z dci_tr_setWindowFlag_QWidget_QEAAXW4WindowType_Qt_N_Z(QWidget* self, Qt::WindowType p0, bool p1) noexcept {
    dci_Translated_dci_tr_setWindowFlag_QWidget_QEAAXW4WindowType_Qt_N_Z out{};
    try {
        self->setWindowFlag(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setWindowFlag_QWidget_QEAAXW4WindowType_Qt_N_Z_destroy(dci_Translated_dci_tr_setWindowFlag_QWidget_QEAAXW4WindowType_Qt_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_overrideWindowFlags_QWidget_QEAAXV_QFlags_W4WindowType_Qt_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_overrideWindowFlags_QWidget_QEAAXV_QFlags_W4WindowType_Qt_Z dci_tr_overrideWindowFlags_QWidget_QEAAXV_QFlags_W4WindowType_Qt_Z(QWidget* self, QFlags<Qt::WindowType> p0) noexcept {
    dci_Translated_dci_tr_overrideWindowFlags_QWidget_QEAAXV_QFlags_W4WindowType_Qt_Z out{};
    try {
        self->overrideWindowFlags(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_overrideWindowFlags_QWidget_QEAAXV_QFlags_W4WindowType_Qt_Z_destroy(dci_Translated_dci_tr_overrideWindowFlags_QWidget_QEAAXV_QFlags_W4WindowType_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setAttribute_QWidget_QEAAXW4WidgetAttribute_Qt_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setAttribute_QWidget_QEAAXW4WidgetAttribute_Qt_N_Z dci_tr_setAttribute_QWidget_QEAAXW4WidgetAttribute_Qt_N_Z(QWidget* self, Qt::WidgetAttribute p0, bool p1) noexcept {
    dci_Translated_dci_tr_setAttribute_QWidget_QEAAXW4WidgetAttribute_Qt_N_Z out{};
    try {
        self->setAttribute(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setAttribute_QWidget_QEAAXW4WidgetAttribute_Qt_N_Z_destroy(dci_Translated_dci_tr_setAttribute_QWidget_QEAAXW4WidgetAttribute_Qt_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_testAttribute_QWidget_QEBA_NW4WidgetAttribute_Qt_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_testAttribute_QWidget_QEBA_NW4WidgetAttribute_Qt_Z dci_tr_testAttribute_QWidget_QEBA_NW4WidgetAttribute_Qt_Z(const QWidget* self, Qt::WidgetAttribute p0) noexcept {
    dci_Translated_dci_tr_testAttribute_QWidget_QEBA_NW4WidgetAttribute_Qt_Z out{};
    try {
        out.ok = self->testAttribute(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_testAttribute_QWidget_QEBA_NW4WidgetAttribute_Qt_Z_destroy(dci_Translated_dci_tr_testAttribute_QWidget_QEBA_NW4WidgetAttribute_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_ensurePolished_QWidget_QEBAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_ensurePolished_QWidget_QEBAXXZ dci_tr_ensurePolished_QWidget_QEBAXXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_ensurePolished_QWidget_QEBAXXZ out{};
    try {
        self->ensurePolished();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_ensurePolished_QWidget_QEBAXXZ_destroy(dci_Translated_dci_tr_ensurePolished_QWidget_QEBAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_autoFillBackground_QWidget_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_autoFillBackground_QWidget_QEBA_NXZ dci_tr_autoFillBackground_QWidget_QEBA_NXZ(const QWidget* self) noexcept {
    dci_Translated_dci_tr_autoFillBackground_QWidget_QEBA_NXZ out{};
    try {
        out.ok = self->autoFillBackground();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_autoFillBackground_QWidget_QEBA_NXZ_destroy(dci_Translated_dci_tr_autoFillBackground_QWidget_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setAutoFillBackground_QWidget_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setAutoFillBackground_QWidget_QEAAX_N_Z dci_tr_setAutoFillBackground_QWidget_QEAAX_N_Z(QWidget* self, bool p0) noexcept {
    dci_Translated_dci_tr_setAutoFillBackground_QWidget_QEAAX_N_Z out{};
    try {
        self->setAutoFillBackground(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setAutoFillBackground_QWidget_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setAutoFillBackground_QWidget_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_windowTitleChanged_QWidget_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_windowTitleChanged_QWidget_QEAAXAEBVQString_Z dci_tr_windowTitleChanged_QWidget_QEAAXAEBVQString_Z(QWidget* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_windowTitleChanged_QWidget_QEAAXAEBVQString_Z out{};
    try {
        self->windowTitleChanged(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_windowTitleChanged_QWidget_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_windowTitleChanged_QWidget_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_windowIconChanged_QWidget_QEAAXAEBVQIcon_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_windowIconChanged_QWidget_QEAAXAEBVQIcon_Z dci_tr_windowIconChanged_QWidget_QEAAXAEBVQIcon_Z(QWidget* self, const QIcon& p0) noexcept {
    dci_Translated_dci_tr_windowIconChanged_QWidget_QEAAXAEBVQIcon_Z out{};
    try {
        self->windowIconChanged(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_windowIconChanged_QWidget_QEAAXAEBVQIcon_Z_destroy(dci_Translated_dci_tr_windowIconChanged_QWidget_QEAAXAEBVQIcon_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_windowIconTextChanged_QWidget_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_windowIconTextChanged_QWidget_QEAAXAEBVQString_Z dci_tr_windowIconTextChanged_QWidget_QEAAXAEBVQString_Z(QWidget* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_windowIconTextChanged_QWidget_QEAAXAEBVQString_Z out{};
    try {
        self->windowIconTextChanged(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_windowIconTextChanged_QWidget_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_windowIconTextChanged_QWidget_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_customContextMenuRequested_QWidget_QEAAXAEBVQPoint_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_customContextMenuRequested_QWidget_QEAAXAEBVQPoint_Z dci_tr_customContextMenuRequested_QWidget_QEAAXAEBVQPoint_Z(QWidget* self, const QPoint& p0) noexcept {
    dci_Translated_dci_tr_customContextMenuRequested_QWidget_QEAAXAEBVQPoint_Z out{};
    try {
        self->customContextMenuRequested(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_customContextMenuRequested_QWidget_QEAAXAEBVQPoint_Z_destroy(dci_Translated_dci_tr_customContextMenuRequested_QWidget_QEAAXAEBVQPoint_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_x_0QApplication_QEAA_AEAHPEAPEADH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_0QApplication_QEAA_AEAHPEAPEADH_Z dci_tr_x_0QApplication_QEAA_AEAHPEAPEADH_Z(QApplication* self, int & p0, char ** p1, int p2) noexcept {
    dci_Translated_dci_tr_x_0QApplication_QEAA_AEAHPEAPEADH_Z out{};
    try {
        ::new (static_cast<void*>(self)) QApplication(p0, p1, p2);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_0QApplication_QEAA_AEAHPEAPEADH_Z_destroy(dci_Translated_dci_tr_x_0QApplication_QEAA_AEAHPEAPEADH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_beep_QApplication_SAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_beep_QApplication_SAXXZ dci_tr_beep_QApplication_SAXXZ(void) noexcept {
    dci_Translated_dci_tr_beep_QApplication_SAXXZ out{};
    try {
        QApplication::beep();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_beep_QApplication_SAXXZ_destroy(dci_Translated_dci_tr_beep_QApplication_SAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setCursorFlashTime_QApplication_SAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setCursorFlashTime_QApplication_SAXH_Z dci_tr_setCursorFlashTime_QApplication_SAXH_Z(int p0) noexcept {
    dci_Translated_dci_tr_setCursorFlashTime_QApplication_SAXH_Z out{};
    try {
        QApplication::setCursorFlashTime(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setCursorFlashTime_QApplication_SAXH_Z_destroy(dci_Translated_dci_tr_setCursorFlashTime_QApplication_SAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_cursorFlashTime_QApplication_SAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_cursorFlashTime_QApplication_SAHXZ dci_tr_cursorFlashTime_QApplication_SAHXZ(void) noexcept {
    dci_Translated_dci_tr_cursorFlashTime_QApplication_SAHXZ out{};
    try {
        out.ok = QApplication::cursorFlashTime();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_cursorFlashTime_QApplication_SAHXZ_destroy(dci_Translated_dci_tr_cursorFlashTime_QApplication_SAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setDoubleClickInterval_QApplication_SAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setDoubleClickInterval_QApplication_SAXH_Z dci_tr_setDoubleClickInterval_QApplication_SAXH_Z(int p0) noexcept {
    dci_Translated_dci_tr_setDoubleClickInterval_QApplication_SAXH_Z out{};
    try {
        QApplication::setDoubleClickInterval(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setDoubleClickInterval_QApplication_SAXH_Z_destroy(dci_Translated_dci_tr_setDoubleClickInterval_QApplication_SAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_doubleClickInterval_QApplication_SAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_doubleClickInterval_QApplication_SAHXZ dci_tr_doubleClickInterval_QApplication_SAHXZ(void) noexcept {
    dci_Translated_dci_tr_doubleClickInterval_QApplication_SAHXZ out{};
    try {
        out.ok = QApplication::doubleClickInterval();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_doubleClickInterval_QApplication_SAHXZ_destroy(dci_Translated_dci_tr_doubleClickInterval_QApplication_SAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setKeyboardInputInterval_QApplication_SAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setKeyboardInputInterval_QApplication_SAXH_Z dci_tr_setKeyboardInputInterval_QApplication_SAXH_Z(int p0) noexcept {
    dci_Translated_dci_tr_setKeyboardInputInterval_QApplication_SAXH_Z out{};
    try {
        QApplication::setKeyboardInputInterval(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setKeyboardInputInterval_QApplication_SAXH_Z_destroy(dci_Translated_dci_tr_setKeyboardInputInterval_QApplication_SAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_keyboardInputInterval_QApplication_SAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_keyboardInputInterval_QApplication_SAHXZ dci_tr_keyboardInputInterval_QApplication_SAHXZ(void) noexcept {
    dci_Translated_dci_tr_keyboardInputInterval_QApplication_SAHXZ out{};
    try {
        out.ok = QApplication::keyboardInputInterval();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_keyboardInputInterval_QApplication_SAHXZ_destroy(dci_Translated_dci_tr_keyboardInputInterval_QApplication_SAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setWheelScrollLines_QApplication_SAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setWheelScrollLines_QApplication_SAXH_Z dci_tr_setWheelScrollLines_QApplication_SAXH_Z(int p0) noexcept {
    dci_Translated_dci_tr_setWheelScrollLines_QApplication_SAXH_Z out{};
    try {
        QApplication::setWheelScrollLines(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setWheelScrollLines_QApplication_SAXH_Z_destroy(dci_Translated_dci_tr_setWheelScrollLines_QApplication_SAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_wheelScrollLines_QApplication_SAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_wheelScrollLines_QApplication_SAHXZ dci_tr_wheelScrollLines_QApplication_SAHXZ(void) noexcept {
    dci_Translated_dci_tr_wheelScrollLines_QApplication_SAHXZ out{};
    try {
        out.ok = QApplication::wheelScrollLines();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_wheelScrollLines_QApplication_SAHXZ_destroy(dci_Translated_dci_tr_wheelScrollLines_QApplication_SAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setStartDragTime_QApplication_SAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setStartDragTime_QApplication_SAXH_Z dci_tr_setStartDragTime_QApplication_SAXH_Z(int p0) noexcept {
    dci_Translated_dci_tr_setStartDragTime_QApplication_SAXH_Z out{};
    try {
        QApplication::setStartDragTime(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setStartDragTime_QApplication_SAXH_Z_destroy(dci_Translated_dci_tr_setStartDragTime_QApplication_SAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_startDragTime_QApplication_SAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_startDragTime_QApplication_SAHXZ dci_tr_startDragTime_QApplication_SAHXZ(void) noexcept {
    dci_Translated_dci_tr_startDragTime_QApplication_SAHXZ out{};
    try {
        out.ok = QApplication::startDragTime();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_startDragTime_QApplication_SAHXZ_destroy(dci_Translated_dci_tr_startDragTime_QApplication_SAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setStartDragDistance_QApplication_SAXH_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setStartDragDistance_QApplication_SAXH_Z dci_tr_setStartDragDistance_QApplication_SAXH_Z(int p0) noexcept {
    dci_Translated_dci_tr_setStartDragDistance_QApplication_SAXH_Z out{};
    try {
        QApplication::setStartDragDistance(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setStartDragDistance_QApplication_SAXH_Z_destroy(dci_Translated_dci_tr_setStartDragDistance_QApplication_SAXH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_startDragDistance_QApplication_SAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_startDragDistance_QApplication_SAHXZ dci_tr_startDragDistance_QApplication_SAHXZ(void) noexcept {
    dci_Translated_dci_tr_startDragDistance_QApplication_SAHXZ out{};
    try {
        out.ok = QApplication::startDragDistance();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_startDragDistance_QApplication_SAHXZ_destroy(dci_Translated_dci_tr_startDragDistance_QApplication_SAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_isEffectEnabled_QApplication_SA_NW4UIEffect_Qt_Z {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_isEffectEnabled_QApplication_SA_NW4UIEffect_Qt_Z dci_tr_isEffectEnabled_QApplication_SA_NW4UIEffect_Qt_Z(Qt::UIEffect p0) noexcept {
    dci_Translated_dci_tr_isEffectEnabled_QApplication_SA_NW4UIEffect_Qt_Z out{};
    try {
        out.ok = QApplication::isEffectEnabled(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_isEffectEnabled_QApplication_SA_NW4UIEffect_Qt_Z_destroy(dci_Translated_dci_tr_isEffectEnabled_QApplication_SA_NW4UIEffect_Qt_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setEffectEnabled_QApplication_SAXW4UIEffect_Qt_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setEffectEnabled_QApplication_SAXW4UIEffect_Qt_N_Z dci_tr_setEffectEnabled_QApplication_SAXW4UIEffect_Qt_N_Z(Qt::UIEffect p0, bool p1) noexcept {
    dci_Translated_dci_tr_setEffectEnabled_QApplication_SAXW4UIEffect_Qt_N_Z out{};
    try {
        QApplication::setEffectEnabled(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setEffectEnabled_QApplication_SAXW4UIEffect_Qt_N_Z_destroy(dci_Translated_dci_tr_setEffectEnabled_QApplication_SAXW4UIEffect_Qt_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_exec_QApplication_SAHXZ {
    std::int8_t tag;
    int ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_exec_QApplication_SAHXZ dci_tr_exec_QApplication_SAHXZ(void) noexcept {
    dci_Translated_dci_tr_exec_QApplication_SAHXZ out{};
    try {
        out.ok = QApplication::exec();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_exec_QApplication_SAHXZ_destroy(dci_Translated_dci_tr_exec_QApplication_SAHXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_autoSipEnabled_QApplication_QEBA_NXZ {
    std::int8_t tag;
    bool ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_autoSipEnabled_QApplication_QEBA_NXZ dci_tr_autoSipEnabled_QApplication_QEBA_NXZ(const QApplication* self) noexcept {
    dci_Translated_dci_tr_autoSipEnabled_QApplication_QEBA_NXZ out{};
    try {
        out.ok = self->autoSipEnabled();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_autoSipEnabled_QApplication_QEBA_NXZ_destroy(dci_Translated_dci_tr_autoSipEnabled_QApplication_QEBA_NXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setStyleSheet_QApplication_QEAAXAEBVQString_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setStyleSheet_QApplication_QEAAXAEBVQString_Z dci_tr_setStyleSheet_QApplication_QEAAXAEBVQString_Z(QApplication* self, const QString& p0) noexcept {
    dci_Translated_dci_tr_setStyleSheet_QApplication_QEAAXAEBVQString_Z out{};
    try {
        self->setStyleSheet(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setStyleSheet_QApplication_QEAAXAEBVQString_Z_destroy(dci_Translated_dci_tr_setStyleSheet_QApplication_QEAAXAEBVQString_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_setAutoSipEnabled_QApplication_QEAAX_N_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_setAutoSipEnabled_QApplication_QEAAX_N_Z dci_tr_setAutoSipEnabled_QApplication_QEAAX_N_Z(QApplication* self, const bool p0) noexcept {
    dci_Translated_dci_tr_setAutoSipEnabled_QApplication_QEAAX_N_Z out{};
    try {
        self->setAutoSipEnabled(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_setAutoSipEnabled_QApplication_QEAAX_N_Z_destroy(dci_Translated_dci_tr_setAutoSipEnabled_QApplication_QEAAX_N_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_closeAllWindows_QApplication_SAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_closeAllWindows_QApplication_SAXXZ dci_tr_closeAllWindows_QApplication_SAXXZ(void) noexcept {
    dci_Translated_dci_tr_closeAllWindows_QApplication_SAXXZ out{};
    try {
        QApplication::closeAllWindows();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_closeAllWindows_QApplication_SAXXZ_destroy(dci_Translated_dci_tr_closeAllWindows_QApplication_SAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_aboutQt_QApplication_SAXXZ {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_aboutQt_QApplication_SAXXZ dci_tr_aboutQt_QApplication_SAXXZ(void) noexcept {
    dci_Translated_dci_tr_aboutQt_QApplication_SAXXZ out{};
    try {
        QApplication::aboutQt();
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_aboutQt_QApplication_SAXXZ_destroy(dci_Translated_dci_tr_aboutQt_QApplication_SAXXZ* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}

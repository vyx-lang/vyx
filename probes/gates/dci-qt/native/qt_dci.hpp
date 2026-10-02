#pragma once

// Bind entry is the real QtCore module, not a C façade over QPoint.
#include <QtCore/QtCore>

// qVersion is `extern "C" const char *qVersion() noexcept` (qtversion.h): a
// pointer to Qt's process-lifetime version string. Adapter will not guess
// raw-pointer ownership; the return is a borrow, never owned.
/* dci-ownership
{
  "qVersion()": { "return": "borrow" }
}
dci-ownership-end */

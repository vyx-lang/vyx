#pragma once

// Real Qt Widgets, not a C façade. Ownership for raw pointers must be explicit.
#include <QtCore/QString>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <QtWidgets/QLCDNumber>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QAbstractButton>

/* dci-ownership
{
  "QApplication::constructor(int&,char**,int)": { "parameters": { "1": "borrow" } },
  "QCoreApplication::constructor(int&,char**,int)": { "parameters": { "1": "borrow" } },
  "QWidget::constructor(QWidget*,Qt::WindowFlags)": { "parameters": { "0": "borrow" } },
  "QLCDNumber::constructor(QWidget*)": { "parameters": { "0": "borrow" } },
  "QPushButton::constructor(QWidget*)": { "parameters": { "0": "borrow" } },
  "QPushButton::constructor(const QString&,QWidget*)": { "parameters": { "1": "borrow" } },
  "QString::constructor(const char*)": { "parameters": { "0": "borrow" } },
  "QString::fromUtf8(const char*,qsizetype)": { "parameters": { "0": "borrow" } },
  "QWidget::setParent(QWidget*)": { "parameters": { "0": "borrow" } },
  "QObject::timerEvent(QTimerEvent*)": { "parameters": { "0": "borrow_mut" } },
  "QWidget::mouseReleaseEvent(QMouseEvent*)": { "parameters": { "0": "borrow_mut" } }
}
dci-ownership-end */

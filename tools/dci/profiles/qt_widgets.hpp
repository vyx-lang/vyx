#pragma once

// SDK Qt Widgets lifetime facts. These are library protocol declarations,
// not facts inferred from C++ pointer spelling. The adapter binds exact native
// signatures and rejects changes or missing facts for other pointer APIs.
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
  "QWidget::mouseReleaseEvent(QMouseEvent*)": { "parameters": { "0": "borrow_mut" } },
  "QVBoxLayout::constructor(QWidget*)": { "parameters": { "0": "borrow" } },
  "QLayout::addWidget(QWidget*)": { "parameters": { "0": "borrow" } },
  "QBoxLayout::addWidget(QWidget*,int,Qt::Alignment)": { "parameters": { "0": "borrow" } }
}
dci-ownership-end */

#pragma once
#include <QtWidgets/QWidget>

// Exception regression instrumentation, linked only by check_unwind.py.
// These functions are not required by DCI or the counter application.
void qt_counter_watch(QWidget* widget, int id);
int qt_counter_throw(int mode);
int qt_counter_call_visible(QWidget* widget);

/* dci-ownership
{
  "qt_counter_watch(QWidget*,int)": { "parameters": { "0": "borrow_mut" } },
  "qt_counter_call_visible(QWidget*)": { "parameters": { "0": "borrow_mut" } }
}
dci-ownership-end */

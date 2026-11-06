// Interface languages. Translations are compiled into the program (:/i18n/ruche_<code>.qm,
// plus Qt's own qtbase_<code>.qm for standard buttons and dialogs).
#pragma once

#include <QPair>
#include <QString>
#include <QVector>

class QApplication;

namespace i18n {
// code, name in that language; English first
const QVector<QPair<QString, QString>> &languages();
// The language to use: the setting, else the first system language Ruche has, else "en".
QString current();
void install(QApplication &app);
} // namespace i18n

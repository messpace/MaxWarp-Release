#include "ThemeController.h"

#include <QGuiApplication>
#include <QSettings>
#include <QStyleHints>

ThemeController::ThemeController(QObject *parent)
    : QObject(parent)
{
    const QString saved = QSettings().value(QStringLiteral("theme/mode")).toString();
    if (saved == QLatin1String("light") || saved == QLatin1String("dark"))
        m_mode = saved;

    connect(qGuiApp->styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this] { refreshDarkActive(); });
    refreshDarkActive();
}

void ThemeController::setMode(const QString &mode)
{
    if (mode != QLatin1String("system") && mode != QLatin1String("light")
        && mode != QLatin1String("dark"))
        return;
    if (m_mode == mode)
        return;
    m_mode = mode;
    QSettings().setValue(QStringLiteral("theme/mode"), m_mode);
    emit modeChanged();
    refreshDarkActive();
}

void ThemeController::cycle()
{
    if (m_mode == QLatin1String("system"))
        setMode(QStringLiteral("light"));
    else if (m_mode == QLatin1String("light"))
        setMode(QStringLiteral("dark"));
    else
        setMode(QStringLiteral("system"));
}

void ThemeController::refreshDarkActive()
{
    bool dark = false;
    if (m_mode == QLatin1String("dark"))
        dark = true;
    else if (m_mode == QLatin1String("light"))
        dark = false;
    else
        dark = qGuiApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    if (dark == m_darkActive)
        return;
    m_darkActive = dark;
    emit darkActiveChanged();
}

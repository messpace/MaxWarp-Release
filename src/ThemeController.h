#pragma once

#include <QObject>
#include <QString>

// Dark/light plumbing: follows the OS scheme by default, with a manual
// override persisted across runs. The visual identity itself is decided in the
// GUI design rounds; this only answers "is dark active right now".
class ThemeController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged) // system|light|dark
    Q_PROPERTY(bool darkActive READ darkActive NOTIFY darkActiveChanged)

public:
    explicit ThemeController(QObject *parent = nullptr);

    QString mode() const { return m_mode; }
    void setMode(const QString &mode);
    bool darkActive() const { return m_darkActive; }

    Q_INVOKABLE void cycle(); // system -> light -> dark -> system

signals:
    void modeChanged();
    void darkActiveChanged();

private:
    void refreshDarkActive();

    QString m_mode = QStringLiteral("system");
    bool m_darkActive = false;
};

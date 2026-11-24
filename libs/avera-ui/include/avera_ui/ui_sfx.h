#pragma once

// SPDX-License-Identifier: GPL-2.0-only

#include <QObject>
#include <QString>

class QAudioSink;

namespace avera {

/// Procedural "glassy" UI chimes, rendered at startup and mixed on demand.
/// No sample assets, no decoder: just additive sine partials through QAudioSink.
class AveraSfx : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)

public:
    enum Cue { Navigate = 0, Select, Back, Startup, CueCount };

    explicit AveraSfx(QObject* parent = nullptr);
    ~AveraSfx() override;

    Q_INVOKABLE void play(int cue);
    Q_INVOKABLE void playNamed(const QString& cue);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);

signals:
    void enabledChanged();

private:
    bool startDevice();
    struct Voice;
    class Mixer;
    Mixer* m_mixer = nullptr;
    QAudioSink* m_sink = nullptr;
    bool m_enabled = true;
};

}  // namespace avera

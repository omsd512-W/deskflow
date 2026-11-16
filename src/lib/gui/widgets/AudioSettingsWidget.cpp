/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AudioSettingsWidget.h"

#include "common/Settings.h"
#include "gui/core/AudioProcess.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

using A = Settings::Audio;

namespace {

bool micDriverInstalled()
{
#if defined(Q_OS_MACOS)
  return QFileInfo::exists(QStringLiteral("/Library/Audio/Plug-Ins/HAL/DeskflowMicrophone.driver"));
#else
  return false;
#endif
}

QString comboValue(const QComboBox *combo)
{
  return combo->currentData().toString();
}

} // namespace

AudioSettingsWidget::AudioSettingsWidget(QWidget *parent) : QWidget(parent)
{
  auto *layout = new QVBoxLayout(this);

  m_clientGroup = new QGroupBox(this);
  auto *clientLayout = new QVBoxLayout(m_clientGroup);
  m_forward = new QCheckBox(m_clientGroup);
  m_keepLocal = new QCheckBox(m_clientGroup);
  auto *keepRow = new QHBoxLayout;
  keepRow->addSpacing(20);
  keepRow->addWidget(m_keepLocal);
  m_remoteMicrophone = new QCheckBox(m_clientGroup);
  m_driverNote = new QLabel(m_clientGroup);
  m_driverNote->setWordWrap(true);
  clientLayout->addWidget(m_forward);
  clientLayout->addLayout(keepRow);
  clientLayout->addWidget(m_remoteMicrophone);
  clientLayout->addWidget(m_driverNote);
  layout->addWidget(m_clientGroup);
#if !defined(Q_OS_MACOS)
  m_clientGroup->setVisible(false);
#endif

  m_serverGroup = new QGroupBox(this);
  auto *serverLayout = new QFormLayout(m_serverGroup);
  m_allowPlayback = new QCheckBox(m_serverGroup);
  m_output = new QComboBox(m_serverGroup);
  m_outputLabel = new QLabel(m_serverGroup);
  m_allowMicrophone = new QCheckBox(m_serverGroup);
  m_input = new QComboBox(m_serverGroup);
  m_inputLabel = new QLabel(m_serverGroup);
  serverLayout->addRow(m_allowPlayback);
  serverLayout->addRow(m_outputLabel, m_output);
  serverLayout->addRow(m_allowMicrophone);
  serverLayout->addRow(m_inputLabel, m_input);
  layout->addWidget(m_serverGroup);

  auto *portRow = new QFormLayout;
  m_port = new QSpinBox(this);
  m_port->setRange(1, 65535);
  m_portLabel = new QLabel(this);
  portRow->addRow(m_portLabel, m_port);
  layout->addLayout(portRow);

  m_note = new QLabel(this);
  m_note->setWordWrap(true);
  layout->addWidget(m_note);
  layout->addStretch();

  const auto [outputs, inputs] = deskflow::gui::AudioProcess::isAvailable()
                                     ? deskflow::gui::AudioProcess::listDevices()
                                     : QPair<QStringList, QStringList>{};
  fillDevices(m_output, outputs, Settings::value(A::OutputDevice).toString());
  fillDevices(m_input, inputs, Settings::value(A::InputDevice).toString());

  retranslate();
  load();

  for (auto *box : {m_forward, m_keepLocal, m_remoteMicrophone, m_allowPlayback, m_allowMicrophone}) {
    connect(box, &QCheckBox::toggled, this, [this] {
      updateEnabled();
      Q_EMIT changed();
    });
  }
  for (auto *combo : {m_output, m_input}) {
    connect(combo, &QComboBox::currentIndexChanged, this, &AudioSettingsWidget::changed);
  }
  connect(m_port, &QSpinBox::valueChanged, this, &AudioSettingsWidget::changed);
}

void AudioSettingsWidget::retranslate()
{
  m_clientGroup->setTitle(tr("When this computer is a client"));
  m_forward->setText(tr("Play this computer's sound on the server"));
  m_keepLocal->setText(tr("Keep playing it on this computer too"));
  m_remoteMicrophone->setText(tr("Use the server's microphone (appears as the input \"Deskflow 麦克风\")"));
  m_driverNote->setText(
      micDriverInstalled() ? QString()
                           : tr("The Deskflow microphone driver is not installed, so the server's microphone cannot "
                                "appear on this computer.")
  );
  m_driverNote->setVisible(!micDriverInstalled());

  m_serverGroup->setTitle(tr("When this computer is the server"));
  m_allowPlayback->setText(tr("Play sound sent by clients"));
  m_outputLabel->setText(tr("Play on"));
  m_allowMicrophone->setText(tr("Let clients use this computer's microphone"));
  m_inputLabel->setText(tr("Record from"));
  if (m_output->count() > 0) {
    m_output->setItemText(0, tr("System default"));
  }
  if (m_input->count() > 0) {
    m_input->setItemText(0, tr("System default"));
  }

  m_portLabel->setText(tr("Audio port"));
  m_note->setText(tr("Sound and microphone audio travel encrypted over TCP and UDP on the audio port, which "
                     "the server's firewall must allow. They only go to computers Deskflow already trusts."));
}

void AudioSettingsWidget::fillDevices(QComboBox *combo, const QStringList &names, const QString &current)
{
  combo->addItem(QString(), QString());
  for (const auto &name : names) {
    combo->addItem(name, name);
  }
  if (!current.isEmpty() && combo->findData(current) < 0) {
    //: A saved audio device that is not connected right now
    combo->addItem(tr("%1 (not connected)").arg(current), current);
  }
}

void AudioSettingsWidget::load()
{
  const QSignalBlocker blocks[] = {
      QSignalBlocker(m_forward),       QSignalBlocker(m_keepLocal), QSignalBlocker(m_remoteMicrophone),
      QSignalBlocker(m_allowPlayback), QSignalBlocker(m_allowMicrophone), QSignalBlocker(m_output),
      QSignalBlocker(m_input),         QSignalBlocker(m_port),
  };
  m_forward->setChecked(Settings::value(A::ForwardSystemAudio).toBool());
  m_keepLocal->setChecked(Settings::value(A::KeepLocalPlayback).toBool());
  m_remoteMicrophone->setChecked(Settings::value(A::UseRemoteMicrophone).toBool());
  m_allowPlayback->setChecked(Settings::value(A::AllowRemotePlayback).toBool());
  m_allowMicrophone->setChecked(Settings::value(A::AllowMicrophoneSharing).toBool());
  m_output->setCurrentIndex(std::max(0, m_output->findData(Settings::value(A::OutputDevice).toString())));
  m_input->setCurrentIndex(std::max(0, m_input->findData(Settings::value(A::InputDevice).toString())));
  m_port->setValue(Settings::value(A::Port).toInt());
  updateEnabled();
}

void AudioSettingsWidget::save() const
{
  Settings::setValue(A::ForwardSystemAudio, m_forward->isChecked());
  Settings::setValue(A::KeepLocalPlayback, m_keepLocal->isChecked());
  Settings::setValue(A::UseRemoteMicrophone, m_remoteMicrophone->isChecked());
  Settings::setValue(A::AllowRemotePlayback, m_allowPlayback->isChecked());
  Settings::setValue(A::AllowMicrophoneSharing, m_allowMicrophone->isChecked());
  Settings::setValue(A::OutputDevice, comboValue(m_output));
  Settings::setValue(A::InputDevice, comboValue(m_input));
  // Stored only when it differs from the default, which follows the Deskflow port.
  Settings::setValue(
      A::Port, m_port->value() == Settings::defaultValue(A::Port).toInt() ? QVariant() : QVariant(m_port->value())
  );
}

bool AudioSettingsWidget::isModified() const
{
  return m_forward->isChecked() != Settings::value(A::ForwardSystemAudio).toBool() ||
         m_keepLocal->isChecked() != Settings::value(A::KeepLocalPlayback).toBool() ||
         m_remoteMicrophone->isChecked() != Settings::value(A::UseRemoteMicrophone).toBool() ||
         m_allowPlayback->isChecked() != Settings::value(A::AllowRemotePlayback).toBool() ||
         m_allowMicrophone->isChecked() != Settings::value(A::AllowMicrophoneSharing).toBool() ||
         comboValue(m_output) != Settings::value(A::OutputDevice).toString() ||
         comboValue(m_input) != Settings::value(A::InputDevice).toString() ||
         m_port->value() != Settings::value(A::Port).toInt();
}

bool AudioSettingsWidget::isDefault() const
{
  return m_forward->isChecked() == Settings::defaultValue(A::ForwardSystemAudio).toBool() &&
         m_keepLocal->isChecked() == Settings::defaultValue(A::KeepLocalPlayback).toBool() &&
         m_remoteMicrophone->isChecked() == Settings::defaultValue(A::UseRemoteMicrophone).toBool() &&
         m_allowPlayback->isChecked() == Settings::defaultValue(A::AllowRemotePlayback).toBool() &&
         m_allowMicrophone->isChecked() == Settings::defaultValue(A::AllowMicrophoneSharing).toBool() &&
         comboValue(m_output).isEmpty() && comboValue(m_input).isEmpty() &&
         m_port->value() == Settings::defaultValue(A::Port).toInt();
}

void AudioSettingsWidget::resetToDefault()
{
  m_forward->setChecked(Settings::defaultValue(A::ForwardSystemAudio).toBool());
  m_keepLocal->setChecked(Settings::defaultValue(A::KeepLocalPlayback).toBool());
  m_remoteMicrophone->setChecked(Settings::defaultValue(A::UseRemoteMicrophone).toBool());
  m_allowPlayback->setChecked(Settings::defaultValue(A::AllowRemotePlayback).toBool());
  m_allowMicrophone->setChecked(Settings::defaultValue(A::AllowMicrophoneSharing).toBool());
  m_output->setCurrentIndex(0);
  m_input->setCurrentIndex(0);
  m_port->setValue(Settings::defaultValue(A::Port).toInt());
  updateEnabled();
}

void AudioSettingsWidget::updateEnabled()
{
  const bool writable = Settings::isWritable();
  setEnabled(writable);
  m_keepLocal->setEnabled(m_forward->isChecked());
  m_output->setEnabled(m_allowPlayback->isChecked());
  m_input->setEnabled(m_allowMicrophone->isChecked());
}

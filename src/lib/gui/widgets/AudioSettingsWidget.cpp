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
  m_mixMode = new QComboBox(m_serverGroup);
  m_mixMode->addItem(QString(), QStringLiteral("mix"));
  m_mixMode->addItem(QString(), QStringLiteral("followCursor"));
  m_mixModeLabel = new QLabel(m_serverGroup);
  m_output = new QComboBox(m_serverGroup);
  m_outputLabel = new QLabel(m_serverGroup);
  m_input = new QComboBox(m_serverGroup);
  m_inputLabel = new QLabel(m_serverGroup);
  m_serverNote = new QLabel(m_serverGroup);
  m_serverNote->setWordWrap(true);
  serverLayout->addRow(m_mixModeLabel, m_mixMode);
  serverLayout->addRow(m_outputLabel, m_output);
  serverLayout->addRow(m_inputLabel, m_input);
  serverLayout->addRow(m_serverNote);
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

  for (auto *box : {m_forward, m_keepLocal, m_remoteMicrophone}) {
    connect(box, &QCheckBox::toggled, this, [this] {
      updateEnabled();
      Q_EMIT changed();
    });
  }
  for (auto *combo : {m_mixMode, m_output, m_input}) {
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
  m_mixModeLabel->setText(tr("Clients' sound"));
  //: Every client's sound plays at once
  m_mixMode->setItemText(0, tr("Mix them together"));
  m_mixMode->setItemText(1, tr("Only the computer the cursor is on"));
  m_outputLabel->setText(tr("Play on"));
  m_inputLabel->setText(tr("Record from"));
  m_serverNote->setText(
      tr("Each client decides whether to send its sound and use this microphone; change a "
         "client's switches here in the Audio menu.")
  );
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
      QSignalBlocker(m_forward), QSignalBlocker(m_keepLocal), QSignalBlocker(m_remoteMicrophone),
      QSignalBlocker(m_mixMode), QSignalBlocker(m_output),    QSignalBlocker(m_input),
      QSignalBlocker(m_port),
  };
  m_forward->setChecked(Settings::value(A::ForwardSystemAudio).toBool());
  m_keepLocal->setChecked(Settings::value(A::KeepLocalPlayback).toBool());
  m_remoteMicrophone->setChecked(Settings::value(A::UseRemoteMicrophone).toBool());
  m_mixMode->setCurrentIndex(std::max(0, m_mixMode->findData(Settings::value(A::MixMode).toString())));
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
  Settings::setValue(A::MixMode, comboValue(m_mixMode));
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
         comboValue(m_mixMode) != Settings::value(A::MixMode).toString() ||
         comboValue(m_output) != Settings::value(A::OutputDevice).toString() ||
         comboValue(m_input) != Settings::value(A::InputDevice).toString() ||
         m_port->value() != Settings::value(A::Port).toInt();
}

bool AudioSettingsWidget::isDefault() const
{
  return m_forward->isChecked() == Settings::defaultValue(A::ForwardSystemAudio).toBool() &&
         m_keepLocal->isChecked() == Settings::defaultValue(A::KeepLocalPlayback).toBool() &&
         m_remoteMicrophone->isChecked() == Settings::defaultValue(A::UseRemoteMicrophone).toBool() &&
         comboValue(m_mixMode) == Settings::defaultValue(A::MixMode).toString() && comboValue(m_output).isEmpty() &&
         comboValue(m_input).isEmpty() && m_port->value() == Settings::defaultValue(A::Port).toInt();
}

void AudioSettingsWidget::resetToDefault()
{
  m_forward->setChecked(Settings::defaultValue(A::ForwardSystemAudio).toBool());
  m_keepLocal->setChecked(Settings::defaultValue(A::KeepLocalPlayback).toBool());
  m_remoteMicrophone->setChecked(Settings::defaultValue(A::UseRemoteMicrophone).toBool());
  m_mixMode->setCurrentIndex(0);
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
}

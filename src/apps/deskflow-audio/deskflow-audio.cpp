/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

// The audio helper. Without --server or --client it runs as the service that
// follows Deskflow's settings and shares real audio devices (AudioService).
//
// With --server or --client it is a link test tool: two instances connect
// over the same TLS + UDP transport, send test tones on simulated capture
// clocks, and check what comes out of simulated playback devices whose clocks
// run at a different speed.

#include "AudioService.h"
#include "TestHarness.h"

#include "audio/AudioDevice.h"
#include "audio/AudioPeer.h"
#include "audio/ControlProtocol.h"
#include "common/Constants.h"
#include "common/Settings.h"
#include "net/SecureUtils.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>

#include <openssl/pem.h>
#include <openssl/x509.h>

#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>

#if defined(Q_OS_WIN)
#include <windows.h>
// After windows.h.
#include <psapi.h>
#elif defined(Q_OS_MACOS)
#include <cerrno>
#include <mach/mach.h>
#include <signal.h>
#else
#include <cerrno>
#include <signal.h>
#endif

using namespace deskflow::audio;

namespace {

constexpr double kToneAmplitude = 0.25;
const StreamKind kKinds[] = {StreamKind::System, StreamKind::Microphone};

std::atomic<bool> g_interrupted{false};

void onSignal(int)
{
  g_interrupted = true;
}

void say(const QString &line)
{
  static QTextStream out(stdout);
  out << QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")) << ' ' << line << Qt::endl;
}

double toneHz(StreamKind kind)
{
  return kind == StreamKind::System ? 440.0 : 1000.0;
}

QString kindName(StreamKind kind)
{
  return QString::fromLatin1(toString(kind));
}

QByteArray certificateSha256(const QString &pemPath, QString *error)
{
  QFile file(pemPath);
  if (!file.open(QIODevice::ReadOnly)) {
    *error = QStringLiteral("cannot read %1").arg(pemPath);
    return {};
  }
  const auto pem = file.readAll();
  BIO *bio = BIO_new_mem_buf(pem.constData(), int(pem.size()));
  X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (!cert) {
    *error = QStringLiteral("no certificate in %1").arg(pemPath);
    return {};
  }
  const auto fingerprint = deskflow::sslCertFingerprint(cert, QCryptographicHash::Sha256);
  X509_free(cert);
  return fingerprint.data;
}

QJsonObject toJson(const ReceiveStats &s)
{
  return {
      {"packets", double(s.packets)},       {"concealed", double(s.concealed)},
      {"late", double(s.late)},
      {"dropped", double(s.dropped)},       {"underruns", double(s.underruns)},
      {"resamplerErrors", double(s.resamplerErrors)},
      {"bufferMs", s.bufferMs},             {"targetMs", s.targetMs},
      {"jitterMs", s.jitterMs},             {"ratioPpm", s.ratioPpm},
      {"playing", s.playing},
  };
}

QJsonObject toJson(const SendStats &s)
{
  return {
      {"packets", double(s.packets)},
      {"bytes", double(s.bytes)},
      {"suppressedFrames", double(s.suppressedFrames)},
      {"errors", double(s.errors)},
      {"paused", s.paused},
  };
}

QJsonObject toJson(const test::VirtualSink::Analysis &a)
{
  return {
      {"frames", double(a.frames)},
      {"audibleFrames", double(a.audibleFrames)},
      {"dropoutMs", double(a.dropoutFrames) * 1000.0 / kSampleRate},
      {"glitches", double(a.glitches)},
      {"toneHz", a.toneHz},
      {"peak", a.peak},
  };
}

//! Resident memory of this process in MiB, for spotting leaks in long runs.
double residentMb()
{
#if defined(Q_OS_WIN)
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    return double(counters.WorkingSetSize) / (1024 * 1024);
  }
#elif defined(Q_OS_MACOS)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
    return double(info.resident_size) / (1024 * 1024);
  }
#endif
  return 0;
}

//! Whether the process that started us is gone (so we do not outlive the GUI or daemon).
class ParentWatch
{
public:
  explicit ParentWatch(qint64 pid) : m_pid(pid)
  {
#if defined(Q_OS_WIN)
    if (pid > 0) {
      m_handle = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    }
#endif
  }
  ~ParentWatch()
  {
#if defined(Q_OS_WIN)
    if (m_handle) {
      CloseHandle(m_handle);
    }
#endif
  }
  bool gone() const
  {
    if (m_pid <= 0) {
      return false;
    }
#if defined(Q_OS_WIN)
    return !m_handle || WaitForSingleObject(m_handle, 0) == WAIT_OBJECT_0;
#else
    return kill(pid_t(m_pid), 0) != 0 && errno == ESRCH;
#endif
  }

private:
  qint64 m_pid;
#if defined(Q_OS_WIN)
  HANDLE m_handle = nullptr;
#endif
};

//! A line for the GUI that started us (see AudioProcess).
void tellGui(const char *kind, const QJsonObject &object)
{
  static QTextStream out(stdout);
  out << kind << ' ' << QJsonDocument(object).toJson(QJsonDocument::Compact) << Qt::endl;
}

//! The running service, for commands from the GUI. Main thread only.
AudioService *g_service = nullptr;

//! Hands the GUI's commands (one per line on stdin) to the service.
void readCommands()
{
  // Never deleted: the reading thread may outlive the service, blocked on
  // stdin, and the calls it queues run on the main thread.
  auto *relay = new QObject;
  std::thread([relay] {
    std::string line;
    while (std::getline(std::cin, line)) {
      const auto text = QString::fromUtf8(line).trimmed();
      if (!text.isEmpty()) {
        QMetaObject::invokeMethod(
            relay,
            [text] {
              if (g_service) {
                g_service->handleCommand(text);
              }
            },
            Qt::QueuedConnection
        );
      }
    }
  }).detach();
}

int runService(const QString &logPath, qint64 parentPid, const QString &trustedDb, double statusLogSeconds)
{
  std::unique_ptr<QFile> logFile;
  if (!logPath.isEmpty()) {
    if (QFileInfo(logPath).size() > 4 * 1024 * 1024) {
      QFile::remove(logPath + QStringLiteral(".1"));
      QFile::rename(logPath, logPath + QStringLiteral(".1"));
    }
    logFile = std::make_unique<QFile>(logPath);
    if (!logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
      say(QStringLiteral("cannot write the log file %1").arg(logPath));
      logFile.reset();
    }
  }
  const auto write = [&logFile](const QString &line) {
    say(line);
    if (logFile) {
      logFile->write(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz ")).toUtf8());
      logFile->write(line.toUtf8());
      logFile->write("\n");
      logFile->flush();
    }
  };

  AudioService service;
  service.setTrustedDbOverride(trustedDb);
  if (!logPath.isEmpty()) {
    service.setStateDirectory(QFileInfo(logPath).absolutePath());
  }
  QObject::connect(&service, &AudioService::logMessage, &service, write);
  QObject::connect(&service, &AudioService::settingRequested, &service, [](const QString &key, bool value) {
    tellGui("SETTING", {{"key", key}, {"value", value}});
  });
  // One line per change, for the GUI that started us to read.
  QByteArray lastStatus;
  QObject::connect(&service, &AudioService::statusChanged, &service, [&service, &lastStatus] {
    const auto status = service.status();
    const auto compact = QJsonDocument(status).toJson(QJsonDocument::Compact);
    if (compact != lastStatus) {
      lastStatus = compact;
      tellGui("STATUS", status);
    }
  });
  write(QStringLiteral("%1 audio helper starting (pid %2)").arg(kAppName).arg(QCoreApplication::applicationPid()));
  service.start();
  g_service = &service;
  readCommands();

  // A periodic snapshot in the log file, for diagnosing problems afterwards.
  QTimer statusLog;
  QObject::connect(&statusLog, &QTimer::timeout, &service, [&service, &logFile] {
    if (logFile) {
      logFile->write(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz ")).toUtf8());
      auto status = service.status();
      status.insert(QStringLiteral("rssMb"), qRound(residentMb() * 10) / 10.0);
      logFile->write("status ");
      logFile->write(QJsonDocument(status).toJson(QJsonDocument::Compact));
      logFile->write("\n");
      logFile->flush();
    }
  });
  if (statusLogSeconds > 0) {
    statusLog.start(int(statusLogSeconds * 1000));
  }

  ParentWatch parent(parentPid);
  QTimer watchdog;
  QObject::connect(&watchdog, &QTimer::timeout, &service, [&parent, &write] {
    if (g_interrupted) {
      write(QStringLiteral("stopping"));
      QCoreApplication::quit();
    } else if (parent.gone()) {
      write(QStringLiteral("the process that started us has exited, stopping"));
      QCoreApplication::quit();
    }
  });
  watchdog.start(200);
  const int result = QCoreApplication::exec();
  g_service = nullptr;
  return result;
}

} // namespace

int main(int argc, char **argv)
{
  QCoreApplication::setApplicationName(QStringLiteral("%1 Audio").arg(kAppName));
  QCoreApplication app(argc, argv);

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral(
      "Deskflow audio helper. Runs as a service following Deskflow's settings, or, with --server or --client, "
      "as a link test that sends test tones and checks what arrives."
  ));
  parser.addHelpOption();
  const QCommandLineOption serverOpt(QStringLiteral("server"), QStringLiteral("Wait for the other computer."));
  const QCommandLineOption clientOpt(
      QStringLiteral("client"), QStringLiteral("Connect to the computer at <host>."), QStringLiteral("host")
  );
  const QCommandLineOption portOpt(
      QStringLiteral("port"), QStringLiteral("TCP and UDP port."), QStringLiteral("port"), QStringLiteral("24801")
  );
  const QCommandLineOption certOpt(
      QStringLiteral("cert"), QStringLiteral("PEM certificate and key (default: Deskflow's)."), QStringLiteral("path")
  );
  const QCommandLineOption generateCertOpt(
      QStringLiteral("generate-cert"), QStringLiteral("Create the --cert file if it does not exist.")
  );
  const QCommandLineOption trustedDbOpt(
      QStringLiteral("trusted-db"), QStringLiteral("Fingerprint database of trusted peers (default: Deskflow's)."),
      QStringLiteral("path")
  );
  const QCommandLineOption trustOpt(
      QStringLiteral("trust"), QStringLiteral("Also trust this SHA-256 fingerprint (hex, colons allowed)."),
      QStringLiteral("sha256")
  );
  const QCommandLineOption trustSelfOpt(
      QStringLiteral("trust-self"), QStringLiteral("Also trust our own certificate (tests on one computer).")
  );
  const QCommandLineOption nameOpt(
      QStringLiteral("name"), QStringLiteral("Name shown to the peer."), QStringLiteral("name"),
      QSysInfo::machineHostName()
  );
  const QCommandLineOption sendOpt(
      QStringLiteral("send"),
      QStringLiteral("Send a test tone (system 440 Hz, microphone 1 kHz) as this stream. Repeatable."),
      QStringLiteral("kind")
  );
  const QCommandLineOption sourcePpmOpt(
      QStringLiteral("source-ppm"), QStringLiteral("Clock error of the simulated capture device."), QStringLiteral("ppm"),
      QStringLiteral("0")
  );
  const QCommandLineOption sinkPpmOpt(
      QStringLiteral("sink-ppm"), QStringLiteral("Clock error of the simulated playback device."), QStringLiteral("ppm"),
      QStringLiteral("0")
  );
  const QCommandLineOption periodOpt(
      QStringLiteral("period-frames"), QStringLiteral("Playback device period in frames."), QStringLiteral("frames"),
      QStringLiteral("480")
  );
  const QCommandLineOption lossOpt(
      QStringLiteral("loss"), QStringLiteral("Drop this percentage of outgoing datagrams."), QStringLiteral("percent"),
      QStringLiteral("0")
  );
  const QCommandLineOption delayOpt(
      QStringLiteral("delay-ms"), QStringLiteral("Delay outgoing datagrams."), QStringLiteral("ms"), QStringLiteral("0")
  );
  const QCommandLineOption jitterOpt(
      QStringLiteral("jitter-ms"), QStringLiteral("Add up to this much random delay, which also reorders."),
      QStringLiteral("ms"), QStringLiteral("0")
  );
  const QCommandLineOption durationOpt(
      QStringLiteral("duration"), QStringLiteral("Quit after this many seconds (0: until interrupted)."),
      QStringLiteral("seconds"), QStringLiteral("0")
  );
  const QCommandLineOption statsOpt(
      QStringLiteral("stats-interval"), QStringLiteral("Seconds between stats lines."), QStringLiteral("seconds"),
      QStringLiteral("5")
  );
  const QCommandLineOption jsonlOpt(
      QStringLiteral("jsonl"), QStringLiteral("Append every stats sample to this file as a JSON line."),
      QStringLiteral("path")
  );
  const QCommandLineOption recordOpt(
      QStringLiteral("record"), QStringLiteral("Save received audio to <prefix>-<kind>.wav."), QStringLiteral("prefix")
  );
  const QCommandLineOption recordSecondsOpt(
      QStringLiteral("record-seconds"), QStringLiteral("How much to record."), QStringLiteral("seconds"),
      QStringLiteral("10")
  );
  const QCommandLineOption logFileOpt(
      QStringLiteral("log-file"), QStringLiteral("Service: also append the log to this file."), QStringLiteral("path")
  );
  const QCommandLineOption parentPidOpt(
      QStringLiteral("parent-pid"), QStringLiteral("Service: exit when this process exits."), QStringLiteral("pid")
  );
  const QCommandLineOption listDevicesOpt(
      QStringLiteral("list-devices"), QStringLiteral("Print the output and input devices as JSON and exit.")
  );
  const QCommandLineOption settingsOpt(
      QStringLiteral("settings"), QStringLiteral("Service: use this settings file instead of Deskflow's (for tests)."),
      QStringLiteral("path")
  );
  const QCommandLineOption amplitudeOpt(
      QStringLiteral("amplitude"), QStringLiteral("Test: test tone amplitude, 0 to 1."), QStringLiteral("value"),
      QString::number(kToneAmplitude)
  );
  const QCommandLineOption wantOpt(
      QStringLiteral("want"),
      QStringLiteral("Test: ask the other side for its stream of this kind (system or microphone); repeatable."),
      QStringLiteral("kind")
  );
  const QCommandLineOption statusLogOpt(
      QStringLiteral("status-log-interval"), QStringLiteral("Service: seconds between status lines in the log file."),
      QStringLiteral("seconds"), QStringLiteral("30")
  );
  parser.addOptions({logFileOpt, parentPidOpt, listDevicesOpt, settingsOpt, amplitudeOpt, wantOpt, statusLogOpt});
  parser.addOptions(
      {serverOpt, clientOpt, portOpt, certOpt, generateCertOpt, trustedDbOpt, trustOpt, trustSelfOpt, nameOpt, sendOpt,
       sourcePpmOpt, sinkPpmOpt, periodOpt, lossOpt, delayOpt, jitterOpt, durationOpt, statsOpt, jsonlOpt, recordOpt,
       recordSecondsOpt}
  );
  parser.process(app);

  const auto fail = [](const QString &message) {
    QTextStream(stderr) << message << Qt::endl;
    return 2;
  };

  if (parser.isSet(listDevicesOpt)) {
    const auto toArray = [](const QStringList &names) { return QJsonArray::fromStringList(names); };
    const QJsonObject devices{
        {"output", toArray(AudioDevice::devices(AudioDevice::Direction::Playback))},
        {"input", toArray(AudioDevice::devices(AudioDevice::Direction::Capture))},
    };
    QTextStream(stdout) << QJsonDocument(devices).toJson(QJsonDocument::Compact) << Qt::endl;
    return 0;
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  if (!parser.isSet(serverOpt) && !parser.isSet(clientOpt)) {
    if (parser.isSet(settingsOpt)) {
      Settings::setSettingsFile(QFileInfo(parser.value(settingsOpt)).absoluteFilePath());
    }
    return runService(
        parser.value(logFileOpt), parser.value(parentPidOpt).toLongLong(),
        parser.isSet(trustedDbOpt) ? QFileInfo(parser.value(trustedDbOpt)).absoluteFilePath() : QString(),
        parser.value(statusLogOpt).toDouble()
    );
  }
  if (parser.isSet(serverOpt) && parser.isSet(clientOpt)) {
    return fail(QStringLiteral("choose one of --server and --client <host>"));
  }

  PeerConfig config;
  config.role = parser.isSet(serverOpt) ? PeerConfig::Role::Server : PeerConfig::Role::Client;
  config.host = parser.value(clientOpt);
  config.port = parser.value(portOpt).toUShort();
  config.name = parser.value(nameOpt);

  // Deskflow's settings are only touched for defaults the caller did not give.
  config.certificatePath =
      parser.isSet(certOpt) ? parser.value(certOpt) : Settings::value(Settings::Security::Certificate).toString();
  if (parser.isSet(generateCertOpt) && !QFileInfo::exists(config.certificatePath)) {
    deskflow::generatePemSelfSignedCert(config.certificatePath);
    say(QStringLiteral("generated certificate %1").arg(config.certificatePath));
  }

  for (const auto &hex : parser.values(trustOpt)) {
    auto digest = QByteArray::fromHex(hex.toLatin1().replace(':', ""));
    if (digest.size() != 32) {
      return fail(QStringLiteral("not a SHA-256 fingerprint: %1").arg(hex));
    }
    config.trustedSha256.append(digest);
  }
  if (parser.isSet(trustSelfOpt)) {
    QString error;
    const auto digest = certificateSha256(config.certificatePath, &error);
    if (digest.isEmpty()) {
      return fail(error);
    }
    config.trustedSha256.append(digest);
  }
  if (parser.isSet(trustedDbOpt)) {
    config.trustedDbPath = parser.value(trustedDbOpt);
  } else if (config.trustedSha256.isEmpty()) {
    config.trustedDbPath = config.role == PeerConfig::Role::Server ? Settings::tlsTrustedClientsDb()
                                                                   : Settings::tlsTrustedServersDb();
  }

  std::set<StreamKind> sending;
  for (const auto &name : parser.values(sendOpt)) {
    const auto kind = protocol::kindFromString(name);
    if (!kind) {
      return fail(QStringLiteral("unknown stream kind: %1").arg(name));
    }
    sending.insert(*kind);
  }

  const double loss = parser.value(lossOpt).toDouble();
  const double delay = parser.value(delayOpt).toDouble();
  const double jitter = parser.value(jitterOpt).toDouble();
  std::unique_ptr<test::NetworkSimulator> network;
  if (loss > 0 || delay > 0 || jitter > 0) {
    network = std::make_unique<test::NetworkSimulator>(loss, delay, jitter);
    config.wrapTransmit = [simulator = network.get()](SendStream::Transmit inner) {
      return simulator->wrap(std::move(inner));
    };
    say(QStringLiteral("outgoing network: %1% loss, %2 ms delay, up to %3 ms jitter").arg(loss).arg(delay).arg(jitter));
  }

  AudioPeer peer(config);
  QObject::connect(&peer, &AudioPeer::logMessage, &app, [](const QString &line) { say(line); });
  std::set<StreamKind> wanted;
  for (const auto &name : parser.values(wantOpt)) {
    const auto kind = protocol::kindFromString(name);
    if (!kind) {
      return fail(QStringLiteral("unknown stream kind: %1").arg(name));
    }
    wanted.insert(*kind);
  }
  QObject::connect(&peer, &AudioPeer::connected, &app, [&peer, wanted] {
    for (const auto kind : wanted) {
      peer.sendDemand(kind, true);
    }
  });
  for (auto kind : sending) {
    peer.setSending(kind, true);
  }
  QString error;
  if (!peer.start(&error)) {
    return fail(error);
  }

  // Declared after the peer so they stop before it goes away.
  std::vector<std::unique_ptr<test::ToneSource>> sources;
  for (auto kind : sending) {
    sources.push_back(std::make_unique<test::ToneSource>(
        peer, kind, toneHz(kind), parser.value(amplitudeOpt).toDouble(), parser.value(sourcePpmOpt).toDouble()
    ));
    sources.back()->start();
  }
  std::map<StreamKind, std::unique_ptr<test::VirtualSink>> sinks;
  for (auto kind : kKinds) {
    auto sink = std::make_unique<test::VirtualSink>(
        peer, kind, parser.value(sinkPpmOpt).toDouble(), parser.value(periodOpt).toInt(), toneHz(kind), parser.value(amplitudeOpt).toDouble()
    );
    if (parser.isSet(recordOpt)) {
      sink->recordTo(
          QStringLiteral("%1-%2.wav").arg(parser.value(recordOpt), kindName(kind)),
          parser.value(recordSecondsOpt).toDouble()
      );
    }
    sink->start();
    sinks[kind] = std::move(sink);
  }

  QFile jsonl;
  if (parser.isSet(jsonlOpt)) {
    jsonl.setFileName(parser.value(jsonlOpt));
    if (!jsonl.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
      return fail(QStringLiteral("cannot write %1").arg(jsonl.fileName()));
    }
  }

  QElapsedTimer elapsed;
  elapsed.start();
  std::map<StreamKind, test::VirtualSink::Analysis> lastAnalysis;

  const auto sample = [&] {
    QJsonObject snapshot{{"t", elapsed.elapsed() / 1000.0}, {"connected", peer.isConnected()},
                         {"rttMs", peer.roundTripMs()}};
    QJsonObject received;
    QJsonObject sent;
    for (auto kind : kKinds) {
      const auto analysis = sinks[kind]->analysis();
      lastAnalysis[kind] = analysis;
      if (auto stream = peer.receiveStream(kind)) {
        const auto s = stream->stats();
        auto entry = toJson(s);
        entry.insert(QStringLiteral("output"), toJson(analysis));
        received.insert(kindName(kind), entry);
        say(QString::asprintf(
            "recv %-10s buf %5.1f/%5.1f ms  jitter %4.1f ms  speed %+7.1f ppm  pkts %llu  plc %llu  late "
            "%llu  drop %llu  underrun %llu  glitch %llu  dropout %.0f ms  tone %.1f Hz  latency %.1f ms",
            toString(kind), s.bufferMs, s.targetMs, s.jitterMs, s.ratioPpm, (unsigned long long)s.packets,
            (unsigned long long)s.concealed, (unsigned long long)s.late,
            (unsigned long long)s.dropped, (unsigned long long)s.underruns, (unsigned long long)analysis.glitches,
            analysis.dropoutFrames * 1000.0 / kSampleRate, analysis.toneHz, peer.latencyMs(kind).value_or(-1)
        ));
      }
      if (auto stream = peer.sendStream(kind)) {
        const auto s = stream->stats();
        auto entry = toJson(s);
        const auto remote = peer.remoteStats(kind);
        if (!remote.isEmpty()) {
          entry.insert(QStringLiteral("peerReports"), remote);
        }
        sent.insert(kindName(kind), entry);
        say(QString::asprintf(
            "send %-10s pkts %llu  %.0f kbit/s avg  silent %llu  paused %d  rtt %.1f ms", toString(kind),
            (unsigned long long)s.packets, elapsed.elapsed() ? s.bytes * 8.0 / elapsed.elapsed() : 0.0,
            (unsigned long long)s.suppressedFrames, int(s.paused), peer.roundTripMs()
        ));
      }
    }
    snapshot.insert(QStringLiteral("received"), received);
    snapshot.insert(QStringLiteral("sent"), sent);
    if (jsonl.isOpen()) {
      jsonl.write(QJsonDocument(snapshot).toJson(QJsonDocument::Compact) + '\n');
      jsonl.flush();
    }
    return snapshot;
  };

  QTimer statsTimer;
  QObject::connect(&statsTimer, &QTimer::timeout, &app, [&] { sample(); });
  statsTimer.start(int(parser.value(statsOpt).toDouble() * 1000));

  QTimer interruptTimer;
  QObject::connect(&interruptTimer, &QTimer::timeout, &app, [] {
    if (g_interrupted) {
      QCoreApplication::quit();
    }
  });
  interruptTimer.start(100);

  if (const double duration = parser.value(durationOpt).toDouble(); duration > 0) {
    QTimer::singleShot(int(duration * 1000), &app, &QCoreApplication::quit);
  }

  const int result = QCoreApplication::exec();

  auto summary = sample();
  for (auto &source : sources) {
    source->stop();
  }
  for (auto &[kind, sink] : sinks) {
    sink->stop();
  }
  const auto counters = peer.datagramCounters();
  summary.insert(
      QStringLiteral("datagrams"),
      QJsonObject{
          {"received", double(counters.received)},
          {"unknownStream", double(counters.unknownStream)},
          {"replayed", double(counters.replayed)},
          {"forged", double(counters.forged)},
      }
  );
  if (network) {
    summary.insert(QStringLiteral("simulatedDrops"), double(network->dropped()));
  }
  peer.stop();
  QTextStream(stdout) << "SUMMARY " << QJsonDocument(summary).toJson(QJsonDocument::Compact) << Qt::endl;
  return result;
}

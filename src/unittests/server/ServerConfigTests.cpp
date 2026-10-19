/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2014 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerConfigTests.h"

#include "common/LayoutLinks.h"
#include "common/Settings.h"
#include "server/Config.h"

#include <QDir>
#include <QFile>

#include <sstream>

class OnlySystemFilter : public InputFilter::Condition
{
public:
  Condition *clone() const override
  {
    return new OnlySystemFilter();
  }
  std::string format() const override
  {
    return "";
  }

  InputFilter::FilterStatus match(const Event &ev) override
  {
    return ev.getType() == EventTypes::System ? InputFilter::FilterStatus::Activate
                                              : InputFilter::FilterStatus::NoMatch;
  }
};

using namespace deskflow::server;

namespace {

void placeComputer(const QString &name, const QRect &geometry)
{
  Settings::setValue(Settings::Computer::Name.arg(name), name);
  Settings::setValue(Settings::Computer::Geometry.arg(name), geometry);
}

} // namespace

void ServerConfigTests::initTestCase()
{
  QVERIFY(QDir().mkpath(m_settingsPath));
  QFile::remove(m_settingsFile);
  Settings::setSettingsFile(m_settingsFile);
  Settings::setStateFile(m_stateFile);
}

void ServerConfigTests::equalityCheck()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addComputer("computerA"));
  QVERIFY(a != b);

  QVERIFY(b.addComputer("computerB"));
  QVERIFY(a != b);

  QVERIFY(a.addComputer("computerB"));
  QVERIFY(a.addComputer("computerC"));
  QVERIFY(a.connect("computerA", Direction::Bottom, 0.0f, 0.5f, "computerB", 0.5f, 1.0f));
  QVERIFY(a.connect("computerB", Direction::Left, 0.0f, 0.5f, "computerB", 0.5f, 1.0f));
  QVERIFY(b.addComputer("computerA"));
  QVERIFY(b.addComputer("computerC"));
  QVERIFY(b.connect("computerA", Direction::Bottom, 0.0f, 0.5f, "computerB", 0.5f, 1.0f));
  QVERIFY(b.connect("computerB", Direction::Left, 0.0f, 0.5f, "computerB", 0.5f, 1.0f));
  QVERIFY(a.addOption("computerA", kOptionClipboardSharing, 1));
  QVERIFY(b.addOption("computerA", kOptionClipboardSharing, 1));
  QVERIFY(a.addOption(std::string(), kOptionClipboardSharing, 1));
  QVERIFY(b.addOption(std::string(), kOptionClipboardSharing, 1));

  a.getInputFilter()->addFilterRule(InputFilter::Rule{new OnlySystemFilter()});
  b.getInputFilter()->addFilterRule(InputFilter::Rule{new OnlySystemFilter()});
  QVERIFY(a.addAlias("computerA", "aliasA"));
  QVERIFY(b.addAlias("computerA", "aliasA"));
  /* TODO Fix linking to the proper libs
  NetworkAddress addr1("localhost", 8080);
  addr1.resolve();
  NetworkAddress addr2("localhost", 8080);
  addr2.resolve();
  a.setDeskflowAddress(addr1);
  b.setDeskflowAddress(addr2);
  */
  QVERIFY(a == b);
}

void ServerConfigTests::equalityCheck_diff_options()
{
  Config a(nullptr);
  Config b(nullptr);

  QVERIFY(a.addComputer("computerA"));
  QVERIFY(b.addComputer("computerA"));
  QVERIFY(a.addOption("computerA", kOptionClipboardSharing, 0));
  QVERIFY(b.addOption("computerA", kOptionClipboardSharing, 1));
  QVERIFY(a != b);
}

void ServerConfigTests::equalityCheck_diff_alias()
{
  Config a(nullptr);
  Config b(nullptr);

  QVERIFY(a.addComputer("computerA"));
  QVERIFY(b.addComputer("computerA"));
  QVERIFY(b.addAlias("computerA", "aliasA"));
  QVERIFY(a != b);

  QVERIFY(a.addAlias("computerA", "aliasA"));
  QVERIFY(b.addAlias("computerA", "aliasB"));
  QVERIFY(a != b);
}

void ServerConfigTests::equalityCheck_diff_filters()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addComputer("computerA"));
  QVERIFY(b.addComputer("computerA"));

  a.getInputFilter()->addFilterRule(InputFilter::Rule{new OnlySystemFilter()});
  QVERIFY(a != b);
}

// TODO FIX
/*
void ServerConfigTests::equalityCheck_diff_address()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addComputer("computerA"));
  QVERIFY(b.addComputer("computerA"));
  a.setDeskflowAddress(NetworkAddress(8000));
  b.setDeskflowAddress(NetworkAddress(9000));
  QVERIFY(a != b);
}
*/

void ServerConfigTests::equalityCheck_diff_neighbours1()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addComputer("computerA"));
  QVERIFY(a.addComputer("computerB"));
  QVERIFY(a.connect("computerA", Direction::Bottom, 0.0f, 0.5f, "computerB", 0.5f, 1.0f));
  QVERIFY(b.addComputer("computerA"));
  QVERIFY(b.addComputer("computerB"));
  QVERIFY(a != b);
  QVERIFY(b != a);
}

void ServerConfigTests::equalityCheck_diff_neighbours2()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addComputer("computerA"));
  QVERIFY(a.addComputer("computerB"));
  QVERIFY(a.connect("computerA", Direction::Bottom, 0.0f, 0.5f, "computerB", 0.5f, 1.0f));
  QVERIFY(b.addComputer("computerA"));
  QVERIFY(b.addComputer("computerB"));
  QVERIFY(b.connect("computerA", Direction::Bottom, 0.0f, 0.25f, "computerB", 0.25f, 1.0f));
  QVERIFY(a != b);
}

void ServerConfigTests::equalityCheck_diff_neighbours3()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addComputer("computerA"));
  QVERIFY(a.addComputer("computerB"));
  QVERIFY(a.addComputer("computerC"));
  QVERIFY(a.connect("computerA", Direction::Bottom, 0.0f, 0.5f, "computerB", 0.5f, 1.0f));
  QVERIFY(b.addComputer("computerA"));
  QVERIFY(b.addComputer("computerB"));
  QVERIFY(b.addComputer("computerC"));
  QVERIFY(b.connect("computerA", Direction::Bottom, 0.0f, 0.5f, "computerC", 0.5f, 1.0f));
  QVERIFY(a != b);
}

void ServerConfigTests::holeLink_addAndGet_roundTrips()
{
  Config config(nullptr);
  QVERIFY(config.addComputer("server"));
  QVERIFY(config.addComputer("client1"));
  QVERIFY(config.addComputer("client2"));

  QVERIFY(config.addHoleLink("server", "bottom-left", "client1"));
  QVERIFY(config.addHoleLink("server", "top-right", "client2"));

  const auto links = config.getHoleLinks("server");
  QCOMPARE(links.size(), 2);
  QCOMPARE(links[0].location, "bottom-left");
  QCOMPARE(links[0].dstName, "client1");
  QCOMPARE(links[1].location, "top-right");
  QCOMPARE(links[1].dstName, "client2");

  // a computer with no hole links registered should report none
  QVERIFY(config.getHoleLinks("client1").empty());
}

void ServerConfigTests::holeLink_unknownComputer_fails()
{
  Config config(nullptr);
  QVERIFY(config.addComputer("server"));

  QVERIFY(!config.addHoleLink("server", "bottom-left", "noSuchComputer"));
  QVERIFY(!config.addHoleLink("noSuchComputer", "bottom-left", "server"));
  QVERIFY(config.getHoleLinks("server").empty());
}

void ServerConfigTests::holeLink_doesNotCreateNeighbor()
{
  Config config(nullptr);
  QVERIFY(config.addComputer("server"));
  QVERIFY(config.addComputer("client1"));
  QVERIFY(config.addHoleLink("server", "bottom-left", "client1"));

  // a hole link is only a pending request -- it must not be usable as a
  // neighbor until the server resolves it against the hole's real position
  QVERIFY(!config.hasNeighbor("server", Direction::Left));
  QVERIFY(!config.hasNeighbor("server", Direction::Bottom));
}

void ServerConfigTests::equalityCheck_diff_holeLinks()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addComputer("computerA"));
  QVERIFY(a.addComputer("computerB"));
  QVERIFY(a.addHoleLink("computerA", "bottom-left", "computerB"));
  QVERIFY(b.addComputer("computerA"));
  QVERIFY(b.addComputer("computerB"));
  QVERIFY(a != b);

  QVERIFY(b.addHoleLink("computerA", "bottom-left", "computerB"));
  QVERIFY(a == b);
}

void ServerConfigTests::restartServerOnZoneChange_defaultsFalse()
{
  // the full "section: options" text-parsing path (Config::read() /
  // operator>>) isn't exercised anywhere else in this suite: it
  // unconditionally constructs a NetworkAddress, which asserts unless the
  // global Arch singleton has been initialized by main() first -- not
  // something a unit test should bootstrap just for this. The actual
  // "restartServerOnZoneChange = ..." line parsing added to
  // readSectionOptions() is exercised manually instead.
  Config config(nullptr);
  QVERIFY(!config.restartServerOnZoneChange());
}

void ServerConfigTests::read_touchingGeometry_linked()
{
  Settings::setValue(Settings::Server::ExternalConfig, false);
  placeComputer("a", QRect(0, 0, 100, 100));
  placeComputer("b", QRect(100, 50, 100, 100));
  Config config(nullptr);
  std::istringstream empty;

  empty >> config;

  QCOMPARE(config.getNeighbor("a", Direction::Right, 0.75f, nullptr), "b");
  QCOMPARE(config.getNeighbor("a", Direction::Right, 0.25f, nullptr), "");
}

void ServerConfigTests::read_serverMonitors_linkedOnInnerEdges()
{
  // Server monitors in an L leaving the top left corner empty, relative to the server's top left
  // (2548,-1080 and so on in the desktop), with the client tucked into that corner.
  Settings::setValue(Settings::Server::ExternalConfig, false);
  placeComputer("lserver", QRect(0, 0, 5120, 2520));
  Settings::setValue(
      Settings::Computer::Monitors.arg("lserver"),
      deskflow::layout::rectsToVariant({QRect(0, 1080, 2560, 1440), QRect(2560, 1080, 2560, 1080), QRect(2548, 0, 1920, 1080)})
  );
  placeComputer("lclient", QRect(-12, -360, 2560, 1440));
  Config config(nullptr);
  std::istringstream empty;

  empty >> config;

  // the top monitor's left edge spans the top 1080 of the server's 2520 pixels
  QCOMPARE(config.getNeighbor("lserver", Direction::Left, 0.2f, nullptr), "lclient");
  QCOMPARE(config.getNeighbor("lserver", Direction::Left, 0.6f, nullptr), "");
  // the bottom left monitor's top edge spans the left 2548 of the server's 5120 pixels
  QCOMPARE(config.getNeighbor("lserver", Direction::Top, 0.25f, nullptr), "lclient");
  QCOMPARE(config.getNeighbor("lserver", Direction::Top, 0.75f, nullptr), "");
  // and back: the bottom 3/4 of the client's right edge faces the top monitor
  float position = 0;
  QCOMPARE(config.getNeighbor("lclient", Direction::Right, 0.2f, &position), "");
  QCOMPARE(config.getNeighbor("lclient", Direction::Right, 0.5f, &position), "lserver");
  // halfway down the client is 360 pixels below its top at -360, so the cursor enters the server at 360
  QCOMPARE(qRound(position * 2520), 360);
}

void ServerConfigTests::read_externalConfig_geometryIgnored()
{
  Settings::setValue(Settings::Server::ExternalConfig, true);
  placeComputer("a", QRect(0, 0, 100, 100));
  placeComputer("b", QRect(100, 0, 100, 100));
  Config config(nullptr);
  std::istringstream empty;

  empty >> config;

  QCOMPARE(config.getNeighbor("a", Direction::Right, 0.5f, nullptr), "");
}

QTEST_MAIN(ServerConfigTests)

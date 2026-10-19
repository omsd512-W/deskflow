/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "LayoutLinksTests.h"

#include "common/LayoutLinks.h"

using namespace deskflow::layout;

void LayoutLinksTests::computeLinks_sideBySideSameHeight_fullEdgeLinks()
{
  const QMap<QString, QRect> placed = {{"a", QRect(0, 0, 1920, 1080)}, {"b", QRect(1920, 0, 1920, 1080)}};

  const auto links = computeLinks(placed);

  QCOMPARE(links, QList<Link>({{"a", Side::Right, 0, 100, "b", 0, 100}, {"b", Side::Left, 0, 100, "a", 0, 100}}));
}

void LayoutLinksTests::computeLinks_halfOverlap_rangedLinks()
{
  const QMap<QString, QRect> placed = {{"a", QRect(0, 0, 1920, 1080)}, {"b", QRect(1920, 540, 1920, 1080)}};

  const auto links = computeLinks(placed);

  QCOMPARE(links, QList<Link>({{"a", Side::Right, 50, 100, "b", 0, 50}, {"b", Side::Left, 0, 50, "a", 50, 100}}));
}

void LayoutLinksTests::computeLinks_differentHeights_rangeOnTallerComputer()
{
  const QMap<QString, QRect> placed = {{"a", QRect(0, 0, 1000, 1000)}, {"b", QRect(1000, -500, 1000, 2000)}};

  const auto links = computeLinks(placed);

  QCOMPARE(links, QList<Link>({{"a", Side::Right, 0, 100, "b", 25, 75}, {"b", Side::Left, 25, 75, "a", 0, 100}}));
}

void LayoutLinksTests::computeLinks_twoNeighboursOnOneEdge_edgeSplit()
{
  const QMap<QString, QRect> placed = {
      {"a", QRect(0, 0, 100, 100)}, {"low", QRect(100, 50, 100, 50)}, {"high", QRect(100, 0, 100, 50)}
  };

  const auto links = computeLinks(placed);

  QCOMPARE(
      links, QList<Link>(
                 {{"a", Side::Right, 0, 50, "high", 0, 100},
                  {"a", Side::Right, 50, 100, "low", 0, 100},
                  {"high", Side::Left, 0, 100, "a", 0, 50},
                  {"high", Side::Down, 0, 100, "low", 0, 100},
                  {"low", Side::Left, 0, 100, "a", 50, 100},
                  {"low", Side::Up, 0, 100, "high", 0, 100}}
             )
  );
}

void LayoutLinksTests::computeLinks_stacked_upDownLinks()
{
  const QMap<QString, QRect> placed = {{"a", QRect(0, 100, 100, 100)}, {"b", QRect(0, 0, 100, 100)}};

  const auto links = computeLinks(placed);

  QCOMPARE(links, QList<Link>({{"a", Side::Up, 0, 100, "b", 0, 100}, {"b", Side::Down, 0, 100, "a", 0, 100}}));
}

void LayoutLinksTests::computeLinks_gapBetween_noLinks()
{
  const QMap<QString, QRect> placed = {{"a", QRect(0, 0, 100, 100)}, {"b", QRect(101, 0, 100, 100)}};

  const auto links = computeLinks(placed);

  QVERIFY(links.isEmpty());
}

void LayoutLinksTests::computeLinks_cornerTouch_noLinks()
{
  const QMap<QString, QRect> placed = {{"a", QRect(0, 0, 100, 100)}, {"b", QRect(100, 100, 100, 100)}};

  const auto links = computeLinks(placed);

  QVERIFY(links.isEmpty());
}

void LayoutLinksTests::snap_nearRightEdge_flushAndAligned()
{
  const QRect other(0, 0, 100, 100);

  const auto snapped = snap(QRect(105, 3, 100, 100), {other}, 10);

  QCOMPARE(snapped, QRect(100, 0, 100, 100));
}

void LayoutLinksTests::snap_nearTopEdge_flushAndAligned()
{
  const QRect other(0, 0, 100, 100);

  const auto snapped = snap(QRect(-7, -104, 100, 100), {other}, 10);

  QCOMPARE(snapped, QRect(0, -100, 100, 100));
}

void LayoutLinksTests::snap_farFromOthers_unchanged()
{
  const QRect moving(150, 30, 100, 100);

  const auto snapped = snap(moving, {QRect(0, 0, 100, 100)}, 10);

  QCOMPARE(snapped, moving);
}

void LayoutLinksTests::snapResize_nearNeighbourEdges_edgesMatched()
{
  const QRect other(200, 0, 100, 100);

  const auto snapped = snapResize(QRect(0, 0, 195, 104), {other}, 10);

  QCOMPARE(snapped, QRect(0, 0, 200, 100));
}

void LayoutLinksTests::snapResize_farFromOthers_unchanged()
{
  const QRect other(500, 500, 100, 100);

  const auto snapped = snapResize(QRect(0, 0, 195, 104), {other}, 10);

  QCOMPARE(snapped, QRect(0, 0, 195, 104));
}

void LayoutLinksTests::computeLinks_clientInCornerOfLShapedServer_linksOnInnerEdges()
{
  // Three server monitors in an L leaving the top left corner empty, with a bigger client tucked into it:
  // the client's right edge meets the top monitor's left edge and its bottom meets the bottom left monitor.
  const QMap<QString, QList<QRect>> placed = {
      {"server", {QRect(0, 0, 2560, 1440), QRect(2560, 0, 2560, 1080), QRect(2548, -1080, 1920, 1080)}},
      {"client", {QRect(-12, -1440, 2560, 1440)}},
  };

  const auto links = computeLinks(placed);

  // The server's ranges are of its whole 5120x2520 shape, starting at -1080.
  const double leftEnd = 1080 * 100.0 / 2520;
  const double upEnd = 2548 * 100.0 / 5120;
  const double clientUpStart = 12 * 100.0 / 2560;
  QCOMPARE(
      links, QList<Link>(
                 {{"client", Side::Right, 25, 100, "server", 0, leftEnd},
                  {"client", Side::Down, clientUpStart, 100, "server", 0, upEnd},
                  {"server", Side::Left, 0, leftEnd, "client", 25, 100},
                  {"server", Side::Up, 0, upEnd, "client", clientUpStart, 100}}
             )
  );
}

void LayoutLinksTests::computeLinks_monitorsOfOneComputer_notLinkedToEachOther()
{
  const QMap<QString, QList<QRect>> placed = {{"server", {QRect(0, 0, 100, 100), QRect(100, 0, 100, 100)}}};

  const auto links = computeLinks(placed);

  QVERIFY(links.isEmpty());
}

void LayoutLinksTests::computeLinks_clientBelowTwoMonitors_oneLinkPerMonitor()
{
  const QMap<QString, QList<QRect>> placed = {
      {"client", {QRect(50, 100, 100, 100)}},
      {"server", {QRect(0, 0, 100, 100), QRect(100, 0, 100, 100)}},
  };

  const auto links = computeLinks(placed);

  QCOMPARE(
      links, QList<Link>(
                 {{"client", Side::Up, 0, 50, "server", 25, 50},
                  {"client", Side::Up, 50, 100, "server", 50, 75},
                  {"server", Side::Down, 25, 50, "client", 0, 50},
                  {"server", Side::Down, 50, 75, "client", 50, 100}}
             )
  );
}

void LayoutLinksTests::boundingRect_unitesAll()
{
  const auto bounds = boundingRect({QRect(0, 0, 100, 100), QRect(100, -50, 50, 50)});

  QCOMPARE(bounds, QRect(0, -50, 150, 150));
}

void LayoutLinksTests::snapGroup_nearEdge_offsetOfClosestRect()
{
  // Only the lower of two stacked rects faces the other one, so the group moves by its snap.
  const QList<QRect> moving = {QRect(0, 0, 100, 100), QRect(0, 100, 100, 100)};

  const auto offset = snapGroup(moving, {QRect(107, 150, 100, 100)}, 10);

  QCOMPARE(offset, QPoint(7, 0));
}

void LayoutLinksTests::snapGroup_farFromOthers_null()
{
  const auto offset = snapGroup({QRect(0, 0, 100, 100)}, {QRect(500, 0, 100, 100)}, 10);

  QVERIFY(offset.isNull());
}

void LayoutLinksTests::snapGroup_nearCornerOfLShape_tucksInWithoutOverlap()
{
  // The empty top left corner of an L is 2548x1080, smaller than the client. Lining the client up with the
  // bottom left monitor's right edge (8 away) would push it 12 pixels into the top monitor, so it has to snap
  // flush against the top monitor (20 away) instead, and onto the bottom left monitor's top edge.
  const QList<QRect> monitors = {QRect(0, 1080, 2560, 1440), QRect(2560, 1080, 2560, 1080), QRect(2548, 0, 1920, 1080)};
  const QRect client(8, -345, 2560, 1440);

  const auto offset = snapGroup({client}, monitors, 50);

  QCOMPARE(offset, QPoint(-20, -15));
  QCOMPARE(client.translated(offset), QRect(-12, -360, 2560, 1440));
}

void LayoutLinksTests::overlapsAny_intersecting_true()
{
  const QRect other(0, 0, 100, 100);

  const bool overlaps = overlapsAny(QRect(50, 50, 100, 100), {other});

  QVERIFY(overlaps);
}

void LayoutLinksTests::overlapsAny_sharedEdge_false()
{
  const QRect other(0, 0, 100, 100);

  const bool overlaps = overlapsAny(QRect(100, 0, 100, 100), {other});

  QVERIFY(!overlaps);
}

QTEST_MAIN(LayoutLinksTests)

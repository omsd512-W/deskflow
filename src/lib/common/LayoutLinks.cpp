/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "LayoutLinks.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>

namespace deskflow::layout {

namespace {

// QRect::right() and bottom() are inclusive, edges here are exclusive.
int rightEdge(const QRect &r)
{
  return r.x() + r.width();
}

int bottomEdge(const QRect &r)
{
  return r.y() + r.height();
}

double percent(int value, int start, int length)
{
  return (value - start) * 100.0 / length;
}

// ra and rb are the touching rectangles (e.g. one monitor each), while the ranges are percentages of the
// computers' bounding rectangles boundsA and boundsB, which is the shape the server switches on.
bool linkOnSide(
    const QString &a, const QRect &ra, const QRect &boundsA, const QString &b, const QRect &rb, const QRect &boundsB,
    Side side, Link &link
)
{
  const bool horizontal = side == Side::Right || side == Side::Left;

  bool touching = false;
  switch (side) {
  case Side::Right:
    touching = rightEdge(ra) == rb.x();
    break;
  case Side::Left:
    touching = ra.x() == rightEdge(rb);
    break;
  case Side::Up:
    touching = ra.y() == bottomEdge(rb);
    break;
  case Side::Down:
    touching = bottomEdge(ra) == rb.y();
    break;
  }
  if (!touching)
    return false;

  const int aStart = horizontal ? ra.y() : ra.x();
  const int aLength = horizontal ? ra.height() : ra.width();
  const int bStart = horizontal ? rb.y() : rb.x();
  const int bLength = horizontal ? rb.height() : rb.width();
  const int overlapStart = std::max(aStart, bStart);
  const int overlapEnd = std::min(aStart + aLength, bStart + bLength);
  if (overlapEnd <= overlapStart)
    return false;

  const int boundsAStart = horizontal ? boundsA.y() : boundsA.x();
  const int boundsALength = horizontal ? boundsA.height() : boundsA.width();
  const int boundsBStart = horizontal ? boundsB.y() : boundsB.x();
  const int boundsBLength = horizontal ? boundsB.height() : boundsB.width();
  link = {
      a,
      side,
      percent(overlapStart, boundsAStart, boundsALength),
      percent(overlapEnd, boundsAStart, boundsALength),
      b,
      percent(overlapStart, boundsBStart, boundsBLength),
      percent(overlapEnd, boundsBStart, boundsBLength)
  };
  return true;
}

} // namespace

QList<Link> computeLinks(const QMap<QString, QRect> &computers)
{
  QMap<QString, QList<QRect>> single;
  for (const auto &[name, rect] : computers.asKeyValueRange())
    single.insert(name, {rect});
  return computeLinks(single);
}

QList<Link> computeLinks(const QMap<QString, QList<QRect>> &computers)
{
  QMap<QString, QRect> bounds;
  for (const auto &[name, rects] : computers.asKeyValueRange())
    bounds.insert(name, boundingRect(rects));

  QList<Link> links;
  for (const auto &[from, fromRects] : computers.asKeyValueRange()) {
    for (const auto side : {Side::Right, Side::Left, Side::Up, Side::Down}) {
      QList<Link> sideLinks;
      for (const auto &[to, toRects] : computers.asKeyValueRange()) {
        if (from == to)
          continue;
        for (const auto &fromRect : fromRects) {
          for (const auto &toRect : toRects) {
            Link link;
            if (linkOnSide(from, fromRect, bounds.value(from), to, toRect, bounds.value(to), side, link))
              sideLinks.append(link);
          }
        }
      }
      std::ranges::sort(sideLinks, {}, &Link::fromStart);
      links.append(sideLinks);
    }
  }
  return links;
}

QRect boundingRect(const QList<QRect> &rects)
{
  QRect result;
  for (const auto &rect : rects)
    result = result.united(rect);
  return result;
}

QPoint snapGroup(const QList<QRect> &moving, const QList<QRect> &others, int threshold)
{
  // Each axis snaps on its own, flush against or lined up with an edge of a rect that is beside the moving one
  // on the other axis, so a rect can tuck into a corner between two others.
  const auto beside = [threshold](int aStart, int aEnd, int bStart, int bEnd) {
    return aStart < bEnd + threshold && bStart < aEnd + threshold;
  };
  QList<int> xs;
  QList<int> ys;
  const auto add = [threshold](QList<int> &deltas, std::initializer_list<int> candidates) {
    for (const int delta : candidates) {
      if (std::abs(delta) <= threshold && !deltas.contains(delta))
        deltas.append(delta);
    }
  };
  for (const auto &m : moving) {
    for (const auto &o : others) {
      if (beside(m.y(), bottomEdge(m), o.y(), bottomEdge(o)))
        add(xs, {o.x() - rightEdge(m), rightEdge(o) - m.x(), o.x() - m.x(), rightEdge(o) - rightEdge(m)});
      if (beside(m.x(), rightEdge(m), o.x(), rightEdge(o)))
        add(ys, {o.y() - bottomEdge(m), bottomEdge(o) - m.y(), o.y() - m.y(), bottomEdge(o) - bottomEdge(m)});
    }
  }

  // Prefer snapping both axes, then one, closest first, and skip any offset that would overlap the others:
  // lining up with the nearest edge can push the group into a neighbour on the other side of a corner.
  const auto clear = [&moving, &others](const QPoint &offset) {
    return std::ranges::none_of(moving, [&](const QRect &m) { return overlapsAny(m.translated(offset), others); });
  };
  QList<QPoint> both;
  for (const int dx : xs) {
    for (const int dy : ys)
      both.append(QPoint(dx, dy));
  }
  QList<QPoint> one;
  for (const int dx : xs)
    one.append(QPoint(dx, 0));
  for (const int dy : ys)
    one.append(QPoint(0, dy));
  for (auto *candidates : {&both, &one}) {
    std::ranges::stable_sort(*candidates, {}, &QPoint::manhattanLength);
    for (const auto &offset : *candidates) {
      if (clear(offset))
        return offset;
    }
  }
  return {};
}

bool overlapsAny(const QList<QRect> &rects, const QList<QRect> &others)
{
  return std::ranges::any_of(rects, [&others](const QRect &rect) { return overlapsAny(rect, others); });
}

QList<QRect> placedRects(const QRect &geometry, const QList<QRect> &monitors)
{
  if (monitors.isEmpty())
    return {geometry};

  QList<QRect> result;
  for (const auto &monitor : monitors)
    result.append(monitor.translated(geometry.topLeft()));
  return result;
}

QList<QRect> rectsFromVariant(const QVariant &value)
{
  QList<QRect> result;
  for (const auto &item : value.toList()) {
    if (const auto rect = item.toRect(); rect.isValid())
      result.append(rect);
  }
  return result;
}

QVariant rectsToVariant(const QList<QRect> &rects)
{
  QVariantList result;
  for (const auto &rect : rects)
    result.append(rect);
  return result;
}

QRect snap(const QRect &moving, const QList<QRect> &others, int threshold)
{
  int bestDistance = threshold + 1;
  QPoint bestOffset;
  const QRect *neighbour = nullptr;
  bool neighbourIsHorizontal = false;

  for (const auto &other : others) {
    const bool facingHorizontally =
        moving.y() < bottomEdge(other) + threshold && other.y() < bottomEdge(moving) + threshold;
    const bool facingVertically =
        moving.x() < rightEdge(other) + threshold && other.x() < rightEdge(moving) + threshold;

    const auto consider = [&](int delta, bool horizontal) {
      if (std::abs(delta) < bestDistance) {
        bestDistance = std::abs(delta);
        bestOffset = horizontal ? QPoint(delta, 0) : QPoint(0, delta);
        neighbour = &other;
        neighbourIsHorizontal = horizontal;
      }
    };
    if (facingHorizontally) {
      consider(rightEdge(other) - moving.x(), true);
      consider(other.x() - rightEdge(moving), true);
    }
    if (facingVertically) {
      consider(bottomEdge(other) - moving.y(), false);
      consider(other.y() - bottomEdge(moving), false);
    }
  }

  if (neighbour == nullptr)
    return moving;

  QRect result = moving.translated(bestOffset);

  const int startDelta = neighbourIsHorizontal ? neighbour->y() - result.y() : neighbour->x() - result.x();
  const int endDelta =
      neighbourIsHorizontal ? bottomEdge(*neighbour) - bottomEdge(result) : rightEdge(*neighbour) - rightEdge(result);
  const int align = std::abs(startDelta) <= std::abs(endDelta) ? startDelta : endDelta;
  if (std::abs(align) <= threshold)
    result.translate(neighbourIsHorizontal ? QPoint(0, align) : QPoint(align, 0));

  return result;
}

QRect snapResize(const QRect &resized, const QList<QRect> &others, int threshold)
{
  int right = rightEdge(resized);
  int bottom = bottomEdge(resized);
  int bestX = threshold + 1;
  int bestY = threshold + 1;
  for (const auto &other : others) {
    for (const int x : {other.x(), rightEdge(other)}) {
      if (x > resized.x() && std::abs(x - rightEdge(resized)) < bestX) {
        bestX = std::abs(x - rightEdge(resized));
        right = x;
      }
    }
    for (const int y : {other.y(), bottomEdge(other)}) {
      if (y > resized.y() && std::abs(y - bottomEdge(resized)) < bestY) {
        bestY = std::abs(y - bottomEdge(resized));
        bottom = y;
      }
    }
  }
  return QRect(resized.topLeft(), QSize(right - resized.x(), bottom - resized.y()));
}

bool overlapsAny(const QRect &rect, const QList<QRect> &others)
{
  return std::ranges::any_of(others, [&rect](const QRect &other) { return rect.intersects(other); });
}

} // namespace deskflow::layout

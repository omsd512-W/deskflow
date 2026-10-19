/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QRect>
#include <QString>

namespace deskflow::layout {
Q_NAMESPACE

// Declared in the order computeLinks returns the links of each computer.
enum class Side
{
  Right,
  Left,
  Up,
  Down
};
Q_ENUM_NS(Side)

/**
 * @brief A link from one edge range of a computer to an edge range of another.
 * Ranges are percentages (0 to 100) of the respective computer's edge.
 */
struct Link
{
  QString from;
  Side side;
  double fromStart;
  double fromEnd;
  QString to;
  double toStart;
  double toEnd;

  bool operator==(const Link &) const = default;
};

/**
 * @brief Computes links between computers whose rectangles share an edge.
 * Computers that only touch at a corner or are separated by a gap are not linked.
 */
QList<Link> computeLinks(const QMap<QString, QRect> &computers);

/**
 * @brief Computes links between computers made of one or more rectangles, e.g. one per monitor.
 * Each rectangle of a computer that shares an edge with a rectangle of another computer gives a link over
 * the shared part of that edge. Ranges are percentages of each computer's bounding rectangle, which is the
 * shape the server switches on, so a link can sit on an inner edge of an L-shaped arrangement of monitors.
 */
QList<Link> computeLinks(const QMap<QString, QList<QRect>> &computers);

/**
 * @brief Smallest rectangle containing all of @p rects, invalid if there are none.
 */
QRect boundingRect(const QList<QRect> &rects);

/**
 * @brief Offset that snaps a group of rects (e.g. a computer's monitors) against @p others: each axis moves flush
 * against or lines up with the nearest edge within @p threshold, so the group can tuck into a corner between two
 * rects. Offsets that would overlap @p others are skipped. Null when nothing fits within @p threshold.
 */
QPoint snapGroup(const QList<QRect> &moving, const QList<QRect> &others, int threshold);

/**
 * @brief Moves @p moving flush against the nearest edge of @p others within @p threshold,
 * then aligns it with that neighbour's parallel edges if they are also within @p threshold.
 */
QRect snap(const QRect &moving, const QList<QRect> &others, int threshold);

/**
 * @brief Moves the right and bottom edges of @p resized onto the nearest parallel edge of @p others
 * within @p threshold, keeping its top left corner.
 */
QRect snapResize(const QRect &resized, const QList<QRect> &others, int threshold);

bool overlapsAny(const QRect &rect, const QList<QRect> &others);
bool overlapsAny(const QList<QRect> &rects, const QList<QRect> &others);

} // namespace deskflow::layout

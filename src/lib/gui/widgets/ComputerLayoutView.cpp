/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ComputerLayoutView.h"

#include "common/LayoutLinks.h"
#include "dialogs/ComputerSettingsDialog.h"
#include "gui/config/ComputerList.h"

#include <QDrag>
#include <QDragEnterEvent>
#include <QGraphicsRectItem>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainterPath>
#include <QPainter>
#include <QStyleOptionGraphicsItem>

using namespace deskflow::layout;

namespace {
// Distance in view pixels within which a dragged computer snaps to a neighbour.
constexpr int kSnapPixels = 12;
constexpr int kMarginPixels = 16;
// Size in view pixels of the bottom right corner that resizes a computer.
constexpr int kHandlePixels = 14;
constexpr QSize kMinimumSize{320, 180};
} // namespace

class ComputerItem : public QGraphicsRectItem
{
public:
  ComputerItem(int index, const Computer &computer)
      : QGraphicsRectItem(QRectF(QPointF(), computer.geometry().size())),
        m_index(index),
        m_name(computer.name()),
        m_pixmap(computer.pixmap()),
        m_isServer(computer.isServer()),
        m_monitors(computer.monitors())
  {
    setPos(computer.geometry().topLeft());
    setFlags(ItemIsMovable | ItemIsSelectable);
    setCursor(Qt::OpenHandCursor);
    const auto sizeHint = hasMonitors() ? QObject::tr("<br>Its monitors are arranged in the system display settings")
                                        : QObject::tr(
                                              "<br>Drag the bottom right corner to resize, or set the size in pixels "
                                              "in its settings"
                                          );
    setToolTip(
        QObject::tr(
            "<center>Computer: <b>%1</b></center>"
            "<br>Drag next to another computer to link their edges"
            "%2"
            "<br>Double click to edit settings"
            "<br>Drag computer to the trashcan to remove it"
        )
            .arg(m_name, sizeHint)
    );
  }

  int index() const
  {
    return m_index;
  }

  bool isServer() const
  {
    return m_isServer;
  }

  // Laid out by its monitors, which are moved together and can't be resized here.
  bool hasMonitors() const
  {
    return !m_monitors.isEmpty();
  }

  QPainterPath shape() const override
  {
    if (!hasMonitors())
      return QGraphicsRectItem::shape();

    // Only the monitors take up space, so a neighbour can be dropped into a gap between them.
    QPainterPath path;
    for (const auto &monitor : m_monitors)
      path.addRect(monitor);
    return path;
  }

  void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override
  {
    // Draw in device pixels so the icon and name stay readable however far the layout is zoomed out.
    const auto transform = painter->worldTransform();
    const auto &palette = widget != nullptr ? widget->palette() : option->palette;

    painter->save();
    painter->resetTransform();
    painter->setRenderHint(QPainter::Antialiasing);
    auto border = palette.color(QPalette::Text);
    border.setAlphaF(0.35f);

    const QList<QRectF> tiles = hasMonitors() ? toTiles(m_monitors) : QList<QRectF>{rect()};
    for (int i = 0; i < tiles.size(); i++) {
      const auto &tile = tiles.at(i);
      const QRectF device = transform.mapRect(tile).adjusted(1, 1, -1, -1);
      painter->setPen(m_isServer ? QPen(palette.color(QPalette::Highlight), 2) : QPen(border, 1));
      painter->setBrush(palette.color(isSelected() ? QPalette::Midlight : QPalette::Button));
      painter->drawRoundedRect(device, 4, 4);
      paintLabel(painter, palette, device, tile.size().toSize(), i == 0);
    }

    if (!hasMonitors()) {
      const QRectF device = transform.mapRect(rect()).adjusted(1, 1, -1, -1);
      painter->setPen(border);
      for (int offset = 4; offset < kHandlePixels; offset += 4) {
        painter->drawLine(device.bottomRight() - QPointF(offset, 2), device.bottomRight() - QPointF(2, offset));
      }
    }
    painter->restore();
  }

private:
  static QList<QRectF> toTiles(const QList<QRect> &rects)
  {
    QList<QRectF> tiles;
    for (const auto &rect : rects)
      tiles.append(QRectF(rect));
    return tiles;
  }

  // The first tile shows the icon and name, every tile shows its size in pixels.
  void paintLabel(QPainter *painter, const QPalette &palette, const QRectF &device, const QSize &pixels, bool named)
      const
  {
    const auto sizeText = QStringLiteral("%1 %2 %3").arg(pixels.width()).arg(QChar(0x00D7)).arg(pixels.height());
    const int textHeight = painter->fontMetrics().height();
    const int lines = named ? 2 : 1;
    const int iconSize = named ? std::min(64, static_cast<int>(device.height()) - lines * textHeight - 8) : 0;
    const bool showIcon = iconSize >= 16;
    const double blockHeight = (showIcon ? iconSize : 0) + lines * textHeight;
    double top = device.center().y() - blockHeight / 2;

    if (showIcon) {
      const QRect iconRect(static_cast<int>(device.center().x()) - iconSize / 2, static_cast<int>(top), iconSize, iconSize);
      painter->drawPixmap(iconRect, m_pixmap);
      top += iconSize;
    }

    const auto drawLine = [&](const QString &text, const QColor &color) {
      const QRectF textRect(device.left() + 4, top, device.width() - 8, textHeight);
      painter->setPen(color);
      painter->drawText(
          textRect, Qt::AlignCenter,
          painter->fontMetrics().elidedText(text, Qt::ElideMiddle, static_cast<int>(textRect.width()))
      );
      top += textHeight;
    };
    if (named)
      drawLine(m_name, palette.color(QPalette::Text));
    auto dim = palette.color(QPalette::Text);
    dim.setAlphaF(0.6f);
    drawLine(sizeText, dim);
  }

  int m_index;
  QString m_name;
  QPixmap m_pixmap;
  bool m_isServer;
  QList<QRect> m_monitors;
};

ComputerLayoutView::ComputerLayoutView(QWidget *parent) : QGraphicsView(parent)
{
  setScene(new QGraphicsScene(this));
  setAcceptDrops(true);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setRenderHint(QPainter::Antialiasing);
  setAlignment(Qt::AlignCenter);
}

const QString &ComputerLayoutView::mimeType()
{
  static const QString type = QStringLiteral("application/x-deskflow-computer");
  return type;
}

void ComputerLayoutView::setComputers(ComputerList *computers)
{
  m_computers = computers;
  rebuild();
}

void ComputerLayoutView::rebuild()
{
  m_dragged = nullptr;
  m_resized = nullptr;
  scene()->clear();
  if (m_computers == nullptr)
    return;

  for (int i = 0; i < m_computers->size(); i++) {
    const auto &computer = m_computers->at(i);
    if (!computer.isNull())
      scene()->addItem(new ComputerItem(i, computer));
  }
  fit();
}

void ComputerLayoutView::fit()
{
  const auto bounds = scene()->itemsBoundingRect();
  if (bounds.isEmpty())
    return;

  const auto scale = std::min(
      (viewport()->width() - 2.0 * kMarginPixels) / bounds.width(),
      (viewport()->height() - 2.0 * kMarginPixels) / bounds.height()
  );
  if (scale <= 0)
    return;
  const auto margin = kMarginPixels / scale;
  const auto padded = bounds.adjusted(-margin, -margin, margin, margin);
  scene()->setSceneRect(padded);
  fitInView(padded, Qt::KeepAspectRatio);
}

void ComputerLayoutView::removeComputer(int index)
{
  if (m_computers->at(index).isServer())
    return;
  m_computers->removeAt(index);
  rebuild();
  Q_EMIT computersChanged();
}

ComputerItem *ComputerLayoutView::itemAt(const QPoint &pos) const
{
  return dynamic_cast<ComputerItem *>(QGraphicsView::itemAt(pos));
}

bool ComputerLayoutView::onResizeHandle(const ComputerItem *item, const QPoint &pos) const
{
  if (item->hasMonitors())
    return false;
  const auto corner = mapFromScene(item->mapToScene(item->rect().bottomRight()));
  return pos.x() <= corner.x() && pos.y() <= corner.y() && corner.x() - pos.x() < kHandlePixels &&
         corner.y() - pos.y() < kHandlePixels;
}

void ComputerLayoutView::finishResize(ComputerItem *item)
{
  auto &computer = (*m_computers)[item->index()];
  const auto others = m_computers->layoutRects(computer.name());
  const QRect resized(item->pos().toPoint(), item->rect().size().toSize());
  const auto snapped = snapResize(resized, others, qRound(kSnapPixels / transform().m11()));
  if (snapped != computer.geometry() && !overlapsAny(snapped, others)) {
    computer.setGeometry(snapped);
    Q_EMIT computersChanged();
  }
  rebuild();
}

void ComputerLayoutView::dragOut(ComputerItem *item)
{
  // Leaving the view hands the computer over to a real drag so it can follow the cursor onto the trash.
  const int index = item->index();
  m_dragged = nullptr;
  if (auto *grabber = scene()->mouseGrabberItem(); grabber != nullptr)
    grabber->ungrabMouse();

  auto *mimeData = new QMimeData();
  mimeData->setData(mimeType(), {});
  auto *drag = new QDrag(this);
  drag->setMimeData(mimeData);
  drag->setPixmap(m_computers->at(index).pixmap());
  if (drag->exec(Qt::MoveAction) == Qt::MoveAction)
    removeComputer(index);
  else
    rebuild();
}

void ComputerLayoutView::resizeEvent(QResizeEvent *event)
{
  QGraphicsView::resizeEvent(event);
  fit();
}

void ComputerLayoutView::mousePressEvent(QMouseEvent *event)
{
  auto *item = event->button() == Qt::LeftButton ? itemAt(event->pos()) : nullptr;
  if (item != nullptr && onResizeHandle(item, event->pos())) {
    m_resized = item;
    return;
  }
  m_dragged = item;
  QGraphicsView::mousePressEvent(event);
}

void ComputerLayoutView::mouseMoveEvent(QMouseEvent *event)
{
  if (m_resized != nullptr) {
    const auto size = mapToScene(event->pos()) - m_resized->pos();
    m_resized->setRect(QRectF(
        QPointF(),
        QSizeF(std::max<qreal>(size.x(), kMinimumSize.width()), std::max<qreal>(size.y(), kMinimumSize.height()))
    ));
    return;
  }

  if (m_dragged != nullptr && !m_dragged->isServer() && !viewport()->rect().contains(event->pos())) {
    dragOut(m_dragged);
    return;
  }

  if (auto *item = itemAt(event->pos()); item != nullptr && event->buttons() == Qt::NoButton)
    item->setCursor(onResizeHandle(item, event->pos()) ? Qt::SizeFDiagCursor : Qt::OpenHandCursor);
  QGraphicsView::mouseMoveEvent(event);
}

void ComputerLayoutView::mouseReleaseEvent(QMouseEvent *event)
{
  if (auto *resized = std::exchange(m_resized, nullptr); resized != nullptr) {
    finishResize(resized);
    return;
  }

  QGraphicsView::mouseReleaseEvent(event);
  auto *item = std::exchange(m_dragged, nullptr);
  if (item == nullptr || event->button() != Qt::LeftButton)
    return;

  auto &computer = (*m_computers)[item->index()];

  // A computer laid out by its monitors moves them together and snaps by whichever monitor is closest.
  const auto others = m_computers->layoutRects(computer.name());
  const auto delta = item->pos().toPoint() - computer.geometry().topLeft();
  QList<QRect> moved;
  for (const auto &rect : computer.layoutRects())
    moved.append(rect.translated(delta));
  const auto offset = snapGroup(moved, others, qRound(kSnapPixels / transform().m11()));
  for (auto &rect : moved)
    rect.translate(offset);
  const auto geometry = computer.geometry().translated(delta + offset);
  if (geometry != computer.geometry() && !overlapsAny(moved, others)) {
    computer.setGeometry(geometry);
    Q_EMIT computersChanged();
  }
  // Redraw every item from the stored geometry, which also undoes moves of other selected items.
  rebuild();
}

void ComputerLayoutView::mouseDoubleClickEvent(QMouseEvent *event)
{
  auto *item = itemAt(event->pos());
  if (item == nullptr || event->button() != Qt::LeftButton) {
    QGraphicsView::mouseDoubleClickEvent(event);
    return;
  }

  ComputerSettingsDialog dialog(this, &(*m_computers)[item->index()], m_computers);
  dialog.exec();
  rebuild();
  Q_EMIT computersChanged();
}

void ComputerLayoutView::keyPressEvent(QKeyEvent *event)
{
  const auto selected = scene()->selectedItems();
  if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && selected.size() == 1) {
    removeComputer(static_cast<ComputerItem *>(selected.first())->index());
    return;
  }
  QGraphicsView::keyPressEvent(event);
}

void ComputerLayoutView::dragEnterEvent(QDragEnterEvent *event)
{
  dragMoveEvent(event);
}

void ComputerLayoutView::dragMoveEvent(QDragMoveEvent *event)
{
  if (m_computers != nullptr && event->source() != this && event->mimeData()->hasFormat(mimeType()))
    event->acceptProposedAction();
  else
    event->ignore();
}

void ComputerLayoutView::dropEvent(QDropEvent *event)
{
  if (m_computers == nullptr || !event->mimeData()->hasFormat(mimeType())) {
    event->ignore();
    return;
  }

  auto encoded = event->mimeData()->data(mimeType());
  QDataStream stream(&encoded, QIODevice::ReadOnly);
  int unusedColumn = -1;
  int unusedRow = -1;
  Computer computer;
  stream >> unusedColumn >> unusedRow >> computer;

  const auto size = ComputerList::kDefaultSize;
  const auto center = mapToScene(event->position().toPoint()).toPoint();
  const auto others = m_computers->layoutRects();
  const auto placed = snap(
      QRect(center - QPoint(size.width() / 2, size.height() / 2), size), others, qRound(kSnapPixels / transform().m11())
  );
  if (overlapsAny(placed, others)) {
    m_computers->addComputerByPriority(computer);
  } else {
    computer.setGeometry(placed);
    m_computers->append(computer);
  }

  event->acceptProposedAction();
  rebuild();
  Q_EMIT computersChanged();
}

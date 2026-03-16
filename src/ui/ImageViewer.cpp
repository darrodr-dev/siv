#include "ImageViewer.h"

#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "io/TileProvider.h"

static constexpr double kZoomFactor  = 1.25;
static constexpr double kZoomMin     = 0.005;  // 0.5%
static constexpr double kZoomMax     = 64.0;   // 6400%
static constexpr int    kTimerMs     = 16;      // ~60 fps
static constexpr double kZoomEase    = 0.2;     // fraction of gap closed per frame
static constexpr double kPanFriction = 0.88;    // velocity multiplier per frame
static constexpr double kPanStop     = 0.3;     // px/frame below which we stop

// ── TiledImageItem ─────────────────────────────────────────────────────────────
namespace {
class TiledImageItem : public QGraphicsItem {
public:
    explicit TiledImageItem(TileProvider* provider) : m_provider(provider) {
        m_provider->setTileReadyCallback([this](int, int) {
            if (!m_frozen) update();
        });
    }

    void setFrozen(bool frozen) {
        m_frozen = frozen;
        if (!frozen) update();
    }

    ~TiledImageItem() override {
        m_provider->setTileReadyCallback(nullptr);
    }

    QRectF boundingRect() const override {
        return QRectF(0, 0, m_provider->imageCols(), m_provider->imageRows());
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget*) override {
        const QRectF& exposed = option->exposedRect;
        if (exposed.isEmpty()) return;

        const QImage& ov  = m_provider->overview();
        const float   ovF = static_cast<float>(m_provider->oversampleFactor());

        // Always draw overview as background/placeholder (smooth-scaled).
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        QRectF ovSrc(exposed.x() / ovF, exposed.y() / ovF,
                     exposed.width() / ovF, exposed.height() / ovF);
        painter->drawImage(exposed, ov, ovSrc.intersected(QRectF(ov.rect())));

        // Overlay full-res tiles only when zoomed in past overview resolution.
        const float viewScale = static_cast<float>(painter->worldTransform().m11());
        if (viewScale * ovF <= 1.0f) return;

        const int T     = m_provider->tileSize();
        const int trMin = std::max(0, static_cast<int>(exposed.top()    / T));
        const int trMax = std::min(m_provider->tileRows() - 1, static_cast<int>(exposed.bottom() / T));
        const int tcMin = std::max(0, static_cast<int>(exposed.left()   / T));
        const int tcMax = std::min(m_provider->tileCols() - 1, static_cast<int>(exposed.right()  / T));

        m_provider->setViewport(trMin, trMax, tcMin, tcMax);

        painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
        for (int tr = trMin; tr <= trMax; ++tr) {
            for (int tc = tcMin; tc <= tcMax; ++tc) {
                const QImage tile = m_frozen ? m_provider->peekTile(tr, tc)
                                             : m_provider->tile(tr, tc);
                if (tile.isNull()) continue;
                const QRectF tileScene(tc * T, tr * T, tile.width(), tile.height());
                const QRectF dst = tileScene.intersected(exposed);
                const QRectF src = dst.translated(-tileScene.x(), -tileScene.y());
                painter->drawImage(dst, tile, src);
            }
        }
    }

private:
    TileProvider* m_provider;
    bool m_frozen = false;
};
}  // namespace

// ── Construction ──────────────────────────────────────────────────────────────

ImageViewer::ImageViewer(QWidget* parent) : QGraphicsView(parent), m_scene(new QGraphicsScene(this)) {
  setScene(m_scene);
  setRenderHint(QPainter::Antialiasing);
  setDragMode(QGraphicsView::NoDrag);
  // Anchor is managed manually during animated zoom so the scene point stays
  // locked for the full duration of the animation (AnchorUnderMouse drifts
  // when scale() is called from a timer rather than an event handler).
  setTransformationAnchor(QGraphicsView::NoAnchor);
  setResizeAnchor(QGraphicsView::AnchorViewCenter);
  setBackgroundBrush(QColor(40, 40, 40));
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);

  // Rendering optimisations
  setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
  setOptimizationFlag(QGraphicsView::DontAdjustForAntialiasing);
  setOptimizationFlag(QGraphicsView::DontSavePainterState);

  // Kinetic pan timer
  m_kineticTimer = new QTimer(this);
  m_kineticTimer->setInterval(kTimerMs);
  connect(m_kineticTimer, &QTimer::timeout, this, &ImageViewer::onKineticPan);

  // Smooth zoom timer
  m_zoomTimer = new QTimer(this);
  m_zoomTimer->setInterval(kTimerMs);
  connect(m_zoomTimer, &QTimer::timeout, this, &ImageViewer::onSmoothZoom);
}

// ── Image ─────────────────────────────────────────────────────────────────────

static TiledImageItem* asTiled(QGraphicsItem* item) {
  return static_cast<TiledImageItem*>(item);
}

void ImageViewer::setTileProvider(TileProvider* provider) {
  m_zoomTimer->stop();
  m_kineticTimer->stop();
  m_scene->clear();   // deletes old TiledImageItem (which clears its callback)
  m_imageItem = nullptr;
  m_tileProvider = nullptr;
  if (!provider) return;
  auto* item = new TiledImageItem(provider);
  m_scene->addItem(item);
  m_imageItem = item;
  m_tileProvider = provider;
  m_scene->setSceneRect(QRectF(0, 0, provider->imageCols(), provider->imageRows()));

  // When stretch changes, redraw the whole scene (overview + visible tiles all updated).
  connect(provider, &TileProvider::imageChanged, this, [this]() {
    if (m_imageItem) m_imageItem->update();
  });

  m_userZoomed = false;
  m_targetZoom = 1.0;
  fitView();
}

double ImageViewer::currentZoomPct() const { return transform().m11() * 100.0; }

// ── Zoom ──────────────────────────────────────────────────────────────────────

void ImageViewer::applyScale(double factor) {
  const double current = transform().m11();
  const double next = std::clamp(current * factor, kZoomMin, kZoomMax);
  scale(next / current, next / current);
  emit zoomChanged(currentZoomPct());
}

void ImageViewer::scheduleZoomTo(double target, QPointF anchorView) {
  // Capture the scene point that must remain fixed under the cursor.
  // If no explicit anchor supplied (keyboard zoom), use viewport centre.
  if (anchorView.isNull()) anchorView = QPointF(viewport()->rect().center());
  m_zoomAnchorView  = anchorView;
  m_zoomAnchorScene = mapToScene(anchorView.toPoint());
  m_targetZoom = std::clamp(target, kZoomMin, kZoomMax);
  if (!m_zoomTimer->isActive()) {
    if (m_imageItem) asTiled(m_imageItem)->setFrozen(true);
    m_zoomTimer->start();
  }
}

/// After every scale() call, shift scrollbars so m_zoomAnchorScene stays
/// exactly under m_zoomAnchorView — eliminates the AnchorUnderMouse drift.
void ImageViewer::keepAnchor() {
  const QPointF newView = mapFromScene(m_zoomAnchorScene);
  const QPointF shift   = newView - m_zoomAnchorView;
  horizontalScrollBar()->setValue(horizontalScrollBar()->value() + qRound(shift.x()));
  verticalScrollBar()->setValue(verticalScrollBar()->value()   + qRound(shift.y()));
}

void ImageViewer::onSmoothZoom() {
  // Suppress all intermediate repaints (tile callbacks, scrollbar changes, scale())
  // so the viewport redraws exactly once per zoom step with a fully consistent state.
  viewport()->setUpdatesEnabled(false);

  const double current = transform().m11();
  const double diff    = m_targetZoom - current;

  if (std::abs(diff) < current * 0.001) {
    scale(m_targetZoom / current, m_targetZoom / current);
    keepAnchor();
    m_zoomTimer->stop();
    if (m_imageItem) asTiled(m_imageItem)->setFrozen(false);
    if (m_tileProvider) m_tileProvider->flushQueue();
    viewport()->setUpdatesEnabled(true);
    emit zoomChanged(currentZoomPct());
    return;
  }

  scale(1.0 + diff * kZoomEase / current, 1.0 + diff * kZoomEase / current);
  keepAnchor();
  viewport()->setUpdatesEnabled(true);
  emit zoomChanged(currentZoomPct());
}

void ImageViewer::zoomIn()  { scheduleZoomTo(transform().m11() * kZoomFactor); }
void ImageViewer::zoomOut() { scheduleZoomTo(transform().m11() / kZoomFactor); }

void ImageViewer::fitView() {
  if (!hasImage()) return;
  m_zoomTimer->stop();
  if (m_imageItem) asTiled(m_imageItem)->setFrozen(false);
  if (m_tileProvider) m_tileProvider->flushQueue();
  m_userZoomed = false;
  resetTransform();
  fitInView(m_scene->sceneRect(), Qt::KeepAspectRatio);
  m_targetZoom = transform().m11();
  emit zoomChanged(currentZoomPct());
}

void ImageViewer::zoom1to1() {
  m_userZoomed = true;
  scheduleZoomTo(1.0, QPointF(viewport()->rect().center()));
}

// ── Kinetic pan ───────────────────────────────────────────────────────────────

void ImageViewer::onKineticPan() {
  if (m_panVelocity.manhattanLength() < kPanStop) {
    m_kineticTimer->stop();
    if (m_tileProvider) m_tileProvider->flushQueue();
    return;
  }
  viewport()->setUpdatesEnabled(false);
  horizontalScrollBar()->setValue(horizontalScrollBar()->value() - static_cast<int>(m_panVelocity.x()));
  verticalScrollBar()->setValue(verticalScrollBar()->value() - static_cast<int>(m_panVelocity.y()));
  viewport()->setUpdatesEnabled(true);
  m_panVelocity *= kPanFriction;
}

// ── Events ────────────────────────────────────────────────────────────────────

void ImageViewer::wheelEvent(QWheelEvent* event) {
  m_userZoomed = true;
  m_kineticTimer->stop();
  m_panVelocity = {};

  const double factor = (event->angleDelta().y() > 0) ? kZoomFactor : 1.0 / kZoomFactor;
  scheduleZoomTo(transform().m11() * factor, event->position());
  event->accept();
}

void ImageViewer::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    m_kineticTimer->stop();
    m_panVelocity = {};
    m_panning = true;
    m_lastPan = event->pos();
    m_panClock.start();
    setCursor(Qt::ClosedHandCursor);
    event->accept();
    return;
  }
  QGraphicsView::mousePressEvent(event);
}

void ImageViewer::mouseReleaseEvent(QMouseEvent* event) {
  if (m_panning && event->button() == Qt::LeftButton) {
    m_panning = false;
    setCursor(Qt::ArrowCursor);
    // Kick off kinetic coast if we have meaningful velocity
    if (m_panVelocity.manhattanLength() >= kPanStop) {
      m_kineticTimer->start();
    } else {
      // Settled without coasting — flush stale tile requests now.
      if (m_tileProvider) m_tileProvider->flushQueue();
    }
    event->accept();
    return;
  }
  QGraphicsView::mouseReleaseEvent(event);
}

void ImageViewer::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    m_panning = false;
    m_kineticTimer->stop();
    m_panVelocity = {};
    setCursor(Qt::ArrowCursor);
    fitView();
    event->accept();
    return;
  }
  QGraphicsView::mouseDoubleClickEvent(event);
}

void ImageViewer::mouseMoveEvent(QMouseEvent* event) {
  if (m_panning) {
    const QPoint delta = event->pos() - m_lastPan;
    const qint64 dt = m_panClock.restart();  // ms since last move

    // Track velocity in viewport pixels per frame (at kTimerMs ms/frame)
    if (dt > 0)
      m_panVelocity = QPointF(delta) * kTimerMs / static_cast<double>(dt);

    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
    verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
    m_lastPan = event->pos();
    event->accept();
  }

  if (m_imageItem) {
    const QPointF scene = mapToScene(event->pos());
    emit pixelHovered(static_cast<int>(scene.x()), static_cast<int>(scene.y()));
  }

  QGraphicsView::mouseMoveEvent(event);
}

void ImageViewer::resizeEvent(QResizeEvent* event) {
  QGraphicsView::resizeEvent(event);
  if (!m_userZoomed) fitView();
}

void ImageViewer::keyPressEvent(QKeyEvent* event) {
  switch (event->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoomIn();   break;
    case Qt::Key_Minus: zoomOut();  break;
    case Qt::Key_F:     fitView();  break;
    case Qt::Key_1:     zoom1to1(); break;
    default: QGraphicsView::keyPressEvent(event);
  }
}

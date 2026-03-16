#pragma once

#include <QElapsedTimer>
#include <QGraphicsView>
#include <QPointF>

class QGraphicsScene;
class QGraphicsItem;
class QTimer;
class TileProvider;

/**
 * A QGraphicsView that supports smooth pan and zoom.
 *
 *  Zoom         – scroll wheel (zooms toward cursor, animated)
 *  Pan          – left-click drag (with kinetic coast on release)
 *  Double-click – reset to fit
 *  Keys         – +/- zoom, F fit, 1 one-to-one
 */
class ImageViewer : public QGraphicsView {
  Q_OBJECT

 public:
  explicit ImageViewer(QWidget* parent = nullptr);

  void setTileProvider(TileProvider* provider);
  bool hasImage() const { return m_imageItem != nullptr; }

  void zoomIn();
  void zoomOut();
  void fitView();
  void zoom1to1();

  /// Current zoom as a percentage (100 = 1:1).
  double currentZoomPct() const;

 signals:
  void zoomChanged(double pct);
  void pixelHovered(int x, int y);

 protected:
  void wheelEvent(QWheelEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private slots:
  void onKineticPan();
  void onSmoothZoom();

 private:
  void applyScale(double factor);
  void scheduleZoomTo(double targetScale, QPointF anchorView = {});
  void keepAnchor();

  QGraphicsScene* m_scene = nullptr;
  QGraphicsItem* m_imageItem = nullptr;
  TileProvider* m_tileProvider = nullptr;

  // Pan state
  bool m_panning = false;
  bool m_userZoomed = false;
  QPoint m_lastPan;
  QPointF m_panVelocity;    // viewport pixels per ms
  QElapsedTimer m_panClock;
  QTimer* m_kineticTimer = nullptr;

  // Zoom animation
  double  m_targetZoom       = 1.0;
  QPointF m_zoomAnchorView;   // viewport px where zoom was initiated
  QPointF m_zoomAnchorScene;  // scene pt that must stay under m_zoomAnchorView
  QTimer* m_zoomTimer        = nullptr;
};

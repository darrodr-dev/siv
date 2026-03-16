#pragma once

#include <QList>
#include <QMainWindow>
#include <QString>
#include "io/TileProvider.h"

class ImageViewer;
class QTreeWidget;
class QLabel;
class QDockWidget;
class QAction;
class QTimer;
class HistogramWidget;

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);

  /// Open a file from code (e.g. command-line argument).
  void openFile(const QString& path);

 private slots:
  void onOpenFile();
  void onZoomIn();
  void onZoomOut();
  void onZoomFit();
  void onZoom1to1();
  void onZoomChanged(double pct);
  void onPixelHovered(int x, int y);

 private:
  void buildUi();
  void buildMenuBar();
  void buildToolBar();
  void buildDock();
  void buildStatusBar();

  void setWindowFile(const QString& path);
  void showError(const QString& msg);
  void populateXmlTree(const QList<QString>& xmlSegments);

  ImageViewer*     m_viewer      = nullptr;
  QTreeWidget*     m_xmlTree     = nullptr;
  QDockWidget*     m_xmlDock     = nullptr;
  HistogramWidget* m_histogram   = nullptr;
  TileProvider*    m_tileProvider = nullptr;
  QString          m_imageInfo;
  StretchParams    m_pendingStretch;
  QTimer*          m_stretchDebounce = nullptr;
  QLabel*          m_zoomLabel   = nullptr;
  QLabel*          m_coordLabel  = nullptr;

  QAction* m_actOpen    = nullptr;
  QAction* m_actZoomIn  = nullptr;
  QAction* m_actZoomOut = nullptr;
  QAction* m_actZoomFit  = nullptr;
  QAction* m_actZoom1to1 = nullptr;
  QAction* m_actXmlPanel = nullptr;
};

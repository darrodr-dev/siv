#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QStack>
#include <QTreeWidget>
#include <QXmlStreamReader>
#include <QtConcurrent>
#include <QTimer>
#include <QToolBar>

#include "ImageViewer.h"
#include "HistogramWidget.h"
#include "io/SARImageLoader.h"
#include "io/TileProvider.h"

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("SIV");
  resize(1920, 1080);

  m_stretchDebounce = new QTimer(this);
  m_stretchDebounce->setSingleShot(true);
  m_stretchDebounce->setInterval(120);  // ms after last slider move to reload tiles
  connect(m_stretchDebounce, &QTimer::timeout, this, [this]() {
    if (m_tileProvider) m_tileProvider->setStretch(m_pendingStretch);
  });

  buildUi();
}

// ── UI construction ───────────────────────────────────────────────────────────

void MainWindow::buildUi() {
  m_viewer = new ImageViewer(this);
  setCentralWidget(m_viewer);

  connect(m_viewer, &ImageViewer::zoomChanged, this, &MainWindow::onZoomChanged);
  connect(m_viewer, &ImageViewer::pixelHovered, this, &MainWindow::onPixelHovered);

  buildMenuBar();
  buildToolBar();
  buildDock();
  buildStatusBar();

  // Connect once here so loading multiple files never accumulates connections.
  connect(m_histogram, &HistogramWidget::stretchChanged, this, [this](float lo, float hi) {
    if (!m_tileProvider) return;
    m_pendingStretch = m_tileProvider->stretch();
    m_pendingStretch.lo = lo;
    m_pendingStretch.hi = hi;
    m_tileProvider->updateOverview(m_pendingStretch);
    m_stretchDebounce->start();
  });
}

void MainWindow::buildMenuBar() {
  m_actOpen = new QAction(QIcon::fromTheme("document-open"), "&Open…", this);
  m_actOpen->setShortcut(QKeySequence::Open);
  m_actOpen->setStatusTip("Open a SICD or SIDD NITF file");
  connect(m_actOpen, &QAction::triggered, this, &MainWindow::onOpenFile);

  auto* actExit = new QAction("E&xit", this);
  actExit->setShortcut(QKeySequence::Quit);
  connect(actExit, &QAction::triggered, qApp, &QApplication::quit);

  QMenu* fileMenu = menuBar()->addMenu("&File");
  fileMenu->addAction(m_actOpen);
  fileMenu->addSeparator();
  fileMenu->addAction(actExit);

  m_actZoomIn = new QAction(QIcon::fromTheme("zoom-in"), "Zoom &In", this);
  m_actZoomIn->setShortcut(Qt::Key_Plus);
  connect(m_actZoomIn, &QAction::triggered, this, &MainWindow::onZoomIn);

  m_actZoomOut = new QAction(QIcon::fromTheme("zoom-out"), "Zoom &Out", this);
  m_actZoomOut->setShortcut(Qt::Key_Minus);
  connect(m_actZoomOut, &QAction::triggered, this, &MainWindow::onZoomOut);

  m_actZoomFit = new QAction(QIcon::fromTheme("zoom-fit-best"), "&Fit to Window", this);
  m_actZoomFit->setShortcut(Qt::Key_F);
  connect(m_actZoomFit, &QAction::triggered, this, &MainWindow::onZoomFit);

  m_actZoom1to1 = new QAction(QIcon::fromTheme("zoom-original"), "1:&1 Zoom", this);
  m_actZoom1to1->setShortcut(Qt::Key_1);
  connect(m_actZoom1to1, &QAction::triggered, this, &MainWindow::onZoom1to1);

  m_actXmlPanel = new QAction("&XML Panel", this);
  m_actXmlPanel->setCheckable(true);
  m_actXmlPanel->setChecked(true);
  m_actXmlPanel->setShortcut(Qt::CTRL | Qt::Key_X);

  QMenu* viewMenu = menuBar()->addMenu("&View");
  viewMenu->addAction(m_actZoomIn);
  viewMenu->addAction(m_actZoomOut);
  viewMenu->addAction(m_actZoomFit);
  viewMenu->addAction(m_actZoom1to1);
  viewMenu->addSeparator();
  viewMenu->addAction(m_actXmlPanel);
}

void MainWindow::buildToolBar() {
  QToolBar* tb = addToolBar("Main");
  tb->setObjectName("MainToolBar");
  tb->setMovable(false);

  tb->addAction(m_actOpen);
  tb->addSeparator();
  tb->addAction(m_actZoomIn);
  tb->addAction(m_actZoomOut);
  tb->addAction(m_actZoomFit);
  tb->addAction(m_actZoom1to1);
}

void MainWindow::buildDock() {
  m_xmlTree = new QTreeWidget(this);
  m_xmlTree->setColumnCount(2);
  m_xmlTree->setHeaderLabels({"Field", "Value"});
  m_xmlTree->setAlternatingRowColors(true);
  m_xmlTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_xmlTree->setSelectionMode(QAbstractItemView::SingleSelection);
  m_xmlTree->header()->setStretchLastSection(true);
  m_xmlTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
  m_xmlTree->setColumnWidth(0, 200);

  m_histogram = new HistogramWidget(this);

  auto* splitter = new QSplitter(Qt::Vertical, this);
  splitter->addWidget(m_xmlTree);
  splitter->addWidget(m_histogram);
  splitter->setStretchFactor(0, 4);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({600, 130});

  m_xmlDock = new QDockWidget("XML Metadata", this);
  m_xmlDock->setObjectName("XmlDock");
  m_xmlDock->setWidget(splitter);
  m_xmlDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                          QDockWidget::DockWidgetClosable);
  addDockWidget(Qt::RightDockWidgetArea, m_xmlDock);

  connect(m_actXmlPanel, &QAction::toggled, m_xmlDock, &QDockWidget::setVisible);
  connect(m_xmlDock, &QDockWidget::visibilityChanged, m_actXmlPanel, &QAction::setChecked);

  resizeDocks({m_xmlDock}, {420}, Qt::Horizontal);
}

void MainWindow::buildStatusBar() {
  m_zoomLabel = new QLabel("Zoom: —", this);
  m_coordLabel = new QLabel("", this);

  m_zoomLabel->setMinimumWidth(100);
  m_coordLabel->setMinimumWidth(120);

  statusBar()->addPermanentWidget(m_zoomLabel);
  statusBar()->addPermanentWidget(m_coordLabel);
  statusBar()->showMessage("Ready — open a SICD or SIDD NITF file");
}

// ── File opening ──────────────────────────────────────────────────────────────

void MainWindow::onOpenFile() {
  static QString lastDir;
  QString path = QFileDialog::getOpenFileName(this, "Open SAR Image", lastDir,
                                              "NITF SAR Images (*.ntf *.nitf *.NTF *.NITF);;"
                                              "All Files (*)");
  if (path.isEmpty()) return;

  lastDir = QFileInfo(path).absolutePath();
  openFile(path);
}

void MainWindow::openFile(const QString& path) {
  statusBar()->showMessage("Loading " + QFileInfo(path).fileName() + " …");
  QApplication::setOverrideCursor(Qt::WaitCursor);
  m_actOpen->setEnabled(false);

  auto* watcher = new QFutureWatcher<SARLoadResult>(this);

  connect(watcher, &QFutureWatcher<SARLoadResult>::finished, this, [this, watcher, path]() {
    watcher->deleteLater();
    QApplication::restoreOverrideCursor();
    m_actOpen->setEnabled(true);

    try {
      SARLoadResult result = watcher->result();

      // Create TileProvider on the main thread so its thread affinity is correct.
      delete m_tileProvider;
      m_tileProvider = new TileProvider(
          path,
          result.numRows, result.numCols, result.pixelType,
          result.tileSize,
          result.stretch,
          std::move(result.overview),
          result.oversampleFactor,
          std::move(result.overviewBuf),
          this);

      m_viewer->setTileProvider(m_tileProvider);

      m_histogram->setHistogram(result.histogram, result.stretch);

      connect(m_tileProvider, &TileProvider::loadingProgress, this, [this](int n) {
        if (n == 0)
          statusBar()->showMessage(m_imageInfo);
        else
          statusBar()->showMessage(QString("Loading %1 tile%2…").arg(n).arg(n == 1 ? "" : "s"));
      });

      populateXmlTree(result.xmlSegments);
      m_xmlDock->setVisible(true);

      setWindowFile(path);
      m_imageInfo = QString("%1  —  %2 × %3 px")
                        .arg(result.dataType).arg(result.numCols).arg(result.numRows);
      statusBar()->showMessage(m_imageInfo);
    } catch (const SARLoadError& e) {
      showError(QString::fromStdString(e.what()));
    } catch (const std::exception& e) {
      showError(QString("Unexpected error: ") + e.what());
    }
  });

  watcher->setFuture(QtConcurrent::run(SARImageLoader::load, path));
}

// ── XML tree ──────────────────────────────────────────────────────────────────

static void appendXmlToTree(QTreeWidget* tree, QTreeWidgetItem* parent, const QString& xml) {
  QXmlStreamReader reader(xml);
  QStack<QTreeWidgetItem*> stack;
  bool rootSkipped = false;

  while (!reader.atEnd() && !reader.hasError()) {
    reader.readNext();

    if (reader.isStartElement()) {
      if (!rootSkipped) {
        rootSkipped = true;
        continue;
      }
      auto* item = new QTreeWidgetItem();
      item->setText(0, reader.name().toString());
      if (stack.isEmpty()) {
        parent ? parent->addChild(item) : tree->addTopLevelItem(item);
      } else {
        stack.top()->addChild(item);
      }
      stack.push(item);
    } else if (reader.isCharacters() && !reader.isWhitespace()) {
      if (!stack.isEmpty()) {
        QString cur = stack.top()->text(1);
        stack.top()->setText(1, cur.isEmpty() ? reader.text().toString().trimmed()
                                               : cur + " " + reader.text().toString().trimmed());
      }
    } else if (reader.isEndElement()) {
      if (!stack.isEmpty()) stack.pop();
    }
  }
}

void MainWindow::populateXmlTree(const QList<QString>& xmlSegments) {
  m_xmlTree->clear();
  if (xmlSegments.isEmpty()) return;

  const bool multiSegment = xmlSegments.size() > 1;

  for (int i = 0; i < xmlSegments.size(); ++i) {
    const QString& xml = xmlSegments[i];
    if (xml.isEmpty()) continue;

    QString rootName;
    {
      QXmlStreamReader probe(xml);
      while (!probe.atEnd()) {
        probe.readNext();
        if (probe.isStartElement()) { rootName = probe.name().toString(); break; }
      }
    }

    if (multiSegment) {
      auto* group = new QTreeWidgetItem(m_xmlTree);
      group->setText(0, QString("Image %1 — %2").arg(i).arg(rootName));
      appendXmlToTree(m_xmlTree, group, xml);
    } else {
      appendXmlToTree(m_xmlTree, nullptr, xml);
    }
  }

  m_xmlTree->collapseAll();

  std::function<void(QTreeWidgetItem*)> expandFirstChild = [&](QTreeWidgetItem* item) {
    if (item->childCount() == 0) return;
    item->child(0)->setExpanded(true);
    for (int i = 0; i < item->childCount(); ++i)
      expandFirstChild(item->child(i));
  };
  for (int i = 0; i < m_xmlTree->topLevelItemCount(); ++i) {
    QTreeWidgetItem* top = m_xmlTree->topLevelItem(i);
    top->setExpanded(true);
    expandFirstChild(top);
  }
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void MainWindow::onZoomIn()  { m_viewer->zoomIn(); }
void MainWindow::onZoomOut() { m_viewer->zoomOut(); }
void MainWindow::onZoomFit() { m_viewer->fitView(); }
void MainWindow::onZoom1to1() { m_viewer->zoom1to1(); }

void MainWindow::onZoomChanged(double pct) {
  m_zoomLabel->setText(QString("Zoom: %1%").arg(pct, 0, 'f', 1));
}

void MainWindow::onPixelHovered(int x, int y) {
  if (m_viewer->hasImage()) m_coordLabel->setText(QString("x:%1  y:%2").arg(x).arg(y));
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void MainWindow::setWindowFile(const QString& path) {
  setWindowTitle("SIV — " + QFileInfo(path).fileName());
  setWindowFilePath(path);
}

void MainWindow::showError(const QString& msg) {
  statusBar()->showMessage("Error: " + msg);
  QMessageBox::critical(this, "SIV", msg);
}

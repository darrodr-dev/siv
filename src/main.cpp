#include <QApplication>

#include "ui/MainWindow.h"

static void applyDarkPalette(QApplication& app) {
  app.setStyle("Fusion");

  QPalette p;
  p.setColor(QPalette::Window, QColor(30, 30, 30));
  p.setColor(QPalette::Base, QColor(20, 20, 20));
  p.setColor(QPalette::AlternateBase, QColor(40, 40, 40));
  p.setColor(QPalette::Button, QColor(45, 45, 45));
  p.setColor(QPalette::Mid, QColor(55, 55, 55));
  p.setColor(QPalette::Dark, QColor(18, 18, 18));
  p.setColor(QPalette::WindowText, QColor(220, 220, 220));
  p.setColor(QPalette::Text, QColor(220, 220, 220));
  p.setColor(QPalette::ButtonText, QColor(220, 220, 220));
  p.setColor(QPalette::BrightText, Qt::white);
  p.setColor(QPalette::Highlight, QColor(60, 120, 200));
  p.setColor(QPalette::HighlightedText, Qt::white);
  p.setColor(QPalette::Link, QColor(100, 160, 255));
  p.setColor(QPalette::ToolTipBase, QColor(50, 50, 50));
  p.setColor(QPalette::ToolTipText, QColor(220, 220, 220));

  const QColor disabled(100, 100, 100);
  p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
  p.setColor(QPalette::Disabled, QPalette::Text, disabled);
  p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);

  app.setPalette(p);
}

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  app.setApplicationName("SIV");
  app.setApplicationDisplayName("SIV — SAR Image Viewer");
  app.setApplicationVersion("0.1.0");

  applyDarkPalette(app);

  MainWindow window;
  window.show();

  if (argc > 1) window.openFile(QString::fromLocal8Bit(argv[1]));

  return app.exec();
}

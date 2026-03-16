#pragma once

#include <QWidget>
#include "io/TileProvider.h"

// Draws a log-scale magnitude histogram with two draggable handles (lo, hi).
// Emits stretchChanged() on every drag move for live preview.
class HistogramWidget : public QWidget {
    Q_OBJECT
public:
    explicit HistogramWidget(QWidget* parent = nullptr);

    QSize minimumSizeHint() const override { return {120, 90}; }
    QSize sizeHint()        const override { return {300, 120}; }

    void setHistogram(const HistogramData& hist, StretchParams stretch);
    void setStretchParams(StretchParams s);

signals:
    void stretchChanged(float lo, float hi);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    float xToMag(int px) const;
    int   magToX(float mag) const;

    HistogramData m_hist;
    StretchParams m_stretch;
    bool          m_valid      = false;
    bool          m_draggingLo = false;
    bool          m_draggingHi = false;

    static constexpr int kPad = 6;
};

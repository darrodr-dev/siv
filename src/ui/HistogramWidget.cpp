#include "HistogramWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cmath>

HistogramWidget::HistogramWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumHeight(90);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void HistogramWidget::setHistogram(const HistogramData& hist, StretchParams stretch) {
    m_hist    = hist;
    m_stretch = stretch;
    m_valid   = hist.valid;
    update();
}

void HistogramWidget::setStretchParams(StretchParams s) {
    m_stretch = s;
    update();
}

float HistogramWidget::xToMag(int px) const {
    const int w = width() - 2 * kPad;
    const float range = m_hist.globalMax - m_hist.globalMin;
    if (w <= 0 || range <= 0.0f) return m_hist.globalMin;
    return m_hist.globalMin + range * static_cast<float>(px - kPad) / w;
}

int HistogramWidget::magToX(float mag) const {
    const int w = width() - 2 * kPad;
    const float range = m_hist.globalMax - m_hist.globalMin;
    if (w <= 0 || range <= 0.0f) return kPad;
    return kPad + static_cast<int>((mag - m_hist.globalMin) / range * w);
}

void HistogramWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(28, 28, 30));

    if (!m_valid || m_hist.bins.empty() || m_hist.globalMax <= m_hist.globalMin) {
        p.setPen(QColor(90, 90, 90));
        p.drawText(rect(), Qt::AlignCenter, "No histogram data");
        return;
    }

    const int W = width()  - 2 * kPad;
    const int H = height() - 2 * kPad;
    if (W <= 0 || H <= 0) return;

    const int nBins      = static_cast<int>(m_hist.bins.size());
    const int displayBins = std::min(W, nBins);

    // Compute log-scaled heights.
    double maxLog = 0.0;
    for (int db = 0; db < displayBins; ++db) {
        const int b0 = db * nBins / displayBins;
        const int b1 = (db + 1) * nBins / displayBins;
        int64_t sum = 0;
        for (int b = b0; b < b1; ++b) sum += m_hist.bins[b];
        maxLog = std::max(maxLog, std::log1p(static_cast<double>(sum)));
    }

    if (maxLog <= 0.0) return;

    const int loX = std::clamp(magToX(m_stretch.lo), kPad, kPad + W);
    const int hiX = std::clamp(magToX(m_stretch.hi), kPad, kPad + W);

    p.setPen(Qt::NoPen);
    for (int db = 0; db < displayBins; ++db) {
        const int b0 = db * nBins / displayBins;
        const int b1 = (db + 1) * nBins / displayBins;
        int64_t sum = 0;
        for (int b = b0; b < b1; ++b) sum += m_hist.bins[b];

        const double logVal = std::log1p(static_cast<double>(sum)) / maxLog;
        const int barH = static_cast<int>(logVal * H);
        const int x  = kPad + db * W / displayBins;
        const int bw = std::max(1, W / displayBins);

        // Inside lo-hi range: brighter blue-gray; outside: dim.
        if (x >= loX && x <= hiX)
            p.setBrush(QColor(110, 130, 170));
        else
            p.setBrush(QColor(60, 60, 72));

        p.drawRect(x, kPad + H - barH, bw, barH);
    }

    // lo handle — cyan
    p.setPen(QPen(QColor(0, 185, 220), 2));
    p.drawLine(loX, kPad, loX, kPad + H);

    // hi handle — orange
    p.setPen(QPen(QColor(245, 140, 0), 2));
    p.drawLine(hiX, kPad, hiX, kPad + H);

    // Value labels
    p.setFont(QFont("monospace", 7));
    const QString loTxt = QString("%1").arg(static_cast<double>(m_stretch.lo), 0, 'g', 3);
    const QString hiTxt = QString("%1").arg(static_cast<double>(m_stretch.hi), 0, 'g', 3);

    const int labelY = kPad + H - 2;
    p.setPen(QColor(0, 185, 220));
    // Place lo label to the right of the handle if it fits, else to the left.
    const int loLabelX = (loX + 3 + 40 < kPad + W) ? loX + 3 : loX - 43;
    p.drawText(loLabelX, labelY, loTxt);

    p.setPen(QColor(245, 140, 0));
    const int hiLabelX = (hiX + 3 + 40 < kPad + W) ? hiX + 3 : hiX - 43;
    p.drawText(hiLabelX, labelY, hiTxt);
}

void HistogramWidget::mousePressEvent(QMouseEvent* e) {
    if (!m_valid || e->button() != Qt::LeftButton) return;

    const int loX = magToX(m_stretch.lo);
    const int hiX = magToX(m_stretch.hi);
    const int x   = e->pos().x();

    const int dLo = std::abs(x - loX);
    const int dHi = std::abs(x - hiX);

    if (dLo <= 8 && dLo <= dHi) {
        m_draggingLo = true;
    } else if (dHi <= 8) {
        m_draggingHi = true;
    }
}

void HistogramWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!m_valid) return;

    // Update cursor when hovering near a handle.
    if (!m_draggingLo && !m_draggingHi) {
        const int loX = magToX(m_stretch.lo);
        const int hiX = magToX(m_stretch.hi);
        const int x   = e->pos().x();
        if (std::abs(x - loX) <= 8 || std::abs(x - hiX) <= 8)
            setCursor(Qt::SizeHorCursor);
        else
            unsetCursor();
        return;
    }

    const float range = m_hist.globalMax - m_hist.globalMin;
    const float mag = std::clamp(xToMag(e->pos().x()), m_hist.globalMin, m_hist.globalMax);

    if (m_draggingLo) {
        m_stretch.lo = std::min(mag, m_stretch.hi - 1e-6f * range);
    } else {
        m_stretch.hi = std::max(mag, m_stretch.lo + 1e-6f * range);
    }

    update();
    emit stretchChanged(m_stretch.lo, m_stretch.hi);
}

void HistogramWidget::mouseReleaseEvent(QMouseEvent*) {
    m_draggingLo = false;
    m_draggingHi = false;
    unsetCursor();
}

#include "TileProvider.h"

#include <QThread>

#include <algorithm>
#include <atomic>
#include <complex>
#include <cstdint>
#include <vector>

#include <six/NITFReadControl.h>
#include <six/Region.h>
#include <six/XMLControlFactory.h>
#include <six/sicd/ComplexXMLControl.h>
#include <six/sidd/DerivedXMLControl.h>

// ── Stretch helper ─────────────────────────────────────────────────────────────
// lo/hi/rng are in dB; mag is raw linear magnitude.

static uchar applyStretch(float mag, float lo, float rng, float gamma) {
    const float db = (mag > 0.0f) ? 20.0f * std::log10(mag) : lo - 1.0f;
    float v = std::clamp((db - lo) / rng, 0.0f, 1.0f);
    return static_cast<uchar>(std::pow(v, gamma) * 255.0f);
}

// ── TileLoader ─────────────────────────────────────────────────────────────────
// One thread per loader; all share a single TileQueue owned by TileProvider.
// Each thread opens its own NITFReadControl so reads truly run in parallel.

Q_DECLARE_METATYPE(QImage)

class TileLoader : public QThread {
    Q_OBJECT
public:
    TileLoader(const QString& path, int rows, int cols, int pixelType, int tileSize,
               TileQueue* queue, QObject* parent = nullptr)
        : QThread(parent)
        , m_path(path)
        , m_rows(rows)
        , m_cols(cols)
        , m_pixelType(pixelType)
        , m_tileSize(tileSize)
        , m_queue(queue)
    {
        qRegisterMetaType<QImage>("QImage");
    }

signals:
    void tileLoaded(int tileRow, int tileCol, int gen, const QImage& img);

protected:
    void run() override {
        six::XMLControlRegistry xmlRegistry;
        xmlRegistry.addCreator<six::sicd::ComplexXMLControl>();
        xmlRegistry.addCreator<six::sidd::DerivedXMLControl>();

        six::NITFReadControl reader;
        reader.setXMLControlRegistry(xmlRegistry);
        try {
            reader.load(m_path.toStdString());
        } catch (...) {
            return;
        }

        const six::PixelType pt = static_cast<six::PixelType::values>(m_pixelType);

        while (!m_queue->stop.load(std::memory_order_acquire)) {
            QPair<int,int> key;
            StretchParams stretch;
            {
                QMutexLocker lock(&m_queue->mutex);
                while (m_queue->items.isEmpty() && !m_queue->stop.load(std::memory_order_acquire))
                    m_queue->cond.wait(&m_queue->mutex);
                if (m_queue->stop.load(std::memory_order_acquire)) break;
                key     = m_queue->items.takeFirst();
                m_queue->queued.remove(key);
                stretch = m_queue->stretch;
                m_gen   = m_queue->generation;
            }

            const int tr = key.first;
            const int tc = key.second;

            const int T        = m_tileSize;
            const int rowStart = tr * T;
            const int colStart = tc * T;
            const int tileH    = std::min(T, m_rows - rowStart);
            const int tileW    = std::min(T, m_cols - colStart);
            if (tileH <= 0 || tileW <= 0) continue;

            QImage img = loadTile(reader, pt, rowStart, colStart, tileH, tileW, stretch);
            if (!img.isNull())
                emit tileLoaded(tr, tc, m_gen, img);
        }
    }

private:
    QImage loadTile(six::NITFReadControl& reader, six::PixelType pt,
                    int rowStart, int colStart, int tileH, int tileW,
                    const StretchParams& stretch) {
        six::Region r;
        r.setStartRow(static_cast<ptrdiff_t>(rowStart));
        r.setStartCol(static_cast<ptrdiff_t>(colStart));
        r.setNumRows(static_cast<ptrdiff_t>(tileH));
        r.setNumCols(static_cast<ptrdiff_t>(tileW));

        const float lo    = stretch.lo;
        const float hi    = stretch.hi;
        const float rng   = (hi > lo) ? (hi - lo) : 1.0f;
        const float gamma = stretch.gamma;

        try {
            if (pt == six::PixelType::RE32F_IM32F) {
                const size_t n = static_cast<size_t>(tileH) * tileW;
                std::vector<std::complex<float>> buf(n);
                r.setBuffer(reinterpret_cast<six::UByte*>(buf.data()));
                reader.interleaved(r, 0);

                QImage img(tileW, tileH, QImage::Format_Grayscale8);
                uchar* bits = img.bits();
                const int bpl = img.bytesPerLine();
                for (int row = 0; row < tileH; ++row) {
                    uchar* line = bits + row * bpl;
                    for (int col = 0; col < tileW; ++col)
                        line[col] = applyStretch(std::abs(buf[static_cast<size_t>(row) * tileW + col]),
                                                 lo, rng, gamma);
                }
                return img;

            } else if (pt == six::PixelType::RE16I_IM16I) {
                const size_t n = static_cast<size_t>(tileH) * tileW;
                std::vector<int16_t> raw(n * 2);
                r.setBuffer(reinterpret_cast<six::UByte*>(raw.data()));
                reader.interleaved(r, 0);

                QImage img(tileW, tileH, QImage::Format_Grayscale8);
                uchar* bits = img.bits();
                const int bpl = img.bytesPerLine();
                for (int row = 0; row < tileH; ++row) {
                    uchar* line = bits + row * bpl;
                    for (int col = 0; col < tileW; ++col) {
                        const size_t idx = (static_cast<size_t>(row) * tileW + col) * 2;
                        const float re = static_cast<float>(raw[idx]);
                        const float im = static_cast<float>(raw[idx + 1]);
                        line[col] = applyStretch(std::sqrt(re * re + im * im), lo, rng, gamma);
                    }
                }
                return img;

            } else if (pt == six::PixelType::AMP8I_PHS8I) {
                const size_t n = static_cast<size_t>(tileH) * tileW;
                std::vector<uint8_t> raw(n * 2);
                r.setBuffer(reinterpret_cast<six::UByte*>(raw.data()));
                reader.interleaved(r, 0);

                QImage img(tileW, tileH, QImage::Format_Grayscale8);
                uchar* bits = img.bits();
                const int bpl = img.bytesPerLine();
                for (int row = 0; row < tileH; ++row) {
                    uchar* line = bits + row * bpl;
                    for (int col = 0; col < tileW; ++col)
                        line[col] = raw[(static_cast<size_t>(row) * tileW + col) * 2];
                }
                return img;

            } else if (pt == six::PixelType::MONO8I) {
                const size_t n = static_cast<size_t>(tileH) * tileW;
                std::vector<uint8_t> buf(n);
                r.setBuffer(reinterpret_cast<six::UByte*>(buf.data()));
                reader.interleaved(r, 0);

                QImage img(tileW, tileH, QImage::Format_Grayscale8);
                uchar* bits = img.bits();
                const int bpl = img.bytesPerLine();
                for (int row = 0; row < tileH; ++row)
                    std::memcpy(bits + row * bpl, buf.data() + static_cast<size_t>(row) * tileW, tileW);
                return img;

            } else if (pt == six::PixelType::MONO16I) {
                const size_t n = static_cast<size_t>(tileH) * tileW;
                std::vector<uint16_t> buf(n);
                r.setBuffer(reinterpret_cast<six::UByte*>(buf.data()));
                reader.interleaved(r, 0);

                QImage img(tileW, tileH, QImage::Format_Grayscale8);
                uchar* bits = img.bits();
                const int bpl = img.bytesPerLine();
                for (int row = 0; row < tileH; ++row) {
                    uchar* line = bits + row * bpl;
                    const uint16_t* src = buf.data() + static_cast<size_t>(row) * tileW;
                    for (int col = 0; col < tileW; ++col)
                        line[col] = static_cast<uchar>(src[col] >> 8);
                }
                return img;

            } else if (pt == six::PixelType::RGB24I) {
                const size_t n = static_cast<size_t>(tileH) * tileW * 3;
                std::vector<uint8_t> buf(n);
                r.setBuffer(reinterpret_cast<six::UByte*>(buf.data()));
                reader.interleaved(r, 0);

                QImage img(tileW, tileH, QImage::Format_RGB888);
                uchar* bits = img.bits();
                const int bpl = img.bytesPerLine();
                for (int row = 0; row < tileH; ++row)
                    std::memcpy(bits + row * bpl,
                                buf.data() + static_cast<size_t>(row) * tileW * 3,
                                static_cast<size_t>(tileW) * 3);
                return img;
            }
        } catch (...) {}

        return QImage{};
    }

    QString    m_path;
    int        m_rows, m_cols, m_pixelType, m_tileSize;
    int        m_gen = 0;   // generation captured when job was dequeued
    TileQueue* m_queue;     // shared, owned by TileProvider
};

// ── TileProvider ───────────────────────────────────────────────────────────────

TileProvider::TileProvider(const QString& path, int rows, int cols, int pixelType,
                            int tileSize,
                            StretchParams stretch, QImage overview, int oversampleFactor,
                            std::vector<float> overviewBuf,
                            QObject* parent)
    : QObject(parent)
    , m_rows(rows)
    , m_cols(cols)
    , m_pixelType(pixelType)
    , m_tileSize(tileSize)
    , m_oversampleFactor(oversampleFactor)
    , m_stretch(stretch)
    , m_overview(std::move(overview))
    , m_overviewBuf(std::move(overviewBuf))
    , m_path(path)
{
    m_queue.stretch = stretch;

    const int nThreads = std::max(2, QThread::idealThreadCount());
    m_loaders.reserve(nThreads);
    for (int i = 0; i < nThreads; ++i) {
        auto* loader = new TileLoader(path, rows, cols, pixelType, tileSize, &m_queue);
        connect(loader, &TileLoader::tileLoaded,
                this,   &TileProvider::onTileLoaded,
                Qt::QueuedConnection);
        loader->start();
        m_loaders.append(loader);
    }
}

TileProvider::~TileProvider() {
    stopLoaders();
}

void TileProvider::stopLoaders() {
    m_queue.stop.store(true, std::memory_order_release);
    {
        QMutexLocker lock(&m_queue.mutex);
        m_queue.cond.wakeAll();
    }
    for (auto* loader : m_loaders) {
        if (!loader->wait(5000)) {
            loader->terminate();
            loader->wait();
        }
        delete loader;
    }
    m_loaders.clear();
}

void TileProvider::updateOverview(StretchParams s) {
    if (m_overviewBuf.empty()) return;
    m_stretch = s;
    const int   oRows = m_overview.height();
    const int   oCols = m_overview.width();
    const float lo    = s.lo;
    const float rng   = (s.hi > lo) ? (s.hi - lo) : 1.0f;
    const float gamma = s.gamma;

    QImage img(oCols, oRows, QImage::Format_Grayscale8);
    uchar* bits = img.bits();
    const int bpl = img.bytesPerLine();
    for (int oy = 0; oy < oRows; ++oy) {
        uchar* line = bits + oy * bpl;
        for (int ox = 0; ox < oCols; ++ox) {
            const float db = m_overviewBuf[static_cast<size_t>(oy) * oCols + ox];
            const float v  = std::clamp((db - lo) / rng, 0.0f, 1.0f);
            line[ox] = static_cast<uchar>(std::pow(v, gamma) * 255.0f);
        }
    }
    m_overview = std::move(img);
    emit imageChanged();
}

void TileProvider::setStretch(StretchParams newStretch) {
    updateOverview(newStretch);

    // Bump generation and flush the queue so workers switch to the new stretch.
    // The tile cache is intentionally kept: stale tiles remain visible as
    // placeholders and are replaced in-place as fresh tiles arrive.
    m_pending.clear();

    {
        QMutexLocker lock(&m_queue.mutex);
        m_queue.items.clear();
        m_queue.queued.clear();
        m_queue.stretch = newStretch;
        m_queue.generation = ++m_generation;
    }

    emit imageChanged();
    emit loadingProgress(0);
}

QImage TileProvider::tile(int tileRow, int tileCol) {
    // Never enqueue a tile that would have zero extent (image edge past last pixel).
    const int rowStart = tileRow * m_tileSize;
    const int colStart = tileCol * m_tileSize;
    if (rowStart >= m_rows || colStart >= m_cols) return QImage{};

    const auto key = qMakePair(tileRow, tileCol);

    if (m_cache.contains(key)) {
        m_cacheOrder.removeOne(key);
        m_cacheOrder.prepend(key);
        // If this tile was rendered with an older stretch, queue a fresh load
        // but return the stale image as a placeholder in the meantime.
        if (m_cacheTileGen.value(key) != m_generation && !m_pending.contains(key)) {
            m_pending.insert(key);
            {
                QMutexLocker lock(&m_queue.mutex);
                if (!m_queue.queued.contains(key)) {
                    m_queue.queued.insert(key);
                    m_queue.items.prepend(key);
                    m_queue.cond.wakeOne();
                }
            }
            emit loadingProgress(static_cast<int>(m_pending.size()));
        }
        return m_cache.value(key);
    }

    if (!m_pending.contains(key)) {
        m_pending.insert(key);
        {
            QMutexLocker lock(&m_queue.mutex);
            if (!m_queue.queued.contains(key)) {
                m_queue.queued.insert(key);
                m_queue.items.prepend(key);
                m_queue.cond.wakeOne();
            }
        }
        emit loadingProgress(static_cast<int>(m_pending.size()));
    }

    return QImage{};
}

void TileProvider::setViewport(int rMin, int rMax, int cMin, int cMax) {
    m_visRMin = rMin; m_visRMax = rMax;
    m_visCMin = cMin; m_visCMax = cMax;
}

QImage TileProvider::peekTile(int tileRow, int tileCol) const {
    return m_cache.value(qMakePair(tileRow, tileCol));
}

void TileProvider::flushQueue() {
    {
        QMutexLocker lock(&m_queue.mutex);
        m_queue.items.clear();
        m_queue.queued.clear();
    }
    m_pending.clear();
    emit loadingProgress(0);
}

void TileProvider::onTileLoaded(int tileRow, int tileCol, int gen, const QImage& img) {
    // Discard tiles rendered for a previous stretch setting.
    if (gen != m_generation) return;

    const auto key = qMakePair(tileRow, tileCol);

    m_pending.remove(key);

    if (m_cache.size() >= kMaxCachedTiles) {
        // Prefer to evict a tile outside the current viewport (off-screen).
        // Fall back to plain LRU only if every cached tile is still visible.
        bool evicted = false;
        for (int i = m_cacheOrder.size() - 1; i >= 0; --i) {
            const auto& k = m_cacheOrder[i];
            if (k.first  < m_visRMin || k.first  > m_visRMax ||
                k.second < m_visCMin || k.second > m_visCMax) {
                m_cache.remove(k);
                m_cacheTileGen.remove(k);
                m_cacheOrder.removeAt(i);
                evicted = true;
                break;
            }
        }
        if (!evicted && !m_cacheOrder.isEmpty()) {
            const auto lru = m_cacheOrder.takeLast();
            m_cache.remove(lru);
            m_cacheTileGen.remove(lru);
        }
    }

    m_cache.insert(key, img);
    m_cacheTileGen.insert(key, gen);
    m_cacheOrder.prepend(key);

    if (m_callback)
        m_callback(tileRow, tileCol);

    emit loadingProgress(static_cast<int>(m_pending.size()));
}

#include "TileProvider.moc"

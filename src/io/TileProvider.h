#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QWaitCondition>
#include <functional>
#include <vector>

struct StretchParams {
    float lo    = 0.0f;
    float hi    = 1.0f;
    float gamma = 1.0f;
};

struct HistogramData {
    std::vector<int64_t> bins;   ///< kHistBins entries spanning [globalMin, globalMax]
    float globalMin = 0.0f;      ///< lower x-axis bound (dB for complex types)
    float globalMax = 0.0f;      ///< upper x-axis bound
    bool  valid     = false;     ///< false for non-complex pixel types
};

// Shared queue consumed by all TileLoader threads.
struct TileQueue {
    QMutex                    mutex;
    QWaitCondition            cond;
    QList<QPair<int,int>>     items;    // LIFO: prepend on push, takeFirst on pop
    QSet<QPair<int,int>>      queued;   // O(1) membership mirror of items
    StretchParams             stretch;
    int                       generation = 0;  // bumped on every setStretch
    std::atomic<bool>         stop{false};
};

class TileLoader;

// Manages on-demand tile loading from a NITF file.
// Must be created and used on the main (GUI) thread.
class TileProvider : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxCachedTiles = 1024;

    TileProvider(const QString& path, int rows, int cols, int pixelType,
                 int tileSize,
                 StretchParams stretch, QImage overview, int oversampleFactor,
                 std::vector<float> overviewBuf,
                 QObject* parent = nullptr);
    ~TileProvider() override;

    int    imageRows()        const { return m_rows; }
    int    imageCols()        const { return m_cols; }
    int    tileSize()         const { return m_tileSize; }
    int    tileRows()         const { return (m_rows + m_tileSize - 1) / m_tileSize; }
    int    tileCols()         const { return (m_cols + m_tileSize - 1) / m_tileSize; }
    int    oversampleFactor() const { return m_oversampleFactor; }
    QImage overview()         const { return m_overview; }
    StretchParams stretch()   const { return m_stretch; }

    /// Re-render the overview image only (fast, no cache invalidation).
    void updateOverview(StretchParams s);

    /// Update stretch, re-render overview, and invalidate tile cache.
    void setStretch(StretchParams s);

    /// Returns cached tile or null. Queues a background load if not cached/pending.
    /// Main thread only.
    QImage tile(int tileRow, int tileCol);

    /// Returns cached tile or null WITHOUT queuing a load request.
    QImage peekTile(int tileRow, int tileCol) const;

    /// Cancels all pending tile requests (clears queue and pending set).
    void flushQueue();

    /// Tell the provider which tile range is currently visible, so eviction
    /// avoids discarding tiles that are still on-screen.
    void setViewport(int rMin, int rMax, int cMin, int cMax);

    void setTileReadyCallback(std::function<void(int, int)> cb) { m_callback = std::move(cb); }

signals:
    void imageChanged();
    void loadingProgress(int pendingTiles);

private slots:
    void onTileLoaded(int tileRow, int tileCol, int gen, const QImage& img);

private:
    void stopLoaders();

    int m_rows, m_cols, m_pixelType, m_tileSize, m_oversampleFactor;
    StretchParams m_stretch;
    QImage m_overview;
    std::vector<float> m_overviewBuf;
    QString m_path;

    QHash<QPair<int,int>, QImage> m_cache;
    QHash<QPair<int,int>, int>    m_cacheTileGen;  // generation of each cached tile
    QList<QPair<int,int>>         m_cacheOrder;
    QSet<QPair<int,int>>          m_pending;

    std::function<void(int,int)> m_callback;

    // Current visible tile range (updated each paint frame).
    int m_visRMin = 0, m_visRMax = -1, m_visCMin = 0, m_visCMax = -1;

    int m_generation  = 0;  // incremented on setStretch AND flushQueue; discards in-flight tiles
    int m_stretchGen  = 0;  // incremented only on setStretch; governs cache content validity

    TileQueue          m_queue;
    QList<TileLoader*> m_loaders;
};

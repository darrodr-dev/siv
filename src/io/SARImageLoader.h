#pragma once

#include <QImage>
#include <QList>
#include <QString>
#include <stdexcept>
#include <vector>

#include "TileProvider.h"  // StretchParams, HistogramData

struct SARLoadResult {
    // Raw data — caller constructs TileProvider on the main thread from these.
    int              numRows          = 0;
    int              numCols          = 0;
    int              pixelType        = 0;
    int              oversampleFactor = 1;
    int              tileSize         = 1024;  ///< Matches NITF file block size
    StretchParams    stretch;
    QImage           overview;
    std::vector<float> overviewBuf;  ///< Raw magnitude samples (complex types only)
    HistogramData    histogram;

    // Metadata
    QString          dataType;       ///< "SICD" or "SIDD"
    QList<QString>   xmlSegments;
};

class SARLoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SARImageLoader {
public:
    /// Load a SICD or SIDD NITF file. Throws SARLoadError on failure.
    static SARLoadResult load(const QString& filePath);
};

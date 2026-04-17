#include "SARImageLoader.h"

#include <QFile>
#include <QThread>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QtConcurrent>

#include <algorithm>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

#include <six/Container.h>
#include <six/NITFReadControl.h>
#include <six/Region.h>
#include <six/XMLControlFactory.h>
#include <six/sicd/ComplexData.h>
#include <six/sicd/ComplexXMLControl.h>
#include <six/sidd/DerivedData.h>
#include <six/sidd/DerivedXMLControl.h>
#include <io/StringStream.h>
#include <nitf/ImageSegment.hpp>
#include <nitf/ImageSubheader.hpp>
#include <nitf/Record.hpp>

// ── Threading helper ──────────────────────────────────────────────────────────

template<typename Fn>
static void parallelFor(size_t count, Fn fn) {
    const int nT = std::max(1, QThread::idealThreadCount());
    const size_t chunk = (count + nT - 1) / nT;
    QList<QFuture<void>> futures;
    futures.reserve(nT);
    for (int t = 0; t < nT; ++t) {
        const size_t lo = static_cast<size_t>(t) * chunk;
        const size_t hi = std::min(lo + chunk, count);
        if (lo >= hi) break;
        futures.append(QtConcurrent::run([fn, lo, hi]() { fn(lo, hi); }));
    }
    for (auto& f : futures) f.waitForFinished();
}

// ── XML helpers ───────────────────────────────────────────────────────────────

static QString prettyXML(const std::string& raw) {
    QByteArray input = QByteArray::fromStdString(raw);
    QByteArray output;
    QXmlStreamReader reader(input);
    QXmlStreamWriter writer(&output);
    writer.setAutoFormatting(true);
    writer.setAutoFormattingIndent(2);
    while (!reader.atEnd()) {
        if (reader.hasError()) break;
        writer.writeCurrentToken(reader);
        reader.readNext();
    }
    return output.isEmpty() ? QString::fromStdString(raw) : QString::fromUtf8(output);
}

static QString dataToXMLString(six::Data* data, const six::XMLControlRegistry& registry) {
    std::unique_ptr<six::XMLControl> ctrl(registry.newXMLControl(data->getDataType(), nullptr));
    const std::vector<std::string> noSchemaPaths;
    std::unique_ptr<xml::lite::Document> doc(ctrl->toXML(data, noSchemaPaths));
    if (!doc) return {};
    io::StringStream ss;
    doc->getRootElement()->print(ss);
    return prettyXML(ss.stream().str());
}

// ── Overview loading ──────────────────────────────────────────────────────────

static constexpr int kHistBins = 65536;

struct OverviewResult {
    QImage             image;
    StretchParams      stretch;
    std::vector<float> rawBuf;    ///< raw magnitudes at overview resolution (complex only)
    HistogramData      histogram;
};

static OverviewResult loadOverview(six::NITFReadControl& reader, six::PixelType pt,
                                   int rows, int cols, int factor) {
    const int oRows = (rows + factor - 1) / factor;
    const int oCols = (cols + factor - 1) / factor;

    if (pt == six::PixelType::RE32F_IM32F || pt == six::PixelType::RE16I_IM16I) {
        // Collect raw magnitudes from subsampled rows.
        std::vector<float> magBuf;
        magBuf.reserve(static_cast<size_t>(oRows) * oCols);

        for (int r = 0; r < rows; r += factor) {
            six::Region reg;
            reg.setStartRow(static_cast<ptrdiff_t>(r));
            reg.setStartCol(0);
            reg.setNumRows(1);
            reg.setNumCols(static_cast<ptrdiff_t>(cols));

            if (pt == six::PixelType::RE32F_IM32F) {
                std::vector<std::complex<float>> rowBuf(static_cast<size_t>(cols));
                reg.setBuffer(reinterpret_cast<six::UByte*>(rowBuf.data()));
                reader.interleaved(reg, 0);
                for (int c = 0; c < cols; c += factor)
                    magBuf.push_back(std::abs(rowBuf[static_cast<size_t>(c)]));
            } else {
                std::vector<int16_t> rowBuf(static_cast<size_t>(cols) * 2);
                reg.setBuffer(reinterpret_cast<six::UByte*>(rowBuf.data()));
                reader.interleaved(reg, 0);
                for (int c = 0; c < cols; c += factor) {
                    const size_t idx = static_cast<size_t>(c) * 2;
                    const float re = static_cast<float>(rowBuf[idx]);
                    const float im = static_cast<float>(rowBuf[idx + 1]);
                    magBuf.push_back(std::sqrt(re * re + im * im));
                }
            }
        }

        // Convert magnitudes to dB.  Zero/negative magnitudes are clamped to a floor.
        constexpr float kDbFloor = -120.0f;
        std::vector<float> dbBuf(magBuf.size());
        float dbMin =  std::numeric_limits<float>::max();
        float dbMax = -std::numeric_limits<float>::max();
        for (size_t i = 0; i < magBuf.size(); ++i) {
            const float db = (magBuf[i] > 0.0f) ? 20.0f * std::log10(magBuf[i]) : kDbFloor;
            dbBuf[i] = db;
            if (db > kDbFloor) {
                if (db < dbMin) dbMin = db;
                if (db > dbMax) dbMax = db;
            }
        }

        if (dbMax <= dbMin) {
            OverviewResult r;
            r.image = QImage(oCols, oRows, QImage::Format_Grayscale8);
            r.histogram.valid = true;
            return r;
        }

        // Build histogram in dB space.
        const float dbRange  = dbMax - dbMin;
        const float binScale = (kHistBins - 1) / dbRange;
        std::vector<int64_t> hist(kHistBins, 0);
        int64_t totalValid = 0;
        for (float db : dbBuf) {
            if (db > kDbFloor) {
                hist[std::min(kHistBins - 1, static_cast<int>((db - dbMin) * binScale))]++;
                ++totalValid;
            }
        }

        // lo = p2 in dB, hi = p99 in dB (some bright pixels clip — that's fine).
        const int64_t target2  = totalValid * 2  / 100;
        const int64_t target99 = totalValid * 99 / 100;
        float lo = dbMin, hi = dbMax;
        int64_t cum = 0;
        bool foundLo = false, foundHi = false;
        for (int b = 0; b < kHistBins && !(foundLo && foundHi); ++b) {
            cum += hist[b];
            if (!foundLo && cum >= target2)  { lo = dbMin + b / binScale; foundLo = true; }
            if (!foundHi && cum >= target99) { hi = dbMin + b / binScale; foundHi = true; }
        }

        const float rng = (hi > lo) ? (hi - lo) : 1.0f;
        constexpr float kGamma = 1.0f;  // dB is already log — no extra gamma needed

        // Render overview.  overviewBuf stores dB values for fast re-render on stretch change.
        QImage img(oCols, oRows, QImage::Format_Grayscale8);
        uchar* bits = img.bits();
        const int bpl = img.bytesPerLine();
        for (int oy = 0; oy < oRows; ++oy) {
            uchar* line = bits + oy * bpl;
            for (int ox = 0; ox < oCols; ++ox) {
                const float db = dbBuf[static_cast<size_t>(oy) * oCols + ox];
                const float v  = std::clamp((db - lo) / rng, 0.0f, 1.0f);
                line[ox] = static_cast<uchar>(v * 255.0f);
            }
        }

        HistogramData histogram;
        histogram.bins      = std::move(hist);
        histogram.globalMin = dbMin;
        histogram.globalMax = dbMax;
        histogram.valid     = true;

        return { img, StretchParams{ lo, hi, kGamma }, std::move(dbBuf), std::move(histogram) };
    }

    // ── Simple pixel types ────────────────────────────────────────────────────

    if (pt == six::PixelType::MONO8I) {
        QImage img(oCols, oRows, QImage::Format_Grayscale8);
        uchar* bits = img.bits();
        const int bpl = img.bytesPerLine();

        for (int r = 0; r < rows; r += factor) {
            six::Region reg;
            reg.setStartRow(static_cast<ptrdiff_t>(r));
            reg.setStartCol(0);
            reg.setNumRows(1);
            reg.setNumCols(static_cast<ptrdiff_t>(cols));
            std::vector<uint8_t> rowBuf(static_cast<size_t>(cols));
            reg.setBuffer(reinterpret_cast<six::UByte*>(rowBuf.data()));
            reader.interleaved(reg, 0);

            const int oy = r / factor;
            uchar* line = bits + oy * bpl;
            for (int c = 0, ox = 0; c < cols; c += factor, ++ox)
                line[ox] = rowBuf[static_cast<size_t>(c)];
        }
        return { img, StretchParams{}, {}, {} };

    } else if (pt == six::PixelType::MONO16I) {
        QImage img(oCols, oRows, QImage::Format_Grayscale8);
        uchar* bits = img.bits();
        const int bpl = img.bytesPerLine();

        for (int r = 0; r < rows; r += factor) {
            six::Region reg;
            reg.setStartRow(static_cast<ptrdiff_t>(r));
            reg.setStartCol(0);
            reg.setNumRows(1);
            reg.setNumCols(static_cast<ptrdiff_t>(cols));
            std::vector<uint16_t> rowBuf(static_cast<size_t>(cols));
            reg.setBuffer(reinterpret_cast<six::UByte*>(rowBuf.data()));
            reader.interleaved(reg, 0);

            const int oy = r / factor;
            uchar* line = bits + oy * bpl;
            for (int c = 0, ox = 0; c < cols; c += factor, ++ox)
                line[ox] = static_cast<uchar>(rowBuf[static_cast<size_t>(c)] >> 8);
        }
        return { img, StretchParams{}, {}, {} };

    } else if (pt == six::PixelType::AMP8I_PHS8I) {
        QImage img(oCols, oRows, QImage::Format_Grayscale8);
        uchar* bits = img.bits();
        const int bpl = img.bytesPerLine();

        for (int r = 0; r < rows; r += factor) {
            six::Region reg;
            reg.setStartRow(static_cast<ptrdiff_t>(r));
            reg.setStartCol(0);
            reg.setNumRows(1);
            reg.setNumCols(static_cast<ptrdiff_t>(cols));
            std::vector<uint8_t> rowBuf(static_cast<size_t>(cols) * 2);
            reg.setBuffer(reinterpret_cast<six::UByte*>(rowBuf.data()));
            reader.interleaved(reg, 0);

            const int oy = r / factor;
            uchar* line = bits + oy * bpl;
            for (int c = 0, ox = 0; c < cols; c += factor, ++ox)
                line[ox] = rowBuf[static_cast<size_t>(c) * 2];
        }
        return { img, StretchParams{}, {}, {} };

    } else if (pt == six::PixelType::RGB24I) {
        QImage img(oCols, oRows, QImage::Format_RGB888);
        uchar* bits = img.bits();
        const int bpl = img.bytesPerLine();

        for (int r = 0; r < rows; r += factor) {
            six::Region reg;
            reg.setStartRow(static_cast<ptrdiff_t>(r));
            reg.setStartCol(0);
            reg.setNumRows(1);
            reg.setNumCols(static_cast<ptrdiff_t>(cols));
            std::vector<uint8_t> rowBuf(static_cast<size_t>(cols) * 3);
            reg.setBuffer(reinterpret_cast<six::UByte*>(rowBuf.data()));
            reader.interleaved(reg, 0);

            const int oy = r / factor;
            uchar* line = bits + oy * bpl;
            for (int c = 0, ox = 0; c < cols; c += factor, ++ox)
                std::memcpy(line + ox * 3, rowBuf.data() + static_cast<size_t>(c) * 3, 3);
        }
        return { img, StretchParams{}, {}, {} };

    } else {
        throw SARLoadError("Unsupported pixel type: " + std::to_string(static_cast<int>(pt)));
    }
}

// ── NITF block size ───────────────────────────────────────────────────────────

// Returns the file's native block dimension, clamped to a reasonable range.
// 0 in the subheader means "not blocked" (treat as full-image = use default).
static int nitfBlockSize(six::NITFReadControl& reader) {
    constexpr int kDefault = 1024;
    try {
        nitf::Record record = reader.getRecord();
        nitf::List   images = record.getImages();
        if (images.isEmpty()) return kDefault;

        nitf::ImageSegment  seg(*images.begin());
        nitf::ImageSubheader sub = seg.getSubheader();

        const uint32_t bw = static_cast<uint32_t>(sub.getNumPixelsPerHorizBlock());
        const uint32_t bh = static_cast<uint32_t>(sub.getNumPixelsPerVertBlock());

        // 0 means the image is not blocked in that dimension.
        if (bw == 0 && bh == 0) return kDefault;
        const uint32_t bs = (bw > 0 && bh > 0) ? std::min(bw, bh)
                          : (bw > 0)            ? bw : bh;

        // Clamp to [128, 4096] and round to the nearest power of two.
        if (bs < 128 || bs > 4096) return kDefault;
        uint32_t p = 128;
        while (p * 2 <= bs) p *= 2;
        return static_cast<int>(p);
    } catch (...) {
        return kDefault;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

SARLoadResult SARImageLoader::load(const QString& filePath) {
    if (!QFile::exists(filePath))
        throw SARLoadError("File not found: " + filePath.toStdString());

    six::XMLControlRegistry xmlRegistry;
    xmlRegistry.addCreator<six::sicd::ComplexXMLControl>();
    xmlRegistry.addCreator<six::sidd::DerivedXMLControl>();

    six::NITFReadControl reader;
    reader.setXMLControlRegistry(xmlRegistry);
    reader.load(filePath.toStdString());

    auto container = reader.getContainer();
    if (!container || container->size() == 0)
        throw SARLoadError("No image segments found in file");

    six::Data* data = container->getData(0);
    if (!data)
        throw SARLoadError("Failed to retrieve image data from container");

    const int rows = static_cast<int>(data->getNumRows());
    const int cols = static_cast<int>(data->getNumCols());
    const six::PixelType pt = data->getPixelType();

    // Launch XML serialization for all segments concurrently.
    const size_t nSeg = container->size();
    QList<QFuture<QString>> xmlFutures;
    xmlFutures.reserve(static_cast<int>(nSeg));
    for (size_t i = 0; i < nSeg; ++i) {
        six::Data* seg = container->getData(i);
        if (seg) xmlFutures.append(QtConcurrent::run(dataToXMLString, seg, std::cref(xmlRegistry)));
    }

    const int tileSize = nitfBlockSize(reader);
    const int factor = std::max(1, std::max(rows, cols) / 1024);

    auto [overview, stretch, overviewBuf, histogram] = loadOverview(reader, pt, rows, cols, factor);

    SARLoadResult result;
    result.numRows          = rows;
    result.numCols          = cols;
    result.pixelType        = static_cast<int>(pt);
    result.oversampleFactor = factor;
    result.tileSize         = tileSize;
    result.stretch          = stretch;
    result.overview         = std::move(overview);
    result.overviewBuf      = std::move(overviewBuf);
    result.histogram        = std::move(histogram);
    result.dataType = (container->getDataType() == six::DataType::COMPLEX) ? "SICD" : "SIDD";

    for (auto& f : xmlFutures) result.xmlSegments.append(f.result());
    return result;
}

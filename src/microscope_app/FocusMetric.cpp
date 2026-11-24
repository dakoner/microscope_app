#include "FocusMetric.h"

#include <QImage>
#include <algorithm>
#include <cmath>

namespace {

// Sampling every third pixel keeps a full 1280x1024 frame well under a millisecond
// while still sampling far above the optical resolution of the objective.
constexpr int kStride = 3;

// Vignetting and the mechanical frame edge stay constant while the stage moves and
// would otherwise dominate the objective, so only the centre is scored.
constexpr double kRoiFraction = 0.6;

inline double luma(const uchar *row, int offset)
{
    return 0.299 * row[offset] + 0.587 * row[offset + 1] + 0.114 * row[offset + 2];
}

}  // namespace

namespace focus {

double sharpnessScore(const QImage &image)
{
    if (image.isNull() || image.width() < 16 || image.height() < 16) {
        return 0.0;
    }

    const bool isGray = image.format() == QImage::Format_Grayscale8;
    QImage frame = image;
    if (!isGray && image.format() != QImage::Format_RGB888) {
        frame = image.convertToFormat(QImage::Format_RGB888);
        if (frame.isNull()) {
            return 0.0;
        }
    }

    const int width = frame.width();
    const int height = frame.height();
    const int roiWidth = std::max(16, static_cast<int>(width * kRoiFraction));
    const int roiHeight = std::max(16, static_cast<int>(height * kRoiFraction));
    const int xBegin = (width - roiWidth) / 2;
    const int yBegin = (height - roiHeight) / 2;
    const int xEnd = xBegin + roiWidth - 1;
    const int yEnd = yBegin + roiHeight - 1;

    const bool rgb = frame.format() == QImage::Format_RGB888;

    double sum = 0.0;
    double count = 0.0;
    for (int y = yBegin + 1; y < yEnd; y += kStride) {
        const uchar *row = frame.constScanLine(y);
        const uchar *up = frame.constScanLine(y - 1);
        const uchar *down = frame.constScanLine(y + 1);
        for (int x = xBegin + 1; x < xEnd; x += kStride) {
            double centre = 0.0;
            double above = 0.0;
            double below = 0.0;
            double left = 0.0;
            double right = 0.0;
            if (rgb) {
                const int offset = x * 3;
                centre = luma(row, offset);
                above = luma(up, offset);
                below = luma(down, offset);
                left = luma(row, offset - 3);
                right = luma(row, offset + 3);
            } else {
                centre = row[x];
                above = up[x];
                below = down[x];
                left = row[x - 1];
                right = row[x + 1];
            }

            const double laplacian = 4.0 * centre - above - below - left - right;
            sum += laplacian * laplacian;
            ++count;
        }
    }

    if (count <= 0.0) {
        return 0.0;
    }
    return sum / count;
}

}  // namespace focus

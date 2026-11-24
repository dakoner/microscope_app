#ifndef FOCUSMETRIC_H
#define FOCUSMETRIC_H

class QImage;

namespace focus {

// Sharpness of a camera frame, used as the objective for closed-loop autofocus.
//
// This is the mean squared discrete Laplacian over a centred region of interest,
// sampled on a stride grid.  A focused frame has strong edges, so the value grows
// with focus and falls off in both defocus directions.  Only values measured with
// the same frame size, exposure and gain are comparable, which is why the caller
// freezes auto exposure for the duration of a search.
double sharpnessScore(const QImage &image);

}  // namespace focus

#endif  // FOCUSMETRIC_H

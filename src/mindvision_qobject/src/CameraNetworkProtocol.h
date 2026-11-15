#ifndef CAMERANETWORKPROTOCOL_H
#define CAMERANETWORKPROTOCOL_H

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QtGlobal>
#include <cstring>

namespace CameraNet {

constexpr quint16 kDefaultServerPort = 8766;

// Packet types framed with a 5-byte header: [uint8 type][uint32 payloadLenBigEndian]
constexpr quint8 kPacketTypeJson  = 0x01;
constexpr quint8 kPacketTypeFrame = 0x02;
constexpr int kPacketHeaderSize   = 5;

// Frame payload header (31 bytes) preceding the encoded image bytes:
//   [0..7]   qint64  timestampMs (big-endian)
//   [8..11]  quint32 width       (big-endian)
//   [12..15] quint32 height      (big-endian)
//   [16]     quint8  encoding    (1 = JPEG, 2 = Raw RGB888)
//   [17..24] quint64 exposureMsBits (IEEE-754 double bits, big-endian)
//   [25..28] qint32  analogGain  (big-endian)
//   [29]     quint8  flags       (bit 0 = autoExposure, bit 1 = remoteRecording)
//   [30]     quint8  aeTarget
constexpr int kFrameHeaderSize = 31;

constexpr quint8 kFrameEncodingJpeg   = 1;
constexpr quint8 kFrameEncodingRawRgb = 2;

constexpr quint8 kFrameFlagAutoExposure    = 0x01;
constexpr quint8 kFrameFlagRemoteRecording = 0x02;

struct FramePacketHeader {
    qint64 timestampMs = 0;
    quint32 width = 0;
    quint32 height = 0;
    quint8 encoding = kFrameEncodingJpeg;
    double exposureTimeMs = 0.0;
    qint32 analogGain = 1;
    bool autoExposure = false;
    bool remoteRecording = false;
    quint8 aeTarget = 120;
};

inline QByteArray packPacket(quint8 packetType, const QByteArray &payload)
{
    QByteArray packet;
    packet.resize(kPacketHeaderSize + payload.size());
    uchar *dst = reinterpret_cast<uchar *>(packet.data());
    dst[0] = packetType;
    qToBigEndian<quint32>(static_cast<quint32>(payload.size()), dst + 1);
    if (!payload.isEmpty()) {
        std::memcpy(dst + kPacketHeaderSize, payload.constData(), static_cast<std::size_t>(payload.size()));
    }
    return packet;
}

inline QByteArray packJsonPacket(const QJsonObject &obj)
{
    const QByteArray jsonBytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    return packPacket(kPacketTypeJson, jsonBytes);
}

inline QByteArray packFramePacket(const FramePacketHeader &hdr, const QByteArray &imageBytes)
{
    const quint32 totalPayloadSize = static_cast<quint32>(kFrameHeaderSize + imageBytes.size());
    QByteArray packet;
    packet.resize(kPacketHeaderSize + static_cast<int>(totalPayloadSize));
    uchar *dst = reinterpret_cast<uchar *>(packet.data());

    // 5-byte outer packet header
    dst[0] = kPacketTypeFrame;
    qToBigEndian<quint32>(totalPayloadSize, dst + 1);

    // 31-byte frame metadata header
    uchar *fh = dst + kPacketHeaderSize;
    qToBigEndian<qint64>(hdr.timestampMs, fh + 0);
    qToBigEndian<quint32>(hdr.width, fh + 8);
    qToBigEndian<quint32>(hdr.height, fh + 12);
    fh[16] = hdr.encoding;

    quint64 expBits = 0;
    std::memcpy(&expBits, &hdr.exposureTimeMs, sizeof(double));
    qToBigEndian<quint64>(expBits, fh + 17);

    qToBigEndian<qint32>(hdr.analogGain, fh + 25);

    quint8 flags = 0;
    if (hdr.autoExposure) flags |= kFrameFlagAutoExposure;
    if (hdr.remoteRecording) flags |= kFrameFlagRemoteRecording;
    fh[29] = flags;
    fh[30] = hdr.aeTarget;

    if (!imageBytes.isEmpty()) {
        std::memcpy(fh + kFrameHeaderSize, imageBytes.constData(), static_cast<std::size_t>(imageBytes.size()));
    }
    return packet;
}

inline bool unpackFrameHeader(const uchar *payload, int payloadSize, FramePacketHeader &hdrOut)
{
    if (payloadSize < kFrameHeaderSize) {
        return false;
    }
    hdrOut.timestampMs = qFromBigEndian<qint64>(payload + 0);
    hdrOut.width = qFromBigEndian<quint32>(payload + 8);
    hdrOut.height = qFromBigEndian<quint32>(payload + 12);
    hdrOut.encoding = payload[16];

    const quint64 expBits = qFromBigEndian<quint64>(payload + 17);
    std::memcpy(&hdrOut.exposureTimeMs, &expBits, sizeof(double));

    hdrOut.analogGain = qFromBigEndian<qint32>(payload + 25);
    const quint8 flags = payload[29];
    hdrOut.autoExposure = (flags & kFrameFlagAutoExposure) != 0;
    hdrOut.remoteRecording = (flags & kFrameFlagRemoteRecording) != 0;
    hdrOut.aeTarget = payload[30];
    return true;
}

} // namespace CameraNet

#endif // CAMERANETWORKPROTOCOL_H

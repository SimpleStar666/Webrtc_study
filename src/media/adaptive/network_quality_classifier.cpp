#include "media/adaptive/network_quality_classifier.h"

namespace crystal {

NetworkQuality classifyNetwork(const NetworkSignals& s) {
    // 带宽压力比：GCC 能给的 / 编码器想要的（分母为 0 视为完全受限）
    double ratio = s.configuredKbps > 0
                       ? static_cast<double>(s.gccTargetKbps) / s.configuredKbps
                       : 0.0;

    // rtt 未知（<0）：两个 rtt 条件都视为不命中（不阻塞也不触发）
    bool rttOkGood = (s.rttMs < 0) || (s.rttMs < 150.0);
    bool rttOkFair = (s.rttMs < 0) || (s.rttMs < 300.0);

    if (ratio >= 0.85 && s.lossPct < 2.0 && rttOkGood)
        return NetworkQuality::Good;
    if (ratio >= 0.60 && s.lossPct < 5.0 && rttOkFair)
        return NetworkQuality::Fair;
    if (ratio < 0.35 || s.lossPct >= 15.0 || s.rttMs > 800.0)
        return NetworkQuality::Bad;
    return NetworkQuality::Poor;
}

const char* networkQualityName(NetworkQuality q) {
    switch (q) {
        case NetworkQuality::Good: return "Good";
        case NetworkQuality::Fair:  return "Fair";
        case NetworkQuality::Poor:  return "Poor";
        case NetworkQuality::Bad:   return "Bad";
    }
    return "Unknown";
}

} // namespace crystal

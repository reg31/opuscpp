// Compile with the same configuration and C flags as the official CELT encoder.
#include "external/official_opus/celt/celt_encoder.c"
int official_transient_analysis(const float* in, int len, int channels, float* estimate, int* channel,
                                int allow_weak, int* weak, float tone_freq, float toneishness) {
  return transient_analysis(in, len, channels, estimate, channel, allow_weak, weak, tone_freq, toneishness);
}
